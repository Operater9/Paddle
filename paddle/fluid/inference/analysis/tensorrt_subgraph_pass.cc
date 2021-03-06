//   Copyright (c) 2018 PaddlePaddle Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "paddle/fluid/inference/analysis/tensorrt_subgraph_pass.h"
#include "paddle/fluid/inference/analysis/subgraph_splitter.h"

namespace paddle {
namespace inference {
namespace analysis {

TensorRTSubGraphPass::TensorRTSubGraphPass(
    const TensorRTSubGraphPass::NodeInsideSubgraphTeller &teller)
    : node_inside_subgraph_teller_(teller) {}

void TensorRTSubGraphPass::Run(DataFlowGraph *graph) {
  SubGraphFuse(graph, node_inside_subgraph_teller_)();
  VLOG(4) << "debug info "
          << graph->HumanReadableInfo(false /*show_values*/,
                                      true /*show_functions*/);
}

}  // namespace analysis
}  // namespace inference

}  // namespace paddle
