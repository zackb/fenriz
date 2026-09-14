package mlb

import "testing"

// rec builds a standings group; each team is "ABBR:rank".
func rec(kind, division string, teams ...string) apiStandRecord {
	r := apiStandRecord{StandingsType: kind}
	for _, t := range teams {
		abbr, rank := t[:len(t)-2], t[len(t)-1:]
		var tr apiTeamRecord
		tr.Team.Abbreviation = abbr
		tr.Team.Division.Name = division
		tr.DivisionRank = rank
		tr.WildCardRank = rank
		r.TeamRecords = append(r.TeamRecords, tr)
	}
	return r
}

func abbrs(rows []Standing) []string {
	out := make([]string, len(rows))
	for i, r := range rows {
		out[i] = r.Abbr
	}
	return out
}

func equal(a, b []string) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i] != b[i] {
			return false
		}
	}
	return true
}

func TestBuildStandings(t *testing.T) {
	alWest := rec("regularSeason", "American League West", "HOU:1", "TEX:2", "SEA:3", "LAA:4", "ATH:5")
	alEast := rec("regularSeason", "American League East", "TB:1", "NYY:2", "BOS:3", "TOR:4", "BAL:5")
	nlWest := rec("regularSeason", "National League West", "LAD:1", "SDP:2")

	tests := []struct {
		name     string
		recs     []apiStandRecord
		team     string
		division string
		teams    []string
		wildCard []string
	}{
		{
			name:     "team outside the wild-card spots is appended",
			recs:     []apiStandRecord{alEast, alWest, rec("wildCard", "American League East", "NYY:1", "BOS:2", "TEX:3", "TOR:4", "SEA:5")},
			team:     "SEA",
			division: "AL West",
			teams:    []string{"HOU", "TEX", "SEA", "LAA", "ATH"},
			wildCard: []string{"NYY", "BOS", "TEX", "SEA"},
		},
		{
			name:     "team inside the spots is not duplicated",
			recs:     []apiStandRecord{alWest, rec("wildCard", "American League West", "NYY:1", "SEA:2", "TEX:3", "TOR:4")},
			team:     "SEA",
			division: "AL West",
			teams:    []string{"HOU", "TEX", "SEA", "LAA", "ATH"},
			wildCard: []string{"NYY", "SEA", "TEX"},
		},
		{
			name:     "national league division name is shortened",
			recs:     []apiStandRecord{alWest, nlWest},
			team:     "LAD",
			division: "NL West",
			teams:    []string{"LAD", "SDP"},
			wildCard: nil,
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			got := buildStandings(tc.recs, tc.team)
			if got == nil {
				t.Fatal("buildStandings returned nil")
			}
			if got.Division != tc.division {
				t.Errorf("division = %q, want %q", got.Division, tc.division)
			}
			if !equal(abbrs(got.Teams), tc.teams) {
				t.Errorf("teams = %v, want %v", abbrs(got.Teams), tc.teams)
			}
			if !equal(abbrs(got.WildCard), tc.wildCard) {
				t.Errorf("wildCard = %v, want %v", abbrs(got.WildCard), tc.wildCard)
			}
			for _, r := range append(got.Teams, got.WildCard...) {
				if (r.Abbr == tc.team) != r.Me {
					t.Errorf("row %s: Me = %v", r.Abbr, r.Me)
				}
			}
		})
	}

	if got := buildStandings([]apiStandRecord{alWest}, "NYM"); got != nil {
		t.Errorf("unknown team: got %+v, want nil", got)
	}
}
