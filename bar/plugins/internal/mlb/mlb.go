// Package mlb tracks the configured team's MLB game and streams a compact
// scoreboard to the bar. Uses the MLB Stats API
// It polls every couple of minutes during a live game and otherwise sleeps
// until just before first pitch when a game is hours away, or until the next
// day when there's no game or the game is over
//
// The team is the caller's ("SEA" or "LAD"). Team cap logos are downloaded once and
// cached on disk, and their paths are handed to the bar so it can draw them
package mlb

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"time"

	"fenriz-plugins/internal/log"
)

const (
	scheduleURL = "https://statsapi.mlb.com/api/v1/schedule?sportId=1&date=%s&hydrate=linescore,team"
	// Both drawer blocks in one request: every division's order plus each
	// league's wild-card race, already ranked by the API.
	standingsURL = "https://statsapi.mlb.com/api/v1/standings?leagueId=103,104&standingsTypes=regularSeason,wildCard&hydrate=team"
	logoURL      = "https://www.mlbstatic.com/team-logos/%d.svg"
	userAgent    = "Mozilla/5.0 (X11; Linux x86_64; rv:124.0) Gecko/20100101 Firefox/124.0"

	livePoll    = 2 * time.Minute  // refresh cadence while a game is in progress
	preBuffer   = 10 * time.Minute // wake this long before a scheduled first pitch
	errRetryMin = 5 * time.Second  // first retry after a fetch failure
	errRetryMax = 5 * time.Minute  // backoff cap for sustained outages

	standingsTTL  = 15 * time.Minute // standings move at most once per game
	wildCardSpots = 3                // playoff berths shown in the wild-card block
)

// Team is one club's line in the scoreboard. Logo is a local file path (or empty if
// the cap logo couldn't be fetched).
type Team struct {
	Abbr  string `json:"abbr"`
	Name  string `json:"name"`
	Score int    `json:"score"`
	Logo  string `json:"logo"`
}

// Standing is one club's line in the standings drawer.
type Standing struct {
	Rank   string `json:"rank"` // divisionRank, or wildCardRank in the wild-card block
	Abbr   string `json:"abbr"`
	Wins   int    `json:"wins"`
	Losses int    `json:"losses"`
	Pct    string `json:"pct"`
	GB     string `json:"gb"`     // gamesBack, or wildCardGamesBack in the wild-card block
	L10    string `json:"l10"`    // "5-5"
	Streak string `json:"streak"` // "W1"
	Me     bool   `json:"me"`     // the configured team, highlighted by the widget
}

// Standings is the drawer payload: the configured team's division plus the
// wild-card race. WildCard holds the playoff spots, with the team's own row
// appended when it's chasing from further back.
type Standings struct {
	Division string     `json:"division"` // "AL West"
	Teams    []Standing `json:"teams"`
	WildCard []Standing `json:"wildCard"`
}

// State is the payload emitted to the bar. Active is false (game idle / error)
// when there's nothing worth showing, which the widget treats as "hide me".
type State struct {
	Active  bool   `json:"active"`
	Class   string `json:"class"` // mlb-live | mlb-delay | mlb-final | mlb-pre | mlb-idle | mlb-error
	Status  string `json:"status"`
	Tooltip string `json:"tooltip"`
	Stale   bool   `json:"stale"` // last-known data re-shown during a fetch outage
	Home    Team   `json:"home"`
	Away    Team   `json:"away"`
	// nil until the first standings fetch lands; the widget's drawer stays empty.
	Standings *Standings `json:"standings,omitempty"`
}

type Service struct {
	team    string
	logoDir string
	client  *http.Client
	emit    func(State)
	last    State         // last good (active) state, replayed across transient outages
	resume  chan struct{} // poked by OnResume to force a re-poll after suspend

	standings   *Standings // last good standings, refreshed on standingsTTL
	standingsAt time.Time
}

func New(team string) *Service {
	team = strings.ToUpper(strings.TrimSpace(team))
	dir := ""
	if cache, err := os.UserCacheDir(); err == nil {
		dir = filepath.Join(cache, "fenriz", "mlb-logos")
	}
	return &Service{
		team:    team,
		logoDir: dir,
		client:  &http.Client{Timeout: 8 * time.Second},
		resume:  make(chan struct{}, 1),
	}
}

// Run polls and emits until ctx ends.
func (s *Service) Run(ctx context.Context, emit func(State)) {
	s.emit = emit
	s.run(ctx)
}

// OnResume nudges the poll loop to refresh after a resume from suspend, where
// its monotonic-clock sleep was frozen and the score is likely stale. The send
// is non-blocking -- a coalesced wake is all the loop needs.
func (s *Service) OnResume() {
	select {
	case s.resume <- struct{}{}:
	default:
	}
}

