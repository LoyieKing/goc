// P22 harness: prove goc_uptr_from_sptr / as_sptr read live g->stack.hi via TLS.
// CGO_ENABLED=0 — freestanding C + TLS .S packed as goc_uptr_amd64.syso.
package main

import (
	"fmt"
	"os"
	"runtime/debug"
	"unsafe"
)

func GetSP() uintptr
func GocStackHiTLS() uintptr
func GocStackLoTLS() uintptr
func GocRuntimeGetg() uintptr
func GocStackHiGo() uintptr
func GocUptrFromSptr(p uintptr) uintptr
func GocUptrAsSptr(u uintptr) uintptr
func GocUptrAsSptrHi(u, hi uintptr) uintptr
func GocUptrFromSptrHi(p, hi uintptr) uintptr

const uptrMSB = uintptr(1) << 63

func pass(msg string) { fmt.Println("PASS", msg) }
func fail(msg string) {
	fmt.Println("FAIL", msg)
	os.Exit(1)
}

//go:noinline
func hugeFrame(n int) int {
	var buf [8192]byte
	buf[n%len(buf)] = byte(n)
	return int(buf[0]) + n
}

func grow(n int, sps *[]uintptr) int {
	*sps = append(*sps, GetSP())
	v := hugeFrame(n)
	if n > 0 {
		v += grow(n-1, sps)
	}
	return v
}

//go:noinline
func encodeGrowDecode() {
	var x int = 0xC0FFEE
	abs0 := uintptr(unsafe.Pointer(&x))
	hi0 := GocStackHiTLS()
	lo := GocStackLoTLS()
	if hi0 == 0 || lo == 0 {
		fail(fmt.Sprintf("tls-hi-nonzero hi=%#x lo=%#x", hi0, lo))
	}
	if abs0 < lo || abs0 >= hi0 {
		fail(fmt.Sprintf("local-in-stack-bounds abs=%#x lo=%#x hi=%#x", abs0, lo, hi0))
	}

	enc := GocUptrFromSptr(abs0)
	if enc&uptrMSB == 0 {
		fail("live-encode-MSB-set")
	}
	if GocUptrAsSptr(enc) != abs0 {
		fail("live-roundtrip-before-grow")
	}
	enc2 := GocUptrFromSptrHi(abs0, hi0)
	if enc2 != enc {
		fail(fmt.Sprintf("live-enc-eq-explicit-hi live=%#x hiAPI=%#x", enc, enc2))
	}
	pass("live-tls-from_sptr-MSB")
	pass("live-tls-as_sptr-roundtrip")
	pass("live-enc-eq-explicit-hi")
	fmt.Printf("P22 encode: hi=%#x abs=%#x enc=%#x\n", hi0, abs0, enc)

	heapEnc := enc

	var sps []uintptr
	_ = grow(40, &sps)

	hi1 := GocStackHiTLS()
	abs1 := uintptr(unsafe.Pointer(&x))
	decoded := GocUptrAsSptr(heapEnc)

	fmt.Printf("P22 growth: hi0=%#x hi1=%#x dHi=%d abs0=%#x abs1=%#x dAbs=%d decoded=%#x sps=%d\n",
		hi0, hi1, int64(hi1-hi0), abs0, abs1, int64(abs1-abs0), decoded, len(sps))

	if x != 0xC0FFEE {
		fail("local-value-after-grow")
	}

	if hi1 != hi0 {
		if abs1-abs0 != hi1-hi0 {
			fail(fmt.Sprintf("growth-delta-mismatch dAbs=%d dHi=%d", abs1-abs0, hi1-hi0))
		}
		if decoded != abs1 {
			fail(fmt.Sprintf("growth-decode got=%#x want abs1=%#x", decoded, abs1))
		}
		pass("stack-grew-same-enc-new-hi")
	} else {
		const delta = uintptr(0x10000)
		hiSim := hi0 + delta
		absSim := abs0 + delta
		got := GocUptrAsSptrHi(heapEnc, hiSim)
		if got != absSim {
			fail(fmt.Sprintf("sim-move got=%#x want=%#x", got, absSim))
		}
		if decoded != abs1 {
			fail(fmt.Sprintf("stable-hi-decode got=%#x abs1=%#x", decoded, abs1))
		}
		pass("sim-stack-move-same-enc-new-hi")
		pass("live-hi-stable-after-grow-attempt")
	}
	_ = x
}

func main() {
	debug.SetMaxStack(64 << 20)

	hiGo := GocStackHiGo()
	hiC := GocStackHiTLS()
	g := GocRuntimeGetg()
	if g == 0 {
		fail("tls-getg-nonzero")
	}
	if hiGo != hiC {
		fail(fmt.Sprintf("tls-hi-agree go=%#x c=%#x", hiGo, hiC))
	}
	pass("tls-hi-read")
	pass("tls-hi-agree-go-asm")

	lo := GocStackLoTLS()
	var probe int
	sp := uintptr(unsafe.Pointer(&probe))
	if !(sp >= lo && sp < hiC) {
		fail(fmt.Sprintf("probe-in-bounds sp=%#x lo=%#x hi=%#x", sp, lo, hiC))
	}
	pass("tls-local-below-hi")

	encodeGrowDecode()

	fmt.Println("PASS p22-uptr-tls (production g->stack.hi TLS)")
}
