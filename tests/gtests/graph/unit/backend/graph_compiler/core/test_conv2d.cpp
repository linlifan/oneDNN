/*******************************************************************************
 * Copyright 2021-2023 Intel Corporation
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
#include <iostream>
#include "context.hpp"
#include "reference/conv_ref.hpp"
#include "reference/eltwise_ref.hpp"
#include "gtest/gtest.h"
#include <compiler/ir/graph/driver.hpp>
#include <compiler/ir/graph/fusible_op.hpp>
#include <compiler/ir/graph/lowering.hpp>
#include <compiler/ir/graph/pass/pass.hpp>
#include <compiler/ir/graph/transform/transform.hpp>
#include <compiler/jit/jit.hpp>
#include <ops/convolution.hpp>
#include <ops/templates/conv1x1_backprop_data.hpp>
#include <ops/templates/conv1x1_backprop_weight.hpp>
#include <ops/templates/conv_bwd.hpp>
#include <ops/templates/conv_fwd.hpp>
#include <ops/templates/nested_conv_fwd.hpp>
#include <runtime/dynamic_dispatch/dynamic_tensor.hpp>
#include <util/any_map.hpp>
#include <util/reflection.hpp>
#include <util/utils.hpp>
using namespace dnnl::impl::graph::gc;

using conv_fwd_config_t = ops::conv_fwd_config_t;
using nested_conv_fwd_config_t = ops::nested_conv_fwd_config_t;
using conv_bwd_data_config_t = ops::conv_bwd_data_config_t;

const conv_fwd_config_t cfg_fwd = {
        64, // K_block
        32, // C_block
        1, // tile_d
        7, // tile_p
        14, // tile_q
        14, // tile_os
        0, // pack_input
        1 // loop_sched
};

const conv_fwd_config_t cfg_fwd_3x3 = {
        64, // K_block
        32, // C_block
        1, // tile_d
        1, // tile_p
        1, // tile_q
        1, // tile_os
        0, // pack_input
        1 // loop_sched
};

bool verbose = false;

void check_conv_correctness_and_tuning_fwd(conv_fwd_config_t cfg, int N, int K,
        int C, int H, int W, int R, int S, sc_dims stride, sc_dims pads_begin,
        sc_dims pads_end, sc_dims dilation, bool fuse_bias = false,
        bool fuse_bn_relu = false, bool fuse_eleadd = false,
        bool default_cfg = false, bool force_blocking = false,
        bool force_channel_last = false) {
    REQUIRE_AVX2();
    int stride_h = stride[0], stride_w = stride[0];
    if (stride.size() > 1) { stride_w = stride[1]; }
    int padding_h = pads_begin[0], padding_w = pads_begin[0];
    if (pads_begin.size() > 1) { padding_w = pads_begin[1]; }
    int dilation_h = dilation[0], dilation_w = dilation[0];
    if (dilation.size() > 1) { dilation_w = dilation[1]; }

    sc_graph_t mgr;
    std::vector<sc_op_ptr> fuse_arg_ops;

    auto in_a = mgr.make_input({graph_tensor::make({N, C, H, W})});
    auto in_weight = mgr.make_input({graph_tensor::make({K, C, R, S})});
    auto conv_out = mgr.make("conv_fwd_core",
            {in_a->get_outputs()[0], in_weight->get_outputs()[0]}, {},
            {{"strides", stride}, {"pads_begin", pads_begin},
                    {"pads_end", pads_end}, {"dilations", dilation}});
    COMPILE_ASSERT(!force_blocking || !force_channel_last,
            "only one of force_blocking and force_channel_last allowed");
    if (force_blocking) {
        conv_out->attrs_.set<std::string>("temp.test_format", "NCHWc");
    } else if (force_channel_last) {
        conv_out->attrs_.set<std::string>("temp.test_format", "NHWC");
    }
    auto tunop = conv_out->dyn_cast<tunable_op_t>();
    int D = 0, P = 0, Q = 0;
    {
        auto gen = tunop->create_generator();
        auto conv_gen = (ops::gen_conv_fwd_t *)gen.get();
        std::tie(D, P, Q) = conv_gen->get_output_shape();
        reflection::shared_general_object_t cfgptr;
        if (!default_cfg) {
            cfgptr = reflection::general_object_t::make(cfg);
        } else {
            cfgptr = gen->get_default_config(get_test_ctx());
            cfg = *(conv_fwd_config_t *)cfgptr.get();
        }
        tunop->set_config(cfgptr);
        auto pcfg = (conv_fwd_config_t *)cfgptr.get();
        tunop->get_inputs()[0]->details_.set_format(
                sc_data_format_t::NCHWc(pcfg->C_block));
        tunop->get_inputs()[1]->details_.set_format(
                sc_data_format_t::KCRSck(pcfg->C_block, pcfg->K_block));
        tunop->get_outputs()[0]->details_.set_format(
                sc_data_format_t::NCHWc(pcfg->K_block));
    }
    fuse_arg_ops = {in_a, in_weight};
    sc_op_ptr final_out = conv_out;
    auto bc_axis = std::vector<int> {1};
    if (fuse_bias) {
        auto bias_in = mgr.make_input({graph_tensor::make({K})});
        final_out = mgr.make("add",
                {final_out->get_outputs()[0], bias_in->get_outputs()[0]}, {},
                {{"bc_axis", bc_axis}});
        fuse_arg_ops.emplace_back(bias_in);
    }
    if (fuse_bn_relu) {
        auto fbn_mul = mgr.make_input({graph_tensor::make({K})});
        auto fbn_add = mgr.make_input({graph_tensor::make({K})});
        final_out = mgr.make("mul",
                {final_out->get_outputs()[0], fbn_mul->get_outputs()[0]}, {},
                {{"bc_axis", bc_axis}});
        final_out = mgr.make("add",
                {final_out->get_outputs()[0], fbn_add->get_outputs()[0]}, {},
                {{"bc_axis", bc_axis}});
        final_out = mgr.make("relu", {final_out->get_outputs()[0]}, {}, {});
        fuse_arg_ops.emplace_back(fbn_mul);
        fuse_arg_ops.emplace_back(fbn_add);
    }
    sc_op_ptr ele_add_in;
    if (fuse_eleadd) {
        ele_add_in = mgr.make_input({std::make_shared<graph_tensor>(
                nullptr, conv_out->get_outputs()[0]->details_)});
        final_out = mgr.make("add",
                {final_out->get_outputs()[0], ele_add_in->get_outputs()[0]}, {},
                {});
        fuse_arg_ops.emplace_back(ele_add_in);
    }
    auto out = mgr.make_output(final_out->get_outputs());
    fuse_arg_ops.insert(fuse_arg_ops.begin(), out);

    mgr.attrs_.set(sc_graph_t::attr_key_t::is_input_plain, false);
    mgr.attrs_.set(sc_graph_t::attr_key_t::is_output_plain, false);

    graph_driver(mgr, get_test_ctx());
    auto f = lower_graph(get_test_ctx(), mgr, fuse_arg_ops);
    if (verbose) { std::cout << f; }

    auto fptr = jit_engine_t::make(get_test_ctx())->get_entry_func(f, true);
    auto output = alloc_array<float>(
            N * K / cfg.K_block * P * Q * cfg.K_block, INIT_NOOP);
    auto input = alloc_array<float>(N * C / cfg.C_block * H * W * cfg.C_block);
    auto weight = alloc_array<float>(K / cfg.K_block * C / cfg.C_block * R * S
            * cfg.C_block * cfg.K_block);
    auto ele_add
            = alloc_array<float>(N * K / cfg.K_block * P * Q * cfg.K_block);
    auto bias = alloc_array<float>(K);
    auto bn_mul = alloc_array<float>(K);
    auto bn_add = alloc_array<float>(K);

    // save original ele_add in advance, to avoid overwrite in fuse_eleadd
    // condition
    auto mkldnn_ele_add
            = NCHWc2NCHW(ele_add, N, K / cfg.K_block, P, Q, cfg.K_block);
    std::vector<float *> sc_args = {&output[0], &input[0], &weight[0]};

    if (fuse_bias) sc_args.emplace_back(&bias[0]);
    if (fuse_bn_relu) {
        sc_args.emplace_back(&bn_mul[0]);
        sc_args.emplace_back(&bn_add[0]);
    }
    if (fuse_eleadd) {
        // TODO(xxx): use in-place: just let output arg point to eleadd
        sc_args.emplace_back(&ele_add[0]);
    }
    std::vector<generic_val> generic_args;
    for (unsigned i = 0; i < sc_args.size(); i++)
        generic_args.emplace_back(sc_args.at(i));
    fptr->call_generic_default(generic_args.data());
    auto output_format = out->get_inputs().at(0)->details_.get_format();
    test_buffer<float> sc_output
            = any2NCHW(output_format, output, N, K, P, Q, cfg.K_block);

    auto mkldnn_input
            = NCHWc2NCHW(input, N, C / cfg.C_block, H, W, cfg.C_block);
    auto mkldnn_weight = KCRSck2KCRS(weight, K / cfg.K_block, C / cfg.C_block,
            R, S, cfg.C_block, cfg.K_block);

    auto mkldnn_bias = std::move(bias);
    auto mkldnn_mul = std::move(bn_mul);
    auto mkldnn_add = std::move(bn_add);
    test_buffer<float> mkldnn_output(N * K * P * Q);

    compute_ref_direct_fwd(N, 1, K, C, H, W, P, Q, R, S, stride_h, stride_w,
            padding_h, padding_w, &mkldnn_input[0], &mkldnn_weight[0],
            &mkldnn_bias[0], &mkldnn_output[0],
            fuse_bias ? dir_t::FWD_B : FWD_I, &mkldnn_mul[0], &mkldnn_add[0],
            fuse_bn_relu, 1, 1, 1, 0, 1, 1, dilation_h, dilation_w);
    if (fuse_eleadd)
        compute_elementwise_ref_direct_fwd(
                &mkldnn_output[0], &mkldnn_ele_add[0], {N, K, P, Q});
    bool correctness = equal(sc_output, mkldnn_output, 1e-3f);
    if (!correctness) {
        std::cout << "Check correctness FAIL." << std::endl;
        print_output(sc_output, mkldnn_output, 100);
        check_sum(sc_output, mkldnn_output);
    }
    EXPECT_TRUE(correctness);
}

void check_conv_correctness_and_tuning_fwd(conv_fwd_config_t cfg, int N, int K,
        int C, int H, int W, int R, int S, sc_dims stride, sc_dims padding,
        sc_dims dilation, bool fuse_bias = false, bool fuse_bn_relu = false,
        bool fuse_eleadd = false, bool default_cfg = false,
        bool force_blocking = false, bool force_channel_last = false) {
    check_conv_correctness_and_tuning_fwd(cfg, N, K, C, H, W, R, S, stride,
            padding, padding, {1, 1}, fuse_bias, fuse_bn_relu, fuse_eleadd,
            default_cfg, force_blocking, force_channel_last);
}

void check_conv_correctness_and_tuning_fwd(conv_fwd_config_t cfg, int N, int K,
        int C, int H, int W, int R, int S, sc_dims stride, sc_dims padding,
        bool fuse_bias = false, bool fuse_bn_relu = false,
        bool fuse_eleadd = false, bool default_cfg = false,
        bool force_blocking = false, bool force_channel_last = false) {
    check_conv_correctness_and_tuning_fwd(cfg, N, K, C, H, W, R, S, stride,
            padding, {1, 1}, fuse_bias, fuse_bn_relu, fuse_eleadd, default_cfg,
            force_blocking, force_channel_last);
}

void check_conv_correctness_and_tuning_fwd(conv_fwd_config_t cfg, int N, int K,
        int C, int H, int W, int R, int S, int stride, int padding,
        bool fuse_bias = false, bool fuse_bn_relu = false,
        bool fuse_eleadd = false, bool default_cfg = false,
        bool force_blocking = false, bool force_channel_last = false) {
    check_conv_correctness_and_tuning_fwd(cfg, N, K, C, H, W, R, S,
            {stride, stride}, {padding, padding}, fuse_bias, fuse_bn_relu,
            fuse_eleadd, default_cfg, force_blocking, force_channel_last);
}

void check_conv_correctness_and_tuning_bwd_d(int N, int K, int C, int H, int W,
        int R, int S, int stride, int padding,
        bool use_inverse_filter = false) {
    REQUIRE_AVX2();
    sc_graph_t mgr;
    std::vector<sc_op_ptr> fuse_arg_ops;
    sc_dims stride_arr = {stride, stride};
    sc_dims padding_arr = {padding, padding};
    int P = (H + 2 * padding - R) / stride + 1;
    int Q = (W + 2 * padding - S) / stride + 1;
    auto in_a = mgr.make_input({graph_tensor::make({N, K, P, Q})});
    auto in_weight = mgr.make_input({graph_tensor::make({K, C, R, S})});
    sc_op_ptr conv_out;
    if (use_inverse_filter) {
        auto permute_channel
                = mgr.make("transpose", {in_weight->get_outputs()[0]}, {},
                        {{"order", std::vector<int> {1, 0, 2, 3}}});
        conv_out = mgr.make("conv_fwd_core",
                {in_a->get_outputs()[0], permute_channel->get_outputs()[0]}, {},
                {{"strides", stride_arr}, {"paddings", padding_arr},
                        {"dst_shape", sc_dims {N, C, H, W}},
                        {"inverse_filter", true}});
    } else {
        conv_out = mgr.make("conv_bwd_data_core",
                {in_a->get_outputs()[0], in_weight->get_outputs()[0]},
                {graph_tensor::make({N, C, H, W})},
                {{"strides", stride_arr}, {"paddings", padding_arr},
                        {"dst_shape", sc_dims {N, C, H, W}}});
    }

    fuse_arg_ops = {in_a, in_weight};
    const sc_op_ptr &final_out = conv_out;
    auto out = mgr.make_output(final_out->get_outputs());
    fuse_arg_ops.insert(fuse_arg_ops.begin(), out);

    graph_driver(mgr, get_test_ctx());

    auto f = lower_graph(get_test_ctx(), mgr, fuse_arg_ops);

    auto fptr = jit_engine_t::make(get_test_ctx())->get_entry_func(f, true);
    auto grad = alloc_array<float>(N * P * Q * K);
    auto grad_data = alloc_array<float>(N * H * W * C);
    auto weight = alloc_array<float>(K * C * R * S);
    test_buffer<float> bias(K);
    bias.zeroout();

    std::vector<float *> sc_args = {&grad_data[0], &grad[0], &weight[0]};
    std::vector<generic_val> generic_args;
    for (unsigned i = 0; i < sc_args.size(); i++)
        generic_args.emplace_back(sc_args.at(i));
    fptr->call_generic_default(generic_args.data());

    auto mkldnn_grad = std::move(grad);
    auto mkldnn_weight = std::move(weight);
    auto mkldnn_bias = std::move(bias);
    test_buffer<float> mkldnn_grad_data(N * C * H * W);
    compute_ref_direct_bwd_d(N, 1, K, C, H, W, P, Q, R, S, stride, stride,
            padding, padding, &mkldnn_grad_data[0], &mkldnn_weight[0],
            &mkldnn_bias[0], &mkldnn_grad[0]);
    test_utils::compare_data(grad_data, mkldnn_grad_data, 1e-3f, 1e-3f);
}

void check_conv_correctness_and_tuning_bwd_w(int N, int K, int C, int H, int W,
        int R, int S, int stride, int padding,
        sc_data_type_t dtype = datatypes::f32) {
    REQUIRE_AVX2();
    sc_graph_t mgr;
    std::vector<sc_op_ptr> fuse_arg_ops;
    sc_dims stride_arr = {stride, stride};
    sc_dims padding_arr = {padding, padding};
    int P = (H + 2 * padding - R) / stride + 1;
    int Q = (W + 2 * padding - S) / stride + 1;
    auto in_data = mgr.make_input({graph_tensor::make({N, C, H, W})});
    auto in_diff_dst = mgr.make_input({graph_tensor::make({N, K, P, Q})});
    auto conv_in_data = in_data, conv_in_diff_dst = in_diff_dst;
    if (dtype == datatypes::bf16) {
        auto cast_data = mgr.make("cast", in_data->get_outputs(), {},
                {{"dtype", datatypes::bf16}});
        auto cast_diff_dst = mgr.make("cast", in_diff_dst->get_outputs(), {},
                {{"dtype", datatypes::bf16}});
        conv_in_data = cast_data;
        conv_in_diff_dst = cast_diff_dst;
    }
    auto conv_out = mgr.make("conv_bwd_weight_core",
            {conv_in_data->get_outputs()[0],
                    conv_in_diff_dst->get_outputs()[0]},
            {graph_tensor::make({K, C, R, S})},
            {{"strides", stride_arr}, {"paddings", padding_arr},
                    {"weights_shape", sc_dims {K, C, R, S}}});

    fuse_arg_ops = {in_data, in_diff_dst};
    const sc_op_ptr &final_out = conv_out;
    auto out = mgr.make_output(final_out->get_outputs());
    fuse_arg_ops.insert(fuse_arg_ops.begin(), out);

    graph_driver(mgr, get_test_ctx());

    auto f = lower_graph(get_test_ctx(), mgr, fuse_arg_ops);

    auto fptr = jit_engine_t::make(get_test_ctx())->get_entry_func(f, true);
    auto data = alloc_array<float>(N * H * W * C);
    auto grad = alloc_array<float>(N * P * Q * K);
    auto grad_weight = alloc_array<float>(K * C * R * S);

    std::vector<float *> sc_args = {&grad_weight[0], &data[0], &grad[0]};
    std::vector<generic_val> generic_args;
    for (unsigned i = 0; i < sc_args.size(); i++)
        generic_args.emplace_back(sc_args.at(i));
    fptr->call_generic_default(generic_args.data());

    auto mkldnn_grad = std::move(grad);
    auto mkldnn_data = std::move(data);
    test_buffer<float> mkldnn_grad_weight(K * C * R * S);

    compute_ref_bwd_weights(N, 1, K, C, H, W, P, Q, R, S, stride, stride,
            padding, padding, &mkldnn_data[0], &mkldnn_grad_weight[0],
            &mkldnn_grad[0]);

    if (dtype == datatypes::bf16) {
        test_utils::compare_data(grad_weight, mkldnn_grad_weight, 1e-1f, 5e-1f);
    } else {
        test_utils::compare_data(grad_weight, mkldnn_grad_weight, 1e-3f, 5e-3f);
    }
}

void check_conv_correctness_and_tuning_fwd(int N, int K, int C, int H, int W,
        int R, int S, sc_dims stride, sc_dims padding, bool fuse_bias = false,
        bool fuse_bn_relu = false, bool fuse_eleadd = false, int real_N = -1,
        int real_H = -1, int real_W = -1) {
    int stride_h = stride[0], stride_w = stride[0];
    if (stride.size() > 1) { stride_w = stride[1]; }
    int padding_h = padding[0], padding_w = padding[0];
    if (padding.size() > 1) { padding_w = padding[1]; }
    bool is_dynamic = N < 0 || H < 0 || W < 0;

    sc_graph_t mgr;
    std::vector<sc_op_ptr> fuse_arg_ops;
    auto in_a = mgr.make_input({graph_tensor::make({N, C, H, W})});
    auto in_weight = mgr.make_input({graph_tensor::make({K, C, R, S})});
    auto conv_out = mgr.make("conv_fwd_core",
            {in_a->get_outputs()[0], in_weight->get_outputs()[0]}, {},
            {{"strides", stride}, {"paddings", padding}, {"no_fuse", false}});

    conv_out->attrs_.set<std::string>("temp.test_format", "NHWC");
    auto tunop = conv_out->dyn_cast<tunable_op_t>();
    int D = 0, P = 0, Q = 0;

    auto gen = tunop->create_generator();
    auto conv_gen = (ops::gen_nested_conv_fwd_t *)gen.get();
    std::tie(D, P, Q) = conv_gen->get_output_shape();
    reflection::shared_general_object_t cfgptr;
    cfgptr = gen->get_default_config(get_test_ctx());
    nested_conv_fwd_config_t cfg = *(nested_conv_fwd_config_t *)cfgptr.get();
    tunop->set_config(cfgptr);
    tunop->get_inputs()[0]->details_.set_format(sc_data_format_t::NHWC());
    tunop->get_inputs()[1]->details_.set_format(
            sc_data_format_t::KCRSck(cfg.im_ic_block, cfg.im_oc_block));
    tunop->get_outputs()[0]->details_.set_format(sc_data_format_t::NHWC());

    fuse_arg_ops = {in_a, in_weight};
    sc_op_ptr final_out = conv_out;
    auto bc_axis = std::vector<int> {1};
    if (fuse_bias) {
        auto bias_in = mgr.make_input({graph_tensor::make({K})});
        final_out = mgr.make("add",
                {final_out->get_outputs()[0], bias_in->get_outputs()[0]}, {},
                {{"bc_axis", bc_axis}});
        fuse_arg_ops.emplace_back(bias_in);
    }
    if (fuse_bn_relu) {
        auto fbn_mul = mgr.make_input({graph_tensor::make({K})});
        auto fbn_add = mgr.make_input({graph_tensor::make({K})});
        final_out = mgr.make("mul",
                {final_out->get_outputs()[0], fbn_mul->get_outputs()[0]}, {},
                {{"bc_axis", bc_axis}});
        final_out = mgr.make("add",
                {final_out->get_outputs()[0], fbn_add->get_outputs()[0]}, {},
                {{"bc_axis", bc_axis}});
        final_out = mgr.make("relu", {final_out->get_outputs()[0]}, {}, {});
        fuse_arg_ops.emplace_back(fbn_mul);
        fuse_arg_ops.emplace_back(fbn_add);
    }
    sc_op_ptr ele_add_in;
    if (fuse_eleadd) {
        ele_add_in = mgr.make_input({std::make_shared<graph_tensor>(
                nullptr, conv_out->get_outputs()[0]->details_)});
        final_out = mgr.make("add",
                {final_out->get_outputs()[0], ele_add_in->get_outputs()[0]}, {},
                {});
        fuse_arg_ops.emplace_back(ele_add_in);
    }
    auto out = mgr.make_output(final_out->get_outputs());
    fuse_arg_ops.insert(fuse_arg_ops.begin(), out);

    mgr.attrs_.set(sc_graph_t::attr_key_t::is_input_plain, false);
    mgr.attrs_.set(sc_graph_t::attr_key_t::is_output_plain, false);

    graph_driver(mgr);
    auto f = lower_graph(get_default_context(), mgr, fuse_arg_ops);
    if (verbose) { std::cout << f; }

    auto fptr = jit_engine_t::make(get_default_context())
                        ->get_entry_func(f, true);
    uint8_t in_mask = 0;
    if (is_dynamic) {
        if (is_dynamic_dim(N)) {
            assert(real_N > 0);
            N = real_N;
            in_mask |= 1 << 0;
        }
        if (is_dynamic_dim(H)) {
            assert(real_H > 0);
            H = real_H;
            in_mask |= 1 << 1;
        }
        if (is_dynamic_dim(W)) {
            assert(real_W > 0);
            W = real_W;
            in_mask |= 1 << 2;
        }
        P = (H + padding_h * 2 - R) / stride_h + 1;
        Q = (W + padding_w * 2 - S) / stride_w + 1;
    }

    sc_dims out_dims = sc_dims {N, K, P, Q};
    sc_dims in_a_dims = sc_dims {N, C, H, W};
    sc_dims in_weight_dims = sc_dims {K, C, R, S};
    sc_dims in_postop_dims = sc_dims {K};
    auto output = alloc_array<float>(
            N * K / cfg.im_oc_block * P * Q * cfg.im_oc_block, INIT_NOOP);
    auto input = alloc_array<float>(
            N * C / cfg.im_ic_block * H * W * cfg.im_ic_block);
    auto weight = alloc_array<float>(K / cfg.im_oc_block * C / cfg.im_ic_block
            * R * S * cfg.im_ic_block * cfg.im_oc_block);
    auto ele_add = alloc_array<float>(
            N * K / cfg.im_oc_block * P * Q * cfg.im_oc_block);
    auto bias = alloc_array<float>(K);
    auto bn_mul = alloc_array<float>(K);
    auto bn_add = alloc_array<float>(K);

    // Define dynamic tensor
    runtime::dynamic_tensor_t dyn_output(&output[0], &out_dims[0],
            out_dims.size(), uint32_t(sc_data_etype::F32), 0);
    runtime::dynamic_tensor_t dyn_input(&input[0], &in_a_dims[0],
            in_a_dims.size(), uint32_t(sc_data_etype::F32), in_mask);
    runtime::dynamic_tensor_t dyn_weight(&weight[0], &in_weight_dims[0],
            in_weight_dims.size(), uint32_t(sc_data_etype::F32), 0);
    runtime::dynamic_tensor_t dyn_bias(&bias[0], &in_postop_dims[0],
            in_postop_dims.size(), uint32_t(sc_data_etype::F32), 0);
    runtime::dynamic_tensor_t dyn_bn_mul(&bn_mul[0], &in_postop_dims[0],
            in_postop_dims.size(), uint32_t(sc_data_etype::F32), 0);
    runtime::dynamic_tensor_t dyn_bn_add(&bn_add[0], &in_postop_dims[0],
            in_postop_dims.size(), uint32_t(sc_data_etype::F32), 0);
    runtime::dynamic_tensor_t dyn_ele_add(&ele_add[0], &out_dims[0],
            out_dims.size(), uint32_t(sc_data_etype::F32), 0);

    std::vector<void *> sc_args = is_dynamic
            ? std::vector<void *> {&dyn_output, &dyn_input, &dyn_weight}
            : std::vector<void *> {&output[0], &input[0], &weight[0]};

    if (fuse_bias) {
        if (is_dynamic)
            sc_args.emplace_back(&dyn_bias);
        else
            sc_args.emplace_back(&bias[0]);
    }
    if (fuse_bn_relu) {
        if (is_dynamic) {
            sc_args.emplace_back(&dyn_bn_mul);
            sc_args.emplace_back(&dyn_bn_add);
        } else {
            sc_args.emplace_back(&bn_mul[0]);
            sc_args.emplace_back(&bn_add[0]);
        }
    }
    if (fuse_eleadd) {
        // TODO(xxx): use in-place: just let output arg point to eleadd
        if (is_dynamic)
            sc_args.emplace_back(&dyn_ele_add);
        else
            sc_args.emplace_back(&ele_add[0]);
    }
    std::vector<generic_val> generic_args;
    for (unsigned i = 0; i < sc_args.size(); i++)
        generic_args.emplace_back(sc_args.at(i));
    fptr->call_generic_default(generic_args.data());
    auto output_format = out->get_inputs().at(0)->details_.get_format();
    test_buffer<float> sc_output
            = any2NCHW(output_format, output, N, K, P, Q, cfg.im_oc_block);

    auto in_a_format = in_a->get_outputs()[0]->details_.get_format();
    auto mkldnn_input
            = any2NCHW(in_a_format, input, N, C, H, W, cfg.im_ic_block);
    auto mkldnn_weight = KCRSck2KCRS(weight, K / cfg.im_oc_block,
            C / cfg.im_ic_block, R, S, cfg.im_ic_block, cfg.im_oc_block);
    auto mkldnn_ele_add
            = any2NCHW(output_format, ele_add, N, K, P, Q, cfg.im_oc_block);

    auto mkldnn_bias = std::move(bias);
    auto mkldnn_mul = std::move(bn_mul);
    auto mkldnn_add = std::move(bn_add);

    test_buffer<float> mkldnn_output(N * K * P * Q);

    compute_ref_direct_fwd(N, 1, K, C, H, W, P, Q, R, S, stride_h, stride_w,
            padding_h, padding_w, &mkldnn_input[0], &mkldnn_weight[0],
            &mkldnn_bias[0], &mkldnn_output[0],
            fuse_bias ? dir_t::FWD_B : FWD_I, &mkldnn_mul[0], &mkldnn_add[0],
            fuse_bn_relu);
    if (fuse_eleadd)
        compute_elementwise_ref_direct_fwd(
                &mkldnn_output[0], &mkldnn_ele_add[0], {N, K, P, Q});
    bool correctness = equal(sc_output, mkldnn_output, 1e-3f);
    if (!correctness) {
        std::cout << "Check correctness FAIL." << std::endl;
        print_output(sc_output, mkldnn_output, 100);
        check_sum(sc_output, mkldnn_output);
    }
    EXPECT_TRUE(correctness);
}

#define conv_padding_support_NXC 0
TEST(GCCore_CPU_conv1d_fwd_cpp, Test_1DConv_1x1_1_NCX) {
    check_conv_correctness_and_tuning_fwd(conv_fwd_config_t(), 1, 16, 16,
            28 * 28, 1, 1, 1, 1, 0, false, false, false, true, true, false);
}
TEST(GCCore_CPU_conv1d_fwd_cpp, Test_1DConv_1x1_1_NXC) {
    check_conv_correctness_and_tuning_fwd(conv_fwd_config_t(), 7, 63 * 8,
            63 * 2, 64 * 64, 1, 1, 1, 1, 0, false, false, false, true, false,
            true);
}
TEST(GCCore_CPU_conv1d_fwd_cpp, Test_1DConv_1x1_2_NCX) {
    check_conv_correctness_and_tuning_fwd(conv_fwd_config_t(), 1, 16, 16,
            28 * 28, 1, 1, 1, 1, 0, false, false, false, true, true, false);
}
TEST(GCCore_CPU_conv1d_fwd_cpp, Test_1DConv_1x1_2_NXC) {
    check_conv_correctness_and_tuning_fwd(conv_fwd_config_t(), 1, 16, 16,
            28 * 28, 1, 1, 1, 1, 0, false, false, false, true, false, true);
}
TEST(GCCore_CPU_conv1d_fwd_cpp, Test_1DConv_1x1_3_NCX) {
    check_conv_correctness_and_tuning_fwd(conv_fwd_config_t(), 28, 16, 16,
            28 * 28, 1, 1, 1, 1, 0, false, false, false, true, true, false);
}
TEST(GCCore_CPU_conv1d_fwd_cpp, Test_1DConv_1x1_3_NXC) {
    check_conv_correctness_and_tuning_fwd(conv_fwd_config_t(), 28, 16, 16,
            28 * 28, 1, 1, 1, 1, 0, false, false, false, true, false, true);
}
TEST(GCCore_CPU_conv1d_fwd_cpp, Test_1DConv_1x1_4_NCX) {
    check_conv_correctness_and_tuning_fwd(conv_fwd_config_t(), 8, 512, 2048, 7,
            7, 1, 1, 1, 0, false, false, false, true, true, false);
}
TEST(GCCore_CPU_conv1d_fwd_cpp, Test_1DConv_1x1_4_NXC) {
    check_conv_correctness_and_tuning_fwd(conv_fwd_config_t(), 8, 512, 2048,
            7 * 7, 1, 1, 1, 1, 0, false, false, false, true, false, true);
}
TEST(GCCore_CPU_conv2d_fwd_cpp, Test_2DConv_1x1_1_NCX) {
    check_conv_correctness_and_tuning_fwd(conv_fwd_config_t(), 1, 16, 16, 14,
            14, 1, 1, 1, 0, false, false, false, true, true, false);
}
TEST(GCCore_CPU_conv2d_fwd_cpp, Test_2DConv_1x1_1_NXC) {
    check_conv_correctness_and_tuning_fwd(conv_fwd_config_t(), 1, 16, 16, 14,
            14, 1, 1, 1, 0, false, false, false, true, false, true);
}
TEST(GCCore_CPU_conv2d_fwd_cpp, Test_2DConv_1x1_2_NCX) {
    check_conv_correctness_and_tuning_fwd(conv_fwd_config_t(), 1, 16, 16, 28,
            28, 3, 3, 1, 0, false, false, false, true, true, false);
}
TEST(GCCore_CPU_conv2d_fwd_cpp, Test_2DConv_1x1_2_NXC) {
    check_conv_correctness_and_tuning_fwd(conv_fwd_config_t(), 1, 16, 16, 28,
            28, 3, 3, 1, 0, false, false, false, true, false, true);
}
TEST(GCCore_CPU_conv2d_fwd_cpp, Test_2DConv_1x1_3_NCX) {
    check_conv_correctness_and_tuning_fwd(conv_fwd_config_t(), 1, 16, 16, 28,
            28, 3, 3, 2, 3, false, false, false, true, true, false);
}
#if conv_padding_support_NXC
TEST(GCCore_CPU_conv2d_fwd_cpp, Test_2DConv_1x1_3_NXC) {
    check_conv_correctness_and_tuning_fwd(conv_fwd_config_t(), 1, 16, 16, 28,
            28, 3, 3, 2, 3, false, false, false, true, false, true);
}
#endif
TEST(GCCore_CPU_conv2d_fwd_cpp, Test_2DConv_1x1_4_NCX) {
    check_conv_correctness_and_tuning_fwd(conv_fwd_config_t(), 1, 16, 16, 28,
            28, 1, 1, {2, 1}, {0, 0}, false, false, false, true, true, false);
}
TEST(GCCore_CPU_conv2d_fwd_cpp, Test_2DConv_1x1_4_NXC) {
    check_conv_correctness_and_tuning_fwd(conv_fwd_config_t(), 1, 16, 16, 28,
            28, 1, 1, {2, 1}, {0, 0}, false, false, false, true, false, true);
}
TEST(GCCore_CPU_conv2d_fwd_cpp, Test_2DConv_1x1_5_NCX) {
    check_conv_correctness_and_tuning_fwd(conv_fwd_config_t(), 28, 16, 16, 28,
            28, 3, 3, {1, 1}, {2, 1}, true, true, true, true, true, false);
}
#if conv_padding_support_NXC
TEST(GCCore_CPU_conv2d_fwd_cpp, Test_2DConv_1x1_5_NXC) {
    check_conv_correctness_and_tuning_fwd(conv_fwd_config_t(), 28, 16, 16, 28,
            28, 3, 3, {1, 1}, {2, 1}, true, true, true, true, false, true);
}
#endif
#ifdef __AVX512F__
TEST(GCCore_CPU_conv2d_fwd_cpp, Test_2DConv_1x1_6_NCX) {
    check_conv_correctness_and_tuning_fwd(conv_fwd_config_t(), 16, 256, 64, 56,
            56, 1, 1, 1, 0, false, false, false, true, true, false);
}
TEST(GCCore_CPU_conv2d_fwd_cpp, Test_2DConv_1x1_6_NXC) {
    check_conv_correctness_and_tuning_fwd(conv_fwd_config_t(), 16, 256, 64, 56,
            56, 1, 1, 1, 0, false, false, false, true, false, true);
}
TEST(GCCore_CPU_conv2d_fwd_cpp, Test_2DConv_1x1_7_NCX) {
    check_conv_correctness_and_tuning_fwd(conv_fwd_config_t(), 16, 512, 128, 28,
            28, 1, 1, 1, 0, false, false, false, true, true, false);
}
TEST(GCCore_CPU_conv2d_fwd_cpp, Test_2DConv_1x1_7_NXC) {
    check_conv_correctness_and_tuning_fwd(conv_fwd_config_t(), 16, 512, 128, 28,
            28, 1, 1, 1, 0, false, false, false, true, false, true);
}
TEST(GCCore_CPU_conv2d_fwd_cpp, Test_2DConv_1x1_8_NCX) {
    check_conv_correctness_and_tuning_fwd(conv_fwd_config_t(), 16, 64, 64, 56,
            56, 1, 1, 1, 0, true, true, true, true, true, false);
}
TEST(GCCore_CPU_conv2d_fwd_cpp, Test_2DConv_1x1_8_NXC) {
    check_conv_correctness_and_tuning_fwd(conv_fwd_config_t(), 16, 64, 64, 56,
            56, 1, 1, 1, 0, true, true, true, true, false, true);
}
TEST(GCCore_CPU_conv2d_fwd_cpp, Test_2DConv_1x1_9_NCX) {
    check_conv_correctness_and_tuning_fwd(conv_fwd_config_t(), 16, 64, 64, 56,
            56, 1, 1, {1, 1}, {0, 0}, true, true, true, true, true, false);
}
TEST(GCCore_CPU_conv2d_fwd_cpp, Test_2DConv_1x1_9_NXC) {
    check_conv_correctness_and_tuning_fwd(conv_fwd_config_t(), 16, 64, 64, 56,
            56, 1, 1, {1, 1}, {0, 0}, true, true, true, true, false, true);
}
// test asymmetric stride
TEST(GCCore_CPU_conv2d_fwd_cpp, Test_2DConv_1x1_10_NCX) {
    check_conv_correctness_and_tuning_fwd(conv_fwd_config_t(), 16, 64, 64, 56,
            56, 1, 1, {1, 2}, {0, 0}, true, true, true, true, true, false);
}
TEST(GCCore_CPU_conv2d_fwd_cpp, Test_2DConv_1x1_10_NXC) {
    check_conv_correctness_and_tuning_fwd(conv_fwd_config_t(), 16, 64, 64, 56,
            56, 1, 1, {1, 2}, {0, 0}, true, true, true, true, false, true);
}
// test asymmetric padding
TEST(GCCore_CPU_conv2d_fwd_cpp, Test_2DConv_1x1_11_NCX) {
    check_conv_correctness_and_tuning_fwd(conv_fwd_config_t(), 16, 64, 64, 56,
            56, 3, 3, {1, 1}, {1, 2}, true, true, true, true, true, false);
}
#if conv_padding_support_NXC
TEST(GCCore_CPU_conv2d_fwd_cpp, Test_2DConv_1x1_11_NXC) {
    check_conv_correctness_and_tuning_fwd(conv_fwd_config_t(), 16, 64, 64, 56,
            56, 3, 3, {1, 1}, {1, 2}, true, true, true, true, false, true);
}
#endif
TEST(GCCore_CPU_conv2d_fwd_cpp, Test_2DConv_1x1_12_NCX) {
    check_conv_correctness_and_tuning_fwd(conv_fwd_config_t(), 16, 64, 64, 56,
            56, 3, 3, {1, 1}, {1, 0}, true, true, true, true, true, false);
}
#if conv_padding_support_NXC
TEST(GCCore_CPU_conv2d_fwd_cpp, Test_2DConv_1x1_12_NXC) {
    check_conv_correctness_and_tuning_fwd(conv_fwd_config_t(), 16, 64, 64, 56,
            56, 3, 3, {1, 1}, {1, 0}, true, true, true, true, false, true);
}
#endif
TEST(GCCore_CPU_conv2d_fwd_cpp, Test_2DConv_1x1_13_NCX) {
    check_conv_correctness_and_tuning_fwd(conv_fwd_config_t(), 16, 64, 64, 56,
            56, 3, 3, {1, 1}, {0, 1}, true, true, true, true, true, false);
}
#if conv_padding_support_NXC
TEST(GCCore_CPU_conv2d_fwd_cpp, Test_2DConv_1x1_13_NXC) {
    check_conv_correctness_and_tuning_fwd(conv_fwd_config_t(), 16, 64, 64, 56,
            56, 3, 3, {1, 1}, {0, 1}, true, true, true, true, false, true);
}
#endif

// test asymmetric stride & padding
TEST(GCCore_CPU_conv2d_fwd_cpp, Test_2DConv_1x1_14_NCX) {
    check_conv_correctness_and_tuning_fwd(conv_fwd_config_t(), 16, 64, 64, 56,
            56, 3, 3, {1, 2}, {2, 1}, true, true, true, true, true, false);
}
#if conv_padding_support_NXC
TEST(GCCore_CPU_conv2d_fwd_cpp, Test_2DConv_1x1_14_NXC) {
    check_conv_correctness_and_tuning_fwd(conv_fwd_config_t(), 16, 64, 64, 56,
            56, 3, 3, {1, 2}, {2, 1}, true, true, true, true, false, true);
}
#endif
TEST(GCCore_CPU_conv2d_fwd_cpp, Test_2DConv_1x1_15_NCX) {
    check_conv_correctness_and_tuning_fwd(conv_fwd_config_t(), 16, 64, 64, 56,
            56, 3, 3, {1, 2}, {0, 1}, true, true, true, true, true, false);
}
#if conv_padding_support_NXC
TEST(GCCore_CPU_conv2d_fwd_cpp, Test_2DConv_1x1_15_NXC) {
    check_conv_correctness_and_tuning_fwd(conv_fwd_config_t(), 16, 64, 64, 56,
            56, 3, 3, {1, 2}, {0, 1}, true, true, true, true, false, true);
}
#endif

// conv1x1 with given cfg
TEST(GCCore_CPU_conv2d_fwd_cpp, Test_2DConv_1x1_16_NCX) {
    check_conv_correctness_and_tuning_fwd(cfg_fwd, 28, 128, 64, 56, 56, 1, 1, 1,
            0, false, false, false, false, true, false);
}
TEST(GCCore_CPU_conv2d_fwd_cpp, Test_2DConv_1x1_16_NXC) {
    check_conv_correctness_and_tuning_fwd(cfg_fwd, 28, 128, 64, 56, 56, 1, 1, 1,
            0, false, false, false, false, false, true);
}
TEST(GCCore_CPU_conv2d_fwd_cpp, Test_2DConv_1x1_17_NCX) {
    check_conv_correctness_and_tuning_fwd(cfg_fwd, 28, 64, 128, 28, 28, 1, 1, 1,
            0, false, false, false, false, true, false);
}
TEST(GCCore_CPU_conv2d_fwd_cpp, Test_2DConv_1x1_17_NXC) {
    check_conv_correctness_and_tuning_fwd(cfg_fwd, 28, 64, 128, 28, 28, 1, 1, 1,
            0, false, false, false, false, false, true);
}
TEST(GCCore_CPU_conv2d_fwd_cpp, Test_2DConv_1x1_18_NCX) {
    check_conv_correctness_and_tuning_fwd(cfg_fwd, 1, 128, 128, 28, 28, 1, 1, 1,
            0, false, false, false, false, true, false);
}
TEST(GCCore_CPU_conv2d_fwd_cpp, Test_2DConv_1x1_18_NXC) {
    check_conv_correctness_and_tuning_fwd(cfg_fwd, 1, 128, 128, 28, 28, 1, 1, 1,
            0, false, false, false, false, false, true);
}
TEST(GCCore_CPU_conv2d_fwd_cpp, Test_2DConv_1x1_19_NCX) {
    check_conv_correctness_and_tuning_fwd(cfg_fwd, 28, 128, 128, 56, 56, 1, 1,
            2, 0, false, false, false, false, true, false);
}
TEST(GCCore_CPU_conv2d_fwd_cpp, Test_2DConv_1x1_19_NXC) {
    check_conv_correctness_and_tuning_fwd(cfg_fwd, 28, 128, 128, 56, 56, 1, 1,
            2, 0, false, false, false, false, false, true);
}
TEST(GCCore_CPU_conv2d_fwd_cpp, Test_2DConv_1x1_20_NCX) {
    check_conv_correctness_and_tuning_fwd(cfg_fwd, 1, 64, 64, 56, 56, 1, 1, 1,
            0, false, false, false, false, true, false);
}
TEST(GCCore_CPU_conv2d_fwd_cpp, Test_2DConv_1x1_20_NXC) {
    check_conv_correctness_and_tuning_fwd(cfg_fwd, 1, 64, 64, 56, 56, 1, 1, 1,
            0, false, false, false, false, false, true);
}
TEST(GCCore_CPU_conv2d_fwd_cpp, Test_2DConv_1x1_21_NCX) {
    check_conv_correctness_and_tuning_fwd({64, 32, 1, 7, 28, 28, 0, 4}, 28, 128,
            64, 28, 28, 1, 1, 1, 0, false, false, false, false, true, false);
}
TEST(GCCore_CPU_conv2d_fwd_cpp, Test_2DConv_1x1_21_NXC) {
    check_conv_correctness_and_tuning_fwd({64, 32, 1, 7, 28, 28, 0, 4}, 28, 128,
            64, 28, 28, 1, 1, 1, 0, false, false, false, false, false, true);
}
TEST(GCCore_CPU_conv2d_fwd_cpp, Test_2DConv_1x1_22_NCX) {
    check_conv_correctness_and_tuning_fwd({64, 32, 1, 7, 28, 28, 0, 5}, 28, 128,
            64, 28, 28, 1, 1, 1, 0, false, false, false, false, true, false);
}
TEST(GCCore_CPU_conv2d_fwd_cpp, Test_2DConv_1x1_22_NXC) {
    check_conv_correctness_and_tuning_fwd({64, 32, 1, 7, 28, 28, 0, 5}, 28, 128,
            64, 28, 28, 1, 1, 1, 0, false, false, false, false, false, true);
}
#endif

// conv1x1 with bias
TEST(GCCore_CPU_conv2d_fwd_bias_cpp, Test_2DConv_1x1_1_NCX) {
    check_conv_correctness_and_tuning_fwd(cfg_fwd, 28, 128, 64, 56, 56, 1, 1, 1,
            0, true, false, false, false, true, false);
}
TEST(GCCore_CPU_conv2d_fwd_bias_cpp, Test_2DConv_1x1_1_NXC) {
    check_conv_correctness_and_tuning_fwd(cfg_fwd, 28, 128, 64, 56, 56, 1, 1, 1,
            0, true, false, false, false, false, true);
}
TEST(GCCore_CPU_conv2d_fwd_bias_cpp, Test_2DConv_1x1_2_NCX) {
    check_conv_correctness_and_tuning_fwd({3, 1, 1, 4, 4, 4, 1, 1}, 28, 3, 16,
            28, 28, 1, 1, 1, 0, true, false, false, false, true, false);
}
TEST(GCCore_CPU_conv2d_fwd_bias_cpp, Test_2DConv_1x1_2_NXC) {
    check_conv_correctness_and_tuning_fwd({3, 1, 1, 4, 4, 4, 1, 1}, 28, 3, 16,
            28, 28, 1, 1, 1, 0, true, false, false, false, false, true);
}

#ifdef __AVX512F__
TEST(GCCore_CPU_conv2d_fwd_bias_cpp, Test_2DConv_1x1_3_NCX) {
    check_conv_correctness_and_tuning_fwd(cfg_fwd, 28, 64, 32, 56, 56, 1, 1, 2,
            0, true, false, false, false, true, false);
}
TEST(GCCore_CPU_conv2d_fwd_bias_cpp, Test_2DConv_1x1_3_NXC) {
    GTEST_SKIP();
    check_conv_correctness_and_tuning_fwd(cfg_fwd, 28, 64, 32, 56, 56, 1, 1, 2,
            0, true, false, false, false, false, true);
}
TEST(GCCore_CPU_conv2d_fwd_bias_cpp, Test_2DConv_1x1_4_NCX) {
    check_conv_correctness_and_tuning_fwd(cfg_fwd, 28, 512, 128, 28, 28, 1, 1,
            1, 0, true, false, false, false, true, false);
}
TEST(GCCore_CPU_conv2d_fwd_bias_cpp, Test_2DConv_1x1_4_NXC) {
    GTEST_SKIP();
    check_conv_correctness_and_tuning_fwd(cfg_fwd, 28, 512, 128, 28, 28, 1, 1,
            1, 0, true, false, false, false, false, true);
}
// conv1x1 with bias with given cfg
TEST(GCCore_CPU_conv2d_fwd_bias_cpp, Test_2DConv_1x1_5_NCX) {
    check_conv_correctness_and_tuning_fwd({32, 32, 1, 7, 28, 28, 0, 4}, 28, 64,
            32, 28, 28, 1, 1, 1, 0, true, false, false, false, true, false);
}
TEST(GCCore_CPU_conv2d_fwd_bias_cpp, Test_2DConv_1x1_5_NXC) {
    GTEST_SKIP();
    check_conv_correctness_and_tuning_fwd({32, 32, 1, 7, 28, 28, 0, 4}, 28, 64,
            32, 28, 28, 1, 1, 1, 0, true, false, false, false, false, true);
}
#endif

#define conv_padding_support_NXC 0

// conv3x3
TEST(GCCore_CPU_conv2d_fwd_cpp, Test_2DConv_3x3_1_NCX) {
    check_conv_correctness_and_tuning_fwd(cfg_fwd_3x3, 1, 64, 64, 28, 28, 3, 3,
            1, 0, false, false, false, false, true, false);
}
TEST(GCCore_CPU_conv2d_fwd_cpp, Test_2DConv_3x3_1_NXC) {
    check_conv_correctness_and_tuning_fwd(cfg_fwd_3x3, 1, 64, 64, 28, 28, 3, 3,
            1, 0, false, false, false, false, false, true);
}
TEST(GCCore_CPU_conv2d_fwd_cpp, Test_2DConv_3x3_2_NCX) {
    check_conv_correctness_and_tuning_fwd(cfg_fwd_3x3, 1, 64, 128, 28, 28, 3, 3,
            2, 0, false, false, false, false, true, false);
}
#if conv_padding_support_NXC
TEST(GCCore_CPU_conv2d_fwd_cpp, Test_2DConv_3x3_2_NXC) {
    check_conv_correctness_and_tuning_fwd(cfg_fwd_3x3, 1, 64, 128, 28, 28, 3, 3,
            2, 0, false, false, false, false, false, true);
}
#endif
TEST(GCCore_CPU_conv2d_fwd_cpp, Test_2DConv_3x3_3_NCX) {
    check_conv_correctness_and_tuning_fwd(cfg_fwd_3x3, 28, 64, 64, 56, 56, 3, 3,
            1, 1, false, false, false, false, true, false);
}
#if conv_padding_support_NXC
TEST(GCCore_CPU_conv2d_fwd_cpp, Test_2DConv_3x3_3_NXC) {
    check_conv_correctness_and_tuning_fwd(cfg_fwd_3x3, 28, 64, 64, 56, 56, 3, 3,
            1, 1, false, false, false, false, false, true);
}
#endif
TEST(GCCore_CPU_conv2d_fwd_cpp, Test_2DConv_3x3_4_NCX) {
    check_conv_correctness_and_tuning_fwd(cfg_fwd_3x3, 28, 128, 64, 14, 14, 3,
            3, 2, 1, false, false, false, false, true, false);
}
#if conv_padding_support_NXC
TEST(GCCore_CPU_conv2d_fwd_cpp, Test_2DConv_3x3_4_NXC) {
    check_conv_correctness_and_tuning_fwd(cfg_fwd_3x3, 28, 128, 64, 14, 14, 3,
            3, 2, 1, false, false, false, false, false, true);
}
#endif
TEST(GCCore_CPU_conv2d_fwd_cpp, Test_2DConv_3x3_large_pad) {
    check_conv_correctness_and_tuning_fwd(
            {16, 17, 1, 1, 1, -1, -1, 3}, 1, 16, 17, 27, 27, 3, 3, 1, 4);
}

#ifdef __AVX512F__
TEST(GCCore_CPU_conv2d_fwd_cpp, Test_2DConv_3x3_5_NCX) {
    check_conv_correctness_and_tuning_fwd(cfg_fwd_3x3, 28, 64, 128, 14, 14, 3,
            3, 1, 0, false, false, false, false, true, false);
}
TEST(GCCore_CPU_conv2d_fwd_cpp, Test_2DConv_3x3_5_NXC) {
    check_conv_correctness_and_tuning_fwd(cfg_fwd_3x3, 28, 64, 128, 14, 14, 3,
            3, 1, 0, false, false, false, false, false, true);
}
TEST(GCCore_CPU_conv2d_fwd_cpp, Test_2DConv_3x3_6_NCX) {
    check_conv_correctness_and_tuning_fwd(cfg_fwd_3x3, 28, 128, 64, 28, 28, 3,
            3, 1, 1, false, false, false, false, true, false);
}
#if conv_padding_support_NXC
TEST(GCCore_CPU_conv2d_fwd_cpp, Test_2DConv_3x3_6_NXC) {
    check_conv_correctness_and_tuning_fwd(cfg_fwd_3x3, 28, 128, 64, 28, 28, 3,
            3, 1, 1, false, false, false, false, false, true);
}
#endif
TEST(GCCore_CPU_conv2d_fwd_cpp, Test_2DConv_3x3_7_NCX) {
    check_conv_correctness_and_tuning_fwd(cfg_fwd_3x3, 1, 64, 64, 56, 56, 3, 3,
            1, 1, false, false, false, false, true, false);
}
#if conv_padding_support_NXC
TEST(GCCore_CPU_conv2d_fwd_cpp, Test_2DConv_3x3_7_NXC) {
    check_conv_correctness_and_tuning_fwd(cfg_fwd_3x3, 1, 64, 64, 56, 56, 3, 3,
            1, 1, false, false, false, false, false, true);
}
#endif
TEST(GCCore_CPU_conv2d_fwd_cpp, Test_2DConv_3x3_8_NCX) {
    check_conv_correctness_and_tuning_fwd(cfg_fwd_3x3, 28, 64, 128, 14, 14, 3,
            3, 1, 1, false, false, false, false, true, false);
}
#if conv_padding_support_NXC
TEST(GCCore_CPU_conv2d_fwd_cpp, Test_2DConv_3x3_8_NXC) {
    check_conv_correctness_and_tuning_fwd(cfg_fwd_3x3, 28, 64, 128, 14, 14, 3,
            3, 1, 1, false, false, false, false, false, true);
}
#endif
TEST(GCCore_CPU_conv2d_fwd_cpp, Test_2DConv_3x3_9_NCX) {
    check_conv_correctness_and_tuning_fwd(cfg_fwd_3x3, 28, 64, 64, 28, 28, 3, 3,
            1, 3, false, false, false, false, true, false);
}
#if conv_padding_support_NXC
TEST(GCCore_CPU_conv2d_fwd_cpp, Test_2DConv_3x3_9_NXC) {
    check_conv_correctness_and_tuning_fwd(cfg_fwd_3x3, 28, 64, 64, 28, 28, 3, 3,
            1, 3, false, false, false, false, false, true);
}
#endif
TEST(GCCore_CPU_conv2d_fwd_cpp, Test_2DConv_3x3_10_NCX) {
    check_conv_correctness_and_tuning_fwd(cfg_fwd_3x3, 1, 64, 64, 56, 56, 3, 3,
            2, 2, false, false, false, false, true, false);
}
#if conv_padding_support_NXC
TEST(GCCore_CPU_conv2d_fwd_cpp, Test_2DConv_3x3_10_NXC) {
    check_conv_correctness_and_tuning_fwd(cfg_fwd_3x3, 1, 64, 64, 56, 56, 3, 3,
            2, 2, false, false, false, false, false, true);
}
#endif
TEST(GCCore_CPU_conv2d_fwd_cpp, Test_2DConv_3x3_11_NCX) {
    check_conv_correctness_and_tuning_fwd(cfg_fwd_3x3, 1, 64, 64, 28, 28, 3, 3,
            2, 3, false, false, false, false, true, false);
}
#if conv_padding_support_NXC
TEST(GCCore_CPU_conv2d_fwd_cpp, Test_2DConv_3x3_11_NXC) {
    check_conv_correctness_and_tuning_fwd(cfg_fwd_3x3, 1, 64, 64, 28, 28, 3, 3,
            2, 3, false, false, false, false, false, true);
}
#endif
TEST(GCCore_CPU_conv2d_fwd_cpp, Test_2DConv_3x3_12_NCX) {
    check_conv_correctness_and_tuning_fwd(cfg_fwd_3x3, 28, 128, 256, 56, 56, 3,
            3, 2, 1, false, false, false, false, true, false);
}
#if conv_padding_support_NXC
TEST(GCCore_CPU_conv2d_fwd_cpp, Test_2DConv_3x3_12_NXC) {
    check_conv_correctness_and_tuning_fwd(cfg_fwd_3x3, 28, 128, 256, 56, 56, 3,
            3, 2, 1, false, false, false, false, false, true);
}
#endif
TEST(GCCore_CPU_conv2d_fwd_cpp, Test_2DConv_3x3_13_NCX_asym_pad) {
    check_conv_correctness_and_tuning_fwd(cfg_fwd_3x3, 1, 64, 64, 224, 224, 7,
            7, sc_dims {2, 2}, sc_dims {3, 3}, sc_dims {2, 2}, sc_dims {1, 1},
            false, false, false, false, true, false);
}
#if conv_padding_support_NXC
TEST(GCCore_CPU_conv2d_fwd_cpp, Test_2DConv_3x3_13_NXC_asym_pad) {
    check_conv_correctness_and_tuning_fwd(cfg_fwd_3x3, 1, 64, 64, 224, 224, 7,
            7, sc_dims {2, 2}, sc_dims {3, 3}, sc_dims {2, 2}, sc_dims {1, 1},
            false, false, false, false, false, true);
}
#endif
TEST(GCCore_CPU_conv2d_fwd_cpp, Test_2DConv_3x3_14_NCX_asym_pad) {
    check_conv_correctness_and_tuning_fwd(cfg_fwd_3x3, 1, 64, 64, 224, 224, 7,
            7, sc_dims {1, 1}, sc_dims {3, 3}, sc_dims {2, 2}, sc_dims {1, 1},
            false, false, false, false, true, false);
}
#if conv_padding_support_NXC
TEST(GCCore_CPU_conv2d_fwd_cpp, Test_2DConv_3x3_14_NXC_asym_pad) {
    check_conv_correctness_and_tuning_fwd(cfg_fwd_3x3, 1, 64, 64, 224, 224, 4,
            4, sc_dims {1, 1}, sc_dims {3, 3}, sc_dims {2, 2}, sc_dims {1, 1},
            false, false, false, false, false, true);
}
#endif
TEST(GCCore_CPU_conv2d_fwd_cpp, Test_2DConv_3x3_with_dilation) {
    std::vector<std::vector<int>> workload_list = {
            // prepadding
            {1, 256, 960, 38, 38, 12, 1, 0}, // deeplabv3_mobilenet
            {1, 256, 960, 62, 62, 24, 1, 0}, // deeplabv3_mobilenet
            {1, 256, 960, 86, 86, 36, 1, 0}, // deeplabv3_mobilenet
            {1, 256, 256, 32, 32, 2, 1, 0}, // deeplabv3_resnet101
            {1, 256, 256, 36, 36, 4, 1, 0}, // deeplabv3_resnet101
            {1, 1024, 512, 31, 31, 6, 1, 0}, // ssd300_vgg16
            // with padding
            {1, 256, 960, 14, 14, 12, 1, 12}, // deeplabv3_mobilenet
            {1, 256, 960, 14, 14, 24, 1, 24}, // deeplabv3_mobilenet
            {1, 256, 960, 14, 14, 36, 1, 36}, // deeplabv3_mobilenet
            {1, 256, 256, 28, 28, 2, 1, 2}, // deeplabv3_resnet101
            {1, 256, 256, 28, 28, 4, 1, 4}, // deeplabv3_resnet101
            {1, 1024, 512, 19, 19, 6, 1, 6}, // ssd300_vgg16
    }; // N, K, C, H, W, Dilation, Stride, Padding

    int R = 3, S = 3;
    for (auto workload : workload_list) {
        auto N = workload[0];
        auto K = workload[1];
        auto C = workload[2];
        auto H = workload[3];
        auto W = workload[4];
        auto dilation = workload[5];
        auto stride = workload[6];
        auto padding = workload[7];
        if (dilation * 2 + 1 > H + 2 * padding) { continue; }
        check_conv_correctness_and_tuning_fwd(conv_fwd_config_t(), N, K, C, H,
                W, R, S, {stride, stride}, {padding, padding},
                {dilation, dilation}, false, false, false, true, false, true);
    }
    return;
}
#endif

// conv3x3 with bias
TEST(GCCore_CPU_conv2d_fwd_bias_cpp, Test_2DConv_3x3_1_NCX) {
    check_conv_correctness_and_tuning_fwd(cfg_fwd_3x3, 1, 64, 64, 56, 56, 3, 3,
            1, 0, true, false, false, false, true, false);
}
TEST(GCCore_CPU_conv2d_fwd_bias_cpp, Test_2DConv_3x3_1_NXC) {
    check_conv_correctness_and_tuning_fwd(cfg_fwd_3x3, 1, 64, 64, 56, 56, 3, 3,
            1, 0, true, false, false, false, false, true);
}
TEST(GCCore_CPU_conv2d_fwd_bias_cpp, Test_2DConv_3x3_2_NCX) {
    check_conv_correctness_and_tuning_fwd(cfg_fwd_3x3, 28, 128, 64, 28, 28, 3,
            3, 1, 1, true, false, false, false, true, false);
}
#if conv_padding_support_NXC
TEST(GCCore_CPU_conv2d_fwd_bias_cpp, Test_2DConv_3x3_2_NXC) {
    check_conv_correctness_and_tuning_fwd(cfg_fwd_3x3, 28, 128, 64, 28, 28, 3,
            3, 1, 1, true, false, false, false, false, true);
}
#endif

#ifdef __AVX512F__
TEST(GCCore_CPU_conv2d_fwd_bias_cpp, Test_2DConv_3x3_3_NCX) {
    check_conv_correctness_and_tuning_fwd(cfg_fwd_3x3, 28, 64, 128, 56, 56, 3,
            3, 1, 0, true, false, false, false, true, false);
}
TEST(GCCore_CPU_conv2d_fwd_bias_cpp, Test_2DConv_3x3_3_NXC) {
    GTEST_SKIP();
    check_conv_correctness_and_tuning_fwd(cfg_fwd_3x3, 28, 64, 128, 56, 56, 3,
            3, 1, 0, true, false, false, false, false, true);
}
TEST(GCCore_CPU_conv2d_fwd_bias_cpp, Test_2DConv_3x3_4_NCX) {
    check_conv_correctness_and_tuning_fwd(cfg_fwd_3x3, 1, 64, 128, 28, 28, 3, 3,
            3, 0, true, false, false, false, true, false);
}
TEST(GCCore_CPU_conv2d_fwd_bias_cpp, Test_2DConv_3x3_4_NXC) {
    GTEST_SKIP();
    check_conv_correctness_and_tuning_fwd(cfg_fwd_3x3, 1, 64, 128, 28, 28, 3, 3,
            3, 0, true, false, false, false, false, true);
}
TEST(GCCore_CPU_conv2d_fwd_bias_cpp, Test_2DConv_3x3_5_NCX) {
    check_conv_correctness_and_tuning_fwd(cfg_fwd_3x3, 1, 64, 64, 56, 56, 3, 3,
            2, 2, true, false, false, false, true, false);
}
TEST(GCCore_CPU_conv2d_fwd_bias_cpp, Test_2DConv_3x3_5_NXC) {
    GTEST_SKIP();
    check_conv_correctness_and_tuning_fwd(cfg_fwd_3x3, 1, 64, 64, 56, 56, 3, 3,
            2, 2, true, false, false, false, false, true);
}
TEST(GCCore_CPU_conv2d_fwd_bias_cpp, Test_2DConv_3x3_6_NCX) {
    check_conv_correctness_and_tuning_fwd(cfg_fwd_3x3, 1, 64, 64, 28, 28, 3, 3,
            2, 3, true, false, false, false, true, false);
}
TEST(GCCore_CPU_conv2d_fwd_bias_cpp, Test_2DConv_3x3_6_NXC) {
    GTEST_SKIP();
    check_conv_correctness_and_tuning_fwd(cfg_fwd_3x3, 1, 64, 64, 28, 28, 3, 3,
            2, 3, true, false, false, false, false, true);
}
TEST(GCCore_CPU_conv2d_fwd_bias_cpp, Test_2DConv_3x3_7_NCX) {
    check_conv_correctness_and_tuning_fwd(cfg_fwd_3x3, 28, 128, 64, 56, 56, 3,
            3, 2, 1, true, false, false, false, true, false);
}
TEST(GCCore_CPU_conv2d_fwd_bias_cpp, Test_2DConv_3x3_7_NXC) {
    GTEST_SKIP();
    check_conv_correctness_and_tuning_fwd(cfg_fwd_3x3, 28, 128, 64, 56, 56, 3,
            3, 2, 1, true, false, false, false, false, true);
}
#endif

// conv with bias/bn/relu
TEST(GCCore_CPU_conv2d_fwd_bias_bn_relu_cpp, Test_2DConv_1_NCX) {
    check_conv_correctness_and_tuning_fwd(cfg_fwd, 28, 64, 64, 56, 56, 1, 1, 1,
            0, true, true, false, false, true, false);
}
TEST(GCCore_CPU_conv2d_fwd_bias_bn_relu_cpp, Test_2DConv_1_NXC) {
    check_conv_correctness_and_tuning_fwd(cfg_fwd, 28, 64, 64, 56, 56, 1, 1, 1,
            0, true, true, false, false, false, true);
}
TEST(GCCore_CPU_conv2d_fwd_bias_bn_relu_cpp, Test_2DConv_2_NCX) {
    check_conv_correctness_and_tuning_fwd(cfg_fwd_3x3, 28, 64, 64, 56, 56, 3, 3,
            2, 1, true, true, false, false, true, false);
}
#if conv_padding_support_NXC
TEST(GCCore_CPU_conv2d_fwd_bias_bn_relu_cpp, Test_2DConv_2_NXC) {
    check_conv_correctness_and_tuning_fwd(cfg_fwd_3x3, 28, 64, 64, 56, 56, 3, 3,
            2, 1, true, true, false, false, false, true);
}
#endif

#ifdef __AVX512F__
TEST(GCCore_CPU_conv2d_fwd_bias_bn_relu_cpp, Test_2DConv_3_NCX) {
    check_conv_correctness_and_tuning_fwd(cfg_fwd, 28, 64, 32, 56, 56, 1, 1, 2,
            0, true, true, false, false, true, false);
}
TEST(GCCore_CPU_conv2d_fwd_bias_bn_relu_cpp, Test_2DConv_3_NXC) {
    GTEST_SKIP();
    check_conv_correctness_and_tuning_fwd(cfg_fwd, 28, 64, 32, 56, 56, 1, 1, 2,
            0, true, true, false, false, false, true);
}
TEST(GCCore_CPU_conv2d_fwd_bias_bn_relu_cpp, Test_2DConv_4_NCX) {
    check_conv_correctness_and_tuning_fwd({3, 1, 1, 4, 4, 4, 1, 1}, 28, 3, 16,
            28, 28, 1, 1, 1, 0, true, true, false, false, true, false);
}
TEST(GCCore_CPU_conv2d_fwd_bias_bn_relu_cpp, Test_2DConv_4_NXC) {
    GTEST_SKIP();
    check_conv_correctness_and_tuning_fwd({3, 1, 1, 4, 4, 4, 1, 1}, 28, 3, 16,
            28, 28, 1, 1, 1, 0, true, true, false, false, false, true);
}
TEST(GCCore_CPU_conv2d_fwd_bias_bn_relu_cpp, Test_2DConv_5_NCX) {
    check_conv_correctness_and_tuning_fwd({32, 32, 1, 7, 28, 28, 0, 4}, 1, 64,
            32, 28, 28, 1, 1, 1, 0, true, true, false, false, true, false);
}
TEST(GCCore_CPU_conv2d_fwd_bias_bn_relu_cpp, Test_2DConv_5_NXC) {
    GTEST_SKIP();
    check_conv_correctness_and_tuning_fwd({32, 32, 1, 7, 28, 28, 0, 4}, 1, 64,
            32, 28, 28, 1, 1, 1, 0, true, true, false, false, false, true);
}
TEST(GCCore_CPU_conv2d_fwd_bias_bn_relu_cpp, Test_2DConv_6_NCX) {
    check_conv_correctness_and_tuning_fwd(cfg_fwd_3x3, 1, 64, 64, 56, 56, 3, 3,
            1, 0, true, true, false, false, true, false);
}
TEST(GCCore_CPU_conv2d_fwd_bias_bn_relu_cpp, Test_2DConv_6_NXC) {
    GTEST_SKIP();
    check_conv_correctness_and_tuning_fwd(cfg_fwd_3x3, 1, 64, 64, 56, 56, 3, 3,
            1, 0, true, true, false, false, false, true);
}
TEST(GCCore_CPU_conv2d_fwd_bias_bn_relu_cpp, Test_2DConv_7_NCX) {
    check_conv_correctness_and_tuning_fwd(cfg_fwd_3x3, 28, 64, 64, 28, 28, 3, 3,
            1, 0, true, true, false, false, true, false);
}
TEST(GCCore_CPU_conv2d_fwd_bias_bn_relu_cpp, Test_2DConv_7_NXC) {
    GTEST_SKIP();
    check_conv_correctness_and_tuning_fwd(cfg_fwd_3x3, 28, 64, 64, 28, 28, 3, 3,
            1, 0, true, true, false, false, false, true);
}
TEST(GCCore_CPU_conv2d_fwd_bias_bn_relu_cpp, Test_2DConv_8_NCX) {
    check_conv_correctness_and_tuning_fwd(cfg_fwd_3x3, 1, 64, 128, 28, 28, 3, 3,
            2, 0, true, true, false, false, true, false);
}
TEST(GCCore_CPU_conv2d_fwd_bias_bn_relu_cpp, Test_2DConv_8_NXC) {
    GTEST_SKIP();
    check_conv_correctness_and_tuning_fwd(cfg_fwd_3x3, 1, 64, 128, 28, 28, 3, 3,
            2, 0, true, true, false, false, false, true);
}
TEST(GCCore_CPU_conv2d_fwd_bias_bn_relu_cpp, Test_2DConv_9_NCX) {
    check_conv_correctness_and_tuning_fwd(cfg_fwd_3x3, 28, 128, 64, 28, 28, 3,
            3, 1, 1, true, true, false, false, true, false);
}
TEST(GCCore_CPU_conv2d_fwd_bias_bn_relu_cpp, Test_2DConv_9_NXC) {
    GTEST_SKIP();
    check_conv_correctness_and_tuning_fwd(cfg_fwd_3x3, 28, 128, 64, 28, 28, 3,
            3, 1, 1, true, true, false, false, false, true);
}
TEST(GCCore_CPU_conv2d_fwd_bias_bn_relu_cpp, Test_2DConv_10_NCX) {
    check_conv_correctness_and_tuning_fwd(cfg_fwd_3x3, 1, 64, 64, 56, 56, 3, 3,
            2, 2, true, true, false, false, true, false);
}
TEST(GCCore_CPU_conv2d_fwd_bias_bn_relu_cpp, Test_2DConv_10_NXC) {
    GTEST_SKIP();
    check_conv_correctness_and_tuning_fwd(cfg_fwd_3x3, 1, 64, 64, 56, 56, 3, 3,
            2, 2, true, true, false, false, false, true);
}
TEST(GCCore_CPU_conv2d_fwd_bias_bn_relu_cpp, Test_2DConv_11_NCX) {
    check_conv_correctness_and_tuning_fwd(cfg_fwd_3x3, 1, 64, 64, 28, 28, 3, 3,
            2, 3, true, true, false, false, true, false);
}
TEST(GCCore_CPU_conv2d_fwd_bias_bn_relu_cpp, Test_2DConv_11_NXC) {
    GTEST_SKIP();
    check_conv_correctness_and_tuning_fwd(cfg_fwd_3x3, 1, 64, 64, 28, 28, 3, 3,
            2, 3, true, true, false, false, false, true);
}
TEST(GCCore_CPU_conv2d_fwd_bias_bn_relu_cpp, Test_2DConv_12_NCX) {
    check_conv_correctness_and_tuning_fwd(cfg_fwd_3x3, 28, 64, 64, 28, 28, 3, 3,
            1, 3, true, true, false, false, true, false);
}
TEST(GCCore_CPU_conv2d_fwd_bias_bn_relu_cpp, Test_2DConv_12_NXC) {
    GTEST_SKIP();
    check_conv_correctness_and_tuning_fwd(cfg_fwd_3x3, 28, 64, 64, 28, 28, 3, 3,
            1, 3, true, true, false, false, false, true);
}
TEST(GCCore_CPU_conv2d_fwd_bias_bn_relu_cpp, Test_2DConv_13_NCX) {
    check_conv_correctness_and_tuning_fwd({3, 1, 1, 2, 2, 2, 1, 1}, 28, 3, 16,
            28, 28, 3, 3, 1, 1, true, true, false, false, true, false);
}
TEST(GCCore_CPU_conv2d_fwd_bias_bn_relu_cpp, Test_2DConv_13_NXC) {
    GTEST_SKIP();
    check_conv_correctness_and_tuning_fwd({3, 1, 1, 2, 2, 2, 1, 1}, 28, 3, 16,
            28, 28, 3, 3, 1, 1, true, true, false, false, false, true);
}
#endif

#define conv_padding_support_NXC 0
// conv with bias/bn/relu/eltwise-add
TEST(GCCore_CPU_conv2d_fwd_bias_bn_relu_eleadd_cpp, Test_2DConv_1_NCX) {
    check_conv_correctness_and_tuning_fwd(cfg_fwd, 28, 128, 128, 56, 56, 1, 1,
            2, 0, true, true, true, false, true, false);
}
TEST(GCCore_CPU_conv2d_fwd_bias_bn_relu_eleadd_cpp, Test_2DConv_1_NXC) {
    check_conv_correctness_and_tuning_fwd(cfg_fwd, 28, 128, 128, 56, 56, 1, 1,
            2, 0, true, true, true, false, false, true);
}
TEST(GCCore_CPU_conv2d_fwd_bias_bn_relu_eleadd_cpp, Test_2DConv_2_NCX) {
    check_conv_correctness_and_tuning_fwd(cfg_fwd_3x3, 1, 64, 64, 56, 56, 3, 3,
            1, 0, true, true, true, false, true, false);
}
#if conv_padding_support_NXC
TEST(GCCore_CPU_conv2d_fwd_bias_bn_relu_eleadd_cpp, Test_2DConv_2_NXC) {
    check_conv_correctness_and_tuning_fwd(cfg_fwd_3x3, 1, 64, 64, 56, 56, 3, 3,
            1, 0, true, true, true, false, false, true);
}
#endif
TEST(GCCore_CPU_conv2d_fwd_bias_bn_relu_eleadd_cpp, Test_2DConv_3_NCX) {
    check_conv_correctness_and_tuning_fwd(cfg_fwd_3x3, 28, 128, 64, 56, 56, 3,
            3, 2, 1, true, true, true, false, true, false);
}
#if conv_padding_support_NXC
TEST(GCCore_CPU_conv2d_fwd_bias_bn_relu_eleadd_cpp, Test_2DConv_3_NXC) {
    check_conv_correctness_and_tuning_fwd(cfg_fwd_3x3, 28, 128, 64, 56, 56, 3,
            3, 2, 1, true, true, true, false, false, true);
}
#endif

#ifdef __AVX512F__
TEST(GCCore_CPU_conv2d_fwd_bias_bn_relu_eleadd_cpp, Test_2DConv_4_NCX) {
    check_conv_correctness_and_tuning_fwd(cfg_fwd, 28, 64, 64, 56, 56, 1, 1, 1,
            0, true, true, true, false, true, false);
}
TEST(GCCore_CPU_conv2d_fwd_bias_bn_relu_eleadd_cpp, Test_2DConv_4_NXC) {
    GTEST_SKIP();
    check_conv_correctness_and_tuning_fwd(cfg_fwd, 28, 64, 64, 56, 56, 1, 1, 1,
            0, true, true, true, false, false, true);
}
TEST(GCCore_CPU_conv2d_fwd_bias_bn_relu_eleadd_cpp, Test_2DConv_5_NCX) {
    check_conv_correctness_and_tuning_fwd(cfg_fwd, 1, 64, 64, 56, 56, 1, 1, 1,
            0, true, true, true, false, true, false);
}
TEST(GCCore_CPU_conv2d_fwd_bias_bn_relu_eleadd_cpp, Test_2DConv_5_NXC) {
    GTEST_SKIP();
    check_conv_correctness_and_tuning_fwd(cfg_fwd, 1, 64, 64, 56, 56, 1, 1, 1,
            0, true, true, true, false, false, true);
}
TEST(GCCore_CPU_conv2d_fwd_bias_bn_relu_eleadd_cpp, Test_2DConv_6_NCX) {
    check_conv_correctness_and_tuning_fwd({3, 1, 1, 4, 4, 4, 1, 1}, 28, 3, 16,
            28, 28, 1, 1, 1, 0, true, true, true, false, true, false);
}
TEST(GCCore_CPU_conv2d_fwd_bias_bn_relu_eleadd_cpp, Test_2DConv_6_NXC) {
    GTEST_SKIP();
    check_conv_correctness_and_tuning_fwd({3, 1, 1, 4, 4, 4, 1, 1}, 28, 3, 16,
            28, 28, 1, 1, 1, 0, true, true, true, false, false, true);
}

// conv3x3 with bias/bn/relu/eltwise-add
TEST(GCCore_CPU_conv2d_fwd_bias_bn_relu_eleadd_cpp, Test_2DConv_7_NCX) {
    check_conv_correctness_and_tuning_fwd(cfg_fwd_3x3, 28, 64, 64, 28, 28, 3, 3,
            1, 0, true, true, true, false, true, false);
}
TEST(GCCore_CPU_conv2d_fwd_bias_bn_relu_eleadd_cpp, Test_2DConv_7_NXC) {
    GTEST_SKIP();
    check_conv_correctness_and_tuning_fwd(cfg_fwd_3x3, 28, 64, 64, 28, 28, 3, 3,
            1, 0, true, true, true, false, false, true);
}
TEST(GCCore_CPU_conv2d_fwd_bias_bn_relu_eleadd_cpp, Test_2DConv_8_NCX) {
    check_conv_correctness_and_tuning_fwd(cfg_fwd_3x3, 1, 64, 128, 28, 28, 3, 3,
            3, 0, true, true, true, false, true, false);
}
TEST(GCCore_CPU_conv2d_fwd_bias_bn_relu_eleadd_cpp, Test_2DConv_8_NXC) {
    GTEST_SKIP();
    check_conv_correctness_and_tuning_fwd(cfg_fwd_3x3, 1, 64, 128, 28, 28, 3, 3,
            3, 0, true, true, true, false, false, true);
}
TEST(GCCore_CPU_conv2d_fwd_bias_bn_relu_eleadd_cpp, Test_2DConv_9_NCX) {
    check_conv_correctness_and_tuning_fwd(cfg_fwd_3x3, 28, 128, 64, 28, 28, 3,
            3, 1, 1, true, true, true, false, true, false);
}
TEST(GCCore_CPU_conv2d_fwd_bias_bn_relu_eleadd_cpp, Test_2DConv_9_NXC) {
    GTEST_SKIP();
    check_conv_correctness_and_tuning_fwd(cfg_fwd_3x3, 28, 128, 64, 28, 28, 3,
            3, 1, 1, true, true, true, false, false, true);
}
TEST(GCCore_CPU_conv2d_fwd_bias_bn_relu_eleadd_cpp, Test_2DConv_10_NCX) {
    check_conv_correctness_and_tuning_fwd(cfg_fwd_3x3, 1, 64, 64, 56, 56, 3, 3,
            2, 2, true, true, true, false, true, false);
}
TEST(GCCore_CPU_conv2d_fwd_bias_bn_relu_eleadd_cpp, Test_2DConv_10_NXC) {
    GTEST_SKIP();
    check_conv_correctness_and_tuning_fwd(cfg_fwd_3x3, 1, 64, 64, 56, 56, 3, 3,
            2, 2, true, true, true, false, false, true);
}
TEST(GCCore_CPU_conv2d_fwd_bias_bn_relu_eleadd_cpp, Test_2DConv_11_NCX) {
    check_conv_correctness_and_tuning_fwd(cfg_fwd_3x3, 1, 64, 64, 28, 28, 3, 3,
            2, 3, true, true, true, false, true, false);
}
TEST(GCCore_CPU_conv2d_fwd_bias_bn_relu_eleadd_cpp, Test_2DConv_11_NXC) {
    GTEST_SKIP();
    check_conv_correctness_and_tuning_fwd(cfg_fwd_3x3, 1, 64, 64, 28, 28, 3, 3,
            2, 3, true, true, true, false, false, true);
}
TEST(GCCore_CPU_conv2d_fwd_bias_bn_relu_eleadd_cpp, Test_2DConv_12_NCX) {
    check_conv_correctness_and_tuning_fwd(cfg_fwd_3x3, 28, 64, 64, 28, 28, 3, 3,
            1, 3, true, true, true, false, true, false);
}
TEST(GCCore_CPU_conv2d_fwd_bias_bn_relu_eleadd_cpp, Test_2DConv_12_NXC) {
    GTEST_SKIP();
    check_conv_correctness_and_tuning_fwd(cfg_fwd_3x3, 28, 64, 64, 28, 28, 3, 3,
            1, 3, true, true, true, false, false, true);
}
TEST(GCCore_CPU_conv2d_fwd_bias_bn_relu_eleadd_cpp, Test_2DConv_13_NCX) {
    check_conv_correctness_and_tuning_fwd({3, 1, 1, 2, 2, 2, 1, 1}, 28, 3, 16,
            28, 28, 3, 3, 1, 1, true, true, true, false, true, false);
}
TEST(GCCore_CPU_conv2d_fwd_bias_bn_relu_eleadd_cpp, Test_2DConv_13_NXC) {
    GTEST_SKIP();
    check_conv_correctness_and_tuning_fwd({3, 1, 1, 2, 2, 2, 1, 1}, 28, 3, 16,
            28, 28, 3, 3, 1, 1, true, true, true, false, false, true);
}
#endif

TEST(GCCore_CPU_conv2d_bwd_d_cpp, TestCONV2D_1x1_1) {
    check_conv_correctness_and_tuning_bwd_d(28, 256, 128, 28, 28, 1, 1, 1, 0);
}
TEST(GCCore_CPU_conv2d_bwd_d_cpp, TestCONV2D_1x1_2) {
    check_conv_correctness_and_tuning_bwd_d(28, 256, 128, 28, 28, 1, 1, 2, 0);
}
TEST(GCCore_CPU_conv2d_bwd_d_cpp, TestCONV2D_1x1_3) {
    check_conv_correctness_and_tuning_bwd_d(28, 256, 128, 112, 112, 1, 1, 1, 1);
}
TEST(GCCore_CPU_conv2d_bwd_d_cpp, TestCONV2D_1x1_4) {
    check_conv_correctness_and_tuning_bwd_d(28, 64, 64, 56, 56, 1, 1, 1, 2);
}

TEST(GCCore_CPU_conv2d_bwd_d_cpp, TestCONV2D_3x3_1) {
    check_conv_correctness_and_tuning_bwd_d(28, 256, 128, 28, 28, 3, 3, 1, 0);
}
TEST(GCCore_CPU_conv2d_bwd_d_cpp, TestCONV2D_3x3_2) {
    check_conv_correctness_and_tuning_bwd_d(28, 256, 128, 28, 28, 3, 3, 2, 0);
}
TEST(GCCore_CPU_conv2d_bwd_d_cpp, TestCONV2D_3x3_3) {
    SET_THREADS_OR_SKIP(28);
    check_conv_correctness_and_tuning_bwd_d(28, 256, 128, 28, 28, 3, 3, 1, 1);
}
TEST(GCCore_CPU_conv2d_bwd_d_cpp, TestCONV2D_3x3_4) {
    SET_THREADS_OR_SKIP(28);
    check_conv_correctness_and_tuning_bwd_d(28, 64, 64, 28, 28, 3, 3, 2, 1);
}
TEST(GCCore_CPU_conv2d_bwd_d_cpp, TestCONV2D_3x3_5) {
    REQUIRE_AMX();
    SET_THREADS_OR_SKIP(56);
    check_conv_correctness_and_tuning_bwd_d(
            1, 64, 64, 56, 56, 3, 3, 1, 1, true);
    check_conv_correctness_and_tuning_bwd_d(
            56, 64, 64, 28, 28, 3, 3, 1, 1, true);
    check_conv_correctness_and_tuning_bwd_d(
            56, 65, 121, 28, 28, 3, 3, 1, 1, true);
}

TEST(GCCore_CPU_conv2d_bwd_w_cpp, TestCONV2D_1x1_1) {
    check_conv_correctness_and_tuning_bwd_w(28, 256, 128, 28, 28, 1, 1, 1, 0);
}
TEST(GCCore_CPU_conv2d_bwd_w_cpp, TestCONV2D_1x1_2) {
    check_conv_correctness_and_tuning_bwd_w(28, 256, 128, 28, 28, 1, 1, 2, 0);
}
TEST(GCCore_CPU_conv2d_bwd_w_cpp, TestCONV2D_1x1_3) {
    check_conv_correctness_and_tuning_bwd_w(28, 256, 128, 28, 28, 1, 1, 1, 1);
}
TEST(GCCore_CPU_conv2d_bwd_w_cpp, TestCONV2D_1x1_4) {
    check_conv_correctness_and_tuning_bwd_w(28, 64, 64, 56, 56, 1, 1, 1, 2);
}
TEST(GCCore_CPU_conv2d_bwd_w_cpp, TestCONV2D_1x1_5) {
    REQUIRE_BF16();
    check_conv_correctness_and_tuning_bwd_w(
            1, 64, 64, 56, 56, 1, 1, 1, 0, datatypes::bf16);
}
TEST(GCCore_CPU_conv2d_bwd_w_cpp, TestCONV2D_1x1_6) {
    REQUIRE_BF16();
    check_conv_correctness_and_tuning_bwd_w(
            1, 64, 64, 56, 56, 1, 1, 2, 0, datatypes::bf16);
}

TEST(GCCore_CPU_conv2d_bwd_w_cpp, TestCONV2D_3x3_1) {
    check_conv_correctness_and_tuning_bwd_w(28, 256, 128, 28, 28, 3, 3, 1, 0);
}
TEST(GCCore_CPU_conv2d_bwd_w_cpp, TestCONV2D_3x3_2) {
    check_conv_correctness_and_tuning_bwd_w(28, 256, 128, 28, 28, 3, 3, 2, 0);
}
TEST(GCCore_CPU_conv2d_bwd_w_cpp, TestCONV2D_3x3_3) {
    check_conv_correctness_and_tuning_bwd_w(28, 256, 128, 28, 28, 3, 3, 1, 1);
}
TEST(GCCore_CPU_conv2d_bwd_w_cpp, TestCONV2D_3x3_4) {
    check_conv_correctness_and_tuning_bwd_w(28, 64, 64, 56, 56, 3, 3, 2, 1);
}
TEST(GCCore_CPU_conv2d_bwd_w_cpp, TestCONV2D_3x3_5) {
    SET_THREADS_OR_SKIP(28);
    check_conv_correctness_and_tuning_bwd_w(32, 32, 32, 28, 28, 3, 3, 1, 1);
}
TEST(GCCore_CPU_conv2d_bwd_w_cpp, TestCONV2D_3x3_6) {
    SET_THREADS_OR_SKIP(28);
    check_conv_correctness_and_tuning_bwd_w(32, 32, 32, 56, 56, 3, 3, 2, 1);
}
TEST(GCCore_CPU_conv2d_bwd_w_cpp, TestCONV2D_3x3_7) {
    REQUIRE_BF16();
    check_conv_correctness_and_tuning_bwd_w(
            1, 64, 64, 56, 56, 3, 3, 1, 1, datatypes::bf16);
}
TEST(GCCore_CPU_conv2d_bwd_w_cpp, TestCONV2D_3x3_8) {
    REQUIRE_BF16();
    check_conv_correctness_and_tuning_bwd_w(
            1, 64, 64, 56, 56, 3, 3, 2, 1, datatypes::bf16);
}

TEST(GCCore_CPU_dynamic_conv2d_fwd_cpp, TestConv2D_1x1_1_NXC) {
    check_conv_correctness_and_tuning_fwd(-1, 256, 64, 56, 56, 1, 1, {1, 1},
            {0, 0}, false, false, false,
            /*real_N*/ 1, /*real_H*/ 56, /*real_W*/ 56);
}

