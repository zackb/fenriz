// Package calendar parses the local vdirsyncer calendar store (iCalendar .ics
// files under ~/.local/share/calendars) and streams the next handful of
// upcoming events to the bar. Recurring events are expanded (RRULE) and
// per-instance overrides (RECURRENCE-ID) are honoured so a moved occurrence
// shows at its new time. It re-scans on a slow ticker -- the data changes
// rarely (vdirsyncer runs on its own schedule) and "upcoming" decays as events
// pass anyway.
package calendar

import (
	"bytes"
	"context"
	"encoding/json"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"time"

	ics "github.com/arran4/golang-ical"
	"github.com/teambition/rrule-go"

	"fenriz-plugins/internal/log"
)

const (
	refresh = 60 * time.Second // re-scan + recompute cadence
	// How far ahead recurring rules expand. Each daily rule costs an allocation
	// per day in the window on every rescan while only maxUpcoming survive, so
	// this trades lookahead against churn (see BenchmarkExpand). Raise it if a
	// sparse calendar starts showing fewer than maxUpcoming events.
	horizon     = 90 * 24 * time.Hour
	maxUpcoming = 10 // events surfaced to the widget
)

// Event is one concrete occurrence surfaced to the bar.
type Event struct {
	Summary  string `json:"summary"`
	Start    string `json:"start"` // RFC3339
	End      string `json:"end"`   // RFC3339, "" when the event has no end
	AllDay   bool   `json:"allDay"`
	Location string `json:"location"`
	Calendar string `json:"calendar"` // collection display name

	// Parsed Start, kept for sorting. Unexported so it stays out of the JSON:
	// the RFC3339 strings carry a UTC offset, so comparing them lexically
	// mis-orders events across timezones.
	startAt time.Time
}

// State is the full payload: the next N occurrences, soonest first.
type State struct {
	Upcoming []Event `json:"upcoming"`
}

type Service struct {
	emit func(State)
	last []byte // last emitted JSON, for change suppression
}

func New() *Service { return &Service{} }

// Run scans and emits changes until ctx ends.
func (s *Service) Run(ctx context.Context, emit func(State)) {
	s.emit = emit
	s.run(ctx)
}

func (s *Service) run(ctx context.Context) {
	s.tick()
	t := time.NewTicker(refresh)
	defer t.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-t.C:
			s.tick()
		}
	}
}

func (s *Service) tick() {
	st := State{Upcoming: s.scan(time.Now())}
	b, err := json.Marshal(st)
	if err != nil {
		log.Warnf("calendar: marshal: %v", err)
		return
	}
	if bytes.Equal(b, s.last) {
		return
	}
	s.last = b
	s.emit(st)
}

func calendarRoot() string {
	home, err := os.UserHomeDir()
	if err != nil {
		return ""
	}
	return filepath.Join(home, ".local", "share", "calendars")
}

// scan walks every collection and returns the next maxUpcoming occurrences at
// or after now, soonest first.
func (s *Service) scan(now time.Time) []Event {
	root := calendarRoot()
	if root == "" {
		return nil
	}
	until := now.Add(horizon)

	var out []Event
	// Collections are leaf directories two levels down: <root>/<account>/<collection>.
	dirs, _ := filepath.Glob(filepath.Join(root, "*", "*"))
	for _, dir := range dirs {
		info, err := os.Stat(dir)
		if err != nil || !info.IsDir() {
			continue
		}
		calName := collectionName(dir)
		files, _ := filepath.Glob(filepath.Join(dir, "*.ics"))
		for _, f := range files {
			out = append(out, eventsFromFile(f, calName, now, until)...)
		}
	}

	return sortTrim(out)
}

// sortTrim orders occurrences by instant and keeps the soonest maxUpcoming.
// Sorting on startAt rather than the formatted Start matters: RFC3339 strings
// carry a UTC offset, so a lexical compare mis-orders events across timezones.
func sortTrim(out []Event) []Event {
	sort.Slice(out, func(i, j int) bool { return out[i].startAt.Before(out[j].startAt) })
	if len(out) > maxUpcoming {
		out = out[:maxUpcoming]
	}
	return out
}

