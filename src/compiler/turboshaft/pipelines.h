// Copyright 2024 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_COMPILER_TURBOSHAFT_PIPELINES_H_
#define V8_COMPILER_TURBOSHAFT_PIPELINES_H_

#include <optional>

#include "src/codegen/optimized-compilation-info.h"
#include "src/compiler/backend/register-allocator-verifier.h"
#include "src/compiler/basic-block-instrumentor.h"
#include "src/compiler/pipeline-statistics.h"
#include "src/compiler/turbofan-graph-visualizer.h"
#include "src/compiler/turboshaft/block-instrumentation-phase.h"
#include "src/compiler/turboshaft/build-graph-phase.h"
#include "src/compiler/turboshaft/code-elimination-and-simplification-phase.h"
#include "src/compiler/turboshaft/debug-feature-lowering-phase.h"
#include "src/compiler/turboshaft/decompression-optimization-phase.h"
#include "src/compiler/turboshaft/instruction-selection-phase.h"
#include "src/compiler/turboshaft/loop-peeling-phase.h"
#include "src/compiler/turboshaft/loop-unrolling-phase.h"
#include "src/compiler/turboshaft/machine-lowering-phase.h"
#include "src/compiler/turboshaft/optimize-phase.h"
#include "src/compiler/turboshaft/phase.h"
#include "src/compiler/turboshaft/register-allocation-phase.h"
#include "src/compiler/turboshaft/sidetable.h"
#include "src/compiler/turboshaft/store-store-elimination-phase.h"
#include "src/compiler/turboshaft/tracing.h"
#include "src/compiler/turboshaft/turbolev-graph-builder.h"
#include "src/compiler/turboshaft/type-assertions-phase.h"
#include "src/compiler/turboshaft/typed-optimizations-phase.h"

#if V8_ENABLE_WEBASSEMBLY
#include "src/compiler/turboshaft/wasm-in-js-inlining-phase.h"
#endif  // V8_ENABLE_WEBASSEMBLY

namespace v8::internal::compiler::turboshaft {

inline constexpr char kTempZoneName[] = "temp-zone";

#define RUN_MAYBE_ABORT(phase, ...) \
  if (V8_UNLIKELY(!Run<phase>(__VA_ARGS__))) return {};

struct SimplificationAndNormalizationPhase {
  DECL_TURBOSHAFT_PHASE_CONSTANTS(SimplificationAndNormalization)

  void Run(PipelineData* data, Zone* temp_zone);
};

class V8_EXPORT_PRIVATE Pipeline {
 public:
  explicit Pipeline(PipelineData* data) : data_(data) {}

  PipelineData* data() const { return data_; }
  void BeginPhaseKind(const char* phase_kind_name) {
    if (auto statistics = data()->pipeline_statistics()) {
      statistics->BeginPhaseKind(phase_kind_name);
    }
  }
  void EndPhaseKind() {
    if (auto statistics = data()->pipeline_statistics()) {
      statistics->EndPhaseKind();
    }
  }

  template <TurboshaftPhase Phase, typename... Args>
  V8_WARN_UNUSED_RESULT auto Run(Args&&... args) {
    // Setup run scope.
    PhaseScope phase_scope(data_->pipeline_statistics(), Phase::phase_name());
    ZoneWithName<Phase::kPhaseName> temp_zone(data_->zone_stats(),
                                              Phase::phase_name());
    NodeOriginTable::PhaseScope origin_scope(data_->node_origins(),
                                             Phase::phase_name());
#ifdef V8_RUNTIME_CALL_STATS
    RuntimeCallTimerScope runtime_call_timer_scope(data_->runtime_call_stats(),
                                                   Phase::kRuntimeCallCounterId,
                                                   Phase::kCounterMode);
#endif

    Phase phase;
    using result_t =
        decltype(phase.Run(data_, temp_zone, std::forward<Args>(args)...));
    if constexpr (std::is_same_v<result_t, void>) {
      phase.Run(data_, temp_zone, std::forward<Args>(args)...);
      if constexpr (produces_printable_graph<Phase>::value) {
        PrintGraph(temp_zone, Phase::phase_name());
      }
      return !data_->info()->was_cancelled();
    } else {
      static_assert(std::is_same_v<result_t, std::optional<BailoutReason>>);
      auto result = phase.Run(data_, temp_zone, std::forward<Args>(args)...);
      if constexpr (produces_printable_graph<Phase>::value) {
        PrintGraph(temp_zone, Phase::phase_name());
      }
      return data_->info()->was_cancelled() ? BailoutReason::kCancelled
                                            : result;
    }
    UNREACHABLE();
  }