TEST(GCCore_CPU_dynamic_conv2d_fwd_cpp, TestConv2D_1x1_1_NXC_fuse_bias) {
    check_conv_correctness_and_tuning_fwd(-1, 256, 64, 56, 56, 1, 1, {1, 1},
            {0, 0}, true, false, false,
            /*real_N*/ 1, /*real_H*/ 56, /*real_W*/ 56);
}

TEST(GCCore_CPU_dynamic_conv2d_fwd_cpp, TestConv2D_1x1_1_NXC_fuse_bn_relu) {
    check_conv_correctness_and_tuning_fwd(-1, 256, 64, 56, 56, 1, 1, {1, 1},
            {0, 0}, true, true, false,
            /*real_N*/ 1, /*real_H*/ 56, /*real_W*/ 56);
}

TEST(GCCore_CPU_dynamic_conv2d_fwd_cpp, TestConv2D_1x1_1_NXC_fuse_eleadd) {
    check_conv_correctness_and_tuning_fwd(-1, 256, 64, 56, 56, 1, 1, {1, 1},
            {0, 0}, true, true, true,
            /*real_N*/ 1, /*real_H*/ 56, /*real_W*/ 56);
}

TEST(GCCore_CPU_dynamic_conv2d_fwd_cpp, TestConv2D_1x1_2_NXC) {
    check_conv_correctness_and_tuning_fwd(-1, 256, 64, -1, -1, 1, 1, {1, 1},
            {0, 0}, false, false, false,
            /*real_N*/ 1, /*real_H*/ 56, /*real_W*/ 56);
}