// run polls, emits, then sleeps for a state-dependent interval until ctx ends.
// Fetch failures retry on an exponential backoff (errRetryMin..errRetryMax) so
// the widget recovers within seconds of the network returning, and the last
// good scoreboard is replayed (flagged stale) so a transient blip doesn't blank
// a live game.
func (s *Service) run(ctx context.Context) {
	var fails int
	for {
		st, next, err := s.poll(ctx)
		if err != nil {
			fails++
			next = backoff(fails)
			// Only surface the bare error state when we have nothing good to
			// fall back to (e.g. boot before the network is up), where the
			// widget stays hidden anyway. Otherwise keep the last game on
			// screen, just dimmed as stale.
			if s.last.Active {
				st = s.last
				st.Stale = true
			}
		} else {
			fails = 0
			s.last = st
		}
		s.emit(st)
		select {
		case <-ctx.Done():
			return
		case <-time.After(next):
		case <-s.resume:
			fails = 0 // re-poll now; if it fails, retry from errRetryMin not a long backoff
		}
	}
}

func (s *Service) poll(ctx context.Context) (State, time.Duration, error) {
	now := time.Now()
	games, err := s.fetch(ctx, fmt.Sprintf(scheduleURL, now.Format("2006-01-02")))
	if err != nil {
		log.Warnf("mlb: fetch: %v", err)
		return State{Active: false, Class: "mlb-error", Tooltip: err.Error()}, 0, err
	}

	game, ok := s.pick(games)
	if !ok {
		return State{
			Active:  false,
			Class:   "mlb-idle",
			Tooltip: fmt.Sprintf("No %s game today", s.team),
		}, untilNextMorning(now), nil
	}
	st, next := s.format(ctx, game, now)
	return st, next, nil
}

// backoff returns the sleep before the next retry after fails consecutive fetch
// failures: errRetryMin doubled each time, capped at errRetryMax.
func backoff(fails int) time.Duration {
	d := errRetryMin
	for i := 1; i < fails && d < errRetryMax; i++ {
		d *= 2
	}
	if d > errRetryMax {
		d = errRetryMax
	}
	return d
}

// MLB Stats API

type apiResponse struct {
	Dates []struct {
		Games []apiGame `json:"games"`
	} `json:"dates"`
}

type apiGame struct {
	GameDate string `json:"gameDate"`
	Status   struct {
		CodedGameState string `json:"codedGameState"`
		DetailedState  string `json:"detailedState"`
		Reason         string `json:"reason"` // e.g. "Rain" during a delay/suspension
	} `json:"status"`
	Teams struct {
		Home apiSide `json:"home"`
		Away apiSide `json:"away"`
	} `json:"teams"`
	Linescore struct {
		CurrentInning int    `json:"currentInning"`
		InningHalf    string `json:"inningHalf"`
	} `json:"linescore"`
}

type apiSide struct {
	Score int `json:"score"`
	Team  struct {
		ID           int    `json:"id"`
		Abbreviation string `json:"abbreviation"`
		TeamName     string `json:"teamName"`
	} `json:"team"`
}

// get performs a JSON GET against the Stats API and returns the raw body.
func (s *Service) get(ctx context.Context, url string) ([]byte, error) {
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, url, nil)
	if err != nil {
		return nil, err
	}
	req.Header.Set("User-Agent", userAgent)
	req.Header.Set("Accept", "application/json")

	resp, err := s.client.Do(req)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("status %s", resp.Status)
	}
	return io.ReadAll(resp.Body)
}

func (s *Service) fetch(ctx context.Context, url string) ([]apiGame, error) {
	body, err := s.get(ctx, url)
	if err != nil {
		return nil, err
	}
	var data apiResponse
	if err := json.Unmarshal(body, &data); err != nil {
		return nil, err
	}
	if len(data.Dates) == 0 {
		return nil, nil
	}
	return data.Dates[0].Games, nil
}

// pick narrows the day's games to the configured team and chooses the most
// interesting one: a live game over an upcoming one over a finished one. This
// keeps doubleheaders sane.
func (s *Service) pick(games []apiGame) (apiGame, bool) {
	var mine []apiGame
	for _, g := range games {
		if g.Teams.Home.Team.Abbreviation == s.team || g.Teams.Away.Team.Abbreviation == s.team {
			mine = append(mine, g)
		}
	}
	if len(mine) == 0 {
		return apiGame{}, false
	}
	sort.SliceStable(mine, func(i, j int) bool { return rank(mine[i]) < rank(mine[j]) })
	return mine[0], true
}

type phase int

const (
	pre phase = iota
	live
	final
)

func phaseOf(code string) phase {
	switch code {
	case "I", "MA", "MC":
		return live
	case "F", "FT", "FR", "O", "UR":
		return final
	default:
		return pre
	}
}

