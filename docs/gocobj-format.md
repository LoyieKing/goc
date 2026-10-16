# gocobj format, version 1.4

A `.gocobj` is what goc's LLVM backend will write for one C file. It is a
normal relocatable ELF file (machine code in `.text` or `.text.<sym>`, data in
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
| 10 | u16 | minor (1) |
| 12 | u8 | arch: 1 amd64, 2 arm64 |
| 13 | u8 | flags: bit 0 `stack_checks_in_code` (entry checks are already in the machine code; goc-llc always sets it, and gocobj2goobj rejects an object with it clear) |
| 14 | u16 | reserved, 0 |
| 16 | u32 strref | ABI profile, e.g. `go1.24-amd64` |
| 20 | u32 strref | producer (goc and LLVM versions) |
| 24 | u32 | reserved, 0 |

### `.goc.strtab`

String pool. Not a record list.

### `.goc.files`

One record per source file: `strref path`. Index = record number. Record 0
is the translation unit; 1.4 adds every other file a line row or an inlined
call refers to (headers), with the directory joined to relative names.

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
| flags | uleb | bit 0 needs_stack_check, 1 leaf, 2 nosplit, 3 has_frame_pointer, 4 unsafe_entry; 1.1: 5 internal linkage, 6 has an sptr color, 7 sysv_ptr_regs not modelled |
| func_id | u8 | 0 normal; other values reserved |
| file | uleb | index into `.goc.files` (1.4: the file of the C definition) |
| start_line | uleb | 1.4: line of the C definition (0 when unknown) |
| pcv | uleb | offset of its `.goc.pcvalue` record |
| gc | uleb | offset of its `.goc.gcmaps` record |
| inl | uleb | 1.4: offset of its `.goc.inltree` record |
| sysv_ptr_regs | uleb | 1.1: bit i set when SysV integer argument register i (rdi rsi rdx rcx r8 r9) holds a pointer on entry |
| sptr_slots | uleb n, n × uleb | 1.2: frame-pointer-relative offsets (`BP - off`, positive, multiples of 8, sorted) of the frame slots that hold sptr values |

### `.goc.relocs`

One record per relocation inside a function's code:

| field | type | meaning |
|---|---|---|
| func | strref | function the bytes belong to |
| offset | uleb | byte offset from the function start |
| width | u8 | 1, 2, 4 or 8; for CALL_INDIRECT_MARK the call instruction length (offset is the instruction start) |
| kind | u8 | see below |
| target | strref | ELF symbol (empty for CALL_INDIRECT_MARK) |
| addend | sleb | |
| flags | u8 | bit 0 safepoint_call (a gc record exists at `offset+width`), bit 1 weak |

Kinds: 1 CALL, 2 CALL_INDIRECT_MARK, 3 PCREL, 4 ADDR_ABS, 5 FUNC_ADDR,
6 DATA_ADDR, 7 TLS_G, 8 GOT_PCREL, 9 JMP_TAILCALL, 10 ARM64_ADRP_ADD,
11 ARM64_CALL26.

ELF's own relocations in the file stay valid and are only a cross-check.

### `.goc.stackmaps` (1.3)

One record per LLVM `STACKMAP` site: `strref func`, `uleb insn_off` (from
the function start), `uleb id`, `uleb nloc`, then `nloc` locations of
`u8 type, uleb size, uleb reg, sleb off`, in the encoding of
`.llvm_stackmaps` version 3 (type 1 Register, 2 Direct, 3 Indirect,
4 Constant; `reg` is a DWARF register number). Records are in code order
within a function. The converter reads bitmaps from `.goc.gcmaps`; these
records are kept for `verify` and debugging.

### `.goc.wrappers` (1.3)

One record per ABI wrapper. The wrapper itself is a `.goc.funcs` record
with kind 1 (its `arg_size` is the Go argument area); this record adds:

| field | type | meaning |
|---|---|---|
| sym | strref | the wrapper (a kind-1 function) |
| target | strref | the function it calls |
| from_abi | u8 | convention it is entered with: 0 ABI0, 1 ABIInternal, 2 C (SysV) |
| to_abi | u8 | convention of `target` |
| nspills | uleb | then `nspills` × {`strref reg`, `uleb off`, `uleb size`, `u8 ptr`}: where the morestack path spills each register argument |
| nstackptr | uleb | then `nstackptr` × `uleb`: offsets of pointer arguments passed on the stack |

### `.goc.pcvalue`

One record per function: `uleb ntables`, then `ntables` tables of
`u8 kind, uleb count, count × {uleb pc_start, sleb value}`. Entry `i` holds
for `[pc_start_i, pc_start_{i+1})`; the last one holds to the end of the
function. The first `pc_start` is 0 and the starts strictly increase.

Kinds: 1 SP_DELTA, 2 FILE, 3 LINE, 4 UNSAFE_POINT, 5 INL_INDEX,
6 STACKMAP_INDEX.

Values are plain, not Go's delta encoding; the converter encodes.

1.4: FILE (index into `.goc.files`), LINE and INL_INDEX (index into the
function's `.goc.inltree` record, -1 outside inlined code) come from the
instructions' debug locations (clang `-gline-tables-only`); code without a
location (the stack check, spills) keeps the previous row. In these three
kinds a row may share its `pc_start` with the next one (its instruction
emits no bytes); the later row wins and the reader drops the earlier.

### `.goc.inltree` (1.4)

One record per function with inlined calls: `uleb n`, then `n` entries of

| field | type | meaning |
|---|---|---|
| parent | sleb | index of the enclosing inlined call, -1 for the function itself |
| file | uleb | index into `.goc.files` of the call site |
| line | uleb | line of the call site |
| func | strref | the inlined C function (its C name, not an ELF symbol) |
| parent_pc | uleb | offset from the function start whose FILE/LINE/INL_INDEX rows are the call site |
| start_line | uleb | line of the inlined function's definition |

Go's runtime finds the call site of an inlined frame by looking up the rows
at `parent_pc`. As Go's compiler does with its inline marks, goc-llc gives
the first non-call instruction of each inlined body the call site's rows.
A body that starts with a direct call uses the call's second byte instead
(a return address minus one is the call's last byte, so its interior bytes
are never looked up). Otherwise `parent_pc` is the last row of the parent
before the body.

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

`.goc.types` is reserved and will be specified when the backend emits it
(a minor version bump).

## What `goc-objdump verify` checks

- header magic, major, known arch and ABI profile; ptr size from the profile
- every name a record uses exists in the ELF symbol table exactly once
- functions: symbol is a defined STT_FUNC whose size equals `text_size`;
  `file` is in range; `pcv`/`gc` offsets point at a record start
- relocs: function known, `offset + width <= text_size`, width and kind valid
- pcvalue: known kinds, first start 0, strictly increasing, inside the function;
  FILE values index `.goc.files`, INL_INDEX values the function's inline tree
- inltree: parents precede children, files in range, `parent_pc` inside the
  function
- gcmaps: pcs sorted and inside `(0, text_size]`; the locals bitmap fits the
  frame (`nlocals_bits * ptr_size <= frame_size`); every safepoint_call reloc
  has a gc record at `offset + width`
- with `stack_checks_in_code`, every `needs_stack_check` function starts with
  the profile's stack check (amd64: `cmp rsp,[r14+16]` or
  `lea r12,[rsp-n]; cmp r12,[r14+16]`)
- data: symbol exists, bitmap fits `size`

## Producer today

`goc-llc -function-sections -goc-tables=FILE` writes the `.goc.*` sections as
assembler text, including the stack check and morestack block it emitted
(shared `GocStackCheck` pass plus the per-arch hook), the SP_DELTA and
UNSAFE_POINT pc tables, the gcmap bitmaps, and (1.4) the FILE, LINE and
INL_INDEX tables and inline tree from the debug locations. `goc-lower goabi` appends the Go
ABI thunks and their records. `tools/goc/realbody.sh` assembles everything
with `llvm-mc` into one ELF, which is the `.gocobj` (saved next to the
`.goobj`), and runs `gocobj2goobj -symabis`. The converter does not decode or
emit instructions; `.llvm_stackmaps` and `meta.json` are no longer read.
For an inlined static function that has no body in the object, the
converter defines a one-byte stub (never referenced, so not linked): Go's
linker reads every inlined function's FuncInfo.

`test/gocobj/verify-real.sh` runs `goc-objdump verify` on every `.gocobj`
under `build/`. `verify` also checks each table relocation against the ELF
relocation at the same place, when there is one.
