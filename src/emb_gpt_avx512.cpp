// Copyright (C) 2018-2023 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include <string>
#include <vector>
#include <memory.h>

#include "common/simple_parallel.hpp"
#include "common/utility.hpp"
#include "utility_kernel_avx512.hpp"
#include "transpose_kernel_avx512.hpp"
#include "llm_emb_gpt.hpp"
#include "emb_gpt_avx512.hpp"
#include "rotary_kernel_avx512.hpp"

using namespace ov::cpu;

namespace llmdnn {

struct emb_gpt_impl_avx512 : public emb_gpt::impl {
    bool create(const emb_gpt::create_param& param) override;
    void exec(const emb_gpt::exec_param& param) override;

    void memcpyPastKV(uint8_t** pastk_src, uint8_t** pastv_src, uint8_t** pastk_dst, uint8_t** pastv_dst,
        size_t batch, size_t past_seq_len, size_t head_stride_in_kv);
    void applyRotaryPosEmbMemcpy(uint8_t* q_src, uint8_t* k_src, uint8_t* v_src, size_t ldq, size_t ldk, size_t ldv, uint8_t* q_dst, uint8_t** k_dst, uint8_t** v_dst,
        size_t batch, size_t q_seq_len, size_t past_seq_len, size_t head_stride_in_kv, float* cos, float* sin);
    void applyRotaryPosEmbMemcpyWithPosition2d(uint8_t* q_src, uint8_t* k_src, uint8_t* v_src, size_t ldq, size_t ldk, size_t ldv, uint8_t* q_dst, uint8_t** k_dst, uint8_t** v_dst,
        size_t batch, size_t q_seq_len, size_t past_seq_len, int* position2d_ids, size_t head_stride_in_kv, float* cos, float* sin);