  void PrintGraph(Zone* zone, const char* phase_name) {
    CodeTracer* code_tracer = nullptr;
    if (data_->info()->trace_turbo_graph()) {
      // NOTE: We must not call `GetCodeTracer` if tracing is not enabled,
      // because it may not yet be initialized then and doing so from the
      // background thread is not threadsafe.
      code_tracer = data_->GetCodeTracer();
      DCHECK_NOT_NULL(code_tracer);
    }
    PrintTurboshaftGraph(data_, zone, code_tracer, phase_name);
  }

  void TraceSequence(const char* phase_name) {
    if (info()->trace_turbo_json()) {
      UnparkedScopeIfNeeded scope(data()->broker());
      AllowHandleDereference allow_deref;
      TurboJsonFile json_of(info(), std::ios_base::app);
      json_of
          << "{\"name\":\"" << phase_name << "\",\"type\":\"sequence\""
          << ",\"blocks\":" << InstructionSequenceAsJSON{data()->sequence()}
          << ",\"register_allocation\":{"
          << RegisterAllocationDataAsJSON{*(data()->register_allocation_data()),
                                          *(data()->sequence())}
          << "}},\n";
    }
    if (info()->trace_turbo_graph()) {
      UnparkedScopeIfNeeded scope(data()->broker());
      AllowHandleDereference allow_deref;
      CodeTracer::StreamScope tracing_scope(data()->GetCodeTracer());
      tracing_scope.stream()
          << "----- Instruction sequence " << phase_name << " -----\n"
          << *data()->sequence();
    }
  }

  bool CreateGraphWithMaglev(Linkage* linkage) {
    UnparkedScopeIfNeeded unparked_scope(data_->broker());

    BeginPhaseKind("V8.TFGraphCreation");
    turboshaft::Tracing::Scope tracing_scope(data_->info());
    std::optional<BailoutReason> bailout =
        Run<turboshaft::TurbolevGraphBuildingPhase>(linkage);
    EndPhaseKind();

    if (bailout.has_value()) {
      data_->info()->AbortOptimization(bailout.value());
      return false;
    }

    return true;
  }

  bool CreateGraphFromTurbofan(compiler::TFPipelineData* turbofan_data,
                               Linkage* linkage) {
    CHECK_IMPLIES(!v8_flags.disable_optimizing_compilers, v8_flags.turboshaft);

    UnparkedScopeIfNeeded scope(data_->broker(),
                                v8_flags.turboshaft_trace_reduction ||
                                    v8_flags.turboshaft_trace_emitted);

    turboshaft::Tracing::Scope tracing_scope(data_->info());

    if (std::optional<BailoutReason> bailout =
            Run<turboshaft::BuildGraphPhase>(turbofan_data, linkage)) {
      info()->AbortOptimization(*bailout);
      return false;
    }

    return true;
  }

  bool OptimizeTurboshaftGraph(Linkage* linkage) {
    UnparkedScopeIfNeeded scope(data_->broker(),
                                v8_flags.turboshaft_trace_reduction ||
                                    v8_flags.turboshaft_trace_emitted);

    turboshaft::Tracing::Scope tracing_scope(data_->info());

    BeginPhaseKind("V8.TurboshaftOptimize");

#ifdef V8_ENABLE_WEBASSEMBLY
    // TODO(dlehmann,353475584): Once the Wasm-in-JS TS inlining MVP is feature-
    // complete and cleaned-up, move its reducer into the beginning of the
    // `MachineLoweringPhase` since we can reuse the `DataViewLoweringReducer`
    // there and avoid a separate phase.
    if (v8_flags.turboshaft_wasm_in_js_inlining) {
      RUN_MAYBE_ABORT(turboshaft::WasmInJSInliningPhase);
    }
#endif  // !V8_ENABLE_WEBASSEMBLY

    RUN_MAYBE_ABORT(turboshaft::MachineLoweringPhase);

    if (v8_flags.turboshaft_loop_unrolling) {
      RUN_MAYBE_ABORT(turboshaft::LoopUnrollingPhase);
    }

    if (v8_flags.turbo_store_elimination) {
      RUN_MAYBE_ABORT(turboshaft::StoreStoreEliminationPhase);
    }

    RUN_MAYBE_ABORT(turboshaft::OptimizePhase);

    if (v8_flags.turboshaft_typed_optimizations) {
      RUN_MAYBE_ABORT(turboshaft::TypedOptimizationsPhase);
    }

    if (v8_flags.turboshaft_assert_types) {
      RUN_MAYBE_ABORT(turboshaft::TypeAssertionsPhase);
    }

    // Perform dead code elimination, reduce stack checks, simplify loads on
    // platforms where required, ...
    RUN_MAYBE_ABORT(turboshaft::CodeEliminationAndSimplificationPhase);

#ifdef V8_ENABLE_DEBUG_CODE
    if (V8_UNLIKELY(v8_flags.turboshaft_enable_debug_features)) {
      // This phase has to run very late to allow all previous phases to use
      // debug features.
      RUN_MAYBE_ABORT(turboshaft::DebugFeatureLoweringPhase);
    }
#endif  // V8_ENABLE_DEBUG_CODE

    return true;
  }

