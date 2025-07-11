// Copyright 2014 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_CODEGEN_BAILOUT_REASON_H_
#define V8_CODEGEN_BAILOUT_REASON_H_

#include <cstdint>

namespace v8 {
namespace internal {

#define ABORT_MESSAGES_LIST(V)                                                 \
  V(kNoReason, "no reason")                                                    \
                                                                               \
  V(k32BitValueInRegisterIsNotZeroExtended,                                    \
    "32 bit value in register is not zero-extended")                           \
  V(kSignedBitOfSmiIsNotZero, "Signed bit of 31 bit smi register is not zero") \
  V(kAPICallReturnedInvalidObject, "API call returned invalid object")         \
  V(kAccumulatorClobbered, "Accumulator clobbered")                            \
  V(kAllocatingNonEmptyPackedArray, "Allocating non-empty packed array")       \
  V(kAllocationIsNotDoubleAligned, "Allocation is not double aligned")         \
  V(kExpectedOptimizationSentinel,                                             \
    "Expected optimized code cell or optimization sentinel")                   \
  V(kExpectedOsrCode, "Expected OSR code")                                     \
  V(kExpectedUndefinedOrCell, "Expected undefined or cell in register")        \
  V(kExpectedFeedbackCell, "Expected feedback cell")                           \
  V(kExpectedFeedbackVector, "Expected feedback vector")                       \
  V(kExpectedBaselineData, "Expected baseline data")                           \
  V(kFloat64IsNotAInt32,                                                       \
    "Float64 cannot be converted to Int32 without loss of precision")          \
  V(kFunctionDataShouldBeBytecodeArrayOnInterpreterEntry,                      \
    "The function_data field should be a BytecodeArray on interpreter entry")  \
  V(kInputStringTooLong, "Input string too long")                              \
  V(kInputDoesNotFitSmi, "Input number is too large to fit in a Smi")          \
  V(kInvalidBytecode, "Invalid bytecode")                                      \
  V(kInvalidBytecodeAdvance, "Cannot advance current bytecode, ")              \
  V(kInvalidDeoptimizedCode, "Invoked code which is deoptimized")              \
  V(kInvalidHandleScopeLevel, "Invalid HandleScope level")                     \
  V(kInvalidJumpTableIndex, "Invalid jump table index")                        \
  V(kInvalidParametersAndRegistersInGenerator,                                 \
    "invalid parameters and registers in generator")                           \
  V(kMissingBytecodeArray, "Missing bytecode array from function")             \
  V(kObjectNotTagged, "The object is not tagged")                              \
  V(kObjectTagged, "The object is tagged")                                     \
  V(kOffsetOutOfRange, "Offset out of range")                                  \
  V(kOperandIsASmi, "Operand is a smi")                                        \
  V(kOperandIsASmiAndNotABoundFunction,                                        \
    "Operand is a smi and not a bound function")                               \
  V(kOperandIsASmiAndNotAConstructor,                                          \
    "Operand is a smi and not a constructor")                                  \
  V(kOperandIsASmiAndNotAFunction, "Operand is a smi and not a function")      \
  V(kOperandIsASmiAndNotAGeneratorObject,                                      \
    "Operand is a smi and not a generator object")                             \
  V(kOperandIsCleared, "Operand is cleared")                                   \
  V(kOperandIsNotABoundFunction, "Operand is not a bound function")            \
  V(kOperandIsNotAConstructor, "Operand is not a constructor")                 \
  V(kOperandIsNotAFixedArray, "Operand is not a fixed array")                  \
  V(kOperandIsNotAFunction, "Operand is not a function")                       \
  V(kOperandIsNotACallableFunction, "Operand is not a callable function")      \
  V(kOperandIsNotAGeneratorObject, "Operand is not a generator object")        \
  V(kOperandIsNotACode, "Operand is not a Code object")                        \
  V(kOperandIsNotAMap, "Operand is not a Map object")                          \
  V(kOperandIsNotASmi, "Operand is not a smi")                                 \
  V(kPromiseAlreadySettled, "Promise already settled")                         \
  V(kReceivedInvalidReturnAddress, "Received invalid return address")          \
  V(kRegisterDidNotMatchExpectedRoot, "Register did not match expected root")  \
  V(kReturnAddressNotFoundInFrame, "Return address not found in frame")        \
  V(kShouldNotDirectlyEnterOsrFunction,                                        \
    "Should not directly enter OSR-compiled function")                         \
  V(kStackAccessBelowStackPointer, "Stack access below stack pointer")         \
  V(kOsrUnexpectedStackSize, "Unexpected stack size on OSR entry")             \
  V(kStackFrameTypesMustMatch, "Stack frame types must match")                 \
  V(kUint32IsNotAInt32,                                                        \
    "Uint32 cannot be converted to Int32 without loss of precision")           \
  V(kUnalignedCellInWriteBarrier, "Unaligned cell in write barrier")           \
  V(kUnexpectedAdditionalPopValue, "Unexpected additional pop value")          \
  V(kUnexpectedElementsKindInArrayConstructor,                                 \
    "Unexpected ElementsKind in array constructor")                            \
  V(kUnexpectedFPCRMode, "Unexpected FPCR mode.")                              \
  V(kUnexpectedFunctionIDForInvokeIntrinsic,                                   \
    "Unexpected runtime function id for the InvokeIntrinsic bytecode")         \
  V(kUnexpectedInitialMapForArrayFunction,                                     \
    "Unexpected initial map for Array function")                               \
  V(kUnexpectedLevelAfterReturnFromApiCall,                                    \
    "Unexpected level after return from api call")                             \
  V(kUnexpectedNegativeValue, "Unexpected negative value")                     \
  V(kUnexpectedReturnFromFrameDropper,                                         \
    "Unexpectedly returned from dropping frames")                              \
  V(kUnexpectedReturnFromThrow, "Unexpectedly returned from a throw")          \
  V(kUnexpectedReturnFromWasmTrap,                                             \
    "Should not return after throwing a wasm trap")                            \
  V(kUnexpectedStackPointer, "The stack pointer is not the expected value")    \
  V(kUnexpectedValue, "Unexpected value")                                      \
  V(kUninhabitableType, "Uninhabitable type")                                  \
  V(kUnsupportedDeopt,                                                         \
    "Lazy deopt after a fast API call with return value is unsupported")       \
  V(kUnsupportedModuleOperation, "Unsupported module operation")               \
  V(kUnsupportedNonPrimitiveCompare, "Unsupported non-primitive compare")      \
  V(kWrongAddressOrValuePassedToRecordWrite,                                   \
    "Wrong address or value passed to RecordWrite")                            \
  V(kWrongArgumentCountForInvokeIntrinsic,                                     \
    "Wrong number of arguments for intrinsic")                                 \
  V(kWrongFunctionCodeStart, "Wrong value in code start register passed")      \
  V(kWrongFunctionContext, "Wrong context passed to function")                 \
  V(kWrongFunctionDispatchHandle,                                              \
    "Wrong value in dispatch handle register passed")                          \
  V(kInvalidReceiver, "Expected JS object or primitive object")                \
  V(kUnexpectedInstanceType, "Unexpected instance type encountered")           \
  V(kTurboshaftTypeAssertionFailed,                                            \
    "A type assertion failed in Turboshaft-generated code")                    \
  V(kTurboshaftLoadEliminationError,                                           \
    "Turboshaft's load elimination wrongly eliminated a Load")                 \
  V(kMetadataAreaStartDoesNotMatch,                                            \
    "The metadata doesn't belong to the chunk")                                \
  V(kExternalPointerTagMismatch,                                               \
    "Tag mismatch during external pointer access")                             \
  V(kJSSignatureMismatch, "Signature mismatch during JS function call")        \
  V(kWasmSignatureMismatch, "Signature mismatch during Wasm indirect call")    \
  V(kFastCallFallbackInvalid, "Fast call fallback returned incorrect type")    \
  V(k32BitValueInRegisterIsNotSignExtended,                                    \
    "32 bit value in register is not sign-extended")                           \
  V(kUnexpectedSandboxMode, "The sandboxing mode is not as expected")

#define TERMINAL_BAILOUT_MESSAGES_LIST(V)                                  \
  V(kFunctionTooBig, "Function is too big to be optimized")                \
  V(kTooManyArguments, "Function contains a call with too many arguments") \
  V(kNativeFunctionLiteral, "Native function literal")                     \
  V(kNeverOptimize, "Optimization is always disabled")

#define TERMINAL_TURBOFAN_BAILOUT_MESSAGES_LIST(V)  \
  V(kTurbofanGraphBuildingFailed,                   \
    "Turbofan optimized graph construction failed") \
  V(kTurbofanCodeGenerationFailed, "Turbofan code generation failed")

#define TERMINAL_MAGLEV_BAILOUT_MESSAGES_LIST(V)                              \
  V(kMaglevGraphBuildingFailed, "Maglev optimized graph construction failed") \
  V(kMaglevCodeGenerationFailed, "Maglev code generation failed")

#define BAILOUT_MESSAGES_LIST(V)                                             \
  V(kNoReason, "no reason")                                                  \
                                                                             \
  TERMINAL_BAILOUT_MESSAGES_LIST(V)                                          \
  TERMINAL_MAGLEV_BAILOUT_MESSAGES_LIST(V)                                   \
  TERMINAL_TURBOFAN_BAILOUT_MESSAGES_LIST(V)                                 \
  V(kBailedOutDueToDependencyChange, "Bailed out due to dependency change")  \
  V(kConcurrentMapDeprecation, "Maps became deprecated during optimization") \
  V(kFunctionBeingDebugged, "Function is being debugged")                    \
  V(kDetachedNativeContext, "The native context is detached")                \
  V(kCancelled, "Job got cancelled")                                         \
  V(kLiveEdit, "LiveEdit")

#define ERROR_MESSAGES_CONSTANTS(C, T) C,
enum class BailoutReason : uint8_t {
  BAILOUT_MESSAGES_LIST(ERROR_MESSAGES_CONSTANTS) kLastErrorMessage
};

enum class AbortReason : uint8_t {
  ABORT_MESSAGES_LIST(ERROR_MESSAGES_CONSTANTS) kLastErrorMessage
};
#undef ERROR_MESSAGES_CONSTANTS

const char* GetBailoutReason(BailoutReason reason);
const char* GetAbortReason(AbortReason reason);
bool IsValidAbortReason(int reason_id);

inline bool IsTerminalBailoutReason(BailoutReason reason) {
  switch (reason) {
#define CASE(name, _) case BailoutReason::name:
    TERMINAL_BAILOUT_MESSAGES_LIST(CASE)
#undef CASE
    return true;
    default:
      return false;
  }
}

inline bool IsTerminalBailoutReasonForMaglev(BailoutReason reason) {
  switch (reason) {
#define CASE(name, _) case BailoutReason::name:
    TERMINAL_BAILOUT_MESSAGES_LIST(CASE)
    TERMINAL_MAGLEV_BAILOUT_MESSAGES_LIST(CASE)
#undef CASE
    return true;
    default:
      return false;
  }
}

inline bool IsTerminalBailoutReasonForTurbofan(BailoutReason reason) {
  switch (reason) {
#define CASE(name, _) case BailoutReason::name:
    TERMINAL_BAILOUT_MESSAGES_LIST(CASE)
    TERMINAL_MAGLEV_BAILOUT_MESSAGES_LIST(CASE)
#undef CASE
    return true;
    default:
      return false;
  }
}

}  // namespace internal
}  // namespace v8

#endif  // V8_CODEGEN_BAILOUT_REASON_H_
