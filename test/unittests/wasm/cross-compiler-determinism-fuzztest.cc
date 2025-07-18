// Copyright 2025 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/utils/ostreams.h"
#include "src/wasm/wasm-opcodes-inl.h"
#include "test/common/wasm/wasm-macro-gen.h"
#include "test/common/wasm/wasm-run-utils.h"
#include "test/unittests/fuzztest.h"
#include "test/unittests/test-utils.h"

namespace v8::internal::wasm {

class CrossCompilerDeterminismTest
    : public fuzztest::PerFuzzTestFixtureAdapter<TestWithNativeContext> {
 public:
  CrossCompilerDeterminismTest() = default;

  void TestFloat32Binop(WasmOpcode opcode, Float32 lhs, Float32 rhs);

 private:
  // IntRepresentation and FloatRepresentation allow to parameterize the test
  // for either passing integer values or floating point values.
  struct IntRepresentation {
    using c_type = uint32_t;
    static c_type FromFloat32(Float32 val) { return val.get_bits(); }
    static Float32 ToFloat32(c_type val) { return Float32::FromBits(val); }

    static constexpr std::array<uint8_t, 1> to_f32{kExprF32ReinterpretI32};
    static constexpr std::array<uint8_t, 1> from_f32{kExprI32ReinterpretF32};

    static auto Constant(Float32 value) {
      return std::to_array<uint8_t>({WASM_I32V(value.get_bits())});
    }
    static auto Param(int param_idx) {
      return std::to_array<uint8_t>({WASM_LOCAL_GET(param_idx)});
    }
  };
  struct FloatRepresentation {
    using c_type = float;
    static c_type FromFloat32(Float32 val) { return val.get_scalar(); }
    static Float32 ToFloat32(c_type val) {
      return Float32::FromNonSignallingFloat(val);
    }

    static constexpr std::array<uint8_t, 0> to_f32{};
    static constexpr std::array<uint8_t, 0> from_f32{};

    static auto Constant(Float32 value) {
      return std::to_array<uint8_t>({WASM_F32(value.get_scalar())});
    }
    static auto Param(int param_idx) {
      return std::to_array<uint8_t>({WASM_LOCAL_GET(param_idx)});
    }
  };

  // Those structs allow to reuse the same test logic for passing or embedding
  // values differently (constants or parameters or mixed).
  template <typename ValueRepresentation>
  struct ConstantInputs : public ValueRepresentation {
    using c_type = ValueRepresentation::c_type;
    using runner_type = CommonWasmRunner<c_type>;

    template <int param_idx>
    static auto GetParam(Float32 value) {
      return ValueRepresentation::Constant(value);
    }

    static auto Call(runner_type& runner, c_type, c_type) {
      return runner.Call();
    }
  };
  template <typename ValueRepresentation, int kIndexOfParam>
  struct ParamAndConstantInput : public ValueRepresentation {
    using c_type = ValueRepresentation::c_type;
    using runner_type = CommonWasmRunner<c_type, c_type>;

    template <int param_idx>
    static auto GetParam(Float32 value) {
      if constexpr (param_idx == kIndexOfParam) {
        return ValueRepresentation::Param(0);
      } else {
        return ValueRepresentation::Constant(value);
      }
    }

    static auto Call(runner_type& runner, c_type lhs, c_type rhs) {
      return runner.Call(kIndexOfParam == 0 ? lhs : rhs);
    }
  };
  template <typename ValueRepresentation>
  struct ParamInputs : public ValueRepresentation {
    using c_type = ValueRepresentation::c_type;
    using runner_type = CommonWasmRunner<c_type, c_type, c_type>;

    template <int param_idx>
    static auto GetParam(Float32 value) {
      return ValueRepresentation::Param(param_idx);
    }

    static auto Call(runner_type& runner, c_type lhs, c_type rhs) {
      return runner.Call(lhs, rhs);
    }
  };

  template <typename Config>
  Float32 GetFloat32BinopResult(TestExecutionTier tier, WasmOpcode opcode,
                                Float32 lhs, Float32 rhs) {
    // Instantiate the WasmRunner of the right type.
    typename Config::runner_type runner(isolate(), tier);
    constexpr size_t kMaxBytecodeSize = 16;
    // Build the bytecode.
    base::SmallVector<uint8_t, kMaxBytecodeSize> bytecode;
    bytecode.insert(bytecode.end(), Config::template GetParam<0>(lhs));
    bytecode.insert(bytecode.end(), Config::to_f32);
    bytecode.insert(bytecode.end(), Config::template GetParam<1>(rhs));
    bytecode.insert(bytecode.end(), Config::to_f32);
    bytecode.push_back(opcode);
    bytecode.insert(bytecode.end(), Config::from_f32);

    // Compile.
    DCHECK_GE(kMaxBytecodeSize, bytecode.size());
    runner.Build(base::VectorOf(bytecode));

    // Convert arguments from Float32, call Wasm, convert result to Float32.
    return Config::ToFloat32(Config::Call(runner, Config::FromFloat32(lhs),
                                          Config::FromFloat32(rhs)));
  }
};

void CrossCompilerDeterminismTest::TestFloat32Binop(WasmOpcode opcode,
                                                    Float32 lhs, Float32 rhs) {
  // Remember all values from all configurations.
  Float32 results[] = {
      // Liftoff does not special-handle float32 constants, so we only test this
      // one Liftoff mode.
      GetFloat32BinopResult<ConstantInputs<IntRepresentation>>(
          TestExecutionTier::kLiftoff, opcode, lhs, rhs),

      // Turbofan, inputs as i32.
      GetFloat32BinopResult<ConstantInputs<IntRepresentation>>(
          TestExecutionTier::kTurbofan, opcode, lhs, rhs),
      GetFloat32BinopResult<ParamAndConstantInput<IntRepresentation, 0>>(
          TestExecutionTier::kTurbofan, opcode, lhs, rhs),
      GetFloat32BinopResult<ParamAndConstantInput<IntRepresentation, 1>>(
          TestExecutionTier::kTurbofan, opcode, lhs, rhs),
      GetFloat32BinopResult<ParamInputs<IntRepresentation>>(
          TestExecutionTier::kTurbofan, opcode, lhs, rhs),

      // Turbofan, inputs as f32.
      GetFloat32BinopResult<ConstantInputs<FloatRepresentation>>(
          TestExecutionTier::kTurbofan, opcode, lhs, rhs),
      GetFloat32BinopResult<ParamAndConstantInput<FloatRepresentation, 0>>(
          TestExecutionTier::kTurbofan, opcode, lhs, rhs),
      GetFloat32BinopResult<ParamAndConstantInput<FloatRepresentation, 1>>(
          TestExecutionTier::kTurbofan, opcode, lhs, rhs),
      GetFloat32BinopResult<ParamInputs<FloatRepresentation>>(
          TestExecutionTier::kTurbofan, opcode, lhs, rhs),
  };

  auto equals_first = [first = results[0]](Float32 v) { return v == first; };
  ASSERT_TRUE(
      std::all_of(std::begin(results) + 1, std::end(results), equals_first))
      << absl::StrFormat("Operation %s on inputs %v and %v\n",
                         WasmOpcodes::OpcodeName(opcode), lhs, rhs)
      << "Different results for different configs: "
      << PrintCollection(base::VectorOf(results));
}

fuzztest::Domain<Float32> ArbitraryFloat32() {
  return fuzztest::ReversibleMap(
      // Mapping function.
      [](uint32_t bits) { return Float32::FromBits(bits); },
      // Inverse mapping function, for mapping back initial seeds.
      [](Float32 f) { return std::optional{std::tuple{f.get_bits()}}; },
      // Input to the mapping function.
      fuzztest::Arbitrary<uint32_t>());
}

constexpr std::tuple<WasmOpcode, Float32, Float32> kFloat32BinopSeeds[]{
    // NaN + NaN
    {kExprF32Add, Float32::quiet_nan(), Float32::quiet_nan()},
    // NaN + -NaN
    {kExprF32Add, Float32::quiet_nan(), -Float32::quiet_nan()},
    // inf + -inf
    {kExprF32Add, Float32::infinity(), -Float32::infinity()},
    // inf - inf
    {kExprF32Sub, Float32::infinity(), Float32::infinity()},
    // 0 * inf
    {kExprF32Mul, Float32::FromBits(0), Float32::infinity()},
    // -0 / 0
    {kExprF32Div, -Float32::FromBits(0), Float32::FromBits(0)},
    // -NaN / -0
    {kExprF32Div, -Float32::quiet_nan(), -Float32::FromBits(0)},
    // -inf / inf
    {kExprF32Div, -Float32::infinity(), Float32::infinity()},
};

V8_FUZZ_TEST_F(CrossCompilerDeterminismTest, TestFloat32Binop)
    .WithDomains(
        // opcode
        // TODO(431593798): Extend to more opcodes.
        fuzztest::ElementOf<WasmOpcode>({kExprF32Add, kExprF32Sub, kExprF32Mul,
                                         kExprF32Div}),
        // lhs
        ArbitraryFloat32(),
        // rhs
        ArbitraryFloat32())
    .WithSeeds(kFloat32BinopSeeds);

}  // namespace v8::internal::wasm