// delayed reports whether the game is paused (rain delay, suspended start, etc).
// A delay can strike before or during a game and isn't reliably encoded in the
// single-letter codedGameState, so key off the human-readable detailedState.
func delayed(g apiGame) bool {
	s := strings.ToLower(g.Status.DetailedState)
	return strings.Contains(s, "delay") || strings.Contains(s, "suspended")
}

// delayStatus is the compact badge shown while a game is delayed, e.g.
// "⏸ Rain Delay". Falls back to a bare "⏸ Delayed" when no reason is given.
func delayStatus(g apiGame) string {
	reason := strings.TrimSpace(g.Status.Reason)
	if reason == "" {
		// detailedState often carries the cause after a colon, e.g.
		// "Delayed: Rain" or "Delayed Start: Rain".
		if i := strings.LastIndex(g.Status.DetailedState, ":"); i >= 0 {
			reason = strings.TrimSpace(g.Status.DetailedState[i+1:])
		}
	}
	if reason == "" {
		return "⏸ Delayed"
	}
	return fmt.Sprintf("⏸ %s Delay", reason)
}

func rank(g apiGame) int {
	if delayed(g) {
		return 0 // a delay is as worth showing as a live game
	}
	switch phaseOf(g.Status.CodedGameState) {
	case live:
		return 0
	case pre:
		return 1
	default:
		return 2
	}
}

// format builds the emitted State and the duration to sleep before the next poll.
func (s *Service) format(ctx context.Context, g apiGame, now time.Time) (State, time.Duration) {
	home := s.side(g.Teams.Home)
	away := s.side(g.Teams.Away)

	st := State{
		Active:    true,
		Home:      home,
		Away:      away,
		Standings: s.standingsFor(ctx),
		Tooltip: fmt.Sprintf("%s vs %s\nScore: %d – %d\nStatus: %s",
			home.Name, away.Name, home.Score, away.Score, g.Status.DetailedState),
	}

	// A delay overrides the normal pre/live/final phase: keep showing the
	// running score but flag the stoppage and poll often for the resume.
	if delayed(g) {
		st.Class = "mlb-delay"
		st.Status = delayStatus(g)
		return st, livePoll
	}

	var next time.Duration
	switch phaseOf(g.Status.CodedGameState) {
	case live:
		half := "T"
		if !strings.EqualFold(g.Linescore.InningHalf, "Top") {
			half = "B"
		}
		st.Class = "mlb-live"
		st.Status = fmt.Sprintf("● %s%d", half, g.Linescore.CurrentInning)
		next = livePoll

	case final:
		st.Class = "mlb-final"
		st.Status = "F"
		next = untilNextMorning(now)

	default: // pre-game
		st.Class = "mlb-pre"
		next = livePoll // about to start: keep an eye on it
		if start, err := time.Parse(time.RFC3339, g.GameDate); err == nil {
			st.Status = strings.ToLower(start.Local().Format("3:04PM"))
			if d := time.Until(start); d > 15*time.Minute {
				next = d - preBuffer // hours away: sleep until just before first pitch
			}
		}
	}
	return st, next
}

func (s *Service) side(a apiSide) Team {
	return Team{
		Abbr:  a.Team.Abbreviation,
		Name:  a.Team.TeamName,
		Score: a.Score,
		Logo:  s.logo(a.Team.ID),
	}
}

// Standings

type apiStandings struct {
	Records []apiStandRecord `json:"records"`
}

type apiStandRecord struct {
	StandingsType string          `json:"standingsType"` // regularSeason | wildCard
	TeamRecords   []apiTeamRecord `json:"teamRecords"`
}

type apiTeamRecord struct {
	Team struct {
		Abbreviation string `json:"abbreviation"`
		Division     struct {
			Name string `json:"name"`
		} `json:"division"`
	} `json:"team"`
	DivisionRank      string `json:"divisionRank"`
	WildCardRank      string `json:"wildCardRank"`
	GamesBack         string `json:"gamesBack"`
	WildCardGamesBack string `json:"wildCardGamesBack"`
	LeagueRecord      struct {
		Wins   int    `json:"wins"`
		Losses int    `json:"losses"`
		Pct    string `json:"pct"`
	} `json:"leagueRecord"`
	Streak struct {
		StreakCode string `json:"streakCode"`
	} `json:"streak"`
	Records struct {
		SplitRecords []struct {
			Wins   int    `json:"wins"`
			Losses int    `json:"losses"`
			Type   string `json:"type"`
		} `json:"splitRecords"`
	} `json:"records"`
}

