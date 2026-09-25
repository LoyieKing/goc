#!/usr/bin/env bash
# Run the tests.conf-selected QuickJS tests through the goc-built Go CLI.
set -euo pipefail

ROOT="${GOC_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
export GOC_ROOT="$ROOT"
OUT="$ROOT/build/qjs"
mkdir -p "$OUT"

python3 - "$ROOT" "$OUT" "${QJS_CLI_TEST_TIMEOUT:-20}" <<'PY'
import json
import os
import re
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path, PurePosixPath


root = Path(sys.argv[1]).resolve()
out_dir = Path(sys.argv[2]).resolve()
try:
    timeout_seconds = float(sys.argv[3])
    if not (0 < timeout_seconds <= 3600):
        raise ValueError
except ValueError:
    print("QJS_CLI_TEST_TIMEOUT must be greater than 0 and at most 3600 seconds", file=sys.stderr)
    raise SystemExit(2)

config_path = root / "third_party/quickjs-ng/tests.conf"
config = {"config": {}, "features": {}, "exclude": []}
section = None
try:
    for line_number, raw in enumerate(config_path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        section_match = re.fullmatch(r"\[([A-Za-z0-9_-]+)\]", line)
        if section_match:
            section = section_match.group(1)
            if section not in config:
                raise ValueError(f"unsupported tests.conf section [{section}] at line {line_number}")
            continue
        if section == "exclude":
            config["exclude"].append(line)
            continue
        if section not in ("config", "features") or "=" not in line:
            raise ValueError(f"unsupported tests.conf entry at line {line_number}: {line}")
        key, value = (part.strip() for part in line.split("=", 1))
        config[section][key] = value
except (OSError, ValueError) as error:
    print(f"cannot read QuickJS tests.conf: {error}", file=sys.stderr)
    raise SystemExit(2)

if config["config"].get("local") != "yes":
    print("tests.conf must select local=yes for this runner", file=sys.stderr)
    raise SystemExit(2)
unknown_config = set(config["config"]) - {"local", "verbose", "testdir"}
if unknown_config:
    print(f"unimplemented tests.conf config keys: {', '.join(sorted(unknown_config))}", file=sys.stderr)
    raise SystemExit(2)
testdir = config["config"].get("testdir", "tests")
tests_root = (root / "third_party/quickjs-ng" / testdir).resolve()
try:
    tests_root.relative_to(root / "third_party/quickjs-ng")
except ValueError:
    print(f"tests.conf testdir escapes the QuickJS tree: {testdir}", file=sys.stderr)
    raise SystemExit(2)
if not tests_root.is_dir():
    print(f"tests.conf testdir does not exist: {testdir}", file=sys.stderr)
    raise SystemExit(2)

excluded = set()
for entry in config["exclude"]:
    normalized = PurePosixPath(entry)
    prefix = PurePosixPath(testdir)
    try:
        relative = normalized.relative_to(prefix)
    except ValueError:
        print(f"tests.conf exclusion is outside {testdir}: {entry}", file=sys.stderr)
        raise SystemExit(2)
    if relative.is_absolute() or ".." in relative.parts:
        print(f"unsafe tests.conf exclusion: {entry}", file=sys.stderr)
        raise SystemExit(2)
    excluded.add(relative.as_posix())
    if not (tests_root / relative).is_file():
        print(f"tests.conf exclusion does not exist: {entry}", file=sys.stderr)
        raise SystemExit(2)

tests = sorted(
    (path for path in tests_root.rglob("*.js")
     if path.is_file() and path.relative_to(tests_root).as_posix() not in excluded),
    key=lambda path: path.relative_to(tests_root).as_posix(),
)
cli = root / "build/qjs/qjscli"


def scalar(value):
    value = value.strip()
    if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
        return value[1:-1]
    return value


def list_values(value):
    value = value.strip()
    if value.startswith("[") and value.endswith("]"):
        return [scalar(part) for part in value[1:-1].split(",") if part.strip()]
    if value:
        return [scalar(value)]
    return []


def metadata_for(source):
    match = re.match(r"\ufeff?\s*/\*---(.*?)---\*/", source, re.S)
    if not match:
        if re.match(r"\ufeff?\s*/\*---", source):
            return {}, "unterminated metadata frontmatter"
        return {}, None

    metadata = {}
    current_key = None
    negative = {}
    for raw in match.group(1).splitlines():
        line = raw.rstrip()
        top = re.match(r"^([A-Za-z][A-Za-z0-9_-]*):(?:\s*(.*))?$", line)
        if top:
            current_key, value = top.group(1), (top.group(2) or "")
            if current_key == "negative":
                metadata[current_key] = negative
            elif current_key in ("flags", "features", "includes"):
                metadata[current_key] = list_values(value)
                if not value.strip():
                    metadata[current_key] = []
            else:
                metadata[current_key] = scalar(value)
            continue
        if current_key == "negative":
            child = re.match(r"^\s+([A-Za-z][A-Za-z0-9_-]*):\s*(.*?)\s*$", line)
            if child:
                negative[child.group(1)] = scalar(child.group(2))
        elif current_key in ("flags", "features", "includes"):
            item = re.match(r"^\s+-\s*(.*?)\s*$", line)
            if item:
                metadata[current_key].append(scalar(item.group(1)))
    return metadata, None


def code_without_comments(source, discard_strings=False):
    """Blank JS comments (and optionally strings) while retaining line positions."""
    chars = list(source)
    state = "code"
    quote = ""
    index = 0
    while index < len(source):
        char = source[index]
        following = source[index + 1] if index + 1 < len(source) else ""
        if state == "code":
            if char == "/" and following == "/":
                chars[index] = chars[index + 1] = " "
                state = "line"
                index += 2
                continue
            if char == "/" and following == "*":
                chars[index] = chars[index + 1] = " "
                state = "block"
                index += 2
                continue
            if char in "\"'`":
                state, quote = "string", char
        elif state == "line":
            if char == "\n":
                state = "code"
            else:
                chars[index] = " "
        elif state == "block":
            if char == "*" and following == "/":
                chars[index] = chars[index + 1] = " "
                state = "code"
                index += 2
                continue
            if char != "\n":
                chars[index] = " "
        elif state == "string":
            if discard_strings and char != "\n":
                chars[index] = " "
            if char == "\\":
                if char != "\n":
                    chars[index] = " "
                if index + 1 < len(source) and source[index + 1] != "\n":
                    chars[index + 1] = " "
                    index += 2
                    continue
            elif char == quote:
                state = "code"
        index += 1
    return "".join(chars)


STATIC_MODULE = re.compile(r"(?m)^[ \t]*(?:import[ \t]+(?!\()|export(?:[ \t]+|\s*\{))")
MODULE_META = re.compile(r"\bimport\s*\.\s*meta\b")
TOP_LEVEL_AWAIT = re.compile(
    r"(?m)^(?:await\b(?!\s*=)|(?:const|let|var)\s+[A-Za-z_$][\w$]*\s*=\s*await\b)"
)
HOST_MODULE = re.compile(r"\b(?:from\s*|import\s*(?:\(\s*)?)(['\"])(qjs:[^'\"]+)\1", re.S)
HUGE_ALLOCATION = re.compile(
    r"(?:new\s+(?:Shared)?ArrayBuffer\s*\(\s*(?:0x80000000|2147483648|2\s*\*\*\s*31)"
    r"|new\s+Uint(?:8|16|32)Array\s*\(\s*(?:0x80000000|2147483648|2\s*\*\*\s*31)"
    r"|(?:\.length|\blength)\s*=\s*(?:0x7fffffff|2147483647|2\s*\*\*\s*31))",
    re.I,
)


def unsupported_reasons(path, source, metadata, metadata_error):
    reasons = []
    if metadata_error:
        reasons.append(metadata_error)

    allowed_metadata = {
        "flags", "negative", "features", "includes", "description", "esid",
        "author", "info", "locale",
    }
    for key in sorted(set(metadata) - allowed_metadata):
        reasons.append(f"unimplemented metadata field {key}")
    if metadata.get("includes"):
        reasons.append("includes metadata requires the upstream test harness")

    flags = metadata.get("flags", [])
    supported_flags = {"module", "noStrict", "raw", "qjs:no-detect-module"}
    for flag in flags:
        if flag == "qjs:set-interrupt-handler":
            pass  # the CLI installs the upstream 150-poll interrupt threshold
        elif flag == "qjs:track-promise-rejections":
            pass  # the CLI checks outstanding rejections after draining jobs
        elif flag == "async":
            reasons.append("async flag requires the upstream async harness")
        elif flag == "CanBlockIsFalse":
            reasons.append("CanBlockIsFalse requires host runtime configuration")
        elif flag == "onlyStrict":
            reasons.append("onlyStrict requires a strict-script CLI mode")
        elif flag not in supported_flags:
            reasons.append(f"unimplemented metadata flag {flag}")

    negative = metadata.get("negative")
    if negative is not None:
        if not isinstance(negative, dict):
            reasons.append("negative metadata must declare phase and type")
        elif not negative.get("phase") or not negative.get("type"):
            reasons.append("negative metadata must declare phase and type")
        elif negative["phase"] not in ("parse", "runtime"):
            reasons.append(f"unimplemented negative phase {negative['phase']}")
        if isinstance(negative, dict) and negative.get("type") and not re.fullmatch(
            r"[A-Za-z_$][A-Za-z0-9_$]*", negative["type"]
        ):
            reasons.append(f"invalid negative error type {negative['type']}")
        if isinstance(negative, dict):
            extra_negative = set(negative) - {"phase", "type"}
            if extra_negative:
                reasons.append(f"unimplemented negative metadata fields: {', '.join(sorted(extra_negative))}")

    for feature in metadata.get("features", []):
        if feature not in config["features"]:
            reasons.append(f"unconfigured tests.conf feature {feature}")
            continue
        setting = config["features"][feature].lower()
        if setting in ("yes", "true", "1", "!tcc"):
            continue
        if setting in ("no", "false", "0", "tcc"):
            reasons.append(f"tests.conf disables feature {feature}")
        else:
            reasons.append(f"unimplemented tests.conf condition for feature {feature}: {setting}")

    code = code_without_comments(source)
    executable_code = code_without_comments(source, discard_strings=True)
    for match in HOST_MODULE.finditer(code):
        if match.group(2) in ("qjs:std", "qjs:os", "qjs:bjson"):
            continue
        reasons.append(f"unsupported host module {match.group(2)}")
    if re.search(r"\b(?:new\s+)?SharedWorker\s*\(", executable_code):
        reasons.append("SharedWorker is unsupported")
    if path.as_posix().endswith("/bug1468.js") or HUGE_ALLOCATION.search(executable_code):
        reasons.append("2 GiB-scale memory stress is unsupported")

    return list(dict.fromkeys(reasons))


def expected_negative(metadata):
    negative = metadata.get("negative")
    if not isinstance(negative, dict):
        return None
    phase, error_type = negative.get("phase"), negative.get("type")
    if phase not in ("parse", "runtime") or not error_type:
        return None
    return {"phase": phase, "type": error_type}


def read_capture(file_obj, limit=8192):
    file_obj.seek(0)
    data = file_obj.read(limit + 1)
    if len(data) > limit:
        return data[:limit].decode("utf-8", "replace") + "\n[diagnostic truncated]"
    return data.decode("utf-8", "replace")


def run_case(path, module, negative, interrupt, track_rejections):
    relative = path.relative_to(tests_root).as_posix()
    argv = [str(cli)]
    if not track_rejections:
        argv.append("--allow-unhandled-rejections")
    if interrupt:
        argv.extend(["--interrupt-after", "150"])
    if module:
        argv.append("-m")
    argv.append(str(path))
    start = time.monotonic()
    with tempfile.TemporaryFile() as stdout_file, tempfile.TemporaryFile() as stderr_file:
        try:
            process = subprocess.Popen(
                argv,
                cwd=str(tests_root),
                stdout=stdout_file,
                stderr=stderr_file,
                start_new_session=True,
            )
        except OSError as error:
            return {
                "status": "fail",
                "reason": f"could not start qjscli: {error}",
                "durationSeconds": round(time.monotonic() - start, 3),
            }
        try:
            return_code = process.wait(timeout=timeout_seconds)
            timed_out = False
        except subprocess.TimeoutExpired:
            timed_out = True
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            return_code = process.wait()
        stdout = read_capture(stdout_file)
        stderr = read_capture(stderr_file)

    result = {
        "status": "fail",
        "returnCode": return_code,
        "durationSeconds": round(time.monotonic() - start, 3),
    }
    if timed_out:
        result["reason"] = f"process exceeded {timeout_seconds:g}s timeout"
        result["diagnostics"] = stderr or stdout
        return result

    if negative:
        if return_code == 0:
            result["reason"] = (
                f"expected {negative['phase']} {negative['type']}, but qjscli exited 0"
            )
            result["diagnostics"] = stderr or stdout
            return result
        markers = []
        for line in stderr.splitlines():
            match = re.search(r"qjscli:(parse|runtime):", line)
            if not match:
                continue
            phase = match.group(1)
            payload = line[match.end():].lstrip(": ")
            for filename in (str(path), relative, path.name):
                if payload.startswith(filename + ":"):
                    payload = payload[len(filename) + 1:].lstrip()
                    break
            class_match = re.match(r"([A-Za-z_$][A-Za-z0-9_$]*)(?::|\b)", payload)
            markers.append({
                "phase": phase,
                "type": class_match.group(1) if class_match else None,
                "line": line,
            })
        if len(markers) == 1 and markers[0]["phase"] == negative["phase"] and markers[0]["type"] == negative["type"]:
            result["status"] = "pass"
            result["reason"] = f"matched {negative['phase']} {negative['type']}"
        else:
            result["reason"] = (
                f"expected {negative['phase']} {negative['type']}; observed "
                + (", ".join(f"{item['phase']} {item['type'] or 'unclassified'}" for item in markers)
                   if markers else "no structured qjscli phase diagnostic")
            )
            result["diagnostics"] = stderr or stdout
    elif return_code == 0:
        result["status"] = "pass"
    else:
        result["reason"] = f"qjscli exited {return_code}"
        result["diagnostics"] = stderr or stdout
    return result


records = []
binary_available = cli.is_file() and os.access(cli, os.X_OK)
if not tests:
    print(f"tests.conf selected no JavaScript files under {testdir}", file=sys.stderr)
    raise SystemExit(2)
for path in tests:
    relative = path.relative_to(tests_root).as_posix()
    source = ""
    try:
        source = path.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as error:
        metadata, metadata_error = {}, None
        reasons = [f"cannot read test source: {error}"]
    else:
        metadata, metadata_error = metadata_for(source)
        reasons = unsupported_reasons(path, source, metadata, metadata_error)

    flags = metadata.get("flags", [])
    module = path.suffix == ".mjs" or "module" in flags
    if "qjs:no-detect-module" not in flags:
        module_code = code_without_comments(source, discard_strings=True)
        if STATIC_MODULE.search(module_code) or MODULE_META.search(module_code) or TOP_LEVEL_AWAIT.search(module_code):
            module = True
    negative = expected_negative(metadata)
    record = {
        "path": relative,
        "status": "unsupported" if reasons else "pending",
        "unsupportedReasons": reasons,
        "module": module,
        "negative": negative,
    }
    if reasons:
        record["reason"] = "; ".join(reasons)
    elif not binary_available:
        record["status"] = "unsupported"
        record["reason"] = "build/qjs/qjscli is missing or not executable"
        record["unsupportedReasons"] = [record["reason"]]
    else:
        record.update(run_case(path, module, negative,
                               "qjs:set-interrupt-handler" in flags,
                               "qjs:track-promise-rejections" in flags))
        record.pop("unsupportedReasons", None)
    records.append(record)
    label = record["status"].upper()
    detail = f" ({record['reason']})" if record.get("reason") else ""
    print(f"{label} {relative}{detail}")

counts = {status: sum(1 for item in records if item["status"] == status)
          for status in ("pass", "fail", "unsupported")}
summary = {
    "schemaVersion": 1,
    "runner": "scripts/qjs-cli-tests.sh",
    "binary": str(cli),
    "testsConf": str(config_path.relative_to(root)),
    "testDirectory": testdir,
    "timeoutSeconds": timeout_seconds,
    "selectedCount": len(records),
    "excludedCount": len(excluded),
    "excludedPaths": sorted(excluded),
    "counts": counts,
    "exitStatus": 0 if counts["fail"] == 0 and counts["unsupported"] == 0 else 1,
    "tests": records,
}
summary_path = out_dir / "qjs-cli-tests.json"
temporary_summary = summary_path.with_suffix(summary_path.suffix + ".tmp")
temporary_summary.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")
temporary_summary.replace(summary_path)
print(
    f"SUMMARY pass={counts['pass']} fail={counts['fail']} "
    f"unsupported={counts['unsupported']} selected={len(records)} excluded={len(excluded)}"
)
print(f"JSON {summary_path}")
raise SystemExit(summary["exitStatus"])
PY
