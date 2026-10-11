// Package store holds what the master server must remember: guest sessions,
// lobbies and the connection details passed between a host and those joining.
// All of it is short-lived. Two implementations: memory (a single master, and
// the tests) and PostgreSQL (survives a restart, can be shared by several).
package store

import (
	"context"
	"errors"
	"time"
)

// ErrNotFound is returned when the session or lobby asked for does not exist (any more).
var ErrNotFound = errors.New("not found")

// ErrCodeTaken is returned by CreateLobby when another lobby has the join code.
var ErrCodeTaken = errors.New("join code in use")

// Session is a guest session. The token itself is never stored, only its hash.
type Session struct {
	ID        string    `json:"id"`
	TokenHash string    `json:"-"`
	Name      string    `json:"name"`
	Version   int       `json:"version"`
	Created   time.Time `json:"created"`
	Seen      time.Time `json:"seen"`
}

// Lobby is a game waiting for players, as its host described it.
type Lobby struct {
	ID            string    `json:"id"`
	Code          string    `json:"code"`
	HostTokenHash string    `json:"-"`
	HostSession   string    `json:"-"`
	Name          string    `json:"name"`
	Public        bool      `json:"public"`
	Version       int       `json:"version"`
	Players       int       `json:"players"`
	Seats         int       `json:"seats"`
	Phase         string    `json:"phase"`
	Level         string    `json:"level"`
	Region        string    `json:"region"`
	IdentityKey   string    `json:"identity_key"`
	Certificate   string    `json:"certificate,omitempty"`
	Candidates    []string  `json:"-"`
	Created       time.Time `json:"created"`
	Seen          time.Time `json:"-"`
}

// Signal is one message of connection details on its way to "host" or to a session id.
type Signal struct {
	From string `json:"from"`
	Data string `json:"data"`
}

// Store is safe for use from many goroutines.
type Store interface {
	CreateSession(ctx context.Context, s Session) error
	SessionByTokenHash(ctx context.Context, hash string, now time.Time) (Session, error) // also marks it seen

	CreateLobby(ctx context.Context, l Lobby) error
	Lobby(ctx context.Context, id string) (Lobby, error)
	LobbyByCode(ctx context.Context, code string) (Lobby, error)
	ListPublic(ctx context.Context, offset, limit int) ([]Lobby, int, error) // newest first; and how many there are
	UpdateLobby(ctx context.Context, l Lobby) error                          // by ID; ErrNotFound if gone
	DeleteLobby(ctx context.Context, id string) error

	PushSignal(ctx context.Context, lobbyID, to string, s Signal) error
	PopSignals(ctx context.Context, lobbyID, to string) ([]Signal, error)

	// Expire removes lobbies not heard from since lobbyBefore (with their signals) and
	// sessions not seen since sessionBefore.
	Expire(ctx context.Context, lobbyBefore, sessionBefore time.Time) error
	Close()
}