  V8_WARN_UNUSED_RESULT bool RunSimplificationAndNormalizationPhase() {
    RUN_MAYBE_ABORT(SimplificationAndNormalizationPhase);
    return true;
  }

  V8_WARN_UNUSED_RESULT bool PrepareForInstructionSelection(
      const ProfileDataFromFile* profile = nullptr) {
    if (V8_UNLIKELY(data()->pipeline_kind() == TurboshaftPipelineKind::kCSA ||
                    data()->pipeline_kind() ==
                        TurboshaftPipelineKind::kTSABuiltin)) {
      if (profile) {
        RUN_MAYBE_ABORT(ProfileApplicationPhase, profile);
      }

      if (v8_flags.reorder_builtins &&
          Builtins::IsBuiltinId(info()->builtin())) {
        UnparkedScopeIfNeeded unparked_scope(data()->broker());
        BasicBlockCallGraphProfiler::StoreCallGraph(info(), data()->graph());
      }

      if (v8_flags.turbo_profiling) {
        UnparkedScopeIfNeeded unparked_scope(data()->broker());

        // Basic block profiling disables concurrent compilation, so handle
        // deref is fine.
        AllowHandleDereference allow_handle_dereference;
        const size_t block_count = data()->graph().block_count();
        BasicBlockProfilerData* profiler_data =
            BasicBlockProfiler::Get()->NewData(block_count);

        // Set the function name.
        profiler_data->SetFunctionName(info()->GetDebugName());
        // Capture the schedule string before instrumentation.
        if (v8_flags.turbo_profiling_verbose) {
          std::ostringstream os;
          os << data()->graph();
          profiler_data->SetSchedule(os);
        }

        info()->set_profiler_data(profiler_data);

        RUN_MAYBE_ABORT(BlockInstrumentationPhase);
      } else {
        // We run an empty copying phase to make sure that we have the same
        // control flow as when taking the profile.
        ZoneWithName<kTempZoneName> temp_zone(data()->zone_stats(),
                                              kTempZoneName);
        CopyingPhase<>::Run(data(), temp_zone);
      }
    }

    // DecompressionOptimization has to run as the last phase because it
    // constructs an (slightly) invalid graph that mixes Tagged and Compressed
    // representations.
    RUN_MAYBE_ABORT(DecompressionOptimizationPhase);

    return Run<SpecialRPOSchedulingPhase>();
  }

  [[nodiscard]] bool SelectInstructions(Linkage* linkage) {
    auto call_descriptor = linkage->GetIncomingDescriptor();

    // Depending on which code path led us to this function, the frame may or
    // may not have been initialized. If it hasn't yet, initialize it now.
    if (!data_->frame()) {
      data_->InitializeFrameData(call_descriptor);
    }

    // Select and schedule instructions covering the scheduled graph.
    CodeTracer* code_tracer = nullptr;
    if (info()->trace_turbo_graph()) {
      // NOTE: We must not call `GetCodeTracer` if tracing is not enabled,
      // because it may not yet be initialized then and doing so from the
      // background thread is not threadsafe.
      code_tracer = data_->GetCodeTracer();
    }

    if (std::optional<BailoutReason> bailout = Run<InstructionSelectionPhase>(
            call_descriptor, linkage, code_tracer)) {
      data_->info()->AbortOptimization(*bailout);
      EndPhaseKind();
      return false;
    }

    return true;

    // TODO(nicohartmann@): We might need to provide this.
    // if (info()->trace_turbo_json()) {
    //   UnparkedScopeIfNeeded scope(turbofan_data->broker());
    //   AllowHandleDereference allow_deref;
    //   TurboCfgFile tcf(isolate());
    //   tcf << AsC1V("CodeGen", turbofan_data->schedule(),
    //                turbofan_data->source_positions(),
    //                turbofan_data->sequence());

    //   std::ostringstream source_position_output;
    //   // Output source position information before the graph is deleted.
    //   if (data_->source_positions() != nullptr) {
    //     data_->source_positions()->PrintJson(source_position_output);
    //   } else {
    //     source_position_output << "{}";
    //   }
    //   source_position_output << ",\n\"nodeOrigins\" : ";
    //   data_->node_origins()->PrintJson(source_position_output);
    //   data_->set_source_position_output(source_position_output.str());
    // }
  }

