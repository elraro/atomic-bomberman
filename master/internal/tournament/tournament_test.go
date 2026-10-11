package tournament

import (
	"fmt"
	"strings"
	"testing"
)

func key(n int) string { return strings.Repeat(fmt.Sprintf("%02x", n), 32) }

func sample(players int) *File {
	f := &File{Name: "Autumn Cup", Organiser: "Tuesday League", Servers: []Server{{Name: "main", Address: "play.example.org:27410", IdentityKey: key(200), SigningKey: key(201)}}}
	for i := 0; i < players; i++ {
		f.Players = append(f.Players, Player{Name: fmt.Sprintf("P%d", i+1), Key: key(i + 1)})
	}
	f.Bracket()
	return f
}

func TestBracket(t *testing.T) {
	// Four players: two matches, then the final between their winners.
	f := sample(4)
	if err := f.Validate(); err != nil {
		t.Fatal(err)
	}
	if len(f.Matches) != 3 || f.Matches[0].ID != "R1-1" || f.Matches[2].ID != "R2-1" || f.Matches[2].Round != 2 {
		t.Fatalf("matches: %+v", f.Matches)
	}
	if got := f.Matches[2].Players; got[0] != "@R1-1" || got[1] != "@R1-2" {
		t.Fatalf("final: %v", got)
	}
	if f.Matches[0].Server != "main" {
		t.Fatalf("server: %q", f.Matches[0].Server)
	}
	// Any number: one match fewer than players, and everybody is somewhere in the first two rounds.
	for n := 2; n <= 17; n++ {
		f := sample(n)
		if err := f.Validate(); err != nil {
			t.Fatalf("%d players: %v", n, err)
		}
		if len(f.Matches) != n-1 {
			t.Fatalf("%d players: %d matches", n, len(f.Matches))
		}
		seen := map[string]int{}
		for _, m := range f.Matches {
			for _, p := range m.Players {
				if !strings.HasPrefix(p, "@") {
					seen[p]++
				}
			}
		}
		for i := 1; i <= n; i++ {
			if seen[fmt.Sprintf("P%d", i)] != 1 {
				t.Fatalf("%d players: P%d plays in %d first matches", n, i, seen[fmt.Sprintf("P%d", i)])
			}
		}
	}
}

func TestResults(t *testing.T) {
	f := sample(5) // R1-1: P1-P2, R1-2: P3-P4, P5 through; R2-1: @R1-1 - @R1-2; R3-1: @R2-1 - P5
	if err := f.SetResult("R2-1", "P1", nil, nil); err == nil || !strings.Contains(err.Error(), "has not been decided") {
		t.Fatalf("a match whose players are not known yet took a result: %v", err)
	}
	if err := f.SetResult("R1-1", "P3", nil, nil); err == nil {
		t.Fatal("a winner who did not play was taken")
	}
	if err := f.SetResult("nope", "P1", nil, nil); err == nil {
		t.Fatal("a result for no match was taken")
	}
	if err := f.SetResult("R1-1", "P2", [][]byte{[]byte("record one"), []byte("record two")}, []string{"https://example.org/1.abrec"}); err != nil {
		t.Fatal(err)
	}
	m := f.Matches[0]
	if m.Winner != "P2" || len(m.Records) != 2 || len(m.Records[0].SHA256) != 64 || m.Records[0].URL == "" || m.Records[1].URL != "" || m.Records[0].SHA256 == m.Records[1].SHA256 {
		t.Fatalf("result: %+v", m)
	}
	// The winner takes the place that waited for them.
	if got := f.Matches[2].Players; got[0] != "P2" || got[1] != "@R1-2" {
		t.Fatalf("next round: %v", got)
	}
	_ = f.SetResult("R1-2", "P3", nil, nil)
	if err := f.SetResult("R2-1", "P3", nil, nil); err != nil {
		t.Fatal(err)
	}
	if got := f.Matches[3].Players; got[0] != "P3" || got[1] != "P5" {
		t.Fatalf("final: %v", got)
	}
	if err := f.Validate(); err != nil {
		t.Fatal(err)
	}
}

func TestValidate(t *testing.T) {
	cases := map[string]func(*File){
		"no name":             func(f *File) { f.Name = "" },
		"two players alike":   func(f *File) { f.Players[1].Name = f.Players[0].Name },
		"one key twice":       func(f *File) { f.Players[1].Key = f.Players[0].Key },
		"a key too short":     func(f *File) { f.Players[0].Key = "abcd" },
		"a name like a match": func(f *File) { f.Players[0].Name = "@R1-1" },
		"server without keys": func(f *File) { f.Servers[0].SigningKey = "" },
		"unknown player":      func(f *File) { f.Matches[0].Players[0] = "Nobody" },
		"unknown match":       func(f *File) { f.Matches[2].Players[0] = "@R9-9" },
		"waits for itself":    func(f *File) { f.Matches[2].Players[0] = "@R2-1" },
		"unknown server":      func(f *File) { f.Matches[0].Server = "elsewhere" },
		"winner from outside": func(f *File) { f.Matches[0].Winner = "P3" },
		"one player":          func(f *File) { f.Matches[0].Players = f.Matches[0].Players[:1] },
		"two ids alike":       func(f *File) { f.Matches[1].ID = f.Matches[0].ID },
		"record without hash": func(f *File) { f.Matches[0].Records = []RecordRef{{SHA256: "xyz"}} },
	}
	for name, breakIt := range cases {
		f := sample(4)
		breakIt(f)
		if err := f.Validate(); err == nil {
			t.Errorf("%s: accepted", name)
		}
	}
}
