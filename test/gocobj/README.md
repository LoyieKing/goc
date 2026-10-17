# gocobj golden files

`v1.0/` holds one valid `.gocobj` and one file per `goc tool objdump verify`
failure (`bad-*`). `gocobj/gocobj_test.go` checks that the valid file passes,
that each broken one fails with its expected message, that re-encoding the
parsed tables gives the same bytes, and that the reader accepts a newer minor
and rejects another major.

Regenerate (needs `llvm-mc-19`): `go run ./test/gocobj/gen`. The output is
deterministic. A new format version gets a new directory; old ones stay.
