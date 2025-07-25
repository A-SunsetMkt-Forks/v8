// Copyright 2024 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/wasm/interpreter/wasm-interpreter-runtime.h"

#include <optional>

#include "src/base/platform/time.h"
#include "src/execution/frames-inl.h"
#include "src/execution/isolate.h"
#include "src/objects/managed-inl.h"
#include "src/runtime/runtime-utils.h"
#include "src/wasm/canonical-types.h"
#include "src/wasm/interpreter/wasm-interpreter-objects-inl.h"
#include "src/wasm/interpreter/wasm-interpreter-runtime-inl.h"
#include "src/wasm/wasm-arguments.h"
#include "src/wasm/wasm-opcodes-inl.h"
#include "src/wasm/wasm-subtyping.h"

namespace v8 {
namespace internal {

namespace wasm {

class V8_EXPORT_PRIVATE ValueTypes {
 public:
  static inline int ElementSizeInBytes(ValueType type) {
    switch (type.kind()) {
      case kI32:
      case kF32:
        return 4;
      case kI64:
      case kF64:
        return 8;
      case kS128:
        return 16;
      case kRef:
      case kRefNull:
        return kSystemPointerSize;
      default:
        UNREACHABLE();
    }
  }
};

}  // namespace wasm

namespace {

// Find the frame pointer of the interpreter frame on the stack.
Address FindInterpreterEntryFramePointer(Isolate* isolate) {
  StackFrameIterator it(isolate, isolate->thread_local_top());

  if (it.frame() == nullptr && v8_flags.drumbrake_fuzzing_mode) {
    wasm::WasmInterpreterThread* thread =
        wasm::WasmInterpreterThread::GetCurrentInterpreterThread(isolate);
    DCHECK_NOT_NULL(thread);
    return thread->fuzzer_entry_frame_pointer();
  }

  // On top: C entry stub.
  DCHECK_EQ(StackFrame::EXIT, it.frame()->type());
  it.Advance();
  // Next: the wasm interpreter entry.
  DCHECK_EQ(StackFrame::WASM_INTERPRETER_ENTRY, it.frame()->type());
  return it.frame()->fp();
}

}  // namespace

RUNTIME_FUNCTION(Runtime_WasmRunInterpreter) {
  DCHECK_EQ(3, args.length());
  HandleScope scope(isolate);
  DirectHandle<WasmInstanceObject> instance = args.at<WasmInstanceObject>(0);
  DirectHandle<WasmTrustedInstanceData> trusted_data(
      instance->trusted_data(isolate), isolate);
  int32_t func_index = NumberToInt32(args[1]);
  DirectHandle<Object> arg_buffer_obj = args.at(2);

  // The arg buffer is the raw pointer to the caller's stack. It looks like a
  // Smi (lowest bit not set, as checked by IsSmi), but is no valid Smi. We just
  // cast it back to the raw pointer.
  CHECK(!IsHeapObject(*arg_buffer_obj));
  CHECK(IsSmi(*arg_buffer_obj));
  Address arg_buffer = (*arg_buffer_obj).ptr();

  // Reserve buffers for argument and return values.
  DCHECK_GT(trusted_data->module()->functions.size(), func_index);
  const wasm::FunctionSig* sig =
      trusted_data->module()->functions[func_index].sig;
  DCHECK_GE(kMaxInt, sig->parameter_count());
  int num_params = static_cast<int>(sig->parameter_count());
  std::vector<wasm::WasmValue> wasm_args(num_params);
  DCHECK_GE(kMaxInt, sig->return_count());
  int num_returns = static_cast<int>(sig->return_count());
  std::vector<wasm::WasmValue> wasm_rets(num_returns);

  // Set the current isolate's context.
  isolate->set_context(trusted_data->native_context());

  // Make sure the WasmInterpreterObject and InterpreterHandle for this instance
  // exist.
  DirectHandle<Tuple2> interpreter_object =
      WasmTrustedInstanceData::GetOrCreateInterpreterObject(instance);
  wasm::InterpreterHandle* interpreter_handle =
      wasm::GetOrCreateInterpreterHandle(isolate, interpreter_object);

  if (wasm::WasmBytecode::ContainsSimd(sig)) {
    interpreter_handle->SetTrapFunctionIndex(func_index);
    isolate->Throw(*isolate->factory()->NewTypeError(
        MessageTemplate::kWasmTrapJSTypeError));
    return ReadOnlyRoots(isolate).exception();
  }

  Address frame_pointer = FindInterpreterEntryFramePointer(isolate);

  // If there are Ref arguments or return values, we store their pointers into
  // an array of bytes so we need to disable GC until they are unpacked by the
  // callee.
  {
    DisallowHeapAllocation no_gc;

    // Copy the arguments for the {arg_buffer} into a vector of {WasmValue}.
    // This also boxes reference types into handles, which needs to happen
    // before any methods that could trigger a GC are being called.
    Address arg_buf_ptr = arg_buffer;
    for (int i = 0; i < num_params; ++i) {
#define CASE_ARG_TYPE(type, ctype)                                     \
  case wasm::type:                                                     \
    DCHECK_EQ(wasm::ValueTypes::ElementSizeInBytes(sig->GetParam(i)),  \
              sizeof(ctype));                                          \
    wasm_args[i] =                                                     \
        wasm::WasmValue(base::ReadUnalignedValue<ctype>(arg_buf_ptr)); \
    arg_buf_ptr += sizeof(ctype);                                      \
    break;

      wasm::ValueType value_type = sig->GetParam(i);
      wasm::ValueKind kind = value_type.kind();
      switch (kind) {
        CASE_ARG_TYPE(kWasmI32.kind(), uint32_t)
        CASE_ARG_TYPE(kWasmI64.kind(), uint64_t)
        CASE_ARG_TYPE(kWasmF32.kind(), float)
        CASE_ARG_TYPE(kWasmF64.kind(), double)
#undef CASE_ARG_TYPE
        case wasm::kRef:
        case wasm::kRefNull: {
          DCHECK_EQ(wasm::ValueTypes::ElementSizeInBytes(sig->GetParam(i)),
                    kSystemPointerSize);
          // MarkCompactCollector::RootMarkingVisitor requires ref slots to be
          // 64-bit aligned.
          arg_buf_ptr += (arg_buf_ptr & 0x04);

          DirectHandle<Object> ref(
              base::ReadUnalignedValue<Tagged<Object>>(arg_buf_ptr), isolate);

          const wasm::WasmInterpreterRuntime* wasm_runtime =
              interpreter_handle->interpreter()->GetWasmRuntime();
          ref = wasm_runtime->JSToWasmObject(ref, value_type);
          if (isolate->has_exception()) {
            interpreter_handle->SetTrapFunctionIndex(func_index);
            return ReadOnlyRoots(isolate).exception();
          }

          if ((value_type != wasm::kWasmExternRef &&
               value_type != wasm::kWasmNullExternRef) &&
              IsNull(*ref, isolate)) {
            ref = isolate->factory()->wasm_null();
          }

          wasm_args[i] = wasm::WasmValue(ref, wasm::kWasmAnyRef);
          arg_buf_ptr += kSystemPointerSize;
          break;
        }
        case wasm::kWasmS128.kind():
        default:
          UNREACHABLE();
      }
    }

    // Run the function in the interpreter. Note that neither the
    // {WasmInterpreterObject} nor the {InterpreterHandle} have to exist,
    // because interpretation might have been triggered by another Isolate
    // sharing the same WasmEngine.
    bool success = WasmInterpreterObject::RunInterpreter(
        isolate, frame_pointer, instance, func_index, wasm_args, wasm_rets);

    // Early return on failure.
    if (!success) {
      DCHECK(isolate->has_exception());
      return ReadOnlyRoots(isolate).exception();
    }

    // Copy return values from the vector of {WasmValue} into {arg_buffer}. This
    // also un-boxes reference types from handles into raw pointers.
    arg_buf_ptr = arg_buffer;

    for (int i = 0; i < num_returns; ++i) {
#define CASE_RET_TYPE(type, ctype)                                           \
  case wasm::type:                                                           \
    DCHECK_EQ(wasm::ValueTypes::ElementSizeInBytes(sig->GetReturn(i)),       \
              sizeof(ctype));                                                \
    base::WriteUnalignedValue<ctype>(arg_buf_ptr, wasm_rets[i].to<ctype>()); \
    arg_buf_ptr += sizeof(ctype);                                            \
    break;

      switch (sig->GetReturn(i).kind()) {
        CASE_RET_TYPE(kWasmI32.kind(), uint32_t)
        CASE_RET_TYPE(kWasmI64.kind(), uint64_t)
        CASE_RET_TYPE(kWasmF32.kind(), float)
        CASE_RET_TYPE(kWasmF64.kind(), double)
#undef CASE_RET_TYPE
        case wasm::kRef:
        case wasm::kRefNull: {
          DCHECK_EQ(wasm::ValueTypes::ElementSizeInBytes(sig->GetReturn(i)),
                    kSystemPointerSize);
          DirectHandle<Object> ref = wasm_rets[i].to_ref();
          // Note: WasmToJSObject(ref) already called in ContinueExecution or
          // CallExternalJSFunction.

          // Make sure ref slots are 64-bit aligned.
          arg_buf_ptr += (arg_buf_ptr & 0x04);
          base::WriteUnalignedValue<Tagged<Object>>(arg_buf_ptr, *ref);
          arg_buf_ptr += kSystemPointerSize;
          break;
        }
        case wasm::kWasmS128.kind():
        default:
          UNREACHABLE();
      }
    }

    return ReadOnlyRoots(isolate).undefined_value();
  }
}

namespace wasm {

V8_EXPORT_PRIVATE InterpreterHandle* GetInterpreterHandle(
    Isolate* isolate, DirectHandle<Tuple2> interpreter_object) {
  DirectHandle<Object> handle(
      WasmInterpreterObject::get_interpreter_handle(*interpreter_object),
      isolate);
  CHECK(!IsUndefined(*handle, isolate));
  return Cast<Managed<InterpreterHandle>>(handle)->raw();
}

V8_EXPORT_PRIVATE InterpreterHandle* GetOrCreateInterpreterHandle(
    Isolate* isolate, DirectHandle<Tuple2> interpreter_object) {
  DirectHandle<Object> handle(
      WasmInterpreterObject::get_interpreter_handle(*interpreter_object),
      isolate);
  if (IsUndefined(*handle, isolate)) {
    // Use the maximum stack size to estimate the maximum size of the
    // interpreter. The interpreter keeps its own stack internally, and the size
    // of the stack should dominate the overall size of the interpreter. We
    // multiply by '2' to account for the growing strategy for the backing store
    // of the stack.
    size_t interpreter_size = v8_flags.stack_size * KB * 2;
    handle = Managed<InterpreterHandle>::From(
        isolate, interpreter_size,
        std::make_shared<InterpreterHandle>(isolate, interpreter_object));
    WasmInterpreterObject::set_interpreter_handle(*interpreter_object, *handle);
  }

  return Cast<Managed<InterpreterHandle>>(handle)->raw();
}

// A helper for an entry in an indirect function table (IFT).
// The underlying storage in the instance is used by generated code to
// call functions indirectly at runtime.
// Each entry has the following fields:
// - implicit_arg = A WasmTrustedInstanceData or a WasmImportData.
// - sig_id = signature id of function.
// - target = entrypoint to Wasm code or import wrapper code.
// - function_index = function index, if a Wasm function, or
// WasmDispatchTable::kInvalidFunctionIndex otherwise.
class IndirectFunctionTableEntry {
 public:
  inline IndirectFunctionTableEntry(DirectHandle<WasmInstanceObject>,
                                    int table_index, int entry_index);

  inline Tagged<Object> implicit_arg() const {
    return table_->implicit_arg(index_);
  }
  inline wasm::CanonicalTypeIndex sig_id() const { return table_->sig(index_); }
  inline WasmCodePointer target() const { return table_->target(index_); }
  inline uint32_t function_index() const {
    return table_->function_index(index_);
  }