TEST(GCCore_CPU_dynamic_conv2d_fwd_cpp, TestConv2D_1x1_2_NXC_fuse) {
    check_conv_correctness_and_tuning_fwd(-1, 256, 64, -1, -1, 1, 1, {1, 1},
            {0, 0}, true, true, true,
            /*real_N*/ 1, /*real_H*/ 56, /*real_W*/ 56);
}

TEST(GCCore_CPU_dynamic_conv2d_fwd_cpp, TestConv2D_1x1_3_NXC) {
    check_conv_correctness_and_tuning_fwd(-1, 256, 64, -1, -1, 1, 1, {1, 1},
            {0, 0}, false, false, false,
            /*real_N*/ 1, /*real_H*/ 55, /*real_W*/ 55);
}

TEST(GCCore_CPU_dynamic_conv2d_fwd_cpp, TestConv2D_1x1_3_NXC_fuse) {
    check_conv_correctness_and_tuning_fwd(-1, 256, 64, -1, -1, 1, 1, {1, 1},
            {0, 0}, true, true, true,
            /*real_N*/ 1, /*real_H*/ 55, /*real_W*/ 55);
}

TEST(GCCore_CPU_dynamic_conv2d_fwd_cpp, TestConv2D_1x1_4_NXC) {
    check_conv_correctness_and_tuning_fwd(-1, 256, 64, -1, -1, 1, 1, {1, 1},
            {0, 0}, false, false, false,
            /*real_N*/ 1, /*real_H*/ 67, /*real_W*/ 67);
}

