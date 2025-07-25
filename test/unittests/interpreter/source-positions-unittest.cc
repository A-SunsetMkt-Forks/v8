// Copyright 2015 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "include/v8-function.h"
#include "src/api/api-inl.h"
#include "src/compiler/pipeline.h"
#include "src/execution/isolate.h"
#include "src/handles/handles.h"
#include "src/init/v8.h"
#include "src/interpreter/bytecode-generator.h"
#include "src/interpreter/interpreter.h"
#include "src/objects/objects-inl.h"
#include "test/unittests/interpreter/source-position-matcher.h"
#include "test/unittests/test-utils.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace v8 {
namespace internal {
namespace interpreter {

// Flags enabling optimizations that change generated bytecode array.
// Format is <command-line flag> <flag name> <bit index>
#define OPTIMIZATION_FLAGS(V)                      \
  V(v8_flags.ignition_reo, kUseReo, 0)             \
  V(v8_flags.ignition_filter_expression_positions, \
    kUseFilterExpressionPositions, 2)

#define DECLARE_BIT(_, Name, BitIndex) static const int Name = 1 << BitIndex;
OPTIMIZATION_FLAGS(DECLARE_BIT)
#undef DECLARE_BIT

struct TestCaseData {
  TestCaseData(const char* const script,
               const char* const declaration_parameters = "",
               const char* const arguments = "")
      : script_(script),
        declaration_parameters_(declaration_parameters),
        arguments_(arguments) {}

  const char* script() const { return script_; }
  const char* declaration_parameters() const { return declaration_parameters_; }
  const char* arguments() const { return arguments_; }

 private:
  TestCaseData() = delete;

  const char* const script_;
  const char* const declaration_parameters_;
  const char* const arguments_;
};

static const TestCaseData kTestCaseData[] = {
    {"var x = (y = 3) + (x = y); return x + y;"},
    {"var x = 55;\n"
     "var y = x + (x = 1) + (x = 2) + (x = 3);\n"
     "return y;"},
    {"var x = 10; return x >>> 3;\n"},
    {"var x = 0; return x || (1, 2, 3);\n"},
    {"return a || (a, b, a, b, c = 5, 3);\n"},
    {"var a = 3; var b = 4; a = b; b = a; a = b; return a;\n"},
    {"var a = 1; return [[a, 2], [a + 2]];\n"},
    {"var a = 1; if (a || a < 0) { return 1; }\n"},
    {"var b;"
     "b = a.name;"
     "b = a.name;"
     "a.name = a;"
     "b = a.name;"
     "a.name = a;"
     "return b;"},
    {"var sum = 0;\n"
     "outer: {\n"
     "  for (var x = 0; x < 10; ++x) {\n"
     "    for (var y = 0; y < 3; ++y) {\n"
     "      ++sum;\n"
     "      if (x + y == 12) { break outer; }\n"
     "    }\n"
     "  }\n"
     "}\n"
     "return sum;\n"},
    {"var a = 1;"
     "switch (a) {"
     "  case 1: return a * a + 1;"
     "  case 1: break;"
     "  case 2: return (a = 3) * a + (a = 4);"
     "  case 3:"
     "}"
     "return a;"},
    {"for (var p of [0, 1, 2]) {}"},
    {"var x = { 'a': 1, 'b': 2 };"
     "for (x['a'] of [1,2,3]) { return x['a']; }"},
    {"while (x == 4) {\n"
     "  var y = x + 1;\n"
     "  if (y == 2) break;\n"
     "  for (z['a'] of [0]) {\n"
     "    x += (x *= 3) + y;"
     "  }\n"
     "}\n"},
    {"function g(a, b) { return a.func(b + b, b); }\n"
     "g(new (function Obj() { this.func = function() { return; }})(), 1)\n"},
    {"return some_global[name];", "name", "'a'"}};

class SourcePositionTest : public TestWithContext,
                           public ::testing::WithParamInterface<

                               std::tuple<int, TestCaseData>> {
 public:
  static void SetUpTestSuite() {
    v8_flags.enable_lazy_source_positions = false;
    TestWithContext::SetUpTestSuite();
  }
  bool SourcePositionsMatch(int optimization_bitmap, const char* function_body,
                            const char* function_decl_params,
                            const char* function_args);

 private:
  Handle<BytecodeArray> MakeBytecode(int optimization_bitmap,
                                     const char* function_body,
                                     const char* function_decl_params,
                                     const char* function_args);
  static std::string MakeScript(const char* function_body,
                                const char* function_decl_params,
                                const char* function_args);
  void SetOptimizationFlags(int optimization_bitmap);
};

// static
std::string SourcePositionTest::MakeScript(const char* function_body,
                                           const char* function_decl_params,
                                           const char* function_args) {
  std::ostringstream os;
  os << "function test_function"
     << "(" << function_decl_params << ") {";
  os << function_body;
  os << "}";
  os << "test_function(" << function_args << ");";
  return os.str();
}

Handle<BytecodeArray> SourcePositionTest::MakeBytecode(
    int optimization_bitmap, const char* function_body,
    const char* function_decl_params, const char* function_args) {
  std::string source =
      MakeScript(function_body, function_decl_params, function_args);
  SetOptimizationFlags(optimization_bitmap);
  Local<v8::Script> script =
      v8::Script::Compile(
          context(),
          v8::String::NewFromUtf8(isolate(), source.c_str()).ToLocalChecked())
          .ToLocalChecked();
  USE(script->Run(context()));

  Local<Function> api_function =
      context()
          ->Global()
          ->Get(context(), v8::String::NewFromUtf8(isolate(), "test_function")
                               .ToLocalChecked())

          .ToLocalChecked()
          .As<Function>();
  DirectHandle<JSFunction> function =
      Cast<JSFunction>(v8::Utils::OpenDirectHandle(*api_function));
  return handle(function->shared()->GetBytecodeArray(i_isolate()), i_isolate());
}

void SourcePositionTest::SetOptimizationFlags(int optimization_bitmap) {
#define SET_FLAG(V8Flag, BitName, _) \
  V8Flag = (optimization_bitmap & BitName) ? true : false;
  OPTIMIZATION_FLAGS(SET_FLAG)
#undef SET_FLAG
}

bool SourcePositionTest::SourcePositionsMatch(int optimization_bitmap,
                                              const char* function_body,
                                              const char* function_decl_params,
                                              const char* function_args) {
  Handle<BytecodeArray> unoptimized_bytecode =
      MakeBytecode(0, function_body, function_decl_params, function_args);
  Handle<BytecodeArray> optimized_bytecode = MakeBytecode(
      optimization_bitmap, function_body, function_decl_params, function_args);
  SourcePositionMatcher matcher;
  if (!matcher.Match(unoptimized_bytecode, optimized_bytecode)) {
    return false;
  }
  return true;
}

TEST_P(SourcePositionTest, SourcePositionsEquivalent) {
  // int optimization_bitmap
  auto [optimization_bitmap, test_case_data] = GetParam();
  CHECK(SourcePositionsMatch(optimization_bitmap, test_case_data.script(),
                             test_case_data.declaration_parameters(),
                             test_case_data.arguments()));
}

INSTANTIATE_TEST_SUITE_P(
    SourcePositionsEquivalentTestCases, SourcePositionTest,
    ::testing::Combine(::testing::Values(kUseReo, kUseFilterExpressionPositions,
                                         kUseReo |
                                             kUseFilterExpressionPositions),
                       ::testing::ValuesIn(kTestCaseData)));

}  // namespace interpreter
}  // namespace internal
}  // namespace v8
