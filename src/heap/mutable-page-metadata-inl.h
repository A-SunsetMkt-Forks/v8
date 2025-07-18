// Copyright 2020 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_HEAP_MUTABLE_PAGE_METADATA_INL_H_
#define V8_HEAP_MUTABLE_PAGE_METADATA_INL_H_

#include "src/heap/mutable-page-metadata.h"
// Include the non-inl header before the rest of the headers.

#include "src/heap/memory-chunk-metadata-inl.h"
#include "src/heap/spaces-inl.h"
#include "src/sandbox/hardware-support.h"

namespace v8 {
namespace internal {

// static
MutablePageMetadata* MutablePageMetadata::FromAddress(Address a) {
  return cast(MemoryChunkMetadata::FromAddress(a));
}

// static
MutablePageMetadata* MutablePageMetadata::FromAddress(const Isolate* i,
                                                      Address a) {
  return cast(MemoryChunkMetadata::FromAddress(i, a));
}

// static
MutablePageMetadata* MutablePageMetadata::FromHeapObject(Tagged<HeapObject> o) {
  return cast(MemoryChunkMetadata::FromHeapObject(o));
}

// static
MutablePageMetadata* MutablePageMetadata::FromHeapObject(const Isolate* i,
                                                         Tagged<HeapObject> o) {
  return cast(MemoryChunkMetadata::FromHeapObject(i, o));
}

void MutablePageMetadata::IncrementExternalBackingStoreBytes(
    ExternalBackingStoreType type, size_t amount) {
  base::CheckedIncrement(&external_backing_store_bytes_[static_cast<int>(type)],
                         amount);
  owner()->IncrementExternalBackingStoreBytes(type, amount);
}

void MutablePageMetadata::DecrementExternalBackingStoreBytes(
    ExternalBackingStoreType type, size_t amount) {
  base::CheckedDecrement(&external_backing_store_bytes_[static_cast<int>(type)],
                         amount);
  owner()->DecrementExternalBackingStoreBytes(type, amount);
}

void MutablePageMetadata::MoveExternalBackingStoreBytes(
    ExternalBackingStoreType type, MutablePageMetadata* from,
    MutablePageMetadata* to, size_t amount) {
  DCHECK_NOT_NULL(from->owner());
  DCHECK_NOT_NULL(to->owner());
  base::CheckedDecrement(
      &(from->external_backing_store_bytes_[static_cast<int>(type)]), amount);
  base::CheckedIncrement(
      &(to->external_backing_store_bytes_[static_cast<int>(type)]), amount);
  Space::MoveExternalBackingStoreBytes(type, from->owner(), to->owner(),
                                       amount);
}

AllocationSpace MutablePageMetadata::owner_identity() const {
  {
    AllowSandboxAccess temporary_sandbox_access;
    DCHECK_EQ(owner() == nullptr, Chunk()->InReadOnlySpace());
  }
  if (!owner()) return RO_SPACE;
  return owner()->identity();
}

template <AccessMode mode>
void MutablePageMetadata::ClearLiveness() {
  marking_bitmap()->Clear<mode>();
  SetLiveBytes(0);
}

void MutablePageMetadata::SetMajorGCInProgress() {
  SetFlagUnlocked(MemoryChunk::IS_MAJOR_GC_IN_PROGRESS);
}

void MutablePageMetadata::ResetMajorGCInProgress() {
  ClearFlagUnlocked(MemoryChunk::IS_MAJOR_GC_IN_PROGRESS);
}

void MutablePageMetadata::ClearFlagsNonExecutable(
    MemoryChunk::MainThreadFlags flags) {
  return ClearFlagsUnlocked(flags);
}

void MutablePageMetadata::SetFlagsNonExecutable(
    MemoryChunk::MainThreadFlags flags, MemoryChunk::MainThreadFlags mask) {
  return SetFlagsUnlocked(flags, mask);
}

void MutablePageMetadata::ClearFlagNonExecutable(MemoryChunk::Flag flag) {
  return ClearFlagUnlocked(flag);
}

void MutablePageMetadata::SetFlagNonExecutable(MemoryChunk::Flag flag) {
  return SetFlagUnlocked(flag);
}

void MutablePageMetadata::SetFlagsUnlocked(MemoryChunk::MainThreadFlags flags,
                                           MemoryChunk::MainThreadFlags mask) {
  auto& untrusted_main_thread_flags = Chunk()->untrusted_main_thread_flags_;
  untrusted_main_thread_flags =
      (untrusted_main_thread_flags & ~mask) | (flags & mask);
}

void MutablePageMetadata::ClearFlagsUnlocked(
    MemoryChunk::MainThreadFlags flags) {
  auto& untrusted_main_thread_flags = Chunk()->untrusted_main_thread_flags_;
  untrusted_main_thread_flags &= ~flags;
}

void MutablePageMetadata::SetFlagUnlocked(MemoryChunk::Flag flag) {
  auto& untrusted_main_thread_flags = Chunk()->untrusted_main_thread_flags_;
  untrusted_main_thread_flags |= flag;
}

void MutablePageMetadata::ClearFlagUnlocked(MemoryChunk::Flag flag) {
  auto& untrusted_main_thread_flags = Chunk()->untrusted_main_thread_flags_;
  untrusted_main_thread_flags = untrusted_main_thread_flags.without(flag);
}

}  // namespace internal
}  // namespace v8

#endif  // V8_HEAP_MUTABLE_PAGE_METADATA_INL_H_
