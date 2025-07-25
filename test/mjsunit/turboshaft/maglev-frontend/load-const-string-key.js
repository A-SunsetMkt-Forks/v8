// Copyright 2024 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Flags: --allow-natives-syntax --turbolev --turbofan

let o = { "a" : 42, "b": 17, "c": 1 };

function load_const_key(k) {
  return o[k];
}

%PrepareFunctionForOptimization(load_const_key);
assertEquals(42, load_const_key("a"));
%OptimizeFunctionOnNextCall(load_const_key);
assertEquals(42, load_const_key("a"));
assertOptimized(load_const_key);
assertEquals(17, load_const_key("b"));
assertUnoptimized(load_const_key);
var c = "c";
o[c];
assertEquals(1, load_const_key(c));
%OptimizeFunctionOnNextCall(load_const_key);
assertEquals(1, load_const_key(c));
