# gocobj format, version 1.0

A `.gocobj` is what goc's LLVM backend will write for one C file. It is a
normal relocatable ELF file (machine code in `.text.<sym>`, data in
`.data.*`/`.rodata.*`/`.bss.*`, ELF symbols and relocations) plus a few
`.goc.*` sections that carry what Go's linker needs and ELF cannot say:
frame sizes, pointer maps, pc-value tables, and relocations in a Go-neutral
form. `gocobj2goobj` turns it into a Go object file. Nothing in a `.gocobj`
depends on the Go version except the ABI profile name in the header
(see [go-abi-profiles.md](go-abi-profiles.md)).

The Go reader and checker live in `gocobj/`; `goc-objdump dump|verify` prints
and checks a file. Golden files are in `test/gocobj/`.

## Encoding

- Integers are little-endian. `uleb` and `sleb` are LEB128.
- A `strref` is a byte offset into `.goc.strtab`. Strings there end with NUL.
  Offset 0 is the empty string.
- Symbols are named, not numbered: a record refers to an ELF symbol by the
  `strref` of its name. (MC assigns ELF symbol indices only when it writes
  the file, after the tables are built.) Names a record refers to must be
  unique in the ELF symbol table.
- Every section other than `.goc.header` and `.goc.strtab` is a list of
  records. Each record is `uleb length` followed by `length` bytes. A reader
  takes the fields it knows and skips the rest of the record, so a later
  minor version can append fields.
- An "offset" field (`pcv`, `gc`, `inl`) is the byte offset of a record in
  its section plus one; 0 means none.

## Versioning

- `major` changes only for incompatible changes. A reader rejects any other
  major.
- `minor` grows when fields or sections are added. A reader accepts a higher
  minor and ignores what it does not know.
- Section names and enum values below are a registry: add, never reuse.
- Each version keeps golden files under `test/gocobj/v<major>.<minor>/`.

## Sections

### `.goc.header` (fixed layout, 28 bytes)

| offset | type | field |
|---|---|---|
| 0 | `[8]byte` | magic `"GOCOBJ\0\0"` |
| 8 | u16 | major (1) |
| 10 | u16 | minor (0) |
| 12 | u8 | arch: 1 amd64, 2 arm64 |
| 13 | u8 | flags: bit 0 `stack_checks_in_code` (entry checks are already in the machine code; until goc's backend emits them, this is 0 and the converter adds them) |
| 14 | u16 | reserved, 0 |
| 16 | u32 strref | ABI profile, e.g. `go1.24-amd64` |
| 20 | u32 strref | producer (goc and LLVM versions) |
| 24 | u32 | reserved, 0 |

### `.goc.strtab`

String pool. Not a record list.

### `.goc.files`

One record per source file: `strref path`. Index = record number.

### `.goc.funcs`

One record per Go-visible function:

| field | type | meaning |
|---|---|---|
| name | strref | ELF symbol (STT_FUNC, defined, size = text_size) |
| text_size | uleb | bytes of machine code |
| frame_size | uleb | local frame, without the return address |
| arg_size | uleb | argument + result area |
| abi | u8 | 0 ABI0, 1 ABIInternal |
| kind | u8 | 0 normal, 1 ABI wrapper |
| flags | uleb | bit 0 needs_stack_check, 1 leaf, 2 nosplit, 3 has_frame_pointer, 4 unsafe_entry |
| func_id | u8 | 0 normal; other values reserved |
| file | uleb | index into `.goc.files` |
| start_line | uleb | |
| pcv | uleb | offset of its `.goc.pcvalue` record |
| gc | uleb | offset of its `.goc.gcmaps` record |
| inl | uleb | offset of its `.goc.inltree` record (reserved; 0 in 1.0) |

### `.goc.relocs`

One record per relocation inside a function's code:

| field | type | meaning |
|---|---|---|
| func | strref | function the bytes belong to |
| offset | uleb | byte offset from the function start |
| width | u8 | 1, 2, 4 or 8 |
| kind | u8 | see below |
| target | strref | ELF symbol |
| addend | sleb | |
| flags | u8 | bit 0 safepoint_call (a gc record exists at `offset+width`), bit 1 weak |

Kinds: 1 CALL, 2 CALL_INDIRECT_MARK, 3 PCREL, 4 ADDR_ABS, 5 FUNC_ADDR,
6 DATA_ADDR, 7 TLS_G, 8 GOT_PCREL, 9 JMP_TAILCALL, 10 ARM64_ADRP_ADD,
11 ARM64_CALL26.

ELF's own relocations in the file stay valid and are only a cross-check.

### `.goc.pcvalue`

One record per function: `uleb ntables`, then `ntables` tables of
`u8 kind, uleb count, count × {uleb pc_start, sleb value}`. Entry `i` holds
for `[pc_start_i, pc_start_{i+1})`; the last one holds to the end of the
function. The first `pc_start` is 0 and the starts strictly increase.

Kinds: 1 SP_DELTA, 2 FILE, 3 LINE, 4 UNSAFE_POINT, 5 INL_INDEX,
6 STACKMAP_INDEX.

Values are plain, not Go's delta encoding; the converter encodes.

### `.goc.gcmaps`

One record per function: `uleb nargs_bits, uleb nlocals_bits, uleb n`, then
`n` safepoints of `uleb pc` (the return address offset), args bitmap
(`ceil(nargs_bits/8)` bytes), locals bitmap (`ceil(nlocals_bits/8)` bytes),
`uleb ncolor` and `ncolor` bytes of optional color bits for diagnostics.
Bit `i` of the locals bitmap is the pointer word at `SP + i*ptr_size`.
Safepoints are sorted by pc.

### `.goc.data`

One record per data symbol: `strref name, uleb size, u8 log2 align,
uleb flags (bit 0 rodata, 1 noptr, 2 local, 3 dupok), uleb nbits,
ceil(nbits/8) bytes pointer bitmap` (bit `i` = pointer word `i`).

### Reserved names

`.goc.inltree`, `.goc.wrappers`, `.goc.types` are reserved and will be
specified when the backend emits them (a minor version bump).

## What `goc-objdump verify` checks

- header magic, major, known arch and ABI profile; ptr size from the profile
- every name a record uses exists in the ELF symbol table exactly once
- functions: symbol is a defined STT_FUNC whose size equals `text_size`;
  `file` is in range; `pcv`/`gc` offsets point at a record start
- relocs: function known, `offset + width <= text_size`, width and kind valid
- pcvalue: known kinds, first start 0, strictly increasing, inside the function
- gcmaps: pcs sorted and inside `(0, text_size]`; the locals bitmap fits the
  frame (`nlocals_bits * ptr_size <= frame_size`); every safepoint_call reloc
  has a gc record at `offset + width`
- with `stack_checks_in_code`, every `needs_stack_check` function starts with
  the profile's stack check (amd64: `cmp rsp,[r14+16]` or
  `lea r12,[rsp-n]; cmp r12,[r14+16]`)
- data: symbol exists, bitmap fits `size`
