// Copyright 2015 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/wasm/wasm-js.h"

#include <cinttypes>
#include <cstring>
#include <optional>

#include "include/v8-function.h"
#include "include/v8-persistent-handle.h"
#include "include/v8-promise.h"
#include "include/v8-wasm.h"
#include "src/api/api-inl.h"
#include "src/api/api-natives.h"
#include "src/base/fpu.h"
#include "src/base/logging.h"
#include "src/execution/execution.h"
#include "src/execution/isolate.h"
#include "src/execution/messages.h"
#include "src/flags/flags.h"
#include "src/handles/handles.h"
#include "src/heap/factory.h"
#include "src/objects/fixed-array.h"
#include "src/objects/instance-type.h"
#include "src/objects/js-function.h"
#include "src/objects/managed-inl.h"
#include "src/objects/objects-inl.h"
#include "src/objects/shared-function-info.h"
#include "src/objects/templates.h"
#include "src/wasm/function-compiler.h"
#include "src/wasm/streaming-decoder.h"
#include "src/wasm/value-type.h"
#include "src/wasm/wasm-debug.h"
#include "src/wasm/wasm-engine.h"
#include "src/wasm/wasm-limits.h"
#include "src/wasm/wasm-objects-inl.h"
#include "src/wasm/wasm-serialization.h"
#include "src/wasm/wasm-value.h"

namespace v8 {

using i::wasm::AddressType;
using i::wasm::CompileTimeImport;
using i::wasm::CompileTimeImports;
using i::wasm::ErrorThrower;
using i::wasm::WasmEnabledFeatures;

namespace internal {

// Note: The implementation of this function is in runtime-wasm.cc, in order
// to be able to use helpers that aren't visible outside that file.
void ToUtf8Lossy(Isolate* isolate, DirectHandle<String> string,
                 std::string& out);

}  // namespace internal

class WasmStreaming::WasmStreamingImpl {
 public:
  WasmStreamingImpl(
      i::Isolate* isolate, const char* api_method_name,
      CompileTimeImports compile_imports,
      std::shared_ptr<internal::wasm::CompilationResultResolver> resolver)
      : i_isolate_(isolate),
        enabled_features_(WasmEnabledFeatures::FromIsolate(i_isolate_)),
        streaming_decoder_(i::wasm::GetWasmEngine()->StartStreamingCompilation(
            i_isolate_, enabled_features_, std::move(compile_imports),
            direct_handle(i_isolate_->context(), i_isolate_), api_method_name,
            resolver)),
        resolver_(std::move(resolver)) {}

  void OnBytesReceived(const uint8_t* bytes, size_t size) {
    streaming_decoder_->OnBytesReceived(base::VectorOf(bytes, size));
  }
  void Finish(bool can_use_compiled_module) {
    streaming_decoder_->Finish(can_use_compiled_module);
  }

  void Abort(i::MaybeHandle<i::JSAny> exception) {
    i::HandleScope scope(i_isolate_);
    streaming_decoder_->Abort();

    // If no exception value is provided, we do not reject the promise. This can
    // happen when streaming compilation gets aborted when no script execution
    // is allowed anymore, e.g. when a browser tab gets refreshed.
    if (exception.is_null()) return;

    resolver_->OnCompilationFailed(exception.ToHandleChecked());
  }

  bool SetCompiledModuleBytes(base::Vector<const uint8_t> bytes) {
    if (!i::wasm::IsSupportedVersion(bytes, enabled_features_)) return false;
    streaming_decoder_->SetCompiledModuleBytes(bytes);
    return true;
  }

  void SetMoreFunctionsCanBeSerializedCallback(
      std::function<void(CompiledWasmModule)> callback) {
    streaming_decoder_->SetMoreFunctionsCanBeSerializedCallback(
        [callback = std::move(callback),
         url = streaming_decoder_->shared_url()](
            const std::shared_ptr<i::wasm::NativeModule>& native_module) {
          callback(CompiledWasmModule{native_module, url->data(), url->size()});
        });
  }

  void SetUrl(base::Vector<const char> url) { streaming_decoder_->SetUrl(url); }

