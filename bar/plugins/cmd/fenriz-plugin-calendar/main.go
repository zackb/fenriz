// Command fenriz-plugin-calendar shows upcoming events from the local
// vdirsyncer store in fenriz-bar: the next event today in the island footer, a
// pill notice shortly before one starts, and a page listing what is coming.
package main

import (
	"context"
	"fmt"
	"math"
	"reflect"
	"strings"
	"time"

	"fenriz-plugins/internal/calendar"
	"fenriz-plugins/internal/plugin"
)

const (
	remindBefore = 10 * time.Minute
	tick         = 30 * time.Second // chip, reminders and day names follow the clock
)

func main() {
	scans := make(chan []calendar.Event, 1)
	go calendar.New().Run(context.Background(), func(st calendar.State) {
		scans <- st.Upcoming
	})

	events := plugin.Events()
	ticker := time.NewTicker(tick)
	var (
		upcoming []calendar.Event
		sent     = plugin.Update{}
		reminded = map[string]bool{}
	)
	for {
		select {
		case upcoming = <-scans:
		case <-events:
		case <-ticker.C:
		}
		now := time.Now()
		u := plugin.Update{}
		// only what changed, so an idle calendar costs the bar nothing
		for key, v := range map[string]any{"chip": nextToday(upcoming, now), "page": page(upcoming, now)} {
			if old, ok := sent[key]; !ok || !reflect.DeepEqual(old, v) {
				u[key], sent[key] = v, v
			}
		}
		if p := reminder(upcoming, now, reminded); p != nil {
			u["pill"] = *p
		}
		if len(u) > 0 {
			plugin.Emit(u)
		}
	}
}

// reminder is a notice for an event starting within remindBefore, once per event.
func reminder(evs []calendar.Event, now time.Time, reminded map[string]bool) *plugin.Pill {
	for _, e := range evs {
		start, err := time.Parse(time.RFC3339, e.Start)
		key := e.Summary + e.Start
		if err != nil || e.AllDay || reminded[key] || start.Before(now) || start.Sub(now) > remindBefore {
			continue
		}
		reminded[key] = true
		mins := int(math.Ceil(start.Sub(now).Minutes()))
		return &plugin.Pill{Text: fmt.Sprintf("%s in %d min", strings.TrimSpace(e.Summary), mins), Icon: "x-office-calendar-symbolic"}
	}
	return nil
}

// nextToday is the chip for the next timed event still ahead today, or nil.
func nextToday(evs []calendar.Event, now time.Time) any {
	for _, e := range evs {
		start, err := time.Parse(time.RFC3339, e.Start)
		if err != nil || e.AllDay || start.Before(now) {
			continue
		}
		if !sameDay(start.Local(), now) {
			return nil
		}
		return plugin.Chip{Text: strings.TrimSpace(e.Summary) + " · " + clock(start), Icon: "x-office-calendar-symbolic"}
	}
	return nil
}

func page(evs []calendar.Event, now time.Time) plugin.Page {
	p := plugin.Page{Title: "Upcoming"}
	if len(evs) == 0 {
		p.Blocks = []plugin.Block{plugin.Text("Nothing coming up", "dim")}
		return p
	}
	day := ""
	for _, e := range evs {
		start, err := time.Parse(time.RFC3339, e.Start)
		if err != nil {
			continue
		}
		start = start.Local()
		if d := dayName(start, now); d != day {
			day = d
			p.Blocks = append(p.Blocks, plugin.Section(d), plugin.Block{Type: "list"})
		}
		when := "All day"
		if !e.AllDay {
			when = clock(start)
			if end, err := time.Parse(time.RFC3339, e.End); err == nil {
				when += "–" + clock(end)
			}
		}
		if place, _, _ := strings.Cut(e.Location, "\n"); place != "" { // the rest of an address is noise here
			when += " · " + strings.TrimSpace(place)
		}
		list := &p.Blocks[len(p.Blocks)-1]
		list.Items = append(list.Items, plugin.Block{Text: strings.TrimSpace(e.Summary), Subtitle: when})
	}
	return p
}

func sameDay(a, b time.Time) bool {
	ay, am, ad := a.Date()
	by, bm, bd := b.Date()
	return ay == by && am == bm && ad == bd
}

func dayName(t, now time.Time) string {
	switch {
	case sameDay(t, now) || t.Before(now):
		return "Today"
	case sameDay(t, now.AddDate(0, 0, 1)):
		return "Tomorrow"
	default:
		return t.Format("Monday, January 2")
	}
}

func clock(t time.Time) string { return t.Local().Format("3:04pm") }