    emb_gpt::create_param _create_param;
    size_t _head_num = 32;
    size_t _size_per_head = 80;
    size_t _hidden_size = 32 * 80;
    size_t _rotary_dim = 20;
    // aligned to cache line
    size_t _size_per_head_aligned = 80;
    int64_t _input_type_size = 1;
    int64_t _output_type_size = 1;
    bool _use_position2d = false;
};

bool emb_gpt_impl_avx512::create(const emb_gpt::create_param& param) {
    if (param.qkv_precision != dnnl_bf16) {
        std::cout << "input precision must be bf16 or int8.\n";
        return false;
    }
    // TODO: support s8
    // if (param.dst_precision != dnnl_bf16 && param.dst_precision != dnnl_s8) {
    //     std::cout << "dst precision must be bf16 or int8.\n";
    //     return false;
    // }
    _create_param = param;

    _head_num = param.num_heads;
    _size_per_head = param.head_size;
    _size_per_head_aligned = param.head_size_aligned;
    _hidden_size = param.head_size * param.num_heads;
    _rotary_dim = param.rotary_dims;
    _input_type_size = sizeof(ov::bfloat16);
    _output_type_size = sizeof(ov::bfloat16);
    if (param.dst_precision == dnnl_s8)
        _output_type_size = sizeof(int8_t);

    _use_position2d = param.use_position2d;

    return true;
}

void emb_gpt_impl_avx512::memcpyPastKV(uint8_t** pastk_src, uint8_t** pastv_src, uint8_t** pastk_dst, uint8_t** pastv_dst,
        size_t batch, size_t past_seq_len, size_t head_stride_in_kv) {
    parallel_for3d(batch, _head_num, past_seq_len, [&](size_t b, size_t h, size_t s) {
        auto k_dst_batch = pastk_dst[b];
        auto v_dst_batch = pastv_dst[b];
        auto k_src_batch = pastk_src[b];
        auto v_src_batch = pastv_src[b];
        auto k_dst_seq = k_dst_batch + s * _size_per_head_aligned * _output_type_size;
        auto v_dst_seq = v_dst_batch + s * _size_per_head_aligned * _output_type_size;
        auto k_src_seq = k_src_batch + s * _size_per_head_aligned * _output_type_size;
        auto v_src_seq = v_src_batch + s * _size_per_head_aligned * _output_type_size;
        auto* k_src_f = k_src_seq + h * past_seq_len * _size_per_head_aligned * _output_type_size;
        auto* k_dst_f = k_dst_seq + h * head_stride_in_kv * _output_type_size;
        auto* v_src_f = v_src_seq + h * past_seq_len * _size_per_head_aligned * _output_type_size;
        auto* v_dst_f = v_dst_seq + h * head_stride_in_kv * _output_type_size;

        memcpy(k_dst_f, k_src_f, _output_type_size * _size_per_head);
        memcpy(v_dst_f, v_src_f, _output_type_size * _size_per_head);
    });
}

// q_src shape: [batch, q_seq_len, num_attention_heads, 3 * head_size]
// q_dst shape: [batch, num_attention_heads, q_seq_len, head_size_aligned]
// kv_src shape: [batch, q_seq_len, num_attention_heads, 3 * head_size]
// kv_dst shape: [batch, num_attention_heads, q_seq_len+past_seq_len, head_size_aligned]
void emb_gpt_impl_avx512::applyRotaryPosEmbMemcpy(uint8_t* q_src, uint8_t* k_src, uint8_t* v_src, size_t ldq, size_t ldk, size_t ldv, uint8_t* q_dst, uint8_t** k_dst, uint8_t** v_dst,
    size_t batch, size_t q_seq_len, size_t past_seq_len, size_t head_stride_in_kv, float* cos, float* sin) {
    auto key_offset = _output_type_size * past_seq_len * _size_per_head_aligned;
    auto* cos_cached = cos + past_seq_len * _rotary_dim;
    auto* sin_cached = sin + past_seq_len * _rotary_dim;
    parallel_for3d(batch, _head_num, q_seq_len, [&](size_t b, size_t h, size_t s) {
        // q, k rotary encoding
        auto q_dst_batch = q_dst + b * _head_num * q_seq_len * _size_per_head_aligned * _output_type_size;
        auto k_dst_batch = k_dst[b] + key_offset;
        auto v_dst_batch = v_dst[b] + key_offset;
        auto q_src_batch = q_src + b * _head_num * ldq * q_seq_len * _input_type_size;
        auto k_src_batch = k_src + b * _head_num * ldk * q_seq_len * _input_type_size;
        auto v_src_batch = v_src + b * _head_num * ldv * q_seq_len * _input_type_size;
        auto q_dst_seq = q_dst_batch + s * _size_per_head_aligned * _output_type_size;
        auto k_dst_seq = k_dst_batch + s * _size_per_head_aligned * _output_type_size;
        auto v_dst_seq = v_dst_batch + s * _size_per_head_aligned * _output_type_size;
        auto q_src_seq = q_src_batch + s * _head_num * ldq * _input_type_size;
        auto k_src_seq = k_src_batch + s * _head_num * ldk * _input_type_size;
        auto v_src_seq = v_src_batch + s * _head_num * ldv * _input_type_size;
        auto* q_src_f = reinterpret_cast<ov::bfloat16*>(q_src_seq + h * ldq * _input_type_size);
        auto* k_src_f = reinterpret_cast<ov::bfloat16*>(k_src_seq + h * ldk * _input_type_size);
        auto* q_dst_f = reinterpret_cast<ov::bfloat16*>(q_dst_seq + h * q_seq_len * _size_per_head_aligned * _output_type_size);
        auto* k_dst_f = reinterpret_cast<ov::bfloat16*>(k_dst_seq + h * head_stride_in_kv * _output_type_size);
        rotary_avx512(_rotary_dim, cos_cached + s * _rotary_dim, sin_cached + s * _rotary_dim, q_src_f, k_src_f, q_dst_f, k_dst_f);

        // q, k concat
        memcpy(reinterpret_cast<uint8_t*>(q_dst_f) + _rotary_dim * _output_type_size, reinterpret_cast<uint8_t*>(q_src_f) + _rotary_dim * _input_type_size, _output_type_size * (_size_per_head - _rotary_dim));
        memcpy(reinterpret_cast<uint8_t*>(k_dst_f) + _rotary_dim * _output_type_size, reinterpret_cast<uint8_t*>(k_src_f) + _rotary_dim * _input_type_size, _output_type_size * (_size_per_head - _rotary_dim));
        // v concat
        memcpy(static_cast<uint8_t*>(v_dst_seq) + h * head_stride_in_kv * _output_type_size,
            static_cast<uint8_t*>(v_src_seq) + h * ldv * _input_type_size,
            _size_per_head * _output_type_size);
    });
}

// q_src shape: [batch, q_seq_len, num_attention_heads, 3 * head_size]
// q_dst shape: [batch, num_attention_heads, q_seq_len, head_size_aligned]
// kv_src shape: [batch, q_seq_len, num_attention_heads, 3 * head_size]
// kv_dst shape: [batch, num_attention_heads, q_seq_len+past_seq_len, head_size_aligned]
// position2d_ids: [batch, 2, q_seq_len]
void emb_gpt_impl_avx512::applyRotaryPosEmbMemcpyWithPosition2d(uint8_t* q_src, uint8_t* k_src, uint8_t* v_src, size_t ldq, size_t ldk, size_t ldv, uint8_t* q_dst, uint8_t** k_dst, uint8_t** v_dst,
    size_t batch, size_t q_seq_len, size_t past_seq_len, int* position2d_ids, size_t head_stride_in_kv, float* cos, float* sin) {
    auto key_offset = _output_type_size * past_seq_len * _size_per_head_aligned;
    auto* cos_cached = cos;
    auto* sin_cached = sin;
    parallel_for3d(batch, _head_num, q_seq_len, [&](size_t b, size_t h, size_t s) {
        // q, k rotary encoding
        auto q_dst_batch = q_dst + b * _head_num * q_seq_len * _size_per_head_aligned * _output_type_size;
        auto k_dst_batch = k_dst[b] + key_offset;
        auto v_dst_batch = v_dst[b] + key_offset;
        auto pos_batch = position2d_ids + b * 2 * q_seq_len;
        auto block_batch = pos_batch + q_seq_len;
        auto q_src_batch = q_src + b * _head_num * ldq * q_seq_len * _input_type_size;
        auto k_src_batch = k_src + b * _head_num * ldk * q_seq_len * _input_type_size;
        auto v_src_batch = v_src + b * _head_num * ldv * q_seq_len * _input_type_size;
        auto q_dst_seq = q_dst_batch + s * _size_per_head_aligned * _output_type_size;
        auto k_dst_seq = k_dst_batch + s * _size_per_head_aligned * _output_type_size;
        auto v_dst_seq = v_dst_batch + s * _size_per_head_aligned * _output_type_size;
        auto q_src_seq = q_src_batch + s * _head_num * ldq * _input_type_size;
        auto k_src_seq = k_src_batch + s * _head_num * ldk * _input_type_size;
        auto v_src_seq = v_src_batch + s * _head_num * ldv * _input_type_size;
        auto* q_src_f = reinterpret_cast<ov::bfloat16*>(q_src_seq + h * ldq * _input_type_size);
        auto* k_src_f = reinterpret_cast<ov::bfloat16*>(k_src_seq + h * ldk * _input_type_size);
        auto* q_dst_f = reinterpret_cast<ov::bfloat16*>(q_dst_seq + h * q_seq_len * _size_per_head_aligned * _output_type_size);
        auto* k_dst_f = reinterpret_cast<ov::bfloat16*>(k_dst_seq + h * head_stride_in_kv * _output_type_size);
        rotary_avx512(_rotary_dim, cos_cached + pos_batch[s] * _rotary_dim, sin_cached + pos_batch[s] * _rotary_dim, q_src_f, k_src_f, q_dst_f, k_dst_f);
        rotary_avx512(_rotary_dim, cos_cached + block_batch[s] * _rotary_dim, sin_cached + block_batch[s] * _rotary_dim,
            q_src_f + _rotary_dim,
            k_src_f + _rotary_dim,
            q_dst_f + _rotary_dim,
            k_dst_f + _rotary_dim);

        // v concat
        memcpy(static_cast<uint8_t*>(v_dst_seq) + h * head_stride_in_kv * _output_type_size,
            static_cast<uint8_t*>(v_src_seq) + h * ldv* _input_type_size,
            _size_per_head * _output_type_size);
    });
}

void emb_gpt_impl_avx512::exec(const emb_gpt::exec_param& param) {
    // [batch, seq_len, (num_heads * 3 * head_size)]
    //   --> [batch, seq_len, num_heads, 3 * head_size]
    auto query = param.q;
    auto key = param.k;
    auto value = param.v;
    auto query_dst = param.query_dst;
    auto key_dst = param.layer_past_key_dst;
    auto value_dst = param.layer_past_value_dst;
    auto batch = param.batch;
    auto query_seq_len = param.query_seq_len;
    auto past_seq_len = param.past_seq_len;
    auto head_stride_in_kv = param.head_stride_in_kv;

    // past kv src != dst, copy src to dst first
    if (param.layer_past_key_src && param.layer_past_key_src[0] != param.layer_past_key_dst[0] && past_seq_len)
        memcpyPastKV(param.layer_past_key_src, param.layer_past_value_src, param.layer_past_key_dst, param.layer_past_value_dst, batch, past_seq_len, head_stride_in_kv);

    // transpose + rotary embbeding:
    // transpose: [batch, seq_len, num_attention_heads, 3 * head_size] -->
    //          3 [batch, num_attention_heads, seq_len, head_size]
    // rotary embbeding: part of key will write to past_key, part of query will write to tempory buffer
    if (_create_param.dst_precision == dnnl_s8) {
        // query pass part(temp buffer): query = torch.cat((query, query_pass), dim=-1)
        // key pass part(past_key): key = torch.cat((key, key_pass), dim=-1)
        // value(pastKeys): value = torch.cat((past_value, value), dim=-2)
        // applyRotaryPosEmbMemcpyQuant(query, key, queryTranspose.get(), current_k_bufs, _output_type_size * new_seq_offset * _size_per_head_aligned,
        //     _cos_cached.get(), _sin_cached.get(), batch, seq_len, new_seq_offset, value, current_v_bufs);
        assert(false);
    } else {
        // query pass part(temp buffer): query = torch.cat((query, query_pass), dim=-1)
        // key pass part(past_key): key = torch.cat((key, key_pass), dim=-1)
        // value(pastKeys): value = torch.cat((past_value, value), dim=-2)
        // q_dst shape: [batch, num_attention_heads, q_seq_len, head_size_aligned]
        // kv_dst shape: [batch, num_attention_heads, q_seq_len+past_seq_len, head_size_aligned]
        if (_use_position2d) {
            applyRotaryPosEmbMemcpyWithPosition2d(query, key, value, param.ldq, param.ldk, param.ldv, query_dst, key_dst, value_dst, batch, query_seq_len, past_seq_len,
                param.position2d_ids, head_stride_in_kv, param.cos, param.sin);
        } else {
            applyRotaryPosEmbMemcpy(query, key, value, param.ldq, param.ldk, param.ldv, query_dst, key_dst, value_dst, batch, query_seq_len, past_seq_len, head_stride_in_kv,
                param.cos, param.sin);
        }
    }
}

emb_gpt::impl* new_impl_avx512() {
    return new emb_gpt_impl_avx512();
}

}