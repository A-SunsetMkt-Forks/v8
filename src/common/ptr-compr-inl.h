// Copyright 2018 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_COMMON_PTR_COMPR_INL_H_
#define V8_COMMON_PTR_COMPR_INL_H_

#include "src/common/ptr-compr.h"
// Include the non-inl header before the rest of the headers.

#include "src/common/globals.h"
#include "src/execution/isolate.h"
#include "src/objects/tagged.h"
#include "src/utils/utils.h"

#ifdef V8_ENABLE_SANDBOX
#include "src/sandbox/sandbox.h"
#endif  // V8_ENABLE_SANDBOX

namespace v8 {
namespace internal {

#ifdef V8_COMPRESS_POINTERS

// Always return the global/thread local cage base for isolate
PtrComprCageBase::PtrComprCageBase(const Isolate* isolate)
    : address_(V8HeapCompressionScheme::base()) {}
PtrComprCageBase::PtrComprCageBase(const LocalIsolate* isolate)
    : address_(V8HeapCompressionScheme::base()) {}

//
// V8HeapCompressionSchemeImpl
//

constexpr Address kPtrComprCageBaseMask = ~(kPtrComprCageBaseAlignment - 1);

// static
template <typename Cage>
constexpr Address V8HeapCompressionSchemeImpl<Cage>::GetPtrComprCageBaseAddress(
    Address on_heap_addr) {
  return RoundDown<kPtrComprCageBaseAlignment>(on_heap_addr);
}

// static
template <typename Cage>
Address V8HeapCompressionSchemeImpl<Cage>::GetPtrComprCageBaseAddress(
    PtrComprCageBase cage_base) {
  Address base = cage_base.address();
  V8_ASSUME((base & kPtrComprCageBaseMask) == base);
  base = reinterpret_cast<Address>(V8_ASSUME_ALIGNED(
      reinterpret_cast<void*>(base), kPtrComprCageBaseAlignment));
  return base;
}

// static
template <typename Cage>
void V8HeapCompressionSchemeImpl<Cage>::InitBase(Address base) {
  CHECK_EQ(base, GetPtrComprCageBaseAddress(base));
#if defined(USING_V8_SHARED_PRIVATE) && \
    defined(V8_COMPRESS_POINTERS_IN_MULTIPLE_CAGES)
  Cage::set_base_non_inlined(base);
#else
  Cage::base_ = base;
#endif
}

// static
template <typename Cage>
Address V8HeapCompressionSchemeImpl<Cage>::base() {
#if defined(USING_V8_SHARED_PRIVATE) && \
    defined(V8_COMPRESS_POINTERS_IN_MULTIPLE_CAGES)
  Address base = Cage::base_non_inlined();
#else
  Address base = Cage::base_;
#endif
  // V8_ASSUME_ALIGNED is often not preserved across ptr-to-int casts (i.e. when
  // casting to an Address). To increase our chances we additionally encode the
  // same information in this V8_ASSUME.
  V8_ASSUME((base & kPtrComprCageBaseMask) == base);
  return reinterpret_cast<Address>(V8_ASSUME_ALIGNED(
      reinterpret_cast<void*>(base), kPtrComprCageBaseAlignment));
}

// static
template <typename Cage>
Tagged_t V8HeapCompressionSchemeImpl<Cage>::CompressObject(Address tagged) {
#ifdef V8_COMPRESS_POINTERS
  // Ensure that we do not accidentally compress value from a different cage.
  // Cleared weak reference must be either pure (with empty upper part) or
  // belong to the same cage.
  DCHECK_IMPLIES(
      !HAS_SMI_TAG(tagged) && (tagged != kClearedWeakHeapObjectLower32),
      (tagged & kPtrComprCageBaseMask) == base());
#endif
  return static_cast<Tagged_t>(tagged);
}

// static
template <typename Cage>
constexpr Tagged_t V8HeapCompressionSchemeImpl<Cage>::CompressAny(
    Address tagged) {
  return static_cast<Tagged_t>(tagged);
}

// static
template <typename Cage>
Address V8HeapCompressionSchemeImpl<Cage>::DecompressTaggedSigned(
    Tagged_t raw_value) {
  // For runtime code the upper 32-bits of the Smi value do not matter.
  return static_cast<Address>(raw_value);
}

// static
template <typename Cage>
Address V8HeapCompressionSchemeImpl<Cage>::DecompressTagged(
    Tagged_t raw_value) {
#ifdef V8_COMPRESS_POINTERS
  Address cage_base = base();
#ifdef V8_COMPRESS_POINTERS_IN_MULTIPLE_CAGES
  DCHECK_WITH_MSG(cage_base != kNullAddress,
                  "V8HeapCompressionSchemeImpl::base is not initialized for "
                  "current thread");
#endif  // V8_COMPRESS_POINTERS_IN_MULTIPLE_CAGES
#else
  Address cage_base = GetPtrComprCageBaseAddress(on_heap_addr);
#endif  // V8_COMPRESS_POINTERS
  Address result = cage_base + static_cast<Address>(raw_value);
  V8_ASSUME(static_cast<uint32_t>(result) == raw_value);
  return result;
}

// static
template <typename Cage>
template <typename ProcessPointerCallback>
void V8HeapCompressionSchemeImpl<Cage>::ProcessIntermediatePointers(
    Address raw_value, ProcessPointerCallback callback) {
  // If pointer compression is enabled, we may have random compressed pointers
  // on the stack that may be used for subsequent operations.
  // Extract, decompress and trace both halfwords.
  Address decompressed_low =
      V8HeapCompressionSchemeImpl<Cage>::DecompressTagged(
          static_cast<Tagged_t>(raw_value));
  callback(decompressed_low);
  Address decompressed_high =
      V8HeapCompressionSchemeImpl<Cage>::DecompressTagged(
          static_cast<Tagged_t>(raw_value >> (sizeof(Tagged_t) * CHAR_BIT)));
  callback(decompressed_high);
}

#ifdef V8_EXTERNAL_CODE_SPACE

//
// ExternalCodeCompressionScheme
//

constexpr Address kMinExpectedOSPageSizeMask = ~(kMinExpectedOSPageSize - 1);

// static
Address ExternalCodeCompressionScheme::PrepareCageBaseAddress(
    Address on_heap_addr) {
  return RoundDown<kMinExpectedOSPageSize>(on_heap_addr);
}

// static
Address ExternalCodeCompressionScheme::GetPtrComprCageBaseAddress(
    PtrComprCageBase cage_base) {
  Address base = cage_base.address();
  V8_ASSUME((base & kMinExpectedOSPageSizeMask) == base);
  base = reinterpret_cast<Address>(
      V8_ASSUME_ALIGNED(reinterpret_cast<void*>(base), kMinExpectedOSPageSize));
  return base;
}

// static
void ExternalCodeCompressionScheme::InitBase(Address base) {
  CHECK_EQ(base, PrepareCageBaseAddress(base));
#if defined(USING_V8_SHARED_PRIVATE) && \
    defined(V8_COMPRESS_POINTERS_IN_MULTIPLE_CAGES)
  set_base_non_inlined(base);
#else
  base_ = base;
#endif
}

// static
V8_CONST Address ExternalCodeCompressionScheme::base() {
#if defined(USING_V8_SHARED_PRIVATE) && \
    defined(V8_COMPRESS_POINTERS_IN_MULTIPLE_CAGES)
  Address base = base_non_inlined();
#else
  Address base = base_;
#endif
  // V8_ASSUME_ALIGNED is often not preserved across ptr-to-int casts (i.e. when
  // casting to an Address). To increase our chances we additionally encode the
  // same information in this V8_ASSUME.
  V8_ASSUME((base & kMinExpectedOSPageSizeMask) == base);
  return reinterpret_cast<Address>(
      V8_ASSUME_ALIGNED(reinterpret_cast<void*>(base), kMinExpectedOSPageSize));
}

// static
Tagged_t ExternalCodeCompressionScheme::CompressObject(Address tagged) {
  // Sanity check - the tagged value should belong to this cage.
#ifdef V8_COMPRESS_POINTERS_IN_SHARED_CAGE
  DCHECK_IMPLIES(
      !HAS_SMI_TAG(tagged),
      (base() <= tagged) && (tagged < base() + kPtrComprCageReservationSize));
#endif
  return static_cast<Tagged_t>(tagged);
}

// static
constexpr Tagged_t ExternalCodeCompressionScheme::CompressAny(Address tagged) {
  return static_cast<Tagged_t>(tagged);
}

// static
Address ExternalCodeCompressionScheme::DecompressTagged(Tagged_t raw_value) {
  Address cage_base = base();
#ifdef V8_COMPRESS_POINTERS_IN_MULTIPLE_CAGES
  DCHECK_WITH_MSG(cage_base != kNullAddress,
                  "ExternalCodeCompressionScheme::base is not initialized for "
                  "current thread");
#endif  // V8_COMPRESS_POINTERS_IN_MULTIPLE_CAGES
  V8_ASSUME((cage_base & kMinExpectedOSPageSizeMask) == cage_base);

  Address diff = static_cast<Address>(static_cast<uint32_t>(raw_value)) -
                 static_cast<Address>(static_cast<uint32_t>(cage_base));
  // The cage base value was chosen such that it's less or equal than any
  // pointer in the cage, thus if we got a negative diff then it means that
  // the decompressed value is off by 4GB.
  if (static_cast<intptr_t>(diff) < 0) {
    diff += size_t{4} * GB;
  }
  DCHECK(is_uint32(diff));
  Address result = cage_base + diff;
  DCHECK_EQ(static_cast<uint32_t>(result), raw_value);
  return result;
}

// static
template <typename ProcessPointerCallback>
void ExternalCodeCompressionScheme::ProcessIntermediatePointers(
    Address raw_value, ProcessPointerCallback callback) {
  // If pointer compression is enabled, we may have random compressed pointers
  // on the stack that may be used for subsequent operations.
  // Extract, decompress and trace both halfwords.
  Address decompressed_low = ExternalCodeCompressionScheme::DecompressTagged(
      static_cast<Tagged_t>(raw_value));
  callback(decompressed_low);
  Address decompressed_high = ExternalCodeCompressionScheme::DecompressTagged(
      static_cast<Tagged_t>(raw_value >> (sizeof(Tagged_t) * CHAR_BIT)));
  callback(decompressed_high);
}

#endif  // V8_EXTERNAL_CODE_SPACE

//
// Misc functions.
//

V8_INLINE PtrComprCageBase
GetPtrComprCageBaseFromOnHeapAddress(Address address) {
  return PtrComprCageBase(
      V8HeapCompressionScheme::GetPtrComprCageBaseAddress(address));
}

// Load the main pointer compression cage base.
V8_INLINE PtrComprCageBase GetPtrComprCageBase() {
  return PtrComprCageBase(V8HeapCompressionScheme::base());
}

#else

//
// V8HeapCompressionSchemeImpl
//

// static
template <typename Cage>
constexpr Address V8HeapCompressionSchemeImpl<Cage>::GetPtrComprCageBaseAddress(
    Address on_heap_addr) {
  UNREACHABLE();
  return {};
}

// static
template <typename Cage>
Tagged_t V8HeapCompressionSchemeImpl<Cage>::CompressObject(Address tagged) {
  UNREACHABLE();
}

// static
template <typename Cage>
constexpr Tagged_t V8HeapCompressionSchemeImpl<Cage>::CompressAny(
    Address tagged) {
  UNREACHABLE();
  return {};
}

// static
template <typename Cage>
Address V8HeapCompressionSchemeImpl<Cage>::DecompressTaggedSigned(
    Tagged_t raw_value) {
  UNREACHABLE();
}

// static
template <typename Cage>
Address V8HeapCompressionSchemeImpl<Cage>::DecompressTagged(
    Tagged_t raw_value) {
  UNREACHABLE();
}

// static
template <typename Cage>
template <typename ProcessPointerCallback>
void V8HeapCompressionSchemeImpl<Cage>::ProcessIntermediatePointers(
    Address raw_value, ProcessPointerCallback callback) {
  UNREACHABLE();
}

//
// Misc functions.
//

V8_INLINE constexpr PtrComprCageBase GetPtrComprCageBaseFromOnHeapAddress(
    Address address) {
  return PtrComprCageBase();
}

V8_INLINE PtrComprCageBase GetPtrComprCageBase() { return PtrComprCageBase(); }

#endif  // V8_COMPRESS_POINTERS

V8_INLINE PtrComprCageBase GetPtrComprCageBase(Tagged<HeapObject> object) {
  return GetPtrComprCageBaseFromOnHeapAddress(object.ptr());
}

#ifdef V8_COMPRESS_POINTERS_IN_MULTIPLE_CAGES

PtrComprCageAccessScope::PtrComprCageAccessScope(Isolate* isolate)
    : cage_base_(V8HeapCompressionScheme::base()),
#ifdef V8_EXTERNAL_CODE_SPACE
      code_cage_base_(ExternalCodeCompressionScheme::base()),
#endif  // V8_EXTERNAL_CODE_SPACE
      saved_current_isolate_group_(IsolateGroup::current())
#ifdef V8_ENABLE_SANDBOX
      ,
      saved_current_sandbox_(Sandbox::current())
#endif  // V8_ENABLE_SANDBOX
{
  V8HeapCompressionScheme::InitBase(isolate->cage_base());
#ifdef V8_EXTERNAL_CODE_SPACE
  ExternalCodeCompressionScheme::InitBase(isolate->code_cage_base());
#endif  // V8_EXTERNAL_CODE_SPACE
  IsolateGroup::set_current(isolate->isolate_group());
#ifdef V8_ENABLE_SANDBOX
  Sandbox::set_current(isolate->isolate_group()->sandbox());
#endif  // V8_ENABLE_SANDBOX
}

PtrComprCageAccessScope::~PtrComprCageAccessScope() {
  V8HeapCompressionScheme::InitBase(cage_base_);
#ifdef V8_EXTERNAL_CODE_SPACE
  ExternalCodeCompressionScheme::InitBase(code_cage_base_);
#endif  // V8_EXTERNAL_CODE_SPACE
  IsolateGroup::set_current(saved_current_isolate_group_);
#ifdef V8_ENABLE_SANDBOX
  Sandbox::set_current(saved_current_sandbox_);
#endif  // V8_ENABLE_SANDBOX
}

#endif  // V8_COMPRESS_POINTERS_IN_MULTIPLE_CAGES

}  // namespace internal
}  // namespace v8

#endif  // V8_COMMON_PTR_COMPR_INL_H_
