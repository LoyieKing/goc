package gocobj

import (
	"encoding/binary"
	"errors"
	"fmt"
)

var errShort = errors.New("truncated")

// buf reads one record (or a whole section) field by field.
type buf struct {
	b   []byte
	err error
}

func (r *buf) fail(e error) {
	if r.err == nil {
		r.err = e
	}
}

func (r *buf) u8() uint8 {
	if len(r.b) < 1 {
		r.fail(errShort)
		return 0
	}
	v := r.b[0]
	r.b = r.b[1:]
	return v
}

func (r *buf) uleb() uint64 {
	v, n := binary.Uvarint(r.b)
	if n <= 0 {
		r.fail(errShort)
		r.b = nil
		return 0
	}
	r.b = r.b[n:]
	return v
}

func (r *buf) sleb() int64 {
	var v int64
	var shift uint
	for {
		if len(r.b) == 0 {
			r.fail(errShort)
			return 0
		}
		c := r.b[0]
		r.b = r.b[1:]
		v |= int64(c&0x7f) << shift
		shift += 7
		if c&0x80 == 0 {
			if shift < 64 && c&0x40 != 0 {
				v |= -1 << shift
			}
			return v
		}
		if shift >= 64 {
			r.fail(errors.New("sleb overflow"))
			return 0
		}
	}
}

func (r *buf) bytes(n uint64) []byte {
	if uint64(len(r.b)) < n {
		r.fail(errShort)
		r.b = nil
		return nil
	}
	v := append([]byte(nil), r.b[:n]...)
	r.b = r.b[n:]
	return v
}

// records splits a section into records and reports each record's
// start offset (the "offset" fields of other records point here).
func records(sec string, b []byte, f func(off uint64, r *buf) error) error {
	off := uint64(0)
	for len(b) > 0 {
		r := &buf{b: b}
		n := r.uleb()
		if r.err != nil || uint64(len(r.b)) < n {
			return fmt.Errorf("%s: record at %d: bad length", sec, off)
		}
		hdr := uint64(len(b) - len(r.b))
		rec := &buf{b: r.b[:n]}
		if err := f(off, rec); err != nil {
			return fmt.Errorf("%s: record at %d: %w", sec, off, err)
		}
		if rec.err != nil {
			return fmt.Errorf("%s: record at %d: %w", sec, off, rec.err)
		}
		b = r.b[n:]
		off += hdr + n
	}
	return nil
}

// enc builds one record or section.
type enc struct{ b []byte }

func (e *enc) u8(v uint8)    { e.b = append(e.b, v) }
func (e *enc) uleb(v uint64) { e.b = binary.AppendUvarint(e.b, v) }
func (e *enc) raw(v []byte)  { e.b = append(e.b, v...) }
func (e *enc) sleb(v int64) {
	for {
		c := byte(v & 0x7f)
		v >>= 7
		if (v == 0 && c&0x40 == 0) || (v == -1 && c&0x40 != 0) {
			e.b = append(e.b, c)
			return
		}
		e.b = append(e.b, c|0x80)
	}
}

// record appends body as one length-prefixed record and returns its offset.
func (e *enc) record(body []byte) uint64 {
	off := uint64(len(e.b))
	e.uleb(uint64(len(body)))
	e.raw(body)
	return off
}
