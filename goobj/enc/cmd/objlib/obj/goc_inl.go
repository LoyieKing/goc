// goc addition, not part of the Go tree (tools/goc-goenc):
// gocobj2goobj builds a function's inline tree from the .goc.inltree table
// instead of from Progs, so it sets ParentPC directly.

package obj

// SetParentPC sets the ParentPC of node i of a per-function inline tree.
func (tree *InlTree) SetParentPC(i int, pc int32) { tree.setParentPC(i, pc) }
