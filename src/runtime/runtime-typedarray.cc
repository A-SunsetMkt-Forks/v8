// Copyright 2014 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/base/atomicops.h"
#include "src/common/message-template.h"
#include "src/execution/arguments-inl.h"
#include "src/heap/factory.h"
#include "src/objects/elements.h"
#include "src/objects/js-array-buffer-inl.h"
#include "src/objects/objects-inl.h"
#include "src/runtime/runtime-utils.h"
#include "src/runtime/runtime.h"
#include "third_party/fp16/src/include/fp16.h"

namespace v8 {
namespace internal {

RUNTIME_FUNCTION(Runtime_ArrayBufferDetach) {
  HandleScope scope(isolate);
  // This runtime function is exposed in ClusterFuzz and as such has to
  // support arbitrary arguments.
  if (args.length() < 1 || !IsJSArrayBuffer(*args.at(0))) {
    THROW_NEW_ERROR_RETURN_FAILURE(
        isolate, NewTypeError(MessageTemplate::kNotTypedArray));
  }
  auto array_buffer = Cast<JSArrayBuffer>(args.at(0));
  constexpr bool kForceForWasmMemory = false;
  MAYBE_RETURN(JSArrayBuffer::Detach(array_buffer, kForceForWasmMemory,
                                     args.atOrUndefined(isolate, 1)),
               ReadOnlyRoots(isolate).exception());
  return ReadOnlyRoots(isolate).undefined_value();
}

RUNTIME_FUNCTION(Runtime_ArrayBufferSetDetachKey) {
  HandleScope scope(isolate);
  DCHECK_EQ(2, args.length());
  Handle<Object> argument = args.at(0);
  DirectHandle<Object> key = args.at(1);
  // This runtime function is exposed in ClusterFuzz and as such has to
  // support arbitrary arguments.
  if (!IsJSArrayBuffer(*argument)) {
    THROW_NEW_ERROR_RETURN_FAILURE(
        isolate, NewTypeError(MessageTemplate::kNotTypedArray));
  }
  auto array_buffer = Cast<JSArrayBuffer>(argument);
  array_buffer->set_detach_key(*key);
  return ReadOnlyRoots(isolate).undefined_value();
}

RUNTIME_FUNCTION(Runtime_TypedArrayCopyElements) {
  HandleScope scope(isolate);
  DCHECK_EQ(3, args.length());
  DirectHandle<JSTypedArray> target = args.at<JSTypedArray>(0);
  DirectHandle<JSAny> source = args.at<JSAny>(1);
  size_t length;
  CHECK(TryNumberToSize(args[2], &length));
  ElementsAccessor* accessor = target->GetElementsAccessor();
  return accessor->CopyElements(isolate, source, target, length, 0);
}

RUNTIME_FUNCTION(Runtime_TypedArrayGetBuffer) {
  HandleScope scope(isolate);
  DCHECK_EQ(1, args.length());
  DirectHandle<JSTypedArray> holder = args.at<JSTypedArray>(0);
  return *holder->GetBuffer(isolate);
}

RUNTIME_FUNCTION(Runtime_GrowableSharedArrayBufferByteLength) {
  HandleScope scope(isolate);
  DCHECK_EQ(1, args.length());
  DirectHandle<JSArrayBuffer> array_buffer = args.at<JSArrayBuffer>(0);

  CHECK_EQ(0, array_buffer->byte_length_unchecked());
  size_t byte_length = array_buffer->GetBackingStore()->byte_length();
  return *isolate->factory()->NewNumberFromSize(byte_length);
}

namespace {

template <typename T>
bool CompareNum(T x, T y) {
  if (x < y) {
    return true;
  } else if (x > y) {
    return false;
  } else if (!std::is_integral_v<T>) {
    double _x = x, _y = y;
    if (x == 0 && x == y) {
      /* -0.0 is less than +0.0 */
      return std::signbit(_x) && !std::signbit(_y);
    } else if (!std::isnan(_x) && std::isnan(_y)) {
      /* number is less than NaN */
      return true;
    }
  }
  return false;
}

bool LessThanFloat16RawBits(uint16_t x, uint16_t y) {
  return CompareNum(fp16_ieee_to_fp32_value(x), fp16_ieee_to_fp32_value(y));
}

}  // namespace

RUNTIME_FUNCTION(Runtime_TypedArraySortFast) {
  HandleScope scope(isolate);
  DCHECK_EQ(1, args.length());

  // Validation is handled in the Torque builtin.
  DirectHandle<JSTypedArray> array = args.at<JSTypedArray>(0);
  DCHECK(!array->WasDetached());
  DCHECK(!array->IsOutOfBounds());

#ifdef V8_OS_LINUX
  if (v8_flags.multi_mapped_mock_allocator) {
    // Sorting is meaningless with the mock allocator, and std::sort
    // might crash (because aliasing elements violate its assumptions).
    return *array;
  }
#endif

  const size_t byte_length = array->GetByteLength();

  // In case of a SAB, the data is copied into temporary memory, as
  // std::sort might crash in case the underlying data is concurrently
  // modified while sorting.
  CHECK(IsJSArrayBuffer(array->buffer()));
  DirectHandle<JSArrayBuffer> buffer(Cast<JSArrayBuffer>(array->buffer()),
                                     isolate);
  const bool copy_data = buffer->is_shared();

  DirectHandle<ByteArray> array_copy;
  std::vector<uint8_t> offheap_copy;
  void* data_copy_ptr = nullptr;
  if (copy_data) {
    if (byte_length <= static_cast<unsigned>(
                           ByteArray::LengthFor(kMaxRegularHeapObjectSize))) {
      array_copy =
          isolate->factory()->NewByteArray(static_cast<int>(byte_length));
      data_copy_ptr = array_copy->begin();
    } else {
      // Allocate copy in C++ heap.
      offheap_copy.resize(byte_length);
      data_copy_ptr = &offheap_copy[0];
    }
    base::Relaxed_Memcpy(static_cast<base::Atomic8*>(data_copy_ptr),
                         static_cast<base::Atomic8*>(array->DataPtr()),
                         byte_length);
  }

  DisallowGarbageCollection no_gc;

  size_t length = array->GetLength();
  DCHECK_LT(1, length);

  switch (array->type()) {
#define TYPED_ARRAY_SORT(Type, type, TYPE, ctype)                            \
  case kExternal##Type##Array: {                                             \
    ctype* data = copy_data ? reinterpret_cast<ctype*>(data_copy_ptr)        \
                            : static_cast<ctype*>(array->DataPtr());         \
    SBXCHECK(length * sizeof(ctype) == byte_length);                         \
    if (kExternal##Type##Array == kExternalFloat64Array ||                   \
        kExternal##Type##Array == kExternalFloat32Array) {                   \
      if (COMPRESS_POINTERS_BOOL && alignof(ctype) > kTaggedSize) {          \
        /* TODO(ishell, v8:8875): See UnalignedSlot<T> for details. */       \
        std::sort(UnalignedSlot<ctype>(data),                                \
                  UnalignedSlot<ctype>(data + length), CompareNum<ctype>);   \
      } else {                                                               \
        std::sort(data, data + length, CompareNum<ctype>);                   \
      }                                                                      \
    } else if (kExternal##Type##Array == kExternalFloat16Array) {            \
      DCHECK_IMPLIES(COMPRESS_POINTERS_BOOL, alignof(ctype) <= kTaggedSize); \
      std::sort(data, data + length, LessThanFloat16RawBits);                \
    } else {                                                                 \
      if (COMPRESS_POINTERS_BOOL && alignof(ctype) > kTaggedSize) {          \
        /* TODO(ishell, v8:8875): See UnalignedSlot<T> for details. */       \
        std::sort(UnalignedSlot<ctype>(data),                                \
                  UnalignedSlot<ctype>(data + length));                      \
      } else {                                                               \
        std::sort(data, data + length);                                      \
      }                                                                      \
    }                                                                        \
    break;                                                                   \
  }
    TYPED_ARRAYS(TYPED_ARRAY_SORT)
#undef TYPED_ARRAY_SORT
  }

