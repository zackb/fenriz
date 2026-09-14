// Package log is a tiny leveled logger that writes to stderr, leaving stdout
// clean for the line-delimited JSON event stream the bar consumes.
package log

import (
	"fmt"
	"os"
)

func write(level, format string, args ...any) {
	fmt.Fprintf(os.Stderr, "["+level+"] "+format+"\n", args...)
}

func Infof(format string, args ...any)  { write("info", format, args...) }
func Warnf(format string, args ...any)  { write("warn", format, args...) }
func Errorf(format string, args ...any) { write("error", format, args...) }