TEST(GCCore_CPU_dynamic_conv2d_fwd_cpp, TestConv2D_1x1_4_NXC_fuse) {
    check_conv_correctness_and_tuning_fwd(-1, 256, 64, -1, -1, 1, 1, {1, 1},
            {0, 0}, true, true, true,
            /*real_N*/ 1, /*real_H*/ 67, /*real_W*/ 67);
}

TEST(GCCore_CPU_dynamic_conv2d_fwd_cpp, TestConv2D_1x1_5_NXC) {
    check_conv_correctness_and_tuning_fwd(-1, 256, 64, 12, 12, 1, 1, {1, 1},
            {0, 0}, false, false, false,
            /*real_N*/ 1, /*real_H*/ 12, /*real_W*/ 12);
    check_conv_correctness_and_tuning_fwd(1, 256, 64, -1, 12, 1, 1, {1, 1},
            {0, 0}, false, false, false,
            /*real_N*/ 1, /*real_H*/ 12, /*real_W*/ 12);
    check_conv_correctness_and_tuning_fwd(1, 256, 64, 12, -1, 1, 1, {1, 1},
            {0, 0}, false, false, false,
            /*real_N*/ 1, /*real_H*/ 12, /*real_W*/ 12);
    check_conv_correctness_and_tuning_fwd(-1, 256, 64, -1, -1, 1, 1, {1, 1},
            {0, 0}, false, false, false,
            /*real_N*/ 1, /*real_H*/ 12, /*real_W*/ 12);
}

