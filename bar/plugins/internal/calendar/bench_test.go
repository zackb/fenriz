package calendar

import (
	"fmt"
	"os"
	"path/filepath"
	"testing"
	"time"
)

// Simulate a realistic calendar: 25 daily/weekly recurring events + 50 one-offs,
// then measure one full expansion pass (what tick() does every 60s).
func BenchmarkExpand(b *testing.B) {
	dir := b.TempDir()
	for i := 0; i < 25; i++ {
		body := fmt.Sprintf("BEGIN:VEVENT\r\nUID:r%d\r\nSUMMARY:Recur %d\r\n"+
			"DTSTART:20260101T090000Z\r\nDTEND:20260101T093000Z\r\n"+
			"RRULE:FREQ=DAILY\r\nEND:VEVENT\r\n", i, i)
		os.WriteFile(filepath.Join(dir, fmt.Sprintf("r%d.ics", i)),
			[]byte(wrap(body)), 0o600)
	}
	for i := 0; i < 50; i++ {
		body := fmt.Sprintf("BEGIN:VEVENT\r\nUID:s%d\r\nSUMMARY:Single %d\r\n"+
			"DTSTART:20260815T090000Z\r\nDTEND:20260815T093000Z\r\nEND:VEVENT\r\n", i, i)
		os.WriteFile(filepath.Join(dir, fmt.Sprintf("s%d.ics", i)),
			[]byte(wrap(body)), 0o600)
	}
	files, _ := filepath.Glob(filepath.Join(dir, "*.ics"))
	now := time.Date(2026, 8, 2, 12, 0, 0, 0, time.UTC)

	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		var out []Event
		for _, f := range files {
			out = append(out, eventsFromFile(f, "Test", now, now.Add(horizon))...)
		}
		sortTrim(out)
	}
}
