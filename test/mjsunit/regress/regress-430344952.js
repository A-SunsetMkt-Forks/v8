// Copyright 2025 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

function fun() {
  ev\u0061l('var z = 3;');
  return z;
}

assertEquals(3, fun());
