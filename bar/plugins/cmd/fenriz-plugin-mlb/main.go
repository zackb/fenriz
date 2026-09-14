// Command fenriz-plugin-mlb shows one team's MLB game in fenriz-bar: the score
// on a Home tile, a pill notice when a run scores or the game ends, and
// a page with the game and the division and wild-card standings.
package main

import (
	"context"
	"flag"
	"fmt"
	"strconv"
	"strings"

	"fenriz-plugins/internal/mlb"
	"fenriz-plugins/internal/plugin"
)

func main() {
	team := flag.String("team", "SEA", "team abbreviation")
	flag.Parse()

	svc := mlb.New(*team)
	var last mlb.State
	go svc.Run(context.Background(), func(st mlb.State) {
		plugin.Emit(update(last, st, strings.ToUpper(*team)))
		last = st
	})

	for e := range plugin.Events() {
		if e.Event == "resume" {
			svc.OnResume()
		}
	}
}

func score(st mlb.State) string {
	return fmt.Sprintf("%s %d – %d %s", st.Away.Abbr, st.Away.Score, st.Home.Score, st.Home.Abbr)
}

func update(prev, st mlb.State, team string) plugin.Update {
	u := plugin.Update{"page": page(st)}
	if !st.Active {
		u["tile"] = nil
		return u
	}
	u["tile"] = tile(st, team)

	sameGame := prev.Active && prev.Home.Abbr == st.Home.Abbr && prev.Away.Abbr == st.Away.Abbr
	switch {
	case !sameGame:
	case st.Class == "mlb-final" && prev.Class != "mlb-final":
		u["pill"] = plugin.Pill{Text: "Final · " + score(st), Image: winner(st).Logo}
	case st.Home.Score > prev.Home.Score:
		u["pill"] = plugin.Pill{Text: score(st) + "  " + status(st), Image: st.Home.Logo}
	case st.Away.Score > prev.Away.Score:
		u["pill"] = plugin.Pill{Text: score(st) + "  " + status(st), Image: st.Away.Logo}
	}
	return u
}

// tile fits a tile's dozen characters: the team's logo over "7–8 · Final",
// its own score first, or just the first pitch before the game.
func tile(st mlb.State, team string) plugin.Tile {
	me, them := st.Home, st.Away
	if st.Away.Abbr == team {
		me, them = st.Away, st.Home
	}
	t := plugin.Tile{Image: me.Logo}
	switch st.Class {
	case "mlb-pre":
		t.Title = st.Status
	case "mlb-delay":
		t.Title = fmt.Sprintf("%d–%d · Delay", me.Score, them.Score)
	default:
		t.Title = fmt.Sprintf("%d–%d · %s", me.Score, them.Score, strings.TrimPrefix(status(st), "● "))
	}
	if t.Image == "" {
		t.Icon = "starred-symbolic"
	}
	return t
}

func status(st mlb.State) string {
	if st.Class == "mlb-final" {
		return "Final"
	}
	return st.Status
}

func winner(st mlb.State) mlb.Team {
	if st.Away.Score > st.Home.Score {
		return st.Away
	}
	return st.Home
}

func page(st mlb.State) plugin.Page {
	p := plugin.Page{Title: "MLB"}
	if !st.Active {
		p.Blocks = append(p.Blocks, plugin.Text(st.Tooltip, "dim"))
	} else {
		for _, t := range []mlb.Team{st.Away, st.Home} {
			p.Blocks = append(p.Blocks, plugin.Block{
				Type: "row", Image: t.Logo, Text: t.Name, Trailing: strconv.Itoa(t.Score),
			})
		}
		line := status(st)
		if st.Stale {
			line += " · offline, last known score"
		}
		p.Blocks = append(p.Blocks, plugin.Text(line, "dim"))
	}

	if s := st.Standings; s != nil {
		p.Blocks = append(p.Blocks, plugin.Section(s.Division), standings(s.Teams))
		if len(s.WildCard) > 0 {
			p.Blocks = append(p.Blocks, plugin.Section("Wild card"), standings(s.WildCard))
		}
	}
	return p
}

func standings(rows []mlb.Standing) plugin.Block {
	me := -1
	out := make([][]string, len(rows))
	for i, r := range rows {
		out[i] = []string{r.Abbr, strconv.Itoa(r.Wins), strconv.Itoa(r.Losses), r.GB, r.L10, r.Streak}
		if r.Me {
			me = i
		}
	}
	return plugin.Table([]string{"", "W", "L", "GB", "L10", "STRK"}, out, me)
}
