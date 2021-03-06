// Copyright (c) 2018 PaddlePaddle Authors. All Rights Reserved.
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

#include <string>
#include <vector>

#include "paddle/fluid/framework/details/computation_op_handle.h"
#include "paddle/fluid/framework/details/multi_devices_helper.h"
#include "paddle/fluid/framework/details/reference_count_pass.h"

namespace paddle {
namespace framework {
namespace details {

std::unique_ptr<ir::Graph> ReferenceCountPass::ApplyImpl(
    std::unique_ptr<ir::Graph> graph) const {
  auto &ref_cnts = Get<DeviceReferenceCountMap>(kGlobalReferenceCount);
  auto &cur_ref_cnts = Get<AtomicDeviceReferenceCountMap>(kCurReferenceCount);
  auto &gcs = Get<DeviceGarbageCollectorMap>(kGarbageCollector);

  // It is not easy to find the right reference counts of varaibles in graph
  // Step 1: Find all variables in computation ops
  // Step 2: Find all variables in non-computation ops which refers to variables
  // in computation ops
  std::unordered_set<std::string> names;
  auto get_ref_cnts_from_compute_op = [&](
      const std::unique_ptr<OpHandleBase> &op,
      const std::vector<VarHandleBase *> &vars) {
    std::vector<std::string> var_names_in_op;
    auto *compute_op = dynamic_cast<ComputationOpHandle *>(op.get());
    if (compute_op == nullptr ||
        !platform::is_gpu_place(compute_op->GetPlace()))
      return var_names_in_op;
    auto place = boost::get<platform::CUDAPlace>(compute_op->GetPlace());
    for (VarHandleBase *var_handle_base : vars) {
      auto *var_handle = dynamic_cast<VarHandle *>(var_handle_base);
      if (var_handle == nullptr || !var_handle->Node()->IsVar()) continue;

      if (!platform::is_gpu_place(var_handle->place_) ||
          boost::get<platform::CUDAPlace>(var_handle->place_) != place)
        continue;

      VarDesc *var_desc = var_handle->Node()->Var();
      auto var_name = var_handle->Node()->Name();

      // This is wierd but there is really some variables without var_desc
      // in computation_op
      if (var_desc == nullptr) {
        if (compute_op->Node()->Op()->Block()->FindVar(var_name) == nullptr)
          continue;
      } else {
        if (var_desc->Persistable() ||
            var_desc->Proto()->type().type() != proto::VarType::LOD_TENSOR)
          continue;
      }

      // compute op only runs in one device
      if (ref_cnts[place.device]->count(var_name))
        ++(*ref_cnts[place.device])[var_name];
      else
        (*ref_cnts[place.device])[var_name] = 1;

      names.insert(var_name);
      var_names_in_op.push_back(var_name);
    }
    return var_names_in_op;
  };

  auto update_ref_cnts_from_non_compute_op = [&](
      const std::unique_ptr<OpHandleBase> &op,
      const std::vector<VarHandleBase *> &vars) {
    if (dynamic_cast<ComputationOpHandle *>(op.get()) != nullptr) return;
    for (VarHandleBase *var_handle_base : vars) {
      auto *var_handle = dynamic_cast<VarHandle *>(var_handle_base);
      if (var_handle == nullptr || !var_handle->Node()->IsVar()) continue;

      auto var_name = var_handle->Node()->Name();
      auto var_place = var_handle->place_;
      if (!platform::is_gpu_place(var_place)) continue;
      auto place = boost::get<platform::CUDAPlace>(var_place);
      if (names.count(var_name) == 0) continue;
      if (ref_cnts.count(place.device) &&
          ref_cnts[place.device]->count(var_name)) {
        ++(*ref_cnts[place.device])[var_name];
      }
    }
  };

  std::unordered_map<OpHandleBase *, ReferenceCountOpHandle *>
      compute_ref_cnt_map;
  auto &all_ops = graph->Get<GraphOps>(kGraphOps);
  for (auto &op : all_ops) {
    auto in_var_names = get_ref_cnts_from_compute_op(op, op->Inputs());
    auto out_var_names = get_ref_cnts_from_compute_op(op, op->Outputs());
    if (in_var_names.empty() && out_var_names.empty()) continue;
    in_var_names.insert(in_var_names.end(), out_var_names.begin(),
                        out_var_names.end());
    auto *compute_op = dynamic_cast<ComputationOpHandle *>(op.get());
    auto place = boost::get<platform::CUDAPlace>(compute_op->GetPlace());
    ir::Node *ref_cnt_node =
        graph->CreateEmptyNode("reference_count", ir::Node::Type::kOperation);
    auto *ref_cnt_handle = new ReferenceCountOpHandle(
        ref_cnt_node, compute_op->GetScope(), place, in_var_names,
        gcs[place.device].get(), cur_ref_cnts[place.device].get());
    auto *dep_var = new DummyVarHandle(graph->CreateControlDepVar());
    compute_op->AddOutput(dep_var);
    ref_cnt_handle->AddInput(dep_var);
    graph->Get<GraphDepVars>(kGraphDepVars).emplace(dep_var);
    compute_ref_cnt_map[compute_op] = ref_cnt_handle;
  }

  for (auto &op : all_ops) {
    update_ref_cnts_from_non_compute_op(op, op->Inputs());
    update_ref_cnts_from_non_compute_op(op, op->Outputs());
  }

  std::vector<std::unique_ptr<OpHandleBase>> new_all_ops;
  new_all_ops.reserve(compute_ref_cnt_map.size() + all_ops.size());
  for (auto &op : all_ops) {
    new_all_ops.emplace_back(std::move(op));
    auto it = compute_ref_cnt_map.find(new_all_ops.back().get());
    if (it != compute_ref_cnt_map.end()) {
      new_all_ops.emplace_back(it->second);
    }
  }

  all_ops.swap(new_all_ops);
  return graph;
}

}  // namespace details
}  // namespace framework
}  // namespace paddle

REGISTER_PASS(reference_count_pass,
              paddle::framework::details::ReferenceCountPass)
    .RequirePassAttr(paddle::framework::details::kGlobalReferenceCount)
    .RequirePassAttr(paddle::framework::details::kCurReferenceCount)
    .RequirePassAttr(paddle::framework::details::kGarbageCollector);
