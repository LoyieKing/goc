// perf-gap mechanism probes (docs/perf-gap.md): each loop stresses one
// runtime service that the goc build implements differently from native
// (libm bridge, clock, localtime, allocator, memcpy, calls). Prints ns/op.
"use strict";
if (typeof console === "undefined") globalThis.console = { log: print };
const now = (typeof performance !== "undefined" && performance.now)
  ? () => performance.now() : () => Date.now();
// qjscli passes no script arguments: a prelude may set globalThis.__N (work
// multiplier) and globalThis.__ONLY (run one probe), see scripts/perfgap-mech.sh.
const N = globalThis.__N || 1;
const ONLY = globalThis.__ONLY || null;
function bench(name, n, f) {
  if (ONLY && ONLY !== name) return;
  let best = Infinity, r;
  for (let k = 0; k < 5; k++) {
    const t0 = now(); r = f(n * N); const t = now() - t0;
    if (t < best) best = t;
  }
  console.log(name + " " + (best * 1e6 / (n * N)).toFixed(2) + " ns/op");
  return r;
}
let sink = 0;
bench("empty_loop", 2e6, n => { let s = 0; for (let i = 0; i < n; i++) s += i; return s; });
bench("math_abs", 2e6, n => { let s = 0; for (let i = 0; i < n; i++) s += Math.abs(i - 5e5); return s; });
bench("math_floor_dbl", 2e6, n => { let s = 0; for (let i = 0; i < n; i++) s += Math.floor(i * 0.37); return s; });
bench("math_sqrt", 2e6, n => { let s = 0; for (let i = 0; i < n; i++) s += Math.sqrt(i); return s; });
bench("math_sin", 1e6, n => { let s = 0; for (let i = 0; i < n; i++) s += Math.sin(i); return s; });
bench("math_exp", 1e6, n => { let s = 0; for (let i = 0; i < n; i++) s += Math.exp(i * 1e-6); return s; });
bench("math_pow", 1e6, n => { let s = 0; for (let i = 0; i < n; i++) s += Math.pow(1.0001, i & 1023); return s; });
bench("pow_op", 1e6, n => { let s = 0; for (let i = 0; i < n; i++) s += 1.0001 ** (i & 1023); return s; });
bench("math_random", 2e6, n => { let s = 0; for (let i = 0; i < n; i++) s += Math.random(); return s; });
bench("date_now", 5e5, n => { let s = 0; for (let i = 0; i < n; i++) s += Date.now(); return s; });
bench("date_local_fields", 2e5, n => { let s = 0; const d = new Date(2020, 1, 1); for (let i = 0; i < n; i++) { d.setTime(1.6e12 + i * 3.6e6); s += d.getHours(); } return s; });
bench("date_utc_fields", 2e5, n => { let s = 0; const d = new Date(0); for (let i = 0; i < n; i++) { d.setTime(1.6e12 + i * 3.6e6); s += d.getUTCHours(); } return s; });
bench("num_tostring", 5e5, n => { let s = 0; for (let i = 0; i < n; i++) s += (i * 1.1).toString().length; return s; });
bench("parse_float", 5e5, n => { let s = 0; for (let i = 0; i < n; i++) s += parseFloat("3.14159e" + (i & 15)); return s; });
bench("alloc_object", 1e6, n => { let o; for (let i = 0; i < n; i++) o = { a: i, b: i }; return o.a; });
bench("alloc_array8", 5e5, n => { let o; for (let i = 0; i < n; i++) o = [i, i, i, i, i, i, i, i]; return o[0]; });
bench("alloc_closure", 1e6, n => { let f; for (let i = 0; i < n; i++) f = () => i; return f(); });
bench("string_concat_small", 1e6, n => { let s; for (let i = 0; i < n; i++) s = "ab" + i; return s.length; });
bench("string_build_1k", 2e4, n => { let t = 0; for (let i = 0; i < n; i++) { let s = ""; for (let j = 0; j < 64; j++) s += "0123456789abcdef"; t += s.length; } return t; });
bench("array_copy_1k", 1e5, n => { const a = new Array(1024).fill(1); let t = 0; for (let i = 0; i < n; i++) t += a.slice().length; return t; });
bench("typed_set_64k", 2e3, n => { const a = new Uint8Array(65536), b = new Uint8Array(65536); for (let i = 0; i < n; i++) a.set(b); return a[0]; });
bench("string_repeat_64k", 5e3, n => { let t = 0; for (let i = 0; i < n; i++) t += "x".repeat(65536).length; return t; });
bench("string_eq_1k", 2e5, n => { const a = "x".repeat(1023) + "a", b = "x".repeat(1023) + "a"; let s = 0; for (let i = 0; i < n; i++) s += (a === b) ? 1 : 0; return s; });
bench("string_lt_1k", 2e5, n => { const a = "x".repeat(1023) + "a", b = "x".repeat(1023) + "b"; let s = 0; for (let i = 0; i < n; i++) s += (a < b) ? 1 : 0; return s; });
bench("alloc_buf_4k", 1e5, n => { let t = 0; for (let i = 0; i < n; i++) t += new ArrayBuffer(4096).byteLength; return t; });
bench("alloc_arr_200", 1e5, n => { let t = 0; for (let i = 0; i < n; i++) { const a = []; for (let j = 0; j < 200; j++) a.push(j); t += a.length; } return t; });
bench("call_js", 2e6, n => { function f(x) { return x + 1; } let s = 0; for (let i = 0; i < n; i++) s = f(s); return s; });
bench("call_native", 2e6, n => { let s = 0; for (let i = 0; i < n; i++) s += Math.max(i, 3); return s; });
bench("prop_get", 2e6, n => { const o = { x: 1, y: 2, z: 3 }; let s = 0; for (let i = 0; i < n; i++) s += o.y; return s; });
bench("regexp_exec", 2e5, n => { const re = /(\d+)-(\w+)/; let s = 0; for (let i = 0; i < n; i++) s += re.exec("abc 123-xyz def")[1].length; return s; });
bench("json_roundtrip", 5e4, n => { let s = 0; for (let i = 0; i < n; i++) s += JSON.parse(JSON.stringify({ a: i, b: [1, 2, 3], c: "str" })).a; return s; });
bench("gc_churn", 2e5, n => { let keep = []; for (let i = 0; i < n; i++) { keep.push({ i: i, s: "v" + i }); if (keep.length > 1000) keep = []; } return keep.length; });
