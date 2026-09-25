#!/usr/bin/env python3
"""Prepare clang IR bodies for Go 1.24 amd64 ABIInternal entry thunks.

The supported signature subset is scalar integer/pointer/floating-point
parameters and returns, plus literal aggregates whose scalar fields each occupy
a distinct eightbyte. Aggregate parameters are assigned atomically by field:
they use registers only when every field fits, otherwise the whole value uses
the stack. Results are limited to 16 bytes; fields become separate Go results
and use their SysV eightbyte class (integer RAX/RDX, floating XMM0/XMM1).
Signatures outside this deliberately small subset remain SysV-only. Go
ABIInternal integer arguments through R10/R11 rely on the realbody split
preamble preserving those registers before the thunk.

Usage: goc_goabi.py <in.ll> <out.ll> <thunks.s> <goabi.json>
"""
import json
import os
import re
import sys


# Go 1.24 src/cmd/compile/abi-internal.md and src/internal/abi/abi_amd64.go.
GO_INT_REGS = ["rax", "rbx", "rcx", "rdi", "rsi", "r8", "r9", "r10", "r11"]
GO_FLOAT_REGS = ["xmm%d" % i for i in range(15)]
SYSV_INT_REGS = ["rdi", "rsi", "rdx", "rcx", "r8", "r9"]
SYSV_FLOAT_REGS = ["xmm%d" % i for i in range(8)]
SYSV_INT_RESULT_REGS = ["rax", "rdx"]
SYSV_FLOAT_RESULT_REGS = ["xmm0", "xmm1"]
INT_WIDTHS = {1, 8, 16, 32, 64}
NAME = r"[A-Za-z_][A-Za-z0-9_.$]*"
HEADER_RE = re.compile(r"^\s*(define|declare)\s+(.+?)@(" + NAME + r")\(", re.M)
TYPE_RE = re.compile(r"^(void|ptr|float|double|i[0-9]+)\b")
NAMED_TYPE_RE = re.compile(r"^%[-A-Za-z$._0-9]+")
TYPED_PTR_RE = re.compile(r"^(?:void|float|double|i[0-9]+)(?:\s+addrspace\([0-9]+\))?\s*\*")
NON_C_CC_RE = re.compile(
    r"\b(?:fastcc|coldcc|ghccc|hipecc|webkit_jscc|anyregcc|preserve_mostcc|"
    r"preserve_allcc|swiftcc|cxx_fast_tlscc|tailcc|cfguard_checkcc|"
    r"x86_stdcallcc|x86_fastcallcc|x86_thiscallcc|x86_vectorcallcc|"
    r"x86_64_sysvcc|cc\s+[0-9]+)\b")
ABI_PARAM_ATTR_RE = re.compile(
    r"\b(byval|sret|inalloca|preallocated|byref|inreg|nest|swiftself|"
    r"swiftasync|swifterror)\b")
ABI_RET_ATTR_RE = re.compile(r"\b(sret|inreg)\b")
GPR_VIEWS = {
    "rax": {1: "al", 2: "ax", 4: "eax", 8: "rax"},
    "rbx": {1: "bl", 2: "bx", 4: "ebx", 8: "rbx"},
    "rcx": {1: "cl", 2: "cx", 4: "ecx", 8: "rcx"},
    "rdx": {1: "dl", 2: "dx", 4: "edx", 8: "rdx"},
    "rsi": {1: "sil", 2: "si", 4: "esi", 8: "rsi"},
    "rdi": {1: "dil", 2: "di", 4: "edi", 8: "rdi"},
    "r8": {1: "r8b", 2: "r8w", 4: "r8d", 8: "r8"},
    "r9": {1: "r9b", 2: "r9w", 4: "r9d", 8: "r9"},
    "r10": {1: "r10b", 2: "r10w", 4: "r10d", 8: "r10"},
    "r11": {1: "r11b", 2: "r11w", 4: "r11d", 8: "r11"},
    "r12": {1: "r12b", 2: "r12w", 4: "r12d", 8: "r12"},
}