TEST(GCCore_CPU_dynamic_conv2d_fwd_cpp, TestConv2D_1x1_5_NXC_fuse) {
    check_conv_correctness_and_tuning_fwd(-1, 256, 64, -1, -1, 1, 1, {1, 1},
            {0, 0}, true, true, true,
            /*real_N*/ 1, /*real_H*/ 12, /*real_W*/ 12);
}

TEST(GCCore_CPU_dynamic_conv2d_fwd_cpp, TestConv2D_1x1_6_NXC) {
    check_conv_correctness_and_tuning_fwd(-1, 256, 64, -1, -1, 1, 1, {1, 1},
            {0, 0}, false, false, false,
            /*real_N*/ 1, /*real_H*/ 2, /*real_W*/ 2);
}

TEST(GCCore_CPU_dynamic_conv2d_fwd_cpp, TestConv2D_1x1_6_NXC_fuse) {
    check_conv_correctness_and_tuning_fwd(-1, 256, 64, -1, -1, 1, 1, {1, 1},
            {0, 0}, true, true, true,
            /*real_N*/ 1, /*real_H*/ 2, /*real_W*/ 2);
}

TEST(GCCore_CPU_dynamic_conv2d_fwd_cpp, TestConv2D_1x1_7_NXC) {
    check_conv_correctness_and_tuning_fwd(-1, 1024, 256, -1, -1, 1, 1, {1, 1},
            {0, 0}, false, false, false,
            /*real_N*/ 1, /*real_H*/ 2, /*real_W*/ 2);
}

