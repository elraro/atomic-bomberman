package store

import (
	"context"
	"errors"
	"os"
	"testing"
	"time"
)

// Every store is put through the same paces. PostgreSQL only where a database is given
// (MASTER_TEST_DATABASE_URL), as the CI job does.
func TestMemory(t *testing.T) { exercise(t, NewMemory()) }

func TestPostgres(t *testing.T) {
	url := os.Getenv("MASTER_TEST_DATABASE_URL")
	if url == "" {
		t.Skip("MASTER_TEST_DATABASE_URL is not set")
	}
	st, err := NewPostgres(context.Background(), url)
	if err != nil {
		t.Fatal(err)
	}
	p := st.(*postgres)
	if _, err := p.pool.Exec(context.Background(), `TRUNCATE sessions, lobbies, signals`); err != nil {
		t.Fatal(err)
	}
	exercise(t, st)
}

func exercise(t *testing.T, st Store) {
	t.Helper()
	defer st.Close()
	ctx := context.Background()
	t0 := time.Date(2026, 10, 11, 12, 0, 0, 0, time.UTC)

	// Sessions.
	if err := st.CreateSession(ctx, Session{ID: "s1", TokenHash: "h1", Name: "Ann", Version: 8, Created: t0, Seen: t0}); err != nil {
		t.Fatal(err)
	}
	if _, err := st.SessionByTokenHash(ctx, "nope", t0); !errors.Is(err, ErrNotFound) {
		t.Fatalf("unknown session: %v", err)
	}
	s, err := st.SessionByTokenHash(ctx, "h1", t0.Add(time.Hour))
	if err != nil || s.ID != "s1" || s.Name != "Ann" || s.Version != 8 || !s.Seen.Equal(t0.Add(time.Hour)) {
		t.Fatalf("session: %+v %v", s, err)
	}

	// Lobbies.
	mk := func(id, code string, public bool, at time.Time) Lobby {
		return Lobby{ID: id, Code: code, HostTokenHash: "hh" + id, HostSession: "s1", Name: "Game " + id, Public: public, Version: 8, Players: 1, Seats: 10,
			Phase: "lobby", Level: "Green Acres", Region: "eu", IdentityKey: "ab", Certificate: "", Candidates: []string{"1.2.3.4:27410", "x"}, Created: at, Seen: at}
	}
	for i, l := range []Lobby{mk("a", "111111", true, t0), mk("b", "222222", false, t0.Add(time.Second)), mk("c", "333333", true, t0.Add(2*time.Second))} {
		if err := st.CreateLobby(ctx, l); err != nil {
			t.Fatalf("create %d: %v", i, err)
		}
	}
	if err := st.CreateLobby(ctx, mk("d", "222222", true, t0)); !errors.Is(err, ErrCodeTaken) {
		t.Fatalf("duplicate code: %v", err)
	}
	l, err := st.Lobby(ctx, "b")
	if err != nil || l.Code != "222222" || l.Public || len(l.Candidates) != 2 || l.Candidates[0] != "1.2.3.4:27410" || l.HostTokenHash != "hhb" {
		t.Fatalf("lobby: %+v %v", l, err)
	}
	if l, err := st.LobbyByCode(ctx, "333333"); err != nil || l.ID != "c" {
		t.Fatalf("by code: %+v %v", l, err)
	}
	if _, err := st.LobbyByCode(ctx, "999999"); !errors.Is(err, ErrNotFound) {
		t.Fatalf("unknown code: %v", err)
	}
	list, total, err := st.ListPublic(ctx, 0, 10)
	if err != nil || total != 2 || len(list) != 2 || list[0].ID != "c" || list[1].ID != "a" {
		t.Fatalf("list: %+v %d %v", list, total, err)
	}
	if list, total, _ := st.ListPublic(ctx, 1, 1); total != 2 || len(list) != 1 || list[0].ID != "a" {
		t.Fatalf("second page: %+v", list)
	}
	if list, _, _ := st.ListPublic(ctx, 5, 10); len(list) != 0 {
		t.Fatalf("beyond the end: %+v", list)
	}
	l.Players, l.Phase, l.Seen, l.Candidates = 4, "round", t0.Add(40*time.Second), nil
	if err := st.UpdateLobby(ctx, l); err != nil {
		t.Fatal(err)
	}
	if got, _ := st.Lobby(ctx, "b"); got.Players != 4 || got.Phase != "round" || len(got.Candidates) != 0 {
		t.Fatalf("after update: %+v", got)
	}
	if err := st.UpdateLobby(ctx, mk("zz", "000000", true, t0)); !errors.Is(err, ErrNotFound) {
		t.Fatalf("update of nothing: %v", err)
	}

	// Signals: in order, once, each receiver its own; no more than the limit wait.
	for _, d := range []string{"one", "two"} {
		if err := st.PushSignal(ctx, "a", "host", Signal{From: "s1", Data: d}); err != nil {
			t.Fatal(err)
		}
	}
	_ = st.PushSignal(ctx, "a", "s9", Signal{From: "host", Data: "for s9"})
	if err := st.PushSignal(ctx, "gone", "host", Signal{}); !errors.Is(err, ErrNotFound) {
		t.Fatalf("signal to no lobby: %v", err)
	}
	got, err := st.PopSignals(ctx, "a", "host")
	if err != nil || len(got) != 2 || got[0].Data != "one" || got[1].Data != "two" || got[0].From != "s1" {
		t.Fatalf("signals: %+v %v", got, err)
	}
	if got, _ := st.PopSignals(ctx, "a", "host"); len(got) != 0 {
		t.Fatalf("signals twice: %+v", got)
	}
	if got, _ := st.PopSignals(ctx, "a", "s9"); len(got) != 1 || got[0].Data != "for s9" {
		t.Fatalf("other receiver: %+v", got)
	}
	if _, err := st.PopSignals(ctx, "gone", "host"); !errors.Is(err, ErrNotFound) {
		t.Fatalf("signals of no lobby: %v", err)
	}
	for i := 0; i < MaxSignalsWaiting+5; i++ {
		_ = st.PushSignal(ctx, "c", "host", Signal{From: "s", Data: string(rune('A' + i%26))})
	}
	if got, _ := st.PopSignals(ctx, "c", "host"); len(got) != MaxSignalsWaiting || got[0].Data != "F" {
		t.Fatalf("limit: %d first %q", len(got), got[0].Data)
	}

	// Expiry: lobby "a" and "c" were last heard at t0 and t0+2s, "b" at t0+40s.
	_ = st.PushSignal(ctx, "a", "host", Signal{From: "s1", Data: "left over"})
	if err := st.Expire(ctx, t0.Add(30*time.Second), t0.Add(30*time.Minute)); err != nil {
		t.Fatal(err)
	}
	if _, err := st.Lobby(ctx, "a"); !errors.Is(err, ErrNotFound) {
		t.Fatalf("stale lobby still there: %v", err)
	}
	if _, err := st.Lobby(ctx, "b"); err != nil {
		t.Fatalf("live lobby gone: %v", err)
	}
	if _, err := st.SessionByTokenHash(ctx, "h1", t0.Add(2*time.Hour)); err != nil {
		t.Fatalf("live session gone: %v", err)
	}
	_ = st.Expire(ctx, t0, t0.Add(3*time.Hour))
	if _, err := st.SessionByTokenHash(ctx, "h1", t0.Add(4*time.Hour)); !errors.Is(err, ErrNotFound) {
		t.Fatalf("stale session still there: %v", err)
	}
	if err := st.DeleteLobby(ctx, "b"); err != nil {
		t.Fatal(err)
	}
	if err := st.DeleteLobby(ctx, "b"); !errors.Is(err, ErrNotFound) {
		t.Fatalf("delete twice: %v", err)
	}
}