def split_top_level(text):
    """Split a comma-separated LLVM list without splitting nested types."""
    parts = []
    start = 0
    stack = []
    pairs = {")": "(", "]": "[", "}": "{", ">": "<"}
    for i, ch in enumerate(text):
        if ch in "([{<":
            stack.append(ch)
        elif ch in ")]}>" and stack and stack[-1] == pairs[ch]:
            stack.pop()
        elif ch == "," and not stack:
            parts.append(text[start:i].strip())
            start = i + 1
    tail = text[start:].strip()
    if tail:
        parts.append(tail)
    return parts


def matching_paren(text, opening):
    depth = 0
    for i in range(opening, len(text)):
        if text[i] == "(":
            depth += 1
        elif text[i] == ")":
            depth -= 1
            if depth == 0:
                return i
    return -1


def llvm_type_prefix(text):
    """Return one LLVM type at the start of text, preserving literal structs."""
    text = text.strip()
    if text.startswith("<{"):
        depth = 0
        for i, ch in enumerate(text[1:], 1):
            if ch == "{":
                depth += 1
            elif ch == "}":
                depth -= 1
                if depth == 0 and i + 1 < len(text) and text[i + 1] == ">":
                    return text[:i + 2]
    if text.startswith("{"):
        depth = 0
        for i, ch in enumerate(text):
            if ch == "{":
                depth += 1
            elif ch == "}":
                depth -= 1
                if depth == 0:
                    return text[:i + 1]
        return ""
    typed_ptr = TYPED_PTR_RE.match(text)
    if typed_ptr:
        return typed_ptr.group(0).strip()
    addrspace = re.match(r"ptr\s+addrspace\([0-9]+\)", text)
    if addrspace:
        return addrspace.group(0)
    named = NAMED_TYPE_RE.match(text)
    if named:
        return named.group(0)
    m = TYPE_RE.match(text)
    return m.group(1) if m else ""


def parse_param(text):
    ty = llvm_type_prefix(text)
    rest = text[len(ty):]
    attrs = re.findall(r"\b(signext|zeroext|noundef)\b", rest)
    abi_attrs = ABI_PARAM_ATTR_RE.findall(rest)
    return ty, attrs, abi_attrs


def parse_return(prefix):
    prefix = prefix.rstrip()
    if prefix.endswith("}") or prefix.endswith("}>"):
        depth = 0
        end = len(prefix) - 1 if prefix.endswith("}") else len(prefix) - 2
        for i in range(end, -1, -1):
            if prefix[i] == "}":
                depth += 1
            elif prefix[i] == "{":
                depth -= 1
                if depth == 0:
                    return prefix[i - 1:] if i > 0 and prefix[i - 1] == "<" else prefix[i:]
        return ""
    m = re.search(r"(%[-A-Za-z$._0-9]+|ptr\s+addrspace\([0-9]+\)|void|ptr|float|double|i[0-9]+)\s*$", prefix)
    return m.group(1) if m else ""


def parse_functions(text, keyword):
    functions = []
    for m in HEADER_RE.finditer(text):
        if m.group(1) != keyword:
            continue
        opening = m.end() - 1
        closing = matching_paren(text, opening)
        if closing < 0:
            continue
        params_text = text[opening + 1:closing]
        prefix = m.group(2)
        raw_params = [] if not params_text.strip() else split_top_level(params_text)
        varargs = "..." in raw_params
        param_info = [parse_param(p) for p in raw_params if p != "..."]
        functions.append({
            "name": m.group(3),
            "ret": parse_return(prefix),
            "ret_attrs": re.findall(r"\b(signext|zeroext)\b", prefix),
            "ret_abi_attrs": ABI_RET_ATTR_RE.findall(prefix),
            "params": [p[0] for p in param_info],
            "param_attrs": [p[1] for p in param_info],
            "param_abi_attrs": [p[2] for p in param_info],
            "varargs": varargs,
            "internal": re.search(r"\binternal\b", prefix) is not None,
            "non_c_calling_convention": NON_C_CC_RE.search(prefix) is not None,
        })
    return functions