  V8_WARN_UNUSED_RESULT bool AllocateRegisters(
      CallDescriptor* call_descriptor) {
    BeginPhaseKind("V8.TFRegisterAllocation");

    bool run_verifier = v8_flags.turbo_verify_allocation;

    // Allocate registers.
    const RegisterConfiguration* config = RegisterConfiguration::Default();
    std::unique_ptr<const RegisterConfiguration> restricted_config;
    if (call_descriptor->HasRestrictedAllocatableRegisters()) {
      RegList registers = call_descriptor->AllocatableRegisters();
      DCHECK_LT(0, registers.Count());
      restricted_config.reset(
          RegisterConfiguration::RestrictGeneralRegisters(registers));
      config = restricted_config.get();
    }
    if (!AllocateRegisters(config, call_descriptor, run_verifier)) return false;

    // Verify the instruction sequence has the same hash in two stages.
    VerifyGeneratedCodeIsIdempotent();

    RUN_MAYBE_ABORT(FrameElisionPhase);

    // TODO(mtrofin): move this off to the register allocator.
    bool generate_frame_at_start =
        data_->sequence()->instruction_blocks().front()->must_construct_frame();
    // Optimimize jumps.
    if (v8_flags.turbo_jt) {
      RUN_MAYBE_ABORT(JumpThreadingPhase, generate_frame_at_start);
    }

    EndPhaseKind();

    return !info()->was_cancelled();
  }

  bool MayHaveUnverifiableGraph() const {
    // TODO(nicohartmann): Are there any graph which are still verifiable?
    return true;
  }

  void VerifyGeneratedCodeIsIdempotent() {
    JumpOptimizationInfo* jump_opt = data()->jump_optimization_info();
    if (jump_opt == nullptr) return;

    InstructionSequence* code = data()->sequence();
    int instruction_blocks = code->InstructionBlockCount();
    int virtual_registers = code->VirtualRegisterCount();
    size_t hash_code =
        base::hash_combine(instruction_blocks, virtual_registers);
    for (Instruction* instr : *code) {
      hash_code = base::hash_combine(hash_code, instr->opcode(),
                                     instr->InputCount(), instr->OutputCount());
    }
    for (int i = 0; i < virtual_registers; i++) {
      hash_code = base::hash_combine(hash_code, code->GetRepresentation(i));
    }
    if (jump_opt->is_collecting()) {
      jump_opt->hash_code = hash_code;
    } else {
      CHECK_EQ(hash_code, jump_opt->hash_code);
    }
  }

  V8_WARN_UNUSED_RESULT bool AllocateRegisters(
      const RegisterConfiguration* config, CallDescriptor* call_descriptor,
      bool run_verifier);

  V8_WARN_UNUSED_RESULT bool AssembleCode(Linkage* linkage) {
    BeginPhaseKind("V8.TFCodeGeneration");
    data()->InitializeCodeGenerator(linkage);

    UnparkedScopeIfNeeded unparked_scope(data()->broker());

    RUN_MAYBE_ABORT(AssembleCodePhase);
    if (info()->trace_turbo_json()) {
      TurboJsonFile json_of(info(), std::ios_base::app);
      json_of
          << "{\"name\":\"code generation\"" << ", \"type\":\"instructions\""
          << InstructionStartsAsJSON{&data()->code_generator()->instr_starts()}
          << TurbolizerCodeOffsetsInfoAsJSON{
                 &data()->code_generator()->offsets_info()};
      json_of << "},\n";
    }

    data()->ClearInstructionComponent();
    EndPhaseKind();
    return !info()->was_cancelled();
  }

