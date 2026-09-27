// Per-instance script for the multi-instance memory probe (qjsmem, the C
// thread harness in tests/qjsmem and scripts/gojamem all evaluate this).
var o = { a: 1, b: [1, 2, 3], c: "x" };
function f(x) { return x * 2 + o.b.length; }
var s = 0;
for (var i = 0; i < 1000; i++) s += f(i);
var arr = [];
for (var j = 0; j < 100; j++) arr.push({ i: j, s: "k" + j });
s + arr.length;