TEST(GCCore_CPU_dynamic_conv2d_fwd_cpp, TestConv2D_1x1_7_NXC_fuse) {
    check_conv_correctness_and_tuning_fwd(-1, 1024, 256, -1, -1, 1, 1, {1, 1},
            {0, 0}, true, true, true,
            /*real_N*/ 1, /*real_H*/ 2, /*real_W*/ 2);
}

TEST(GCCore_CPU_dynamic_conv2d_fwd_cpp, TestConv2D_1x1_1_NXC_stride2) {
    check_conv_correctness_and_tuning_fwd(-1, 256, 64, 56, 56, 1, 1, {2, 2},
            {0, 0}, false, false, false,
            /*real_N*/ 1, /*real_H*/ 56, /*real_W*/ 56);
}

TEST(GCCore_CPU_dynamic_conv2d_fwd_cpp, TestConv2D_1x1_1_NXC_stride2_fuse) {
    check_conv_correctness_and_tuning_fwd(-1, 256, 64, 56, 56, 1, 1, {2, 2},
            {0, 0}, true, true, true,
            /*real_N*/ 1, /*real_H*/ 56, /*real_W*/ 56);
}

TEST(GCCore_CPU_dynamic_conv2d_fwd_cpp, TestConv2D_1x1_2_NXC_stride2) {
    check_conv_correctness_and_tuning_fwd(-1, 256, 64, -1, -1, 1, 1, {2, 2},
            {0, 0}, false, false, false,
            /*real_N*/ 1, /*real_H*/ 56, /*real_W*/ 56);
}

