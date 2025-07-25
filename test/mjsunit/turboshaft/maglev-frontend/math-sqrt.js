// Copyright 2025 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Flags: --allow-natives-syntax --turbolev --turbofan

function mysqrt(x) {
  return Math.sqrt(x);
}

%PrepareFunctionForOptimization(mysqrt);
assertEquals(3, mysqrt(9));
%OptimizeFunctionOnNextCall(mysqrt);
assertEquals(3, mysqrt(9));
assertOptimized(mysqrt);

let o = {
  valueOf: function() {
    %DeoptimizeFunction(mysqrt);
    return 16;
  }
};

assertEquals(4, mysqrt(o));
assertUnoptimized(mysqrt);
assertEquals(4, mysqrt(o));
