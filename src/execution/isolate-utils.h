// Copyright 2019 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_EXECUTION_ISOLATE_UTILS_H_
#define V8_EXECUTION_ISOLATE_UTILS_H_

#include "src/common/globals.h"

namespace v8::internal {

class HeapObjectLayout;

// Computes the pointer compression cage base from any read only or writable
// heap object. The resulting value is intended to be used only as a hoisted
// computation of cage base inside trivial accessors for optimizing value
// decompression. When pointer compression is disabled this function always
// returns nullptr.
V8_INLINE PtrComprCageBase GetPtrComprCageBase(Tagged<HeapObject> object);

// Returns true if it succeeded to obtain isolate from given object.
// If it fails then the object is definitely a read-only object but it may also
// succeed for read only objects if pointer compression is enabled.
V8_INLINE bool GetIsolateFromHeapObject(Tagged<HeapObject> object,
                                        Isolate** isolate);

}  // namespace v8::internal

#endif  // V8_EXECUTION_ISOLATE_UTILS_H_
