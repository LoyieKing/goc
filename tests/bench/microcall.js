// Microcall workload: tiny functions, no allocation in the timed loop.
//
// The score is calls/ms over the call-shaped cases only (higher is faster).
// arith and propget are controls: they should not move with call-prologue
// changes. Run with either engine:
//
//   qjs --stack-size 16384 tests/bench/microcall.js
//   build/qjs/qjscli --stack-size 16384 tests/bench/microcall.js
//
// Prints one JSON line: MICROCALL {...}

"use strict";

var REPS = 5;

function empty() {
  return 1;
}
function id(x) {
  return x;
}
function six(a, b, c, d, e, f) {
  return a + b + c + d + e + f;
}
function eight(a, b, c, d, e, f, g, h) {
  return a + b + c + d + e + f + g + h;
}
function leaf() {
  return 1;
}
function c() {
  return leaf();
}
function b() {
  return c();
}
function a() {
  return b();
}
function makeClosure(n0) {
  var n = n0;
  return function (x) {
    n += x;
    return n;
  };
}
var closure = makeClosure(0);
function fmut(n) {
  if (n <= 0)
    return 0;
  return gmut(n - 1);
}
function gmut(n) {
  if (n <= 0)
    return 1;
  return fmut(n - 1);
}

var obj = {
  a: 1,
  b: 2,
  c: 3,
  d: 4,
  inc: function () {
    this.a++;
    return this.a;
  }
};

function TCB(id, link) {
  this.id = id;
  this.link = link;
  this.state = 0;
}
TCB.prototype.held = function () {
  return (this.state & 4) != 0;
};
TCB.prototype.run = function () {
  this.state = 1;
  return this.link;
};
var tcbA = new TCB(1, null);
var tcbB = new TCB(2, tcbA);
tcbA.link = tcbB;

function benchArith(n) {
  var s = 0;
  for (var i = 0; i < n; i++)
    s = (s + i) | 0;
  return s;
}
function benchProp(n) {
  var s = 0;
  var o = obj;
  for (var i = 0; i < n; i++)
    s += o.a + o.b + o.c + o.d;
  return s;
}
function benchEmpty(n) {
  var s = 0;
  for (var i = 0; i < n; i++)
    s += empty();
  return s;
}
function benchId(n) {
  var s = 0;
  for (var i = 0; i < n; i++)
    s += id(i);
  return s;
}
function benchSix(n) {
  var s = 0;
  for (var i = 0; i < n; i++)
    s += six(i, 1, 2, 3, 4, 5);
  return s;
}
function benchEight(n) {
  var s = 0;
  for (var i = 0; i < n; i++)
    s += eight(i, 1, 2, 3, 4, 5, 6, 7);
  return s;
}
function benchMethod(n) {
  var s = 0;
  var o = obj;
  for (var i = 0; i < n; i++)
    s += o.inc();
  return s;
}
function benchDepth(n) {
  var s = 0;
  for (var i = 0; i < n; i++)
    s += a();
  return s;
}
function benchClosure(n) {
  var s = 0;
  var f = closure;
  for (var i = 0; i < n; i++)
    s += f(1);
  return s;
}
function benchMutual(n) {
  var s = 0;
  for (var i = 0; i < n; i++)
    s += fmut(4);
  return s;
}
function benchSched(n) {
  var s = 0;
  var cur = tcbA;
  for (var i = 0; i < n; i++) {
    if (!cur.held())
      s += cur.id;
    cur = cur.run();
    cur.state = 0;
  }
  return s;
}

// calls per iteration, used for the score. Mutual recursion is one call per
// level, so n iterations of fmut(n) is about n calls; we time fmut(N) once
// per rep and count N calls.
var CASES = [
  { name: "arith", fn: benchArith, n: 4000000, calls: 0, control: true },
  { name: "propget", fn: benchProp, n: 2000000, calls: 0, control: true },
  { name: "empty", fn: benchEmpty, n: 2000000, calls: 1 },
  { name: "id", fn: benchId, n: 2000000, calls: 1 },
  { name: "six", fn: benchSix, n: 1000000, calls: 1 },
  { name: "eight", fn: benchEight, n: 1000000, calls: 1 },
  { name: "method", fn: benchMethod, n: 1000000, calls: 1 },
  { name: "depth4", fn: benchDepth, n: 500000, calls: 4 },
  { name: "closure", fn: benchClosure, n: 1000000, calls: 1 },
  { name: "mutual", fn: benchMutual, n: 400000, calls: 5 },
  { name: "sched", fn: benchSched, n: 1000000, calls: 2 }
];

function median(xs) {
  var a = xs.slice().sort(function (x, y) { return x - y; });
  return a[(a.length / 2) | 0];
}

function timeCase(c) {
  var samples = [];
  var sink = 0;
  for (var r = 0; r < REPS; r++) {
    var t0 = Date.now();
    sink = c.fn(c.n);
    samples.push(Date.now() - t0);
  }
  if (sink === undefined)
    throw new Error(c.name + " produced no sink");
  var ms = median(samples);
  if (ms < 1)
    ms = 1;
  var callCount = c.calls * c.n;
  return {
    name: c.name,
    ms: ms,
    n: c.n,
    calls: callCount,
    ns: c.calls ? (ms * 1e6) / callCount : 0,
    control: !!c.control,
    sink: sink
  };
}

// One untimed pass so parse and the first compilation sit outside the median.
for (var i = 0; i < CASES.length; i++)
  CASES[i].fn(64);

var out = [];
var scoreCalls = 0;
var scoreMs = 0;
var logScore = 0;
var scoreN = 0;
for (var i = 0; i < CASES.length; i++) {
  var row = timeCase(CASES[i]);
  out.push(row);
  print(row.name + " ms=" + row.ms + " ns/call=" + (row.ns | 0) + " sink=" + row.sink);
  if (!row.control && row.calls > 0 && row.ms > 0) {
    scoreCalls += row.calls;
    scoreMs += row.ms;
    logScore += Math.log(row.calls / row.ms);
    scoreN++;
  }
}
var score = scoreN ? Math.exp(logScore / scoreN) : 0;
print("MICROCALL " + JSON.stringify({
  score: Math.round(score),
  calls: scoreCalls,
  ms: scoreMs,
  cases: out
}));
