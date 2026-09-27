var N = (typeof scriptArgs !== "undefined" && scriptArgs.length > 1) ? +scriptArgs[1] : 0;
var ONLY = globalThis.__ONLY || null;
var reps = {Richards:40, DeltaBlue:40, Crypto:4, RayTrace:10, EarleyBoyer:6, RegExp:2, Splay:6, NavierStokes:4};
var scale = globalThis.__SCALE || 1;
for (var s of BenchmarkSuite.suites) {
  if (ONLY && s.name !== ONLY) continue;
  var t0 = Date.now();
  for (var b of s.benchmarks) {
    b.Setup();
    for (var i = 0; i < reps[s.name]*scale; i++) b.run();
    b.TearDown();
  }
  console.log("FIXED " + s.name + " ms=" + (Date.now()-t0));
}