  MaybeHandle<Code> GenerateCode(CallDescriptor* call_descriptor) {
    Linkage linkage(call_descriptor);
    if (!PrepareForInstructionSelection()) return {};
    if (!SelectInstructions(&linkage)) {
      return MaybeHandle<Code>();
    }
    if (!AllocateRegisters(linkage.GetIncomingDescriptor())) return {};
    if (!AssembleCode(&linkage)) return {};
    return FinalizeCode();
  }

  [[nodiscard]] bool GenerateCode(
      Linkage* linkage, std::shared_ptr<OsrHelper> osr_helper = {},
      JumpOptimizationInfo* jump_optimization_info = nullptr,
      const ProfileDataFromFile* profile = nullptr, int initial_graph_hash = 0);

  OptimizedCompilationInfo* info() { return data_->info(); }

  MaybeIndirectHandle<Code> FinalizeCode(bool retire_broker = true) {
    BeginPhaseKind("V8.TFFinalizeCode");
    if (data_->broker() && retire_broker) {
      data_->broker()->Retire();
    }
    RUN_MAYBE_ABORT(FinalizeCodePhase);

    MaybeIndirectHandle<Code> maybe_code = data_->code();
    IndirectHandle<Code> code;
    if (!maybe_code.ToHandle(&code)) {
      return maybe_code;
    }

    data_->info()->SetCode(code);
    PrintCode(data_->isolate(), code, data_->info());

    // Functions with many inline candidates are sensitive to correct call
    // frequency feedback and should therefore not be tiered up early.
    if (v8_flags.profile_guided_optimization &&
        info()->could_not_inline_all_candidates() &&
        info()->shared_info()->cached_tiering_decision() !=
            CachedTieringDecision::kDelayMaglev) {
      info()->shared_info()->set_cached_tiering_decision(
          CachedTieringDecision::kNormal);
    }

    if (info()->trace_turbo_json()) {
      TurboJsonFile json_of(info(), std::ios_base::app);

      json_of << "{\"name\":\"disassembly\",\"type\":\"disassembly\""
              << BlockStartsAsJSON{&data_->code_generator()->block_starts()}
              << "\"data\":\"";
#ifdef ENABLE_DISASSEMBLER
      std::stringstream disassembly_stream;
      code->Disassemble(nullptr, disassembly_stream, data_->isolate());
      std::string disassembly_string(disassembly_stream.str());
      for (const auto& c : disassembly_string) {
        json_of << AsEscapedUC16ForJSON(c);
      }
#endif  // ENABLE_DISASSEMBLER
      json_of << "\"}\n],\n";
      json_of << "\"nodePositions\":";
      // TODO(nicohartmann): We should try to always provide source positions.
      json_of << (data_->source_position_output().empty()
                      ? "{}"
                      : data_->source_position_output())
              << ",\n";
      JsonPrintAllSourceWithPositions(json_of, data_->info(), data_->isolate());
      if (info()->has_bytecode_array()) {
        json_of << ",\n";
        JsonPrintAllBytecodeSources(json_of, info(), data_->isolate());
      }
      json_of << "\n}";
    }
    if (info()->trace_turbo_json() || info()->trace_turbo_graph()) {
      CodeTracer::StreamScope tracing_scope(data_->GetCodeTracer());
      tracing_scope.stream()
          << "---------------------------------------------------\n"
          << "Finished compiling method " << info()->GetDebugName().get()
          << " using TurboFan" << std::endl;
    }
    EndPhaseKind();
    return code;
  }

  bool CommitDependencies(Handle<Code> code) {
    return data_->depedencies() == nullptr ||
           data_->depedencies()->Commit(code);
  }

 private:
#ifdef DEBUG
  virtual bool IsBuiltinPipeline() const { return false; }
#endif

  PipelineData* data_;
};

class BuiltinPipeline : public Pipeline {
 public:
  explicit BuiltinPipeline(PipelineData* data) : Pipeline(data) {}

  void OptimizeBuiltin();

#ifdef DEBUG
  bool IsBuiltinPipeline() const override { return true; }
#endif
};

#undef RUN_MAYBE_ABORT

}  // namespace v8::internal::compiler::turboshaft

#endif  // V8_COMPILER_TURBOSHAFT_PIPELINES_H_