 private:
  DirectHandle<WasmDispatchTable> const table_;
  int const index_;
};

IndirectFunctionTableEntry::IndirectFunctionTableEntry(
    DirectHandle<WasmInstanceObject> instance, int table_index, int entry_index)
    : table_(table_index != 0
                 ? direct_handle(Cast<WasmDispatchTable>(
                                     instance->trusted_data(Isolate::Current())
                                         ->dispatch_tables()
                                         ->get(table_index)),
                                 Isolate::Current())
                 : direct_handle(Cast<WasmDispatchTable>(
                                     instance->trusted_data(Isolate::Current())
                                         ->dispatch_table0()),
                                 Isolate::Current())),
      index_(entry_index) {
  DCHECK_GE(entry_index, 0);
  DCHECK_LT(entry_index, table_->length());
}

WasmInterpreterRuntime::WasmInterpreterRuntime(
    const WasmModule* module, Isolate* isolate,
    IndirectHandle<WasmInstanceObject> instance_object,
    WasmInterpreter::CodeMap* codemap)
    : isolate_(isolate),
      module_(module),
      instance_object_(instance_object),
      codemap_(codemap),
      start_function_index_(UINT_MAX),
      trap_function_index_(-1),
      trap_pc_(0),
      current_thread_(nullptr),
      fuzzer_start_time_(base::TimeTicks::Now()),
      memory_start_(nullptr),
      instruction_table_(kInstructionTable),
      generic_wasm_to_js_interpreter_wrapper_fn_(
          GeneratedCode<WasmToJSCallSig>::FromAddress(isolate, {}))
#ifdef V8_ENABLE_DRUMBRAKE_TRACING
      ,
      shadow_stack_(nullptr)
#endif  // V8_ENABLE_DRUMBRAKE_TRACING
{
  DCHECK(v8_flags.wasm_jitless);

  InitGlobalAddressCache();
  InitMemoryAddresses();
  InitIndirectFunctionTables();

  // Initialize address of GenericWasmToJSInterpreterWrapper builtin.
  Address wasm_to_js_code_addr_addr =
      isolate->isolate_root() +
      IsolateData::BuiltinEntrySlotOffset(Builtin::kWasmInterpreterCWasmEntry);
  Address wasm_to_js_code_addr =
      *reinterpret_cast<Address*>(wasm_to_js_code_addr_addr);
  generic_wasm_to_js_interpreter_wrapper_fn_ =
      GeneratedCode<WasmToJSCallSig>::FromAddress(isolate,
                                                  wasm_to_js_code_addr);
}

void WasmInterpreterRuntime::InitGlobalAddressCache() {
  global_addresses_.resize(module_->globals.size());
  for (size_t index = 0; index < module_->globals.size(); index++) {
    const WasmGlobal& global = module_->globals[index];
    if (!global.type.is_reference()) {
      global_addresses_[index] =
          wasm_trusted_instance_data()->GetGlobalStorage(global);
    }
  }
}

// static
void WasmInterpreterRuntime::UpdateMemoryAddress(
    DirectHandle<WasmInstanceObject> instance) {
  Isolate* isolate = Isolate::Current();
  DirectHandle<Tuple2> interpreter_object =
      WasmTrustedInstanceData::GetOrCreateInterpreterObject(instance);
  InterpreterHandle* handle =
      GetOrCreateInterpreterHandle(isolate, interpreter_object);
  WasmInterpreterRuntime* wasm_runtime =
      handle->interpreter()->GetWasmRuntime();
  wasm_runtime->InitMemoryAddresses();
}

int32_t WasmInterpreterRuntime::MemoryGrow(uint32_t delta_pages) {
  HandleScope handle_scope(isolate_);  // Avoid leaking handles.
  // TODO(paolosev@microsoft.com): Support multiple memories.
  uint32_t memory_index = 0;
  DirectHandle<WasmMemoryObject> memory(
      wasm_trusted_instance_data()->memory_object(memory_index), isolate_);
  int32_t result = WasmMemoryObject::Grow(isolate_, memory, delta_pages);
  InitMemoryAddresses();
  return result;
}

void WasmInterpreterRuntime::InitIndirectFunctionTables() {
  int table_count = static_cast<int>(module_->tables.size());
  indirect_call_tables_.resize(table_count);
  for (int table_index = 0; table_index < table_count; ++table_index) {
    PurgeIndirectCallCache(table_index);
  }
}

bool WasmInterpreterRuntime::TableGet(const uint8_t*& current_code,
                                      uint32_t table_index,
                                      uint32_t entry_index,
                                      DirectHandle<Object>* result) {
  // This function assumes that it is executed in a HandleScope.

  auto table = direct_handle(
      Cast<WasmTableObject>(
          wasm_trusted_instance_data()->tables()->get(table_index)),
      isolate_);
  uint32_t table_size = table->current_length();
  if (entry_index >= table_size) {
    SetTrap(TrapReason::kTrapTableOutOfBounds, current_code);
    return false;
  }

  *result = WasmTableObject::Get(isolate_, table, entry_index);
  return true;
}

void WasmInterpreterRuntime::TableSet(const uint8_t*& current_code,
                                      uint32_t table_index,
                                      uint32_t entry_index,
                                      DirectHandle<Object> ref) {
  // This function assumes that it is executed in a HandleScope.

  auto table = direct_handle(
      Cast<WasmTableObject>(
          wasm_trusted_instance_data()->tables()->get(table_index)),
      isolate_);
  uint32_t table_size = table->current_length();
  if (entry_index >= table_size) {
    SetTrap(TrapReason::kTrapTableOutOfBounds, current_code);
  } else {
    WasmTableObject::Set(isolate_, table, entry_index, ref);
  }
}

void WasmInterpreterRuntime::TableInit(const uint8_t*& current_code,
                                       uint32_t table_index,
                                       uint32_t element_segment_index,
                                       uint32_t dst, uint32_t src,
                                       uint32_t size) {
  HandleScope scope(isolate_);  // Avoid leaking handles.

  DirectHandle<WasmTrustedInstanceData> trusted_data =
      wasm_trusted_instance_data();
  auto table = direct_handle(
      Cast<WasmTableObject>(trusted_data->tables()->get(table_index)),
      isolate_);
  if (IsSubtypeOf(table->type(module_), kWasmFuncRef, module_)) {
    PurgeIndirectCallCache(table_index);
  }

  std::optional<MessageTemplate> msg_template =
      WasmTrustedInstanceData::InitTableEntries(
          Isolate::Current(), trusted_data, trusted_data, table_index,
          element_segment_index, dst, src, size);
  // See WasmInstanceObject::InitTableEntries.
  if (msg_template == MessageTemplate::kWasmTrapTableOutOfBounds) {
    SetTrap(TrapReason::kTrapTableOutOfBounds, current_code);
  } else if (msg_template ==
             MessageTemplate::kWasmTrapElementSegmentOutOfBounds) {
    SetTrap(TrapReason::kTrapElementSegmentOutOfBounds, current_code);
  }
}

void WasmInterpreterRuntime::TableCopy(const uint8_t*& current_code,
                                       uint32_t dst_table_index,
                                       uint32_t src_table_index, uint32_t dst,
                                       uint32_t src, uint32_t size) {
  HandleScope scope(isolate_);  // Avoid leaking handles.

  DirectHandle<WasmTrustedInstanceData> trusted_data =
      wasm_trusted_instance_data();
  auto table_dst = direct_handle(
      Cast<WasmTableObject>(trusted_data->tables()->get(dst_table_index)),
      isolate_);
  if (IsSubtypeOf(table_dst->type(module_), kWasmFuncRef, module_)) {
    PurgeIndirectCallCache(dst_table_index);
  }

  if (!WasmTrustedInstanceData::CopyTableEntries(
          isolate_, trusted_data, dst_table_index, src_table_index, dst, src,
          size)) {
    SetTrap(TrapReason::kTrapTableOutOfBounds, current_code);
  }
}

uint32_t WasmInterpreterRuntime::TableGrow(uint32_t table_index, uint32_t delta,
                                           DirectHandle<Object> value) {
  // This function assumes that it is executed in a HandleScope.

  auto table = direct_handle(
      Cast<WasmTableObject>(
          wasm_trusted_instance_data()->tables()->get(table_index)),
      isolate_);
  return WasmTableObject::Grow(isolate_, table, delta, value);
}

uint32_t WasmInterpreterRuntime::TableSize(uint32_t table_index) {
  HandleScope handle_scope(isolate_);  // Avoid leaking handles.
  auto table = direct_handle(
      Cast<WasmTableObject>(
          wasm_trusted_instance_data()->tables()->get(table_index)),
      isolate_);
  return table->current_length();
}

void WasmInterpreterRuntime::TableFill(const uint8_t*& current_code,
                                       uint32_t table_index, uint32_t count,
                                       DirectHandle<Object> value,
                                       uint32_t start) {
  // This function assumes that it is executed in a HandleScope.

  auto table = direct_handle(
      Cast<WasmTableObject>(
          wasm_trusted_instance_data()->tables()->get(table_index)),
      isolate_);
  uint32_t table_size = table->current_length();
  if (start + count < start ||  // Check for overflow.
      start + count > table_size) {
    SetTrap(TrapReason::kTrapTableOutOfBounds, current_code);
    return;
  }

  if (count == 0) {
    return;
  }

  WasmTableObject::Fill(isolate_, table, start, value, count);
}

bool WasmInterpreterRuntime::MemoryInit(const uint8_t*& current_code,
                                        uint32_t data_segment_index,
                                        uint64_t dst, uint64_t src,
                                        uint64_t size) {
  DirectHandle<WasmTrustedInstanceData> trusted_data =
      wasm_trusted_instance_data();
  Address dst_addr;
  uint64_t src_max =
      trusted_data->data_segment_sizes()->get(data_segment_index);
  if (!BoundsCheckMemRange(dst, &size, &dst_addr) ||
      !base::IsInBounds(src, size, src_max)) {
    SetTrap(TrapReason::kTrapMemOutOfBounds, current_code);
    return false;
  }

  Address src_addr =
      trusted_data->data_segment_starts()->get(data_segment_index) + src;
  std::memmove(reinterpret_cast<void*>(dst_addr),
               reinterpret_cast<void*>(src_addr), size);
  return true;
}

bool WasmInterpreterRuntime::MemoryCopy(const uint8_t*& current_code,
                                        uint64_t dst, uint64_t src,
                                        uint64_t size) {
  Address dst_addr;
  Address src_addr;
  if (!BoundsCheckMemRange(dst, &size, &dst_addr) ||
      !BoundsCheckMemRange(src, &size, &src_addr)) {
    SetTrap(TrapReason::kTrapMemOutOfBounds, current_code);
    return false;
  }

  std::memmove(reinterpret_cast<void*>(dst_addr),
               reinterpret_cast<void*>(src_addr), size);
  return true;
}

bool WasmInterpreterRuntime::MemoryFill(const uint8_t*& current_code,
                                        uint64_t dst, uint32_t value,
                                        uint64_t size) {
  Address dst_addr;
  if (!BoundsCheckMemRange(dst, &size, &dst_addr)) {
    SetTrap(TrapReason::kTrapMemOutOfBounds, current_code);
    return false;
  }

  std::memset(reinterpret_cast<void*>(dst_addr), value, size);
  return true;
}

// Unpack the values encoded in the given exception. The exception values are
// pushed onto the operand stack.
void WasmInterpreterRuntime::UnpackException(
    uint32_t* sp, const WasmTag& tag, DirectHandle<Object> exception_object,
    uint32_t first_param_slot_index, uint32_t first_param_ref_stack_index) {
  DirectHandle<FixedArray> encoded_values =
      Cast<FixedArray>(WasmExceptionPackage::GetExceptionValues(
          isolate_, Cast<WasmExceptionPackage>(exception_object)));
  // Decode the exception values from the given exception package and push
  // them onto the operand stack. This encoding has to be in sync with other
  // backends so that exceptions can be passed between them.
  const WasmTagSig* sig = tag.sig;
  uint32_t encoded_index = 0;
  uint32_t* p = sp + first_param_slot_index;
  for (size_t i = 0; i < sig->parameter_count(); ++i) {
#ifdef V8_ENABLE_DRUMBRAKE_TRACING
    if (v8_flags.trace_drumbrake_execution) {
      TracePush(sig->GetParam(i).kind(), static_cast<uint32_t>(p - sp));
    }
#endif  // V8_ENABLE_DRUMBRAKE_TRACING

    WasmValue value;
    switch (sig->GetParam(i).kind()) {
      case kI32: {
        uint32_t u32 = 0;
        DecodeI32ExceptionValue(encoded_values, &encoded_index, &u32);
        base::WriteUnalignedValue<uint32_t>(reinterpret_cast<Address>(p), u32);
        p += sizeof(uint32_t) / kSlotSize;
        break;
      }
      case kF32: {
        uint32_t f32_bits = 0;
        DecodeI32ExceptionValue(encoded_values, &encoded_index, &f32_bits);
        float f32 = Float32::FromBits(f32_bits).get_scalar();
        base::WriteUnalignedValue<float>(reinterpret_cast<Address>(p), f32);
        p += sizeof(float) / kSlotSize;
        break;
      }
      case kI64: {
        uint64_t u64 = 0;
        DecodeI64ExceptionValue(encoded_values, &encoded_index, &u64);
        base::WriteUnalignedValue<uint64_t>(reinterpret_cast<Address>(p), u64);
        p += sizeof(uint64_t) / kSlotSize;
        break;
      }
      case kF64: {
        uint64_t f64_bits = 0;
        DecodeI64ExceptionValue(encoded_values, &encoded_index, &f64_bits);
        float f64 = Float64::FromBits(f64_bits).get_scalar();
        base::WriteUnalignedValue<double>(reinterpret_cast<Address>(p), f64);
        p += sizeof(double) / kSlotSize;
        break;
      }
      case kS128: {
        int32x4 s128 = {0, 0, 0, 0};
        uint32_t* vals = reinterpret_cast<uint32_t*>(s128.val);
        DecodeI32ExceptionValue(encoded_values, &encoded_index, &vals[0]);
        DecodeI32ExceptionValue(encoded_values, &encoded_index, &vals[1]);
        DecodeI32ExceptionValue(encoded_values, &encoded_index, &vals[2]);
        DecodeI32ExceptionValue(encoded_values, &encoded_index, &vals[3]);
        base::WriteUnalignedValue<Simd128>(reinterpret_cast<Address>(p),
                                           Simd128(s128));
        p += sizeof(Simd128) / kSlotSize;
        break;
      }
      case kRef:
      case kRefNull: {
        DirectHandle<Object> ref(encoded_values->get(encoded_index++),
                                 isolate_);
        if (sig->GetParam(i).value_type_code() == wasm::kFuncRefCode &&
            i::IsNull(*ref, isolate_)) {
          ref = isolate_->factory()->wasm_null();
        }
        StoreWasmRef(first_param_ref_stack_index++, ref);
        base::WriteUnalignedValue<WasmRef>(reinterpret_cast<Address>(p), ref);
        p += sizeof(WasmRef) / kSlotSize;
        break;
      }
      default:
        UNREACHABLE();
    }
  }
  DCHECK_EQ(WasmExceptionPackage::GetEncodedSize(&tag), encoded_index);
}

namespace {
void RedirectCodeToUnwindHandler(const uint8_t*& code) {
  // Resume execution from s2s_Unwind, which unwinds the Wasm stack frames
  code = reinterpret_cast<uint8_t*>(&s_unwind_code);
}
}  // namespace

// Allocate a new exception.
DirectHandle<WasmExceptionPackage>
WasmInterpreterRuntime::CreateWasmExceptionPackage(uint32_t tag_index) const {
  DirectHandle<WasmTrustedInstanceData> trusted_data =
      wasm_trusted_instance_data();
  DirectHandle<WasmExceptionTag> exception_tag(
      Cast<WasmExceptionTag>(trusted_data->tags_table()->get(tag_index)),
      isolate_);
  const WasmTag& tag = GetWasmTag(tag_index);
  uint32_t encoded_size = WasmExceptionPackage::GetEncodedSize(&tag);
  DirectHandle<WasmExceptionPackage> exception_object =
      WasmExceptionPackage::New(isolate_, exception_tag, encoded_size);
  return exception_object;
}

// Throw a Wasm exception.
void WasmInterpreterRuntime::ThrowException(const uint8_t*& code, uint32_t* sp,
                                            Tagged<Object> exception_object) {
  // Keep track of the code offset of the current instruction, which we'll need
  // to calculate the stack trace from Isolate::Throw.
  current_frame_.current_bytecode_ = code;

  DCHECK_NOT_NULL(current_thread_);
  current_thread_->SetCurrentFrame(current_frame_);

  // Now that the exception is ready, set it as pending.
  {
    isolate_->Throw(exception_object);
    if (HandleException(sp, code) != WasmInterpreterThread::HANDLED) {
      RedirectCodeToUnwindHandler(code);
    }
  }
}

// Throw a given existing exception caught by the catch block specified.
void WasmInterpreterRuntime::RethrowException(const uint8_t*& code,
                                              uint32_t* sp,
                                              uint32_t catch_block_index) {
  DirectHandle<Object> exception_object =
      current_frame_.GetCaughtException(isolate_, catch_block_index);
  DCHECK(!IsTheHole(*exception_object));
  ThrowException(code, sp, *exception_object);
}

// Handle a thrown exception. Returns whether the exception was handled inside
// of wasm. Unwinds the interpreted stack accordingly.
WasmInterpreterThread::ExceptionHandlingResult
WasmInterpreterRuntime::HandleException(uint32_t* sp,
                                        const uint8_t*& current_code) {
  DCHECK_IMPLIES(current_code, current_frame_.current_function_);
  DCHECK_IMPLIES(!current_code, !current_frame_.current_function_);
  DCHECK(isolate_->has_exception());

  bool catchable = current_frame_.current_function_ &&
                   isolate_->is_catchable_by_wasm(isolate_->exception());
  if (catchable) {
    HandleScope scope(isolate_);
    DirectHandle<Object> exception(isolate_->exception(), isolate_);
    Tagged<WasmTrustedInstanceData> trusted_data =
        *wasm_trusted_instance_data();

    // We might need to allocate a new FixedArray<Object> to store the caught
    // exception.
    DCHECK(AllowHeapAllocation::IsAllowed());

    size_t current_code_offset =
        current_code - current_frame_.current_function_->GetCode();
    const WasmEHData::TryBlock* try_block =
        current_frame_.current_function_->GetTryBlock(current_code_offset);
    while (try_block) {
      for (const auto& catch_handler : try_block->catch_handlers) {
        if (catch_handler.tag_index < 0) {
          // Catch all.
          current_code = current_frame_.current_function_->GetCode() +
                         catch_handler.code_offset;
          current_frame_.SetCaughtException(
              isolate_, catch_handler.catch_block_index, exception);
          isolate_->clear_exception();
          return WasmInterpreterThread::HANDLED;
        } else if (IsWasmExceptionPackage(*exception, isolate_)) {
          // The exception was thrown by Wasm code and it's wrapped in a
          // WasmExceptionPackage.
          DirectHandle<Object> caught_tag =
              WasmExceptionPackage::GetExceptionTag(
                  isolate_, Cast<WasmExceptionPackage>(exception));
          DirectHandle<Object> expected_tag(
              trusted_data->tags_table()->get(catch_handler.tag_index),
              isolate_);
          DCHECK(IsWasmExceptionTag(*expected_tag));
          // Determines whether the given exception has a tag matching the
          // expected tag for the given index within the exception table of the
          // current instance.
          if (expected_tag.is_identical_to(caught_tag)) {
            current_code = current_frame_.current_function_->GetCode() +
                           catch_handler.code_offset;
            DCHECK_LT(catch_handler.tag_index, module_->tags.size());
            const WasmTag& tag = module_->tags[catch_handler.tag_index];
            auto exception_payload_slot_offsets =
                current_frame_.current_function_
                    ->GetExceptionPayloadStartSlotOffsets(
                        catch_handler.catch_block_index);
            UnpackException(
                sp, tag, exception,
                exception_payload_slot_offsets.first_param_slot_offset,
                exception_payload_slot_offsets.first_param_ref_stack_index);
            current_frame_.SetCaughtException(
                isolate_, catch_handler.catch_block_index, exception);
            isolate_->clear_exception();
            return WasmInterpreterThread::HANDLED;
          }
        } else {
          // Check for the special case where the tag is WebAssembly.JSTag and
          // the exception is not a WebAssembly.Exception. In this case the
          // exception is caught and pushed on the operand stack.
          // Only perform this check if the tag signature is the same as
          // the JSTag signature, i.e. a single externref, otherwise we know
          // statically that it cannot be the JSTag.
          DCHECK_LT(catch_handler.tag_index, module_->tags.size());
          const WasmTagSig* sig = module_->tags[catch_handler.tag_index].sig;
          if (sig->return_count() != 0 || sig->parameter_count() != 1 ||
              (sig->GetParam(0).kind() != kRefNull &&
               sig->GetParam(0).kind() != kRef)) {
            continue;
          }

          DirectHandle<JSObject> js_tag_object(
              isolate_->native_context()->wasm_js_tag(), isolate_);
          DirectHandle<WasmTagObject> wasm_tag_object(
              Cast<WasmTagObject>(*js_tag_object), isolate_);
          DirectHandle<Object> caught_tag(wasm_tag_object->tag(), isolate_);
          DirectHandle<Object> expected_tag(
              trusted_data->tags_table()->get(catch_handler.tag_index),
              isolate_);
          if (!expected_tag.is_identical_to(caught_tag)) {
            continue;
          }

          current_code = current_frame_.current_function_->GetCode() +
                         catch_handler.code_offset;
          // Push exception on the operand stack.
          auto exception_payload_slot_offsets =
              current_frame_.current_function_
                  ->GetExceptionPayloadStartSlotOffsets(
                      catch_handler.catch_block_index);
          StoreWasmRef(
              exception_payload_slot_offsets.first_param_ref_stack_index,
              exception);
          base::WriteUnalignedValue<WasmRef>(
              reinterpret_cast<Address>(
                  sp + exception_payload_slot_offsets.first_param_slot_offset),
              exception);

          current_frame_.SetCaughtException(
              isolate_, catch_handler.catch_block_index, exception);
          isolate_->clear_exception();
          return WasmInterpreterThread::HANDLED;
        }
      }
      try_block =
          current_frame_.current_function_->GetParentTryBlock(try_block);
    }
  }

  DCHECK_NOT_NULL(current_thread_);
  current_thread_->Unwinding();
  return WasmInterpreterThread::UNWOUND;
}

bool WasmInterpreterRuntime::AllowsAtomicsWait() const {
  return !module_->memories.empty() && module_->memories[0].is_shared &&
         isolate_->allow_atomics_wait();
}

int32_t WasmInterpreterRuntime::AtomicNotify(uint64_t buffer_offset,
                                             int32_t val) {
  if (module_->memories.empty() || !module_->memories[0].is_shared) {
    return 0;
  } else {
    HandleScope handle_scope(isolate_);
    // TODO(paolosev@microsoft.com): Support multiple memories.
    uint32_t memory_index = 0;
    DirectHandle<JSArrayBuffer> array_buffer(wasm_trusted_instance_data()
                                                 ->memory_object(memory_index)
                                                 ->array_buffer(),
                                             isolate_);
    int result = FutexEmulation::Wake(*array_buffer, buffer_offset, val);
    return result;
  }
}

int32_t WasmInterpreterRuntime::I32AtomicWait(uint64_t buffer_offset,
                                              int32_t val, int64_t timeout) {
  // We might handle interrupts (for example from a debugger) here while we are
  // locked in the futex. We need to make sure that the current activation is
  // up-to-date, if we need to traverse the call stack.
  current_thread_->SetCurrentFrame(current_frame_);

  HandleScope handle_scope(isolate_);
  // TODO(paolosev@microsoft.com): Support multiple memories.
  uint32_t memory_index = 0;
  DirectHandle<JSArrayBuffer> array_buffer(
      wasm_trusted_instance_data()->memory_object(memory_index)->array_buffer(),
      isolate_);
  auto result = FutexEmulation::WaitWasm32(isolate_, array_buffer,
                                           buffer_offset, val, timeout);
  return result.ToSmi().value();
}

int32_t WasmInterpreterRuntime::I64AtomicWait(uint64_t buffer_offset,
                                              int64_t val, int64_t timeout) {
  // We might handle interrupts (for example from a debugger) here while we are
  // locked in the futex. We need to make sure that the current activation is
  // up-to-date, if we need to traverse the call stack.
  current_thread_->SetCurrentFrame(current_frame_);

  HandleScope handle_scope(isolate_);
  // TODO(paolosev@microsoft.com): Support multiple memories.
  uint32_t memory_index = 0;
  DirectHandle<JSArrayBuffer> array_buffer(
      wasm_trusted_instance_data()->memory_object(memory_index)->array_buffer(),
      isolate_);
  auto result = FutexEmulation::WaitWasm64(isolate_, array_buffer,
                                           buffer_offset, val, timeout);
  return result.ToSmi().value();
}

void WasmInterpreterRuntime::BeginExecution(
    WasmInterpreterThread* thread, uint32_t func_index, Address frame_pointer,
    uint8_t* interpreter_fp, uint32_t ref_stack_offset,
    const std::vector<WasmValue>* argument_values) {
  current_thread_ = thread;
  start_function_index_ = func_index;

  thread->StartActivation(this, frame_pointer, interpreter_fp, current_frame_);

  current_frame_.current_function_ = nullptr;
  current_frame_.previous_frame_ = nullptr;
  current_frame_.current_bytecode_ = nullptr;
  current_frame_.current_sp_ = interpreter_fp;
  current_frame_.ref_array_current_sp_ = ref_stack_offset;
  current_frame_.thread_ = thread;
#ifdef V8_ENABLE_DRUMBRAKE_TRACING
  current_frame_.current_stack_start_args_ = thread->CurrentStackFrameStart();
#endif  // V8_ENABLE_DRUMBRAKE_TRACING

  const FunctionSig* sig = module_->functions[func_index].sig;
  size_t args_count = 0;
  uint32_t rets_slots_size = 0;
  uint32_t ref_rets_count = 0;
  uint32_t ref_args_count = 0;
  WasmBytecode* target_function = GetFunctionBytecode(func_index);
  if (target_function) {
    args_count = target_function->args_count();
    rets_slots_size = target_function->rets_slots_size();
    ref_rets_count = target_function->ref_rets_count();
    ref_args_count = target_function->ref_args_count();
  } else {
    // We begin execution by calling an imported function.
    args_count = sig->parameter_count();
    rets_slots_size = WasmBytecode::RetsSizeInSlots(sig);
    ref_rets_count = WasmBytecode::RefRetsCount(sig);
    ref_args_count = WasmBytecode::RefArgsCount(sig);
  }

  // Here GC is disabled, we cannot "resize" the reference_stack_ FixedArray
  // before having created Handles for the Ref arguments passed in
  // argument_values.
  HandleScope handle_scope(isolate_);  // Avoid leaking handles.

  DirectHandleVector<Object> ref_args(isolate_);
  if (ref_args_count > 0) {
    ref_args.reserve(ref_args_count);
  }

  uint8_t* p = interpreter_fp + rets_slots_size * kSlotSize;

  // Check stack overflow.
  const uint8_t* stack_limit = thread->StackLimitAddress();
  if (V8_UNLIKELY(p + (ref_rets_count + ref_args_count) * sizeof(WasmRef) >=
                  stack_limit)) {
    size_t additional_required_size =
        p + (ref_rets_count + ref_args_count) * sizeof(WasmRef) - stack_limit;
    if (!thread->ExpandStack(additional_required_size)) {
      // TODO(paolosev@microsoft.com) - Calculate initial function offset.
      SealHandleScope shs(isolate_);
      isolate_->StackOverflow();
      const pc_t trap_pc = 0;
      SetTrap(TrapReason::kTrapUnreachable, trap_pc);
      thread->FinishActivation();
      return;
    }
  }

  if (argument_values) {
    // We are being called from JS, arguments are passed in the
    // {argument_values} vector.
    for (size_t i = 0; i < argument_values->size(); i++) {
      const WasmValue& value = (*argument_values)[i];
      switch (value.type().kind()) {
        case kI32:
          base::WriteUnalignedValue<int32_t>(reinterpret_cast<Address>(p),
                                             value.to<int32_t>());
          p += sizeof(int32_t);
          break;
        case kI64:
          base::WriteUnalignedValue<int64_t>(reinterpret_cast<Address>(p),
                                             value.to<int64_t>());
          p += sizeof(int64_t);
          break;
        case kF32:
          base::WriteUnalignedValue<float>(reinterpret_cast<Address>(p),
                                           value.to<float>());
          p += sizeof(float);
          break;
        case kF64:
          base::WriteUnalignedValue<double>(reinterpret_cast<Address>(p),
                                            value.to<double>());
          p += sizeof(double);
          break;
        case kRef:
        case kRefNull:
          ref_args.push_back(value.to_ref());
          base::WriteUnalignedValue<uint64_t>(reinterpret_cast<Address>(p),
                                              kSlotsZapValue);
          p += sizeof(WasmRef);
          break;
        case kS128:
        default:
          UNREACHABLE();
      }
    }
  } else {
    // We are being called from Wasm, arguments are already in the stack.
    for (size_t i = 0; i < args_count; i++) {
      switch (sig->GetParam(i).kind()) {
        case kI32:
          p += sizeof(int32_t);
          break;
        case kI64:
          p += sizeof(int64_t);
          break;
        case kF32:
          p += sizeof(float);
          break;
        case kF64:
          p += sizeof(double);
          break;
        case kS128:
          p += sizeof(Simd128);
          break;
        case kRef:
        case kRefNull: {
          DirectHandle<Object> ref =
              base::ReadUnalignedValue<DirectHandle<Object>>(
                  reinterpret_cast<Address>(p));
          ref_args.push_back(ref);
          p += sizeof(WasmRef);
          break;
        }
        default:
          UNREACHABLE();
      }
    }
  }

  {
    // Once we have read ref argument passed on the stack and we have stored
    // them into the ref_args vector of Handles, we can re-enable the GC.
    AllowHeapAllocation allow_gc;

    if (ref_rets_count + ref_args_count > 0) {
      // Reserve space for reference args and return values in the
      // reference_stack_.
      current_thread_->EnsureRefStackSpace(
          current_frame_.ref_array_current_sp_ + ref_rets_count +
          ref_args_count);

      uint32_t ref_stack_arg_index = ref_rets_count;
      for (uint32_t ref_arg_index = 0; ref_arg_index < ref_args_count;
           ref_arg_index++) {
        StoreWasmRef(ref_stack_arg_index++, ref_args[ref_arg_index]);
      }
    }
  }
}

void WasmInterpreterRuntime::ContinueExecution(WasmInterpreterThread* thread,
                                               bool called_from_js) {
  DCHECK_NE(start_function_index_, UINT_MAX);

  uint32_t start_function_index = start_function_index_;
  FrameState current_frame = current_frame_;

  const uint8_t* code = nullptr;
  const FunctionSig* sig = nullptr;
  const CanonicalSig* canonicalized_sig = nullptr;
  uint32_t return_count = 0;
  WasmBytecode* target_function = GetFunctionBytecode(start_function_index_);
  if (target_function) {
    sig = target_function->GetFunctionSignature();
    canonicalized_sig = target_function->GetCanonicalFunctionSignature();
    return_count = target_function->return_count();
    ExecuteFunction(code, start_function_index_, target_function->args_count(),
                    0, 0, 0);
  } else {
    sig = module_->functions[start_function_index_].sig;
    CanonicalTypeIndex canonical_sig_index = module_->canonical_sig_id(
        module_->functions[start_function_index_].sig_index);
    canonicalized_sig =
        GetTypeCanonicalizer()->LookupFunctionSignature(canonical_sig_index);
    return_count = static_cast<uint32_t>(sig->return_count());
    ExecuteImportedFunction(code, start_function_index_,
                            static_cast<uint32_t>(sig->parameter_count()), 0, 0,
                            0);
  }

  // If there are Ref types in the set of result types defined in the function
  // signature, they are located from the first ref_stack_ slot of the current
  // Activation.
  uint32_t ref_result_slot_index = 0;

  if (state() == WasmInterpreterThread::State::RUNNING) {
    if (return_count > 0) {
      uint32_t* dst = reinterpret_cast<uint32_t*>(current_frame_.current_sp_);

      if (called_from_js) {
        // We are returning the results to a JS caller, we need to store them
        // into the {function_result_} vector and they will be retrieved via
        // {GetReturnValue}.
        function_result_.resize(return_count);
        for (size_t index = 0; index < return_count; index++) {
          CanonicalValueType ret_value_type =
              canonicalized_sig->GetReturn(index);
          switch (ret_value_type.kind()) {
            case kI32:
              function_result_[index] =
                  WasmValue(base::ReadUnalignedValue<int32_t>(
                      reinterpret_cast<Address>(dst)));
              dst += sizeof(uint32_t) / kSlotSize;
              break;
            case kI64:
              function_result_[index] =
                  WasmValue(base::ReadUnalignedValue<int64_t>(
                      reinterpret_cast<Address>(dst)));
              dst += sizeof(uint64_t) / kSlotSize;
              break;
            case kF32:
              function_result_[index] =
                  WasmValue(base::ReadUnalignedValue<float>(
                      reinterpret_cast<Address>(dst)));
              dst += sizeof(float) / kSlotSize;
              break;
            case kF64:
              function_result_[index] =
                  WasmValue(base::ReadUnalignedValue<double>(
                      reinterpret_cast<Address>(dst)));
              dst += sizeof(double) / kSlotSize;
              break;
            case kRef:
            case kRefNull: {
              DirectHandle<Object> ref =
                  ExtractWasmRef(ref_result_slot_index++);
              ref = WasmToJSObject(ref);
              function_result_[index] = WasmValue(ref, ret_value_type);
              dst += sizeof(WasmRef) / kSlotSize;
              break;
            }
            case kS128:
            default:
              UNREACHABLE();
          }
        }
      } else {
        // We are returning the results on the stack
        for (size_t index = 0; index < return_count; index++) {
          switch (sig->GetReturn(index).kind()) {
            case kI32:
              dst += sizeof(uint32_t) / kSlotSize;
              break;
            case kI64:
              dst += sizeof(uint64_t) / kSlotSize;
              break;
            case kF32:
              dst += sizeof(float) / kSlotSize;
              break;
            case kF64:
              dst += sizeof(double) / kSlotSize;
              break;
            case kS128:
              dst += sizeof(Simd128) / kSlotSize;
              break;
            case kRef:
            case kRefNull: {
              // Make sure the ref result is termporarily stored in a stack
              // slot, to be retrieved by the caller.
              DirectHandle<Object> ref =
                  ExtractWasmRef(ref_result_slot_index++);
              base::WriteUnalignedValue<WasmRef>(reinterpret_cast<Address>(dst),
                                                 ref);
              dst += sizeof(WasmRef) / kSlotSize;
              break;
            }
            default:
              UNREACHABLE();
          }
        }
      }
    }

    if (ref_result_slot_index > 0) {
      current_thread_->ClearRefStackValues(current_frame_.ref_array_current_sp_,
                                           ref_result_slot_index);
    }

    DCHECK(current_frame_.caught_exceptions_.is_null());

    start_function_index_ = start_function_index;
    current_frame_ = current_frame;
  } else if (state() == WasmInterpreterThread::State::TRAPPED) {
    MessageTemplate message_id =
        WasmOpcodes::TrapReasonToMessageId(thread->GetTrapReason());
    thread->RaiseException(isolate_, message_id);
  } else if (state() == WasmInterpreterThread::State::EH_UNWINDING) {
    // Uncaught exception.
    thread->Stop();
  } else {
    DCHECK_EQ(state(), WasmInterpreterThread::State::STOPPED);
  }

  thread->FinishActivation();
  const FrameState* frame_state = thread->GetCurrentActivationFor(this);
  current_frame_ = frame_state ? *frame_state : FrameState();
}

void WasmInterpreterRuntime::StoreWasmRef(uint32_t ref_stack_index,
                                          const WasmRef& ref) {
  uint32_t index = ref_stack_index + current_frame_.ref_array_current_sp_;
  if (ref.is_null()) {
    reference_stack()->set_the_hole(isolate_, index);
  } else {
    reference_stack()->set(index, *ref);
  }
}

WasmRef WasmInterpreterRuntime::ExtractWasmRef(uint32_t ref_stack_index) {
  int index =
      static_cast<int>(ref_stack_index) + current_frame_.ref_array_current_sp_;
  DirectHandle<Object> ref(reference_stack()->get(index), isolate_);
  DCHECK(!IsTheHole(*ref, isolate_));
  return WasmRef(ref);
}

// A tail call should not add an additional stack frame to the interpreter
// stack. This is implemented by unwinding the current stack frame just before
// the tail call.
void WasmInterpreterRuntime::UnwindCurrentStackFrame(
    uint32_t* sp, uint32_t slot_offset, uint32_t rets_size, uint32_t args_size,
    uint32_t rets_refs, uint32_t args_refs, uint32_t ref_stack_fp_offset) {
  // At the moment of the call the interpreter stack is as in the diagram below.
  // A new interpreter frame for the callee function has been initialized, with
  // `R` slots to contain the R return values, followed by {args_size} slots to
  // contain the callee arguments.
  //
  // In order to unwind an interpreter stack frame we just copy the content of
  // the slots that contain the callee arguments into the caller stack frame,
  // just after the slots of the return values. Note that the return call is
  // invalid if the number and types of the return values of the callee function
  // do not exactly match the number and types of the return values of the
  // caller function. Instead, the number of types of the caller and callee
  // functions arguments can differ.
  //
  // The other slots in the caller frame, for const values and locals, will be
  // initialized later in ExecuteFunction().
  //
  // +----------------------+
  // | argA-1               |      ^         ^
  // | ...                  |      |         | ->-----+
  // | ...                  |      |         |        |
  // | arg0                 |    callee      v        |
  // | retR-1               |    frame                |
  // | ...                  |      |                  |
  // | ret0                 |      v                  | copy
  // +----------------------+ (slot_offset)           |
  // | ...                  |      ^                  V
  // | <stack slots>        |      |                  |
  // | <locals slots>       |      |                  |
  // | <const slots>        |      |         ^        |
  // | argN-1               |    caller      | <------+
  // | ...                  |    frame       |
  // | arg0                 |      |         v
  // | retR-1               |      |
  // | ...                  |      |
  // | ret0                 |      v
  // +----------------------+ (0)

  uint8_t* next_sp = reinterpret_cast<uint8_t*>(sp);
  uint8_t* prev_sp = next_sp + slot_offset;
  // Here {args_size} is the number of arguments expected by the function we are
  // calling, which can be different from the number of args of the caller
  // function.
  ::memmove(next_sp + rets_size, prev_sp, args_size);

  // If some of the argument-slots contain Ref values, we need to move them
  // accordingly, in the thread {reference_stack_}.
  if (rets_refs) {
    current_thread_->ClearRefStackValues(current_frame_.ref_array_current_sp_,
                                         rets_refs);
  }
  // Here {args_refs} is the number of reference args expected by the function
  // we are calling, which can be different from the number of reference args of
  // the caller function.
  for (uint32_t i = 0; i < args_refs; i++) {
    StoreWasmRef(rets_refs + i, ExtractWasmRef(ref_stack_fp_offset + i));
  }
  if (ref_stack_fp_offset > rets_refs + args_refs) {
    current_thread_->ClearRefStackValues(
        current_frame_.ref_array_current_sp_ + rets_refs + args_refs,
        ref_stack_fp_offset - rets_refs - args_refs);
  }
}

void WasmInterpreterRuntime::StoreRefArgsIntoStackSlots(
    uint8_t* sp, uint32_t ref_stack_fp_offset, const FunctionSig* sig) {
  // Argument values of type Ref, if present, are already stored in the thread's
  // reference_stack_ starting at index ref_stack_fp_offset + RefRetsCount(sig).
  // We want to temporarily copy the pointers to these object also in the stack
  // slots, because functions WasmInterpreter::RunInterpreter() and
  // WasmInterpreter::CallExternalJSFunction gets all arguments from the stack.

  // TODO(paolosev@microsoft.com) - Too slow?
  ref_stack_fp_offset += WasmBytecode::RefRetsCount(sig);

  size_t args_count = sig->parameter_count();
  sp += WasmBytecode::RetsSizeInSlots(sig) * kSlotSize;
  for (size_t i = 0; i < args_count; i++) {
    switch (sig->GetParam(i).kind()) {
      case kI32:
      case kF32:
        sp += sizeof(int32_t);
        break;
      case kI64:
      case kF64:
        sp += sizeof(int64_t);
        break;
      case kS128:
        sp += sizeof(Simd128);
        break;
      case kRef:
      case kRefNull: {
        WasmRef ref = ExtractWasmRef(ref_stack_fp_offset++);
        base::WriteUnalignedValue<WasmRef>(reinterpret_cast<Address>(sp), ref);
        sp += sizeof(WasmRef);
        break;
      }
      default:
        UNREACHABLE();
    }
  }
}

void WasmInterpreterRuntime::StoreRefResultsIntoRefStack(
    uint8_t* sp, uint32_t ref_stack_fp_offset, const FunctionSig* sig) {
  size_t rets_count = sig->return_count();
  for (size_t i = 0; i < rets_count; i++) {
    switch (sig->GetReturn(i).kind()) {
      case kI32:
      case kF32:
        sp += sizeof(int32_t);
        break;
      case kI64:
      case kF64:
        sp += sizeof(int64_t);
        break;
      case kS128:
        sp += sizeof(Simd128);
        break;
      case kRef:
      case kRefNull:
        StoreWasmRef(ref_stack_fp_offset++, base::ReadUnalignedValue<WasmRef>(
                                                reinterpret_cast<Address>(sp)));
        base::WriteUnalignedValue<uint64_t>(reinterpret_cast<Address>(sp),
                                            kSlotsZapValue);
        sp += sizeof(WasmRef);
        break;
      default:
        UNREACHABLE();
    }
  }
}

void WasmInterpreterRuntime::ExecuteImportedFunction(
    const uint8_t*& code, uint32_t func_index, uint32_t current_stack_size,
    uint32_t ref_stack_fp_offset, uint32_t slot_offset,
    uint32_t return_slot_offset, bool is_tail_call) {
  WasmInterpreterThread* thread = this->thread();
  DCHECK_NOT_NULL(thread);

  // Store a pointer to the current FrameState before leaving the current
  // Activation.
  current_frame_.current_bytecode_ = code;
  thread->SetCurrentFrame(current_frame_);
  thread->SetCurrentActivationFrame(
      reinterpret_cast<uint32_t*>(current_frame_.current_sp_ + slot_offset),
      slot_offset, current_stack_size,
      current_frame_.ref_array_current_sp_ + ref_stack_fp_offset,
      ref_stack_fp_offset);

  ExternalCallResult result = CallImportedFunction(
      code, func_index,
      reinterpret_cast<uint32_t*>(current_frame_.current_sp_ + slot_offset),
      current_stack_size, ref_stack_fp_offset, slot_offset);

  if (result == ExternalCallResult::EXTERNAL_EXCEPTION) {
    WasmInterpreterThread::ExceptionHandlingResult exception_handling_result =
        HandleException(reinterpret_cast<uint32_t*>(current_frame_.current_sp_),
                        code);
    if (exception_handling_result ==
        WasmInterpreterThread::ExceptionHandlingResult::HANDLED) {
      // The exception was caught by Wasm EH. Resume execution,
      // {HandleException} has already updated {code} to point to the first
      // instruction in the catch handler.
      thread->Run();
    } else {
      DCHECK_EQ(exception_handling_result,
                WasmInterpreterThread::ExceptionHandlingResult::UNWOUND);
      if (thread->state() != WasmInterpreterThread::State::EH_UNWINDING) {
        thread->Stop();
      }
      // Resume execution from s2s_Unwind, which unwinds the Wasm stack frames.
      RedirectCodeToUnwindHandler(code);
    }
  }

  if (is_tail_call) {
    RedirectCodeToUnwindHandler(code);
  }
}

inline DISABLE_CFI_ICALL void CallThroughDispatchTable(
    const uint8_t* code, uint32_t* sp, WasmInterpreterRuntime* wasm_runtime,
    int64_t r0, double fp0) {
  kInstructionTable[ReadFnId(code) & kInstructionTableMask](
      code, sp, wasm_runtime, r0, fp0);
}

void WasmInterpreterRuntime::InitializeRefLocalsRefs(
    const WasmBytecode* target_function) {
  if (V8_UNLIKELY(target_function->ref_locals_count() > 0)) {
    uint32_t ref_stack_index =
        target_function->ref_rets_count() + target_function->ref_args_count();
    for (uint32_t i = 0; i < target_function->locals_count(); i++) {
      ValueType local_type = target_function->local_type(i);
      if (local_type == kWasmExternRef || local_type == kWasmNullExternRef) {
        StoreWasmRef(ref_stack_index++,
                     WasmRef(isolate_->factory()->null_value()));
      } else if (local_type.is_reference()) {
        StoreWasmRef(ref_stack_index++,
                     WasmRef(isolate_->factory()->wasm_null()));
      }
    }
  }
}

// Sets up the current interpreter stack frame to start executing a new function
// with a tail call. Do not move the stack pointer for the interpreter stack,
// and avoids calling WasmInterpreterRuntime::ExecuteFunction(), which would add
// a new C++ stack frame.
void WasmInterpreterRuntime::PrepareTailCall(const uint8_t*& code,
                                             uint32_t func_index,
                                             uint32_t current_stack_size,
                                             uint32_t return_slot_offset) {
  // TODO(paolosev@microsoft.com): avoid to duplicate code from ExecuteFunction?

  WasmBytecode* target_function = GetFunctionBytecode(func_index);
  DCHECK_NOT_NULL(target_function);

  // Update the current (caller) frame setting current_bytecode_ to the initial
  // instruction of the callee.
  current_frame_.current_function_ = target_function;
  current_frame_.DisposeCaughtExceptionsArray(isolate_);

#ifdef V8_ENABLE_DRUMBRAKE_TRACING
  current_frame_.current_stack_start_locals_ =
      current_frame_.current_stack_start_args_ + target_function->args_count();
  current_frame_.current_stack_start_stack_ =
      current_frame_.current_stack_start_locals_ +
      target_function->locals_count();

  if (v8_flags.trace_drumbrake_execution) {
    Trace("\nTailCallFunction: %d\n", func_index);
    Trace("= > PushFrame #%d(#%d @%d)\n", current_frame_.current_stack_height_,
          func_index, 0);
  }
#endif  // V8_ENABLE_DRUMBRAKE_TRACING

  if (!WasmStackCheck(target_function->GetCode(), code)) {
    return;
  }

  const uint8_t* stack_limit = current_frame_.thread_->StackLimitAddress();
  if (V8_UNLIKELY(stack_limit <= current_frame_.current_sp_ ||
                  !target_function->InitializeSlots(
                      current_frame_.current_sp_,
                      stack_limit - current_frame_.current_sp_))) {
    // Try to resize the stack.
    size_t additional_required_space =
        target_function->frame_size() -
        (stack_limit - current_frame_.current_sp_);
    // Try again.
    if (!current_frame_.thread_->ExpandStack(additional_required_space) ||
        !target_function->InitializeSlots(
            current_frame_.current_sp_,
            (stack_limit = current_frame_.thread_->StackLimitAddress()) -
                current_frame_.current_sp_)) {
      SealHandleScope shs(isolate_);
      SetTrap(TrapReason::kTrapUnreachable, code);
      isolate_->StackOverflow();
      return;
    }
  }

  uint32_t ref_slots_count = target_function->ref_slots_count();
  if (V8_UNLIKELY(ref_slots_count > 0)) {
    uint32_t ref_array_length =
        current_frame_.ref_array_current_sp_ + ref_slots_count;
    current_thread_->EnsureRefStackSpace(ref_array_length);
    InitializeRefLocalsRefs(target_function);
  }

#ifdef V8_ENABLE_DRUMBRAKE_TRACING
  uint32_t shadow_stack_offset = 0;
  if (v8_flags.trace_drumbrake_execution) {
    shadow_stack_offset = target_function->rets_slots_size() * kSlotSize;
    for (uint32_t i = 0; i < target_function->args_count(); i++) {
      shadow_stack_offset +=
          TracePush(target_function->arg_type(i).kind(), shadow_stack_offset);
    }

    // Make room for locals in shadow stack
    shadow_stack_offset += target_function->const_slots_size_in_bytes();
    for (size_t i = 0; i < target_function->locals_count(); i++) {
      shadow_stack_offset +=
          TracePush(target_function->local_type(i).kind(), shadow_stack_offset);
    }
  }
#endif  // V8_ENABLE_DRUMBRAKE_TRACING

  code = target_function->GetCode();
}

void WasmInterpreterRuntime::ExecuteFunction(const uint8_t*& code,
                                             uint32_t func_index,
                                             uint32_t current_stack_size,
                                             uint32_t ref_stack_fp_offset,
                                             uint32_t slot_offset,
                                             uint32_t return_slot_offset) {
  // Execute an internal call.
  WasmBytecode* target_function = GetFunctionBytecode(func_index);
  DCHECK_NOT_NULL(target_function);

#ifdef V8_ENABLE_DRUMBRAKE_TRACING
  ShadowStack* prev_shadow_stack = shadow_stack_;
  ShadowStack shadow_stack;
  if (v8_flags.trace_drumbrake_execution) {
    shadow_stack_ = &shadow_stack;
  }
#endif  // V8_ENABLE_DRUMBRAKE_TRACING

  // This HandleScope is used for all handles created in instruction handlers.
  // We reset it every time we get to a backward jump in a loop.
  HandleScope handle_scope(GetIsolate());

  // Update the current (caller) frame setting current_bytecode_ to the current
  // call instruction.
  current_frame_.current_bytecode_ = code;

  FrameState prev_frame_state = current_frame_;
  current_frame_.current_sp_ += slot_offset;
  current_frame_.handle_scope_ = &handle_scope;

#ifdef V8_ENABLE_DRUMBRAKE_TRACING
  current_frame_.current_stack_start_args_ +=
      (current_stack_size - target_function->args_count());
#endif  // V8_ENABLE_DRUMBRAKE_TRACING

  current_frame_.current_function_ = target_function;
  current_frame_.previous_frame_ = &prev_frame_state;
  current_frame_.caught_exceptions_ = Handle<FixedArray>::null();

#ifdef V8_ENABLE_DRUMBRAKE_TRACING
  current_frame_.current_stack_height_++;
  current_frame_.current_stack_start_locals_ =
      current_frame_.current_stack_start_args_ + target_function->args_count();
  current_frame_.current_stack_start_stack_ =
      current_frame_.current_stack_start_locals_ +
      target_function->locals_count();

  if (v8_flags.trace_drumbrake_execution) {
    Trace("\nCallFunction: %d\n", func_index);
    Trace("= > PushFrame #%d(#%d @%d)\n", current_frame_.current_stack_height_,
          func_index, 0);
  }
#endif  // V8_ENABLE_DRUMBRAKE_TRACING

  if (!WasmStackCheck(target_function->GetCode(), code)) {
    return;
  }

  const uint8_t* stack_limit = current_frame_.thread_->StackLimitAddress();
  if (V8_UNLIKELY(stack_limit <= current_frame_.current_sp_ ||
                  !target_function->InitializeSlots(
                      current_frame_.current_sp_,
                      stack_limit - current_frame_.current_sp_))) {
    // Try to resize the stack.
    size_t additional_required_space =
        target_function->frame_size() -
        (stack_limit - current_frame_.current_sp_);
    // Try again.
    if (!current_frame_.thread_->ExpandStack(additional_required_space) ||
        !target_function->InitializeSlots(
            current_frame_.current_sp_,
            (stack_limit = current_frame_.thread_->StackLimitAddress()) -
                current_frame_.current_sp_)) {
      SealHandleScope shs(isolate_);
      SetTrap(TrapReason::kTrapUnreachable, code);
      isolate_->StackOverflow();
      return;
    }
  }

  uint32_t ref_slots_count = target_function->ref_slots_count();
  current_frame_.ref_array_current_sp_ += ref_stack_fp_offset;
  if (V8_UNLIKELY(ref_slots_count > 0)) {
    uint32_t ref_array_length =
        current_frame_.ref_array_current_sp_ + ref_slots_count;
    current_thread_->EnsureRefStackSpace(ref_array_length);
    InitializeRefLocalsRefs(target_function);
  }

#ifdef V8_ENABLE_DRUMBRAKE_TRACING
  uint32_t shadow_stack_offset = 0;
  if (v8_flags.trace_drumbrake_execution) {
    shadow_stack_offset = target_function->rets_slots_size() * kSlotSize;
    for (uint32_t i = 0; i < target_function->args_count(); i++) {
      shadow_stack_offset +=
          TracePush(target_function->arg_type(i).kind(), shadow_stack_offset);
    }

    // Make room for locals in shadow stack
    shadow_stack_offset += target_function->const_slots_size_in_bytes();
    for (size_t i = 0; i < target_function->locals_count(); i++) {
      shadow_stack_offset +=
          TracePush(target_function->local_type(i).kind(), shadow_stack_offset);
    }
  }
#endif  // V8_ENABLE_DRUMBRAKE_TRACING

  const uint8_t* callee_code = target_function->GetCode();
  int64_t r0 = 0;
  double fp0 = .0;

  // Execute function
  CallThroughDispatchTable(
      callee_code, reinterpret_cast<uint32_t*>(current_frame_.current_sp_),
      this, r0, fp0);

  uint32_t ref_slots_to_clear =
      ref_slots_count - target_function->ref_rets_count();
  if (V8_UNLIKELY(ref_slots_to_clear > 0)) {
    current_thread_->ClearRefStackValues(current_frame_.ref_array_current_sp_ +
                                             target_function->ref_rets_count(),
                                         ref_slots_to_clear);
  }

#ifdef V8_ENABLE_DRUMBRAKE_TRACING
  shadow_stack_ = prev_shadow_stack;

  if (v8_flags.trace_drumbrake_execution && shadow_stack_ != nullptr &&
      prev_frame_state.current_function_) {
    for (size_t i = 0; i < target_function->args_count(); i++) {
      TracePop();
    }

    for (size_t i = 0; i < target_function->return_count(); i++) {
      return_slot_offset +=
          TracePush(target_function->return_type(i).kind(), return_slot_offset);
    }
  }
#endif  // V8_ENABLE_DRUMBRAKE_TRACING

  current_frame_.handle_scope_ = nullptr;
  current_frame_.DisposeCaughtExceptionsArray(isolate_);
  current_frame_ = prev_frame_state;

  // Check state.
  WasmInterpreterThread::State current_state = state();
  if (V8_UNLIKELY(current_state != WasmInterpreterThread::State::RUNNING)) {
    switch (current_state) {
      case WasmInterpreterThread::State::EH_UNWINDING:
        DCHECK(isolate_->has_exception());
        if (!current_frame_.current_function_) {
          // We unwound the whole call stack without finding a catch handler.
          current_frame_.thread_->Stop();
          RedirectCodeToUnwindHandler(code);
        } else if (HandleException(
                       reinterpret_cast<uint32_t*>(current_frame_.current_sp_),
                       code) == WasmInterpreterThread::HANDLED) {
          current_frame_.thread_->Run();
        } else {
          // UNWOUND
          RedirectCodeToUnwindHandler(code);
        }
        break;

      case WasmInterpreterThread::State::TRAPPED:
      case WasmInterpreterThread::State::STOPPED:
        RedirectCodeToUnwindHandler(code);
        break;

      default:
        UNREACHABLE();
    }
  }
  // TODO(paolosev@microsoft.com): StackCheck.
}

void WasmInterpreterRuntime::PurgeIndirectCallCache(uint32_t table_index) {
  DCHECK_LT(table_index, indirect_call_tables_.size());
  const WasmTable& table = module_->tables[table_index];
  if (IsSubtypeOf(table.type, kWasmFuncRef, module_)) {
    size_t length =
        Tagged<WasmDispatchTable>::cast(
            wasm_trusted_instance_data()->dispatch_tables()->get(table_index))
            ->length();
    indirect_call_tables_[table_index].resize(length);
    for (size_t i = 0; i < length; i++) {
      indirect_call_tables_[table_index][i] = {};
    }
  }
}

// static
void WasmInterpreterRuntime::ClearIndirectCallCacheEntry(
    Isolate* isolate, DirectHandle<WasmInstanceObject> instance,
    uint32_t table_index, uint32_t entry_index) {
  DirectHandle<Tuple2> interpreter_object =
      WasmTrustedInstanceData::GetOrCreateInterpreterObject(instance);
  InterpreterHandle* handle =
      GetOrCreateInterpreterHandle(isolate, interpreter_object);
  WasmInterpreterRuntime* wasm_runtime =
      handle->interpreter()->GetWasmRuntime();
  DCHECK_LT(table_index, wasm_runtime->indirect_call_tables_.size());
  DCHECK_LT(entry_index,
            wasm_runtime->indirect_call_tables_[table_index].size());
  wasm_runtime->indirect_call_tables_[table_index][entry_index] = {};
}

// static
void WasmInterpreterRuntime::UpdateIndirectCallTable(
    Isolate* isolate, DirectHandle<WasmInstanceObject> instance,
    uint32_t table_index) {
  DirectHandle<Tuple2> interpreter_object =
      WasmTrustedInstanceData::GetOrCreateInterpreterObject(instance);
  InterpreterHandle* handle =
      GetOrCreateInterpreterHandle(isolate, interpreter_object);
  WasmInterpreterRuntime* wasm_runtime =
      handle->interpreter()->GetWasmRuntime();
  wasm_runtime->PurgeIndirectCallCache(table_index);
}

bool WasmInterpreterRuntime::CheckIndirectCallSignature(
    uint32_t table_index, uint32_t entry_index, uint32_t sig_index) const {
  const WasmTable& table = module_->tables[table_index];
  bool needs_type_check =
      !EquivalentTypes(table.type.AsNonNull(),
                       ValueType::Ref(ModuleTypeIndex({sig_index}), false,
                                      RefTypeKind::kFunction),
                       module_, module_);
  bool needs_null_check = table.type.is_nullable();

  // Copied from Liftoff.
  // We do both the type check and the null check by checking the signature,
  // so this shares most code. For the null check we then only check if the
  // stored signature is != -1.
  if (needs_type_check || needs_null_check) {
    const IndirectCallTable& dispatch_table =
        indirect_call_tables_[table_index];
    const wasm::CanonicalTypeIndex real_sig_id =
        dispatch_table[entry_index].sig_index;
    wasm::CanonicalTypeIndex canonical_sig_id =
        module_->canonical_sig_id(ModuleTypeIndex({sig_index}));
    if (!needs_type_check) {
      // Only check for -1 (nulled table entry).
      if (!real_sig_id.valid()) return false;
    } else if (!module_->types[sig_index].is_final) {
      if (real_sig_id == canonical_sig_id) return true;
      if (needs_null_check && !real_sig_id.valid()) return false;

      Tagged<Map> rtt = Tagged<Map>::cast(isolate_->heap()
                                              ->wasm_canonical_rtts()
                                              ->get(real_sig_id.index)
                                              .GetHeapObjectAssumeWeak());
      DirectHandle<Map> formal_rtt = RttCanon(sig_index);
      return SubtypeCheck(rtt, *formal_rtt, sig_index);
    } else {
      if (real_sig_id != canonical_sig_id) return false;
    }
  }

  return true;
}

void WasmInterpreterRuntime::ExecuteIndirectCall(
    const uint8_t*& current_code, uint32_t table_index, uint32_t sig_index,
    uint32_t entry_index, uint32_t stack_pos, uint32_t* sp,
    uint32_t ref_stack_fp_offset, uint32_t slot_offset,
    uint32_t return_slot_offset, bool is_tail_call) {
  DCHECK_LT(table_index, indirect_call_tables_.size());

  IndirectCallTable& table = indirect_call_tables_[table_index];

  // Bounds check against table size.
  DCHECK_GE(
      table.size(),
      Tagged<WasmDispatchTable>::cast(
          wasm_trusted_instance_data()->dispatch_tables()->get(table_index))
          ->length());
  if (entry_index >= table.size()) {
    SetTrap(TrapReason::kTrapTableOutOfBounds, current_code);
    return;
  }

  if (!table[entry_index]) {
    HandleScope handle_scope(isolate_);  // Avoid leaking handles.

    IndirectFunctionTableEntry entry(instance_object_, table_index,
                                     entry_index);
    const FunctionSig* signature = module_->signature({sig_index});

    DirectHandle<Object> object_implicit_arg(entry.implicit_arg(), isolate_);
    if (IsWasmTrustedInstanceData(*object_implicit_arg)) {
      Tagged<WasmTrustedInstanceData> trusted_instance_object =
          Cast<WasmTrustedInstanceData>(*object_implicit_arg);
      DirectHandle<WasmInstanceObject> instance_object(
          Cast<WasmInstanceObject>(trusted_instance_object->instance_object()),
          isolate_);
      if (instance_object_.is_identical_to(instance_object)) {
        // Call to an import.
        uint32_t func_index = entry.function_index();
        DCHECK(func_index != IndirectCallValue::kInvalidFunctionIndex);
        table[entry_index] = IndirectCallValue(func_index, entry.sig_id());
      } else {
        // Cross-instance call.
        table[entry_index] = IndirectCallValue(signature, entry.sig_id());
      }
    } else {
      // Call JS function.
      table[entry_index] = IndirectCallValue(signature, entry.sig_id());
    }
  }

  if (!CheckIndirectCallSignature(table_index, entry_index, sig_index)) {
    SetTrap(TrapReason::kTrapFuncSigMismatch, current_code);
    return;
  }

  IndirectCallValue indirect_call = table[entry_index];
  DCHECK(indirect_call);

  if (indirect_call.mode == IndirectCallValue::Mode::kInternalCall) {
    if (is_tail_call) {
      PrepareTailCall(current_code, indirect_call.func_index, stack_pos,
                      return_slot_offset);
    } else {
      ExecuteFunction(current_code, indirect_call.func_index, stack_pos,
                      ref_stack_fp_offset, slot_offset, return_slot_offset);
      if (state() == WasmInterpreterThread::State::TRAPPED ||
          state() == WasmInterpreterThread::State::STOPPED ||
          state() == WasmInterpreterThread::State::EH_UNWINDING) {
        RedirectCodeToUnwindHandler(current_code);
      }
    }
  } else {
    // ExternalCall
    HandleScope handle_scope(isolate_);  // Avoid leaking handles.

    DCHECK_NOT_NULL(indirect_call.signature);

    // Store a pointer to the current FrameState before leaving the current
    // Activation.
    WasmInterpreterThread* thread = this->thread();
    current_frame_.current_bytecode_ = current_code;
    thread->SetCurrentFrame(current_frame_);
    thread->SetCurrentActivationFrame(
        sp, slot_offset, stack_pos,
        current_frame_.ref_array_current_sp_ + ref_stack_fp_offset,
        ref_stack_fp_offset);

    // TODO(paolosev@microsoft.com): Optimize this code.
    IndirectFunctionTableEntry entry(instance_object_, table_index,
                                     entry_index);
    DirectHandle<Object> object_implicit_arg(entry.implicit_arg(), isolate_);

    if (IsWasmTrustedInstanceData(*object_implicit_arg)) {
      // Call Wasm function in a different instance.

      // Note that tail calls across WebAssembly module boundaries should
      // guarantee tail behavior, so this implementation does not conform to the
      // spec for a tail call. But it is really difficult to implement
      // cross-instance calls in the interpreter without recursively adding C++
      // stack frames.
      DirectHandle<WasmInstanceObject> target_instance(
          Cast<WasmInstanceObject>(
              Cast<WasmTrustedInstanceData>(*object_implicit_arg)
                  ->instance_object()),
          isolate_);

      // Make sure the target WasmInterpreterObject and InterpreterHandle exist.
      DirectHandle<Tuple2> interpreter_object =
          WasmTrustedInstanceData::GetOrCreateInterpreterObject(
              target_instance);
      GetOrCreateInterpreterHandle(isolate_, interpreter_object);

      Address frame_pointer = FindInterpreterEntryFramePointer(isolate_);

      {
        // We should not allocate anything in the heap and avoid GCs after we
        // store ref arguments into stack slots.
        DisallowHeapAllocation no_gc;

        uint8_t* fp = reinterpret_cast<uint8_t*>(sp) + slot_offset;
        StoreRefArgsIntoStackSlots(fp, ref_stack_fp_offset,
                                   indirect_call.signature);
        bool success = WasmInterpreterObject::RunInterpreter(
            isolate_, frame_pointer, target_instance, entry.function_index(),
            fp);
        if (success) {
          StoreRefResultsIntoRefStack(fp, ref_stack_fp_offset,
                                      indirect_call.signature);

#ifdef V8_ENABLE_DRUMBRAKE_TRACING
          // Update shadow stack
          if (v8_flags.trace_drumbrake_execution && shadow_stack_ != nullptr) {
            for (size_t i = 0; i < indirect_call.signature->parameter_count();
                 i++) {
              TracePop();
            }

            for (size_t i = 0; i < indirect_call.signature->return_count();
                 i++) {
              return_slot_offset +=
                  TracePush(indirect_call.signature->GetReturn(i).kind(),
                            return_slot_offset);
            }
          }
#endif  // V8_ENABLE_DRUMBRAKE_TRACING
        } else {
          thread->Stop();
          RedirectCodeToUnwindHandler(current_code);
        }
      }
    } else {
      // We should not allocate anything in the heap and avoid GCs after we
      // store ref arguments into stack slots.
      DisallowHeapAllocation no_gc;

      // Note that tail calls to host functions do not have to guarantee tail
      // behaviour, so it is ok to recursively allocate C++ stack frames here.
      uint8_t* fp = reinterpret_cast<uint8_t*>(sp) + slot_offset;
      StoreRefArgsIntoStackSlots(fp, ref_stack_fp_offset,
                                 indirect_call.signature);
      ExternalCallResult result = CallExternalJSFunction(
          current_code, module_, object_implicit_arg, indirect_call.signature,
          sp + slot_offset / kSlotSize, slot_offset);
      if (result == ExternalCallResult::EXTERNAL_RETURNED) {
        StoreRefResultsIntoRefStack(fp, ref_stack_fp_offset,
                                    indirect_call.signature);
      } else {  // ExternalCallResult::EXTERNAL_EXCEPTION
        AllowHeapAllocation allow_gc;

        if (HandleException(sp, current_code) ==
            WasmInterpreterThread::ExceptionHandlingResult::UNWOUND) {
          thread->Stop();
          RedirectCodeToUnwindHandler(current_code);
        } else {
          thread->Run();
        }
      }
    }

    if (is_tail_call) {
      // For tail calls to external functions, we need to unwind the callee
      // frame.
      RedirectCodeToUnwindHandler(current_code);
    }
  }
}

void WasmInterpreterRuntime::ExecuteCallRef(
    const uint8_t*& current_code, WasmRef func_ref, uint32_t sig_index,
    uint32_t stack_pos, uint32_t* sp, uint32_t ref_stack_fp_offset,
    uint32_t slot_offset, uint32_t return_slot_offset, bool is_tail_call) {
  if (IsWasmFuncRef(*func_ref)) {
    func_ref = direct_handle(Cast<WasmFuncRef>(*func_ref)->internal(isolate_),
                             isolate_);
  }
  if (IsWasmInternalFunction(*func_ref)) {
    Tagged<WasmInternalFunction> wasm_internal_function =
        Cast<WasmInternalFunction>(*func_ref);
    Tagged<Object> implicit_arg = wasm_internal_function->implicit_arg();
    if (IsWasmImportData(implicit_arg)) {
      func_ref = direct_handle(implicit_arg, isolate_);
    } else {
      DCHECK(IsWasmTrustedInstanceData(implicit_arg));
      func_ref = WasmInternalFunction::GetOrCreateExternal(
          direct_handle(wasm_internal_function, isolate_));
      DCHECK(IsJSFunction(*func_ref) || IsUndefined(*func_ref));
    }
  }

  const FunctionSig* signature = module_->signature({sig_index});

  // ExternalCall
  HandleScope handle_scope(isolate_);  // Avoid leaking handles.

  // Store a pointer to the current FrameState before leaving the current
  // Activation.
  WasmInterpreterThread* thread = this->thread();
  current_frame_.current_bytecode_ = current_code;
  thread->SetCurrentFrame(current_frame_);
  thread->SetCurrentActivationFrame(
      sp, slot_offset, stack_pos,
      current_frame_.ref_array_current_sp_ + ref_stack_fp_offset,
      ref_stack_fp_offset);

  // We should not allocate anything in the heap and avoid GCs after we
  // store ref arguments into stack slots.
  DisallowHeapAllocation no_gc;

  // Note that tail calls to host functions do not have to guarantee tail
  // behaviour, so it is ok to recursively allocate C++ stack frames here.
  uint8_t* fp = reinterpret_cast<uint8_t*>(sp) + slot_offset;
  StoreRefArgsIntoStackSlots(fp, ref_stack_fp_offset, signature);
  ExternalCallResult result =
      CallExternalJSFunction(current_code, module_, func_ref, signature,
                             sp + slot_offset / kSlotSize, return_slot_offset);
  if (result == ExternalCallResult::EXTERNAL_RETURNED) {
    StoreRefResultsIntoRefStack(fp, ref_stack_fp_offset, signature);
  } else {  // ExternalCallResult::EXTERNAL_EXCEPTION
    AllowHeapAllocation allow_gc;

    if (HandleException(sp, current_code) ==
        WasmInterpreterThread::ExceptionHandlingResult::UNWOUND) {
      thread->Stop();
      RedirectCodeToUnwindHandler(current_code);
    } else {
      thread->Run();
    }
  }

  if (is_tail_call) {
    RedirectCodeToUnwindHandler(current_code);
  }
}

ExternalCallResult WasmInterpreterRuntime::CallImportedFunction(
    const uint8_t*& current_code, uint32_t function_index, uint32_t* sp,
    uint32_t current_stack_size, uint32_t ref_stack_fp_offset,
    uint32_t current_slot_offset) {
  DCHECK_GT(module_->num_imported_functions, function_index);
  HandleScope handle_scope(isolate_);  // Avoid leaking handles.

  const FunctionSig* sig = module_->functions[function_index].sig;

  ImportedFunctionEntry entry(wasm_trusted_instance_data(), function_index);
  int target_function_index = entry.function_index_in_called_module();
  if (target_function_index >= 0) {
    // WasmToWasm call.
    DCHECK(IsWasmTrustedInstanceData(entry.implicit_arg()));
    DirectHandle<WasmInstanceObject> target_instance(
        Cast<WasmInstanceObject>(
            Cast<WasmTrustedInstanceData>(entry.implicit_arg())
                ->instance_object()),
        isolate_);

    // Make sure the WasmInterpreterObject and InterpreterHandle for this
    // instance exist.
    DirectHandle<Tuple2> interpreter_object =
        WasmTrustedInstanceData::GetOrCreateInterpreterObject(target_instance);
    GetOrCreateInterpreterHandle(isolate_, interpreter_object);

    Address frame_pointer = FindInterpreterEntryFramePointer(isolate_);

    {
      // We should not allocate anything in the heap and avoid GCs after we
      // store ref arguments into stack slots.
      DisallowHeapAllocation no_gc;

      uint8_t* fp = reinterpret_cast<uint8_t*>(sp);
      StoreRefArgsIntoStackSlots(fp, ref_stack_fp_offset, sig);
      // Note that tail calls across WebAssembly module boundaries should
      // guarantee tail behavior, so this implementation does not conform to the
      // spec for a tail call. But it is really difficult to implement
      // cross-instance calls in the interpreter without recursively adding C++
      // stack frames.

      // TODO(paolosev@microsoft.com) - Is it possible to short-circuit this in
      // the case where we are calling a function in the same Wasm instance,
      // with a simple call to WasmInterpreterRuntime::ExecuteFunction()?
      bool success = WasmInterpreterObject::RunInterpreter(
          isolate_, frame_pointer, target_instance, target_function_index, fp);
      if (success) {
        StoreRefResultsIntoRefStack(fp, ref_stack_fp_offset, sig);

#ifdef V8_ENABLE_DRUMBRAKE_TRACING
        // Update shadow stack
        if (v8_flags.trace_drumbrake_execution && shadow_stack_ != nullptr) {
          for (size_t i = 0; i < sig->parameter_count(); i++) {
            TracePop();
          }

          for (size_t i = 0; i < sig->return_count(); i++) {
            current_slot_offset +=
                TracePush(sig->GetReturn(i).kind(), current_slot_offset);
          }
        }
#endif  // V8_ENABLE_DRUMBRAKE_TRACING
        return ExternalCallResult::EXTERNAL_RETURNED;
      }
      return ExternalCallResult::EXTERNAL_EXCEPTION;
    }
  } else {
    // WasmToJS call.

    // Note that tail calls to host functions do not have to guarantee tail
    // behaviour, so it is ok to recursively allocate C++ stack frames here.

    DirectHandle<Object> object_implicit_arg(entry.implicit_arg(), isolate_);

    // We should not allocate anything in the heap and avoid GCs after we store
    // ref arguments into stack slots.
    DisallowHeapAllocation no_gc;

    uint8_t* fp = reinterpret_cast<uint8_t*>(sp);
    StoreRefArgsIntoStackSlots(fp, ref_stack_fp_offset, sig);
    ExternalCallResult result =
        CallExternalJSFunction(current_code, module_, object_implicit_arg, sig,
                               sp, current_slot_offset);
    if (result == ExternalCallResult::EXTERNAL_RETURNED) {
      StoreRefResultsIntoRefStack(fp, ref_stack_fp_offset, sig);
    }
    return result;
  }
}

// static
int WasmInterpreterRuntime::memory_start_offset() {
  return OFFSET_OF(WasmInterpreterRuntime, memory_start_);
}

// static
int WasmInterpreterRuntime::instruction_table_offset() {
  return OFFSET_OF(WasmInterpreterRuntime, instruction_table_);
}

struct StackHandlerMarker {
  Address next;
  Address padding;
};

void WasmInterpreterRuntime::CallWasmToJSBuiltin(
    Isolate* isolate, DirectHandle<Object> object_ref, Address packed_args,
    const FunctionSig* sig) {
  DCHECK(!WasmBytecode::ContainsSimd(sig));
  DirectHandle<Object> callable;
  if (IsWasmImportData(*object_ref)) {
    callable =
        direct_handle(Cast<WasmImportData>(*object_ref)->callable(), isolate);
  } else {
    callable = object_ref;
    DCHECK(!IsUndefined(*callable));
  }

  // TODO(paolosev@microsoft.com) - Can callable be a JSProxy?
  DirectHandle<Object> js_function = callable;
  while (IsJSBoundFunction(*js_function, isolate_)) {
    if (IsJSBoundFunction(*js_function, isolate_)) {
      js_function = direct_handle(
          Cast<JSBoundFunction>(js_function)->bound_target_function(), isolate);
    }
  }

  if (IsJSProxy(*js_function, isolate_)) {
    do {
      Tagged<HeapObject> target = Cast<JSProxy>(js_function)->target(isolate);
      js_function = DirectHandle<Object>(target, isolate);
    } while (IsJSProxy(*js_function, isolate_));
  }

  if (!IsJSFunction(*js_function, isolate_)) {
    AllowHeapAllocation allow_gc;

    isolate->set_exception(*isolate_->factory()->NewTypeError(
        MessageTemplate::kWasmTrapJSTypeError));
    return;
  }

  // Save and restore context around invocation and block the
  // allocation of handles without explicit handle scopes.
  SaveContext save(isolate);
  SealHandleScope shs(isolate);

  Address saved_c_entry_fp = *isolate->c_entry_fp_address();
  Address saved_js_entry_sp = *isolate->js_entry_sp_address();
  if (saved_js_entry_sp == kNullAddress) {
    *isolate->js_entry_sp_address() = GetCurrentStackPosition();
  }
  StackHandlerMarker stack_handler;
  stack_handler.next = isolate->thread_local_top()->handler_;
#ifdef V8_USE_ADDRESS_SANITIZER
  stack_handler.padding = GetCurrentStackPosition();
#else
  stack_handler.padding = 0;
#endif
  // {saved_c_entry_fp} can be null if we run the interpreter directly from the
  // fuzzer, not from JS. In this case, we need to break the handler chain.
  isolate->thread_local_top()->handler_ =
      saved_c_entry_fp ? reinterpret_cast<Address>(&stack_handler)
                       : kNullAddress;
  {
    RCS_SCOPE(isolate, RuntimeCallCounterId::kJS_Execution);
    Address result = generic_wasm_to_js_interpreter_wrapper_fn_.Call(
        (*js_function).ptr(), packed_args, isolate->isolate_root(), sig,
        saved_c_entry_fp, (*callable).ptr());
    if (result != WasmToJSInterpreterFrameConstants::kSuccess) {
      isolate->set_exception(Tagged<Object>(result));
    } else {
      current_thread_->Run();
    }
  }

  isolate->thread_local_top()->handler_ = stack_handler.next;
  if (saved_js_entry_sp == kNullAddress) {
    *isolate->js_entry_sp_address() = saved_js_entry_sp;
  }
  *isolate->c_entry_fp_address() = saved_c_entry_fp;
}

ExternalCallResult WasmInterpreterRuntime::CallExternalJSFunction(
    const uint8_t*& current_code, const WasmModule* module,
    DirectHandle<Object> object_ref, const FunctionSig* sig, uint32_t* sp,
    uint32_t return_slot_offset) {
  // TODO(paolosev@microsoft.com) Cache IsJSCompatibleSignature result?
  if (!IsJSCompatibleSignature(
          reinterpret_cast<const wasm::CanonicalSig*>(sig))) {
    AllowHeapAllocation allow_gc;

    isolate_->Throw(*isolate_->factory()->NewTypeError(
        MessageTemplate::kWasmTrapJSTypeError));
    return ExternalCallResult::EXTERNAL_EXCEPTION;
  }

#ifdef V8_ENABLE_DRUMBRAKE_TRACING
  if (v8_flags.trace_drumbrake_execution) {
    Trace("  => Calling external function\n");
  }
#endif  // V8_ENABLE_DRUMBRAKE_TRACING

  CWasmArgumentsPacker packer(CWasmArgumentsPacker::TotalSize(
      reinterpret_cast<const wasm::CanonicalSig*>(sig)));
  uint32_t* p = sp + WasmBytecode::RetsSizeInSlots(sig);
  for (size_t i = 0; i < sig->parameter_count(); ++i) {
    switch (sig->GetParam(i).kind()) {
      case kI32:
        packer.Push(
            base::ReadUnalignedValue<int32_t>(reinterpret_cast<Address>(p)));
        p += sizeof(int32_t) / kSlotSize;
        break;
      case kI64:
        packer.Push(
            base::ReadUnalignedValue<int64_t>(reinterpret_cast<Address>(p)));
        p += sizeof(int64_t) / kSlotSize;
        break;
      case kF32:
        packer.Push(
            base::ReadUnalignedValue<float>(reinterpret_cast<Address>(p)));
        p += sizeof(float) / kSlotSize;
        break;
      case kF64:
        packer.Push(
            base::ReadUnalignedValue<double>(reinterpret_cast<Address>(p)));
        p += sizeof(double) / kSlotSize;
        break;
      case kRef:
      case kRefNull: {
        DirectHandle<Object> ref =
            base::ReadUnalignedValue<WasmRef>(reinterpret_cast<Address>(p));
        ref = WasmToJSObject(ref);
        packer.Push(*ref);
        p += sizeof(WasmRef) / kSlotSize;
        break;
      }
      case kS128:
      default:
        UNREACHABLE();
    }
  }

  DCHECK_NOT_NULL(current_thread_);
  current_thread_->StopExecutionTimer();
  {
    // If there were Ref values passed as arguments they have already been read
    // in BeginExecution(), so we can re-enable GC.
    AllowHeapAllocation allow_gc;

    CallWasmToJSBuiltin(isolate_, object_ref, packer.argv(), sig);
  }
  current_thread_->StartExecutionTimer();

#ifdef V8_ENABLE_DRUMBRAKE_TRACING
  if (v8_flags.trace_drumbrake_execution) {
    Trace("  => External wasm function returned%s\n",
          isolate_->has_exception() ? " with exception" : "");
  }
#endif  // V8_ENABLE_DRUMBRAKE_TRACING

  if (V8_UNLIKELY(isolate_->has_exception())) {
    return ExternalCallResult::EXTERNAL_EXCEPTION;
  }

  // Push return values.
  if (sig->return_count() > 0) {
    packer.Reset();
    for (size_t i = 0; i < sig->return_count(); i++) {
      switch (sig->GetReturn(i).kind()) {
        case kI32:
          base::WriteUnalignedValue<int32_t>(reinterpret_cast<Address>(sp),
                                             packer.Pop<uint32_t>());
          sp += sizeof(uint32_t) / kSlotSize;
          break;
        case kI64:
          base::WriteUnalignedValue<uint64_t>(reinterpret_cast<Address>(sp),
                                              packer.Pop<uint64_t>());
          sp += sizeof(uint64_t) / kSlotSize;
          break;
        case kF32:
          base::WriteUnalignedValue<float>(reinterpret_cast<Address>(sp),
                                           packer.Pop<float>());
          sp += sizeof(float) / kSlotSize;
          break;
        case kF64:
          base::WriteUnalignedValue<double>(reinterpret_cast<Address>(sp),
                                            packer.Pop<double>());
          sp += sizeof(double) / kSlotSize;
          break;
        case kRef:
        case kRefNull:
          // TODO(paolosev@microsoft.com): Handle WasmNull case?
#ifdef V8_COMPRESS_POINTERS
        {
          Address address = packer.Pop<Address>();
          DirectHandle<Object> ref(Tagged<Object>(address), isolate_);
          if (sig->GetReturn(i).value_type_code() == wasm::kFuncRefCode &&
              i::IsNull(*ref, isolate_)) {
            ref = isolate_->factory()->wasm_null();
          }
          ref = JSToWasmObject(ref, sig->GetReturn(i));
          if (isolate_->has_exception()) {
            return ExternalCallResult::EXTERNAL_EXCEPTION;
          }
          base::WriteUnalignedValue<DirectHandle<Object>>(
              reinterpret_cast<Address>(sp), ref);
          sp += sizeof(WasmRef) / kSlotSize;
        }
#else
          CHECK(false);  // Not supported.
#endif  // V8_COMPRESS_POINTERS
        break;
        case kS128:
        default:
          UNREACHABLE();
      }
    }
  }

#ifdef V8_ENABLE_DRUMBRAKE_TRACING
  if (v8_flags.trace_drumbrake_execution && shadow_stack_ != nullptr) {
    for (size_t i = 0; i < sig->parameter_count(); i++) {
      TracePop();
    }

    for (size_t i = 0; i < sig->return_count(); i++) {
      return_slot_offset +=
          TracePush(sig->GetReturn(i).kind(), return_slot_offset);
    }
  }
#endif  // V8_ENABLE_DRUMBRAKE_TRACING

  return ExternalCallResult::EXTERNAL_RETURNED;
}

DirectHandle<Map> WasmInterpreterRuntime::RttCanon(uint32_t type_index) const {
  DirectHandle<Map> rtt{
      Cast<Map>(
          wasm_trusted_instance_data()->managed_object_maps()->get(type_index)),
      isolate_};
  return rtt;
}

std::pair<DirectHandle<WasmStruct>, const StructType*>
WasmInterpreterRuntime::StructNewUninitialized(uint32_t index) const {
  const StructType* struct_type = module_->struct_type({index});
  DirectHandle<Map> rtt = RttCanon(index);
  return {isolate_->factory()->NewWasmStructUninitialized(struct_type, rtt),
          struct_type};
}

std::pair<DirectHandle<WasmArray>, const ArrayType*>
WasmInterpreterRuntime::ArrayNewUninitialized(uint32_t length,
                                              uint32_t array_index) const {
  const ArrayType* array_type = GetArrayType(array_index);
  if (V8_UNLIKELY(static_cast<int>(length) < 0 ||
                  static_cast<int>(length) >
                      WasmArray::MaxLength(array_type))) {
    return {};
  }

  DirectHandle<Map> rtt = RttCanon(array_index);
  return {
      {isolate_->factory()->NewWasmArrayUninitialized(length, rtt), isolate_},
      array_type};
}

WasmRef WasmInterpreterRuntime::WasmArrayNewSegment(uint32_t array_index,
                                                    uint32_t segment_index,
                                                    uint32_t offset,
                                                    uint32_t length) {
  DirectHandle<Map> rtt = RttCanon(array_index);
  // Call runtime function Runtime_WasmArrayNewSegment. Store the arguments in
  // reverse order and pass a pointer to the first argument, which is the last
  // on the stack.
  //
  // args[args_length] -> |       rtt        |
  //                      |      length      |
  //                      |      offset      |
  //                      |  segment_index   |
  //    first_arg_addr -> | trusted_instance |
  //
  constexpr size_t kArgsLength = 5;
  Address args[kArgsLength] = {rtt->ptr(), IntToSmi(length), IntToSmi(offset),
                               IntToSmi(segment_index),
                               wasm_trusted_instance_data()->ptr()};
  Address* first_arg_addr = &args[kArgsLength - 1];

  // A runtime function can throw, therefore we need to make sure that the
  // current activation is up-to-date, if we need to traverse the call stack.
  current_thread_->SetCurrentFrame(current_frame_);

  Address result =
      Runtime_WasmArrayNewSegment(kArgsLength, first_arg_addr, isolate_);
  if (isolate_->has_exception()) return {};

  return direct_handle(Tagged<Object>(result), isolate_);
}

bool WasmInterpreterRuntime::WasmArrayInitSegment(uint32_t segment_index,
                                                  WasmRef wasm_array,
                                                  uint32_t array_offset,
                                                  uint32_t segment_offset,
                                                  uint32_t length) {
  // Call runtime function Runtime_WasmArrayInitSegment. Store the arguments in
  // reverse order and pass a pointer to the first argument, which is the last
  // on the stack.
  //
  // args[args_length] -> |      length       |
  //                      |  segment_offset   |
  //                      |   array_offset    |
  //                      |    wasm_array     |
  //                      |   segment_index   |
  //    first_arg_addr -> | trusted_instance  |
  //
  constexpr size_t kArgsLength = 6;
  Address args[kArgsLength] = {
      IntToSmi(length),        IntToSmi(segment_offset),
      IntToSmi(array_offset),  (*wasm_array).ptr(),
      IntToSmi(segment_index), wasm_trusted_instance_data()->ptr()};
  Address* first_arg_addr = &args[kArgsLength - 1];

  // A runtime function can throw, therefore we need to make sure that the
  // current activation is up-to-date, if we need to traverse the call stack.
  current_thread_->SetCurrentFrame(current_frame_);

  Runtime_WasmArrayInitSegment(kArgsLength, first_arg_addr, isolate_);
  return (!isolate_->has_exception());
}

bool WasmInterpreterRuntime::WasmArrayCopy(WasmRef dest_wasm_array,
                                           uint32_t dest_index,
                                           WasmRef src_wasm_array,
                                           uint32_t src_index,
                                           uint32_t length) {
  // Call runtime function Runtime_WasmArrayCopy. Store the arguments in reverse
  // order and pass a pointer to the first argument, which is the last on the
  // stack.
  //
  // args[args_length] -> |     length     |
  //                      |   src_index    |
  //                      |   src_array    |
  //                      |   dest_index   |
  //    first_arg_addr -> |   dest_array   |
  //
  constexpr size_t kArgsLength = 5;
  Address args[kArgsLength] = {IntToSmi(length), IntToSmi(src_index),
                               (*src_wasm_array).ptr(), IntToSmi(dest_index),
                               (*dest_wasm_array).ptr()};
  Address* first_arg_addr = &args[kArgsLength - 1];

  // A runtime function can throw, therefore we need to make sure that the
  // current activation is up-to-date, if we need to traverse the call stack.
  current_thread_->SetCurrentFrame(current_frame_);

  Runtime_WasmArrayCopy(kArgsLength, first_arg_addr, isolate_);
  return (!isolate_->has_exception());
}

WasmRef WasmInterpreterRuntime::WasmJSToWasmObject(
    WasmRef extern_ref, ValueType value_type, uint32_t canonical_index) const {
  // Call runtime function Runtime_WasmJSToWasmObject. Store the arguments in
  // reverse order and pass a pointer to the first argument, which is the last
  // on the stack.
  //
  // args[args_length] -> | value_type represent.|
  //    first_arg_addr -> |      extern_ref      |
  //
  constexpr size_t kArgsLength = 2;
  Address args[kArgsLength] = {
      IntToSmi(value_type.raw_bit_field()), (*extern_ref).ptr()};
  Address* first_arg_addr = &args[kArgsLength - 1];

  // A runtime function can throw, therefore we need to make sure that the
  // current activation is up-to-date, if we need to traverse the call stack.
  current_thread_->SetCurrentFrame(current_frame_);

  Address result =
      Runtime_WasmJSToWasmObject(kArgsLength, first_arg_addr, isolate_);
  if (isolate_->has_exception()) return {};

  return direct_handle(Tagged<Object>(result), isolate_);
}

WasmRef WasmInterpreterRuntime::JSToWasmObject(WasmRef extern_ref,
                                               ValueType type) const {
  DirectHandle<Object> result;
  const char* error_message;
  if (wasm::JSToWasmObject(isolate_, module_, extern_ref, type, &error_message)
          .ToHandle(&result)) {
    return result;
  }

  {
    // Only in case of exception it can allocate.
    AllowHeapAllocation allow_gc;

    Tagged<Object> error = isolate_->Throw(*isolate_->factory()->NewTypeError(
        MessageTemplate::kWasmTrapJSTypeError));
    return direct_handle(error, isolate_);
  }
}

WasmRef WasmInterpreterRuntime::WasmToJSObject(WasmRef value) const {
  if (IsWasmFuncRef(*value)) {
    value =
        direct_handle(Cast<WasmFuncRef>(*value)->internal(isolate_), isolate_);
  }
  if (IsWasmInternalFunction(*value)) {
    DirectHandle<WasmInternalFunction> internal =
        Cast<WasmInternalFunction>(value);
    return WasmInternalFunction::GetOrCreateExternal(internal);
  }
  if (IsWasmNull(*value)) {
    return direct_handle(ReadOnlyRoots(isolate_).null_value(), isolate_);
  }
  return value;
}

// Implementation similar to Liftoff's SubtypeCheck in
// src\wasm\baseline\liftoff-compiler.cc.
bool WasmInterpreterRuntime::SubtypeCheck(Tagged<Map> rtt,
                                          Tagged<Map> formal_rtt,
                                          uint32_t type_index) const {
  // Constant-time subtyping check: load exactly one candidate RTT from the
  // supertypes list.
  // Step 1: load the WasmTypeInfo.
  Tagged<WasmTypeInfo> type_info = rtt->wasm_type_info();

  // Step 2: check the list's length if needed.
  uint32_t rtt_depth =
      GetSubtypingDepth(module_, ModuleTypeIndex({type_index}));
  if (rtt_depth >= kMinimumSupertypeArraySize &&
      static_cast<uint32_t>(type_info->supertypes_length()) <= rtt_depth) {
    return false;
  }

  // Step 3: load the candidate list slot into {tmp1}, and compare it.
  Tagged<Object> supertype = type_info->supertypes(rtt_depth);
  if (formal_rtt != supertype) return false;
  return true;
}

// Implementation similar to Liftoff's SubtypeCheck in
// src\wasm\baseline\liftoff-compiler.cc.
bool WasmInterpreterRuntime::SubtypeCheck(const WasmRef obj,
                                          const ValueType obj_type,
                                          const DirectHandle<Map> rtt,
                                          const ModuleTypeIndex target_type,
                                          bool null_succeeds) const {
  bool is_cast_from_any = obj_type.is_reference_to(HeapType::kAny);

  // Skip the null check if casting from any and not {null_succeeds}.
  // In that case the instance type check will identify null as not being a
  // wasm object and fail.
  if (obj_type.is_nullable() && (!is_cast_from_any || null_succeeds)) {
    if (obj_type == kWasmExternRef || obj_type == kWasmNullExternRef) {
      if (i::IsNull(*obj, isolate_)) return null_succeeds;
    } else {
      if (i::IsWasmNull(*obj, isolate_)) return null_succeeds;
    }
  }

  // Add Smi check if the source type may store a Smi (i31ref or JS Smi).
  if (IsSubtypeOf(kWasmRefI31, obj_type, module_) && IsSmi(*obj)) {
    return false;
  }

  if (!IsHeapObject(*obj)) return false;
  Tagged<Map> obj_map = Cast<HeapObject>(obj)->map();

  if (module_->types[target_type.index].is_final) {
    // In this case, simply check for map equality.
    if (*obj_map != *rtt) {
      return false;
    }
  } else {
    // Check for rtt equality, and if not, check if the rtt is a struct/array
    // rtt.
    if (*obj_map == *rtt) {
      return true;
    }

    if (is_cast_from_any) {
      // Check for map being a map for a wasm object (struct, array, func).
      InstanceType obj_type1 = obj_map->instance_type();
      if (obj_type1 < FIRST_WASM_OBJECT_TYPE ||
          obj_type1 > LAST_WASM_OBJECT_TYPE) {
        return false;
      }
    }

    return SubtypeCheck(obj_map, *rtt, target_type.index);
  }

  return true;
}

using TypeChecker = bool (*)(const WasmRef obj);

template <TypeChecker type_checker>
bool AbstractTypeCast(Isolate* isolate, const WasmRef obj,
                      const ValueType obj_type, bool null_succeeds) {
  if (null_succeeds && obj_type.is_nullable() &&
      WasmInterpreterRuntime::IsNull(isolate, obj, obj_type)) {
    return true;
  }
  return type_checker(obj);
}

static bool EqCheck(const WasmRef obj) {
  if (IsSmi(*obj)) {
    return true;
  }
  if (!IsHeapObject(*obj)) return false;
  InstanceType instance_type = Cast<HeapObject>(obj)->map()->instance_type();
  return instance_type >= FIRST_WASM_OBJECT_TYPE &&
         instance_type <= LAST_WASM_OBJECT_TYPE;
}
bool WasmInterpreterRuntime::RefIsEq(const WasmRef obj,
                                     const ValueType obj_type,
                                     bool null_succeeds) const {
  return AbstractTypeCast<&EqCheck>(isolate_, obj, obj_type, null_succeeds);
}

static bool I31Check(const WasmRef obj) { return IsSmi(*obj); }
bool WasmInterpreterRuntime::RefIsI31(const WasmRef obj,
                                      const ValueType obj_type,
                                      bool null_succeeds) const {
  return AbstractTypeCast<&I31Check>(isolate_, obj, obj_type, null_succeeds);
}

static bool StructCheck(const WasmRef obj) {
  if (IsSmi(*obj)) {
    return false;
  }
  if (!IsHeapObject(*obj)) return false;
  InstanceType instance_type = Cast<HeapObject>(obj)->map()->instance_type();
  return instance_type == WASM_STRUCT_TYPE;
}
bool WasmInterpreterRuntime::RefIsStruct(const WasmRef obj,
                                         const ValueType obj_type,
                                         bool null_succeeds) const {
  return AbstractTypeCast<&StructCheck>(isolate_, obj, obj_type, null_succeeds);
}

static bool ArrayCheck(const WasmRef obj) {
  if (IsSmi(*obj)) {
    return false;
  }
  if (!IsHeapObject(*obj)) return false;
  InstanceType instance_type = Cast<HeapObject>(obj)->map()->instance_type();
  return instance_type == WASM_ARRAY_TYPE;
}
bool WasmInterpreterRuntime::RefIsArray(const WasmRef obj,
                                        const ValueType obj_type,
                                        bool null_succeeds) const {
  return AbstractTypeCast<&ArrayCheck>(isolate_, obj, obj_type, null_succeeds);
}

static bool StringCheck(const WasmRef obj) {
  if (IsSmi(*obj)) {
    return false;
  }
  if (!IsHeapObject(*obj)) return false;
  InstanceType instance_type = Cast<HeapObject>(obj)->map()->instance_type();
  return instance_type < FIRST_NONSTRING_TYPE;
}
bool WasmInterpreterRuntime::RefIsString(const WasmRef obj,
                                         const ValueType obj_type,
                                         bool null_succeeds) const {
  return AbstractTypeCast<&StringCheck>(isolate_, obj, obj_type, null_succeeds);
}

void WasmInterpreterRuntime::SetTrap(TrapReason trap_reason, pc_t trap_pc) {
  trap_function_index_ =
      current_frame_.current_function_
          ? current_frame_.current_function_->GetFunctionIndex()
          : 0;
  DCHECK_GE(trap_function_index_, 0);
  DCHECK_LT(trap_function_index_, module_->functions.size());

  trap_pc_ = trap_pc;
  thread()->Trap(trap_reason, trap_function_index_, static_cast<int>(trap_pc_),
                 current_frame_);
}

void WasmInterpreterRuntime::SetTrap(TrapReason trap_reason,
                                     const uint8_t*& code) {
  SetTrap(trap_reason,
          current_frame_.current_function_
              ? current_frame_.current_function_->GetPcFromTrapCode(code)
              : 0);
  RedirectCodeToUnwindHandler(code);
}

void WasmInterpreterRuntime::ResetCurrentHandleScope() {
  current_frame_.ResetHandleScope(isolate_);
}

std::vector<WasmInterpreterStackEntry>
WasmInterpreterRuntime::GetInterpretedStack(Address frame_pointer) {
  // The current thread can be nullptr if we throw an exception before calling
  // {BeginExecution}.
  if (current_thread_) {
    WasmInterpreterThread::Activation* activation =
        current_thread_->GetActivation(frame_pointer);
    if (activation) {
      return activation->GetStackTrace();
    }

    if (trap_function_index_ < 0) {
      // It is possible to have a trap before starting to execute Wasm code.
      // This can happen if the JSToWasmObject fails for a type mismatch in the
      // JSToWasmInterpreterWrapper. In this case we know that we were going to
      // execute the initial function {start_function_index_}.
      trap_function_index_ = start_function_index_;
    }

    return {{trap_function_index_, static_cast<int>(trap_pc_)}};
  }

  // It is possible to throw before entering a Wasm function, while converting
  // the args from JS to Wasm, with JSToWasmObject.
  return {{0, 0}};
}

int WasmInterpreterRuntime::GetFunctionIndex(Address frame_pointer,
                                             int index) const {
  if (current_thread_) {
    WasmInterpreterThread::Activation* activation =
        current_thread_->GetActivation(frame_pointer);
    if (activation) {
      return activation->GetFunctionIndex(index);
    }
  }
  return -1;
}

void WasmInterpreterRuntime::SetTrapFunctionIndex(int32_t func_index) {
  trap_function_index_ = func_index;
  trap_pc_ = 0;
}

void WasmInterpreterRuntime::PrintStack(uint32_t* sp, RegMode reg_mode,
                                        int64_t r0, double fp0) {
#ifdef V8_ENABLE_DRUMBRAKE_TRACING
  if (tracer_ && tracer_->ShouldTraceFunction(
                     current_frame_.current_function_->GetFunctionIndex())) {
    shadow_stack_->Print(this, sp, current_frame_.current_stack_start_args_,
                         current_frame_.current_stack_start_locals_,
                         current_frame_.current_stack_start_stack_, reg_mode,
                         r0, fp0);
  }
#endif  // V8_ENABLE_DRUMBRAKE_TRACING
}

#ifdef V8_ENABLE_DRUMBRAKE_TRACING
InterpreterTracer* WasmInterpreterRuntime::GetTracer() {
  if (tracer_ == nullptr) tracer_.reset(new InterpreterTracer(-1));
  return tracer_.get();
}

void WasmInterpreterRuntime::Trace(const char* format, ...) {
  if (!current_frame_.current_function_) {
    // This can happen when the entry function is an imported JS function.
    return;
  }
  InterpreterTracer* tracer = GetTracer();
  if (tracer->ShouldTraceFunction(
          current_frame_.current_function_->GetFunctionIndex())) {
    va_list arguments;
    va_start(arguments, format);
    base::OS::VFPrint(tracer->file(), format, arguments);
    va_end(arguments);
    tracer->CheckFileSize();
  }
}
#endif  // V8_ENABLE_DRUMBRAKE_TRACING

// static
ModuleWireBytes InterpreterHandle::GetBytes(Tagged<Tuple2> interpreter_object) {
  Tagged<WasmInstanceObject> wasm_instance =
      WasmInterpreterObject::get_wasm_instance(interpreter_object);
  NativeModule* native_module = wasm_instance->module_object()->native_module();
  return ModuleWireBytes{native_module->wire_bytes()};
}

InterpreterHandle::InterpreterHandle(Isolate* isolate,
                                     DirectHandle<Tuple2> interpreter_object)
    : isolate_(isolate),
      module_(WasmInterpreterObject::get_wasm_instance(*interpreter_object)
                  ->module()),
      interpreter_(isolate, module_, GetBytes(*interpreter_object),
                   direct_handle(WasmInterpreterObject::get_wasm_instance(
                                     *interpreter_object),
                                 isolate)) {}

inline WasmInterpreterThread::State InterpreterHandle::RunExecutionLoop(
    WasmInterpreterThread* thread, bool called_from_js) {
  // If there were Ref values passed as arguments they have already been read
  // in BeginExecution(), so we can re-enable GC.
  AllowHeapAllocation allow_gc;

  bool finished = false;
  WasmInterpreterThread::State state = thread->state();
  if (state != WasmInterpreterThread::State::RUNNING) {
    return state;
  }

  while (!finished) {
    state = ContinueExecution(thread, called_from_js);
    switch (state) {
      case WasmInterpreterThread::State::FINISHED:
      case WasmInterpreterThread::State::RUNNING:
        // Perfect, just break the switch and exit the loop.
        finished = true;
        break;
      case WasmInterpreterThread::State::TRAPPED: {
        if (!isolate_->has_exception()) {
          // An exception handler was found, keep running the loop.
          break;
        }
        thread->Stop();
        [[fallthrough]];
      }
      case WasmInterpreterThread::State::STOPPED:
        // An exception happened, and the current activation was unwound
        // without hitting a local exception handler. All that remains to be
        // done is finish the activation and let the exception propagate.
        DCHECK(isolate_->has_exception());
        return state;  // Either STOPPED or TRAPPED.
      case WasmInterpreterThread::State::EH_UNWINDING: {
        thread->Stop();
        return WasmInterpreterThread::State::STOPPED;
      }
    }
  }
  return state;
}

V8_EXPORT_PRIVATE bool InterpreterHandle::Execute(
    WasmInterpreterThread* thread, Address frame_pointer, uint32_t func_index,
    const std::vector<WasmValue>& argument_values,
    std::vector<WasmValue>& return_values) {
  DCHECK_GT(module()->functions.size(), func_index);
  const FunctionSig* sig = module()->functions[func_index].sig;
  DCHECK_EQ(sig->parameter_count(), argument_values.size());
  DCHECK_EQ(sig->return_count(), return_values.size());

  thread->StartExecutionTimer();
  interpreter_.BeginExecution(thread, func_index, frame_pointer,
                              thread->NextFrameAddress(),
                              thread->NextRefStackOffset(), argument_values);

  WasmInterpreterThread::State state = RunExecutionLoop(thread, true);
  thread->StopExecutionTimer();

  switch (state) {
    case WasmInterpreterThread::RUNNING:
    case WasmInterpreterThread::FINISHED:
      for (unsigned i = 0; i < sig->return_count(); ++i) {
        return_values[i] = interpreter_.GetReturnValue(i);
      }
      return true;

    case WasmInterpreterThread::TRAPPED:
      for (unsigned i = 0; i < sig->return_count(); ++i) {
        return_values[i] = WasmValue(0xDEADBEEF);
      }
      return false;

    case WasmInterpreterThread::STOPPED:
      return false;

    case WasmInterpreterThread::EH_UNWINDING:
      UNREACHABLE();
  }
}

bool InterpreterHandle::Execute(WasmInterpreterThread* thread,
                                Address frame_pointer, uint32_t func_index,
                                uint8_t* interpreter_fp) {
  DCHECK_GT(module()->functions.size(), func_index);

  interpreter_.BeginExecution(thread, func_index, frame_pointer,
                              interpreter_fp);
  WasmInterpreterThread::State state = RunExecutionLoop(thread, false);
  return (state == WasmInterpreterThread::RUNNING ||
          state == WasmInterpreterThread::FINISHED);
}

DirectHandle<WasmInstanceObject> InterpreterHandle::GetInstanceObject() {
  DebuggableStackFrameIterator it(isolate_);
  WasmInterpreterEntryFrame* frame =
      WasmInterpreterEntryFrame::cast(it.frame());
  DirectHandle<WasmInstanceObject> instance_obj(frame->wasm_instance(),
                                                isolate_);
  // Check that this is indeed the instance which is connected to this
  // interpreter.
  DCHECK_EQ(this,
            Cast<Managed<InterpreterHandle>>(
                WasmInterpreterObject::get_interpreter_handle(
                    instance_obj->trusted_data(isolate_)->interpreter_object()))
                ->raw());
  return instance_obj;
}

std::vector<WasmInterpreterStackEntry> InterpreterHandle::GetInterpretedStack(
    Address frame_pointer) {
  return interpreter_.GetInterpretedStack(frame_pointer);
}

int InterpreterHandle::GetFunctionIndex(Address frame_pointer,
                                        int index) const {
  return interpreter_.GetFunctionIndex(frame_pointer, index);
}

void InterpreterHandle::SetTrapFunctionIndex(int32_t func_index) {
  interpreter_.SetTrapFunctionIndex(func_index);
}

}  // namespace wasm
}  // namespace internal
}  // namespace v8
