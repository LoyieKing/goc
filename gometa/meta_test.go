package gometa

import (
	"bytes"
	"strings"
	"testing"
)

func TestEmbedded(t *testing.T) {
	rs := Releases()
	if len(rs) < 5 {
		t.Fatalf("releases %v", rs)
	}
	for _, v := range rs {
		m, err := ForVersion(v + ".1")
		if err != nil {
			t.Fatal(err)
		}
		if m.Go != v {
			t.Errorf("%s: meta says %s", v, m.Go)
		}
		b, err := m.Encode()
		if err != nil {
			t.Fatal(err)
		}
		m2, err := Parse(b)
		if err != nil {
			t.Fatal(err)
		}
		b2, _ := m2.Encode()
		if !bytes.Equal(b, b2) {
			t.Errorf("%s: encode/parse does not round-trip", v)
		}
	}
}

func TestUnknownVersion(t *testing.T) {
	_, err := ForVersion("go1.99.0")
	if err == nil || !strings.Contains(err.Error(), "golangc-metadata-resolver") {
		t.Errorf("want an error naming the resolver, got %v", err)
	}
}
