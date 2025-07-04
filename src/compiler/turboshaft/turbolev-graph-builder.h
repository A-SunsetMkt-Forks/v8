// Copyright 2023 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_COMPILER_TURBOSHAFT_TURBOLEV_GRAPH_BUILDER_H_
#define V8_COMPILER_TURBOSHAFT_TURBOLEV_GRAPH_BUILDER_H_

#include <optional>

#include "src/compiler/turboshaft/phase.h"
#include "src/zone/zone.h"

namespace v8::internal::compiler::turboshaft {

struct TurbolevGraphBuildingPhase {
  DECL_TURBOSHAFT_PHASE_CONSTANTS(TurbolevGraphBuilding)

  std::optional<BailoutReason> Run(PipelineData* data, Zone* temp_zone,
                                   Linkage* linkage);
};

}  // namespace v8::internal::compiler::turboshaft

#endif  // V8_COMPILER_TURBOSHAFT_TURBOLEV_GRAPH_BUILDER_H_
