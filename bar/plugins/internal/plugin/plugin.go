// Package plugin speaks the fenriz-bar plugin protocol: state as JSON lines on
// stdout, events as JSON lines on stdin. See bar/README.md.
package plugin

import (
	"bufio"
	"encoding/json"
	"os"
	"sync"
)

// Update is one line to the bar. Each key present replaces that slot
// ("pill", "chip", "banner", "tile", "page"); a nil value clears it.
type Update map[string]any

type Pill struct {
	Text  string `json:"text"`
	Icon  string `json:"icon,omitempty"`
	Image string `json:"image,omitempty"`
}

type Chip struct {
	Text string `json:"text"`
	Icon string `json:"icon,omitempty"`
}

// Banner is informational text under the date on Home.
type Banner = Chip

type Tile struct {
	Title string `json:"title"`
	Icon  string `json:"icon,omitempty"`
	Image string `json:"image,omitempty"`
}

type Page struct {
	Title  string  `json:"title,omitempty"`
	Blocks []Block `json:"blocks"`
}

type Block struct {
	Type      string     `json:"type"`
	Text      string     `json:"text,omitempty"`
	Subtitle  string     `json:"subtitle,omitempty"`
	Trailing  string     `json:"trailing,omitempty"`
	Icon      string     `json:"icon,omitempty"`
	Image     string     `json:"image,omitempty"`
	Style     string     `json:"style,omitempty"`
	Action    string     `json:"action,omitempty"`
	Value     float64    `json:"value,omitempty"`
	Highlight *int       `json:"highlight,omitempty"`
	Columns   []string   `json:"columns,omitempty"`
	Rows      [][]string `json:"rows,omitempty"`
	Items     []Block    `json:"items,omitempty"`
}

func Text(text, style string) Block { return Block{Type: "text", Text: text, Style: style} }

func Section(text string) Block { return Text(text, "section") }

// Table highlights row `highlight`; a negative one highlights nothing.
func Table(columns []string, rows [][]string, highlight int) Block {
	b := Block{Type: "table", Columns: columns, Rows: rows}
	if highlight >= 0 {
		b.Highlight = &highlight
	}
	return b
}

type Event struct {
	Event string `json:"event"` // open, close, resume, action
	ID    string `json:"id,omitempty"`
}

var mu sync.Mutex

// Emit writes one update line. Safe for concurrent use.
func Emit(u Update) {
	b, err := json.Marshal(u)
	if err != nil {
		return
	}
	mu.Lock()
	defer mu.Unlock()
	os.Stdout.Write(append(b, '\n'))
}

// Events delivers the bar's events and exits the process when the bar closes
// stdin, so a plugin never outlives its bar.
func Events() <-chan Event {
	ch := make(chan Event, 8)
	go func() {
		sc := bufio.NewScanner(os.Stdin)
		for sc.Scan() {
			var e Event
			if json.Unmarshal(sc.Bytes(), &e) == nil {
				select {
				case ch <- e:
				default: // a busy plugin misses a coalesced open/close, never blocks the reader
				}
			}
		}
		os.Exit(0)
	}()
	return ch
}