// collectionName prefers vdirsyncer's displayname metadata file, falling back to
// the directory name (often an opaque UUID).
func collectionName(dir string) string {
	if b, err := os.ReadFile(filepath.Join(dir, "displayname")); err == nil {
		if name := strings.TrimSpace(string(b)); name != "" {
			return name
		}
	}
	return filepath.Base(dir)
}

// eventsFromFile expands one .ics into concrete occurrences within [now, until].
// A file may hold a master VEVENT plus RECURRENCE-ID overrides sharing its UID;
// overrides are emitted at their own time and suppress the master's slot.
func eventsFromFile(path, calName string, now, until time.Time) []Event {
	data, err := os.Open(path)
	if err != nil {
		return nil
	}
	defer data.Close()

	cal, err := ics.ParseCalendar(data)
	if err != nil {
		return nil
	}

	var masters []*ics.VEvent
	var overrides []*ics.VEvent
	for _, e := range cal.Events() {
		if e.GetProperty(ics.ComponentPropertyRecurrenceId) != nil {
			overrides = append(overrides, e)
		} else {
			masters = append(masters, e)
		}
	}

	// Times of the original occurrences that overrides replace, so we don't also
	// emit them from the master's RRULE expansion.
	suppressed := make([]time.Time, 0, len(overrides))
	for _, e := range overrides {
		if t, ok := parseTimeProp(e.GetProperty(ics.ComponentPropertyRecurrenceId)); ok {
			suppressed = append(suppressed, t)
		}
	}

	var out []Event

	for _, e := range overrides {
		if isCancelled(e) {
			continue
		}
		if ev, ok := occurrence(e, calName, now, until); ok {
			out = append(out, ev)
		}
	}

	for _, e := range masters {
		if isCancelled(e) {
			continue
		}
		start, allDay, ok := startOf(e)
		if !ok {
			continue
		}
		base := Event{Calendar: calName} // fill() supplies every other field
		// Occurrences already under way still count as upcoming.
		from := now.Add(-lookback(e, allDay))

		rprop := e.GetProperty(ics.ComponentPropertyRrule)
		if rprop == nil {
			// Single event.
			if !start.Before(from) && !start.After(until) {
				out = append(out, fill(base, e, start, allDay))
			}
			continue
		}

		// Recurring: expand within the window.
		r, err := rrule.StrToRRule(rprop.Value)
		if err != nil {
			// Unparseable rule: fall back to the single master instance.
			if !start.Before(from) && !start.After(until) {
				out = append(out, fill(base, e, start, allDay))
			}
			continue
		}
		r.DTStart(start)
		ex := exdates(e)
		for _, occ := range r.Between(from, until, true) {
			if containsTime(suppressed, occ) || containsTime(ex, occ) {
				continue
			}
			out = append(out, fill(base, e, occ, allDay))
		}
	}

	return out
}

// occurrence builds an Event from the event's own DTSTART (used for single
// events and overrides). ok is false when the start is missing or outside the
// window.
func occurrence(e *ics.VEvent, calName string, now, until time.Time) (Event, bool) {
	start, allDay, ok := startOf(e)
	if !ok {
		return Event{}, false
	}
	ev := fill(Event{Calendar: calName}, e, start, allDay)
	if start.Before(now.Add(-lookback(e, allDay))) || start.After(until) {
		return ev, false
	}
	return ev, true
}

// fill populates summary/location/start/end for a given occurrence start,
// carrying the event's duration onto recurring instances.
func fill(base Event, e *ics.VEvent, start time.Time, allDay bool) Event {
	base.Summary = propValue(e, ics.ComponentPropertySummary)
	base.Location = propValue(e, ics.ComponentPropertyLocation)
	base.AllDay = allDay
	base.Start = start.Format(time.RFC3339)
	base.startAt = start
	if dur, ok := duration(e); ok {
		base.End = start.Add(dur).Format(time.RFC3339)
	}
	return base
}

// lookback is how far before `now` an occurrence may have started and still
// count as upcoming: an event stays listed until it ends. All-day events start
// at midnight and usually carry no DTEND, so they get a full day, keeping
// today's all-day events visible for the whole day.
func lookback(e *ics.VEvent, allDay bool) time.Duration {
	if d, ok := duration(e); ok {
		return d
	}
	if allDay {
		return 24 * time.Hour
	}
	return 0
}