  if (copy_data) {
    DCHECK_NOT_NULL(data_copy_ptr);
    DCHECK_NE(array_copy.is_null(), offheap_copy.empty());
    base::Relaxed_Memcpy(static_cast<base::Atomic8*>(array->DataPtr()),
                         static_cast<base::Atomic8*>(data_copy_ptr),
                         byte_length);
  }

  return *array;
}

RUNTIME_FUNCTION(Runtime_TypedArraySet) {
  HandleScope scope(isolate);
  DCHECK_EQ(4, args.length());
  DirectHandle<JSTypedArray> target = args.at<JSTypedArray>(0);
  DirectHandle<JSAny> source = args.at<JSAny>(1);
  size_t length;
  CHECK(TryNumberToSize(args[2], &length));
  size_t offset;
  CHECK(TryNumberToSize(args[3], &offset));
  ElementsAccessor* accessor = target->GetElementsAccessor();
  return accessor->CopyElements(isolate, source, target, length, offset);
}

RUNTIME_FUNCTION(Runtime_ArrayBufferMaxByteLength) {
  HandleScope shs(isolate);
  size_t heap_max = isolate->array_buffer_allocator()->MaxAllocationSize();
  return *isolate->factory()->NewNumber(heap_max);
}

}  // namespace internal
}  // namespace v8
