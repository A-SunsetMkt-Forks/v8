// Copyright 2020 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Original: differential_fuzz/fake_resource.js
print("I'm a resource.");

// Original: differential_fuzz/fake_resource.js
print("I'm a resource.");

// Original: differential_fuzz/fake_resource.js
print("I'm a resource.");

// Original: differential_fuzz/fake_resource.js
print("I'm a resource.");

// Original: differential_fuzz/fake_resource.js
print("I'm a resource.");

try {
  print("Hash: " + __hash);
  print("Caught: " + __caught);
} catch (e) {}
print("v8-foozzie source: v8/differential_fuzz/input1.js");
// Original: v8/differential_fuzz/input1.js
try {
  var __v_0 = 0;
} catch (e) {}
try {
  __prettyPrintExtra(__v_0);
} catch (e) {}
try {
  print("Hash: " + __hash);
  print("Caught: " + __caught);
  __prettyPrint(__v_0);
} catch (e) {}
print("v8-foozzie source: v8/differential_fuzz/input2.js");
// Original: v8/differential_fuzz/input2.js
let __v_1 = 1;
try {
  print("Hash: " + __hash);
  print("Caught: " + __caught);
  __prettyPrint(__v_0);
  __prettyPrint(__v_1);
} catch (e) {}