 private:
  i::Isolate* const i_isolate_;
  const WasmEnabledFeatures enabled_features_;
  const std::shared_ptr<internal::wasm::StreamingDecoder> streaming_decoder_;
  const std::shared_ptr<internal::wasm::CompilationResultResolver> resolver_;
};

WasmStreaming::WasmStreaming(std::unique_ptr<WasmStreamingImpl> impl)
    : impl_(std::move(impl)) {
  TRACE_EVENT0("v8.wasm", "wasm.InitializeStreaming");
}

// The destructor is defined here because we have a unique_ptr with forward
// declaration.
WasmStreaming::~WasmStreaming() = default;

void WasmStreaming::OnBytesReceived(const uint8_t* bytes, size_t size) {
  TRACE_EVENT1("v8.wasm", "wasm.OnBytesReceived", "bytes", size);
  impl_->OnBytesReceived(bytes, size);
}

void WasmStreaming::Finish(bool can_use_compiled_module) {
  TRACE_EVENT0("v8.wasm", "wasm.FinishStreaming");
  impl_->Finish(can_use_compiled_module);
}

void WasmStreaming::Abort(MaybeLocal<Value> exception) {
  TRACE_EVENT0("v8.wasm", "wasm.AbortStreaming");
  i::MaybeHandle<i::JSAny> maybe_exception;
  if (!exception.IsEmpty()) {
    maybe_exception =
        Cast<i::JSAny>(Utils::OpenHandle(*exception.ToLocalChecked()));
  }
  impl_->Abort(maybe_exception);
}

bool WasmStreaming::SetCompiledModuleBytes(const uint8_t* bytes, size_t size) {
  TRACE_EVENT0("v8.wasm", "wasm.SetCompiledModuleBytes");
  return impl_->SetCompiledModuleBytes(base::VectorOf(bytes, size));
}

void WasmStreaming::SetMoreFunctionsCanBeSerializedCallback(
    std::function<void(CompiledWasmModule)> callback) {
  impl_->SetMoreFunctionsCanBeSerializedCallback(std::move(callback));
}

void WasmStreaming::SetUrl(const char* url, size_t length) {
  DCHECK_EQ('\0', url[length]);  // {url} is null-terminated.
  TRACE_EVENT1("v8.wasm", "wasm.SetUrl", "url", url);
  impl_->SetUrl(base::VectorOf(url, length));
}

// static
std::shared_ptr<WasmStreaming> WasmStreaming::Unpack(Isolate* isolate,
                                                     Local<Value> value) {
  TRACE_EVENT0("v8.wasm", "wasm.WasmStreaming.Unpack");
  i::HandleScope scope(reinterpret_cast<i::Isolate*>(isolate));
  auto managed =
      i::Cast<i::Managed<WasmStreaming>>(Utils::OpenDirectHandle(*value));
  return managed->get();
}

namespace {

i::DirectHandle<i::String> v8_str(i::Isolate* isolate, const char* str) {
  return isolate->factory()->NewStringFromAsciiChecked(str);
}
Local<String> v8_str(Isolate* isolate, const char* str) {
  return Utils::ToLocal(v8_str(reinterpret_cast<i::Isolate*>(isolate), str));
}

#define GET_FIRST_ARGUMENT_AS(Type)                                      \
  i::MaybeDirectHandle<i::Wasm##Type##Object> GetFirstArgumentAs##Type(  \
      const v8::FunctionCallbackInfo<v8::Value>& info,                   \
      ErrorThrower* thrower) {                                           \
    i::DirectHandle<i::Object> arg0 = Utils::OpenDirectHandle(*info[0]); \
    if (!IsWasm##Type##Object(*arg0)) {                                  \
      thrower->TypeError("Argument 0 must be a WebAssembly." #Type);     \
      return {};                                                         \
    }                                                                    \
    return i::Cast<i::Wasm##Type##Object>(arg0);                         \
  }

GET_FIRST_ARGUMENT_AS(Module)
GET_FIRST_ARGUMENT_AS(Tag)

#undef GET_FIRST_ARGUMENT_AS

base::Vector<const uint8_t> GetFirstArgumentAsBytes(
    const v8::FunctionCallbackInfo<v8::Value>& info, size_t max_length,
    ErrorThrower* thrower, bool* is_shared) {
  const uint8_t* start = nullptr;
  size_t length = 0;
  v8::Local<v8::Value> source = info[0];
  if (source->IsArrayBuffer()) {
    // A raw array buffer was passed.
    Local<ArrayBuffer> buffer = Local<ArrayBuffer>::Cast(source);
    auto backing_store = buffer->GetBackingStore();

    start = reinterpret_cast<const uint8_t*>(backing_store->Data());
    length = backing_store->ByteLength();
    *is_shared = buffer->IsSharedArrayBuffer();
  } else if (source->IsTypedArray()) {
    // A TypedArray was passed.
    Local<TypedArray> array = Local<TypedArray>::Cast(source);
    Local<ArrayBuffer> buffer = array->Buffer();

    auto backing_store = buffer->GetBackingStore();

    start = reinterpret_cast<const uint8_t*>(backing_store->Data()) +
            array->ByteOffset();
    length = array->ByteLength();
    *is_shared = buffer->IsSharedArrayBuffer();
  } else {
    thrower->TypeError("Argument 0 must be a buffer source");
    return {};
  }
  DCHECK_IMPLIES(length, start != nullptr);
  if (length == 0) {
    thrower->CompileError("BufferSource argument is empty");
    return {};
  }
  if (length > max_length) {
    // The spec requires a CompileError for implementation-defined limits, see
    // https://webassembly.github.io/spec/js-api/index.html#limits.
    thrower->CompileError("buffer source exceeds maximum size of %zu (is %zu)",
                          max_length, length);
    return {};
  }

  return base::VectorOf(start, length);
}

base::OwnedVector<const uint8_t> GetAndCopyFirstArgumentAsBytes(
    const v8::FunctionCallbackInfo<v8::Value>& info, size_t max_length,
    ErrorThrower* thrower) {
  bool is_shared = false;
  base::Vector<const uint8_t> bytes =
      GetFirstArgumentAsBytes(info, max_length, thrower, &is_shared);
  if (bytes.empty()) {
    return {};
  }

  // Use relaxed reads (and writes, which is unnecessary here) to avoid TSan
  // reports in case the buffer is shared and is being modified concurrently.
  auto result = base::OwnedVector<uint8_t>::NewForOverwrite(bytes.size());
  base::Relaxed_Memcpy(reinterpret_cast<base::Atomic8*>(result.begin()),
                       reinterpret_cast<const base::Atomic8*>(bytes.data()),
                       bytes.size());
  return result;
}

namespace {
i::MaybeDirectHandle<i::JSReceiver> ImportsAsMaybeReceiver(Local<Value> ffi) {
  if (ffi->IsUndefined()) return {};

  Local<Object> obj = Local<Object>::Cast(ffi);
  return i::Cast<i::JSReceiver>(v8::Utils::OpenDirectHandle(*obj));
}

// This class resolves the result of WebAssembly.compile. It just places the
// compilation result in the supplied {promise}.
class AsyncCompilationResolver : public i::wasm::CompilationResultResolver {
 public:
  AsyncCompilationResolver(Isolate* isolate, Local<Context> context,
                           Local<Promise::Resolver> promise_resolver)
      : isolate_(isolate),
        context_(isolate, context),
        promise_resolver_(isolate, promise_resolver) {
    context_.SetWeak();
    promise_resolver_.AnnotateStrongRetainer(kGlobalPromiseHandle);
  }

  void OnCompilationSucceeded(
      i::DirectHandle<i::WasmModuleObject> result) override {
    if (finished_) return;
    finished_ = true;
    if (context_.IsEmpty()) return;
    auto callback = reinterpret_cast<i::Isolate*>(isolate_)
                        ->wasm_async_resolve_promise_callback();
    CHECK(callback);
    callback(isolate_, context_.Get(isolate_), promise_resolver_.Get(isolate_),
             Utils::ToLocal(i::Cast<i::Object>(result)),
             WasmAsyncSuccess::kSuccess);
  }

  void OnCompilationFailed(i::DirectHandle<i::JSAny> error_reason) override {
    if (finished_) return;
    finished_ = true;
    if (context_.IsEmpty()) return;
    auto callback = reinterpret_cast<i::Isolate*>(isolate_)
                        ->wasm_async_resolve_promise_callback();
    CHECK(callback);
    callback(isolate_, context_.Get(isolate_), promise_resolver_.Get(isolate_),
             Utils::ToLocal(error_reason), WasmAsyncSuccess::kFail);
  }

 private:
  static constexpr char kGlobalPromiseHandle[] =
      "AsyncCompilationResolver::promise_";
  bool finished_ = false;
  Isolate* isolate_;
  Global<Context> context_;
  Global<Promise::Resolver> promise_resolver_;
};

constexpr char AsyncCompilationResolver::kGlobalPromiseHandle[];

// This class resolves the result of WebAssembly.instantiate(module, imports).
// It just places the instantiation result in the supplied {promise}.
class InstantiateModuleResultResolver
    : public i::wasm::InstantiationResultResolver {
 public:
  InstantiateModuleResultResolver(Isolate* isolate, Local<Context> context,
                                  Local<Promise::Resolver> promise_resolver)
      : isolate_(isolate),
        context_(isolate, context),
        promise_resolver_(isolate, promise_resolver) {
    context_.SetWeak();
    promise_resolver_.AnnotateStrongRetainer(kGlobalPromiseHandle);
  }

  void OnInstantiationSucceeded(
      i::DirectHandle<i::WasmInstanceObject> instance) override {
    if (context_.IsEmpty()) return;
    auto callback = reinterpret_cast<i::Isolate*>(isolate_)
                        ->wasm_async_resolve_promise_callback();
    CHECK(callback);
    callback(isolate_, context_.Get(isolate_), promise_resolver_.Get(isolate_),
             Utils::ToLocal(i::Cast<i::Object>(instance)),
             WasmAsyncSuccess::kSuccess);
  }

  void OnInstantiationFailed(i::DirectHandle<i::JSAny> error_reason) override {
    if (context_.IsEmpty()) return;
    auto callback = reinterpret_cast<i::Isolate*>(isolate_)
                        ->wasm_async_resolve_promise_callback();
    CHECK(callback);
    callback(isolate_, context_.Get(isolate_), promise_resolver_.Get(isolate_),
             Utils::ToLocal(error_reason), WasmAsyncSuccess::kFail);
  }

 private:
  static constexpr char kGlobalPromiseHandle[] =
      "InstantiateModuleResultResolver::promise_";
  Isolate* isolate_;
  Global<Context> context_;
  Global<Promise::Resolver> promise_resolver_;
};

constexpr char InstantiateModuleResultResolver::kGlobalPromiseHandle[];

// This class resolves the result of WebAssembly.instantiate(bytes, imports).
// For that it creates a new {JSObject} which contains both the provided
// {WasmModuleObject} and the resulting {WebAssemblyInstanceObject} itself.
class InstantiateBytesResultResolver
    : public i::wasm::InstantiationResultResolver {
 public:
  InstantiateBytesResultResolver(Isolate* isolate, Local<Context> context,
                                 Local<Promise::Resolver> promise_resolver,
                                 Local<Value> module)
      : isolate_(isolate),
        context_(isolate, context),
        promise_resolver_(isolate, promise_resolver),
        module_(isolate, module) {
    context_.SetWeak();
    promise_resolver_.AnnotateStrongRetainer(kGlobalPromiseHandle);
    module_.AnnotateStrongRetainer(kGlobalModuleHandle);
  }

  void OnInstantiationSucceeded(
      i::DirectHandle<i::WasmInstanceObject> instance) override {
    if (context_.IsEmpty()) return;
    Local<Context> context = context_.Get(isolate_);
    WasmAsyncSuccess success = WasmAsyncSuccess::kSuccess;

    // The result is a JSObject with 2 fields which contain the
    // WasmInstanceObject and the WasmModuleObject.
    Local<Object> result = Object::New(isolate_);
    if (V8_UNLIKELY(result
                        ->CreateDataProperty(context,
                                             v8_str(isolate_, "module"),
                                             module_.Get(isolate_))
                        .IsNothing())) {
      i::Isolate* i_isolate = reinterpret_cast<i::Isolate*>(isolate_);
      // We assume that a TerminationException is the only reason why
      // `CreateDataProperty` can fail here. We should revisit
      // https://crbug.com/1515227 again if this CHECK fails.
      CHECK(i::IsTerminationException(i_isolate->exception()));
      result = Utils::ToLocal(direct_handle(
          i::Cast<i::JSObject>(i_isolate->exception()), i_isolate));
      success = WasmAsyncSuccess::kFail;
    }
    if (V8_UNLIKELY(result
                        ->CreateDataProperty(
                            context, v8_str(isolate_, "instance"),
                            Utils::ToLocal(i::Cast<i::Object>(instance)))
                        .IsNothing())) {
      i::Isolate* i_isolate = reinterpret_cast<i::Isolate*>(isolate_);
      CHECK(i::IsTerminationException(i_isolate->exception()));
      result = Utils::ToLocal(direct_handle(
          i::Cast<i::JSObject>(i_isolate->exception()), i_isolate));
      success = WasmAsyncSuccess::kFail;
    }

    auto callback = reinterpret_cast<i::Isolate*>(isolate_)
                        ->wasm_async_resolve_promise_callback();
    CHECK(callback);
    callback(isolate_, context, promise_resolver_.Get(isolate_), result,
             success);
  }

  void OnInstantiationFailed(i::DirectHandle<i::JSAny> error_reason) override {
    if (context_.IsEmpty()) return;
    auto callback = reinterpret_cast<i::Isolate*>(isolate_)
                        ->wasm_async_resolve_promise_callback();
    CHECK(callback);
    callback(isolate_, context_.Get(isolate_), promise_resolver_.Get(isolate_),
             Utils::ToLocal(error_reason), WasmAsyncSuccess::kFail);
  }

 private:
  static constexpr char kGlobalPromiseHandle[] =
      "InstantiateBytesResultResolver::promise_";
  static constexpr char kGlobalModuleHandle[] =
      "InstantiateBytesResultResolver::module_";
  Isolate* isolate_;
  Global<Context> context_;
  Global<Promise::Resolver> promise_resolver_;
  Global<Value> module_;
};

constexpr char InstantiateBytesResultResolver::kGlobalPromiseHandle[];
constexpr char InstantiateBytesResultResolver::kGlobalModuleHandle[];

// This class is the {CompilationResultResolver} for
// WebAssembly.instantiate(bytes, imports). When compilation finishes,
// {AsyncInstantiate} is started on the compilation result.
class AsyncInstantiateCompileResultResolver
    : public i::wasm::CompilationResultResolver {
 public:
  AsyncInstantiateCompileResultResolver(
      Isolate* isolate, Local<Context> context,
      Local<Promise::Resolver> promise_resolver, Local<Value> imports)
      : isolate_(isolate),
        context_(isolate, context),
        promise_resolver_(isolate, promise_resolver),
        imports_(isolate, imports) {
    context_.SetWeak();
    promise_resolver_.AnnotateStrongRetainer(kGlobalPromiseHandle);
    imports_.AnnotateStrongRetainer(kGlobalImportsHandle);
  }

  void OnCompilationSucceeded(
      i::DirectHandle<i::WasmModuleObject> result) override {
    if (finished_) return;
    finished_ = true;
    i::wasm::GetWasmEngine()->AsyncInstantiate(
        reinterpret_cast<i::Isolate*>(isolate_),
        std::make_unique<InstantiateBytesResultResolver>(
            isolate_, context_.Get(isolate_), promise_resolver_.Get(isolate_),
            Utils::ToLocal(i::Cast<i::Object>(result))),
        result, ImportsAsMaybeReceiver(imports_.Get(isolate_)));
  }

  void OnCompilationFailed(i::DirectHandle<i::JSAny> error_reason) override {
    if (finished_) return;
    finished_ = true;
    if (context_.IsEmpty()) return;
    auto callback = reinterpret_cast<i::Isolate*>(isolate_)
                        ->wasm_async_resolve_promise_callback();
    CHECK(callback);
    callback(isolate_, context_.Get(isolate_), promise_resolver_.Get(isolate_),
             Utils::ToLocal(error_reason), WasmAsyncSuccess::kFail);
  }

 private:
  static constexpr char kGlobalPromiseHandle[] =
      "AsyncInstantiateCompileResultResolver::promise_";
  static constexpr char kGlobalImportsHandle[] =
      "AsyncInstantiateCompileResultResolver::module_";
  bool finished_ = false;
  Isolate* isolate_;
  Global<Context> context_;
  Global<Promise::Resolver> promise_resolver_;
  Global<Value> imports_;
};

constexpr char AsyncInstantiateCompileResultResolver::kGlobalPromiseHandle[];
constexpr char AsyncInstantiateCompileResultResolver::kGlobalImportsHandle[];

// TODO(clemensb): Make this less inefficient.
std::string ToString(const char* name) { return std::string(name); }

std::string ToString(const i::DirectHandle<i::String> name) {
  return std::string("Property '") + name->ToStdString() + "'";
}

// Web IDL: '[EnforceRange] unsigned long'
// https://heycam.github.io/webidl/#EnforceRange
template <typename Name>
std::optional<uint32_t> EnforceUint32(Name argument_name, Local<v8::Value> v,
                                      Local<Context> context,
                                      ErrorThrower* thrower) {
  double double_number;
  if (!v->NumberValue(context).To(&double_number)) {
    thrower->TypeError("%s must be convertible to a number",
                       ToString(argument_name).c_str());
    return std::nullopt;
  }
  if (!std::isfinite(double_number)) {
    thrower->TypeError("%s must be convertible to a valid number",
                       ToString(argument_name).c_str());
    return std::nullopt;
  }
  if (double_number < 0) {
    thrower->TypeError("%s must be non-negative",
                       ToString(argument_name).c_str());
    return std::nullopt;
  }
  if (double_number > std::numeric_limits<uint32_t>::max()) {
    thrower->TypeError("%s must be in the unsigned long range",
                       ToString(argument_name).c_str());
    return std::nullopt;
  }

  return static_cast<uint32_t>(double_number);
}

// First step of AddressValueToU64, for addrtype == "i64".
template <typename Name>
std::optional<uint64_t> EnforceBigIntUint64(Name argument_name, Local<Value> v,
                                            Local<Context> context,
                                            ErrorThrower* thrower) {
  // Use the internal API, as v8::Value::ToBigInt clears exceptions.
  i::DirectHandle<i::BigInt> bigint;
  i::Isolate* i_isolate = i::Isolate::Current();
  if (!i::BigInt::FromObject(i_isolate, Utils::OpenDirectHandle(*v))
           .ToHandle(&bigint)) {
    return std::nullopt;
  }

  bool lossless;
  uint64_t result = bigint->AsUint64(&lossless);
  if (!lossless) {
    thrower->TypeError("%s must be in u64 range",
                       ToString(argument_name).c_str());
    return std::nullopt;
  }

  return result;
}

// The enum values need to match "WasmCompilationMethod" in
// tools/metrics/histograms/enums.xml.
enum CompilationMethod {
  kSyncCompilation = 0,
  kAsyncCompilation = 1,
  kStreamingCompilation = 2,
  kAsyncInstantiation = 3,
  kStreamingInstantiation = 4,
};

void RecordCompilationMethod(i::Isolate* isolate, CompilationMethod method) {
  isolate->counters()->wasm_compilation_method()->AddSample(method);
}

CompileTimeImports ArgumentToCompileOptions(
    Local<Value> arg_value, i::Isolate* isolate,
    WasmEnabledFeatures enabled_features) {
  CompileTimeImports result;
  if (base::FPU::GetFlushDenormals()) {
    result.Add(CompileTimeImport::kDisableDenormalFloats);
  }
  if (!enabled_features.has_imported_strings()) return result;
  i::DirectHandle<i::Object> arg = Utils::OpenDirectHandle(*arg_value);
  if (!i::IsJSReceiver(*arg)) return result;
  i::DirectHandle<i::JSReceiver> receiver = i::Cast<i::JSReceiver>(arg);

  // ==================== Builtins ====================
  i::DirectHandle<i::JSAny> builtins;
  ASSIGN_RETURN_ON_EXCEPTION_VALUE(isolate, builtins,
                                   i::Cast<i::JSAny>(i::JSReceiver::GetProperty(
                                       isolate, receiver, "builtins")),
                                   {});
  if (i::IsJSReceiver(*builtins)) {
    i::DirectHandle<i::Object> length_obj;
    ASSIGN_RETURN_ON_EXCEPTION_VALUE(
        isolate, length_obj,
        i::Object::GetLengthFromArrayLike(isolate,
                                          i::Cast<i::JSReceiver>(builtins)),
        {});
    double raw_length = i::Object::NumberValue(*length_obj);
    // Technically we should probably iterate up to 2^53-1 if {length_obj} says
    // so, but lengths above 2^32 probably don't happen in practice (and would
    // be very slow if they do), so just use a saturating to-uint32 conversion
    // for simplicity.
    uint32_t len = raw_length >= i::kMaxUInt32
                       ? i::kMaxUInt32
                       : static_cast<uint32_t>(raw_length);
    for (uint32_t i = 0; i < len; i++) {
      i::LookupIterator it(isolate, builtins, i);
      Maybe<bool> maybe_found = i::JSReceiver::HasProperty(&it);
      MAYBE_RETURN(maybe_found, {});
      if (!maybe_found.FromJust()) continue;
      i::DirectHandle<i::Object> value;
      ASSIGN_RETURN_ON_EXCEPTION_VALUE(isolate, value,
                                       i::Object::GetProperty(&it), {});
      if (i::IsString(*value)) {
        i::Tagged<i::String> builtin = i::Cast<i::String>(*value);
        if (builtin->IsEqualTo(base::CStrVector("js-string"))) {
          result.Add(CompileTimeImport::kJsString);
          continue;
        }
        if (enabled_features.has_imported_strings_utf8()) {
          if (builtin->IsEqualTo(base::CStrVector("text-encoder"))) {
            result.Add(CompileTimeImport::kTextEncoder);
            continue;
          }
          if (builtin->IsEqualTo(base::CStrVector("text-decoder"))) {
            result.Add(CompileTimeImport::kTextDecoder);
            continue;
          }
        }
        if (enabled_features.has_custom_descriptors()) {
          if (builtin->IsEqualTo(base::CStrVector("js-prototypes"))) {
            result.Add(CompileTimeImport::kJsPrototypes);
            continue;
          }
        }
      }
    }
  }

  // ==================== String constants ====================
  i::DirectHandle<i::String> importedStringConstants =
      isolate->factory()->InternalizeUtf8String("importedStringConstants");
  if (i::JSReceiver::HasProperty(isolate, receiver, importedStringConstants)
          .FromMaybe(false)) {
    i::DirectHandle<i::Object> constants_value;
    ASSIGN_RETURN_ON_EXCEPTION_VALUE(
        isolate, constants_value,
        i::JSReceiver::GetProperty(isolate, receiver, importedStringConstants),
        {});
    if (i::IsString(*constants_value)) {
      i::ToUtf8Lossy(isolate, i::Cast<i::String>(constants_value),
                     result.constants_module());
      result.Add(CompileTimeImport::kStringConstants);
    }
  }

  return result;
}

// A scope object with accessors and destructur DCHECKs to be used in
// implementations of Wasm JS-API methods.
class WasmJSApiScope {
 public:
  explicit WasmJSApiScope(
      const v8::FunctionCallbackInfo<v8::Value>& callback_info,
      const char* api_name)
      : callback_info_(callback_info),
        isolate_{callback_info.GetIsolate()},
        handle_scope_{isolate_},
        thrower_{reinterpret_cast<i::Isolate*>(isolate_), api_name} {
    DCHECK(i::ValidateCallbackInfo(callback_info));
  }

  WasmJSApiScope(const WasmJSApiScope&) = delete;
  WasmJSApiScope& operator=(const WasmJSApiScope&) = delete;

  void AssertException() const {
    DCHECK(i_isolate()->has_exception() || thrower_.error());
  }

  const v8::FunctionCallbackInfo<v8::Value>& callback_info() {
    return callback_info_;
  }

  const char* api_name() const { return thrower_.context_name(); }

  // Accessor for all essential fields. To be decomposed into individual aliases
  // via structured binding.
  std::tuple<v8::Isolate*, i::Isolate*, ErrorThrower&> isolates_and_thrower() {
    return {isolate_, i_isolate(), thrower_};
  }

 private:
  i::Isolate* i_isolate() const {
    return reinterpret_cast<i::Isolate*>(isolate_);
  }

  const v8::FunctionCallbackInfo<v8::Value>& callback_info_;
  v8::Isolate* const isolate_;
  HandleScope handle_scope_;
  ErrorThrower thrower_;
};

}  // namespace

// WebAssembly.compile(bytes, options) -> Promise
void WebAssemblyCompileImpl(const v8::FunctionCallbackInfo<v8::Value>& info) {
  WasmJSApiScope js_api_scope{info, "WebAssembly.compile()"};
  auto [isolate, i_isolate, thrower] = js_api_scope.isolates_and_thrower();
  RecordCompilationMethod(i_isolate, kAsyncCompilation);

  Local<Context> context = isolate->GetCurrentContext();
  Local<Promise::Resolver> promise_resolver;
  if (!Promise::Resolver::New(context).ToLocal(&promise_resolver)) {
    return js_api_scope.AssertException();
  }
  Local<Promise> promise = promise_resolver->GetPromise();
  info.GetReturnValue().Set(promise);

  std::shared_ptr<i::wasm::CompilationResultResolver> resolver(
      new AsyncCompilationResolver(isolate, context, promise_resolver));

  i::DirectHandle<i::NativeContext> native_context =
      i_isolate->native_context();
  if (!i::wasm::IsWasmCodegenAllowed(i_isolate, native_context)) {
    i::DirectHandle<i::String> error =
        i::wasm::ErrorStringForCodegen(i_isolate, native_context);
    thrower.CompileError("%s", error->ToCString().get());
    resolver->OnCompilationFailed(thrower.Reify());
    return;
  }

  base::OwnedVector<const uint8_t> bytes = GetAndCopyFirstArgumentAsBytes(
      info, i::wasm::max_module_size(), &thrower);
  if (bytes.empty()) {
    resolver->OnCompilationFailed(thrower.Reify());
    return;
  }
  auto enabled_features = WasmEnabledFeatures::FromIsolate(i_isolate);
  CompileTimeImports compile_imports =
      ArgumentToCompileOptions(info[1], i_isolate, enabled_features);
  if (i_isolate->has_exception()) {
    if (i_isolate->is_execution_terminating()) return;
    i::DirectHandle<i::JSAny> exception(Cast<i::JSAny>(i_isolate->exception()),
                                        i_isolate);
    i_isolate->clear_exception();
    resolver->OnCompilationFailed(exception);
    return;
  }
  i::wasm::GetWasmEngine()->AsyncCompile(
      i_isolate, enabled_features, std::move(compile_imports),
      std::move(resolver), std::move(bytes), js_api_scope.api_name());
}

void WasmStreamingCallbackForTesting(
    const v8::FunctionCallbackInfo<v8::Value>& info) {
  WasmJSApiScope js_api_scope{info, "WebAssembly.compile()"};
  auto [isolate, i_isolate, thrower] = js_api_scope.isolates_and_thrower();

  std::shared_ptr<v8::WasmStreaming> streaming =
      v8::WasmStreaming::Unpack(info.GetIsolate(), info.Data());

  // We don't check the buffer length up front, to allow d8 to test that the
  // streaming decoder implementation handles overly large inputs correctly.
  size_t unlimited = std::numeric_limits<size_t>::max();
  base::OwnedVector<const uint8_t> bytes =
      GetAndCopyFirstArgumentAsBytes(info, unlimited, &thrower);
  if (bytes.empty()) {
    streaming->Abort(Utils::ToLocal(thrower.Reify()));
    return;
  }
  streaming->OnBytesReceived(bytes.begin(), bytes.size());
  streaming->Finish();
  CHECK(!thrower.error());
}

void WasmStreamingPromiseFailedCallback(
    const v8::FunctionCallbackInfo<v8::Value>& info) {
  DCHECK(i::ValidateCallbackInfo(info));
  std::shared_ptr<v8::WasmStreaming> streaming =
      v8::WasmStreaming::Unpack(info.GetIsolate(), info.Data());
  streaming->Abort(info[0]);
}

void StartAsyncCompilationWithResolver(
    WasmJSApiScope& js_api_scope, Local<Value> response_or_promise,
    Local<Value> options_arg_value,
    std::shared_ptr<i::wasm::CompilationResultResolver> resolver) {
  auto [isolate, i_isolate, thrower] = js_api_scope.isolates_and_thrower();

  Local<Context> context = isolate->GetCurrentContext();
  i::DirectHandle<i::NativeContext> native_context =
      Utils::OpenHandle(*context);

  if (!i::wasm::IsWasmCodegenAllowed(i_isolate, native_context)) {
    i::DirectHandle<i::String> error =
        i::wasm::ErrorStringForCodegen(i_isolate, native_context);
    thrower.CompileError("%s", error->ToCString().get());
    resolver->OnCompilationFailed(thrower.Reify());
    return;
  }

  auto enabled_features = WasmEnabledFeatures::FromIsolate(i_isolate);
  CompileTimeImports compile_imports =
      ArgumentToCompileOptions(options_arg_value, i_isolate, enabled_features);
  if (i_isolate->has_exception()) {
    if (i_isolate->is_execution_terminating()) return;
    i::DirectHandle<i::JSAny> exception(Cast<i::JSAny>(i_isolate->exception()),
                                        i_isolate);
    i_isolate->clear_exception();
    resolver->OnCompilationFailed(exception);
    return;
  }

  // Allocate the streaming decoder in a Managed so we can pass it to the
  // embedder.
  std::shared_ptr<WasmStreaming> streaming = std::make_shared<WasmStreaming>(
      std::make_unique<WasmStreaming::WasmStreamingImpl>(
          i_isolate, js_api_scope.api_name(), std::move(compile_imports),
          resolver));
  i::DirectHandle<i::Managed<WasmStreaming>> data =
      i::Managed<WasmStreaming>::From(i_isolate, 0, streaming);

  DCHECK_NOT_NULL(i_isolate->wasm_streaming_callback());
  Local<v8::Function> compile_callback, reject_callback;
  if (!v8::Function::New(context, i_isolate->wasm_streaming_callback(),
                         Utils::ToLocal(i::Cast<i::Object>(data)), 1)
           .ToLocal(&compile_callback) ||
      !v8::Function::New(context, WasmStreamingPromiseFailedCallback,
                         Utils::ToLocal(i::Cast<i::Object>(data)), 1)
           .ToLocal(&reject_callback)) {
    return js_api_scope.AssertException();
  }

  // The parameter may be of type {Response} or of type {Promise<Response>}.
  // Treat either case of parameter as Promise.resolve(parameter)
  // as per https://www.w3.org/2001/tag/doc/promises-guide#resolve-arguments

  // Ending with:
  //    return Promise.resolve(parameter).then(compile_callback);
  Local<Promise::Resolver> input_resolver;
  if (!Promise::Resolver::New(context).ToLocal(&input_resolver) ||
      input_resolver->Resolve(context, response_or_promise).IsNothing()) {
    return js_api_scope.AssertException();
  }

  // Calling `then` on the promise can fail if the user monkey-patched stuff,
  // see https://crbug.com/374820218 / https://crbug.com/396461004.
  // If this does not fail, then the {compile_callback} will start streaming
  // compilation, which will eventually resolve the promise we set as result
  // value.
  if (input_resolver->GetPromise()
          ->Then(context, compile_callback, reject_callback)
          .IsEmpty()) {
    streaming->Abort(MaybeLocal<Value>{});
    return js_api_scope.AssertException();
  }
}

// WebAssembly.compileStreaming(Response | Promise<Response>, options)
//   -> Promise<WebAssembly.Module>
void WebAssemblyCompileStreaming(
    const v8::FunctionCallbackInfo<v8::Value>& info) {
  WasmJSApiScope js_api_scope{info, "WebAssembly.compileStreaming()"};
  auto [isolate, i_isolate, thrower] = js_api_scope.isolates_and_thrower();
  RecordCompilationMethod(i_isolate, kStreamingCompilation);
  Local<Context> context = isolate->GetCurrentContext();

  // Create and assign the return value of this function.
  Local<Promise::Resolver> promise_resolver;
  if (!Promise::Resolver::New(context).ToLocal(&promise_resolver)) {
    return js_api_scope.AssertException();
  }
  Local<Promise> promise = promise_resolver->GetPromise();
  info.GetReturnValue().Set(promise);

  // Prepare the CompilationResultResolver for the compilation.
  auto resolver = std::make_shared<AsyncCompilationResolver>(isolate, context,
                                                             promise_resolver);

  StartAsyncCompilationWithResolver(js_api_scope, info[0], info[1], resolver);
}

// WebAssembly.validate(bytes, options) -> bool
void WebAssemblyValidateImpl(const v8::FunctionCallbackInfo<v8::Value>& info) {
  WasmJSApiScope js_api_scope{info, "WebAssembly.validate()"};
  auto [isolate, i_isolate, thrower] = js_api_scope.isolates_and_thrower();
  v8::ReturnValue<v8::Value> return_value = info.GetReturnValue();

  bool bytes_are_shared = false;
  base::Vector<const uint8_t> bytes = GetFirstArgumentAsBytes(
      info, i::wasm::max_module_size(), &thrower, &bytes_are_shared);
  if (bytes.empty()) {
    js_api_scope.AssertException();
    // Propagate anything except wasm exceptions.
    if (!thrower.wasm_error()) return;
    // Clear wasm exceptions; return false instead.
    thrower.Reset();
    return_value.Set(v8::False(isolate));
    return;
  }

  auto enabled_features = WasmEnabledFeatures::FromIsolate(i_isolate);
  CompileTimeImports compile_imports =
      ArgumentToCompileOptions(info[1], i_isolate, enabled_features);
  if (i_isolate->has_exception()) {
    if (i_isolate->is_execution_terminating()) return;
    i_isolate->clear_exception();
    return_value.Set(v8::False(isolate));
    return;
  }

  bool validated = false;
  if (bytes_are_shared) {
    // Make a copy of the wire bytes to avoid concurrent modification.
    // Use relaxed reads (and writes, which is unnecessary here) to avoid TSan
    // reports in case the buffer is shared and is being modified concurrently.
    auto bytes_copy = base::OwnedVector<uint8_t>::NewForOverwrite(bytes.size());
    base::Relaxed_Memcpy(reinterpret_cast<base::Atomic8*>(bytes_copy.begin()),
                         reinterpret_cast<const base::Atomic8*>(bytes.data()),
                         bytes.size());
    validated = i::wasm::GetWasmEngine()->SyncValidate(
        i_isolate, enabled_features, std::move(compile_imports),
        bytes_copy.as_vector());
  } else {
    // The wire bytes are not shared, OK to use them directly.
    validated = i::wasm::GetWasmEngine()->SyncValidate(
        i_isolate, enabled_features, std::move(compile_imports), bytes);
  }

  return_value.Set(validated);
}

namespace {
bool TransferPrototype(i::Isolate* isolate,
                       i::DirectHandle<i::JSObject> destination,
                       i::DirectHandle<i::JSReceiver> source) {
  i::MaybeDirectHandle<i::HeapObject> maybe_prototype =
      i::JSObject::GetPrototype(isolate, source);
  i::DirectHandle<i::HeapObject> prototype;
  if (maybe_prototype.ToHandle(&prototype)) {
    Maybe<bool> result = i::JSObject::SetPrototype(
        isolate, destination, prototype,
        /*from_javascript=*/false, internal::kThrowOnError);
    if (!result.FromJust()) {
      DCHECK(isolate->has_exception());
      return false;
    }
  }
  return true;
}
}  // namespace

// new WebAssembly.Module(bytes, options) -> WebAssembly.Module
void WebAssemblyModuleImpl(const v8::FunctionCallbackInfo<v8::Value>& info) {
  WasmJSApiScope js_api_scope{info, "WebAssembly.Module()"};
  auto [isolate, i_isolate, thrower] = js_api_scope.isolates_and_thrower();
  if (i_isolate->wasm_module_callback()(info)) return;
  RecordCompilationMethod(i_isolate, kSyncCompilation);

  if (!info.IsConstructCall()) {
    thrower.TypeError("WebAssembly.Module must be invoked with 'new'");
    return;
  }
  i::DirectHandle<i::NativeContext> native_context =
      i_isolate->native_context();
  if (!i::wasm::IsWasmCodegenAllowed(i_isolate, native_context)) {
    i::DirectHandle<i::String> error =
        i::wasm::ErrorStringForCodegen(i_isolate, native_context);
    thrower.CompileError("%s", error->ToCString().get());
    return;
  }

  base::OwnedVector<const uint8_t> bytes = GetAndCopyFirstArgumentAsBytes(
      info, i::wasm::max_module_size(), &thrower);

  if (bytes.empty()) return js_api_scope.AssertException();

  auto enabled_features = WasmEnabledFeatures::FromIsolate(i_isolate);
  CompileTimeImports compile_imports =
      ArgumentToCompileOptions(info[1], i_isolate, enabled_features);
  if (i_isolate->has_exception()) {
    // TODO(14179): Does this need different error message handling?
    return;
  }
  i::MaybeDirectHandle<i::WasmModuleObject> maybe_module_obj;

  maybe_module_obj = i::wasm::GetWasmEngine()->SyncCompile(
      i_isolate, enabled_features, std::move(compile_imports), &thrower,
      std::move(bytes));

  i::DirectHandle<i::WasmModuleObject> module_obj;
  if (!maybe_module_obj.ToHandle(&module_obj)) return;

  // The infrastructure for `new Foo` calls allocates an object, which is
  // available here as {info.This()}. We're going to discard this object
  // and use {module_obj} instead, but it does have the correct prototype,
  // which we must harvest from it. This makes a difference when the JS
  // constructor function wasn't {WebAssembly.Module} directly, but some
  // subclass: {module_obj} has {WebAssembly.Module}'s prototype at this
  // point, so we must overwrite that with the correct prototype for {Foo}.
  if (!TransferPrototype(i_isolate, module_obj,
                         Utils::OpenDirectHandle(*info.This()))) {
    return;
  }

  v8::ReturnValue<v8::Value> return_value = info.GetReturnValue();
  return_value.Set(Utils::ToLocal(module_obj));
}

// WebAssembly.Module.imports(module) -> Array<Import>
void WebAssemblyModuleImportsImpl(
    const v8::FunctionCallbackInfo<v8::Value>& info) {
  WasmJSApiScope js_api_scope{info, "WebAssembly.Module.imports()"};
  auto [isolate, i_isolate, thrower] = js_api_scope.isolates_and_thrower();

  i::DirectHandle<i::WasmModuleObject> module_object;
  if (!GetFirstArgumentAsModule(info, &thrower).ToHandle(&module_object)) {
    return js_api_scope.AssertException();
  }
  auto imports = i::wasm::GetImports(i_isolate, module_object);
  info.GetReturnValue().Set(Utils::ToLocal(imports));
}

// WebAssembly.Module.exports(module) -> Array<Export>
void WebAssemblyModuleExportsImpl(
    const v8::FunctionCallbackInfo<v8::Value>& info) {
  WasmJSApiScope js_api_scope{info, "WebAssembly.Module.exports()"};
  auto [isolate, i_isolate, thrower] = js_api_scope.isolates_and_thrower();

  i::DirectHandle<i::WasmModuleObject> module_object;
  if (!GetFirstArgumentAsModule(info, &thrower).ToHandle(&module_object)) {
    return js_api_scope.AssertException();
  }
  auto exports = i::wasm::GetExports(i_isolate, module_object);
  info.GetReturnValue().Set(Utils::ToLocal(exports));
}

// WebAssembly.Module.customSections(module, name) -> Array<Section>
void WebAssemblyModuleCustomSectionsImpl(
    const v8::FunctionCallbackInfo<v8::Value>& info) {
  WasmJSApiScope js_api_scope{info, "WebAssembly.Module.customSections()"};
  auto [isolate, i_isolate, thrower] = js_api_scope.isolates_and_thrower();

  i::DirectHandle<i::WasmModuleObject> module_object;
  if (!GetFirstArgumentAsModule(info, &thrower).ToHandle(&module_object)) {
    return js_api_scope.AssertException();
  }

  if (info[1]->IsUndefined()) {
    thrower.TypeError("Argument 1 is required");
    return;
  }

  i::DirectHandle<i::Object> name;
  if (!i::Object::ToString(i_isolate, Utils::OpenDirectHandle(*info[1]))
           .ToHandle(&name)) {
    return js_api_scope.AssertException();
  }
  auto custom_sections = i::wasm::GetCustomSections(
      i_isolate, module_object, i::Cast<i::String>(name), &thrower);
  if (thrower.error()) return;
  info.GetReturnValue().Set(Utils::ToLocal(custom_sections));
}

// new WebAssembly.Instance(module, imports) -> WebAssembly.Instance
void WebAssemblyInstanceImpl(const v8::FunctionCallbackInfo<v8::Value>& info) {
  WasmJSApiScope js_api_scope{info, "WebAssembly.Instance()"};
  auto [isolate, i_isolate, thrower] = js_api_scope.isolates_and_thrower();
  RecordCompilationMethod(i_isolate, kAsyncInstantiation);
  i_isolate->CountUsage(
      v8::Isolate::UseCounterFeature::kWebAssemblyInstantiation);

  if (i_isolate->wasm_instance_callback()(info)) return;

  i::DirectHandle<i::JSObject> instance_obj;
  {
    if (!info.IsConstructCall()) {
      thrower.TypeError("WebAssembly.Instance must be invoked with 'new'");
      return;
    }

    i::DirectHandle<i::WasmModuleObject> module_object;
    if (!GetFirstArgumentAsModule(info, &thrower).ToHandle(&module_object)) {
      return js_api_scope.AssertException();
    }

    Local<Value> ffi = info[1];

    if (!ffi->IsUndefined() && !ffi->IsObject()) {
      thrower.TypeError("Argument 1 must be an object");
      return;
    }

    if (!i::wasm::GetWasmEngine()
             ->SyncInstantiate(i_isolate, &thrower, module_object,
                               ImportsAsMaybeReceiver(ffi),
                               i::MaybeDirectHandle<i::JSArrayBuffer>())
             .ToHandle(&instance_obj)) {
      return js_api_scope.AssertException();
    }
  }

  // The infrastructure for `new Foo` calls allocates an object, which is
  // available here as {info.This()}. We're going to discard this object
  // and use {instance_obj} instead, but it does have the correct prototype,
  // which we must harvest from it. This makes a difference when the JS
  // constructor function wasn't {WebAssembly.Instance} directly, but some
  // subclass: {instance_obj} has {WebAssembly.Instance}'s prototype at this
  // point, so we must overwrite that with the correct prototype for {Foo}.
  if (!TransferPrototype(i_isolate, instance_obj,
                         Utils::OpenDirectHandle(*info.This()))) {
    return js_api_scope.AssertException();
  }

  info.GetReturnValue().Set(Utils::ToLocal(instance_obj));
}

// WebAssembly.instantiateStreaming(
//     Response | Promise<Response> [, imports [, options]])
//   -> Promise<ResultObject>
// (where ResultObject has a "module" and an "instance" field)
void WebAssemblyInstantiateStreaming(
    const v8::FunctionCallbackInfo<v8::Value>& info) {
  WasmJSApiScope js_api_scope{info, "WebAssembly.instantiateStreaming()"};
  auto [isolate, i_isolate, thrower] = js_api_scope.isolates_and_thrower();
  RecordCompilationMethod(i_isolate, kStreamingInstantiation);
  i_isolate->CountUsage(
      v8::Isolate::UseCounterFeature::kWebAssemblyInstantiation);

  Local<Context> context = isolate->GetCurrentContext();

  // Create and assign the return value of this function.
  Local<Promise::Resolver> result_resolver;
  if (!Promise::Resolver::New(context).ToLocal(&result_resolver)) {
    return js_api_scope.AssertException();
  }
  Local<Promise> promise = result_resolver->GetPromise();
  info.GetReturnValue().Set(promise);

  // If info.Length < 2, this will be undefined - see FunctionCallbackInfo.
  Local<Value> ffi = info[1];

  if (!ffi->IsUndefined() && !ffi->IsObject()) {
    thrower.TypeError("Argument 1 must be an object");
    InstantiateModuleResultResolver resolver(isolate, context, result_resolver);
    resolver.OnInstantiationFailed(thrower.Reify());
    return;
  }

  auto compilation_resolver =
      std::make_shared<AsyncInstantiateCompileResultResolver>(
          isolate, context, result_resolver, ffi);
  StartAsyncCompilationWithResolver(js_api_scope, info[0], info[2],
                                    std::move(compilation_resolver));
}

// WebAssembly.instantiate(module, imports) -> WebAssembly.Instance
// WebAssembly.instantiate(bytes, imports, options) ->
//     {module: WebAssembly.Module, instance: WebAssembly.Instance}
void WebAssemblyInstantiateImpl(
    const v8::FunctionCallbackInfo<v8::Value>& info) {
  WasmJSApiScope js_api_scope{info, "WebAssembly.instantiate()"};
  auto [isolate, i_isolate, thrower] = js_api_scope.isolates_and_thrower();
  i_isolate->CountUsage(
      v8::Isolate::UseCounterFeature::kWebAssemblyInstantiation);

  Local<Context> context = isolate->GetCurrentContext();

  Local<Promise::Resolver> promise_resolver;
  if (!Promise::Resolver::New(context).ToLocal(&promise_resolver)) {
    return js_api_scope.AssertException();
  }
  Local<Promise> promise = promise_resolver->GetPromise();
  info.GetReturnValue().Set(promise);

  std::unique_ptr<i::wasm::InstantiationResultResolver> resolver(
      new InstantiateModuleResultResolver(isolate, context, promise_resolver));

  Local<Value> first_arg_value = info[0];
  i::DirectHandle<i::Object> first_arg =
      Utils::OpenDirectHandle(*first_arg_value);
  if (!IsJSObject(*first_arg)) {
    thrower.TypeError(
        "Argument 0 must be a buffer source or a WebAssembly.Module object");
    resolver->OnInstantiationFailed(thrower.Reify());
    return;
  }

  // If info.Length < 2, this will be undefined - see FunctionCallbackInfo.
  Local<Value> ffi = info[1];

  if (!ffi->IsUndefined() && !ffi->IsObject()) {
    thrower.TypeError("Argument 1 must be an object");
    resolver->OnInstantiationFailed(thrower.Reify());
    return;
  }

  if (IsWasmModuleObject(*first_arg)) {
    i::DirectHandle<i::WasmModuleObject> module_obj =
        i::Cast<i::WasmModuleObject>(first_arg);

    i::wasm::GetWasmEngine()->AsyncInstantiate(i_isolate, std::move(resolver),
                                               module_obj,
                                               ImportsAsMaybeReceiver(ffi));
    return;
  }

  base::OwnedVector<const uint8_t> bytes = GetAndCopyFirstArgumentAsBytes(
      info, i::wasm::max_module_size(), &thrower);
  if (bytes.empty()) {
    resolver->OnInstantiationFailed(thrower.Reify());
    return;
  }

  // We start compilation now, we have no use for the
  // {InstantiationResultResolver}.
  resolver.reset();

  std::shared_ptr<i::wasm::CompilationResultResolver> compilation_resolver(
      new AsyncInstantiateCompileResultResolver(isolate, context,
                                                promise_resolver, ffi));

  // The first parameter is a buffer source, we have to check if we are allowed
  // to compile it.
  i::DirectHandle<i::NativeContext> native_context =
      i_isolate->native_context();
  if (!i::wasm::IsWasmCodegenAllowed(i_isolate, native_context)) {
    i::DirectHandle<i::String> error =
        i::wasm::ErrorStringForCodegen(i_isolate, native_context);
    thrower.CompileError("%s", error->ToCString().get());
    compilation_resolver->OnCompilationFailed(thrower.Reify());
    return;
  }

  auto enabled_features = WasmEnabledFeatures::FromIsolate(i_isolate);
  CompileTimeImports compile_imports =
      ArgumentToCompileOptions(info[2], i_isolate, enabled_features);
  if (i_isolate->has_exception()) {
    if (i_isolate->is_execution_terminating()) return;
    i::DirectHandle<i::JSAny> exception(Cast<i::JSAny>(i_isolate->exception()),
                                        i_isolate);
    i_isolate->clear_exception();
    compilation_resolver->OnCompilationFailed(exception);
    return;
  }

  i::wasm::GetWasmEngine()->AsyncCompile(
      i_isolate, enabled_features, std::move(compile_imports),
      std::move(compilation_resolver), std::move(bytes),
      js_api_scope.api_name());
}

namespace {
// {AddressValueToU64} as defined in the memory64 js-api spec.
// Returns std::nullopt on error (exception or error set in the thrower), and
// the address value otherwise.
template <typename Name>
std::optional<uint64_t> AddressValueToU64(ErrorThrower* thrower,
                                          Local<Context> context,
                                          v8::Local<v8::Value> value,
                                          Name property_name,
                                          AddressType address_type) {
  switch (address_type) {
    case AddressType::kI32:
      return EnforceUint32(property_name, value, context, thrower);
    case AddressType::kI64:
      return EnforceBigIntUint64(property_name, value, context, thrower);
  }
  // The enum value is coming from inside the sandbox and while the switch is
  // exhaustive, it's not guaranteed that value is one of the declared values.
  SBXCHECK(false);
}

// {AddressValueToU64} plus additional bounds checks.
std::optional<uint64_t> AddressValueToBoundedU64(
    ErrorThrower* thrower, Local<Context> context, v8::Local<v8::Value> value,
    i::DirectHandle<i::String> property_name, AddressType address_type,
    uint64_t lower_bound, uint64_t upper_bound) {
  std::optional<uint64_t> maybe_address_value =
      AddressValueToU64(thrower, context, value, property_name, address_type);
  if (!maybe_address_value) return std::nullopt;
  uint64_t address_value = *maybe_address_value;

  if (address_value < lower_bound) {
    thrower->RangeError(
        "Property '%s': value %" PRIu64 " is below the lower bound %" PRIx64,
        property_name->ToCString().get(), address_value, lower_bound);
    return std::nullopt;
  }

  if (address_value > upper_bound) {
    thrower->RangeError(
        "Property '%s': value %" PRIu64 " is above the upper bound %" PRIu64,
        property_name->ToCString().get(), address_value, upper_bound);
    return std::nullopt;
  }

  return address_value;
}

// Returns std::nullopt on error (exception or error set in the thrower).
// The inner optional is std::nullopt if the property did not exist, and the
// address value otherwise.
std::optional<std::optional<uint64_t>> GetOptionalAddressValue(
    ErrorThrower* thrower, Local<Context> context, Local<v8::Object> descriptor,
    Local<String> property, AddressType address_type, int64_t lower_bound,
    uint64_t upper_bound) {
  v8::Local<v8::Value> value;
  if (!descriptor->Get(context, property).ToLocal(&value)) {
    return std::nullopt;
  }

  // Web IDL: dictionary presence
  // https://heycam.github.io/webidl/#dfn-present
  if (value->IsUndefined()) {
    // No exception, but no value either.
    return std::optional<uint64_t>{};
  }

  i::DirectHandle<i::String> property_name =
      v8::Utils::OpenDirectHandle(*property);

  std::optional<uint64_t> maybe_address_value =
      AddressValueToBoundedU64(thrower, context, value, property_name,
                               address_type, lower_bound, upper_bound);
  if (!maybe_address_value) return std::nullopt;
  return *maybe_address_value;
}

// Fetch 'initial' or 'minimum' property from `descriptor`. If both are
// provided, a TypeError is thrown.
// Returns std::nullopt on error (exception or error set in the thrower).
// TODO(aseemgarg): change behavior when the following bug is resolved:
// https://github.com/WebAssembly/js-types/issues/6
std::optional<uint64_t> GetInitialOrMinimumProperty(
    v8::Isolate* isolate, ErrorThrower* thrower, Local<Context> context,
    Local<v8::Object> descriptor, AddressType address_type,
    uint64_t upper_bound) {
  auto maybe_maybe_initial = GetOptionalAddressValue(
      thrower, context, descriptor, v8_str(isolate, "initial"), address_type, 0,
      upper_bound);
  if (!maybe_maybe_initial) return std::nullopt;
  std::optional<uint64_t> maybe_initial = *maybe_maybe_initial;

  auto enabled_features =
      WasmEnabledFeatures::FromIsolate(reinterpret_cast<i::Isolate*>(isolate));
  if (enabled_features.has_type_reflection()) {
    auto maybe_maybe_minimum = GetOptionalAddressValue(
        thrower, context, descriptor, v8_str(isolate, "minimum"), address_type,
        0, upper_bound);
    if (!maybe_maybe_minimum) return std::nullopt;
    std::optional<uint64_t> maybe_minimum = *maybe_maybe_minimum;

    if (maybe_initial && maybe_minimum) {
      thrower->TypeError(
          "The properties 'initial' and 'minimum' are not allowed at the same "
          "time");
      return std::nullopt;
    }
    if (maybe_minimum) {
      // Only 'minimum' exists, so we use 'minimum' as 'initial'.
      return *maybe_minimum;
    }
  }
  if (!maybe_initial) {
    // TODO(aseemgarg): update error message when the spec issue is resolved.
    thrower->TypeError("Property 'initial' is required");
    return std::nullopt;
  }
  return *maybe_initial;
}

v8::Local<Value> AddressValueFromUnsigned(Isolate* isolate,
                                          i::wasm::AddressType type,
                                          unsigned value) {
  return type == i::wasm::AddressType::kI64
             ? BigInt::NewFromUnsigned(isolate, value).As<Value>()
             : Integer::NewFromUnsigned(isolate, value).As<Value>();
}

i::DirectHandle<i::HeapObject> DefaultReferenceValue(i::Isolate* isolate,
                                                     i::wasm::ValueType type) {
  DCHECK(type.is_object_reference());
  // Use undefined for JS type (externref) but null for wasm types as wasm does
  // not know undefined.
  if (type.heap_representation() == i::wasm::HeapType::kExtern) {
    return isolate->factory()->undefined_value();
  } else if (!type.use_wasm_null()) {
    return isolate->factory()->null_value();
  }
  return isolate->factory()->wasm_null();
}

// Read the address type from a Memory or Table descriptor.
std::optional<AddressType> GetAddressType(Isolate* isolate,
                                          Local<Context> context,
                                          Local<v8::Object> descriptor,
                                          ErrorThrower* thrower) {
  v8::Local<v8::Value> address_value;
  if (!descriptor->Get(context, v8_str(isolate, "address"))
           .ToLocal(&address_value)) {
    return std::nullopt;
  }

  if (address_value->IsUndefined()) return AddressType::kI32;

  i::DirectHandle<i::String> address;
  if (!i::Object::ToString(reinterpret_cast<i::Isolate*>(isolate),
                           Utils::OpenDirectHandle(*address_value))
           .ToHandle(&address)) {
    return std::nullopt;
  }

  if (address->IsEqualTo(base::CStrVector("i64"))) return AddressType::kI64;
  if (address->IsEqualTo(base::CStrVector("i32"))) return AddressType::kI32;

  thrower->TypeError("Unknown address type '%s'; pass 'i32' or 'i64'",
                     address->ToCString().get());
  return std::nullopt;
}
}  // namespace

// new WebAssembly.Table(descriptor) -> WebAssembly.Table
void WebAssemblyTableImpl(const v8::FunctionCallbackInfo<v8::Value>& info) {
  WasmJSApiScope js_api_scope{info, "WebAssembly.Table()"};
  auto [isolate, i_isolate, thrower] = js_api_scope.isolates_and_thrower();

  if (!info.IsConstructCall()) {
    thrower.TypeError("WebAssembly.Table must be invoked with 'new'");
    return;
  }
  if (!info[0]->IsObject()) {
    thrower.TypeError("Argument 0 must be a table descriptor");
    return;
  }
  Local<Context> context = isolate->GetCurrentContext();
  Local<v8::Object> descriptor = Local<Object>::Cast(info[0]);
  i::wasm::ValueType type;
  // Parse the 'element' property of the `descriptor`.
  {
    v8::Local<v8::Value> value;
    if (!descriptor->Get(context, v8_str(isolate, "element")).ToLocal(&value)) {
      return js_api_scope.AssertException();
    }
    i::DirectHandle<i::String> string;
    if (!i::Object::ToString(reinterpret_cast<i::Isolate*>(isolate),
                             Utils::OpenDirectHandle(*value))
             .ToHandle(&string)) {
      return js_api_scope.AssertException();
    }
    auto enabled_features = WasmEnabledFeatures::FromIsolate(i_isolate);
    // The JS api uses 'anyfunc' instead of 'funcref'.
    if (string->IsEqualTo(base::CStrVector("anyfunc"))) {
      type = i::wasm::kWasmFuncRef;
    } else if (enabled_features.has_type_reflection() &&
               string->IsEqualTo(base::CStrVector("funcref"))) {
      // With the type reflection proposal, "funcref" replaces "anyfunc",
      // and anyfunc just becomes an alias for "funcref".
      type = i::wasm::kWasmFuncRef;
    } else if (string->IsEqualTo(base::CStrVector("externref"))) {
      type = i::wasm::kWasmExternRef;
    } else if (enabled_features.has_stringref() &&
               string->IsEqualTo(base::CStrVector("stringref"))) {
      type = i::wasm::kWasmStringRef;
    } else if (string->IsEqualTo(base::CStrVector("anyref"))) {
      type = i::wasm::kWasmAnyRef;
    } else if (string->IsEqualTo(base::CStrVector("eqref"))) {
      type = i::wasm::kWasmEqRef;
    } else if (string->IsEqualTo(base::CStrVector("structref"))) {
      type = i::wasm::kWasmStructRef;
    } else if (string->IsEqualTo(base::CStrVector("arrayref"))) {
      type = i::wasm::kWasmArrayRef;
    } else if (string->IsEqualTo(base::CStrVector("i31ref"))) {
      type = i::wasm::kWasmI31Ref;
    } else {
      thrower.TypeError(
          "Descriptor property 'element' must be a WebAssembly reference type");
      return;
    }
    // TODO(14616): Support shared types.
  }

  // Parse the 'address' property of the `descriptor`.
  std::optional<AddressType> maybe_address_type =
      GetAddressType(isolate, context, descriptor, &thrower);
  if (!maybe_address_type) {
    DCHECK(i_isolate->has_exception() || thrower.error());
    return;
  }
  AddressType address_type = *maybe_address_type;

  // Parse the 'initial' or 'minimum' property of the `descriptor`.
  std::optional<uint64_t> maybe_initial = GetInitialOrMinimumProperty(
      isolate, &thrower, context, descriptor, address_type,
      i::wasm::max_table_init_entries());
  if (!maybe_initial) return js_api_scope.AssertException();
  static_assert(i::wasm::kV8MaxWasmTableInitEntries <= i::kMaxUInt32);
  uint32_t initial = static_cast<uint32_t>(*maybe_initial);

  // Parse the 'maximum' property of the `descriptor`.
  uint64_t kNoMaximum = i::kMaxUInt64;
  auto maybe_maybe_maximum = GetOptionalAddressValue(
      &thrower, context, descriptor, v8_str(isolate, "maximum"), address_type,
      initial, kNoMaximum);
  if (!maybe_maybe_maximum) return js_api_scope.AssertException();
  std::optional<uint64_t> maybe_maximum = *maybe_maybe_maximum;

  DCHECK(!type.has_index());  // The JS API can't express type indices.
  i::wasm::CanonicalValueType canonical_type{type};
  i::DirectHandle<i::WasmTableObject> table_obj = i::WasmTableObject::New(
      i_isolate, i::DirectHandle<i::WasmTrustedInstanceData>(), type,
      canonical_type, initial, maybe_maximum.has_value(),
      maybe_maximum.value_or(0) /* note: unused if previous param is false */,
      DefaultReferenceValue(i_isolate, type), address_type);

  // The infrastructure for `new Foo` calls allocates an object, which is
  // available here as {info.This()}. We're going to discard this object
  // and use {table_obj} instead, but it does have the correct prototype,
  // which we must harvest from it. This makes a difference when the JS
  // constructor function wasn't {WebAssembly.Table} directly, but some
  // subclass: {table_obj} has {WebAssembly.Table}'s prototype at this
  // point, so we must overwrite that with the correct prototype for {Foo}.
  if (!TransferPrototype(i_isolate, table_obj,
                         Utils::OpenDirectHandle(*info.This()))) {
    return js_api_scope.AssertException();
  }

  if (initial > 0 && info.Length() >= 2 && !info[1]->IsUndefined()) {
    i::DirectHandle<i::Object> element = Utils::OpenDirectHandle(*info[1]);
    const char* error_message;
    if (!i::WasmTableObject::JSToWasmElement(i_isolate, table_obj, element,
                                             &error_message)
             .ToHandle(&element)) {
      thrower.TypeError(
          "Argument 2 must be undefined or a value of type compatible "
          "with the type of the new table: %s.",
          error_message);
      return;
    }
    for (uint32_t index = 0; index < static_cast<uint32_t>(initial); ++index) {
      i::WasmTableObject::Set(i_isolate, table_obj, index, element);
    }
  } else if (initial > 0) {
    DCHECK_EQ(type, table_obj->unsafe_type());
    switch (type.heap_representation()) {
      case i::wasm::HeapType::kString:
        thrower.TypeError(
            "Missing initial value when creating stringref table");
        return;
      case i::wasm::HeapType::kStringViewWtf8:
        thrower.TypeError("stringview_wtf8 has no JS representation");
        return;
      case i::wasm::HeapType::kStringViewWtf16:
        thrower.TypeError("stringview_wtf16 has no JS representation");
        return;
      case i::wasm::HeapType::kStringViewIter:
        thrower.TypeError("stringview_iter has no JS representation");
        return;
      default:
        break;
    }
  }
  v8::ReturnValue<v8::Value> return_value = info.GetReturnValue();
  return_value.Set(Utils::ToLocal(i::Cast<i::JSObject>(table_obj)));
}

// new WebAssembly.Memory(descriptor) -> WebAssembly.Memory
void WebAssemblyMemoryImpl(const v8::FunctionCallbackInfo<v8::Value>& info) {
  WasmJSApiScope js_api_scope{info, "WebAssembly.Memory()"};
  auto [isolate, i_isolate, thrower] = js_api_scope.isolates_and_thrower();

  if (!info.IsConstructCall()) {
    thrower.TypeError("WebAssembly.Memory must be invoked with 'new'");
    return;
  }
  if (!info[0]->IsObject()) {
    thrower.TypeError("Argument 0 must be a memory descriptor");
    return;
  }
  Local<Context> context = isolate->GetCurrentContext();
  Local<v8::Object> descriptor = Local<Object>::Cast(info[0]);

  // Parse the 'address' property of the `descriptor`.
  std::optional<AddressType> maybe_address_type =
      GetAddressType(isolate, context, descriptor, &thrower);
  if (!maybe_address_type) return js_api_scope.AssertException();
  AddressType address_type = *maybe_address_type;
  uint64_t max_supported_pages = address_type == AddressType::kI64
                                     ? i::wasm::kSpecMaxMemory64Pages
                                     : i::wasm::kSpecMaxMemory32Pages;
  // {max_supported_pages} will actually be in integer range. That's the type
  // {WasmMemoryObject::New} uses.
  static_assert(i::wasm::kSpecMaxMemory32Pages < i::kMaxInt);
  static_assert(i::wasm::kSpecMaxMemory64Pages < i::kMaxInt);

  // Parse the 'initial' or 'minimum' property of the `descriptor`.
  std::optional<uint64_t> maybe_initial =
      GetInitialOrMinimumProperty(isolate, &thrower, context, descriptor,
                                  address_type, max_supported_pages);
  if (!maybe_initial) {
    return js_api_scope.AssertException();
  }
  uint64_t initial = *maybe_initial;

  // Parse the 'maximum' property of the `descriptor`.
  auto maybe_maybe_maximum = GetOptionalAddressValue(
      &thrower, context, descriptor, v8_str(isolate, "maximum"), address_type,
      initial, max_supported_pages);
  if (!maybe_maybe_maximum) {
    return js_api_scope.AssertException();
  }
  std::optional<uint64_t> maybe_maximum = *maybe_maybe_maximum;

  // Parse the 'shared' property of the `descriptor`.
  v8::Local<v8::Value> value;
  if (!descriptor->Get(context, v8_str(isolate, "shared")).ToLocal(&value)) {
    return js_api_scope.AssertException();
  }

  auto shared = value->BooleanValue(isolate) ? i::SharedFlag::kShared
                                             : i::SharedFlag::kNotShared;

  // Throw TypeError if shared is true, and the descriptor has no "maximum".
  if (shared == i::SharedFlag::kShared && !maybe_maximum.has_value()) {
    thrower.TypeError("If shared is true, maximum property should be defined.");
    return;
  }

  i::DirectHandle<i::JSObject> memory_obj;
  if (!i::WasmMemoryObject::New(i_isolate, static_cast<int>(initial),
                                maybe_maximum ? static_cast<int>(*maybe_maximum)
                                              : i::WasmMemoryObject::kNoMaximum,
                                shared, address_type)
           .ToHandle(&memory_obj)) {
    thrower.RangeError("could not allocate memory");
    return;
  }

  // The infrastructure for `new Foo` calls allocates an object, which is
  // available here as {info.This()}. We're going to discard this object
  // and use {memory_obj} instead, but it does have the correct prototype,
  // which we must harvest from it. This makes a difference when the JS
  // constructor function wasn't {WebAssembly.Memory} directly, but some
  // subclass: {memory_obj} has {WebAssembly.Memory}'s prototype at this
  // point, so we must overwrite that with the correct prototype for {Foo}.
  if (!TransferPrototype(i_isolate, memory_obj,
                         Utils::OpenDirectHandle(*info.This()))) {
    return js_api_scope.AssertException();
  }

  if (shared == i::SharedFlag::kShared) {
    i::DirectHandle<i::JSArrayBuffer> buffer(
        i::Cast<i::WasmMemoryObject>(memory_obj)->array_buffer(), i_isolate);
    Maybe<bool> result =
        buffer->SetIntegrityLevel(i_isolate, buffer, i::FROZEN, i::kDontThrow);
    if (!result.FromJust()) {
      thrower.TypeError(
          "Status of setting SetIntegrityLevel of buffer is false.");
      return;
    }
  }
  info.GetReturnValue().Set(Utils::ToLocal(memory_obj));
}

// new WebAssembly.MemoryMapDescriptor(size) -> WebAssembly.MemoryMapDescriptor
void WebAssemblyMemoryMapDescriptorImpl(
    const v8::FunctionCallbackInfo<v8::Value>& info) {
  CHECK(i::v8_flags.experimental_wasm_memory_control);
  WasmJSApiScope js_api_scope{info, "WebAssembly.MemoryMapDescriptor()"};
  auto [isolate, i_isolate, thrower] = js_api_scope.isolates_and_thrower();
  if (!info.IsConstructCall()) {
    thrower.TypeError(
        "WebAssembly.MemoryMapDescriptor must be invoked with 'new'");
    return js_api_scope.AssertException();
  }

  std::optional<uint32_t> size =
      EnforceUint32("size", info[0], isolate->GetCurrentContext(), &thrower);

  if (!size.has_value()) {
    return js_api_scope.AssertException();
  }

  i::DirectHandle<i::JSObject> descriptor_obj;
  if (!i::WasmMemoryMapDescriptor::NewFromAnonymous(i_isolate, size.value())
           .ToHandle(&descriptor_obj)) {
    thrower.RuntimeError("Failed to create a MemoryMapDescriptor");
    return js_api_scope.AssertException();
  }

  // The infrastructure for `new Foo` calls allocates an object, which is
  // available here as {info.This()}. We're going to discard this object
  // and use {memory_obj} instead, but it does have the correct prototype,
  // which we must harvest from it. This makes a difference when the JS
  // constructor function wasn't {WebAssembly.Memory} directly, but some
  // subclass: {memory_obj} has {WebAssembly.Memory}'s prototype at this
  // point, so we must overwrite that with the correct prototype for {Foo}.
  if (!TransferPrototype(i_isolate, descriptor_obj,
                         Utils::OpenDirectHandle(*info.This()))) {
    DCHECK(i_isolate->has_exception());
    return js_api_scope.AssertException();
  }

  info.GetReturnValue().Set(Utils::ToLocal(descriptor_obj));
}

// Determines the type encoded in a value type property (e.g. type reflection).
// Returns false if there was an exception, true upon success. On success the
// outgoing {type} is set accordingly, or set to {wasm::kWasmVoid} in case the
// type could not be properly recognized.
std::optional<i::wasm::ValueType> GetValueType(
    Isolate* isolate, MaybeLocal<Value> maybe, Local<Context> context,
    WasmEnabledFeatures enabled_features) {
  v8::Local<v8::Value> value;
  if (!maybe.ToLocal(&value)) return std::nullopt;
  i::DirectHandle<i::String> string;
  if (!i::Object::ToString(reinterpret_cast<i::Isolate*>(isolate),
                           Utils::OpenDirectHandle(*value))
           .ToHandle(&string)) {
    return std::nullopt;
  }
  if (string->IsEqualTo(base::CStrVector("i32"))) {
    return i::wasm::kWasmI32;
  } else if (string->IsEqualTo(base::CStrVector("f32"))) {
    return i::wasm::kWasmF32;
  } else if (string->IsEqualTo(base::CStrVector("i64"))) {
    return i::wasm::kWasmI64;
  } else if (string->IsEqualTo(base::CStrVector("f64"))) {
    return i::wasm::kWasmF64;
  } else if (string->IsEqualTo(base::CStrVector("v128"))) {
    return i::wasm::kWasmS128;
  } else if (string->IsEqualTo(base::CStrVector("externref"))) {
    return i::wasm::kWasmExternRef;
  } else if (enabled_features.has_type_reflection() &&
             string->IsEqualTo(base::CStrVector("funcref"))) {
    // The type reflection proposal renames "anyfunc" to "funcref", and makes
    // "anyfunc" an alias of "funcref".
    return i::wasm::kWasmFuncRef;
  } else if (string->IsEqualTo(base::CStrVector("anyfunc"))) {
    // The JS api spec uses 'anyfunc' instead of 'funcref'.
    return i::wasm::kWasmFuncRef;
  } else if (string->IsEqualTo(base::CStrVector("eqref"))) {
    return i::wasm::kWasmEqRef;
  } else if (enabled_features.has_stringref() &&
             string->IsEqualTo(base::CStrVector("stringref"))) {
    return i::wasm::kWasmStringRef;
  } else if (string->IsEqualTo(base::CStrVector("anyref"))) {
    return i::wasm::kWasmAnyRef;
  } else if (string->IsEqualTo(base::CStrVector("structref"))) {
    return i::wasm::kWasmStructRef;
  } else if (string->IsEqualTo(base::CStrVector("arrayref"))) {
    return i::wasm::kWasmArrayRef;
  } else if (string->IsEqualTo(base::CStrVector("i31ref"))) {
    return i::wasm::kWasmI31Ref;
  } else if (enabled_features.has_exnref() &&
             string->IsEqualTo(base::CStrVector("exnref"))) {
    return i::wasm::kWasmExnRef;
  }
  // Unrecognized type.
  return i::wasm::kWasmVoid;
}

namespace {

bool ToI32(Local<v8::Value> value, Local<Context> context, int32_t* i32_value) {
  if (!value->IsUndefined()) {
    v8::Local<v8::Int32> int32_value;
    if (!value->ToInt32(context).ToLocal(&int32_value)) return false;
    if (!int32_value->Int32Value(context).To(i32_value)) return false;
  }
  return true;
}

bool ToI64(Local<v8::Value> value, Local<Context> context, int64_t* i64_value) {
  if (!value->IsUndefined()) {
    v8::Local<v8::BigInt> bigint_value;
    if (!value->ToBigInt(context).ToLocal(&bigint_value)) return false;
    *i64_value = bigint_value->Int64Value();
  }
  return true;
}

bool ToF32(Local<v8::Value> value, Local<Context> context, float* f32_value) {
  if (!value->IsUndefined()) {
    double f64_value = 0;
    v8::Local<v8::Number> number_value;
    if (!value->ToNumber(context).ToLocal(&number_value)) return false;
    if (!number_value->NumberValue(context).To(&f64_value)) return false;
    *f32_value = i::DoubleToFloat32(f64_value);
  }
  return true;
}

bool ToF64(Local<v8::Value> value, Local<Context> context, double* f64_value) {
  if (!value->IsUndefined()) {
    v8::Local<v8::Number> number_value;
    if (!value->ToNumber(context).ToLocal(&number_value)) return false;
    if (!number_value->NumberValue(context).To(f64_value)) return false;
  }
  return true;
}
}  // namespace

// WebAssembly.Global
void WebAssemblyGlobalImpl(const v8::FunctionCallbackInfo<v8::Value>& info) {
  WasmJSApiScope js_api_scope{info, "WebAssembly.Global()"};
  auto [isolate, i_isolate, thrower] = js_api_scope.isolates_and_thrower();

  if (!info.IsConstructCall()) {
    thrower.TypeError("WebAssembly.Global must be invoked with 'new'");
    return;
  }
  if (!info[0]->IsObject()) {
    thrower.TypeError("Argument 0 must be a global descriptor");
    return;
  }
  Local<Context> context = isolate->GetCurrentContext();
  Local<v8::Object> descriptor = Local<Object>::Cast(info[0]);
  auto enabled_features = WasmEnabledFeatures::FromIsolate(i_isolate);

  // The descriptor's 'mutable'.
  bool is_mutable;
  {
    v8::Local<v8::Value> value;
    if (!descriptor->Get(context, v8_str(isolate, "mutable")).ToLocal(&value)) {
      return js_api_scope.AssertException();
    }
    is_mutable = value->BooleanValue(isolate);
    if (is_mutable) {
      i_isolate->CountUsage(
          v8::Isolate::UseCounterFeature::kWasmMutableGlobals);
    }
  }

  // The descriptor's type, called 'value'. It is called 'value' because this
  // descriptor is planned to be reused as the global's type for reflection,
  // so calling it 'type' is redundant.
  i::wasm::ValueType type;
  {
    v8::MaybeLocal<v8::Value> maybe =
        descriptor->Get(context, v8_str(isolate, "value"));
    std::optional<i::wasm::ValueType> maybe_type =
        GetValueType(isolate, maybe, context, enabled_features);
    if (!maybe_type) return js_api_scope.AssertException();
    type = *maybe_type;
    if (type == i::wasm::kWasmVoid) {
      thrower.TypeError(
          "Descriptor property 'value' must be a WebAssembly type");
      return;
    }
  }

  const uint32_t offset = 0;
  i::MaybeDirectHandle<i::WasmGlobalObject> maybe_global_obj =
      i::WasmGlobalObject::New(
          i_isolate, i::DirectHandle<i::WasmTrustedInstanceData>(),
          i::MaybeDirectHandle<i::JSArrayBuffer>(),
          i::MaybeDirectHandle<i::FixedArray>(), type, offset, is_mutable);

  i::DirectHandle<i::WasmGlobalObject> global_obj;
  if (!maybe_global_obj.ToHandle(&global_obj)) {
    return js_api_scope.AssertException();
  }

  // The infrastructure for `new Foo` calls allocates an object, which is
  // available here as {info.This()}. We're going to discard this object
  // and use {global_obj} instead, but it does have the correct prototype,
  // which we must harvest from it. This makes a difference when the JS
  // constructor function wasn't {WebAssembly.Global} directly, but some
  // subclass: {global_obj} has {WebAssembly.Global}'s prototype at this
  // point, so we must overwrite that with the correct prototype for {Foo}.
  if (!TransferPrototype(i_isolate, global_obj,
                         Utils::OpenDirectHandle(*info.This()))) {
    return js_api_scope.AssertException();
  }

  // Convert value to a WebAssembly value, the default value is 0.
  Local<v8::Value> value = Local<Value>::Cast(info[1]);
  switch (type.kind()) {
    case i::wasm::kI32: {
      int32_t i32_value = 0;
      if (!ToI32(value, context, &i32_value)) {
        return js_api_scope.AssertException();
      }
      global_obj->SetI32(i32_value);
      break;
    }
    case i::wasm::kI64: {
      int64_t i64_value = 0;
      if (!ToI64(value, context, &i64_value)) {
        return js_api_scope.AssertException();
      }
      global_obj->SetI64(i64_value);
      break;
    }
    case i::wasm::kF32: {
      float f32_value = 0;
      if (!ToF32(value, context, &f32_value)) {
        return js_api_scope.AssertException();
      }
      global_obj->SetF32(f32_value);
      break;
    }
    case i::wasm::kF64: {
      double f64_value = 0;
      if (!ToF64(value, context, &f64_value)) {
        return js_api_scope.AssertException();
      }
      global_obj->SetF64(f64_value);
      break;
    }
    case i::wasm::kRef:
      if (info.Length() < 2) {
        thrower.TypeError("Non-defaultable global needs initial value");
        return;
      }
      [[fallthrough]];
    case i::wasm::kRefNull: {
      // We need the wasm default value {null} over {undefined}.
      i::DirectHandle<i::Object> value_handle;
      if (info.Length() < 2) {
        value_handle = DefaultReferenceValue(i_isolate, type);
      } else {
        value_handle = Utils::OpenDirectHandle(*value);
        const char* error_message;
        // While the JS API generally allows indexed types, it currently has
        // no way to specify such types in `new WebAssembly.Global(...)`.
        // TODO(14034): Fix this if that changes.
        DCHECK(!type.has_index());
        i::wasm::CanonicalValueType canonical_type{type};
        if (!i::wasm::JSToWasmObject(i_isolate, value_handle, canonical_type,
                                     &error_message)
                 .ToHandle(&value_handle)) {
          thrower.TypeError("%s", error_message);
          return;
        }
      }
      global_obj->SetRef(value_handle);
      break;
    }
    case i::wasm::kS128: {
      thrower.TypeError(
          "A global of type 'v128' cannot be created in JavaScript");
      return;
    }
    case i::wasm::kI8:
    case i::wasm::kI16:
    case i::wasm::kF16:
    case i::wasm::kVoid:
    case i::wasm::kTop:
    case i::wasm::kBottom:
      UNREACHABLE();
  }

  i::DirectHandle<i::JSObject> global_js_object(global_obj);
  info.GetReturnValue().Set(Utils::ToLocal(global_js_object));
}

namespace {

uint32_t GetIterableLength(i::Isolate* isolate, Local<Context> context,
                           Local<Object> iterable) {
  Local<String> length = Utils::ToLocal(isolate->factory()->length_string());
  MaybeLocal<Value> property = iterable->Get(context, length);
  if (property.IsEmpty()) return i::kMaxUInt32;
  MaybeLocal<Uint32> number = property.ToLocalChecked()->ToArrayIndex(context);
  if (number.IsEmpty()) return i::kMaxUInt32;
  DCHECK_NE(i::kMaxUInt32, number.ToLocalChecked()->Value());
  return number.ToLocalChecked()->Value();
}

}  // namespace

// WebAssembly.Tag
void WebAssemblyTagImpl(const v8::FunctionCallbackInfo<v8::Value>& info) {
  WasmJSApiScope js_api_scope{info, "WebAssembly.Tag()"};
  auto [isolate, i_isolate, thrower] = js_api_scope.isolates_and_thrower();

  if (!info.IsConstructCall()) {
    thrower.TypeError("WebAssembly.Tag must be invoked with 'new'");
    return;
  }
  if (!info[0]->IsObject()) {
    thrower.TypeError("Argument 0 must be a tag type");
    return;
  }

  Local<Object> event_type = Local<Object>::Cast(info[0]);
  Local<Context> context = isolate->GetCurrentContext();
  auto enabled_features = WasmEnabledFeatures::FromIsolate(i_isolate);

  // Load the 'parameters' property of the event type.
  Local<String> parameters_key = v8_str(isolate, "parameters");
  v8::MaybeLocal<v8::Value> parameters_maybe =
      event_type->Get(context, parameters_key);
  v8::Local<v8::Value> parameters_value;
  if (!parameters_maybe.ToLocal(&parameters_value) ||
      !parameters_value->IsObject()) {
    thrower.TypeError("Argument 0 must be a tag type with 'parameters'");
    return;
  }
  Local<Object> parameters = parameters_value.As<Object>();
  uint32_t parameters_len = GetIterableLength(i_isolate, context, parameters);
  if (parameters_len == i::kMaxUInt32) {
    thrower.TypeError("Argument 0 contains parameters without 'length'");
    return;
  }
  if (parameters_len > i::wasm::kV8MaxWasmFunctionParams) {
    thrower.TypeError("Argument 0 contains too many parameters");
    return;
  }

  // Decode the tag type and construct a signature.
  std::vector<i::wasm::ValueType> param_types(parameters_len,
                                              i::wasm::kWasmVoid);
  for (uint32_t i = 0; i < parameters_len; ++i) {
    i::wasm::ValueType& type = param_types[i];
    MaybeLocal<Value> maybe = parameters->Get(context, i);
    std::optional<i::wasm::ValueType> maybe_type =
        GetValueType(isolate, maybe, context, enabled_features);
    if (!maybe_type) return;
    type = *maybe_type;
    if (type == i::wasm::kWasmVoid) {
      thrower.TypeError(
          "Argument 0 parameter type at index #%u must be a value type", i);
      return;
    }
  }
  const i::wasm::FunctionSig sig{0, parameters_len, param_types.data()};
  // Set the tag index to 0. It is only used for debugging purposes, and has no
  // meaningful value when declared outside of a wasm module.
  auto tag = i::WasmExceptionTag::New(i_isolate, 0);

  i::wasm::CanonicalTypeIndex type_index =
      i::wasm::GetWasmEngine()->type_canonicalizer()->AddRecursiveGroup(&sig);

  i::DirectHandle<i::JSObject> tag_object =
      i::WasmTagObject::New(i_isolate, &sig, type_index, tag,
                            i::DirectHandle<i::WasmTrustedInstanceData>());
  info.GetReturnValue().Set(Utils::ToLocal(tag_object));
}

namespace {

uint32_t GetEncodedSize(i::DirectHandle<i::WasmTagObject> tag_object) {
  auto serialized_sig = tag_object->serialized_signature();
  i::wasm::WasmTagSig sig{
      0, static_cast<size_t>(serialized_sig->length()),
      reinterpret_cast<i::wasm::ValueType*>(serialized_sig->begin())};
  return i::WasmExceptionPackage::GetEncodedSize(&sig);
}

V8_WARN_UNUSED_RESULT bool EncodeExceptionValues(
    v8::Isolate* isolate,
    i::DirectHandle<i::PodArray<i::wasm::ValueType>> signature,
    i::DirectHandle<i::WasmTagObject> tag_object, const Local<Value>& arg,
    ErrorThrower* thrower, i::DirectHandle<i::FixedArray> values_out) {
  Local<Context> context = isolate->GetCurrentContext();
  uint32_t index = 0;
  if (!arg->IsObject()) {
    thrower->TypeError("Exception values must be an iterable object");
    return false;
  }
  i::Isolate* i_isolate = reinterpret_cast<i::Isolate*>(isolate);
  auto values = arg.As<Object>();
  uint32_t length = GetIterableLength(i_isolate, context, values);
  if (length == i::kMaxUInt32) {
    thrower->TypeError("Exception values argument has no length");
    return false;
  }
  if (length != static_cast<uint32_t>(signature->length())) {
    thrower->TypeError(
        "Number of exception values does not match signature length");
    return false;
  }
  for (int i = 0; i < signature->length(); ++i) {
    Local<Value> value;
    if (!values->Get(context, i).ToLocal(&value)) return false;
    i::wasm::ValueType type = signature->get(i);
    switch (type.kind()) {
      case i::wasm::kI32: {
        int32_t i32 = 0;
        if (!ToI32(value, context, &i32)) return false;
        i::EncodeI32ExceptionValue(values_out, &index, i32);
        break;
      }
      case i::wasm::kI64: {
        int64_t i64 = 0;
        if (!ToI64(value, context, &i64)) return false;
        i::EncodeI64ExceptionValue(values_out, &index, i64);
        break;
      }
      case i::wasm::kF32: {
        float f32 = 0;
        if (!ToF32(value, context, &f32)) return false;
        int32_t i32 = base::bit_cast<int32_t>(f32);
        i::EncodeI32ExceptionValue(values_out, &index, i32);
        break;
      }
      case i::wasm::kF64: {
        double f64 = 0;
        if (!ToF64(value, context, &f64)) return false;
        int64_t i64 = base::bit_cast<int64_t>(f64);
        i::EncodeI64ExceptionValue(values_out, &index, i64);
        break;
      }
      case i::wasm::kRef:
      case i::wasm::kRefNull: {
        const char* error_message;
        i::DirectHandle<i::Object> value_handle =
            Utils::OpenDirectHandle(*value);
        i::wasm::CanonicalValueType canonical_type = i::wasm::kWasmBottom;
        if (type.has_index()) {
          // Canonicalize the type using the tag's original module.
          // Indexed types are guaranteed to come from an instance.
          DCHECK(tag_object->has_trusted_data());
          i::Tagged<i::WasmTrustedInstanceData> wtid =
              tag_object->trusted_data(i_isolate);
          const i::wasm::WasmModule* module = wtid->module();
          canonical_type =
              type.Canonicalize(module->canonical_type_id(type.ref_index()));
        } else {
          canonical_type = i::wasm::CanonicalValueType{type};
        }
        if (!i::wasm::JSToWasmObject(i_isolate, value_handle, canonical_type,
                                     &error_message)
                 .ToHandle(&value_handle)) {
          thrower->TypeError("%s", error_message);
          return false;
        }
        values_out->set(index++, *value_handle);
        break;
      }
      case i::wasm::kS128:
        thrower->TypeError("Invalid type v128");
        return false;
      case i::wasm::kI8:
      case i::wasm::kI16:
      case i::wasm::kF16:
      case i::wasm::kVoid:
      case i::wasm::kTop:
      case i::wasm::kBottom:
        UNREACHABLE();
    }
  }
  return true;
}

}  // namespace

void WebAssemblyExceptionImpl(const v8::FunctionCallbackInfo<v8::Value>& info) {
  WasmJSApiScope js_api_scope{info, "WebAssembly.Exception()"};
  auto [isolate, i_isolate, thrower] = js_api_scope.isolates_and_thrower();

  if (!info.IsConstructCall()) {
    thrower.TypeError("WebAssembly.Exception must be invoked with 'new'");
    return;
  }
  if (!info[0]->IsObject()) {
    thrower.TypeError("Argument 0 must be a WebAssembly tag");
    return;
  }
  i::DirectHandle<i::Object> arg0 = Utils::OpenDirectHandle(*info[0]);
  if (!IsWasmTagObject(i::Cast<i::HeapObject>(*arg0))) {
    thrower.TypeError("Argument 0 must be a WebAssembly tag");
    return;
  }
  auto tag_object = i::Cast<i::WasmTagObject>(arg0);
  i::DirectHandle<i::WasmExceptionTag> tag(
      i::Cast<i::WasmExceptionTag>(tag_object->tag()), i_isolate);
  auto js_tag = i::Cast<i::WasmTagObject>(i_isolate->context()->wasm_js_tag());
  if (*tag == js_tag->tag()) {
    thrower.TypeError("Argument 0 cannot be WebAssembly.JSTag");
    return;
  }
  const i::wasm::CanonicalSig* sig =
      i::wasm::GetTypeCanonicalizer()->LookupFunctionSignature(
          i::wasm::CanonicalTypeIndex{
              static_cast<uint32_t>(tag_object->canonical_type_index())});

  if (sig->return_count() != 0) {
    thrower.TypeError(
        "Invalid WebAssembly tag (return values not permitted in Exception "
        "tag)");
    return;
  }

  uint32_t size = GetEncodedSize(tag_object);
  i::DirectHandle<i::WasmExceptionPackage> runtime_exception =
      i::WasmExceptionPackage::New(i_isolate, tag, size);
  // The constructor above should guarantee that the cast below succeeds.
  i::DirectHandle<i::FixedArray> values =
      i::Cast<i::FixedArray>(i::WasmExceptionPackage::GetExceptionValues(
          i_isolate, runtime_exception));
  i::DirectHandle<i::PodArray<i::wasm::ValueType>> signature(
      tag_object->serialized_signature(), i_isolate);
  if (!EncodeExceptionValues(isolate, signature, tag_object, info[1], &thrower,
                             values)) {
    return js_api_scope.AssertException();
  }

  // Third argument: optional ExceptionOption ({traceStack: <bool>}).
  if (!info[2]->IsNullOrUndefined() && !info[2]->IsObject()) {
    thrower.TypeError("Argument 2 is not an object");
    return;
  }
  if (info[2]->IsObject()) {
    Local<Context> context = isolate->GetCurrentContext();
    Local<Object> trace_stack_obj = Local<Object>::Cast(info[2]);
    Local<String> trace_stack_key = v8_str(isolate, "traceStack");
    v8::Local<Value> trace_stack_value;
    if (!trace_stack_obj->Get(context, trace_stack_key)
             .ToLocal(&trace_stack_value)) {
      return js_api_scope.AssertException();
    }
    if (trace_stack_value->BooleanValue(isolate)) {
      i::Handle<i::Object> caller = Utils::OpenHandle(*info.NewTarget());

      i::DirectHandle<i::Object> capture_result;
      if (!i::ErrorUtils::CaptureStackTrace(i_isolate, runtime_exception,
                                            i::SKIP_NONE, caller)
               .ToHandle(&capture_result)) {
        return js_api_scope.AssertException();
      }
    }
  }

  info.GetReturnValue().Set(
      Utils::ToLocal(i::Cast<i::Object>(runtime_exception)));
}

i::DirectHandle<i::JSFunction> NewPromisingWasmExportedFunction(
    i::Isolate* i_isolate, i::DirectHandle<i::WasmExportedFunctionData> data,
    ErrorThrower& thrower) {
  i::DirectHandle<i::WasmTrustedInstanceData> trusted_instance_data{
      data->instance_data(), i_isolate};
  int func_index = data->function_index();
  const i::wasm::WasmModule* module = trusted_instance_data->module();
  i::wasm::ModuleTypeIndex sig_index = module->functions[func_index].sig_index;
  const i::wasm::CanonicalSig* sig =
      i::wasm::GetTypeCanonicalizer()->LookupFunctionSignature(
          module->canonical_sig_id(sig_index));
  i::DirectHandle<i::Code> wrapper;
  if (!internal::wasm::IsJSCompatibleSignature(sig)) {
    // If the signature is incompatible with JS, the original export will have
    // compiled an incompatible signature wrapper, so just reuse that.
    wrapper =
        i::DirectHandle<i::Code>(data->wrapper_code(i_isolate), i_isolate);
  } else {
    wrapper = BUILTIN_CODE(i_isolate, WasmPromising);
  }

  // TODO(14034): Create funcref RTTs lazily?
  i::DirectHandle<i::Map> rtt{
      i::Cast<i::Map>(
          trusted_instance_data->managed_object_maps()->get(sig_index.index)),
      i_isolate};

  int num_imported_functions = module->num_imported_functions;
  i::DirectHandle<i::TrustedObject> implicit_arg;
  constexpr bool kShared = false;
  if (func_index >= num_imported_functions) {
    implicit_arg = trusted_instance_data;
  } else {
    implicit_arg = i_isolate->factory()->NewWasmImportData(
        direct_handle(i::Cast<i::WasmImportData>(
                          trusted_instance_data->dispatch_table_for_imports()
                              ->implicit_arg(func_index)),
                      i_isolate),
        kShared);
  }

  i::DirectHandle<i::WasmInternalFunction> internal =
      i_isolate->factory()->NewWasmInternalFunction(
          implicit_arg, func_index, kShared,
          trusted_instance_data->GetCallTarget(func_index));
  i::DirectHandle<i::WasmFuncRef> func_ref =
      i_isolate->factory()->NewWasmFuncRef(internal, rtt, kShared);
  if (func_index < num_imported_functions) {
    i::Cast<i::WasmImportData>(implicit_arg)->set_call_origin(*internal);
  }

  i::DirectHandle<i::JSFunction> result = i::WasmExportedFunction::New(
      i_isolate, trusted_instance_data, func_ref, internal,
      static_cast<int>(data->sig()->parameter_count()), wrapper);
  return result;
}

// WebAssembly.Function
void WebAssemblyFunction(const v8::FunctionCallbackInfo<v8::Value>& info) {
  WasmJSApiScope js_api_scope{info, "WebAssembly.Function()"};
  auto [isolate, i_isolate, thrower] = js_api_scope.isolates_and_thrower();

  if (!info.IsConstructCall()) {
    thrower.TypeError("WebAssembly.Function must be invoked with 'new'");
    return;
  }
  if (!info[0]->IsObject()) {
    thrower.TypeError("Argument 0 must be a function type");
    return;
  }
  Local<Object> function_type = Local<Object>::Cast(info[0]);
  Local<Context> context = isolate->GetCurrentContext();
  auto enabled_features = WasmEnabledFeatures::FromIsolate(i_isolate);

  // Load the 'parameters' property of the function type.
  Local<String> parameters_key = v8_str(isolate, "parameters");
  v8::MaybeLocal<v8::Value> parameters_maybe =
      function_type->Get(context, parameters_key);
  v8::Local<v8::Value> parameters_value;
  if (!parameters_maybe.ToLocal(&parameters_value) ||
      !parameters_value->IsObject()) {
    thrower.TypeError("Argument 0 must be a function type with 'parameters'");
    return;
  }
  Local<Object> parameters = parameters_value.As<Object>();
  uint32_t parameters_len = GetIterableLength(i_isolate, context, parameters);
  if (parameters_len == i::kMaxUInt32) {
    thrower.TypeError("Argument 0 contains parameters without 'length'");
    return;
  }
  if (parameters_len > i::wasm::kV8MaxWasmFunctionParams) {
    thrower.TypeError("Argument 0 contains too many parameters");
    return;
  }

  // Load the 'results' property of the function type.
  v8::Local<v8::Value> results_value;
  if (!function_type->Get(context, v8_str(isolate, "results"))
           .ToLocal(&results_value)) {
    return js_api_scope.AssertException();
  }
  if (!results_value->IsObject()) {
    thrower.TypeError("Argument 0 must be a function type with 'results'");
    return;
  }
  Local<Object> results = results_value.As<Object>();
  uint32_t results_len = GetIterableLength(i_isolate, context, results);
  if (results_len == i::kMaxUInt32) {
    thrower.TypeError("Argument 0 contains results without 'length'");
    return;
  }
  if (results_len > i::wasm::kV8MaxWasmFunctionReturns) {
    thrower.TypeError("Argument 0 contains too many results");
    return;
  }

  // Decode the function type and construct a signature.
  i::Zone zone(i_isolate->allocator(), ZONE_NAME);
  i::wasm::FunctionSig::Builder builder(&zone, results_len, parameters_len);
  for (uint32_t i = 0; i < parameters_len; ++i) {
    MaybeLocal<Value> maybe = parameters->Get(context, i);
    std::optional<i::wasm::ValueType> maybe_type =
        GetValueType(isolate, maybe, context, enabled_features);
    if (!maybe_type) return;
    i::wasm::ValueType type = *maybe_type;
    if (type == i::wasm::kWasmVoid) {
      thrower.TypeError(
          "Argument 0 parameter type at index #%u must be a value type", i);
      return;
    }
    builder.AddParam(type);
  }
  for (uint32_t i = 0; i < results_len; ++i) {
    MaybeLocal<Value> maybe = results->Get(context, i);
    std::optional<i::wasm::ValueType> maybe_type =
        GetValueType(isolate, maybe, context, enabled_features);
    if (!maybe_type) return js_api_scope.AssertException();
    i::wasm::ValueType type = *maybe_type;
    if (type == i::wasm::kWasmVoid) {
      thrower.TypeError(
          "Argument 0 result type at index #%u must be a value type", i);
      return;
    }
    builder.AddReturn(type);
  }

  if (!info[1]->IsObject()) {
    thrower.TypeError("Argument 1 must be a function");
    return;
  }
  const i::wasm::FunctionSig* sig = builder.Get();
  i::wasm::Suspend suspend = i::wasm::kNoSuspend;

  i::DirectHandle<i::JSReceiver> callable =
      Utils::OpenDirectHandle(*info[1].As<Object>());
  if (i::IsWasmSuspendingObject(*callable)) {
    suspend = i::wasm::kSuspend;
    callable = direct_handle(
        i::Cast<i::WasmSuspendingObject>(*callable)->callable(), i_isolate);
    DCHECK(i::IsCallable(*callable));
  } else if (!i::IsCallable(*callable)) {
    thrower.TypeError("Argument 1 must be a function");
    return;
  }

  i::DirectHandle<i::JSFunction> result =
      i::WasmJSFunction::New(i_isolate, sig, callable, suspend);
  info.GetReturnValue().Set(Utils::ToLocal(result));
}

// WebAssembly.promising(Function) -> Function
void WebAssemblyPromising(const v8::FunctionCallbackInfo<v8::Value>& info) {
  WasmJSApiScope js_api_scope{info, "WebAssembly.promising()"};
  auto [isolate, i_isolate, thrower] = js_api_scope.isolates_and_thrower();
  i_isolate->CountUsage(v8::Isolate::kWasmJavaScriptPromiseIntegration);

  if (!info[0]->IsFunction()) {
    thrower.TypeError("Argument 0 must be a function");
    return;
  }
  i::DirectHandle<i::JSReceiver> callable =
      Utils::OpenDirectHandle(*info[0].As<Function>());

  if (!i::WasmExportedFunction::IsWasmExportedFunction(*callable)) {
    thrower.TypeError("Argument 0 must be a WebAssembly exported function");
    return;
  }
  auto wasm_exported_function = i::Cast<i::WasmExportedFunction>(*callable);
  i::DirectHandle<i::WasmExportedFunctionData> data(
      wasm_exported_function->shared()->wasm_exported_function_data(),
      i_isolate);
  if (i::wasm::is_asmjs_module(data->instance_data()->module())) {
    thrower.TypeError("Argument 0 must be a WebAssembly exported function");
    return;
  }
  i::DirectHandle<i::JSFunction> result =
      NewPromisingWasmExportedFunction(i_isolate, data, thrower);
  info.GetReturnValue().Set(Utils::ToLocal(i::Cast<i::JSObject>(result)));
}

// WebAssembly.Suspending(Function) -> Suspending
void WebAssemblySuspendingImpl(
    const v8::FunctionCallbackInfo<v8::Value>& info) {
  WasmJSApiScope js_api_scope{info, "WebAssembly.Suspending()"};
  auto [isolate, i_isolate, thrower] = js_api_scope.isolates_and_thrower();
  i_isolate->CountUsage(v8::Isolate::kWasmJavaScriptPromiseIntegration);

  if (!info.IsConstructCall()) {
    thrower.TypeError("WebAssembly.Suspending must be invoked with 'new'");
    return;
  }
  if (!info[0]->IsFunction()) {
    thrower.TypeError("Argument 0 must be a function");
    return;
  }

  i::DirectHandle<i::JSReceiver> callable =
      Utils::OpenDirectHandle(*info[0].As<Function>());

  if (i::WasmExportedFunction::IsWasmExportedFunction(*callable) ||
      i::WasmJSFunction::IsWasmJSFunction(*callable)) {
    thrower.TypeError("Argument 0 must not be a WebAssembly function");
    return;
  }

  i::DirectHandle<i::WasmSuspendingObject> result =
      i::WasmSuspendingObject::New(i_isolate, callable);
  info.GetReturnValue().Set(Utils::ToLocal(i::Cast<i::JSObject>(result)));
}

// WebAssembly.DescriptorOptions({options}) -> DescriptorOptions
void WebAssemblyDescriptorOptionsImpl(
    const FunctionCallbackInfo<v8::Value>& info) {
  WasmJSApiScope js_api_scope{info, "WebAssembly.DescriptorOptions()"};
  auto [isolate, i_isolate, thrower] = js_api_scope.isolates_and_thrower();

  if (!info.IsConstructCall()) {
    thrower.TypeError(
        "WebAssembly.DescriptorOptions must be invoked with 'new'");
    return;
  }
  if (!info[0]->IsObject()) {
    thrower.TypeError("Argument 0 must be an object");
    return;
  }

  i::DirectHandle<i::JSReceiver> options =
      Utils::OpenDirectHandle(*Local<v8::Object>::Cast(info[0]));
  i::DirectHandle<i::Object> prototype;
  if (!i::JSReceiver::GetProperty(i_isolate, options, "prototype")
           .ToHandle(&prototype)) {
    return js_api_scope.AssertException();
  }
  if (!i::IsJSReceiver(*prototype)) {
    thrower.TypeError("Prototype must be an object");
    return;
  }
  i::DirectHandle<i::WasmDescriptorOptions> result =
      i::WasmDescriptorOptions::New(i_isolate, prototype);
  info.GetReturnValue().Set(Utils::ToLocal(i::Cast<i::JSObject>(result)));
}

// WebAssembly.Function.prototype.type() -> FunctionType
void WebAssemblyFunctionType(const v8::FunctionCallbackInfo<v8::Value>& info) {
  WasmJSApiScope js_api_scope{info, "WebAssembly.Function.type()"};
  auto [isolate, i_isolate, thrower] = js_api_scope.isolates_and_thrower();

  i::DirectHandle<i::JSObject> type;

  i::DirectHandle<i::Object> fun = Utils::OpenDirectHandle(*info.This());
  if (i::WasmExportedFunction::IsWasmExportedFunction(*fun)) {
    auto wasm_exported_function = i::Cast<i::WasmExportedFunction>(fun);
    i::Tagged<i::WasmExportedFunctionData> data =
        wasm_exported_function->shared()->wasm_exported_function_data();
    // Note: while {zone} is only referenced directly in the if-block below,
    // its lifetime must exceed that of {sig}.
    // TODO(42210967): Creating a Zone just to create a modified copy of a
    // single signature is rather expensive. It would be good to find a more
    // efficient approach, if this function is ever considered performance
    // relevant.
    i::Zone zone(i_isolate->allocator(), ZONE_NAME);
    const i::wasm::FunctionSig* sig =
        data->instance_data()->module()->functions[data->function_index()].sig;
    i::wasm::Promise promise_flags =
        i::WasmFunctionData::PromiseField::decode(data->js_promise_flags());
    if (promise_flags == i::wasm::kPromise) {
      // The wrapper function returns a promise as an externref instead of the
      // original return type.
      size_t param_count = sig->parameter_count();
      i::wasm::FunctionSig::Builder builder(&zone, 1, param_count);
      for (size_t i = 0; i < param_count; ++i) {
        builder.AddParam(sig->GetParam(i));
      }
      builder.AddReturn(i::wasm::kWasmExternRef);
      sig = builder.Get();
    }
    type = i::wasm::GetTypeForFunction(i_isolate, sig);
  } else if (i::WasmJSFunction::IsWasmJSFunction(*fun)) {
    const i::wasm::CanonicalSig* sig = i::Cast<i::WasmJSFunction>(fun)
                                           ->shared()
                                           ->wasm_js_function_data()
                                           ->GetSignature();
    // As long as WasmJSFunctions cannot use indexed types, their canonical
    // signatures are bit-compatible with module-specific signatures.
#if DEBUG
    for (i::wasm::CanonicalValueType t : sig->all()) {
      DCHECK(!t.has_index());
    }
#endif
    static_assert(sizeof(i::wasm::ValueType) ==
                  sizeof(i::wasm::CanonicalValueType));
    type = i::wasm::GetTypeForFunction(
        i_isolate, reinterpret_cast<const i::wasm::FunctionSig*>(sig));
  } else {
    thrower.TypeError("Receiver must be a WebAssembly.Function");
    return;
  }

  info.GetReturnValue().Set(Utils::ToLocal(type));
}

constexpr const char* kName_WasmGlobalObject = "WebAssembly.Global";
constexpr const char* kName_WasmMemoryObject = "WebAssembly.Memory";
constexpr const char* kName_WasmMemoryMapDescriptor =
    "WebAssembly.MemoryMapDescriptor";
constexpr const char* kName_WasmInstanceObject = "WebAssembly.Instance";
constexpr const char* kName_WasmTableObject = "WebAssembly.Table";
constexpr const char* kName_WasmTagObject = "WebAssembly.Tag";
constexpr const char* kName_WasmExceptionPackage = "WebAssembly.Exception";

#define EXTRACT_THIS(var, WasmType)                                \
  i::DirectHandle<i::WasmType> var;                                \
  {                                                                \
    i::DirectHandle<i::Object> this_arg =                          \
        Utils::OpenDirectHandle(*info.This());                     \
    if (!Is##WasmType(*this_arg)) {                                \
      thrower.TypeError("Receiver is not a %s", kName_##WasmType); \
      return;                                                      \
    }                                                              \
    var = i::Cast<i::WasmType>(this_arg);                          \
  }

void WebAssemblyInstanceGetExportsImpl(
    const v8::FunctionCallbackInfo<v8::Value>& info) {
  WasmJSApiScope js_api_scope{info, "WebAssembly.Instance.exports()"};
  auto [isolate, i_isolate, thrower] = js_api_scope.isolates_and_thrower();
  EXTRACT_THIS(receiver, WasmInstanceObject);
  i::DirectHandle<i::JSObject> exports_object(receiver->exports_object(),
                                              i_isolate);

  info.GetReturnValue().Set(Utils::ToLocal(exports_object));
}

void WebAssemblyTableGetLengthImpl(
    const v8::FunctionCallbackInfo<v8::Value>& info) {
  WasmJSApiScope js_api_scope{info, "WebAssembly.Table.length()"};
  auto [isolate, i_isolate, thrower] = js_api_scope.isolates_and_thrower();
  EXTRACT_THIS(receiver, WasmTableObject);

  int length = receiver->current_length();
  DCHECK_LE(0, length);
  info.GetReturnValue().Set(
      AddressValueFromUnsigned(isolate, receiver->address_type(), length));
}

// WebAssembly.Table.grow(num, init_value = null) -> num
void WebAssemblyTableGrowImpl(const v8::FunctionCallbackInfo<v8::Value>& info) {
  WasmJSApiScope js_api_scope{info, "WebAssembly.Table.grow()"};
  auto [isolate, i_isolate, thrower] = js_api_scope.isolates_and_thrower();
  Local<Context> context = isolate->GetCurrentContext();
  EXTRACT_THIS(receiver, WasmTableObject);

  std::optional<uint64_t> maybe_grow_by = AddressValueToU64(
      &thrower, context, info[0], "Argument 0", receiver->address_type());
  if (!maybe_grow_by) return js_api_scope.AssertException();
  uint64_t grow_by = *maybe_grow_by;

  i::DirectHandle<i::Object> init_value;

  if (info.Length() >= 2) {
    init_value = Utils::OpenDirectHandle(*info[1]);
    const char* error_message;
    if (!i::WasmTableObject::JSToWasmElement(i_isolate, receiver, init_value,
                                             &error_message)
             .ToHandle(&init_value)) {
      thrower.TypeError("Argument 1 is invalid: %s", error_message);
      return;
    }
  } else if (receiver->unsafe_type().is_non_nullable()) {
    thrower.TypeError(
        "Argument 1 must be specified for non-nullable element type");
    return;
  } else {
    init_value = DefaultReferenceValue(i_isolate, receiver->unsafe_type());
  }

  static_assert(i::wasm::kV8MaxWasmTableSize <= i::kMaxUInt32);
  int old_size = grow_by > i::wasm::max_table_size()
                     ? -1
                     : i::WasmTableObject::Grow(i_isolate, receiver,
                                                static_cast<uint32_t>(grow_by),
                                                init_value);
  if (old_size < 0) {
    thrower.RangeError("failed to grow table by %" PRIu64, grow_by);
    return;
  }
  info.GetReturnValue().Set(
      AddressValueFromUnsigned(isolate, receiver->address_type(), old_size));
}

namespace {
V8_WARN_UNUSED_RESULT bool WasmObjectToJSReturnValue(
    v8::ReturnValue<v8::Value>& return_value, i::DirectHandle<i::Object> value,
    i::wasm::ValueType type, i::Isolate* isolate, ErrorThrower* thrower) {
  switch (type.heap_type().representation()) {
    case internal::wasm::HeapType::kStringViewWtf8:
      thrower->TypeError("%s", "stringview_wtf8 has no JS representation");
      return false;
    case internal::wasm::HeapType::kStringViewWtf16:
      thrower->TypeError("%s", "stringview_wtf16 has no JS representation");
      return false;
    case internal::wasm::HeapType::kStringViewIter:
      thrower->TypeError("%s", "stringview_iter has no JS representation");
      return false;
    case internal::wasm::HeapType::kExn:
    case internal::wasm::HeapType::kNoExn:
    case internal::wasm::HeapType::kCont:
    case internal::wasm::HeapType::kNoCont:
      thrower->TypeError("invalid type %s", type.name().c_str());
      return false;
    default:
      return_value.Set(Utils::ToLocal(i::wasm::WasmToJSObject(isolate, value)));
      return true;
  }
}
}  // namespace

// WebAssembly.Table.get(num) -> any
void WebAssemblyTableGetImpl(const v8::FunctionCallbackInfo<v8::Value>& info) {
  WasmJSApiScope js_api_scope{info, "WebAssembly.Table.get()"};
  auto [isolate, i_isolate, thrower] = js_api_scope.isolates_and_thrower();
  Local<Context> context = isolate->GetCurrentContext();
  EXTRACT_THIS(receiver, WasmTableObject);

  std::optional<uint64_t> maybe_address = AddressValueToU64(
      &thrower, context, info[0], "Argument 0", receiver->address_type());
  if (!maybe_address) return;
  uint64_t address = *maybe_address;

  if (address > i::kMaxUInt32 ||
      !receiver->is_in_bounds(static_cast<uint32_t>(address))) {
    thrower.RangeError("invalid address %" PRIu64 " in %s table of size %d",
                       address, receiver->unsafe_type().name().c_str(),
                       receiver->current_length());
    return;
  }

  i::DirectHandle<i::Object> result = i::WasmTableObject::Get(
      i_isolate, receiver, static_cast<uint32_t>(address));

  v8::ReturnValue<v8::Value> return_value = info.GetReturnValue();
  if (!WasmObjectToJSReturnValue(return_value, result, receiver->unsafe_type(),
                                 i_isolate, &thrower)) {
    return js_api_scope.AssertException();
  }
}

// WebAssembly.Table.set(num, any)
void WebAssemblyTableSetImpl(const v8::FunctionCallbackInfo<v8::Value>& info) {
  WasmJSApiScope js_api_scope{info, "WebAssembly.Table.set()"};
  auto [isolate, i_isolate, thrower] = js_api_scope.isolates_and_thrower();
  Local<Context> context = isolate->GetCurrentContext();
  EXTRACT_THIS(table_object, WasmTableObject);

  std::optional<uint64_t> maybe_address = AddressValueToU64(
      &thrower, context, info[0], "Argument 0", table_object->address_type());
  if (!maybe_address) return js_api_scope.AssertException();
  uint64_t address = *maybe_address;

  if (address > i::kMaxUInt32 ||
      !table_object->is_in_bounds(static_cast<uint32_t>(address))) {
    thrower.RangeError("invalid address %" PRIu64 " in %s table of size %d",
                       address, table_object->unsafe_type().name().c_str(),
                       table_object->current_length());
    return;
  }

  i::DirectHandle<i::Object> element;
  if (info.Length() >= 2) {
    element = Utils::OpenDirectHandle(*info[1]);
    const char* error_message;
    if (!i::WasmTableObject::JSToWasmElement(i_isolate, table_object, element,
                                             &error_message)
             .ToHandle(&element)) {
      thrower.TypeError("Argument 1 is invalid for table: %s", error_message);
      return;
    }
  } else if (table_object->unsafe_type().is_defaultable()) {
    element = DefaultReferenceValue(i_isolate, table_object->unsafe_type());
  } else {
    thrower.TypeError("Table of non-defaultable type %s needs explicit element",
                      table_object->unsafe_type().name().c_str());
    return;
  }

  i::WasmTableObject::Set(i_isolate, table_object,
                          static_cast<uint32_t>(address), element);
}

// WebAssembly.Table.type() -> TableType
void WebAssemblyTableType(const v8::FunctionCallbackInfo<v8::Value>& info) {
  WasmJSApiScope js_api_scope{info, "WebAssembly.Table.type()"};
  auto [isolate, i_isolate, thrower] = js_api_scope.isolates_and_thrower();
  EXTRACT_THIS(table, WasmTableObject);
  std::optional<uint64_t> max_size = table->maximum_length_u64();
  auto type = i::wasm::GetTypeForTable(i_isolate, table->unsafe_type(),
                                       table->current_length(), max_size,
                                       table->address_type());
  info.GetReturnValue().Set(Utils::ToLocal(type));
}

// WebAssembly.MemoryMapDescriptor.map()
void WebAssemblyMemoryMapDescriptorMapImpl(
    const v8::FunctionCallbackInfo<v8::Value>& info) {
  CHECK(i::v8_flags.experimental_wasm_memory_control);
  WasmJSApiScope js_api_scope{info, "WebAssembly.MemoryMapDescriptor.map()"};
  auto [isolate, i_isolate, thrower] = js_api_scope.isolates_and_thrower();
  EXTRACT_THIS(receiver, WasmMemoryMapDescriptor);

  i::DirectHandle<i::WasmMemoryObject> memory;
  {
    i::DirectHandle<i::Object> memory_param = Utils::OpenDirectHandle(*info[0]);
    if (!i::IsWasmMemoryObject(*memory_param)) {
      thrower.TypeError("Parameter is not a WebAssembly.Memory");
      return js_api_scope.AssertException();
    }
    memory = i::Cast<i::WasmMemoryObject>(memory_param);
  }

  Local<Context> context = isolate->GetCurrentContext();
  std::optional<uint32_t> offset =
      EnforceUint32("Argument 1", info[1], context, &thrower);
  if (!offset.has_value()) {
    return js_api_scope.AssertException();
  }
  size_t mapped_size = receiver->MapDescriptor(memory, offset.value());
  if (!mapped_size) {
    thrower.RuntimeError(
        "Failed to map the MemoryMapDescriptor to WebAssembly memory.");
    return js_api_scope.AssertException();
  }
  receiver->set_memory(MakeWeak(*memory));
  receiver->set_offset(offset.value());
  receiver->set_size(static_cast<uint32_t>(mapped_size));
  info.GetReturnValue().Set(static_cast<int64_t>(mapped_size));
}

// WebAssembly.MemoryMapDescriptor.unmap()
void WebAssemblyMemoryMapDescriptorUnmapImpl(
    const v8::FunctionCallbackInfo<v8::Value>& info) {
  CHECK(i::v8_flags.experimental_wasm_memory_control);
  WasmJSApiScope js_api_scope{info, "WebAssembly.MemoryMapDescriptor.unmap()"};
  auto [isolate, i_isolate, thrower] = js_api_scope.isolates_and_thrower();
  EXTRACT_THIS(receiver, WasmMemoryMapDescriptor);

  if (!receiver->UnmapDescriptor()) {
    thrower.RangeError("Failed to unmap the MemoryMapDescriptor.");
    return;
  }
}

// WebAssembly.Memory.grow(num) -> num
void WebAssemblyMemoryGrowImpl(
    const v8::FunctionCallbackInfo<v8::Value>& info) {
  WasmJSApiScope js_api_scope{info, "WebAssembly.Memory.grow()"};
  auto [isolate, i_isolate, thrower] = js_api_scope.isolates_and_thrower();
  Local<Context> context = isolate->GetCurrentContext();
  EXTRACT_THIS(receiver, WasmMemoryObject);

  std::optional<uint64_t> maybe_delta_pages = AddressValueToU64(
      &thrower, context, info[0], "Argument 0", receiver->address_type());
  if (!maybe_delta_pages) return js_api_scope.AssertException();
  uint64_t delta_pages = *maybe_delta_pages;

  i::DirectHandle<i::JSArrayBuffer> old_buffer(receiver->array_buffer(),
                                               i_isolate);

  uint64_t old_pages = old_buffer->GetByteLength() / i::wasm::kWasmPageSize;
  uint64_t max_pages = receiver->maximum_pages();

  if (delta_pages > max_pages - old_pages) {
    thrower.RangeError("Maximum memory size exceeded");
    return;
  }

  static_assert(i::wasm::kV8MaxWasmMemory64Pages <= i::kMaxUInt32);
  int32_t ret = i::WasmMemoryObject::Grow(i_isolate, receiver,
                                          static_cast<uint32_t>(delta_pages));
  if (ret == -1) {
    thrower.RangeError("Unable to grow instance memory");
    return;
  }
  info.GetReturnValue().Set(
      AddressValueFromUnsigned(isolate, receiver->address_type(), ret));
}

// WebAssembly.Memory.buffer -> ArrayBuffer
void WebAssemblyMemoryGetBufferImpl(
    const v8::FunctionCallbackInfo<v8::Value>& info) {
  WasmJSApiScope js_api_scope{info, "WebAssembly.Memory.buffer"};
  auto [isolate, i_isolate, thrower] = js_api_scope.isolates_and_thrower();
  EXTRACT_THIS(receiver, WasmMemoryObject);

  i::DirectHandle<i::Object> buffer_obj(receiver->array_buffer(), i_isolate);
  DCHECK(IsJSArrayBuffer(*buffer_obj));
  i::DirectHandle<i::JSArrayBuffer> buffer(
      i::Cast<i::JSArrayBuffer>(*buffer_obj), i_isolate);
  if (buffer->is_shared()) {
    // TODO(gdeepti): More needed here for when cached buffer, and current
    // buffer are out of sync, handle that here when bounds checks, and Grow
    // are handled correctly.
    Maybe<bool> result =
        buffer->SetIntegrityLevel(i_isolate, buffer, i::FROZEN, i::kDontThrow);
    if (!result.FromJust()) {
      thrower.TypeError(
          "Status of setting SetIntegrityLevel of buffer is false.");
      return;
    }
  }
  info.GetReturnValue().Set(Utils::ToLocal(buffer));
}

// WebAssembly.Memory.toFixedLengthBuffer() -> ArrayBuffer
void WebAssemblyMemoryToFixedLengthBufferImpl(
    const v8::FunctionCallbackInfo<v8::Value>& info) {
  WasmJSApiScope js_api_scope{info, "WebAssembly.Memory.toFixedLengthBuffer()"};
  auto [isolate, i_isolate, thrower] = js_api_scope.isolates_and_thrower();
  EXTRACT_THIS(receiver, WasmMemoryObject);

  i::DirectHandle<i::Object> buffer_obj(receiver->array_buffer(), i_isolate);
  DCHECK(IsJSArrayBuffer(*buffer_obj));
  i::DirectHandle<i::JSArrayBuffer> buffer(
      i::Cast<i::JSArrayBuffer>(*buffer_obj), i_isolate);
  if (buffer->is_resizable_by_js()) {
    buffer = i::WasmMemoryObject::ToFixedLengthBuffer(i_isolate, receiver);
  }
  if (buffer->is_shared()) {
    Maybe<bool> result =
        buffer->SetIntegrityLevel(i_isolate, buffer, i::FROZEN, i::kDontThrow);
    if (!result.FromJust()) {
      thrower.TypeError(
          "Status of setting SetIntegrityLevel of buffer is false.");
      return;
    }
  }
  v8::ReturnValue<v8::Value> return_value = info.GetReturnValue();
  return_value.Set(Utils::ToLocal(buffer));
}

// WebAssembly.Memory.toResizableBuffer() -> ArrayBuffer
void WebAssemblyMemoryToResizableBufferImpl(
    const v8::FunctionCallbackInfo<v8::Value>& info) {
  WasmJSApiScope js_api_scope{info, "WebAssembly.Memory.toResizableBuffer()"};
  auto [isolate, i_isolate, thrower] = js_api_scope.isolates_and_thrower();
  EXTRACT_THIS(receiver, WasmMemoryObject);

  i::DirectHandle<i::Object> buffer_obj(receiver->array_buffer(), i_isolate);
  DCHECK(IsJSArrayBuffer(*buffer_obj));
  i::DirectHandle<i::JSArrayBuffer> buffer(
      i::Cast<i::JSArrayBuffer>(*buffer_obj), i_isolate);
  if (!buffer->is_resizable_by_js()) {
    if (!receiver->has_maximum_pages()) {
      thrower.TypeError("Memory must have a maximum");
      return;
    }
    buffer = i::WasmMemoryObject::ToResizableBuffer(i_isolate, receiver);
  }
  if (buffer->is_shared()) {
    Maybe<bool> result =
        buffer->SetIntegrityLevel(i_isolate, buffer, i::FROZEN, i::kDontThrow);
    if (!result.FromJust()) {
      thrower.TypeError(
          "Status of setting SetIntegrityLevel of buffer is false.");
      return;
    }
  }
  v8::ReturnValue<v8::Value> return_value = info.GetReturnValue();
  return_value.Set(Utils::ToLocal(buffer));
}

// WebAssembly.Memory.type() -> MemoryType
void WebAssemblyMemoryType(const v8::FunctionCallbackInfo<v8::Value>& info) {
  WasmJSApiScope js_api_scope{info, "WebAssembly.Memory.type()"};
  auto [isolate, i_isolate, thrower] = js_api_scope.isolates_and_thrower();
  EXTRACT_THIS(memory, WasmMemoryObject);

  i::DirectHandle<i::JSArrayBuffer> buffer(memory->array_buffer(), i_isolate);
  size_t curr_size = buffer->GetByteLength() / i::wasm::kWasmPageSize;
  DCHECK_LE(curr_size, std::numeric_limits<uint32_t>::max());
  uint32_t min_size = static_cast<uint32_t>(curr_size);
  std::optional<uint32_t> max_size;
  if (memory->has_maximum_pages()) {
    uint64_t max_size64 = memory->maximum_pages();
    DCHECK_LE(max_size64, std::numeric_limits<uint32_t>::max());
    max_size.emplace(static_cast<uint32_t>(max_size64));
  }
  bool shared = buffer->is_shared();
  auto type = i::wasm::GetTypeForMemory(i_isolate, min_size, max_size, shared,
                                        memory->address_type());
  info.GetReturnValue().Set(Utils::ToLocal(type));
}

// WebAssembly.Tag.type() -> FunctionType
void WebAssemblyTagType(const v8::FunctionCallbackInfo<v8::Value>& info) {
  WasmJSApiScope js_api_scope{info, "WebAssembly.Tag.type()"};
  auto [isolate, i_isolate, thrower] = js_api_scope.isolates_and_thrower();
  EXTRACT_THIS(tag, WasmTagObject);

  int n = tag->serialized_signature()->length();
  std::vector<i::wasm::ValueType> data(n);
  if (n > 0) {
    tag->serialized_signature()->copy_out(0, data.data(), n);
  }
  const i::wasm::FunctionSig sig{0, data.size(), data.data()};
  constexpr bool kForException = true;
  auto type = i::wasm::GetTypeForFunction(i_isolate, &sig, kForException);
  info.GetReturnValue().Set(Utils::ToLocal(type));
}

void WebAssemblyExceptionGetArgImpl(
    const v8::FunctionCallbackInfo<v8::Value>& info) {
  WasmJSApiScope js_api_scope{info, "WebAssembly.Exception.getArg()"};
  auto [isolate, i_isolate, thrower] = js_api_scope.isolates_and_thrower();
  EXTRACT_THIS(exception, WasmExceptionPackage);

  i::DirectHandle<i::WasmTagObject> tag_object;
  if (!GetFirstArgumentAsTag(info, &thrower).ToHandle(&tag_object)) {
    return js_api_scope.AssertException();
  }
  Local<Context> context = isolate->GetCurrentContext();
  std::optional<uint32_t> maybe_index =
      EnforceUint32("Index", info[1], context, &thrower);
  if (!maybe_index) return js_api_scope.AssertException();
  uint32_t index = *maybe_index;
  auto maybe_values =
      i::WasmExceptionPackage::GetExceptionValues(i_isolate, exception);

  auto this_tag =
      i::WasmExceptionPackage::GetExceptionTag(i_isolate, exception);
  DCHECK(IsWasmExceptionTag(*this_tag));
  if (tag_object->tag() != *this_tag) {
    thrower.TypeError("First argument does not match the exception tag");
    return;
  }

  DCHECK(!IsUndefined(*maybe_values));
  auto values = i::Cast<i::FixedArray>(maybe_values);
  auto signature = tag_object->serialized_signature();
  if (index >= static_cast<uint32_t>(signature->length())) {
    thrower.RangeError("Index out of range");
    return;
  }
  // First, find the index in the values array.
  uint32_t decode_index = 0;
  // Since the bounds check above passed, the cast to int is safe.
  for (int i = 0; i < static_cast<int>(index); ++i) {
    switch (signature->get(i).kind()) {
      case i::wasm::kI32:
      case i::wasm::kF32:
        decode_index += 2;
        break;
      case i::wasm::kI64:
      case i::wasm::kF64:
        decode_index += 4;
        break;
      case i::wasm::kRef:
      case i::wasm::kRefNull:
        decode_index++;
        break;
      case i::wasm::kS128:
        decode_index += 8;
        break;
      case i::wasm::kI8:
      case i::wasm::kI16:
      case i::wasm::kF16:
      case i::wasm::kVoid:
      case i::wasm::kTop:
      case i::wasm::kBottom:
        UNREACHABLE();
    }
  }
  // Decode the value at {decode_index}.
  Local<Value> result;
  switch (signature->get(index).kind()) {
    case i::wasm::kI32: {
      uint32_t u32_bits = 0;
      i::DecodeI32ExceptionValue(values, &decode_index, &u32_bits);
      int32_t i32 = static_cast<int32_t>(u32_bits);
      result = v8::Integer::New(isolate, i32);
      break;
    }
    case i::wasm::kI64: {
      uint64_t u64_bits = 0;
      i::DecodeI64ExceptionValue(values, &decode_index, &u64_bits);
      int64_t i64 = static_cast<int64_t>(u64_bits);
      result = v8::BigInt::New(isolate, i64);
      break;
    }
    case i::wasm::kF32: {
      uint32_t f32_bits = 0;
      DecodeI32ExceptionValue(values, &decode_index, &f32_bits);
      float f32 = base::bit_cast<float>(f32_bits);
      result = v8::Number::New(isolate, f32);
      break;
    }
    case i::wasm::kF64: {
      uint64_t f64_bits = 0;
      DecodeI64ExceptionValue(values, &decode_index, &f64_bits);
      double f64 = base::bit_cast<double>(f64_bits);
      result = v8::Number::New(isolate, f64);
      break;
    }
    case i::wasm::kRef:
    case i::wasm::kRefNull: {
      i::DirectHandle<i::Object> obj(values->get(decode_index), i_isolate);
      ReturnValue<Value> return_value = info.GetReturnValue();
      if (!WasmObjectToJSReturnValue(return_value, obj, signature->get(index),
                                     i_isolate, &thrower)) {
        return js_api_scope.AssertException();
      }
      return;
    }
    case i::wasm::kS128:
      thrower.TypeError("Invalid type v128");
      return;
    case i::wasm::kI8:
    case i::wasm::kI16:
    case i::wasm::kF16:
    case i::wasm::kVoid:
    case i::wasm::kTop:
    case i::wasm::kBottom:
      UNREACHABLE();
  }
  info.GetReturnValue().Set(result);
}

void WebAssemblyExceptionIsImpl(
    const v8::FunctionCallbackInfo<v8::Value>& info) {
  WasmJSApiScope js_api_scope{info, "WebAssembly.Exception.is()"};
  auto [isolate, i_isolate, thrower] = js_api_scope.isolates_and_thrower();
  EXTRACT_THIS(exception, WasmExceptionPackage);

  auto tag = i::WasmExceptionPackage::GetExceptionTag(i_isolate, exception);
  DCHECK(IsWasmExceptionTag(*tag));

  i::DirectHandle<i::WasmTagObject> tag_object;
  if (!GetFirstArgumentAsTag(info, &thrower).ToHandle(&tag_object)) {
    return js_api_scope.AssertException();
  }
  info.GetReturnValue().Set(tag_object->tag() == *tag);
}

void WebAssemblyGlobalGetValueCommon(WasmJSApiScope& js_api_scope) {
  auto [isolate, i_isolate, thrower] = js_api_scope.isolates_and_thrower();
  auto& info = js_api_scope.callback_info();  // Needed by EXTRACT_THIS.
  EXTRACT_THIS(receiver, WasmGlobalObject);

  v8::ReturnValue<v8::Value> return_value = info.GetReturnValue();

  switch (receiver->type().kind()) {
    case i::wasm::kI32:
      return_value.Set(receiver->GetI32());
      break;
    case i::wasm::kI64: {
      Local<BigInt> value = BigInt::New(isolate, receiver->GetI64());
      return_value.Set(value);
      break;
    }
    case i::wasm::kF32:
      return_value.Set(receiver->GetF32());
      break;
    case i::wasm::kF64:
      return_value.Set(receiver->GetF64());
      break;
    case i::wasm::kS128:
      thrower.TypeError("Can't get the value of s128 WebAssembly.Global");
      break;
    case i::wasm::kRef:
    case i::wasm::kRefNull:
      if (!WasmObjectToJSReturnValue(return_value, receiver->GetRef(),
                                     receiver->type(), i_isolate, &thrower)) {
        return js_api_scope.AssertException();
      }
      break;
    case i::wasm::kI8:
    case i::wasm::kI16:
    case i::wasm::kF16:
    case i::wasm::kTop:
    case i::wasm::kBottom:
    case i::wasm::kVoid:
      UNREACHABLE();
  }
}

// WebAssembly.Global.valueOf() -> num
void WebAssemblyGlobalValueOfImpl(
    const v8::FunctionCallbackInfo<v8::Value>& info) {
  WasmJSApiScope js_api_scope{info, "WebAssembly.Global.valueOf()"};
  return WebAssemblyGlobalGetValueCommon(js_api_scope);
}

// get WebAssembly.Global.value -> num
void WebAssemblyGlobalGetValueImpl(
    const v8::FunctionCallbackInfo<v8::Value>& info) {
  WasmJSApiScope js_api_scope{info, "get WebAssembly.Global.value)"};
  return WebAssemblyGlobalGetValueCommon(js_api_scope);
}

// set WebAssembly.Global.value(num)
void WebAssemblyGlobalSetValueImpl(
    const v8::FunctionCallbackInfo<v8::Value>& info) {
  WasmJSApiScope js_api_scope{info, "set WebAssembly.Global.value)"};
  auto [isolate, i_isolate, thrower] = js_api_scope.isolates_and_thrower();
  EXTRACT_THIS(receiver, WasmGlobalObject);

  if (!receiver->is_mutable()) {
    thrower.TypeError("Can't set the value of an immutable global.");
    return;
  }
  if (info.Length() == 0) {
    thrower.TypeError("Argument 0 is required");
    return;
  }

  Local<Context> context = isolate->GetCurrentContext();
  switch (receiver->type().kind()) {
    case i::wasm::kI32: {
      int32_t i32_value = 0;
      if (!info[0]->Int32Value(context).To(&i32_value)) {
        return js_api_scope.AssertException();
      }
      receiver->SetI32(i32_value);
      break;
    }
    case i::wasm::kI64: {
      v8::Local<v8::BigInt> bigint_value;
      if (!info[0]->ToBigInt(context).ToLocal(&bigint_value)) {
        return js_api_scope.AssertException();
      }
      receiver->SetI64(bigint_value->Int64Value());
      break;
    }
    case i::wasm::kF32: {
      double f64_value = 0;
      if (!info[0]->NumberValue(context).To(&f64_value)) {
        return js_api_scope.AssertException();
      }
      receiver->SetF32(i::DoubleToFloat32(f64_value));
      break;
    }
    case i::wasm::kF64: {
      double f64_value = 0;
      if (!info[0]->NumberValue(context).To(&f64_value)) {
        return js_api_scope.AssertException();
      }
      receiver->SetF64(f64_value);
      break;
    }
    case i::wasm::kS128:
      thrower.TypeError("Can't set the value of s128 WebAssembly.Global");
      break;
    case i::wasm::kRef:
    case i::wasm::kRefNull: {
      const i::wasm::WasmModule* module =
          receiver->has_trusted_data()
              ? receiver->trusted_data(i_isolate)->module()
              : nullptr;
      i::DirectHandle<i::Object> value = Utils::OpenDirectHandle(*info[0]);
      const char* error_message;
      if (!i::wasm::JSToWasmObject(i_isolate, module, value, receiver->type(),
                                   &error_message)
               .ToHandle(&value)) {
        thrower.TypeError("%s", error_message);
        return;
      }
      receiver->SetRef(value);
      return;
    }
    case i::wasm::kI8:
    case i::wasm::kI16:
    case i::wasm::kF16:
    case i::wasm::kTop:
    case i::wasm::kBottom:
    case i::wasm::kVoid:
      UNREACHABLE();
  }
}

// WebAssembly.Global.type() -> GlobalType
void WebAssemblyGlobalType(const v8::FunctionCallbackInfo<v8::Value>& info) {
  WasmJSApiScope js_api_scope{info, "WebAssembly.Global.type())"};
  auto [isolate, i_isolate, thrower] = js_api_scope.isolates_and_thrower();
  EXTRACT_THIS(global, WasmGlobalObject);

  auto type = i::wasm::GetTypeForGlobal(i_isolate, global->is_mutable(),
                                        global->type());
  info.GetReturnValue().Set(Utils::ToLocal(type));
}

}  // namespace

namespace internal {
namespace wasm {

// Define the callbacks in v8::internal::wasm namespace. The implementation is
// in v8::internal directly.
#define DEF_WASM_JS_EXTERNAL_REFERENCE(Name)                   \
  void Name(const v8::FunctionCallbackInfo<v8::Value>& info) { \
    Name##Impl(info);                                          \
  }
WASM_JS_EXTERNAL_REFERENCE_LIST(DEF_WASM_JS_EXTERNAL_REFERENCE)
#undef DEF_WASM_JS_EXTERNAL_REFERENCE

}  // namespace wasm
}  // namespace internal

// TODO(titzer): we use the API to create the function template because the
// internal guts are too ugly to replicate here.
static i::DirectHandle<i::FunctionTemplateInfo> NewFunctionTemplate(
    i::Isolate* i_isolate, FunctionCallback func, bool has_prototype,
    SideEffectType side_effect_type = SideEffectType::kHasSideEffect) {
  Isolate* isolate = reinterpret_cast<Isolate*>(i_isolate);
  ConstructorBehavior behavior =
      has_prototype ? ConstructorBehavior::kAllow : ConstructorBehavior::kThrow;
  Local<FunctionTemplate> templ = FunctionTemplate::New(
      isolate, func, {}, {}, 0, behavior, side_effect_type);
  if (has_prototype) templ->ReadOnlyPrototype();
  return v8::Utils::OpenDirectHandle(*templ);
}

static i::DirectHandle<i::ObjectTemplateInfo> NewObjectTemplate(
    i::Isolate* i_isolate) {
  Isolate* isolate = reinterpret_cast<Isolate*>(i_isolate);
  Local<ObjectTemplate> templ = ObjectTemplate::New(isolate);
  return v8::Utils::OpenDirectHandle(*templ);
}

namespace internal {
namespace {

DirectHandle<JSFunction> CreateFunc(
    Isolate* isolate, DirectHandle<String> name, FunctionCallback func,
    bool has_prototype,
    SideEffectType side_effect_type = SideEffectType::kHasSideEffect,
    DirectHandle<FunctionTemplateInfo> parent = {}) {
  DirectHandle<FunctionTemplateInfo> temp =
      NewFunctionTemplate(isolate, func, has_prototype, side_effect_type);

  if (!parent.is_null()) {
    DCHECK(has_prototype);
    FunctionTemplateInfo::SetParentTemplate(isolate, temp, parent);
  }

  DirectHandle<JSFunction> function =
      ApiNatives::InstantiateFunction(isolate, temp, name).ToHandleChecked();
  DCHECK(function->shared()->HasSharedName());
  return function;
}

DirectHandle<JSFunction> InstallFunc(
    Isolate* isolate, DirectHandle<JSObject> object, DirectHandle<String> name,
    FunctionCallback func, int length, bool has_prototype = false,
    PropertyAttributes attributes = NONE,
    SideEffectType side_effect_type = SideEffectType::kHasSideEffect) {
  DirectHandle<JSFunction> function =
      CreateFunc(isolate, name, func, has_prototype, side_effect_type);
  function->shared()->set_length(length);
  CHECK(!JSObject::HasRealNamedProperty(isolate, object, name).FromMaybe(true));
  CHECK(object->map()->is_extensible());
  JSObject::AddProperty(isolate, object, name, function, attributes);
  return function;
}

DirectHandle<JSFunction> InstallFunc(
    Isolate* isolate, DirectHandle<JSObject> object, const char* str,
    FunctionCallback func, int length, bool has_prototype = false,
    PropertyAttributes attributes = NONE,
    SideEffectType side_effect_type = SideEffectType::kHasSideEffect) {
  DirectHandle<String> name = v8_str(isolate, str);
  return InstallFunc(isolate, object, name, func, length, has_prototype,
                     attributes, side_effect_type);
}

DirectHandle<JSFunction> InstallConstructorFunc(Isolate* isolate,
                                                DirectHandle<JSObject> object,
                                                const char* str,
                                                FunctionCallback func) {
  return InstallFunc(isolate, object, str, func, 1, true, DONT_ENUM,
                     SideEffectType::kHasNoSideEffect);
}

DirectHandle<String> GetterName(Isolate* isolate, DirectHandle<String> name) {
  return Name::ToFunctionName(isolate, name, isolate->factory()->get_string())
      .ToHandleChecked();
}

void InstallGetter(Isolate* isolate, DirectHandle<JSObject> object,
                   const char* str, FunctionCallback func) {
  DirectHandle<String> name = v8_str(isolate, str);
  DirectHandle<JSFunction> function =
      CreateFunc(isolate, GetterName(isolate, name), func, false,
                 SideEffectType::kHasNoSideEffect);

  Utils::ToLocal(object)->SetAccessorProperty(Utils::ToLocal(name),
                                              Utils::ToLocal(function),
                                              Local<Function>(), v8::None);
}

DirectHandle<String> SetterName(Isolate* isolate, DirectHandle<String> name) {
  return Name::ToFunctionName(isolate, name, isolate->factory()->set_string())
      .ToHandleChecked();
}

void InstallGetterSetter(Isolate* isolate, DirectHandle<JSObject> object,
                         const char* str, FunctionCallback getter,
                         FunctionCallback setter) {
  DirectHandle<String> name = v8_str(isolate, str);
  DirectHandle<JSFunction> getter_func =
      CreateFunc(isolate, GetterName(isolate, name), getter, false,
                 SideEffectType::kHasNoSideEffect);
  DirectHandle<JSFunction> setter_func =
      CreateFunc(isolate, SetterName(isolate, name), setter, false);
  setter_func->shared()->set_length(1);

  Utils::ToLocal(object)->SetAccessorProperty(
      Utils::ToLocal(name), Utils::ToLocal(getter_func),
      Utils::ToLocal(setter_func), v8::None);
}

// Assigns a dummy instance template to the given constructor function. Used to
// make sure the implicit receivers for the constructors in this file have an
// instance type different from the internal one, they allocate the resulting
// object explicitly and ignore implicit receiver.
void SetDummyInstanceTemplate(Isolate* isolate, DirectHandle<JSFunction> fun) {
  DirectHandle<ObjectTemplateInfo> instance_template =
      NewObjectTemplate(isolate);
  FunctionTemplateInfo::SetInstanceTemplate(
      isolate, direct_handle(fun->shared()->api_func_data(), isolate),
      instance_template);
}

DirectHandle<JSObject> SetupConstructor(Isolate* isolate,
                                        DirectHandle<JSFunction> constructor,
                                        InstanceType instance_type,
                                        int instance_size,
                                        const char* name = nullptr,
                                        int in_object_properties = 0) {
  SetDummyInstanceTemplate(isolate, constructor);
  JSFunction::EnsureHasInitialMap(isolate, constructor);
  DirectHandle<JSObject> proto(
      Cast<JSObject>(constructor->instance_prototype()), isolate);
  DirectHandle<Map> map = isolate->factory()->NewContextfulMap(
      constructor, instance_type, instance_size, TERMINAL_FAST_ELEMENTS_KIND,
      in_object_properties);
  JSFunction::SetInitialMap(isolate, constructor, map, proto);
  constexpr PropertyAttributes ro_attributes =
      static_cast<PropertyAttributes>(DONT_ENUM | READ_ONLY);
  if (name) {
    JSObject::AddProperty(isolate, proto,
                          isolate->factory()->to_string_tag_symbol(),
                          v8_str(isolate, name), ro_attributes);
  }
  return proto;
}

constexpr wasm::ValueType kWasmExceptionTagParams[] = {
    wasm::kWasmExternRef,
};
constexpr wasm::FunctionSig kWasmExceptionTagSignature{
    0, arraysize(kWasmExceptionTagParams), kWasmExceptionTagParams};
}  // namespace

// static
void WasmJs::PrepareForSnapshot(Isolate* isolate) {
  DirectHandle<JSGlobalObject> global = isolate->global_object();
  DirectHandle<NativeContext> native_context(global->native_context(), isolate);

  CHECK(IsUndefined(
      native_context->GetNoCell(Context::WASM_WEBASSEMBLY_OBJECT_INDEX),
      isolate));
  CHECK(IsUndefined(
      native_context->GetNoCell(Context::WASM_MODULE_CONSTRUCTOR_INDEX),
      isolate));

  Factory* const f = isolate->factory();
  static constexpr PropertyAttributes ro_attributes =
      static_cast<PropertyAttributes>(DONT_ENUM | READ_ONLY);

  // Create the WebAssembly object.
  DirectHandle<JSObject> webassembly;
  {
    DirectHandle<String> WebAssembly_string = v8_str(isolate, "WebAssembly");
    // Not supposed to be called, hence using the kIllegal builtin as code.
    DirectHandle<SharedFunctionInfo> sfi = f->NewSharedFunctionInfoForBuiltin(
        WebAssembly_string, Builtin::kIllegal, 0, kDontAdapt);
    sfi->set_language_mode(LanguageMode::kStrict);

    DirectHandle<JSFunction> ctor =
        Factory::JSFunctionBuilder{isolate, sfi, native_context}.Build();
    JSFunction::SetPrototype(isolate, ctor,
                             isolate->initial_object_prototype());
    webassembly = f->NewJSObject(ctor, AllocationType::kOld);
    native_context->set_wasm_webassembly_object(*webassembly);

    JSObject::AddProperty(isolate, webassembly, f->to_string_tag_symbol(),
                          WebAssembly_string, ro_attributes);
    InstallFunc(isolate, webassembly, "compile", wasm::WebAssemblyCompile, 1);
    InstallFunc(isolate, webassembly, "validate", wasm::WebAssemblyValidate, 1);
    InstallFunc(isolate, webassembly, "instantiate",
                wasm::WebAssemblyInstantiate, 1);
  }

  // Create the Module object.
  InstallModule(isolate, webassembly);

  // Create the Instance object.
  {
    DirectHandle<JSFunction> instance_constructor = InstallConstructorFunc(
        isolate, webassembly, "Instance", wasm::WebAssemblyInstance);
    DirectHandle<JSObject> instance_proto = SetupConstructor(
        isolate, instance_constructor, WASM_INSTANCE_OBJECT_TYPE,
        WasmInstanceObject::kHeaderSize, "WebAssembly.Instance");
    native_context->set_wasm_instance_constructor(*instance_constructor);
    InstallGetter(isolate, instance_proto, "exports",
                  wasm::WebAssemblyInstanceGetExports);
  }

  // Create the Table object.
  {
    DirectHandle<JSFunction> table_constructor = InstallConstructorFunc(
        isolate, webassembly, "Table", wasm::WebAssemblyTable);
    DirectHandle<JSObject> table_proto =
        SetupConstructor(isolate, table_constructor, WASM_TABLE_OBJECT_TYPE,
                         WasmTableObject::kHeaderSize, "WebAssembly.Table");
    native_context->set_wasm_table_constructor(*table_constructor);
    InstallGetter(isolate, table_proto, "length",
                  wasm::WebAssemblyTableGetLength);
    InstallFunc(isolate, table_proto, "grow", wasm::WebAssemblyTableGrow, 1);
    InstallFunc(isolate, table_proto, "set", wasm::WebAssemblyTableSet, 1);
    InstallFunc(isolate, table_proto, "get", wasm::WebAssemblyTableGet, 1,
                false, NONE, SideEffectType::kHasNoSideEffect);
  }

  // Create the Memory object.
  {
    DirectHandle<JSFunction> memory_constructor = InstallConstructorFunc(
        isolate, webassembly, "Memory", wasm::WebAssemblyMemory);
    DirectHandle<JSObject> memory_proto =
        SetupConstructor(isolate, memory_constructor, WASM_MEMORY_OBJECT_TYPE,
                         WasmMemoryObject::kHeaderSize, "WebAssembly.Memory");
    native_context->set_wasm_memory_constructor(*memory_constructor);
    InstallFunc(isolate, memory_proto, "grow", wasm::WebAssemblyMemoryGrow, 1);
    InstallGetter(isolate, memory_proto, "buffer",
                  wasm::WebAssemblyMemoryGetBuffer);
  }

  // Create the Global object.
  {
    DirectHandle<JSFunction> global_constructor = InstallConstructorFunc(
        isolate, webassembly, "Global", wasm::WebAssemblyGlobal);
    DirectHandle<JSObject> global_proto =
        SetupConstructor(isolate, global_constructor, WASM_GLOBAL_OBJECT_TYPE,
                         WasmGlobalObject::kHeaderSize, "WebAssembly.Global");
    native_context->set_wasm_global_constructor(*global_constructor);
    InstallFunc(isolate, global_proto, "valueOf",
                wasm::WebAssemblyGlobalValueOf, 0, false, NONE,
                SideEffectType::kHasNoSideEffect);
    InstallGetterSetter(isolate, global_proto, "value",
                        wasm::WebAssemblyGlobalGetValue,
                        wasm::WebAssemblyGlobalSetValue);
  }

  // Create the Exception object.
  {
    DirectHandle<JSFunction> tag_constructor = InstallConstructorFunc(
        isolate, webassembly, "Tag", wasm::WebAssemblyTag);
    SetupConstructor(isolate, tag_constructor, WASM_TAG_OBJECT_TYPE,
                     WasmTagObject::kHeaderSize, "WebAssembly.Tag");
    native_context->set_wasm_tag_constructor(*tag_constructor);
    auto js_tag = WasmExceptionTag::New(isolate, 0);
    // Note the canonical_type_index is reset in WasmJs::Install s.t.
    // type_canonicalizer bookkeeping remains valid.
    static constexpr wasm::CanonicalTypeIndex kInitialCanonicalTypeIndex{0};
    DirectHandle<JSObject> js_tag_object = WasmTagObject::New(
        isolate, &kWasmExceptionTagSignature, kInitialCanonicalTypeIndex,
        js_tag, DirectHandle<WasmTrustedInstanceData>());
    native_context->set_wasm_js_tag(*js_tag_object);
    JSObject::AddProperty(isolate, webassembly, "JSTag", js_tag_object,
                          ro_attributes);
  }

  // Set up the runtime exception constructor.
  {
    DirectHandle<JSFunction> exception_constructor = InstallConstructorFunc(
        isolate, webassembly, "Exception", wasm::WebAssemblyException);
    SetDummyInstanceTemplate(isolate, exception_constructor);
    DirectHandle<JSObject> exception_proto = SetupConstructor(
        isolate, exception_constructor, WASM_EXCEPTION_PACKAGE_TYPE,
        WasmExceptionPackage::kSize, "WebAssembly.Exception",
        WasmExceptionPackage::kInObjectFieldCount);
    InstallFunc(isolate, exception_proto, "getArg",
                wasm::WebAssemblyExceptionGetArg, 2);
    InstallFunc(isolate, exception_proto, "is", wasm::WebAssemblyExceptionIs,
                1);
    native_context->set_wasm_exception_constructor(*exception_constructor);

    DirectHandle<Map> initial_map(exception_constructor->initial_map(),
                                  isolate);
    Map::EnsureDescriptorSlack(isolate, initial_map, 2);
    {
      Descriptor d = Descriptor::DataField(
          isolate, f->wasm_exception_tag_symbol(),
          WasmExceptionPackage::kTagIndex, DONT_ENUM, Representation::Tagged());
      initial_map->AppendDescriptor(isolate, &d);
    }
    {
      Descriptor d =
          Descriptor::DataField(isolate, f->wasm_exception_values_symbol(),
                                WasmExceptionPackage::kValuesIndex, DONT_ENUM,
                                Representation::Tagged());
      initial_map->AppendDescriptor(isolate, &d);
    }
  }

  // By default, make all exported functions an instance of {Function}.
  {
    DirectHandle<Map> function_map =
        isolate->sloppy_function_without_prototype_map();
    native_context->set_wasm_exported_function_map(*function_map);
  }

  // Setup errors.
  {
    InstallError(isolate, webassembly, f->CompileError_string(),
                 Context::WASM_COMPILE_ERROR_FUNCTION_INDEX);
    InstallError(isolate, webassembly, f->LinkError_string(),
                 Context::WASM_LINK_ERROR_FUNCTION_INDEX);
    InstallError(isolate, webassembly, f->RuntimeError_string(),
                 Context::WASM_RUNTIME_ERROR_FUNCTION_INDEX);
  }
}

void WasmJs::InstallModule(Isolate* isolate,
                           DirectHandle<JSObject> webassembly) {
  DirectHandle<JSGlobalObject> global = isolate->global_object();
  DirectHandle<NativeContext> native_context(global->native_context(), isolate);

  DirectHandle<JSFunction> module_constructor;
  if (v8_flags.js_source_phase_imports) {
    DirectHandle<FunctionTemplateInfo>
        intrinsic_abstract_module_source_interface_template =
            NewFunctionTemplate(isolate, nullptr, false);
    DirectHandle<JSObject> abstract_module_source_prototype =
        DirectHandle<JSObject>(
            native_context->abstract_module_source_prototype(), isolate);
    ApiNatives::AddDataProperty(
        isolate, intrinsic_abstract_module_source_interface_template,
        isolate->factory()->prototype_string(),
        abstract_module_source_prototype, NONE);

    // Check that this is a reinstallation of the Module object.
    DirectHandle<String> name = v8_str(isolate, "Module");
    CHECK(
        JSObject::HasRealNamedProperty(isolate, webassembly, name).ToChecked());
    // Reinstall the Module object with AbstractModuleSource as prototype.
    module_constructor =
        CreateFunc(isolate, name, wasm::WebAssemblyModule, true,
                   SideEffectType::kHasNoSideEffect,
                   intrinsic_abstract_module_source_interface_template);
    // WebAssembly.Module is a subclass of %AbstractModuleSource%, hence
    // Object.GetPrototypeOf(WebAssembly.Module) === %AbstractModuleSource%.
    JSObject::ForceSetPrototype(
        isolate, module_constructor,
        direct_handle(native_context->abstract_module_source_function(),
                      isolate));
    module_constructor->shared()->set_length(1);
    JSObject::SetOwnPropertyIgnoreAttributes(webassembly, name,
                                             module_constructor, DONT_ENUM)
        .Assert();
  } else {
    module_constructor = InstallConstructorFunc(isolate, webassembly, "Module",
                                                wasm::WebAssemblyModule);
  }
  SetupConstructor(isolate, module_constructor, WASM_MODULE_OBJECT_TYPE,
                   WasmModuleObject::kHeaderSize, "WebAssembly.Module");
  native_context->set_wasm_module_constructor(*module_constructor);

  InstallFunc(isolate, module_constructor, "imports",
              wasm::WebAssemblyModuleImports, 1, false, NONE,
              SideEffectType::kHasNoSideEffect);
  InstallFunc(isolate, module_constructor, "exports",
              wasm::WebAssemblyModuleExports, 1, false, NONE,
              SideEffectType::kHasNoSideEffect);
  InstallFunc(isolate, module_constructor, "customSections",
              wasm::WebAssemblyModuleCustomSections, 2, false, NONE,
              SideEffectType::kHasNoSideEffect);
}

// static
void WasmJs::Install(Isolate* isolate) {
  DirectHandle<JSGlobalObject> global = isolate->global_object();
  DirectHandle<NativeContext> native_context(global->native_context(), isolate);

  if (native_context->is_wasm_js_installed() != Smi::zero()) return;
  native_context->set_is_wasm_js_installed(Smi::FromInt(1));

  // We always use the WebAssembly object from the native context; as this code
  // is executed before any user code, this is expected to be the same as the
  // global "WebAssembly" property. But even later during execution we always
  // want to use this preallocated object instead of whatever user code
  // installed as "WebAssembly" property.
  DirectHandle<JSObject> webassembly(native_context->wasm_webassembly_object(),
                                     isolate);
  if (v8_flags.js_source_phase_imports) {
    // Reinstall the Module object with the experimental interface.
    InstallModule(isolate, webassembly);
  }

  // Expose the API on the global object if not in jitless mode (with more
  // subtleties).
  //
  // Even in interpreter-only mode, wasm currently still creates executable
  // memory at runtime. Unexpose wasm until this changes.
  // The correctness fuzzers are a special case: many of their test cases are
  // built by fetching a random property from the the global object, and thus
  // the global object layout must not change between configs. That is why we
  // continue exposing wasm on correctness fuzzers even in jitless mode.
  // TODO(jgruber): Remove this once / if wasm can run without executable
  // memory.
  bool expose_wasm = !i::v8_flags.jitless ||
                     i::v8_flags.correctness_fuzzer_suppressions ||
                     i::v8_flags.wasm_jitless;
  if (expose_wasm) {
    DirectHandle<String> WebAssembly_string = v8_str(isolate, "WebAssembly");
    JSObject::AddProperty(isolate, global, WebAssembly_string, webassembly,
                          DONT_ENUM);
  }

  {
    // Reset the JSTag's canonical_type_index based on this Isolate's
    // type_canonicalizer.
    DirectHandle<WasmTagObject> js_tag_object(
        Cast<WasmTagObject>(native_context->wasm_js_tag()), isolate);
    js_tag_object->set_canonical_type_index(
        wasm::GetWasmEngine()
            ->type_canonicalizer()
            ->AddRecursiveGroup(&kWasmExceptionTagSignature)
            .index);
  }

  if (v8_flags.wasm_test_streaming) {
    isolate->set_wasm_streaming_callback(WasmStreamingCallbackForTesting);
  }

  if (isolate->wasm_streaming_callback() != nullptr) {
    InstallFunc(isolate, webassembly, "compileStreaming",
                WebAssemblyCompileStreaming, 1);
    InstallFunc(isolate, webassembly, "instantiateStreaming",
                WebAssemblyInstantiateStreaming, 1);
  }

  // The native_context is not set up completely yet. That's why we cannot use
  // {WasmEnabledFeatures::FromIsolate} and have to use
  // {WasmEnabledFeatures::FromFlags} instead.
  const auto enabled_features = wasm::WasmEnabledFeatures::FromFlags();

  if (enabled_features.has_type_reflection()) {
    InstallTypeReflection(isolate, native_context, webassembly);
  }

  if (enabled_features.has_memory_control()) {
    InstallMemoryControl(isolate, native_context, webassembly);
  }

  if (enabled_features.has_custom_descriptors()) {
    InstallCustomDescriptors(isolate, native_context, webassembly);
  }

  // Initialize and install JSPI feature.
  if (enabled_features.has_jspi()) {
    CHECK(native_context->is_wasm_jspi_installed() == Smi::zero());
    isolate->WasmInitJSPIFeature();
    InstallJSPromiseIntegration(isolate, native_context, webassembly);
    native_context->set_is_wasm_jspi_installed(Smi::FromInt(1));
  } else if (v8_flags.stress_wasm_stack_switching) {
    // Set up the JSPI objects necessary for stress-testing stack-switching, but
    // don't install WebAssembly.promising and WebAssembly.Suspending.
    isolate->WasmInitJSPIFeature();
  }

  if (enabled_features.has_rab_integration()) {
    InstallResizableBufferIntegration(isolate, native_context, webassembly);
  }
}

// static
void WasmJs::InstallConditionalFeatures(Isolate* isolate,
                                        DirectHandle<NativeContext> context) {
  DirectHandle<JSObject> webassembly{context->wasm_webassembly_object(),
                                     isolate};
  if (!webassembly->map()->is_extensible()) return;
  if (webassembly->map()->is_access_check_needed()) return;

  // If you need to install some optional features, follow the pattern:
  //
  // if (isolate->IsMyWasmFeatureEnabled(context)) {
  //   DirectHandle<String> feature = isolate->factory()->...;
  //   if (!JSObject::HasRealNamedProperty(isolate, webassembly, feature)
  //            .FromMaybe(true)) {
  //     InstallFeature(isolate, webassembly);
  //   }
  // }

  // Install JSPI-related features.
  if (isolate->IsWasmJSPIRequested(context)) {
    if (context->is_wasm_jspi_installed() == Smi::zero()) {
      isolate->WasmInitJSPIFeature();
      if (InstallJSPromiseIntegration(isolate, context, webassembly)) {
        context->set_is_wasm_jspi_installed(Smi::FromInt(1));
      }
    }
  }
}

// static
// Return true if this call results in JSPI being installed.
bool WasmJs::InstallJSPromiseIntegration(Isolate* isolate,
                                         DirectHandle<NativeContext> context,
                                         DirectHandle<JSObject> webassembly) {
  DirectHandle<String> suspender_string = v8_str(isolate, "Suspender");
  if (JSObject::HasRealNamedProperty(isolate, webassembly, suspender_string)
          .FromMaybe(true)) {
    return false;
  }
  DirectHandle<String> suspending_string = v8_str(isolate, "Suspending");
  if (JSObject::HasRealNamedProperty(isolate, webassembly, suspending_string)
          .FromMaybe(true)) {
    return false;
  }
  DirectHandle<String> promising_string = v8_str(isolate, "promising");
  if (JSObject::HasRealNamedProperty(isolate, webassembly, promising_string)
          .FromMaybe(true)) {
    return false;
  }
  DirectHandle<String> suspend_error_string = v8_str(isolate, "SuspendError");
  if (JSObject::HasRealNamedProperty(isolate, webassembly, suspend_error_string)
          .FromMaybe(true)) {
    return false;
  }
  DirectHandle<JSFunction> suspending_constructor = InstallConstructorFunc(
      isolate, webassembly, "Suspending", WebAssemblySuspendingImpl);
  context->set_wasm_suspending_constructor(*suspending_constructor);
  SetupConstructor(isolate, suspending_constructor, WASM_SUSPENDING_OBJECT_TYPE,
                   WasmSuspendingObject::kHeaderSize, "WebAssembly.Suspending");
  InstallFunc(isolate, webassembly, "promising", WebAssemblyPromising, 1);
  InstallError(isolate, webassembly, isolate->factory()->SuspendError_string(),
               Context::WASM_SUSPEND_ERROR_FUNCTION_INDEX);
  return true;
}

bool WasmJs::InstallCustomDescriptors(Isolate* isolate,
                                      DirectHandle<NativeContext> context,
                                      DirectHandle<JSObject> webassembly) {
  DirectHandle<String> descriptor_options_string =
      v8_str(isolate, "DescriptorOptions");
  if (JSObject::HasRealNamedProperty(isolate, webassembly,
                                     descriptor_options_string)
          .FromMaybe(true)) {
    return false;
  }
  DirectHandle<JSFunction> descriptor_options_constructor =
      InstallConstructorFunc(isolate, webassembly, "DescriptorOptions",
                             WebAssemblyDescriptorOptionsImpl);
  context->set_wasm_descriptor_options_constructor(
      *descriptor_options_constructor);
  SetupConstructor(
      isolate, descriptor_options_constructor, WASM_DESCRIPTOR_OPTIONS_TYPE,
      WasmDescriptorOptions::kHeaderSize, "WebAssembly.DescriptorOptions");
  return true;
}

void WasmJs::InstallMemoryControl(Isolate* isolate,
                                  DirectHandle<NativeContext> context,
                                  DirectHandle<JSObject> webassembly) {
  // Extensibility of the `WebAssembly` object should already have been checked
  // by the caller.
  DCHECK(webassembly->map()->is_extensible());

  DirectHandle<JSFunction> descriptor_constructor =
      InstallConstructorFunc(isolate, webassembly, "MemoryMapDescriptor",
                             wasm::WebAssemblyMemoryMapDescriptor);
  SetupConstructor(
      isolate, descriptor_constructor, WASM_MEMORY_MAP_DESCRIPTOR_TYPE,
      WasmMemoryMapDescriptor::kHeaderSize, "WebAssembly.MemoryMapDescriptor");
  context->set_wasm_memory_map_descriptor_constructor(*descriptor_constructor);

  DirectHandle<JSObject> descriptor_proto = direct_handle(
      Cast<JSObject>(descriptor_constructor->instance_prototype()), isolate);

  InstallFunc(isolate, descriptor_proto, "map",
              wasm::WebAssemblyMemoryMapDescriptorMap, 2);
  InstallFunc(isolate, descriptor_proto, "unmap",
              wasm::WebAssemblyMemoryMapDescriptorUnmap, 0);
}

// Return true only if this call resulted in installation of type reflection.
// static
bool WasmJs::InstallTypeReflection(Isolate* isolate,
                                   DirectHandle<NativeContext> context,
                                   DirectHandle<JSObject> webassembly) {
  // Extensibility of the `WebAssembly` object should already have been checked
  // by the caller.
  DCHECK(webassembly->map()->is_extensible());

  // First check if any of the type reflection fields already exist. If so, bail
  // out and don't install any new fields.
  if (JSObject::HasRealNamedProperty(isolate, webassembly,
                                     isolate->factory()->Function_string())
          .FromMaybe(true)) {
    return false;
  }

  auto GetProto = [isolate](Tagged<JSFunction> constructor) {
    return handle(Cast<JSObject>(constructor->instance_prototype()), isolate);
  };
  DirectHandle<JSObject> table_proto =
      GetProto(context->wasm_table_constructor());
  DirectHandle<JSObject> global_proto =
      GetProto(context->wasm_global_constructor());
  DirectHandle<JSObject> memory_proto =
      GetProto(context->wasm_memory_constructor());
  DirectHandle<JSObject> tag_proto = GetProto(context->wasm_tag_constructor());

  DirectHandle<String> type_string = v8_str(isolate, "type");
  auto CheckProto = [isolate, type_string](DirectHandle<JSObject> proto) {
    if (JSObject::HasRealNamedProperty(isolate, proto, type_string)
            .FromMaybe(true)) {
      return false;
    }
    // Also check extensibility, otherwise adding properties will fail.
    if (!proto->map()->is_extensible()) return false;
    return true;
  };
  if (!CheckProto(table_proto)) return false;
  if (!CheckProto(global_proto)) return false;
  if (!CheckProto(memory_proto)) return false;
  if (!CheckProto(tag_proto)) return false;

  // Checks are done, start installing the new fields.
  InstallFunc(isolate, table_proto, type_string, WebAssemblyTableType, 0, false,
              NONE, SideEffectType::kHasNoSideEffect);
  InstallFunc(isolate, memory_proto, type_string, WebAssemblyMemoryType, 0,
              false, NONE, SideEffectType::kHasNoSideEffect);
  InstallFunc(isolate, global_proto, type_string, WebAssemblyGlobalType, 0,
              false, NONE, SideEffectType::kHasNoSideEffect);
  InstallFunc(isolate, tag_proto, type_string, WebAssemblyTagType, 0, false,
              NONE, SideEffectType::kHasNoSideEffect);

  // Create the Function object.
  DirectHandle<JSFunction> function_constructor = InstallConstructorFunc(
      isolate, webassembly, "Function", WebAssemblyFunction);
  SetDummyInstanceTemplate(isolate, function_constructor);
  JSFunction::EnsureHasInitialMap(isolate, function_constructor);
  DirectHandle<JSObject> function_proto(
      Cast<JSObject>(function_constructor->instance_prototype()), isolate);
  DirectHandle<Map> function_map =
      Map::Copy(isolate, isolate->sloppy_function_without_prototype_map(),
                "WebAssembly.Function");
  CHECK(JSObject::SetPrototype(
            isolate, function_proto,
            direct_handle(context->function_function()->prototype(), isolate),
            false, kDontThrow)
            .FromJust());
  JSFunction::SetInitialMap(isolate, function_constructor, function_map,
                            function_proto);

  constexpr PropertyAttributes ro_attributes =
      static_cast<PropertyAttributes>(DONT_ENUM | READ_ONLY);
  JSObject::AddProperty(isolate, function_proto,
                        isolate->factory()->to_string_tag_symbol(),
                        v8_str(isolate, "WebAssembly.Function"), ro_attributes);

  InstallFunc(isolate, function_proto, type_string, WebAssemblyFunctionType, 0);
  SimpleInstallFunction(isolate, function_proto, "bind",
                        Builtin::kWebAssemblyFunctionPrototypeBind, 1,
                        kDontAdapt);
  // Make all exported functions an instance of {WebAssembly.Function}.
  context->set_wasm_exported_function_map(*function_map);
  return true;
}

// static
void WasmJs::InstallResizableBufferIntegration(
    Isolate* isolate, DirectHandle<NativeContext> context,
    DirectHandle<JSObject> webassembly) {
  // Extensibility of the `WebAssembly` object should already have been checked
  // by the caller.
  DCHECK(webassembly->map()->is_extensible());

  DirectHandle<JSObject> memory_proto = direct_handle(
      Cast<JSObject>(context->wasm_memory_constructor()->instance_prototype()),
      isolate);
  InstallFunc(isolate, memory_proto, "toFixedLengthBuffer",
              wasm::WebAssemblyMemoryToFixedLengthBuffer, 0);
  InstallFunc(isolate, memory_proto, "toResizableBuffer",
              wasm::WebAssemblyMemoryToResizableBuffer, 0);
}

namespace wasm {
// static
std::unique_ptr<WasmStreaming> StartStreamingForTesting(
    Isolate* isolate,
    std::shared_ptr<wasm::CompilationResultResolver> resolver) {
  return std::make_unique<WasmStreaming>(
      std::make_unique<WasmStreaming::WasmStreamingImpl>(
          isolate, "StartStreamingForTesting", CompileTimeImports{}, resolver));
}
}  // namespace wasm

#undef ASSIGN
#undef EXTRACT_THIS

}  // namespace internal
}  // namespace v8
