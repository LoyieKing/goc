// mir2lower converts LLVM MIR text into goc-mi-lower-1 for mirlower/binwriter.
package main

import (
	"flag"
	"fmt"
	"os"
	"strings"

	"goc.local/p5-machinepass-goobj/goobj/mirparse"
)

func main() {
	in := flag.String("in", "", "input .mir (YAML or MF dump)")
	out := flag.String("out", "", "output mi_lower.txt")
	recipe := flag.String("recipe", "", "optional stackcheck.recipe.txt lines to prepend as metadata")
	fallbackBodies := flag.String("fallback-bodies", "", "if set and -in missing/fails, copy this mi_full_bodies-style file (documented fallback)")
	flag.Parse()
	if *out == "" {
		fmt.Fprintf(os.Stderr, "usage: mir2lower -in FILE.mir -out mi_lower.txt [-recipe recipe.txt]\n")
		os.Exit(2)
	}

	var recipeLines []string
	if *recipe != "" {
		b, err := os.ReadFile(*recipe)
		if err != nil {
			fmt.Fprintf(os.Stderr, "mir2lower: recipe: %v\n", err)
			os.Exit(1)
		}
		for _, L := range strings.Split(string(b), "\n") {
			L = strings.TrimSpace(L)
			if L == "" || strings.HasPrefix(L, "#") {
				continue
			}
			recipeLines = append(recipeLines, L)
		}
	}

	useFallback := false
	var mod *mirparse.Module
	var err error
	if *in == "" {
		useFallback = true
	} else {
		mod, err = mirparse.ParseFile(*in)
		if err != nil {
			fmt.Fprintf(os.Stderr, "mir2lower: MIR parse failed: %v\n", err)
			if *fallbackBodies != "" {
				fmt.Fprintf(os.Stderr, "mir2lower: using documented fallback bodies %s\n", *fallbackBodies)
				useFallback = true
			} else {
				os.Exit(1)
			}
		}
	}

	var text string
	if useFallback {
		if *fallbackBodies == "" {
			fmt.Fprintf(os.Stderr, "mir2lower: FATAL no MIR and no fallback\n")
			os.Exit(1)
		}
		body, err := os.ReadFile(*fallbackBodies)
		if err != nil {
			fmt.Fprintf(os.Stderr, "mir2lower: fallback: %v\n", err)
			os.Exit(1)
		}
		var b strings.Builder
		b.WriteString("# MIR→goobj lower input (FALLBACK mi_full_bodies; prefer pass/harness.mir)\n")
		b.WriteString("format goc-mi-lower-1\n")
		b.WriteString("abi amd64_ABIInternal\n")
		b.WriteString("arch amd64\n")
		for _, L := range recipeLines {
			b.WriteString(L)
			b.WriteByte('\n')
		}
		b.WriteString("\n# ---- full MI bodies (fallback) ----\n")
		b.Write(body)
		text = b.String()
		fmt.Println("mir2lower: wrote FALLBACK mi_lower from", *fallbackBodies)
	} else {
		ml, err := mirparse.ToMiLower(mod, mirparse.DefaultHarnessOptions())
		if err != nil {
			fmt.Fprintf(os.Stderr, "mir2lower: convert: %v\n", err)
			os.Exit(1)
		}
		// Require harness TEXTs
		for _, need := range []string{"goc_checked_add", "goc_hold_live", "goc_store_gptr", "goc_leaf"} {
			if fn, ok := ml.Fns[need]; !ok || len(fn.Ops) == 0 {
				fmt.Fprintf(os.Stderr, "mir2lower: FATAL missing required fn %s after MIR parse\n", need)
				os.Exit(1)
			}
		}
		text = mirparse.WriteMiLower(ml, recipeLines)
		fmt.Printf("mir2lower: parsed %d functions from %s → %s\n", len(mod.Functions), *in, *out)
	}

	if err := os.WriteFile(*out, []byte(text), 0o644); err != nil {
		fmt.Fprintf(os.Stderr, "mir2lower: write: %v\n", err)
		os.Exit(1)
	}
}