TEST(GCCore_CPU_dynamic_conv2d_fwd_cpp, TestConv2D_1x1_2_NXC_stride2_fuse) {
    check_conv_correctness_and_tuning_fwd(-1, 256, 64, -1, -1, 1, 1, {2, 2},
            {0, 0}, true, true, true,
            /*real_N*/ 1, /*real_H*/ 56, /*real_W*/ 56);
}

TEST(GCCore_CPU_dynamic_conv2d_fwd_cpp, TestConv2D_1x1_3_NXC_stride2) {
    check_conv_correctness_and_tuning_fwd(-1, 256, 64, 55, 55, 1, 1, {2, 2},
            {0, 0}, false, false, false,
            /*real_N*/ 1, /*real_H*/ 55, /*real_W*/ 55);
}

TEST(GCCore_CPU_dynamic_conv2d_fwd_cpp, TestConv2D_1x1_3_NXC_stride2_fuse) {
    check_conv_correctness_and_tuning_fwd(-1, 256, 64, 55, 55, 1, 1, {2, 2},
            {0, 0}, true, true, true,
            /*real_N*/ 1, /*real_H*/ 55, /*real_W*/ 55);
}

TEST(GCCore_CPU_dynamic_conv2d_fwd_cpp, TestConv2D_1x1_4_NXC_stride2) {
    check_conv_correctness_and_tuning_fwd(-1, 256, 64, -1, -1, 1, 1, {2, 2},
            {0, 0}, false, false, false,
            /*real_N*/ 1, /*real_H*/ 67, /*real_W*/ 67);
}

TEST(GCCore_CPU_dynamic_conv2d_fwd_cpp, TestConv2D_1x1_4_NXC_stride2_fuse) {
    check_conv_correctness_and_tuning_fwd(-1, 256, 64, -1, -1, 1, 1, {2, 2},
            {0, 0}, true, true, true,
            /*real_N*/ 1, /*real_H*/ 67, /*real_W*/ 67);
}

TEST(GCCore_CPU_dynamic_conv2d_fwd_cpp, TestConv2D_1x1_5_NXC_stride2) {
    check_conv_correctness_and_tuning_fwd(-1, 256, 64, -1, -1, 1, 1, {2, 2},
            {0, 0}, false, false, false,
            /*real_N*/ 1, /*real_H*/ 12, /*real_W*/ 12);
}

TEST(GCCore_CPU_dynamic_conv2d_fwd_cpp, TestConv2D_1x1_5_NXC_stride2_fuse) {
    check_conv_correctness_and_tuning_fwd(-1, 256, 64, -1, -1, 1, 1, {2, 2},
            {0, 0}, true, true, true,
            /*real_N*/ 1, /*real_H*/ 12, /*real_W*/ 12);
}

TEST(GCCore_CPU_dynamic_conv2d_fwd_cpp, TestConv2D_1x1_6_NXC_stride2) {
    check_conv_correctness_and_tuning_fwd(-1, 256, 64, -1, -1, 1, 1, {2, 2},
            {0, 0}, false, false, false,
            /*real_N*/ 1, /*real_H*/ 2, /*real_W*/ 2);
}

TEST(GCCore_CPU_dynamic_conv2d_fwd_cpp, TestConv2D_1x1_6_NXC_stride2_fuse) {
    check_conv_correctness_and_tuning_fwd(-1, 256, 64, -1, -1, 1, 1, {2, 2},
            {0, 0}, true, true, true,
            /*real_N*/ 1, /*real_H*/ 2, /*real_W*/ 2);
}

