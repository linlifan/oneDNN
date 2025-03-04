/*******************************************************************************
* Copyright 2023 Intel Corporation
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*******************************************************************************/

#include "graph_bridge.hpp"
#include "graph_memory.hpp"

namespace {

bool use_graph_lt_shape_for_mem(
        const graph::deserialized_op &base_op_ref, int arg) {
    const auto &op_kind = base_op_ref.kind_;

    if (base_op_ref.has_NXC_format() && graph::is_nxc_lt_arg(op_kind, arg))
        return true;

    bool t_a = false, t_b = false;
    bool has_t_a = base_op_ref.get_attr_bool(t_a, "transpose_a");
    bool has_t_b = base_op_ref.get_attr_bool(t_b, "transpose_b");
    if ((has_t_a && t_a && arg == DNNL_ARG_SRC)
            || (has_t_b && t_b && arg == DNNL_ARG_WEIGHTS))
        return true;

    int64_t groups;
    bool has_group = base_op_ref.get_attr_s64(groups, "groups");
    std::string f_fmt;
    bool has_w_format = base_op_ref.get_attr_string(f_fmt, "weights_format");
    if (op_kind == "Convolution" || op_kind == "ConvolutionBackwardData"
            || op_kind == "ConvTranspose"
            || op_kind == "ConvTransposeBackwardData") {
        if (((has_group && groups > 1) || (has_w_format && f_fmt != "OIX"))
                && arg == DNNL_ARG_WEIGHTS)
            return true;
    }

    if (op_kind == "StaticReshape") { return true; }

    return false;
}

} // namespace

namespace graph {

const std::unordered_set<std::string> &get_special_backward_op_kind_set() {
    static const std::unordered_set<std::string> set_ = {
            // bnorm backward
            "BatchNormTrainingBackward",
            // eltwise backward
            "AbsBackward",
            "ClampBackward",
            "EluBackward",
            "GELUBackward",
            "HardSigmoidBackward",
            "HardSwishBackward",
            "MishBackward",
            "ReLUBackward",
            "SigmoidBackward",
            "SoftPlusBackward",
            "SqrtBackward",
            "TanhBackward",
            // pool backward
            "AvgPoolBackward",
            "MaxPoolBackward",
    };
    return set_;
}

const std::unordered_map<size_t, data_kind_t> &get_dnnl_arg_2_data_kind_map() {
    static const std::unordered_map<size_t, data_kind_t> map_ {
            {DNNL_ARG_SRC, SRC},
            {DNNL_ARG_WEIGHTS_0, WEI},
            {DNNL_ARG_DIFF_WEIGHTS_0, WEI},
            {DNNL_ARG_BIAS, BIA},
            {DNNL_ARG_DIFF_BIAS, BIA},
            {DNNL_ARG_DST, DST},
            {DNNL_ARG_DIFF_SRC_0, DST},
            {DNNL_ARG_SRC_1, SRC_1},
            {DNNL_ARG_MEAN, MEAN},
            {DNNL_ARG_VARIANCE, VAR},
            {DNNL_ARG_SCALE, SC},
            {DNNL_ARG_DIFF_SCALE, SC},
            {DNNL_ARG_SHIFT, SH},
            {DNNL_ARG_DIFF_SHIFT, SH},
    };
    return map_;
}

int init_graph_memory_args(const dnn_mem_map_t &mems,
        partition_mem_map_t &graph_mem_map,
        const std::vector<size_t> &partition_in_ids,
        const std::vector<size_t> &partition_out_ids,
        const deserialized_op &base_op_ref, res_t *res) {

    for (size_t in_idx = 0; in_idx < base_op_ref.in_lts_.size(); ++in_idx) {
        int in_arg = get_prim_arg_name_from_graph_op_input_offset(
                opstr2kind(base_op_ref.kind_), static_cast<int>(in_idx),
                eltwise::get_flag_use_dst_for_bwd_compute(base_op_ref));
        if (in_arg == -1) return res->state = FAILED, FAIL;

        const auto iter = mems.find(in_arg);
        if (iter == mems.end()) {
            BENCHDNN_PRINT(
                    0, "Fail: cannot find primitive memory for arg %d", in_arg);
            return res->state = FAILED, FAIL;
        }
        const auto &mem = iter->second;
        const auto &in_lt = base_op_ref.in_lts_[in_idx];
        bool is_par_input = std::find(partition_in_ids.begin(),
                                    partition_in_ids.end(), in_lt.id_)
                != partition_in_ids.end();

        if (is_par_input) {
            bool should_use_graph_shape
                    = use_graph_lt_shape_for_mem(base_op_ref, in_arg);
            graph_mem_map.emplace(in_lt.id_,
                    dnn_graph_mem_t(mem, in_lt, should_use_graph_shape,
                            /* is_op_input = */ true));
        }
    }

    for (size_t out_idx = 0; out_idx < base_op_ref.out_lts_.size(); ++out_idx) {
        int out_arg = get_prim_arg_name_from_graph_op_output_offset(
                opstr2kind(base_op_ref.kind_), out_idx);
        if (out_arg == -1) return res->state = FAILED, FAIL;

        const auto &out_lt = base_op_ref.out_lts_[out_idx];
        bool is_par_output = std::find(partition_out_ids.begin(),
                                     partition_out_ids.end(), out_lt.id_)
                != partition_out_ids.end();

        if (is_par_output && out_arg != 0) {
            const auto iter = mems.find(out_arg);
            if (iter == mems.end()) {
                BENCHDNN_PRINT(0,
                        "Fail: cannot find primitive memory for arg %d",
                        out_arg);
                return res->state = FAILED, FAIL;
            }
            const auto &mem = iter->second;
            bool should_use_graph_shape
                    = use_graph_lt_shape_for_mem(base_op_ref, out_arg);
            graph_mem_map.emplace(out_lt.id_,
                    dnn_graph_mem_t(mem, out_lt, should_use_graph_shape,
                            /* is op input */ false));

        } else if (is_par_output && out_arg == 0) {
            graph_mem_map.emplace(out_lt.id_,
                    dnn_graph_mem_t({}, out_lt,
                            /* should use graph shape= */ false,
                            /* is op input */ false,
                            /* is fake output */ true));
        }
    }

    return OK;
}

} // namespace graph
