// Package tournament is the organiser's side of tournaments as signed files
// (docs/specifications/online-play.md, section 7): the file's shape, a bracket made
// from a list of players, and results written into it. The game reads the same
// file (src/online/tournament.*).
package tournament

import (
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"strings"
)

type Rules struct {
	GameVersion string `json:"game_version"`
	Protocol    int    `json:"protocol"`
	Wins        int    `json:"wins"`
	Level       int    `json:"level"`
	Scheme      string `json:"scheme"`
	PlayTime    int    `json:"play_time"`
	Enclosement int    `json:"enclosement"`
	TeamPlay    bool   `json:"team_play"`
	Computers   int    `json:"computers"`
}

type Player struct {
	Name string `json:"name"`
	Key  string `json:"key"`
}

type Server struct {
	Name        string `json:"name"`
	Address     string `json:"address"`
	IdentityKey string `json:"identity_key"`
	SigningKey  string `json:"signing_key"`
}

type RecordRef struct {
	SHA256 string `json:"sha256"`
	URL    string `json:"url"`
}

type Match struct {
	ID      string      `json:"id"`
	Round   int         `json:"round"`
	Players []string    `json:"players"` // names, or "@ID": the winner of that match
	Server  string      `json:"server"`
	Time    int64       `json:"time"`
	Winner  string      `json:"winner"`
	Records []RecordRef `json:"records"`
}

type File struct {
	Kind      string   `json:"kind"`
	Name      string   `json:"name"`
	Organiser string   `json:"organiser"`
	Serial    int64    `json:"serial"`
	Issued    int64    `json:"issued"`
	Rules     Rules    `json:"rules"`
	Players   []Player `json:"players"`
	Servers   []Server `json:"servers"`
	Matches   []Match  `json:"matches"`
}

func isKey(s string) bool {
	raw, err := hex.DecodeString(s)
	return err == nil && len(raw) == 32
}

// Validate says what the game would refuse the file for.
func (f *File) Validate() error {
	if f.Name == "" {
		return errors.New("the tournament has no name")
	}
	names := map[string]bool{}
	keys := map[string]bool{}
	for _, p := range f.Players {
		if p.Name == "" || strings.HasPrefix(p.Name, "@") || names[p.Name] {
			return fmt.Errorf("player %q: every player needs a name of their own (not starting with @)", p.Name)
		}
		if !isKey(p.Key) || keys[strings.ToLower(p.Key)] {
			return fmt.Errorf("player %q: needs a key of 64 hexadecimal digits that nobody else has", p.Name)
		}
		names[p.Name] = true
		keys[strings.ToLower(p.Key)] = true
	}
	servers := map[string]bool{}
	for _, s := range f.Servers {
		if s.Name == "" || s.Address == "" || servers[s.Name] || !isKey(s.IdentityKey) || !isKey(s.SigningKey) {
			return fmt.Errorf("server %q: needs a name of its own, an address and its two keys", s.Name)
		}
		servers[s.Name] = true
	}
	ids := map[string]bool{}
	for _, m := range f.Matches {
		if m.ID == "" || strings.HasPrefix(m.ID, "@") || ids[m.ID] {
			return fmt.Errorf("match %q: every match needs an id of its own", m.ID)
		}
		ids[m.ID] = true
	}
	for _, m := range f.Matches {
		if len(m.Players) < 2 || len(m.Players) > 10 {
			return fmt.Errorf("match %s: needs two to ten players", m.ID)
		}
		for _, p := range m.Players {
			if later, ok := strings.CutPrefix(p, "@"); ok {
				if !ids[later] || later == m.ID {
					return fmt.Errorf("match %s: %q is not another match", m.ID, p)
				}
			} else if !names[p] {
				return fmt.Errorf("match %s: %q is not a player of the tournament", m.ID, p)
			}
		}
		if m.Server != "" && !servers[m.Server] {
			return fmt.Errorf("match %s: %q is not a server of the tournament", m.ID, m.Server)
		}
		if m.Winner != "" && !contains(m.Players, m.Winner) {
			return fmt.Errorf("match %s: the winner %q did not play in it", m.ID, m.Winner)
		}
		for _, r := range m.Records {
			if !isKey(r.SHA256) {
				return fmt.Errorf("match %s: a record without its hash", m.ID)
			}
		}
	}
	return nil
}

func contains(list []string, s string) bool {
	for _, x := range list {
		if x == s {
			return true
		}
	}
	return false
}

// Bracket fills in the matches of a knock-out for the players in the order given: the
// first plays the second, the third the fourth, and so on; whoever is left over when the
// number is odd goes through to the next round without playing. Every match is put on the
// first server; the organiser moves them and sets the times.
func (f *File) Bracket() {
	f.Matches = nil
	server := ""
	if len(f.Servers) > 0 {
		server = f.Servers[0].Name
	}
	var entrants []string
	for _, p := range f.Players {
		entrants = append(entrants, p.Name)
	}
	for round := 1; len(entrants) > 1; round++ {
		var next []string
		for i := 0; i+1 < len(entrants); i += 2 {
			id := fmt.Sprintf("R%d-%d", round, i/2+1)
			f.Matches = append(f.Matches, Match{ID: id, Round: round, Players: []string{entrants[i], entrants[i+1]}, Server: server, Records: []RecordRef{}})
			next = append(next, "@"+id)
		}
		if len(entrants)%2 == 1 {
			next = append(next, entrants[len(entrants)-1])
		}
		entrants = next
	}
}

// SetResult writes the winner of a match and the hashes of its records into the file, and
// puts the winner's name wherever a later match waits for "the winner of" this one.
func (f *File) SetResult(matchID, winner string, records [][]byte, urls []string) error {
	var match *Match
	for i := range f.Matches {
		if f.Matches[i].ID == matchID {
			match = &f.Matches[i]
		}
	}
	if match == nil {
		return fmt.Errorf("no match %q", matchID)
	}
	for _, p := range match.Players {
		if strings.HasPrefix(p, "@") {
			return fmt.Errorf("match %s cannot have a result yet: %s has not been decided", matchID, p[1:])
		}
	}
	if !contains(match.Players, winner) {
		return fmt.Errorf("%q did not play in match %s", winner, matchID)
	}
	match.Winner = winner
	match.Records = []RecordRef{}
	for i, r := range records {
		sum := sha256.Sum256(r)
		ref := RecordRef{SHA256: hex.EncodeToString(sum[:])}
		if i < len(urls) {
			ref.URL = urls[i]
		}
		match.Records = append(match.Records, ref)
	}
	for i := range f.Matches {
		for j, p := range f.Matches[i].Players {
			if p == "@"+matchID {
				f.Matches[i].Players[j] = winner
			}
		}
	}
	return nil
}
