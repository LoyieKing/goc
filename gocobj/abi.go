package gocobj

import "strings"

// Profile holds the Go runtime ABI constants generated code must match.
// It mirrors llvm/include/Goc/AbiProfiles.def; TestProfilesMatchDef keeps
// the two equal.
type Profile struct {
	Name           string
	Arch           string
	PtrSize        int
	GReg           string
	StackGuard0Off int
	StackSmall     int
	StackBig       int
	Morestack      string
	IntRegs        []string
	FPRegs         []string
}

var profiles = []Profile{
	{"go1.24-amd64", "amd64", 8, "R14", 16, 128, 4096, "runtime.morestack_noctxt",
		regs("RAX,RBX,RCX,RDI,RSI,R8,R9,R10,R11"),
		regs("X0,X1,X2,X3,X4,X5,X6,X7,X8,X9,X10,X11,X12,X13,X14")},
	{"go1.24-arm64", "arm64", 8, "R28", 16, 128, 4096, "runtime.morestack_noctxt",
		regs("R0,R1,R2,R3,R4,R5,R6,R7,R8,R9,R10,R11,R12,R13,R14,R15"),
		regs("F0,F1,F2,F3,F4,F5,F6,F7,F8,F9,F10,F11,F12,F13,F14,F15")},
}

func regs(s string) []string { return strings.Split(s, ",") }

// LookupProfile returns the named profile.
func LookupProfile(name string) (Profile, bool) {
	for _, p := range profiles {
		if p.Name == name {
			return p, true
		}
	}
	return Profile{}, false
}

// Profiles returns all known profiles.
func Profiles() []Profile { return append([]Profile(nil), profiles...) }
