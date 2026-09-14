// Command fenriz-plugin-weather shows the weather from Open-Meteo (no API key)
// in fenriz-bar: current conditions in the island footer and a page with the
// next hours and the week.
package main

import (
	"context"
	"encoding/json"
	"flag"
	"fmt"
	"math"
	"net/http"
	"net/url"
	"strconv"
	"time"

	"fenriz-plugins/internal/log"
	"fenriz-plugins/internal/plugin"
)

const (
	endpoint = "https://api.open-meteo.com/v1/forecast"
	refresh  = 15 * time.Minute
	retryMin = 10 * time.Second
	hours    = 6 // columns in the next-hours table, 2 hours apart
)

type forecast struct {
	Current struct {
		Temperature float64 `json:"temperature_2m"`
		Apparent    float64 `json:"apparent_temperature"`
		Humidity    float64 `json:"relative_humidity_2m"`
		Wind        float64 `json:"wind_speed_10m"`
		Code        int     `json:"weather_code"`
		IsDay       int     `json:"is_day"`
	} `json:"current"`
	Hourly struct {
		Time   []string  `json:"time"`
		Temp   []float64 `json:"temperature_2m"`
		Code   []int     `json:"weather_code"`
		Precip []float64 `json:"precipitation_probability"`
	} `json:"hourly"`
	Daily struct {
		Time   []string  `json:"time"`
		Code   []int     `json:"weather_code"`
		Max    []float64 `json:"temperature_2m_max"`
		Min    []float64 `json:"temperature_2m_min"`
		Precip []float64 `json:"precipitation_probability_max"`
	} `json:"daily"`
}

func main() {
	lat := flag.Float64("lat", math.NaN(), "latitude")
	lon := flag.Float64("lon", math.NaN(), "longitude")
	fahrenheit := flag.Bool("fahrenheit", false, "°F and mph instead of °C and km/h")
	flag.Parse()

	events := plugin.Events()
	if math.IsNaN(*lat) || math.IsNaN(*lon) {
		plugin.Emit(plugin.Update{"page": plugin.Page{Title: "Weather", Blocks: []plugin.Block{
			plugin.Text("Set -lat and -lon in the plugin's command in fenriz-desktop.conf", "dim"),
		}}})
		for range events {
		}
	}

	q := url.Values{}
	q.Set("latitude", strconv.FormatFloat(*lat, 'f', 4, 64))
	q.Set("longitude", strconv.FormatFloat(*lon, 'f', 4, 64))
	q.Set("current", "temperature_2m,apparent_temperature,relative_humidity_2m,wind_speed_10m,weather_code,is_day")
	q.Set("hourly", "temperature_2m,weather_code,precipitation_probability")
	q.Set("daily", "weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max")
	q.Set("timezone", "auto")
	q.Set("forecast_days", "7")
	wind := "km/h"
	if *fahrenheit {
		q.Set("temperature_unit", "fahrenheit")
		q.Set("wind_speed_unit", "mph")
		wind = "mph"
	}
	req := endpoint + "?" + q.Encode()

	client := &http.Client{Timeout: 10 * time.Second}
	timer := time.NewTimer(0)
	retry := retryMin
	for {
		select {
		case e := <-events:
			if e.Event != "resume" { // the timer froze while asleep
				continue
			}
		case <-timer.C:
		}
		f, err := fetch(client, req)
		if err != nil {
			log.Warnf("weather: %v", err)
			timer.Reset(retry)
			retry = min(retry*2, refresh)
			continue
		}
		timer.Reset(refresh)
		retry = retryMin
		plugin.Emit(plugin.Update{"chip": chip(f), "page": page(f, wind, time.Now())})
	}
}

func fetch(client *http.Client, req string) (*forecast, error) {
	ctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
	defer cancel()
	r, err := http.NewRequestWithContext(ctx, http.MethodGet, req, nil)
	if err != nil {
		return nil, err
	}
	resp, err := client.Do(r)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("status %s", resp.Status)
	}
	var f forecast
	return &f, json.NewDecoder(resp.Body).Decode(&f)
}

