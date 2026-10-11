package store

import (
	"context"
	"sort"
	"sync"
	"time"
)

type memory struct {
	mu       sync.Mutex
	sessions map[string]Session // by token hash
	lobbies  map[string]Lobby   // by id
	signals  map[string][]Signal
}

// NewMemory returns a store that lives and dies with the process.
func NewMemory() Store {
	return &memory{sessions: map[string]Session{}, lobbies: map[string]Lobby{}, signals: map[string][]Signal{}}
}

func (m *memory) Close() {}

func (m *memory) CreateSession(_ context.Context, s Session) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	m.sessions[s.TokenHash] = s
	return nil
}

func (m *memory) SessionByTokenHash(_ context.Context, hash string, now time.Time) (Session, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	s, ok := m.sessions[hash]
	if !ok {
		return Session{}, ErrNotFound
	}
	s.Seen = now
	m.sessions[hash] = s
	return s, nil
}

func (m *memory) CreateLobby(_ context.Context, l Lobby) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	for _, other := range m.lobbies {
		if other.Code == l.Code {
			return ErrCodeTaken
		}
	}
	m.lobbies[l.ID] = l
	return nil
}

func (m *memory) Lobby(_ context.Context, id string) (Lobby, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	l, ok := m.lobbies[id]
	if !ok {
		return Lobby{}, ErrNotFound
	}
	return l, nil
}

func (m *memory) LobbyByCode(_ context.Context, code string) (Lobby, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	for _, l := range m.lobbies {
		if l.Code == code {
			return l, nil
		}
	}
	return Lobby{}, ErrNotFound
}

func (m *memory) ListPublic(_ context.Context, offset, limit int) ([]Lobby, int, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	var all []Lobby
	for _, l := range m.lobbies {
		if l.Public {
			all = append(all, l)
		}
	}
	sort.Slice(all, func(i, j int) bool {
		if !all[i].Created.Equal(all[j].Created) {
			return all[i].Created.After(all[j].Created)
		}
		return all[i].ID < all[j].ID
	})
	total := len(all)
	if offset > total {
		offset = total
	}
	all = all[offset:]
	if len(all) > limit {
		all = all[:limit]
	}
	return all, total, nil
}

func (m *memory) UpdateLobby(_ context.Context, l Lobby) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	if _, ok := m.lobbies[l.ID]; !ok {
		return ErrNotFound
	}
	m.lobbies[l.ID] = l
	return nil
}

func (m *memory) DeleteLobby(_ context.Context, id string) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	if _, ok := m.lobbies[id]; !ok {
		return ErrNotFound
	}
	m.drop(id)
	return nil
}

func (m *memory) drop(id string) {
	delete(m.lobbies, id)
	for key := range m.signals {
		if len(key) > len(id) && key[:len(id)+1] == id+"/" {
			delete(m.signals, key)
		}
	}
}

func (m *memory) PushSignal(_ context.Context, lobbyID, to string, s Signal) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	if _, ok := m.lobbies[lobbyID]; !ok {
		return ErrNotFound
	}
	key := lobbyID + "/" + to
	if len(m.signals[key]) >= MaxSignalsWaiting {
		m.signals[key] = m.signals[key][1:]
	}
	m.signals[key] = append(m.signals[key], s)
	return nil
}

func (m *memory) PopSignals(_ context.Context, lobbyID, to string) ([]Signal, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	if _, ok := m.lobbies[lobbyID]; !ok {
		return nil, ErrNotFound
	}
	key := lobbyID + "/" + to
	out := m.signals[key]
	delete(m.signals, key)
	return out, nil
}

func (m *memory) Expire(_ context.Context, lobbyBefore, sessionBefore time.Time) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	for id, l := range m.lobbies {
		if l.Seen.Before(lobbyBefore) {
			m.drop(id)
		}
	}
	for hash, s := range m.sessions {
		if s.Seen.Before(sessionBefore) {
			delete(m.sessions, hash)
		}
	}
	return nil
}

// MaxSignalsWaiting is how many messages may wait for one receiver; the oldest goes first.
const MaxSignalsWaiting = 64