TEST(GCCore_CPU_dynamic_conv2d_fwd_cpp, TestConv2D_3x3_1_NXC) {
    check_conv_correctness_and_tuning_fwd(-1, 256, 64, 58, 58, 3, 3, {1, 1},
            {0, 0}, false, false, false,
            /*real_N*/ 1, /*real_H*/ 58, /*real_W*/ 58);
}

TEST(GCCore_CPU_dynamic_conv2d_fwd_cpp, TestConv2D_3x3_2_NXC) {
    check_conv_correctness_and_tuning_fwd(-1, 256, 64, -1, -1, 3, 3, {1, 1},
            {0, 0}, false, false, false,
            /*real_N*/ 1, /*real_H*/ 58, /*real_W*/ 58);
}

TEST(GCCore_CPU_dynamic_conv2d_fwd_cpp, TestConv2D_3x3_3_NXC) {
    check_conv_correctness_and_tuning_fwd(-1, 256, 64, -1, -1, 3, 3, {1, 1},
            {0, 0}, false, false, false,
            /*real_N*/ 8, /*real_H*/ 69, /*real_W*/ 69);
}

TEST(GCCore_CPU_dynamic_conv2d_fwd_cpp, TestConv2D_3x3_4_NXC) {
    check_conv_correctness_and_tuning_fwd(-1, 256, 64, -1, -1, 3, 3, {1, 1},
            {0, 0}, false, false, false,
            /*real_N*/ 1, /*real_H*/ 9, /*real_W*/ 9);
}

TEST(GCCore_CPU_dynamic_conv2d_fwd_cpp, TestConv2D_3x3_5_NXC) {
    check_conv_correctness_and_tuning_fwd(-1, 256, 64, -1, -1, 3, 3, {1, 1},
            {0, 0}, false, false, false,
            /*real_N*/ 1, /*real_H*/ 6, /*real_W*/ 6);
}

TEST(GCCore_CPU_dynamic_conv2d_fwd_cpp, TestConv2D_3x3_6_NXC) {
    check_conv_correctness_and_tuning_fwd(-1, 256, 64, 58, 58, 3, 3, {2, 2},
            {0, 0}, false, false, false,
            /*real_N*/ 1, /*real_H*/ 58, /*real_W*/ 58);
}

TEST(GCCore_CPU_dynamic_conv2d_fwd_cpp, TestConv2D_3x3_7_NXC) {
    check_conv_correctness_and_tuning_fwd(-1, 256, 64, -1, -1, 3, 3, {2, 2},
            {0, 0}, false, false, false,
            /*real_N*/ 8, /*real_H*/ 69, /*real_W*/ 69);
}

TEST(GCCore_CPU_dynamic_conv2d_fwd_cpp, TestConv2D_3x3_8_NXC) {
    check_conv_correctness_and_tuning_fwd(-1, 256, 64, -1, -1, 3, 3, {3, 2},
            {0, 0}, false, false, false,
            /*real_N*/ 8, /*real_H*/ 69, /*real_W*/ 69);
}

TEST(GCCore_CPU_dynamic_conv2d_fwd_cpp, TestConv2D_3x3_padding_1_NXC) {
    check_conv_correctness_and_tuning_fwd(-1, 256, 64, 56, 56, 3, 3, {1, 1},
            {1, 1}, false, false, false,
            /*real_N*/ 8, /*real_H*/ 56, /*real_W*/ 56);
}

TEST(GCCore_CPU_dynamic_conv2d_fwd_cpp, TestConv2D_3x3_padding_2_NXC) {
    check_conv_correctness_and_tuning_fwd(-1, 256, 64, -1, -1, 3, 3, {1, 1},
            {1, 1}, false, false, false,
            /*real_N*/ 8, /*real_H*/ 56, /*real_W*/ 56);
}

TEST(GCCore_CPU_dynamic_conv2d_fwd_cpp, TestConv2D_3x3_padding_3_NXC) {
    check_conv_correctness_and_tuning_fwd(-1, 256, 64, -1, -1, 3, 3, {1, 1},
            {1, 1}, false, false, false,
            /*real_N*/ 8, /*real_H*/ 67, /*real_W*/ 56);
}

TEST(GCCore_CPU_dynamic_conv2d_fwd_cpp, TestConv2D_3x3_padding_4_NXC) {
    check_conv_correctness_and_tuning_fwd(-1, 256, 64, -1, -1, 3, 3, {1, 1},
            {1, 1}, false, false, false,
            /*real_N*/ 8, /*real_H*/ 67, /*real_W*/ 56);
}

TEST(GCCore_CPU_dynamic_conv2d_fwd_cpp, TestConv2D_3x3_padding_5_NXC) {
    check_conv_correctness_and_tuning_fwd(-1, 256, 64, -1, -1, 3, 3, {1, 1},
            {2, 2}, false, false, false,
            /*real_N*/ 8, /*real_H*/ 56, /*real_W*/ 56);
}

TEST(GCCore_CPU_dynamic_conv2d_fwd_cpp, TestConv2D_3x3_padding_6_NXC) {
    check_conv_correctness_and_tuning_fwd(-1, 256, 64, -1, -1, 3, 3, {1, 1},
            {1, 1}, false, false, false,
            /*real_N*/ 8, /*real_H*/ 7, /*real_W*/ 7);
}

TEST(GCCore_CPU_dynamic_conv2d_fwd_cpp, TestConv2D_3x3_padding_7_NXC) {
    check_conv_correctness_and_tuning_fwd(-1, 256, 64, -1, -1, 3, 3, {2, 2},
            {1, 1}, false, false, false,
            /*real_N*/ 8, /*real_H*/ 20, /*real_W*/ 20);
}

TEST(GCCore_CPU_dynamic_conv2d_fwd_cpp, TestConv2D_3x3_1_NXC_fuse) {
    check_conv_correctness_and_tuning_fwd(-1, 256, 64, 58, 58, 3, 3, {1, 1},
            {0, 0}, true, true, true,
            /*real_N*/ 1, /*real_H*/ 58, /*real_W*/ 58);
}

TEST(GCCore_CPU_dynamic_conv2d_fwd_cpp, TestConv2D_3x3_2_NXC_fuse) {
    check_conv_correctness_and_tuning_fwd(-1, 256, 64, -1, -1, 3, 3, {1, 1},
            {0, 0}, true, true, true,
            /*real_N*/ 1, /*real_H*/ 58, /*real_W*/ 58);
}

TEST(GCCore_CPU_dynamic_conv2d_fwd_cpp, TestConv2D_3x3_3_NXC_fuse) {
    check_conv_correctness_and_tuning_fwd(-1, 256, 64, -1, -1, 3, 3, {1, 1},
            {0, 0}, true, true, true,
            /*real_N*/ 8, /*real_H*/ 69, /*real_W*/ 69);
}

TEST(GCCore_CPU_dynamic_conv2d_fwd_cpp, TestConv2D_3x3_4_NXC_fuse) {
    check_conv_correctness_and_tuning_fwd(-1, 256, 64, -1, -1, 3, 3, {1, 1},
            {0, 0}, true, true, true,
            /*real_N*/ 1, /*real_H*/ 9, /*real_W*/ 9);
}

TEST(GCCore_CPU_dynamic_conv2d_fwd_cpp, TestConv2D_3x3_5_NXC_fuse) {
    check_conv_correctness_and_tuning_fwd(-1, 256, 64, -1, -1, 3, 3, {1, 1},
            {0, 0}, true, true, true,
            /*real_N*/ 1, /*real_H*/ 6, /*real_W*/ 6);
}

TEST(GCCore_CPU_dynamic_conv2d_fwd_cpp, TestConv2D_3x3_6_NXC_fuse) {
    check_conv_correctness_and_tuning_fwd(-1, 256, 64, 58, 58, 3, 3, {2, 2},
            {0, 0}, true, true, true,
            /*real_N*/ 1, /*real_H*/ 58, /*real_W*/ 58);
}

TEST(GCCore_CPU_dynamic_conv2d_fwd_cpp, TestConv2D_3x3_7_NXC_fuse) {
    check_conv_correctness_and_tuning_fwd(-1, 256, 64, -1, -1, 3, 3, {2, 2},
            {0, 0}, true, true, true,
            /*real_N*/ 8, /*real_H*/ 69, /*real_W*/ 69);
}

TEST(GCCore_CPU_dynamic_conv2d_fwd_cpp, TestConv2D_3x3_8_NXC_fuse) {
    check_conv_correctness_and_tuning_fwd(-1, 256, 64, -1, -1, 3, 3, {3, 2},
            {0, 0}, true, true, true,
            /*real_N*/ 8, /*real_H*/ 69, /*real_W*/ 69);
}

TEST(GCCore_CPU_dynamic_conv2d_fwd_cpp, TestConv2D_3x3_padding_1_NXC_fuse) {
    check_conv_correctness_and_tuning_fwd(-1, 256, 64, 56, 56, 3, 3, {1, 1},
            {1, 1}, true, true, true,
            /*real_N*/ 8, /*real_H*/ 56, /*real_W*/ 56);
}

TEST(GCCore_CPU_dynamic_conv2d_fwd_cpp, TestConv2D_3x3_padding_2_NXC_fuse) {
    check_conv_correctness_and_tuning_fwd(-1, 256, 64, -1, -1, 3, 3, {1, 1},
            {1, 1}, true, true, true,
            /*real_N*/ 8, /*real_H*/ 56, /*real_W*/ 56);
}

TEST(GCCore_CPU_dynamic_conv2d_fwd_cpp, TestConv2D_3x3_padding_3_NXC_fuse) {
    check_conv_correctness_and_tuning_fwd(-1, 256, 64, -1, -1, 3, 3, {1, 1},
            {1, 1}, true, true, true,
            /*real_N*/ 8, /*real_H*/ 67, /*real_W*/ 56);
}

TEST(GCCore_CPU_dynamic_conv2d_fwd_cpp, TestConv2D_3x3_padding_4_NXC_fuse) {
    check_conv_correctness_and_tuning_fwd(-1, 256, 64, -1, -1, 3, 3, {1, 1},
            {1, 1}, true, true, true,
            /*real_N*/ 8, /*real_H*/ 67, /*real_W*/ 56);
}

TEST(GCCore_CPU_dynamic_conv2d_fwd_cpp, TestConv2D_3x3_padding_5_NXC_fuse) {
    check_conv_correctness_and_tuning_fwd(-1, 256, 64, -1, -1, 3, 3, {1, 1},
            {2, 2}, true, true, true,
            /*real_N*/ 8, /*real_H*/ 56, /*real_W*/ 56);
}

TEST(GCCore_CPU_dynamic_conv2d_fwd_cpp, TestConv2D_3x3_padding_6_NXC_fuse) {
    check_conv_correctness_and_tuning_fwd(-1, 256, 64, -1, -1, 3, 3, {1, 1},
            {1, 1}, true, true, true,
            /*real_N*/ 8, /*real_H*/ 7, /*real_W*/ 7);
}

TEST(GCCore_CPU_dynamic_conv2d_fwd_cpp, TestConv2D_3x3_padding_7_NXC_fuse) {
    check_conv_correctness_and_tuning_fwd(-1, 256, 64, -1, -1, 3, 3, {2, 2},
            {1, 1}, true, true, true,
            /*real_N*/ 8, /*real_H*/ 20, /*real_W*/ 20);
}

TEST(GCCore_CPU_dynamic_conv2d_fwd_cpp, TestConv2D_3x3_padding_8_NXC_fuse) {
    check_conv_correctness_and_tuning_fwd(-1, 256, 256, 12, 12, 3, 3, {3, 3},
            {1, 1}, true, true, true,
            /*real_N*/ 1, /*real_H*/ 12, /*real_W*/ 12);
}

TEST(GCCore_CPU_dynamic_conv2d_fwd_cpp, TestConv2D_1x1_7_NXC_stride2_fuse) {
    check_conv_correctness_and_tuning_fwd(-1, 256, 64, 2, 2, 1, 1, {2, 2},
            {0, 0}, true, true, true,
            /*real_N*/ 1, /*real_H*/ 2, /*real_W*/ 2);
}

TEST(GCCore_CPU_dynamic_conv2d_fwd_cpp,
        TestConv2D_3x3_padding_large_padding_1) {
    check_conv_correctness_and_tuning_fwd(-1, 64, 64, -1, -1, 3, 3, {1, 1},
            {5, 5}, false, false, false,
            /*real_N*/ 1, /*real_H*/ 56, /*real_W*/ 56);
}

TEST(GCCore_CPU_dynamic_conv2d_fwd_cpp,
        TestConv2D_3x3_padding_large_padding_2) {
    check_conv_correctness_and_tuning_fwd(-1, 64, 64, -1, -1, 3, 3, {1, 1},
            {6, 6}, false, false, false,
            /*real_N*/ 1, /*real_H*/ 56, /*real_W*/ 56);
}

TEST(GCCore_CPU_dynamic_conv2d_fwd_cpp,
        TestConv2D_3x3_padding_large_padding_3) {
    check_conv_correctness_and_tuning_fwd(-1, 64, 64, -1, -1, 3, 3, {2, 2},
            {6, 6}, false, false, false,
            /*real_N*/ 1, /*real_H*/ 56, /*real_W*/ 56);
}

TEST(GCCore_CPU_dynamic_conv2d_fwd_cpp,
        TestConv2D_3x3_padding_large_padding_4) {
    check_conv_correctness_and_tuning_fwd(-1, 256, 256, 12, 12, 3, 3, {3, 3},
            {6, 6}, true, true, true,
            /*real_N*/ 1, /*real_H*/ 12, /*real_W*/ 12);
}

TEST(GCCore_CPU_dynamic_conv2d_fwd_cpp,
        TestConv2D_3x3_padding_large_padding_5) {
    check_conv_correctness_and_tuning_fwd(-1, 256, 64, -1, -1, 3, 3, {2, 2},
            {4, 4}, true, true, true,
            /*real_N*/ 8, /*real_H*/ 20, /*real_W*/ 20);
}

TEST(GCCore_CPU_dynamic_conv2d_fwd_cpp,
        TestConv2D_3x3_padding_large_padding_6) {
    check_conv_correctness_and_tuning_fwd(-1, 256, 64, -1, -1, 3, 3, {2, 2},
            {100, 100}, true, true, true,
            /*real_N*/ 8, /*real_H*/ 20, /*real_W*/ 20);
}
