// Copyright 2014 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/heap/heap.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <utility>

#include "include/v8-isolate.h"
#include "include/v8-object.h"
#include "src/common/globals.h"
#include "src/flags/flags.h"
#include "src/handles/handles-inl.h"
#include "src/heap/gc-tracer-inl.h"
#include "src/heap/gc-tracer.h"
#include "src/heap/heap-controller.h"
#include "src/heap/heap-layout-inl.h"
#include "src/heap/heap-layout.h"
#include "src/heap/marking-state-inl.h"
#include "src/heap/minor-mark-sweep.h"
#include "src/heap/mutable-page-metadata.h"
#include "src/heap/remembered-set.h"
#include "src/heap/safepoint.h"
#include "src/heap/spaces-inl.h"
#include "src/heap/trusted-range.h"
#include "src/objects/fixed-array.h"
#include "src/objects/free-space-inl.h"
#include "src/objects/internal-index.h"
#include "src/objects/js-array-buffer-inl.h"
#include "src/objects/js-collection-inl.h"
#include "src/sandbox/external-pointer-table.h"
#include "test/unittests/heap/heap-utils.h"
#include "test/unittests/test-utils.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace v8 {
namespace internal {

using HeapTest = TestWithHeapInternalsAndContext;

TEST(Heap, YoungGenerationSizeFromOldGenerationSize) {
  const uint64_t physical_memory = 0;
  const size_t pm = i::Heap::kPointerMultiplier;
  const size_t hlm = i::Heap::HeapLimitMultiplier(physical_memory);
  const size_t max_heap_size = i::Heap::DefaulMaxHeapSize(physical_memory);

  // Low memory
  ASSERT_EQ((v8_flags.minor_ms ? 4 : 3) * 512u * pm * KB,
            i::Heap::YoungGenerationSizeFromOldGenerationSize(physical_memory,
                                                              128u * hlm * MB));
  // High memory
  ASSERT_EQ((i::Heap::DefaultMaxSemiSpaceSize(physical_memory) / 4) *
                (v8_flags.minor_ms ? (2 * 4) : 3),
            i::Heap::YoungGenerationSizeFromOldGenerationSize(
                physical_memory, max_heap_size / 4));
  ASSERT_EQ((i::Heap::DefaultMaxSemiSpaceSize(physical_memory) / 2) *
                (v8_flags.minor_ms ? (2 * 2) : 3),
            i::Heap::YoungGenerationSizeFromOldGenerationSize(
                physical_memory, max_heap_size / 2));
  ASSERT_EQ(i::Heap::DefaultMaxSemiSpaceSize(physical_memory) *
                (v8_flags.minor_ms ? 2 : 3),
            i::Heap::YoungGenerationSizeFromOldGenerationSize(physical_memory,
                                                              max_heap_size));
}

TEST(Heap, GenerationSizesFromHeapSize) {
  const uint64_t physical_memory = 0;
  const size_t pm = i::Heap::kPointerMultiplier;
  const size_t hlm = i::Heap::HeapLimitMultiplier(physical_memory);
  const size_t max_heap_size = i::Heap::DefaulMaxHeapSize(physical_memory);

  size_t old, young;

  // Low memory
  i::Heap::GenerationSizesFromHeapSize(physical_memory, 1 * KB, &young, &old);
  ASSERT_EQ(0u, old);
  ASSERT_EQ(0u, young);

  // On tiny heap max semi space capacity is set to the default capacity which
  // MinorMS does not double.
  i::Heap::GenerationSizesFromHeapSize(
      physical_memory, 1 * KB + (v8_flags.minor_ms ? 2 : 3) * 512u * pm * KB,
      &young, &old);
  ASSERT_EQ(1u * KB, old);
  ASSERT_EQ((v8_flags.minor_ms ? 2 : 3) * 512u * pm * KB, young);

  i::Heap::GenerationSizesFromHeapSize(
      physical_memory,
      128 * hlm * MB + (v8_flags.minor_ms ? 4 : 3) * 512 * pm * KB, &young,
      &old);
  ASSERT_EQ(128u * hlm * MB, old);
  ASSERT_EQ((v8_flags.minor_ms ? 4 : 3) * 512u * pm * KB, young);

  // High memory
  i::Heap::GenerationSizesFromHeapSize(
      physical_memory,
      max_heap_size / 4 +
          (i::Heap::DefaultMaxSemiSpaceSize(physical_memory) / 4) *
              (v8_flags.minor_ms ? (2 * 4) : 3),
      &young, &old);
  ASSERT_EQ(max_heap_size / 4, old);
  ASSERT_EQ((i::Heap::DefaultMaxSemiSpaceSize(physical_memory) / 4) *
                (v8_flags.minor_ms ? (2 * 4) : 3),
            young);

  i::Heap::GenerationSizesFromHeapSize(
      physical_memory,
      max_heap_size / 2 +
          (i::Heap::DefaultMaxSemiSpaceSize(physical_memory) / 2) *
              (v8_flags.minor_ms ? (2 * 2) : 3),
      &young, &old);
  ASSERT_EQ(max_heap_size / 2, old);
  ASSERT_EQ((i::Heap::DefaultMaxSemiSpaceSize(physical_memory) / 2) *
                (v8_flags.minor_ms ? (2 * 2) : 3),
            young);

  i::Heap::GenerationSizesFromHeapSize(
      physical_memory,
      max_heap_size + i::Heap::DefaultMaxSemiSpaceSize(physical_memory) *
                          (v8_flags.minor_ms ? 2 : 3),
      &young, &old);
  ASSERT_EQ(max_heap_size, old);
  ASSERT_EQ(i::Heap::DefaultMaxSemiSpaceSize(physical_memory) *
                (v8_flags.minor_ms ? 2 : 3),
            young);
}

TEST(Heap, HeapSizeFromPhysicalMemory) {
  const uint64_t physical_memory = 0;
  const size_t pm = i::Heap::kPointerMultiplier;
  const size_t hlm = i::Heap::HeapLimitMultiplier(physical_memory);
  const size_t max_heap_size = i::Heap::DefaulMaxHeapSize(physical_memory);

  // The expected value is old_generation_size + semi_space_multiplier *
  // semi_space_size.

  // Low memory
  ASSERT_EQ(128 * hlm * MB + (v8_flags.minor_ms ? 4 : 3) * 512 * pm * KB,
            i::Heap::HeapSizeFromPhysicalMemory(0u));
  ASSERT_EQ(128 * hlm * MB + (v8_flags.minor_ms ? 4 : 3) * 512 * pm * KB,
            i::Heap::HeapSizeFromPhysicalMemory(512u * MB));
  // High memory
  ASSERT_EQ(max_heap_size / 4 +
                (i::Heap::DefaultMaxSemiSpaceSize(physical_memory) / 4) *
                    (v8_flags.minor_ms ? (2 * 4) : 3),
            i::Heap::HeapSizeFromPhysicalMemory(1u * GB));
  ASSERT_EQ(max_heap_size / 2 +
                (i::Heap::DefaultMaxSemiSpaceSize(physical_memory) / 2) *
                    (v8_flags.minor_ms ? (2 * 2) : 3),
            i::Heap::HeapSizeFromPhysicalMemory(2u * GB));
  ASSERT_EQ(
      max_heap_size + i::Heap::DefaultMaxSemiSpaceSize(physical_memory) *
                          (v8_flags.minor_ms ? 2 : 3),
      i::Heap::HeapSizeFromPhysicalMemory(static_cast<uint64_t>(4u) * GB));
  ASSERT_EQ(
      max_heap_size + i::Heap::DefaultMaxSemiSpaceSize(physical_memory) *
                          (v8_flags.minor_ms ? 2 : 3),
      i::Heap::HeapSizeFromPhysicalMemory(static_cast<uint64_t>(8u) * GB));
}

TEST_F(HeapTest, ASLR) {
#if V8_TARGET_ARCH_X64
#if V8_OS_DARWIN
  Heap* heap = i_isolate()->heap();
  std::set<void*> hints;
  for (int i = 0; i < 1000; i++) {
    hints.insert(heap->GetRandomMmapAddr());
  }
  if (hints.size() == 1) {
    EXPECT_TRUE((*hints.begin()) == nullptr);
    EXPECT_TRUE(i::GetRandomMmapAddr() == nullptr);
  } else {
    // It is unlikely that 1000 random samples will collide to less then 500
    // values.
    EXPECT_GT(hints.size(), 500u);
    const uintptr_t kRegionMask = 0xFFFFFFFFu;
    void* first = *hints.begin();
    for (void* hint : hints) {
      uintptr_t diff = reinterpret_cast<uintptr_t>(first) ^
                       reinterpret_cast<uintptr_t>(hint);
      EXPECT_LE(diff, kRegionMask);
    }
  }
#endif  // V8_OS_DARWIN
#endif  // V8_TARGET_ARCH_X64
}

TEST_F(HeapTest, ExternalLimitDefault) {
  Heap* heap = i_isolate()->heap();
  EXPECT_EQ(kExternalAllocationSoftLimit, heap->external_memory_soft_limit());
}

TEST_F(HeapTest, ExternalLimitStaysAboveDefaultForExplicitHandling) {
  v8::ExternalMemoryAccounter accounter;
  accounter.Increase(v8_isolate(), 10 * MB);
  accounter.Decrease(v8_isolate(), 10 * MB);
  Heap* heap = i_isolate()->heap();
  EXPECT_GE(heap->external_memory_soft_limit(), kExternalAllocationSoftLimit);
}

#ifdef V8_COMPRESS_POINTERS
TEST_F(HeapTest, HeapLayout) {
  // Produce some garbage.
  RunJS(
      "let ar = [];"
      "for (let i = 0; i < 100; i++) {"
      "  ar.push(Array(i));"
      "}"
      "ar.push(Array(32 * 1024 * 1024));");

  Address cage_base = i_isolate()->cage_base();
  EXPECT_TRUE(IsAligned(cage_base, size_t{4} * GB));

  Address code_cage_base = i_isolate()->code_cage_base();
  if (V8_EXTERNAL_CODE_SPACE_BOOL) {
    EXPECT_TRUE(IsAligned(code_cage_base, kMinExpectedOSPageSize));
  } else {
    EXPECT_TRUE(IsAligned(code_cage_base, size_t{4} * GB));
  }

#if V8_ENABLE_SANDBOX
  Address trusted_space_base =
      TrustedRange::GetProcessWideTrustedRange()->base();
  EXPECT_TRUE(IsAligned(trusted_space_base, size_t{4} * GB));
  base::AddressRegion trusted_reservation(trusted_space_base, size_t{4} * GB);
#endif

  // Check that all memory chunks belong this region.
  base::AddressRegion heap_reservation(cage_base, size_t{4} * GB);
  base::AddressRegion code_reservation(code_cage_base, size_t{4} * GB);

  IsolateSafepointScope scope(i_isolate()->heap());
  OldGenerationMemoryChunkIterator iter(i_isolate()->heap());
  while (MutablePageMetadata* chunk = iter.next()) {
    Address address = chunk->ChunkAddress();
    size_t size = chunk->area_end() - address;
    AllocationSpace owner_id = chunk->owner_identity();
    if (V8_EXTERNAL_CODE_SPACE_BOOL && IsAnyCodeSpace(owner_id)) {
      EXPECT_TRUE(code_reservation.contains(address, size));
#if V8_ENABLE_SANDBOX
    } else if (IsAnyTrustedSpace(owner_id)) {
      EXPECT_TRUE(trusted_reservation.contains(address, size));
#endif
    } else {
      EXPECT_TRUE(heap_reservation.contains(address, size));
    }
  }
}
#endif  // V8_COMPRESS_POINTERS

namespace {
void ShrinkNewSpace(NewSpace* new_space) {
  if (!v8_flags.minor_ms) {
    new_space->heap()->ReduceNewSpaceSizeForTesting();
    return;
  }
  // MinorMS shrinks the space as part of sweeping. Here we fake a GC cycle, in
  // which we just shrink without marking or sweeping.
  PagedNewSpace* paged_new_space = PagedNewSpace::From(new_space);
  Heap* heap = paged_new_space->heap();
  heap->EnsureSweepingCompleted(
      Heap::SweepingForcedFinalizationMode::kUnifiedHeap);
  GCTracer* tracer = heap->tracer();
  tracer->StartObservablePause(base::TimeTicks::Now());
  tracer->StartCycle(GarbageCollector::MARK_COMPACTOR,
                     GarbageCollectionReason::kTesting, "heap unittest",
                     GCTracer::MarkingType::kAtomic);
  tracer->StartAtomicPause();
  paged_new_space->StartShrinking(paged_new_space->MinimumCapacity());
  for (auto it = paged_new_space->begin();
       it != paged_new_space->end() &&
       (paged_new_space->ShouldReleaseEmptyPage());) {
    PageMetadata* page = *it++;
    if (page->allocated_bytes() == 0) {
      paged_new_space->paged_space()->RemovePageFromSpace(page);
      heap->memory_allocator()->Free(MemoryAllocator::FreeMode::kImmediately,
                                     page);
    } else {
      // The number of live bytes should be zero, because at this point we're
      // after a GC.
      DCHECK_EQ(0, page->live_bytes());
      // We set it to the number of allocated bytes, because FinishShrinking
      // below expects that all pages have been swept and those that remain
      // contain live bytes.
      page->SetLiveBytes(page->allocated_bytes());
    }
  }
  paged_new_space->FinishShrinking();
  for (PageMetadata* page : *paged_new_space) {
    // We reset the number of live bytes to zero, as is expected after a GC.
    page->SetLiveBytes(0);
  }
  tracer->StopAtomicPause();
  tracer->StopObservablePause(GarbageCollector::MARK_COMPACTOR,
                              base::TimeTicks::Now());
  if (heap->cpp_heap()) {
    cppgc::internal::StatsCollector* stats_collector =
        CppHeap::From(heap->cpp_heap())->stats_collector();
    stats_collector->NotifyMarkingStarted(
        cppgc::internal::CollectionType::kMajor,
        cppgc::Heap::MarkingType::kAtomic,
        cppgc::internal::MarkingConfig::IsForcedGC::kNotForced);
    stats_collector->NotifyMarkingCompleted(0);
    stats_collector->NotifySweepingCompleted(
        cppgc::Heap::SweepingType::kAtomic);
  }
  tracer->NotifyFullSweepingCompletedAndStopCycleIfFinished();
}
}  // namespace

TEST_F(HeapTest, GrowAndShrinkNewSpace) {
  if (v8_flags.single_generation) return;
  {
    ManualGCScope manual_gc_scope(i_isolate());
    // Avoid shrinking new space in GC epilogue. This can happen if allocation
    // throughput samples have been taken while executing the benchmark.
    v8_flags.predictable = true;
    v8_flags.stress_concurrent_allocation = false;  // For SimulateFullSpace.
  }
  NewSpace* new_space = heap()->new_space();

  if (heap()->MaxSemiSpaceSize() == heap()->InitialSemiSpaceSize()) {
    return;
  }

  // Make sure we're in a consistent state to start out.
  InvokeMajorGC();
  InvokeMajorGC();
  ShrinkNewSpace(new_space);

  // Explicitly growing should double the space capacity.
  size_t old_capacity, new_capacity;
  old_capacity = new_space->TotalCapacity();
  GrowNewSpaceToMaximumCapacity();
  new_capacity = new_space->TotalCapacity();

  old_capacity = new_space->TotalCapacity();
  {
    v8::HandleScope temporary_scope(reinterpret_cast<v8::Isolate*>(isolate()));
    SimulateFullSpace(new_space);
  }
  new_capacity = new_space->TotalCapacity();
  CHECK_EQ(old_capacity, new_capacity);

  // Explicitly shrinking should not affect space capacity.
  old_capacity = new_space->TotalCapacity();
  ShrinkNewSpace(new_space);
  new_capacity = new_space->TotalCapacity();
  CHECK_EQ(old_capacity, new_capacity);

  // Let the scavenger empty the new space.
  EmptyNewSpaceUsingGC();
  CHECK_LE(new_space->Size(), old_capacity);

  // Explicitly shrinking should halve the space capacity.
  old_capacity = new_space->TotalCapacity();
  ShrinkNewSpace(new_space);
  new_capacity = new_space->TotalCapacity();
  if (v8_flags.minor_ms) {
    // Shrinking may not be able to remove any pages if all contain live
    // objects.
    CHECK_GE(old_capacity, new_capacity);
  } else {
    CHECK_EQ(new_capacity, new_space->MinimumCapacity());
  }

  // Consecutive shrinking should not affect space capacity.
  old_capacity = new_space->TotalCapacity();
  ShrinkNewSpace(new_space);
  ShrinkNewSpace(new_space);
  ShrinkNewSpace(new_space);
  new_capacity = new_space->TotalCapacity();
  CHECK_EQ(old_capacity, new_capacity);
}

TEST_F(HeapTest, CollectingAllAvailableGarbageShrinksNewSpace) {
  if (v8_flags.single_generation) return;
  v8_flags.stress_concurrent_allocation = false;  // For SimulateFullSpace.
  if (heap()->MaxSemiSpaceSize() == heap()->InitialSemiSpaceSize()) {
    return;
  }

  v8::Isolate* iso = reinterpret_cast<v8::Isolate*>(isolate());
  v8::HandleScope scope(iso);
  NewSpace* new_space = heap()->new_space();
  GrowNewSpaceToMaximumCapacity();
  {
    v8::HandleScope temporary_scope(iso);
    SimulateFullSpace(new_space);
  }
  InvokeMemoryReducingMajorGCs();
  CHECK_EQ(new_space->TotalCapacity(), new_space->MinimumCapacity());
}

// Test that HAllocateObject will always return an object in new-space.
TEST_F(HeapTest, OptimizedAllocationAlwaysInNewSpace) {
  if (v8_flags.single_generation) return;
  v8_flags.allow_natives_syntax = true;
  v8_flags.stress_concurrent_allocation = false;  // For SimulateFullSpace.
  if (!isolate()->use_optimizer()) return;
  if (v8_flags.gc_global || v8_flags.stress_compaction ||
      v8_flags.stress_incremental_marking)
    return;
  v8::Isolate* iso = reinterpret_cast<v8::Isolate*>(isolate());
  ManualGCScope manual_gc_scope(isolate());
  v8::HandleScope scope(iso);
  v8::Local<v8::Context> ctx = iso->GetCurrentContext();
  SimulateFullSpace(heap()->new_space());
  AlwaysAllocateScopeForTesting always_allocate(heap());
  v8::Local<v8::Value> res = WithIsolateScopeMixin::RunJS(
      "function c(x) {"
      "  this.x = x;"
      "  for (var i = 0; i < 32; i++) {"
      "    this['x' + i] = x;"
      "  }"
      "}"
      "function f(x) { return new c(x); };"
      "%PrepareFunctionForOptimization(f);"
      "f(1); f(2); f(3);"
      "%OptimizeFunctionOnNextCall(f);"
      "f(4);");

  CHECK_EQ(4, res.As<v8::Object>()
                  ->GetRealNamedProperty(ctx, NewString("x"))
                  .ToLocalChecked()
                  ->Int32Value(ctx)
                  .FromJust());

  i::DirectHandle<JSReceiver> o =
      v8::Utils::OpenDirectHandle(*v8::Local<v8::Object>::Cast(res));

  CHECK(HeapLayout::InYoungGeneration(*o));
}

namespace {
template <RememberedSetType direction>
static size_t GetRememberedSetSize(Tagged<HeapObject> obj) {
  size_t count = 0;
  auto chunk = MutablePageMetadata::FromHeapObject(obj);
  RememberedSet<direction>::Iterate(
      chunk,
      [&count](MaybeObjectSlot slot) {
        count++;
        return KEEP_SLOT;
      },
      SlotSet::KEEP_EMPTY_BUCKETS);
  return count;
}
}  // namespace

TEST_F(HeapTest, RememberedSet_InsertOnPromotingObjectToOld) {
  if (v8_flags.single_generation || v8_flags.stress_incremental_marking) return;
  v8_flags.stress_concurrent_allocation = false;  // For SealCurrentObjects.
  v8_flags.scavenger_precise_object_pinning = false;
  ManualGCScope manual_gc_scope(isolate());
  Factory* factory = isolate()->factory();
  Heap* heap = isolate()->heap();
  SealCurrentObjects();
  HandleScope handle_scope(isolate());

  // Create a young object and age it one generation inside the new space.
  IndirectHandle<FixedArray> arr = factory->NewFixedArray(1);
  std::vector<Handle<FixedArray>> handles;
  if (v8_flags.minor_ms) {
    NewSpace* new_space = heap->new_space();
    CHECK_NE(new_space->TotalCapacity(), new_space->MaximumCapacity());
    // Fill current pages to force MinorMS to promote them.
    SimulateFullSpace(new_space, &handles);
    IsolateSafepointScope scope(heap);
    // New empty pages should remain in new space.
    GrowNewSpaceToMaximumCapacity();
  }
  InvokeMinorGC();
  CHECK(HeapLayout::InYoungGeneration(*arr));

  // Add into 'arr' a reference to an object one generation younger.
  {
    HandleScope scope_inner(isolate());
    DirectHandle<Object> number = factory->NewHeapNumber(42);
    arr->set(0, *number);
  }

  // Promote 'arr' into old, its element is still in new, the old to new
  // refs are inserted into the remembered sets during GC.
  {
    // CSS prevents promoting objects to old space.
    DisableConservativeStackScanningScopeForTesting no_stack_scanning(heap);
    InvokeMinorGC();
  }
  heap->EnsureSweepingCompleted(Heap::SweepingForcedFinalizationMode::kV8Only);

  CHECK(heap->InOldSpace(*arr));
  CHECK(HeapLayout::InYoungGeneration(arr->get(0)));
  if (v8_flags.minor_ms) {
    CHECK_EQ(1, GetRememberedSetSize<OLD_TO_NEW_BACKGROUND>(*arr));
  } else {
    CHECK_EQ(1, GetRememberedSetSize<OLD_TO_NEW>(*arr));
  }
}

TEST_F(HeapTest, Regress978156) {
  if (!v8_flags.incremental_marking) return;
  if (v8_flags.single_generation) return;
  ManualGCScope manual_gc_scope(isolate());

  HandleScope handle_scope(isolate());
  Heap* heap = isolate()->heap();

  // 1. Ensure that the new space is empty.
  EmptyNewSpaceUsingGC();
  // 2. Fill the new space with FixedArrays.
  std::vector<Handle<FixedArray>> arrays;
  SimulateFullSpace(heap->new_space(), &arrays);
  // 3. Trim the last array by one word thus creating a one-word filler.
  DirectHandle<FixedArray> last = arrays.back();
  CHECK_GT(last->length(), 0);
  heap->RightTrimArray(*last, last->length() - 1, last->length());
  // 4. Get the last filler on the page.
  Tagged<HeapObject> filler = HeapObject::FromAddress(
      MutablePageMetadata::FromHeapObject(*last)->area_end() - kTaggedSize);
  HeapObject::FromAddress(last->address() + last->Size());
  CHECK(IsFiller(filler));
  // 5. Start incremental marking.
  i::IncrementalMarking* marking = heap->incremental_marking();
  if (marking->IsStopped()) {
    IsolateSafepointScope scope(heap);
    heap->tracer()->StartCycle(
        GarbageCollector::MARK_COMPACTOR, GarbageCollectionReason::kTesting,
        "collector cctest", GCTracer::MarkingType::kIncremental);
    marking->Start(GarbageCollector::MARK_COMPACTOR,
                   i::GarbageCollectionReason::kTesting);
  }
  // 6. Mark the filler black to access its two markbits. This triggers
  // an out-of-bounds access of the marking bitmap in a bad case.
  heap->marking_state()->TryMarkAndAccountLiveBytes(filler);
}

TEST_F(HeapTest, SemiSpaceNewSpaceGrowsDuringFullGCIncrementalMarking) {
  if (!v8_flags.incremental_marking) return;
  if (v8_flags.single_generation) return;
  if (v8_flags.minor_ms) return;
  ManualGCScope manual_gc_scope(isolate());

  HandleScope handle_scope(isolate());
  Heap* heap = isolate()->heap();

  // 1. Record gc_count and last scavenger epoch.
  auto gc_count = heap->gc_count();
  auto last_scavenger_epoch =
      heap->tracer()->CurrentEpoch(GCTracer::Scope::ScopeId::SCAVENGER);
  // 2. Fill the new space with FixedArrays.
  std::vector<Handle<FixedArray>> arrays;
  SimulateFullSpace(heap->new_space(), &arrays);
  CHECK_EQ(0, heap->new_space()->Available());
  AllocationResult failed_allocation = heap->allocator()->AllocateRaw(
      2 * kTaggedSize, AllocationType::kYoung, AllocationOrigin::kRuntime);
  EXPECT_TRUE(failed_allocation.IsFailure());
  // 3. Start incremental marking.
  i::IncrementalMarking* marking = heap->incremental_marking();
  CHECK(marking->IsStopped());
  {
    IsolateSafepointScope scope(heap);
    heap->tracer()->StartCycle(GarbageCollector::MARK_COMPACTOR,
                               GarbageCollectionReason::kTesting, "tesing",
                               GCTracer::MarkingType::kIncremental);
    marking->Start(GarbageCollector::MARK_COMPACTOR,
                   i::GarbageCollectionReason::kTesting);
  }
  // 4. Allocate in new space.
  AllocationResult allocation = heap->allocator()->AllocateRaw(
      2 * kTaggedSize, AllocationType::kYoung, AllocationOrigin::kRuntime);
  EXPECT_FALSE(allocation.IsFailure());
  // 5. Allocation should succeed without triggering a GC.
  EXPECT_EQ(gc_count, heap->gc_count());
  EXPECT_EQ(last_scavenger_epoch,
            heap->tracer()->CurrentEpoch(GCTracer::Scope::ScopeId::SCAVENGER));
}

#ifdef V8_ENABLE_ALLOCATION_TIMEOUT
namespace {
struct RandomGCIntervalTestSetter {
  RandomGCIntervalTestSetter() {
    static constexpr int kInterval = 87;
    v8_flags.random_gc_interval = kInterval;
  }
  ~RandomGCIntervalTestSetter() { v8_flags.random_gc_interval = 0; }
};

struct HeapTestWithRandomGCInterval : RandomGCIntervalTestSetter, HeapTest {};
}  // namespace

TEST_F(HeapTestWithRandomGCInterval, AllocationTimeout) {
  if (v8_flags.stress_incremental_marking) return;
  if (v8_flags.stress_concurrent_allocation) return;

  auto* allocator = heap()->allocator();

  // Invoke major GC to cause the timeout to be updated.
  InvokeMajorGC();
  const int initial_allocation_timeout =
      allocator->get_allocation_timeout_for_testing().value_or(0);
  ASSERT_GT(initial_allocation_timeout, 0);

  for (int i = 0; i < initial_allocation_timeout - 1; ++i) {
    AllocationResult allocation = allocator->AllocateRaw(
        2 * kTaggedSize, AllocationType::kYoung, AllocationOrigin::kRuntime);
    EXPECT_FALSE(allocation.IsFailure());
  }

  // The last allocation must fail.
  AllocationResult allocation = allocator->AllocateRaw(
      2 * kTaggedSize, AllocationType::kYoung, AllocationOrigin::kRuntime);
  EXPECT_TRUE(allocation.IsFailure());
}
#endif  // V8_ENABLE_ALLOCATION_TIMEOUT

namespace {
struct CompactionDisabler {
  CompactionDisabler() : was_enabled_(v8_flags.compact) {
    v8_flags.compact = false;
  }
  ~CompactionDisabler() {
    if (was_enabled_) {
      v8_flags.compact = true;
    }
  }
  const bool was_enabled_;
};
}  // namespace

TEST_F(HeapTest, BlackAllocatedPages) {
  if (!v8_flags.black_allocated_pages) return;
  if (!v8_flags.incremental_marking) return;

  // Disable compaction to test that the FreeListCategories of black allocated
  // pages are not reset.
  CompactionDisabler disable_compaction;

  Isolate* iso = isolate();
  ManualGCScope manual_gc_scope(iso);

  auto in_free_list = [](PageMetadata* page, Address address) {
    bool found = false;
    page->ForAllFreeListCategories(
        [address, &found](FreeListCategory* category) {
          category->IterateNodesForTesting(
              [address, &found](Tagged<FreeSpace> node) {
                if (!found) found = node.address() == address;
              });
        });
    return found;
  };

  Heap* heap = iso->heap();
  SimulateFullSpace(heap->old_space());

  // Allocate an object on a new page.
  HandleScope scope(iso);
  DirectHandle<FixedArray> arr =
      iso->factory()->NewFixedArray(1, AllocationType::kOld);
  Address next = arr->address() + arr->Size();

  // Assert that the next address is in the lab.
  const Address lab_top = heap->allocator()->old_space_allocator()->top();
  ASSERT_EQ(lab_top, next);

  auto* page = PageMetadata::FromAddress(next);
  const size_t wasted_before_incremental_marking_start = page->wasted_memory();

  heap->StartIncrementalMarking(
      GCFlag::kNoFlags, GarbageCollectionReason::kTesting,
      GCCallbackFlags::kNoGCCallbackFlags, GarbageCollector::MARK_COMPACTOR);

  // Expect the free-space object is created.
  auto freed = HeapObject::FromAddress(next);
  EXPECT_TRUE(IsFreeSpaceOrFiller(freed));

  // The free-space object must be accounted as wasted.
  EXPECT_EQ(wasted_before_incremental_marking_start + freed->Size(),
            page->wasted_memory());

  // Check that the free-space object is not in freelist.
  EXPECT_FALSE(in_free_list(page, next));

  // The page allocated before incremental marking is not black.
  EXPECT_FALSE(page->Chunk()->IsFlagSet(MemoryChunk::BLACK_ALLOCATED));

  // Allocate a new object on a BLACK_ALLOCATED page.
  arr = iso->factory()->NewFixedArray(1, AllocationType::kOld);
  next = arr->address() + arr->Size();

  // Expect the page to be black.
  page = PageMetadata::FromHeapObject(*arr);
  EXPECT_TRUE(page->Chunk()->IsFlagSet(MemoryChunk::BLACK_ALLOCATED));

  // Invoke GC.
  InvokeMajorGC();

  // The page is not black now.
  EXPECT_FALSE(page->Chunk()->IsFlagSet(MemoryChunk::BLACK_ALLOCATED));
  // After the GC the next free-space object must be in freelist.
  EXPECT_TRUE(in_free_list(page, next));
}

TEST_F(HeapTest, ContainsSlow) {
  Isolate* iso = isolate();
  ManualGCScope manual_gc_scope(iso);

  Heap* heap = iso->heap();
  SimulateFullSpace(heap->old_space());

  // Allocate an object on a new page.
  HandleScope scope(iso);
  DirectHandle<FixedArray> arr =
      iso->factory()->NewFixedArray(1, AllocationType::kOld);
  CHECK(heap->old_space()->ContainsSlow(arr->address()));
  CHECK(heap->old_space()->ContainsSlow(
      MemoryChunk::FromAddress(arr->address())->address()));
  CHECK(!heap->old_space()->ContainsSlow(0));

  DirectHandle<FixedArray> large_arr = iso->factory()->NewFixedArray(
      kMaxRegularHeapObjectSize + 1, AllocationType::kOld);
  CHECK(heap->lo_space()->ContainsSlow(large_arr->address()));
  CHECK(heap->lo_space()->ContainsSlow(
      MemoryChunk::FromAddress(large_arr->address())->address()));
  CHECK(!heap->lo_space()->ContainsSlow(0));
}

TEST_F(
    HeapTest,
    ConservativePinningScavengerDoesntMoveObjectReachableFromStackNoPromotion) {
  if (v8_flags.single_generation) return;
  if (v8_flags.minor_ms) return;
  v8_flags.scavenger_conservative_object_pinning = true;
  v8_flags.scavenger_precise_object_pinning = false;
  v8_flags.scavenger_promote_quarantined_pages = false;
  ManualGCScope manual_gc_scope(isolate());

  IndirectHandle<HeapObject> number =
      isolate()->factory()->NewHeapNumber<AllocationType::kYoung>(42);

  // The conservative stack visitor will find this on the stack, so `number`
  // will not move during GCs with stack.
  Address number_address = number->address();

  CHECK(HeapLayout::InYoungGeneration(*number));

  for (int i = 0; i < 10; i++) {
    InvokeMinorGC();
    CHECK(HeapLayout::InYoungGeneration(*number));
    CHECK_EQ(number_address, number->address());
  }

  // `number` is already in the intermediate generation. A stackless GC should
  // now evacuate the object to the old generation.
  {
    DisableConservativeStackScanningScopeForTesting no_stack_scanning(
        isolate()->heap());
    InvokeMinorGC();
  }
  CHECK(!HeapLayout::InYoungGeneration(*number));
  CHECK_NE(number_address, number->address());
}

TEST_F(HeapTest,
       ConservativePinningScavengerDoesntMoveObjectReachableFromStack) {
  if (v8_flags.single_generation) return;
  if (v8_flags.minor_ms) return;
  v8_flags.scavenger_conservative_object_pinning = true;
  v8_flags.scavenger_precise_object_pinning = false;
  v8_flags.scavenger_promote_quarantined_pages = true;
  ManualGCScope manual_gc_scope(isolate());

  IndirectHandle<HeapObject> number =
      isolate()->factory()->NewHeapNumber<AllocationType::kYoung>(42);

  // The conservative stack visitor will find this on the stack, so `number`
  // will not move during a GC with stack.
  Address number_address = number->address();

  CHECK(HeapLayout::InYoungGeneration(*number));

  InvokeMinorGC();
  CHECK(HeapLayout::InYoungGeneration(*number));
  CHECK_EQ(number_address, number->address());

  // `number` is already in the intermediate generation. Another GC should
  // now promote the page to the old generation, again not moving the object.
  InvokeMinorGC();
  CHECK(!HeapLayout::InYoungGeneration(*number));
  CHECK_EQ(number_address, number->address());
}

TEST_F(HeapTest, ConservativePinningScavengerObjectWithSelfReference) {
  if (v8_flags.single_generation) return;
  if (v8_flags.minor_ms) return;
  v8_flags.scavenger_conservative_object_pinning = true;
  ManualGCScope manual_gc_scope(isolate());

  static constexpr int kArraySize = 10;
  DirectHandle<FixedArray> array =
      isolate()->factory()->NewFixedArray(kArraySize, AllocationType::kYoung);
  CHECK(HeapLayout::InYoungGeneration(*array));

  for (int i = 0; i < kArraySize; i++) {
    array->set(i, *array);
  }

  InvokeMinorGC();
}

TEST_F(HeapTest,
       PrecisePinningScavengerDoesntMoveObjectReachableFromHandlesNoPromotion) {
  if (v8_flags.single_generation) return;
  if (v8_flags.minor_ms) return;
  v8_flags.scavenger_precise_object_pinning = true;
  v8_flags.scavenger_conservative_object_pinning = false;
  v8_flags.scavenger_promote_quarantined_pages = false;
  ManualGCScope manual_gc_scope(isolate());

  v8::HandleScope handle_scope(reinterpret_cast<v8::Isolate*>(isolate()));
  IndirectHandle<HeapObject> number =
      isolate()->factory()->NewHeapNumber<AllocationType::kYoung>(42);

  Address number_address = number->address();

  CHECK(HeapLayout::InYoungGeneration(*number));

  for (int i = 0; i < 10; i++) {
    InvokeMinorGC();
    CHECK(HeapLayout::InYoungGeneration(*number));
    CHECK_EQ(number_address, number->address());
  }
}

TEST_F(HeapTest, PrecisePinningScavengerDoesntMoveObjectReachableFromHandles) {
  if (v8_flags.single_generation) return;
  if (v8_flags.minor_ms) return;
  v8_flags.scavenger_precise_object_pinning = true;
  v8_flags.scavenger_conservative_object_pinning = false;
  v8_flags.scavenger_promote_quarantined_pages = true;
  ManualGCScope manual_gc_scope(isolate());

  v8::HandleScope handle_scope(reinterpret_cast<v8::Isolate*>(isolate()));
  IndirectHandle<HeapObject> number =
      isolate()->factory()->NewHeapNumber<AllocationType::kYoung>(42);

  Address number_address = number->address();

  CHECK(HeapLayout::InYoungGeneration(*number));

  InvokeMinorGC();
  CHECK(HeapLayout::InYoungGeneration(*number));
  CHECK_EQ(number_address, number->address());

  // `number` is already in the intermediate generation. Another GC should
  // now move it to old gen.
  InvokeMinorGC();
  CHECK(!HeapLayout::InYoungGeneration(*number));
  CHECK_EQ(number_address, number->address());
}

TEST_F(HeapTest,
       PrecisePinningFullGCDoesntMoveYoungObjectReachableFromHandles) {
  if (v8_flags.single_generation) return;
  v8_flags.precise_object_pinning = true;
  ManualGCScope manual_gc_scope(isolate());

  v8::HandleScope handle_scope(reinterpret_cast<v8::Isolate*>(isolate()));
  IndirectHandle<HeapObject> number =
      isolate()->factory()->NewHeapNumber<AllocationType::kYoung>(42);

  Address number_address = number->address();

  CHECK(HeapLayout::InYoungGeneration(*number));
  InvokeMajorGC();
  CHECK(!HeapLayout::InYoungGeneration(*number));

  CHECK_EQ(number_address, number->address());
}

TEST_F(HeapTest, PrecisePinningFullGCDoesntMoveOldObjectReachableFromHandles) {
  v8_flags.precise_object_pinning = true;
  v8_flags.manual_evacuation_candidates_selection = true;
  ManualGCScope manual_gc_scope(isolate());

  SimulateFullSpace(isolate()->heap()->old_space());

  v8::HandleScope handle_scope(reinterpret_cast<v8::Isolate*>(isolate()));
  IndirectHandle<HeapObject> number =
      isolate()->factory()->NewHeapNumber<AllocationType::kOld>(42);

  CHECK(!HeapLayout::InYoungGeneration(*number));

  Address number_address = number->address();

  i::MutablePageMetadata::FromHeapObject(*number)
      ->set_forced_evacuation_candidate_for_testing(true);

  InvokeMajorGC();

  CHECK_EQ(number_address, number->address());
}

TEST_F(
    HeapTest,
    ConservativePinningScopeScavengeDoesntMoveObjectReachableFromStackNoPromotion) {  // NOLINT(whitespace/line_length)
  if (v8_flags.single_generation) return;
  if (v8_flags.minor_ms) return;
  v8_flags.scavenger_conservative_object_pinning = false;
  v8_flags.scavenger_precise_object_pinning = false;
  v8_flags.scavenger_promote_quarantined_pages = false;
  ManualGCScope manual_gc_scope(isolate());

  ConservativePinningScope conservative_pinning_scope(isolate()->heap());

  IndirectHandle<HeapObject> number =
      isolate()->factory()->NewHeapNumber<AllocationType::kYoung>(42);

  // The conservative stack visitor will find this on the stack, so `number`
  // will not move during GCs with stack.
  Address number_address = number->address();

  CHECK(HeapLayout::InYoungGeneration(*number));

  for (int i = 0; i < 10; i++) {
    InvokeMinorGC();
    CHECK(HeapLayout::InYoungGeneration(*number));
    CHECK_EQ(number_address, number->address());
  }
}

TEST_F(HeapTest,
       ConservativePinningScopeScavengeDoesntMoveObjectReachableFromStack) {
  if (v8_flags.single_generation) return;
  if (v8_flags.minor_ms) return;
  v8_flags.scavenger_conservative_object_pinning = false;
  v8_flags.scavenger_precise_object_pinning = false;
  v8_flags.scavenger_promote_quarantined_pages = true;
  ManualGCScope manual_gc_scope(isolate());

  ConservativePinningScope conservative_pinning_scope(isolate()->heap());

  IndirectHandle<HeapObject> number =
      isolate()->factory()->NewHeapNumber<AllocationType::kYoung>(42);

  // The conservative stack visitor will find this on the stack, so `number`
  // will not move during a GC with stack.
  Address number_address = number->address();

  CHECK(HeapLayout::InYoungGeneration(*number));

  InvokeMinorGC();
  CHECK(HeapLayout::InYoungGeneration(*number));
  CHECK_EQ(number_address, number->address());

  // `number` is already in the intermediate generation. Another GC should
  // now promote the page to the old generation, again not moving the object.
  InvokeMinorGC();
  CHECK(!HeapLayout::InYoungGeneration(*number));
  CHECK_EQ(number_address, number->address());
}

TEST_F(HeapTest,
       ConservativePinningScopeMarkCompactDoesntMoveObjectReachableFromStack) {
  v8_flags.manual_evacuation_candidates_selection = true;
  v8_flags.compact_with_stack = true;
  ManualGCScope manual_gc_scope(isolate());

  SimulateFullSpace(isolate()->heap()->old_space());

  ConservativePinningScope conservative_pinning_scope(isolate()->heap());

  IndirectHandle<HeapObject> number =
      isolate()->factory()->NewHeapNumber<AllocationType::kOld>(42);

  // The conservative stack visitor will find this on the stack, so `number`
  // will not move during a GC with stack.
  Address number_address = number->address();

  for (int i = 0; i < 10; i++) {
    i::MutablePageMetadata::FromHeapObject(*number)
        ->set_forced_evacuation_candidate_for_testing(true);
    InvokeMajorGC();
    CHECK_EQ(number_address, number->address());
  }
}

TEST_F(HeapTest, ScavengerDoesntStrongifyWeakGlobals) {
  if (v8_flags.single_generation) return;
  if (v8_flags.minor_ms) return;
  v8_flags.scavenger_conservative_object_pinning = false;
  v8_flags.scavenger_precise_object_pinning = false;
  ManualGCScope manual_gc_scope(isolate());
  Factory* factory = isolate()->factory();
  v8::Isolate* iso = reinterpret_cast<v8::Isolate*>(isolate());

  // Make sure weak Globals are not strongified by Scavenger. If this ever
  // changes the CHECK in this block should fail.
  v8::Global<v8::String> global;
  {
    v8::HandleScope handle_scope(iso);
    IndirectHandle<String> str =
        factory->NewStringFromStaticChars("should be reclaimed");
    global.Reset(iso, Utils::ToLocal(str));
    global.SetWeak();
  }
  CHECK(!global.IsEmpty());
  InvokeMinorGC();
  CHECK(global.IsEmpty());
}

namespace {
template <typename GCCallback>
void ConservativePinningScopeScavengeRetainsObjectReachableFromStack(
    HeapTest* test, AllocationType allocation_type, GCCallback gc_callback) {
  v8_flags.scavenger_conservative_object_pinning = false;
  v8_flags.scavenger_precise_object_pinning = false;
  v8_flags.scavenger_promote_quarantined_pages = false;
  ManualGCScope manual_gc_scope(test->isolate());
  Factory* factory = test->isolate()->factory();
  v8::Isolate* iso = reinterpret_cast<v8::Isolate*>(test->isolate());

  ConservativePinningScope conservative_pinning_scope(test->isolate()->heap());

  // The conservative stack visitor will find this on the stack, so `object`
  // will not move during GCs with stack.
  Tagged<String> object;
  // Use a `Global` to check whether `object` is actually retained. There should
  // be no other references to it.
  v8::Global<v8::String> global;
  {
    v8::HandleScope handle_scope(iso);
    IndirectHandle<String> str =
        factory->NewStringFromStaticChars("test", allocation_type);
    // Set a weak Global reference to `str` to check that it isn't reclaimed.
    // Scavenger should not strongify weak Globals.
    global.Reset(iso, Utils::ToLocal(str));
    global.SetWeak();
    object = *str;
  }

  CHECK(!global.IsEmpty());
  CHECK(global.IsWeak());
  gc_callback(object);
  CHECK(global.IsWeak());
  CHECK(!global.IsEmpty());
  // Make sure `object` isn't optimized away.
  CHECK_EQ(*Utils::OpenDirectHandle(*global.Get(iso)), object);
}
}  // namespace

TEST_F(HeapTest,
       ConservativePinningScopeScavengeRetainsObjectReachableFromStack) {
  if (v8_flags.single_generation) return;
  if (v8_flags.minor_ms) return;
  ConservativePinningScopeScavengeRetainsObjectReachableFromStack(
      this, AllocationType::kYoung, [this](Tagged<String> object) {
        CHECK(HeapLayout::InYoungGeneration(object));
        InvokeMinorGC();
        CHECK(HeapLayout::InYoungGeneration(object));
      });
}

TEST_F(HeapTest,
       ConservativePinningScopeMarkCompactRetainsObjectReachableFromStack) {
  ConservativePinningScopeScavengeRetainsObjectReachableFromStack(
      this, AllocationType::kOld,
      [this](Tagged<String> object) { InvokeMajorGC(); });
}

}  // namespace internal
}  // namespace v8
