// Copyright 2017 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Flags: --no-lazy-feedback-allocation --function-context-specialization
// Flags: --verify-heap --invocation-count-for-turbofan=1

function* gen() {
  for (var i = 0; i < 3; ++i) {
    yield i
  }
}
gen().next();
gen().next();