// duration is DTEND-DTSTART of the master, applied to every occurrence.
func duration(e *ics.VEvent) (time.Duration, bool) {
	s, _, ok := startOf(e)
	if !ok {
		return 0, false
	}
	end, ok := endOf(e)
	if !ok {
		return 0, false
	}
	d := end.Sub(s)
	if d <= 0 {
		return 0, false
	}
	return d, true
}

func startOf(e *ics.VEvent) (time.Time, bool, bool) {
	p := e.GetProperty(ics.ComponentPropertyDtStart)
	if p == nil {
		return time.Time{}, false, false
	}
	if isDateOnly(p) {
		t, err := e.GetAllDayStartAt()
		if err != nil {
			return time.Time{}, true, false
		}
		return t, true, true
	}
	t, err := e.GetStartAt()
	if err != nil {
		return time.Time{}, false, false
	}
	return t, false, true
}

func endOf(e *ics.VEvent) (time.Time, bool) {
	p := e.GetProperty(ics.ComponentPropertyDtEnd)
	if p == nil {
		return time.Time{}, false
	}
	if isDateOnly(p) {
		t, err := e.GetAllDayEndAt()
		if err != nil {
			return time.Time{}, false
		}
		return t, true
	}
	t, err := e.GetEndAt()
	if err != nil {
		return time.Time{}, false
	}
	return t, true
}

func isDateOnly(p *ics.IANAProperty) bool {
	v := p.ICalParameters["VALUE"]
	return len(v) == 1 && strings.EqualFold(v[0], "DATE")
}

func isCancelled(e *ics.VEvent) bool {
	return strings.EqualFold(propValue(e, ics.ComponentPropertyStatus), "CANCELLED")
}

func propValue(e *ics.VEvent, p ics.ComponentProperty) string {
	if prop := e.GetProperty(p); prop != nil {
		return prop.Value
	}
	return ""
}

// exdates collects every EXDATE value (across repeated properties and
// comma-separated lists) as excluded occurrence times.
func exdates(e *ics.VEvent) []time.Time {
	var out []time.Time
	for i := range e.Properties {
		prop := &e.Properties[i]
		if !strings.EqualFold(prop.IANAToken, "EXDATE") {
			continue
		}
		loc := time.UTC
		if tz := prop.ICalParameters["TZID"]; len(tz) == 1 {
			if l, err := time.LoadLocation(tz[0]); err == nil {
				loc = l
			}
		}
		for _, v := range strings.Split(prop.Value, ",") {
			if t, ok := parseTimeValue(strings.TrimSpace(v), loc); ok {
				out = append(out, t)
			}
		}
	}
	return out
}

// parseTimeProp parses a single date-time property (RECURRENCE-ID), honouring
// its TZID parameter.
func parseTimeProp(p *ics.IANAProperty) (time.Time, bool) {
	if p == nil {
		return time.Time{}, false
	}
	loc := time.UTC
	if tz := p.ICalParameters["TZID"]; len(tz) == 1 {
		if l, err := time.LoadLocation(tz[0]); err == nil {
			loc = l
		}
	}
	return parseTimeValue(p.Value, loc)
}

// parseTimeValue handles the three iCal forms: UTC (...Z), floating/local
// date-time, and date-only.
func parseTimeValue(val string, loc *time.Location) (time.Time, bool) {
	switch {
	case strings.HasSuffix(val, "Z"):
		if t, err := time.ParseInLocation("20060102T150405Z", val, time.UTC); err == nil {
			return t, true
		}
	case len(val) == 8:
		if t, err := time.ParseInLocation("20060102", val, loc); err == nil {
			return t, true
		}
	default:
		if t, err := time.ParseInLocation("20060102T150405", val, loc); err == nil {
			return t, true
		}
	}
	return time.Time{}, false
}

// containsTime reports whether ts holds the same instant as t.
func containsTime(ts []time.Time, t time.Time) bool {
	for _, x := range ts {
		if x.Equal(t) {
			return true
		}
	}
	return false
}
