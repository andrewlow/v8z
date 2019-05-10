// Copyright 2014 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// On z/OS, this test needs a stack size of at least 260 kBytes.
// Flags: --stack-size=260 --allow-natives-syntax

%ScheduleBreak(); // Schedule an interrupt that does not go away.

function f() { f(); }
assertThrows(f, RangeError);

var locals = "";
for (var i = 0; i < 1024; i++) locals += "var v" + i + ";";
eval("function g() {" + locals + "f();}");
assertThrows("g()", RangeError);
