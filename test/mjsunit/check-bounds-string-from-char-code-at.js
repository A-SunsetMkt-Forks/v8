// Copyright 2019 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Flags: --allow-natives-syntax --turbofan
// Flags: --no-stress-flush-code

let string = "foobar";

function f(useArrayIndex) {
  let index = useArrayIndex ? '1': '4294967296';
  return string.charCodeAt(index);
}

%PrepareFunctionForOptimization(f);
f(true);

%OptimizeFunctionOnNextCall(f);
f(false);
assertUnoptimized(f);

%PrepareFunctionForOptimization(f);
f(true);

// Might deopt again with oob support since the argument cannot be converted to an index
%OptimizeFunctionOnNextCall(f);
f(false);

%PrepareFunctionForOptimization(f);
f(true);

%OptimizeFunctionOnNextCall(f);
f(true);

// no deopt here
f(false);
assertOptimized(f);
