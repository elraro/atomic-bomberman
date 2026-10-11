package signed

import (
	"bytes"
	"crypto/ed25519"
	"crypto/rand"
	"encoding/json"
	"errors"
	"testing"
)

func TestSignVerify(t *testing.T) {
	public, private, _ := ed25519.GenerateKey(rand.Reader)
	other, _, _ := ed25519.GenerateKey(rand.Reader)
	document := []byte(`{"serial": 7, "note": "exact bytes  matter"}`)
	envelope, err := Sign(private, document)
	if err != nil {
		t.Fatal(err)
	}
	got, key, err := Verify(envelope, []ed25519.PublicKey{other, public})
	if err != nil || !bytes.Equal(got, document) || !bytes.Equal(key, public) {
		t.Fatalf("valid document refused: %v", err)
	}
	if _, _, err := Verify(envelope, []ed25519.PublicKey{other}); !errors.Is(err, ErrUntrusted) {
		t.Fatalf("untrusted key: got %v", err)
	}
	// One changed character of the document.
	var env Envelope
	_ = json.Unmarshal(envelope, &env)
	altered, _ := Sign(private, []byte(`{"serial": 8, "note": "exact bytes  matter"}`))
	var alteredEnv Envelope
	_ = json.Unmarshal(altered, &alteredEnv)
	env.Document = alteredEnv.Document
	forged, _ := json.Marshal(env)
	if _, _, err := Verify(forged, []ed25519.PublicKey{public}); !errors.Is(err, ErrSignature) {
		t.Fatalf("altered document: got %v", err)
	}
	// Somebody else's signature under the trusted key's name.
	_, stranger, _ := ed25519.GenerateKey(rand.Reader)
	theirs, _ := Sign(stranger, document)
	var theirEnv Envelope
	_ = json.Unmarshal(theirs, &theirEnv)
	_ = json.Unmarshal(envelope, &env)
	env.Signature = theirEnv.Signature
	forged, _ = json.Marshal(env)
	if _, _, err := Verify(forged, []ed25519.PublicKey{public}); !errors.Is(err, ErrSignature) {
		t.Fatalf("foreign signature: got %v", err)
	}
	for _, junk := range []string{``, `{}`, `[1]`, `{"document":"!!","signature":"","key":""}`} {
		if _, _, err := Verify([]byte(junk), []ed25519.PublicKey{public}); !errors.Is(err, ErrMalformed) {
			t.Fatalf("junk %q: got %v", junk, err)
		}
	}
}