func degrees(v float64) string { return fmt.Sprintf("%.0f°", v) }

func chip(f *forecast) plugin.Chip {
	c := f.Current
	return plugin.Chip{Text: degrees(c.Temperature), Icon: icon(c.Code, c.IsDay == 1)}
}

func page(f *forecast, wind string, now time.Time) plugin.Page {
	c := f.Current
	p := plugin.Page{Title: "Weather", Blocks: []plugin.Block{{
		Type:     "row",
		Icon:     icon(c.Code, c.IsDay == 1),
		Text:     degrees(c.Temperature) + " · " + describe(c.Code),
		Subtitle: fmt.Sprintf("Feels %s · %.0f %s · %.0f%% humidity", degrees(c.Apparent), c.Wind, wind, c.Humidity),
	}}}

	// Hourly times are local to the forecast's timezone, without an offset.
	h := f.Hourly
	first := 0
	for first < len(h.Time) {
		if t, err := time.ParseInLocation("2006-01-02T15:04", h.Time[first], now.Location()); err == nil && t.After(now) {
			break
		}
		first++
	}
	cols := []string{""}
	temps := []string{"Temp"}
	rain := []string{"Rain"}
	n := min(len(h.Time), len(h.Temp), len(h.Precip))
	for i := first; i < n && len(cols) <= hours; i += 2 {
		t, _ := time.ParseInLocation("2006-01-02T15:04", h.Time[i], now.Location())
		cols = append(cols, t.Format("3pm"))
		temps = append(temps, degrees(h.Temp[i]))
		rain = append(rain, fmt.Sprintf("%.0f%%", h.Precip[i]))
	}
	if len(cols) > 1 {
		p.Blocks = append(p.Blocks, plugin.Section("Next hours"), plugin.Table(cols, [][]string{temps, rain}, -1))
	}

	d := f.Daily
	var days [][]string
	for i := range min(len(d.Time), len(d.Code), len(d.Max), len(d.Min), len(d.Precip)) {
		t, err := time.Parse("2006-01-02", d.Time[i])
		if err != nil {
			continue
		}
		name := t.Format("Mon")
		if i == 0 {
			name = "Today"
		}
		days = append(days, []string{name, describe(d.Code[i]), degrees(d.Max[i]), degrees(d.Min[i]),
			fmt.Sprintf("%.0f%%", d.Precip[i])})
	}
	if len(days) > 0 {
		p.Blocks = append(p.Blocks, plugin.Section("This week"),
			plugin.Table([]string{"", "", "High", "Low", "Rain"}, days, -1))
	}
	return p
}

// WMO weather interpretation codes, as Open-Meteo documents them.
func describe(code int) string {
	switch {
	case code == 0:
		return "Clear"
	case code <= 2:
		return "Partly cloudy"
	case code == 3:
		return "Cloudy"
	case code <= 48:
		return "Fog"
	case code <= 57:
		return "Drizzle"
	case code <= 67:
		return "Rain"
	case code <= 77:
		return "Snow"
	case code <= 82:
		return "Showers"
	case code <= 86:
		return "Snow showers"
	default:
		return "Thunderstorm"
	}
}

func icon(code int, day bool) string {
	switch {
	case code == 0 && day:
		return "weather-clear-symbolic"
	case code == 0:
		return "weather-clear-night-symbolic"
	case code <= 2 && day:
		return "weather-few-clouds-symbolic"
	case code <= 2:
		return "weather-few-clouds-night-symbolic"
	case code == 3:
		return "weather-overcast-symbolic"
	case code <= 48:
		return "weather-fog-symbolic"
	case code <= 67:
		return "weather-showers-symbolic"
	case code <= 77, code >= 85 && code <= 86:
		return "weather-snow-symbolic"
	case code <= 82:
		return "weather-showers-scattered-symbolic"
	default:
		return "weather-storm-symbolic"
	}
}