def scalar(ty):
    if ty == "ptr":
        return ("int", 8, 8)
    if ty in ("float", "double"):
        size = 4 if ty == "float" else 8
        return ("float", size, size)
    m = re.fullmatch(r"i([0-9]+)", ty)
    if not m:
        return None
    bits = int(m.group(1))
    if bits not in INT_WIDTHS:
        return None
    size = 1 if bits == 1 else bits // 8
    return ("int", size, size)


def aggregate_layout(ty, purpose="aggregate", return_limit=False):
    if ty.startswith("<{"):
        return None, "packed aggregate %s layouts are unsupported" % purpose
    if not (ty.startswith("{") and ty.endswith("}")):
        return None, None
    body = ty[1:-1].strip()
    fields = split_top_level(body) if body else []
    if not fields:
        return None, "empty aggregate %s is unsupported" % purpose

    offset = 0
    struct_align = 1
    laid_out = []
    for ty_field in fields:
        info = scalar(ty_field)
        if info is None:
            return None, "aggregate field type %s is outside the supported scalar subset" % ty_field
        bank, size, alignment = info
        offset = align(offset, alignment)
        laid_out.append({"type": ty_field, "bank": bank, "size": size,
                         "offset": offset})
        offset += size
        struct_align = max(struct_align, alignment)

    size = align(offset, struct_align)
    if return_limit and size > 16:
        return None, ("aggregate return is %d bytes; SysV register returns are limited to "
                      "16 bytes (larger results require sret)" % size)

    # A field spanning two eightbytes or multiple fields sharing one eightbyte
    # needs SysV class merging/coercion (for example {float, i32} -> i64).
    # The thunk only models one scalar leaf per eightbyte, so reject those
    # layouts instead of treating a packed C representation as separate values.
    for field in laid_out:
        first = field["offset"] // 8
        last = (field["offset"] + field["size"] - 1) // 8
        if first != last:
            return None, "aggregate %s field crosses an eightbyte boundary" % purpose
    eightbytes = [field["offset"] // 8 for field in laid_out]
    if len(set(eightbytes)) != len(eightbytes):
        return None, "aggregate %s packs several fields into one eightbyte" % purpose
    return {"fields": laid_out, "size": size, "alignment": struct_align}, None


def go_type(ty, attrs=(), aggregate_field=False):
    if ty == "ptr":
        return "unsafe.Pointer"
    if ty == "float":
        return "float32"
    if ty == "double":
        return "float64"
    m = re.fullmatch(r"i([0-9]+)", ty)
    if not m:
        return None
    bits = int(m.group(1))
    if bits == 1:
        return "bool"
    if aggregate_field:
        return "uint%d" % bits
    prefix = "uint" if "zeroext" in attrs else "int"
    return prefix + str(bits)


def go_param_type(ty, attrs=()):
    info = scalar(ty)
    if info:
        return go_type(ty, attrs)
    aggregate, _ = aggregate_layout(ty, "parameter")
    if aggregate is None:
        return None
    fields = []
    for index, field in enumerate(aggregate["fields"]):
        field_type = go_type(field["type"], aggregate_field=True)
        if field_type is None:
            return None
        fields.append("F%d %s" % (index, field_type))
    return "struct { " + "; ".join(fields) + " }"


def location(kind, name_or_offset):
    if kind == "reg":
        return {"kind": "register", "register": name_or_offset}
    return {"kind": "stack", "offset": name_or_offset}


def align(value, alignment):
    return (value + alignment - 1) & -alignment


def assign_params(params):
    go_int = go_float = c_int = c_float = 0
    go_stack = 0
    c_stack = 0
    assignments = []
    for ty in params:
        cls_size = scalar(ty)
        if cls_size is None:
            aggregate, reason = aggregate_layout(ty, "parameter")
            if reason:
                return None, reason
            if aggregate:
                fields = aggregate["fields"]
                go_need_int = sum(field["bank"] == "int" for field in fields)
                go_need_float = len(fields) - go_need_int
                go_in_regs = (go_int + go_need_int <= len(GO_INT_REGS) and
                              go_float + go_need_float <= len(GO_FLOAT_REGS))
                if go_in_regs:
                    go_locs = []
                    for field in fields:
                        if field["bank"] == "int":
                            go_locs.append(location("reg", GO_INT_REGS[go_int]))
                            go_int += 1
                        else:
                            go_locs.append(location("reg", GO_FLOAT_REGS[go_float]))
                            go_float += 1
                else:
                    go_base = align(go_stack, aggregate["alignment"])
                    go_locs = [location("stack", go_base + field["offset"])
                               for field in fields]
                    go_stack = go_base + aggregate["size"]

                # SysV passes an aggregate larger than two eightbytes in
                # memory. For smaller values, all classes must fit or the
                # entire aggregate is rolled back to the stack.
                c_need_int = sum(field["bank"] == "int" for field in fields)
                c_need_float = len(fields) - c_need_int
                c_in_regs = (aggregate["size"] <= 16 and
                             c_int + c_need_int <= len(SYSV_INT_REGS) and
                             c_float + c_need_float <= len(SYSV_FLOAT_REGS))
                if c_in_regs:
                    c_locs = []
                    for field in fields:
                        if field["bank"] == "int":
                            c_locs.append(location("reg", SYSV_INT_REGS[c_int]))
                            c_int += 1
                        else:
                            c_locs.append(location("reg", SYSV_FLOAT_REGS[c_float]))
                            c_float += 1
                else:
                    c_base = align(c_stack, 8)
                    c_locs = [location("stack", c_base + field["offset"])
                              for field in fields]
                    c_stack = c_base + align(aggregate["size"], 8)

                assignments.extend({
                    "type": field["type"], "go": go_loc, "sysv": c_loc
                } for field, go_loc, c_loc in zip(fields, go_locs, c_locs))
                continue
            if ty.startswith("%"):
                return None, ("identified aggregate parameter type %s has no literal "
                              "field layout and is unsupported" % ty)
            if ty.startswith("{") or ty.startswith("<{"):
                return None, "aggregate parameter type %s is unsupported" % ty
            return None, "parameter type %s is outside the scalar subset" % ty
        bank, size, alignment = cls_size
        if bank == "int" and go_int < len(GO_INT_REGS):
            go_loc = location("reg", GO_INT_REGS[go_int])
            go_int += 1
        elif bank == "float" and go_float < len(GO_FLOAT_REGS):
            go_loc = location("reg", GO_FLOAT_REGS[go_float])
            go_float += 1
        else:
            go_stack = align(go_stack, min(alignment, 8))
            go_loc = location("stack", go_stack)
            go_stack += size

        if bank == "int" and c_int < len(SYSV_INT_REGS):
            c_loc = location("reg", SYSV_INT_REGS[c_int])
            c_int += 1
        elif bank == "float" and c_float < len(SYSV_FLOAT_REGS):
            c_loc = location("reg", SYSV_FLOAT_REGS[c_float])
            c_float += 1
        else:
            c_loc = location("stack", c_stack)
            c_stack += 8
        assignments.append({"type": ty, "go": go_loc, "sysv": c_loc})

    return {
        "params": assignments,
        "go_stack_size": align(go_stack, 8),
        "sysv_stack_size": c_stack,
    }, None


def return_assignment(ret):
    if ret == "void":
        return [], [], [], None
    info = scalar(ret)
    if info:
        bank = info[0]
        reg = "rax" if bank == "int" else "xmm0"
        return ([{"type": ret, "location": location("reg", reg)}],
                [{"type": ret, "location": location("reg", reg)}],
                [], None)

    aggregate, reason = aggregate_layout(ret, "return", return_limit=True)
    if reason:
        return None, None, None, reason
    if aggregate:
        int_result = float_result = 0
        go_results = []
        sysv_results = []
        for field in aggregate["fields"]:
            if field["bank"] == "int":
                reg = GO_INT_REGS[int_result]
                sysv_reg = SYSV_INT_RESULT_REGS[int_result]
                int_result += 1
            else:
                reg = GO_FLOAT_REGS[float_result]
                sysv_reg = SYSV_FLOAT_RESULT_REGS[float_result]
                float_result += 1
            go_results.append({"type": field["type"],
                               "location": location("reg", reg)})
            sysv_results.append({"type": field["type"],
                                 "location": location("reg", sysv_reg)})
        return (go_results, sysv_results,
                [field["type"] for field in aggregate["fields"]], None)
    if ret.startswith("{") or ret.startswith("<{"):
        return None, None, None, "aggregate return type %s is unsupported" % ret
    if ret.startswith("%"):
        return None, None, None, ("identified aggregate return type %s has no literal "
                                  "field layout and is unsupported" % ret)
    return (None, None, None,
            "return type %s is outside the supported result subset" % ret)


def analyze_signature(d, reserve):
    if d["name"].startswith("llvm."):
        return None, "LLVM intrinsics are not C entrypoints"
    if d["internal"]:
        return None, "file-local functions are not Go-callable"
    if d["varargs"]:
        return None, "variadic functions are unsupported"
    if d["non_c_calling_convention"]:
        return None, "non-C LLVM calling conventions are unsupported"
    if any(d["param_abi_attrs"]):
        return None, "ABI-changing LLVM parameter attributes are unsupported"
    if d["ret_abi_attrs"]:
        return None, "ABI-changing LLVM return attributes are unsupported"
    if not d["ret"]:
        return None, "LLVM return type could not be parsed"
    assigned, reason = assign_params(d["params"])
    if reason:
        return None, reason
    go_results, sysv_results, result_types, reason = return_assignment(d["ret"])
    if reason:
        return None, reason

    # Both ABIs hand out result registers per class in field order: SysV
    # RAX/RDX and XMM0/XMM1, Go RAX/RBX and X0/X1. The only possible move is
    # therefore RDX -> RBX, whose destination is never a source.
    result_moves = [(src["location"]["register"], dst["location"]["register"])
                    for src, dst in zip(sysv_results, go_results)
                    if src["location"]["register"] != dst["location"]["register"]]

    # A C body may clobber XMM15, which is Go ABIInternal's fixed-zero
    # register. A tail jump has no return edge on which to restore it, so every
    # emitted thunk uses the fixed CALL frame and clears XMM15 on return.
    if reserve <= 0:
        return None, "safe Go return requires a fixed thunk frame; set GOC_CRESERVE"
    if assigned["sysv_stack_size"] > reserve:
        return None, ("SysV outgoing stack arguments need %d bytes but GOC_CRESERVE is %d" %
                      (assigned["sysv_stack_size"], reserve))

    go_params = [go_param_type(ty, attrs)
                 for ty, attrs in zip(d["params"], d["param_attrs"])]
    if any(ty is None for ty in go_params):
        return None, "a parameter has no supported Go-facing type"
    if result_types:
        go_result_types = [go_type(ty, aggregate_field=True) for ty in result_types]
    else:
        go_result_types = [go_type(d["ret"], d["ret_attrs"])] if d["ret"] != "void" else []
    if any(ty is None for ty in go_result_types):
        return None, "the result has no supported Go-facing type"

    signature = "func %s(%s)" % (d["name"], ", ".join(go_params))
    if len(go_result_types) == 1:
        signature += " " + go_result_types[0]
    elif go_result_types:
        signature += " (" + ", ".join(go_result_types) + ")"

    abi = {
        "go": {
            "params": [{"type": a["type"], "location": a["go"]}
                       for a in assigned["params"]],
            "results": go_results,
            "stack_args_size": assigned["go_stack_size"],
        },
        "sysv": {
            "params": [{"type": a["type"], "location": a["sysv"]}
                       for a in assigned["params"]],
            "results": sysv_results,
            "stack_args_size": assigned["sysv_stack_size"],
        },
    }
    return {
        "abi": abi,
        "go_signature": signature,
        "result_moves": result_moves,
    }, None


def gpr_view(reg, size):
    return GPR_VIEWS[reg][size]


def move_mnemonic(size):
    return {1: "movb", 2: "movw", 4: "movl", 8: "movq"}[size]


def stack_source(location_info):
    # Go stack argument offsets start at the caller's outgoing-argument area;
    # at thunk entry that area is 8(%rsp), or 16(%rbp) after saving RBP.
    return "%d(%%rbp)" % (16 + location_info["offset"])


def parallel_gpr_moves(moves):
    """Emit dependency-safe parallel qword copies, using R12 for a cycle."""
    pending = [(src, dst) for src, dst in moves if src != dst]
    output = []
    while pending:
        sources = {src for src, _ in pending}
        ready = next((i for i, (_, dst) in enumerate(pending) if dst not in sources), None)
        if ready is not None:
            src, dst = pending.pop(ready)
            output.append("\tmovq\t%%%s, %%%s" % (src, dst))
            continue
        src, _ = pending[0]
        output.append("\tmovq\t%%%s, %%r12" % src)
        pending = [("r12" if old_src == src else old_src, dst)
                   for old_src, dst in pending]
    return output


def parallel_xmm_moves(moves):
    """Emit dependency-safe scalar FP copies, using XMM15 for a cycle."""
    pending = [(src, dst, ty) for src, dst, ty in moves if src != dst]
    output = []
    while pending:
        sources = {src for src, _, _ in pending}
        ready = next((i for i, (_, dst, _) in enumerate(pending)
                      if dst not in sources), None)
        if ready is not None:
            src, dst, ty = pending.pop(ready)
            output.append("\tmov%s\t%%%s, %%%s" % (
                "ss" if ty == "float" else "sd", src, dst))
            continue
        src, _, ty = pending[0]
        output.append("\tmov%s\t%%%s, %%xmm15" % (
            "ss" if ty == "float" else "sd", src))
        pending = [("xmm15" if old_src == src else old_src, dst, old_ty)
                   for old_src, dst, old_ty in pending]
    return output


def emit_stack_argument(ty, source, dest):
    info = scalar(ty)
    bank, size, _ = info
    if bank == "float":
        op = "movss" if size == 4 else "movsd"
        if source["kind"] == "register":
            src = "%%%s" % source["register"]
            return ["\t%s\t%s, %s" % (op, src, dest)]
        return ["\t%s\t%s, %%xmm15" % (op, stack_source(source)),
                "\t%s\t%%xmm15, %s" % (op, dest)]

    op = move_mnemonic(size)
    if source["kind"] == "register":
        src_reg = source["register"]
        src = "%" + gpr_view(src_reg, size)
        return ["\t%s\t%s, %s" % (op, src, dest)]
    load = "\t%s\t%s, %%%s" % (op, stack_source(source), gpr_view("r12", size))
    store = "\t%s\t%%%s, %s" % (op, gpr_view("r12", size), dest)
    return [load, store]


def emit_register_argument(ty, source, dest):
    """Load a stack-assigned Go field directly into its SysV argument register."""
    bank, size, _ = scalar(ty)
    if bank == "float":
        op = "movss" if size == 4 else "movsd"
        return ["\t%s\t%s, %%%s" % (op, stack_source(source), dest)]
    return ["\t%s\t%s, %%%s" % (
        move_mnemonic(size), stack_source(source), gpr_view(dest, size))]


def emit_thunks(goabi, reserve):
    lines = ["# Go 1.24 ABIInternal -> SysV entry thunks — generated", "\t.text"]
    for d in goabi:
        lines += ["\t.globl\t%s" % d["name"],
                  "\t.type\t%s,@function" % d["name"],
                  "%s:" % d["name"]]
        plan = d["_plan"]
        if reserve > 0:
            lines += ["\tpushq\t%rbp", "\tmovq\t%rsp, %rbp",
                      "\tsubq\t$%d, %%rsp" % reserve]

        reg_moves = []
        float_moves = []
        int_loads = []
        float_loads = []
        for i, arg in enumerate(plan["abi"]["go"]["params"]):
            ty = arg["type"]
            go_loc = arg["location"]
            sysv_loc = plan["abi"]["sysv"]["params"][i]["location"]
            if sysv_loc["kind"] == "stack":
                lines += emit_stack_argument(ty, go_loc,
                                             "%d(%%rsp)" % sysv_loc["offset"])
                continue

            bank = scalar(ty)[0]
            if go_loc["kind"] == "stack":
                load = emit_register_argument(ty, go_loc, sysv_loc["register"])
                (int_loads if bank == "int" else float_loads).extend(load)
            elif bank == "int":
                reg_moves.append((go_loc["register"], sysv_loc["register"]))
            elif go_loc["register"] != sysv_loc["register"]:
                float_moves.append((go_loc["register"], sysv_loc["register"], ty))
        lines += parallel_gpr_moves(reg_moves)
        # Aggregate rollback can advance one ABI's FP sequence without
        # advancing the other, so XMM moves may shift in either direction.
        # Resolve cycles before stack loads that may replace move sources.
        lines += parallel_xmm_moves(float_moves)
        lines += int_loads + float_loads

        lines.append("\tcall\t%s" % d["impl"])
        for src, dst in plan["result_moves"]:
            lines.append("\tmovq\t%%%s, %%%s" % (src, dst))
        # X15 is Go's fixed-zero register at ABI boundaries; SysV code may
        # freely clobber it, so restore it before returning to Go.
        lines += ["\tpxor\t%xmm15, %xmm15",
                  "\tmovq\t%rbp, %rsp", "\tpopq\t%rbp", "\tret"]
        lines.append("\t.size\t%s, .-%s" % (d["name"], d["name"]))
    lines.append("")
    return lines


def main():
    in_ll, out_ll, out_s, out_json = sys.argv[1:5]
    text = open(in_ll).read()
    reserve = int(os.environ.get("GOC_CRESERVE", "0") or "0")
    if reserve < 0 or reserve % 8 or reserve > 0x7fffffff:
        raise ValueError("GOC_CRESERVE must be an 8-byte multiple within signed 32-bit range")

    defs = parse_functions(text, "define")
    goabi, skipped, reasons = [], [], {}
    for d in defs:
        plan, reason = analyze_signature(d, reserve)
        if reason:
            skipped.append(d["name"])
            reasons[d["name"]] = reason
            continue
        d["_plan"] = plan
        d["abi"] = plan["abi"]
        d["go_signature"] = plan["go_signature"]
        d["impl"] = d["name"] + ".impl"
        goabi.append(d)

    # Cross-TU C references use the same .impl symbol as a supported defining
    # TU. Apply exactly the same signature and reserve constraints to decls.
    rename_names = {d["name"] for d in goabi}
    for d in parse_functions(text, "declare"):
        if d["name"] in rename_names:
            continue
        plan, _ = analyze_signature(d, reserve)
        if plan:
            rename_names.add(d["name"])

    for name in sorted(rename_names):
        text = re.sub(r"@" + re.escape(name) + r"(?![A-Za-z0-9_.$])",
                      "@" + name + ".impl", text)
    open(out_ll, "w").write(text)

    lines = emit_thunks(goabi, reserve)
    open(out_s, "w").write("\n".join(lines))

    # Keep the established top-level schema and function names. Added ABI maps
    # and go_signature strings make the register/stack decisions inspectable.
    json_functions = []
    for d in goabi:
        entry = {key: value for key, value in d.items() if key != "_plan"}
        json_functions.append(entry)
    json.dump({"functions": json_functions, "skipped": skipped},
              open(out_json, "w"), indent=2)
    print("goabi: %d rewritten, %d left SysV-only%s" %
          (len(goabi), len(skipped),
           (" [" + ", ".join(skipped) + "]") if skipped else ""))
    for name in skipped:
        print("goabi: skipped %s: %s" % (name, reasons[name]))


if __name__ == "__main__":
    main()
