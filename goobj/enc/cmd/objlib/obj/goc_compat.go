// goc addition, not part of the Go tree (tools/goc-goenc): the obj API
// gocobj2goobj uses that differs between Go releases.

package obj

// GocAddRel appends relocation r to s.
func (s *LSym) GocAddRel(ctxt *Link, r Reloc) { s.AddRel(ctxt, r) }

// GocSetAlign sets a data symbol's alignment where LSym has one.
func (s *LSym) GocSetAlign(a int) {}
