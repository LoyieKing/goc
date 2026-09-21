// Copyright 2024 The Go Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

//go:build arm64

package sys

import (
	"goc.local/p5-machinepass-goobj/goobj/enc/stdinternal/cpu"
)

var DITSupported = cpu.ARM64.HasDIT

func EnableDIT() bool
func DITEnabled() bool
func DisableDIT()