// standingsFor returns the drawer's standings, refetching only once the cached
// copy passes standingsTTL. A failure is never fatal -- the score matters more
// than the drawer, so the last good copy (or nil) is reused.
func (s *Service) standingsFor(ctx context.Context) *Standings {
	if s.standings != nil && time.Since(s.standingsAt) < standingsTTL {
		return s.standings
	}
	body, err := s.get(ctx, standingsURL)
	if err == nil {
		var data apiStandings
		err = json.Unmarshal(body, &data)
		if err == nil {
			if st := buildStandings(data.Records, s.team); st != nil {
				s.standings = st
				s.standingsAt = time.Now()
			}
		}
	}
	if err != nil {
		log.Warnf("mlb: standings: %v", err)
	}
	return s.standings
}

// buildStandings picks the drawer's two blocks out of a standings response: the
// team's division in API order, and the wild-card race trimmed to the playoff
// spots plus the team itself when it's chasing from further back. Returns nil
// if the team isn't in the response (off-season, renamed club).
func buildStandings(recs []apiStandRecord, team string) *Standings {
	var div *apiStandRecord
	for i := range recs {
		if recs[i].StandingsType == "regularSeason" && hasTeam(recs[i], team) {
			div = &recs[i]
			break
		}
	}
	if div == nil || len(div.TeamRecords) == 0 {
		return nil
	}

	out := &Standings{Division: shortDivision(div.TeamRecords[0].Team.Division.Name)}
	for _, tr := range div.TeamRecords {
		out.Teams = append(out.Teams, standingRow(tr, tr.DivisionRank, tr.GamesBack, team))
	}

	for i := range recs {
		if recs[i].StandingsType != "wildCard" || !hasTeam(recs[i], team) {
			continue
		}
		for _, tr := range recs[i].TeamRecords {
			r := standingRow(tr, tr.WildCardRank, tr.WildCardGamesBack, team)
			if len(out.WildCard) < wildCardSpots || r.Me {
				out.WildCard = append(out.WildCard, r)
			}
		}
		break
	}
	return out
}

func hasTeam(r apiStandRecord, team string) bool {
	for _, tr := range r.TeamRecords {
		if tr.Team.Abbreviation == team {
			return true
		}
	}
	return false
}

func standingRow(tr apiTeamRecord, rank, gb, team string) Standing {
	return Standing{
		Rank:   rank,
		Abbr:   tr.Team.Abbreviation,
		Wins:   tr.LeagueRecord.Wins,
		Losses: tr.LeagueRecord.Losses,
		Pct:    tr.LeagueRecord.Pct,
		GB:     gb,
		L10:    lastTen(tr),
		Streak: tr.Streak.StreakCode,
		Me:     tr.Team.Abbreviation == team,
	}
}

func lastTen(tr apiTeamRecord) string {
	for _, sr := range tr.Records.SplitRecords {
		if sr.Type == "lastTen" {
			return fmt.Sprintf("%d-%d", sr.Wins, sr.Losses)
		}
	}
	return ""
}

// shortDivision trims "American League West" to "AL West".
func shortDivision(name string) string {
	name = strings.Replace(name, "American League", "AL", 1)
	return strings.Replace(name, "National League", "NL", 1)
}

// Logos

// logo returns a local path to the team's cap logo, downloading it once. A
// failure is non-fatal: the widget falls back to text when the path is empty.
func (s *Service) logo(id int) string {
	if s.logoDir == "" || id == 0 {
		return ""
	}
	path := filepath.Join(s.logoDir, fmt.Sprintf("%d.svg", id))
	if _, err := os.Stat(path); err == nil {
		return path
	}
	if err := os.MkdirAll(s.logoDir, 0o755); err != nil {
		log.Warnf("mlb: logo dir: %v", err)
		return ""
	}
	if err := s.download(fmt.Sprintf(logoURL, id), path); err != nil {
		log.Warnf("mlb: logo %d: %v", id, err)
		return ""
	}
	return path
}

// download fetches url to dst atomically (write temp, rename) so a partial file
// can never be served as a cached logo.
func (s *Service) download(url, dst string) error {
	req, err := http.NewRequest(http.MethodGet, url, nil)
	if err != nil {
		return err
	}
	req.Header.Set("User-Agent", userAgent)
	resp, err := s.client.Do(req)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return fmt.Errorf("status %s", resp.Status)
	}
	tmp, err := os.CreateTemp(s.logoDir, "logo-*.tmp")
	if err != nil {
		return err
	}
	defer os.Remove(tmp.Name())
	if _, err := io.Copy(tmp, resp.Body); err != nil {
		tmp.Close()
		return err
	}
	if err := tmp.Close(); err != nil {
		return err
	}
	return os.Rename(tmp.Name(), dst)
}

// untilNextMorning is the sleep used when there's nothing happening today: wake
// just after the next local midnight, when the new day's schedule exists.
func untilNextMorning(now time.Time) time.Duration {
	next := time.Date(now.Year(), now.Month(), now.Day(), 0, 10, 0, 0, now.Location()).
		Add(24 * time.Hour)
	if d := next.Sub(now); d > time.Minute {
		return d
	}
	return time.Minute
}
