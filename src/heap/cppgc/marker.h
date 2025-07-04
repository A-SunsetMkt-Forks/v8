// Copyright 2020 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_HEAP_CPPGC_MARKER_H_
#define V8_HEAP_CPPGC_MARKER_H_

#include <memory>

#include "include/cppgc/heap.h"
#include "include/cppgc/platform.h"
#include "include/cppgc/visitor.h"
#include "src/base/macros.h"
#include "src/base/platform/time.h"
#include "src/heap/base/incremental-marking-schedule.h"
#include "src/heap/base/worklist.h"
#include "src/heap/cppgc/concurrent-marker.h"
#include "src/heap/cppgc/globals.h"
#include "src/heap/cppgc/heap-config.h"
#include "src/heap/cppgc/marking-state.h"
#include "src/heap/cppgc/marking-visitor.h"
#include "src/heap/cppgc/marking-worklists.h"
#include "src/heap/cppgc/stats-collector.h"
#include "src/heap/cppgc/task-handle.h"

namespace cppgc {
namespace internal {

class HeapBase;

// Marking algorithm. Example for a valid call sequence creating the marking
// phase:
// 1. StartMarking()
// 2. AdvanceMarkingWithLimits() [Optional, depending on environment.]
// 3. EnterAtomicPause()
// 4. AdvanceMarkingWithLimits() [Optional]
// 5. EnterProcessGlobalAtomicPause()
// 6. AdvanceMarkingWithLimits()
// 7. LeaveAtomicPause()
//
// Alternatively, FinishMarking() combines steps 3.-7.
//
// The marker protects cross-thread roots from being created between 5.-7. This
// currently requires entering a process-global atomic pause.
class V8_EXPORT_PRIVATE MarkerBase {
 public:
  class IncrementalMarkingTask;

  enum class WriteBarrierType {
    kDijkstra,
    kSteele,
  };

  // Pauses concurrent marking if running while this scope is active.
  class PauseConcurrentMarkingScope final {
   public:
    explicit PauseConcurrentMarkingScope(MarkerBase&);
    ~PauseConcurrentMarkingScope();

   private:
    MarkerBase& marker_;
    const bool resume_on_exit_;
  };

  virtual ~MarkerBase();

  MarkerBase(const MarkerBase&) = delete;
  MarkerBase& operator=(const MarkerBase&) = delete;

  template <typename Class>
  Class& To() {
    return *static_cast<Class*>(this);
  }

  // Signals entering the atomic marking pause. The method
  // - stops incremental/concurrent marking;
  // - flushes back any in-construction worklists if needed;
  // - Updates the MarkingConfig if the stack state has changed;
  // - marks local roots
  void EnterAtomicPause(StackState);

  // Enters the process-global pause. The phase marks cross-thread roots and
  // acquires a lock that prevents any cross-thread references from being
  // created.
  //
  // The phase is ended with `LeaveAtomicPause()`.
  void EnterProcessGlobalAtomicPause();

  // Re-enable concurrent marking assuming it isn't enabled yet in GC cycle.
  void ReEnableConcurrentMarking();

  // Makes marking progress. The deadlines are only checked every few objects.
  // - `max_duration`: A maximum marking duration.
  // - `marked_bytes_limit`: Only process this many bytes. Ignored for
  //   processing concurrent bailout objects.
  bool AdvanceMarkingWithLimits(v8::base::TimeDelta max_duration,
                                size_t marked_bytes_limit);

  // Returns the size of the bytes marked in the last invocation of
  // `AdvanceMarkingWithLimits()`.
  size_t last_bytes_marked() const { return last_bytes_marked_; }

  // Signals leaving the atomic marking pause. This method expects no more
  // objects to be marked and merely updates marking states if needed.
  void LeaveAtomicPause();

  // Initialize marking according to the given config. This method will
  // trigger incremental/concurrent marking if needed.
  void StartMarking();

  // Combines:
  // - EnterAtomicPause()
  // - EnterProcessGlobalAtomicPause()
  // - AdvanceMarkingWithLimits()
  // - ProcessWeakness()
  // - LeaveAtomicPause()
  void FinishMarking(StackState);

  void ProcessCrossThreadWeaknessIfNeeded();
  void ProcessWeakness();

  bool JoinConcurrentMarkingIfNeeded();
  void NotifyConcurrentMarkingOfWorkIfNeeded(cppgc::TaskPriority);

  template <WriteBarrierType type>
  inline void WriteBarrierForObject(HeapObjectHeader&);

  HeapBase& heap() { return heap_; }

  cppgc::Visitor& Visitor() { return visitor(); }

  bool IsMarking() const { return is_marking_; }

  void SetMainThreadMarkingDisabledForTesting(bool);
  void WaitForConcurrentMarkingForTesting();
  void ClearAllWorklistsForTesting();
  bool IncrementalMarkingStepForTesting(StackState);

  MarkingWorklists& MarkingWorklistsForTesting() { return marking_worklists_; }
  MutatorMarkingState& MutatorMarkingStateForTesting() {
    return mutator_marking_state_;
  }

