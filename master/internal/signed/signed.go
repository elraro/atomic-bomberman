// Package signed is the form every signed document of the project takes (the
// configuration; later server certificates and tournament files): the document's
// exact bytes, an Ed25519 signature over them, and the key that made it, in one
// small JSON envelope. Whoever reads one decides which keys to believe.
package signed

import (
	"bytes"
	"crypto/ed25519"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"errors"
)

// Envelope is what is stored and fetched.
type Envelope struct {
	Document  string `json:"document"`  // base64 of the document's bytes
	Signature string `json:"signature"` // base64 of the Ed25519 signature over those bytes
	Key       string `json:"key"`       // hex of the public key that signed
}

var (
	ErrMalformed = errors.New("not a signed document")
	ErrUntrusted = errors.New("signed by a key that is not trusted")
	ErrSignature = errors.New("the signature does not match the document")
)

// Sign wraps the document.
func Sign(key ed25519.PrivateKey, document []byte) ([]byte, error) {
	env := Envelope{
		Document:  base64.StdEncoding.EncodeToString(document),
		Signature: base64.StdEncoding.EncodeToString(ed25519.Sign(key, document)),
		Key:       hex.EncodeToString(key.Public().(ed25519.PublicKey)),
	}
	return json.MarshalIndent(env, "", "  ")
}

// Verify returns the document if one of the trusted keys signed it.
func Verify(envelope []byte, trusted []ed25519.PublicKey) ([]byte, ed25519.PublicKey, error) {
	var env Envelope
	if err := json.Unmarshal(envelope, &env); err != nil {
		return nil, nil, ErrMalformed
	}
	document, err1 := base64.StdEncoding.DecodeString(env.Document)
	signature, err2 := base64.StdEncoding.DecodeString(env.Signature)
	key, err3 := hex.DecodeString(env.Key)
	if err1 != nil || err2 != nil || err3 != nil || len(key) != ed25519.PublicKeySize || len(signature) != ed25519.SignatureSize {
		return nil, nil, ErrMalformed
	}
	for _, t := range trusted {
		if bytes.Equal(t, key) {
			if !ed25519.Verify(t, document, signature) {
				return nil, nil, ErrSignature
			}
			return document, t, nil
		}
	}
	return nil, nil, ErrUntrusted
}
