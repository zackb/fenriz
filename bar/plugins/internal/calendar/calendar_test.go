package calendar

import (
	"os"
	"path/filepath"
	"testing"
	"time"
)

// write an .ics into a temp dir and expand it through eventsFromFile.
func expand(t *testing.T, ics string, now time.Time) []Event {
	t.Helper()
	path := filepath.Join(t.TempDir(), "e.ics")
	if err := os.WriteFile(path, []byte(ics), 0o600); err != nil {
		t.Fatal(err)
	}
	return eventsFromFile(path, "Test", now, now.Add(horizon))
}

func wrap(body string) string {
	return "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:-//test//EN\r\n" + body + "END:VCALENDAR\r\n"
}

// An all-day event for today must stay listed all day, not vanish at 00:00:01.
// It starts at midnight, so a naive `start >= now` filter drops it immediately.
func TestAllDayTodayStaysVisible(t *testing.T) {
	now := time.Date(2026, 8, 2, 15, 30, 0, 0, time.Local)
	got := expand(t, wrap(
		"BEGIN:VEVENT\r\nUID:a\r\nSUMMARY:Holiday\r\n"+
			"DTSTART;VALUE=DATE:20260802\r\nEND:VEVENT\r\n"), now)

	if len(got) != 1 {
		t.Fatalf("want today's all-day event, got %d events: %+v", len(got), got)
	}
	if !got[0].AllDay || got[0].Summary != "Holiday" {
		t.Fatalf("unexpected event: %+v", got[0])
	}
}

// A timed event already under way stays listed until its DTEND; one that has
// finished does not.
func TestInProgressVsFinished(t *testing.T) {
	now := time.Date(2026, 8, 2, 10, 30, 0, 0, time.UTC)

	inProgress := expand(t, wrap(
		"BEGIN:VEVENT\r\nUID:b\r\nSUMMARY:Standup\r\n"+
			"DTSTART:20260802T100000Z\r\nDTEND:20260802T110000Z\r\nEND:VEVENT\r\n"), now)
	if len(inProgress) != 1 {
		t.Fatalf("in-progress event should be listed, got %+v", inProgress)
	}

	finished := expand(t, wrap(
		"BEGIN:VEVENT\r\nUID:c\r\nSUMMARY:Earlier\r\n"+
			"DTSTART:20260802T080000Z\r\nDTEND:20260802T090000Z\r\nEND:VEVENT\r\n"), now)
	if len(finished) != 0 {
		t.Fatalf("finished event should be dropped, got %+v", finished)
	}
}

// Events are ordered by instant. RFC3339 strings carry a UTC offset, so sorting
// them lexically puts the later-but-westward event first.
func TestSortAcrossTimezones(t *testing.T) {

	early := Event{Summary: "early", Start: "2026-08-02T12:00:00+02:00",
		startAt: time.Date(2026, 8, 2, 10, 0, 0, 0, time.UTC)}
	late := Event{Summary: "late", Start: "2026-08-02T10:00:00-07:00",
		startAt: time.Date(2026, 8, 2, 17, 0, 0, 0, time.UTC)}

	if !(late.Start < early.Start) {
		t.Fatal("precondition: the strings should sort the wrong way round")
	}

	got := sortTrim([]Event{late, early})
	if len(got) != 2 || got[0].Summary != "early" {
		t.Fatalf("want early first, got %+v", got)
	}
}