 protected:
  class IncrementalMarkingAllocationObserver final
      : public StatsCollector::AllocationObserver {
   public:
    static constexpr size_t kMinAllocatedBytesPerStep = 256 * kKB;

    explicit IncrementalMarkingAllocationObserver(MarkerBase& marker);

    void AllocatedObjectSizeIncreased(size_t delta) final;

   private:
    MarkerBase& marker_;
    size_t current_allocated_size_ = 0;
  };

  using IncrementalMarkingTaskHandle = SingleThreadedHandle;

  static constexpr v8::base::TimeDelta kMaximumIncrementalStepDuration =
      v8::base::TimeDelta::FromMilliseconds(2);

  MarkerBase(HeapBase&, cppgc::Platform*, MarkingConfig);

  virtual cppgc::Visitor& visitor() = 0;
  virtual ConservativeTracingVisitor& conservative_visitor() = 0;
  virtual heap::base::StackVisitor& stack_visitor() = 0;
  virtual ConcurrentMarkerBase& concurrent_marker() = 0;
  virtual heap::base::IncrementalMarkingSchedule& schedule() = 0;

  // Processes the worklists with given deadlines. The deadlines are only
  // checked every few objects.
  // - `time_deadline`: Time deadline that is always respected.
  // - `marked_bytes_deadline`: Only process this many bytes. Ignored for
  //   processing concurrent bailout objects.
  bool ProcessWorklistsWithDeadline(v8::base::TimeTicks time_deadline,
                                    size_t marked_bytes_deadline);
  void AdvanceMarkingWithLimitsEpilogue();

  void VisitLocalRoots(StackState);
  void VisitCrossThreadRoots();

  void MarkNotFullyConstructedObjects();

  virtual void ScheduleIncrementalMarkingTask();

  bool IncrementalMarkingStep(StackState);

  void AdvanceMarkingOnAllocation();
  virtual void AdvanceMarkingOnAllocationImpl();

  void HandleNotFullyConstructedObjects();
  void MarkStrongCrossThreadRoots();

  HeapBase& heap_;
  MarkingConfig config_ = MarkingConfig::Default();
  cppgc::Platform* platform_;
  std::shared_ptr<cppgc::TaskRunner> foreground_task_runner_;
  IncrementalMarkingTaskHandle incremental_marking_handle_;
  IncrementalMarkingAllocationObserver incremental_marking_allocation_observer_;
  MarkingWorklists marking_worklists_;
  MutatorMarkingState mutator_marking_state_;
  size_t last_bytes_marked_ = 0;
  bool is_marking_{false};
  bool main_marking_disabled_for_testing_{false};
  bool visited_cross_thread_persistents_in_atomic_pause_{false};
  bool processed_cross_thread_weakness_{false};
};

class V8_EXPORT_PRIVATE Marker final : public MarkerBase {
 public:
  Marker(HeapBase&, cppgc::Platform*, MarkingConfig = MarkingConfig::Default());

 protected:
  cppgc::Visitor& visitor() final { return marking_visitor_; }

  ConservativeTracingVisitor& conservative_visitor() final {
    return conservative_marking_visitor_;
  }

  heap::base::StackVisitor& stack_visitor() final {
    return conservative_marking_visitor_;
  }

  ConcurrentMarkerBase& concurrent_marker() final { return concurrent_marker_; }

  heap::base::IncrementalMarkingSchedule& schedule() final {
    return *schedule_.get();
  }

 private:
  MutatorMarkingVisitor marking_visitor_;
  ConservativeMarkingVisitor conservative_marking_visitor_;
  std::unique_ptr<heap::base::IncrementalMarkingSchedule> schedule_;
  ConcurrentMarker concurrent_marker_;
};

template <MarkerBase::WriteBarrierType type>
void MarkerBase::WriteBarrierForObject(HeapObjectHeader& header) {
  // The barrier optimizes for the bailout cases:
  // - kDijkstra: Marked objects.
  // - kSteele: Unmarked objects.
  switch (type) {
    case MarkerBase::WriteBarrierType::kDijkstra:
      if (!header.TryMarkAtomic()) {
        return;
      }
      break;
    case MarkerBase::WriteBarrierType::kSteele:
      if (!header.IsMarked<AccessMode::kAtomic>()) {
        return;
      }
      break;
  }

  // The barrier fired. Filter out in-construction objects here. This possibly
  // requires unmarking the object again.
  if (V8_UNLIKELY(header.IsInConstruction<AccessMode::kNonAtomic>())) {
    // In construction objects are traced only if they are unmarked. If marking
    // reaches this object again when it is fully constructed, it will re-mark
    // it and tracing it as a previously not fully constructed object would know
    // to bail out.
    header.Unmark<AccessMode::kAtomic>();
    mutator_marking_state_.not_fully_constructed_worklist()
        .Push<AccessMode::kAtomic>(&header);
    return;
  }

  switch (type) {
    case MarkerBase::WriteBarrierType::kDijkstra:
      mutator_marking_state_.write_barrier_worklist().Push(&header);
      break;
    case MarkerBase::WriteBarrierType::kSteele:
      mutator_marking_state_.retrace_marked_objects_worklist().Push(&header);
      break;
  }
}

}  // namespace internal
}  // namespace cppgc

#endif  // V8_HEAP_CPPGC_MARKER_H_
