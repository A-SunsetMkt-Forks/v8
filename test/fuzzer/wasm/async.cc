// Copyright 2017 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <limits.h>
#include <stddef.h>
#include <stdint.h>

#include "include/v8-context.h"
#include "include/v8-exception.h"
#include "include/v8-isolate.h"
#include "include/v8-local-handle.h"
#include "src/execution/isolate-inl.h"
#include "src/wasm/wasm-engine.h"
#include "src/wasm/wasm-module.h"
#include "test/common/wasm/wasm-module-runner.h"
#include "test/fuzzer/fuzzer-support.h"
#include "test/fuzzer/wasm/fuzzer-common.h"

namespace v8::internal {
class WasmModuleObject;
}

namespace v8::internal::wasm::fuzzing {

class AsyncFuzzerResolver : public CompilationResultResolver {
 public:
  AsyncFuzzerResolver(Isolate* isolate, bool* done)
      : isolate_(isolate), done_(done) {}

  void OnCompilationSucceeded(DirectHandle<WasmModuleObject> module) override {
    *done_ = true;
    fuzzing_return_value_ = ExecuteAgainstReference(
        isolate_, module, kDefaultMaxFuzzerExecutedInstructions);
  }

  void OnCompilationFailed(DirectHandle<JSAny> error_reason) override {
    *done_ = true;
    // Keep {fuzzing_return_value_} at -1; while failed compilation is also
    // somewhat interesting, we get this often enough via mutation, so no need
    // to add this to the corpus.
  }

  int fuzzing_return_value() const { return fuzzing_return_value_; }

 private:
  Isolate* isolate_;
  bool* done_;
  int fuzzing_return_value_ = -1;
};

V8_SYMBOL_USED extern "C" int LLVMFuzzerInitialize(int* argc, char*** argv) {
  v8_fuzzer::FuzzerSupport::InitializeFuzzerSupport(argc, argv);
  return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  v8_fuzzer::FuzzerSupport* support = v8_fuzzer::FuzzerSupport::Get();
  v8::Isolate* isolate = support->GetIsolate();

  // Set some more flags.
  v8_flags.wasm_async_compilation = true;
  v8_flags.wasm_max_mem_pages = 32;
  v8_flags.wasm_max_table_size = 100;

  Isolate* i_isolate = reinterpret_cast<Isolate*>(isolate);

  v8::Isolate::Scope isolate_scope(isolate);

  // Clear any exceptions from a prior run.
  if (i_isolate->has_exception()) {
    i_isolate->clear_exception();
  }

  v8::HandleScope handle_scope(isolate);
  v8::Context::Scope context_scope(support->GetContext());

  // We explicitly enable staged/experimental WebAssembly features here to
  // increase fuzzer coverage. For libfuzzer fuzzers it is not possible that the
  // fuzzer enables the flag by itself.
  EnableExperimentalWasmFeatures(isolate);

  TryCatch try_catch(isolate);

  bool done = false;
  auto enabled_features = WasmEnabledFeatures::FromIsolate(i_isolate);
  constexpr const char* kAPIMethodName = "WasmAsyncFuzzer.compile";
  base::OwnedVector<const uint8_t> bytes = base::OwnedCopyOf(data, size);
  std::shared_ptr<AsyncFuzzerResolver> resolver =
      std::make_shared<AsyncFuzzerResolver>(i_isolate, &done);
  GetWasmEngine()->AsyncCompile(i_isolate, enabled_features,
                                CompileTimeImportsForFuzzing(), resolver,
                                std::move(bytes), kAPIMethodName);

  // Wait for the promise to resolve.
  while (!done) {
    support->PumpMessageLoop(platform::MessageLoopBehavior::kWaitForWork);
    isolate->PerformMicrotaskCheckpoint();
  }

  // Differently to fuzzers generating "always valid" wasm modules, also mark
  // invalid modules as interesting to have coverage-guidance for invalid cases.
  return 0;
}

}  // namespace v8::internal::wasm::fuzzing
