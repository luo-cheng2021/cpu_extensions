// Copyright (C) 2018-2023 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include <cstdint>
#include <string>
#include <vector>

#include "common/simple_parallel.hpp"
#include "common/tensor2d.hpp"
#include "common/utility.hpp"
#include "utility_kernel_avx512.hpp"
#include "mm_kernel_common_amx.hpp"
#include "softmax_kernel_avx512.hpp"
#include "transpose_kernel_avx512.hpp"
#include "llm_mha_gpt.hpp"
#include "mha_gpt_amx.hpp"

using namespace ov::cpu;

namespace llmdnn {

struct mha_gpt_impl_amx : public mha_gpt::impl {
    bool create(const mha_gpt::create_param& param) override;
    void exec(const mha_gpt::exec_param& param) override;

    mha_gpt::create_param _create_param;

    void mha_bf16(const mha_gpt::exec_param &param);
    void mha_i8(const mha_gpt::exec_param &param);

    size_t head_size_aligned;
    size_t bufferMatMul0OutSize;
    size_t bufferMatMul1OutSize;

    std::shared_ptr<uint8_t> bufferMatMul0Out;
    std::shared_ptr<uint8_t> bufferMatMul1Out;
    std::shared_ptr<float> qkvQuantBuf;

    std::vector<std::shared_ptr<amx_kernel::MatmulVector<ov::bfloat16, ov::bfloat16>>> gemAvB_BF16xBF16;
    std::vector<std::shared_ptr<amx_kernel::Matmul<ov::bfloat16, ov::bfloat16>>> qKtrGemm_BF16xBF16;
    std::vector<std::shared_ptr<amx_kernel::Matmul<ov::bfloat16, ov::bfloat16>>> qKVGemm_BF16xBF16;

    std::vector<std::shared_ptr<amx_kernel::Matmul<int8_t, int8_t>>> qKtrGemm_i8xi8;
    std::vector<std::shared_ptr<amx_kernel::Matmul<uint8_t, int8_t>>> qKVGemm_u8xi8;
    std::vector<std::shared_ptr<amx_kernel::MatmulVector<int8_t, int8_t>>> gemAvB_i8xi8;
};

bool mha_gpt_impl_amx::create(const mha_gpt::create_param& param) {
    if (param.qkv_precision != dnnl_bf16 && param.qkv_precision != dnnl_s8) {
        std::cout << "input precision must be bf16 or int8.\n";
        return false;
    }
    if (param.dst_precision != dnnl_bf16 && param.dst_precision != dnnl_s8) {
        std::cout << "dst precision must be bf16 or int8.\n";
        return false;
    }
    _create_param = param;

    // q: [batch, num_heads, query_seq_len, head_size]
    // k: [batch, num_heads, maxSeqLen(valid: key_seq_len), head_size]
    // v: [batch, num_heads, maxSeqLen(valid: value_seq_len), head_size]
    // attention_mask: [batch, 1, 1, maxSeqLen(valid: key_seq_len)]
    // matmul1: [batch, num_heads, query_seq_len, head_size]
    // attn_output: [batch, query_seq_len, num_heads * head_size]
    size_t numThreads = getTotalThreads();
    if (_create_param.qkv_precision == dnnl_s8) {
        head_size_aligned = rndup(_create_param.head_size, 64);
        qKtrGemm_i8xi8.resize(numThreads);
        for (size_t i = 0; i < numThreads; i++) {
            qKtrGemm_i8xi8[i] = std::make_shared<amx_kernel::Matmul<int8_t, int8_t>>(false, !param.is_bloom);
        }
        qKVGemm_u8xi8.resize(numThreads);
        for (size_t i = 0; i < numThreads; i++) {
            qKVGemm_u8xi8[i] = std::make_shared<amx_kernel::Matmul<uint8_t, int8_t>>(false, false);
        }
        gemAvB_i8xi8.resize(numThreads);
        for (size_t i = 0; i < numThreads; i++) {
            gemAvB_i8xi8[i] = std::make_shared<amx_kernel::MatmulVector<int8_t, int8_t>>();
        }
        qkvQuantBuf = std::shared_ptr<float>(
                            reinterpret_cast<float*>(aligned_alloc(64, param.head_size * sizeof(float))),
                            [](void * p) { ::free(p); });
        memset(qkvQuantBuf.get(), 0, sizeof(param.head_size * sizeof(float)));
    } else {
        head_size_aligned = rndup(_create_param.head_size, 32);
        gemAvB_BF16xBF16.resize(numThreads);
        for (size_t i = 0; i < numThreads; i++) {
            gemAvB_BF16xBF16[i] = std::make_shared<amx_kernel::MatmulVector<ov::bfloat16, ov::bfloat16>>();
        }
        qKtrGemm_BF16xBF16.resize(numThreads);
        for (size_t i = 0; i < numThreads; i++) {
            qKtrGemm_BF16xBF16[i] = std::make_shared<amx_kernel::Matmul<ov::bfloat16, ov::bfloat16>>(false, !param.is_bloom);
        }
        qKVGemm_BF16xBF16.resize(numThreads);
        for (size_t i = 0; i < numThreads; i++) {
            qKVGemm_BF16xBF16[i] = std::make_shared<amx_kernel::Matmul<ov::bfloat16, ov::bfloat16>>(false, false);
        }
    }

    bufferMatMul0OutSize = _create_param.max_seq_len * rndup(_create_param.max_seq_len * sizeof(float), 64);
    bufferMatMul1OutSize = _create_param.max_seq_len * head_size_aligned * sizeof(float);

    bufferMatMul0Out = std::shared_ptr<uint8_t>(
                            reinterpret_cast<uint8_t*>(aligned_alloc(64, numThreads * bufferMatMul0OutSize)),
                            [](void * p) { ::free(p); });
    memset(bufferMatMul0Out.get(), 0, numThreads * bufferMatMul0OutSize);
    bufferMatMul1Out = std::shared_ptr<uint8_t>(
                            reinterpret_cast<uint8_t*>(aligned_alloc(64, numThreads * bufferMatMul1OutSize)),
                            [](void * p) { ::free(p); });
    memset(bufferMatMul1Out.get(), 0, numThreads * bufferMatMul1OutSize);
    return true;
}

void mha_gpt_impl_amx::mha_bf16(const mha_gpt::exec_param &param) {
    auto& q = param.q;
    auto& k = param.k;
    auto& v = param.v;
    auto* attn_masks = param.attention_mask.data<float>();
    uint8_t* pout = param.attn_output.data<uint8_t>();
    auto alibi = param.alibi.data<float>();

    auto outPrcSize = get_precision_size(_create_param.qkv_precision);
    auto& gemAvB_ops = gemAvB_BF16xBF16;
    auto& qKtrGemm_ops = qKtrGemm_BF16xBF16;
    auto& qKVGemm_ops = qKVGemm_BF16xBF16;
    bool is_vector = param.query_seq_len == 1 && _create_param.head_size >= 32 && _create_param.head_size <= 32 * 6 && !_create_param.is_bloom;
    size_t head_stride_in_attn = _create_param.head_size;
    size_t batch_stride_in_attn = _create_param.head_size * _create_param.num_heads * param.query_seq_len;
    size_t causal_mask_offset_start = param.key_seq_len - param.query_seq_len;

    if (is_vector) {
        parallel_for2d(param.batch, _create_param.num_heads, [&](size_t threadNum, size_t i0, size_t i1) {
            auto pQIn0_aux = &q.at<uint8_t>({i0, i1});
            auto pKIn0_aux = &k.at<uint8_t>({i0, i1});
            auto pVIn0_aux = &v.at<uint8_t>({i0, i1});

            auto pAddIn1_aux = attn_masks + i0 * param.key_seq_len;

            auto bufferMatMul0Out_local = reinterpret_cast<uint8_t*>(bufferMatMul0Out.get() + threadNum * bufferMatMul0OutSize);
            auto bufferMatMul1Out_local = reinterpret_cast<uint8_t*>(bufferMatMul1Out.get() + threadNum * bufferMatMul1OutSize);

            tensor2D<ov::bfloat16> matK(param.key_seq_len, _create_param.head_size, reinterpret_cast<ov::bfloat16*>(pKIn0_aux), k.m_strides[2]);
            // N: key_seq_len, K: head_size
            // q[1, K] * transpose(k[N, K])        ==>
            //     k[N, K] * transpose(q[1, K])    ==>
            //     k[N, K] * q[K, 1]
            (*gemAvB_ops[threadNum])(matK, reinterpret_cast<ov::bfloat16*>(pQIn0_aux), reinterpret_cast<float*>(bufferMatMul0Out_local));

            float* pMatMul0Out = reinterpret_cast<float*>(bufferMatMul0Out_local);
            mul_add_f32_avx512(pMatMul0Out, pMatMul0Out, _create_param.normal_factor, pAddIn1_aux, param.key_seq_len);
            softmax_avx512<ov::bfloat16>(reinterpret_cast<ov::bfloat16*>(pMatMul0Out), pMatMul0Out, param.key_seq_len, nullptr);
            auto pOut_aux = pout + (i0 * batch_stride_in_attn + i1 * head_stride_in_attn) * outPrcSize;
            tensor2D<ov::bfloat16> matQK(param.query_seq_len, param.key_seq_len, reinterpret_cast<ov::bfloat16*>(bufferMatMul0Out_local), rndup(param.key_seq_len * sizeof(ov::bfloat16), 64));
            tensor2D<ov::bfloat16> matV(param.key_seq_len, _create_param.head_size, reinterpret_cast<ov::bfloat16*>(pVIn0_aux), v.m_strides[2]);
            tensor2D<float> matQKV(param.query_seq_len, _create_param.head_size, reinterpret_cast<float*>(bufferMatMul1Out_local), head_size_aligned * sizeof(float));
            amx_kernel::PP::BiasGeluStore<float, amx_kernel::PP::Steps::NONE> pp(matQKV);
            (*qKVGemm_ops[threadNum])(matQK, matV, 0, _create_param.head_size, pp);
            memcpy2d_stride_avx512<ov::bfloat16>(reinterpret_cast<ov::bfloat16*>(pOut_aux), reinterpret_cast<float*>(bufferMatMul1Out_local), param.query_seq_len,
                _create_param.head_size, head_size_aligned * sizeof(float), _create_param.num_heads * _create_param.head_size * sizeof(ov::bfloat16), nullptr);
        });
    } else {
        auto numThreads = getTotalThreads();
        int seq_cout_all = rndup(param.query_seq_len, 32) / 32;
        int work_amount = param.batch * _create_param.num_heads * seq_cout_all;
        parallel_for(numThreads, [&](int threadNum) {
            int i0;
            int i1;
            int seq;
            int start {0}, end {0};
            splitter(work_amount, static_cast<int>(numThreads), threadNum, start, end);
            if (start >= work_amount) return;

            parallel_it_init(start, i0, param.batch, i1, _create_param.num_heads, seq, seq_cout_all);
            uint8_t* prev_k = nullptr;
            uint8_t* prev_v = nullptr;
            for (int iwork = start; iwork < end; ++iwork) {
                int seq_start = seq * 32;
                int seq_end = std::min(static_cast<size_t>(seq_start) + 32, param.query_seq_len);
                int seq_cout = seq_end - seq_start;
                // q: [batch, num_heads, query_seq_len, head_size]
                // k: [batch, num_heads, key_seq_len, head_size]
                // v: [batch, num_heads, value_seq_len, head_size]
                auto pQIn0_aux = &q.at<uint8_t>({static_cast<size_t>(i0), static_cast<size_t>(i1), static_cast<size_t>(seq_start)});
                auto pKIn0_aux = &k.at<uint8_t>({static_cast<size_t>(i0), static_cast<size_t>(i1)});
                auto pVIn0_aux = &v.at<uint8_t>({static_cast<size_t>(i0), static_cast<size_t>(i1)});

                auto bufferMatMul0Out_local = reinterpret_cast<uint8_t*>(bufferMatMul0Out.get() + threadNum * bufferMatMul0OutSize);
                auto bufferMatMul1Out_local = reinterpret_cast<uint8_t*>(bufferMatMul1Out.get() + threadNum * bufferMatMul1OutSize);
                
                tensor2D<ov::bfloat16> matQ(seq_cout, _create_param.head_size, reinterpret_cast<ov::bfloat16*>(pQIn0_aux), q.m_strides[2]);
                tensor2D<float> matQK(seq_cout, param.key_seq_len, reinterpret_cast<float*>(bufferMatMul0Out_local), rndup(param.key_seq_len * sizeof(float), 64));
                amx_kernel::PP::BiasGeluStore<float, amx_kernel::PP::Steps::NONE> pp(matQK);
                if (!_create_param.is_bloom) {
                    tensor2D<ov::bfloat16> matK(param.key_seq_len, _create_param.head_size, reinterpret_cast<ov::bfloat16*>(pKIn0_aux), k.m_strides[2]);
                    (*qKtrGemm_ops[threadNum])(matQ, matK, 0, param.key_seq_len, pp, pKIn0_aux == prev_k);
                } else {
                    tensor2D<ov::bfloat16> matK(_create_param.head_size, param.key_seq_len, reinterpret_cast<ov::bfloat16*>(pKIn0_aux), k.m_strides[3]);
                    (*qKtrGemm_ops[threadNum])(matQ, matK, 0, param.key_seq_len, pp, pKIn0_aux == prev_k);
                }
                prev_k = pKIn0_aux;

                auto pMatMul0Out = bufferMatMul0Out_local;
                if (param.is_causal_in_attention) {
                    auto pAddIn1_aux = attn_masks + i0 * param.key_seq_len * param.query_seq_len;
                    // loop along K dimension
                    for (int m = 0; m < seq_cout; m++) {
                        float* src = reinterpret_cast<float*>(pMatMul0Out + m * rndup(param.key_seq_len * sizeof(float), 64));
                        ov::bfloat16* dst = reinterpret_cast<ov::bfloat16*>(pMatMul0Out + m * rndup(param.key_seq_len * sizeof(ov::bfloat16), 64));
                        if (!_create_param.is_bloom)
                            mul_add_f32_avx512(src, src, _create_param.normal_factor, pAddIn1_aux + (m + seq_start) * param.key_seq_len, param.key_seq_len);
                        else
                            // alibi shape: [batch, head_num, 1, key_seq_len]
                            mul_add2_f32_avx512(src, src, _create_param.normal_factor,
                                alibi + i0 * _create_param.num_heads * param.key_seq_len + i1 * param.key_seq_len,
                                pAddIn1_aux + (m + seq_start) * param.key_seq_len,
                                param.key_seq_len);
                        softmax_avx512<ov::bfloat16>(dst, src, param.key_seq_len, nullptr);
                    }
                } else {
                    auto pAddIn1_aux = attn_masks + i0 * param.key_seq_len;
                    // loop along K dimension
                    size_t valid_softmax_items = causal_mask_offset_start + seq_start + 1;
                    for (int m = 0; m < seq_cout; m++) {
                        float* src = reinterpret_cast<float*>(pMatMul0Out + m * rndup(param.key_seq_len * sizeof(float), 64));
                        ov::bfloat16* dst = reinterpret_cast<ov::bfloat16*>(pMatMul0Out + m * rndup(param.key_seq_len * sizeof(ov::bfloat16), 64));
                        if (!_create_param.is_bloom)
                            mul_add_f32_avx512(src, src, _create_param.normal_factor, pAddIn1_aux, valid_softmax_items);
                        else
                            mul_add2_f32_avx512(src, src, _create_param.normal_factor, 
                                alibi + i0 * _create_param.num_heads * param.key_seq_len + i1 * param.key_seq_len,
                                pAddIn1_aux,
                                valid_softmax_items);
                        softmax_avx512<ov::bfloat16>(dst, src, valid_softmax_items, nullptr);
                        // attn_scores = torch.where(causal_mask, attn_scores, mask_value)
                        if (param.key_seq_len > valid_softmax_items) {
                            auto *invalidPtr = dst + valid_softmax_items;
                            memset(static_cast<void*>(invalidPtr), 0, (param.key_seq_len - valid_softmax_items) * get_precision_size(_create_param.qkv_precision));
                            valid_softmax_items = std::min(valid_softmax_items + 1, param.key_seq_len);
                        }
                    }
                }

                auto pOut_aux = pout + (i0 * batch_stride_in_attn + i1 * head_stride_in_attn
                    + seq_start * head_stride_in_attn * _create_param.num_heads) * outPrcSize;
                tensor2D<ov::bfloat16> matQKBF16(seq_cout, param.key_seq_len, reinterpret_cast<ov::bfloat16*>(bufferMatMul0Out_local), rndup(param.key_seq_len * sizeof(ov::bfloat16), 64));
                tensor2D<ov::bfloat16> matV(param.key_seq_len, _create_param.head_size, reinterpret_cast<ov::bfloat16*>(pVIn0_aux), v.m_strides[2]);
                tensor2D<float> matQKV(seq_cout, _create_param.head_size, reinterpret_cast<float*>(bufferMatMul1Out_local), head_size_aligned * sizeof(float));
                amx_kernel::PP::BiasGeluStore<float, amx_kernel::PP::Steps::NONE> pp2(matQKV);
                (*qKVGemm_ops[threadNum])(matQKBF16, matV, 0, _create_param.head_size, pp2, prev_v == pVIn0_aux);
                prev_v = pVIn0_aux;
                memcpy2d_stride_avx512<ov::bfloat16>(reinterpret_cast<ov::bfloat16*>(pOut_aux), reinterpret_cast<float*>(bufferMatMul1Out_local), seq_cout,
                    _create_param.head_size, head_size_aligned * sizeof(float), _create_param.num_heads * _create_param.head_size * sizeof(ov::bfloat16), nullptr);
                parallel_it_step(i0, param.batch, i1, _create_param.num_heads, seq, seq_cout_all);
            }
        });
    }
}

void mha_gpt_impl_amx::mha_i8(const mha_gpt::exec_param &param) {
    auto& q = param.q;
    auto& k = param.k;
    auto& v = param.v;
    auto attn_masks = param.attention_mask.data<float>();
    uint8_t* pout = param.attn_output.data<uint8_t>();
    auto alibi = param.alibi.data<float>();

    auto outPrcSize = get_precision_size(_create_param.dst_precision);
    auto& gemAvB_ops = gemAvB_i8xi8;
    auto& qKtrGemm_ops = qKtrGemm_i8xi8;
    auto& qKVGemm_ops = qKVGemm_u8xi8;
    bool is_vector = param.query_seq_len == 1 && _create_param.head_size >= 64 && _create_param.head_size <= 64 * 6 && !_create_param.is_bloom;
    // dequant param
    auto mul_scales = _create_param.normal_factor * param.q_dequant * param.k_dequant;
    // prepare for per channel
    assert(param.qkv_quant.size() == 1 || param.qkv_quant.size() == _create_param.head_size);
    for (size_t i = 0; i < param.qkv_quant.size(); i++) {
        (qkvQuantBuf.get())[i] = param.qkv_quant[i] * param.v_dequant / param.qk_quant;
    }
    if (param.qkv_quant.size() == 1) {
        std::fill(qkvQuantBuf.get() + 1, qkvQuantBuf.get() + _create_param.head_size, *qkvQuantBuf.get());
    }
    size_t head_stride_in_attn = _create_param.head_size;
    size_t batch_stride_in_attn = _create_param.head_size * _create_param.num_heads * param.query_seq_len;
    size_t causal_mask_offset_start = param.key_seq_len - param.query_seq_len;

    if (is_vector) {
        parallel_for2d(param.batch, _create_param.num_heads, [&](size_t threadNum, size_t i0, size_t i1) {
            auto pQIn0_aux = &q.at<uint8_t>({i0, i1});
            auto pKIn0_aux = &k.at<uint8_t>({i0, i1});
            auto pVIn0_aux = &v.at<uint8_t>({i0, i1});

            auto pAddIn1_aux = attn_masks + i0 * param.key_seq_len;

            auto bufferMatMul0Out_local = reinterpret_cast<uint8_t*>(bufferMatMul0Out.get() + threadNum * bufferMatMul0OutSize);
            auto bufferMatMul1Out_local = reinterpret_cast<uint8_t*>(bufferMatMul1Out.get() + threadNum * bufferMatMul1OutSize);
            
            tensor2D<int8_t> matK(param.key_seq_len, _create_param.head_size, reinterpret_cast<int8_t*>(pKIn0_aux), k.m_strides[2]);
            // N: key_seq_len, K: head_size
            // q[1, K] * transpose(k[N, K])        ==>
            //     k[N, K] * transpose(q[1, K])    ==>
            //     k[N, K] * q[K, 1]
            (*gemAvB_ops[threadNum])(matK, reinterpret_cast<int8_t*>(pQIn0_aux), reinterpret_cast<int32_t*>(bufferMatMul0Out_local));
            cvt_i32_f32_avx512(reinterpret_cast<float*>(bufferMatMul0Out_local), reinterpret_cast<int32_t*>(bufferMatMul0Out_local), param.key_seq_len);

            float* pMatMul0Out = reinterpret_cast<float*>(bufferMatMul0Out_local);
            mul_add_f32_avx512(pMatMul0Out, pMatMul0Out, mul_scales, pAddIn1_aux, param.key_seq_len);
            softmax_avx512<uint8_t>(reinterpret_cast<uint8_t*>(pMatMul0Out), pMatMul0Out, param.key_seq_len, param.qk_quant);
            auto pOut_aux = pout + (i0 * batch_stride_in_attn + i1 * head_stride_in_attn) * outPrcSize;
            tensor2D<uint8_t> matQK(param.query_seq_len, param.key_seq_len, reinterpret_cast<uint8_t*>(bufferMatMul0Out_local), rndup(param.key_seq_len * sizeof(uint8_t), 64));
            tensor2D<int8_t> matV(param.key_seq_len, _create_param.head_size, reinterpret_cast<int8_t*>(pVIn0_aux), v.m_strides[2]);
            tensor2D<float> matQKV(param.query_seq_len, _create_param.head_size, reinterpret_cast<float*>(bufferMatMul1Out_local), head_size_aligned * sizeof(float));
            amx_kernel::PP::BiasGeluStore<float, amx_kernel::PP::Steps::NONE> pp(matQKV);
            (*qKVGemm_ops[threadNum])(matQK, matV, 0, _create_param.head_size, pp);
            memcpy2d_stride_avx512<int8_t>(reinterpret_cast<int8_t*>(pOut_aux), reinterpret_cast<float*>(bufferMatMul1Out_local), param.query_seq_len,
                _create_param.head_size, head_size_aligned * sizeof(float), _create_param.num_heads * _create_param.head_size, qkvQuantBuf.get());
        });
    } else {
        auto numThreads = getTotalThreads();
        int seq_cout_all = rndup(param.query_seq_len, 32) / 32;
        int work_amount = param.batch * _create_param.num_heads * seq_cout_all;
        parallel_for(numThreads, [&](int threadNum) {
            int i0;
            int i1;
            int seq;
            int start {0}, end {0};
            splitter(work_amount, static_cast<int>(numThreads), threadNum, start, end);
            if (start >= work_amount) return;

            parallel_it_init(start, i0, param.batch, i1, _create_param.num_heads, seq, seq_cout_all);
            uint8_t* prev_k = nullptr;
            uint8_t* prev_v = nullptr;
            for (int iwork = start; iwork < end; ++iwork) {
                int seq_start = seq * 32;
                int seq_end = std::min(static_cast<size_t>(seq_start) + 32, param.query_seq_len);
                int seq_cout = seq_end - seq_start;
                // q: [batch, num_heads, query_seq_len, head_size]
                // k: [batch, num_heads, key_seq_len, head_size]
                // v: [batch, num_heads, value_seq_len, head_size]
                auto pQIn0_aux = &q.at<uint8_t>({static_cast<size_t>(i0), static_cast<size_t>(i1), static_cast<size_t>(seq_start)});
                auto pKIn0_aux = &k.at<uint8_t>({static_cast<size_t>(i0), static_cast<size_t>(i1)});
                auto pVIn0_aux = &v.at<uint8_t>({static_cast<size_t>(i0), static_cast<size_t>(i1)});

                auto bufferMatMul0Out_local = reinterpret_cast<uint8_t*>(bufferMatMul0Out.get() + threadNum * bufferMatMul0OutSize);
                auto bufferMatMul1Out_local = reinterpret_cast<uint8_t*>(bufferMatMul1Out.get() + threadNum * bufferMatMul1OutSize);
                
                tensor2D<int8_t> matQ(seq_cout, _create_param.head_size, reinterpret_cast<int8_t*>(pQIn0_aux), q.m_strides[2]);
                tensor2D<float> matQK(seq_cout, param.key_seq_len, reinterpret_cast<float*>(bufferMatMul0Out_local), rndup(param.key_seq_len * sizeof(float), 64));
                amx_kernel::PP::BiasGeluStore<float, amx_kernel::PP::Steps::NONE> pp(matQK);
                if (!_create_param.is_bloom) {
                    tensor2D<int8_t> matK(param.key_seq_len, _create_param.head_size, reinterpret_cast<int8_t*>(pKIn0_aux), k.m_strides[2]);
                    (*qKtrGemm_ops[threadNum])(matQ, matK, 0, param.key_seq_len, pp, prev_k == pKIn0_aux);
                } else {
                    tensor2D<int8_t> matK(_create_param.head_size, param.key_seq_len, reinterpret_cast<int8_t*>(pKIn0_aux), k.m_strides[3]);
                    (*qKtrGemm_ops[threadNum])(matQ, matK, 0, param.key_seq_len, pp, prev_k == pKIn0_aux);
                }
                prev_k = pKIn0_aux;

                auto pMatMul0Out = bufferMatMul0Out_local;
                if (param.is_causal_in_attention) {
                    auto pAddIn1_aux = attn_masks + i0 * param.key_seq_len * param.query_seq_len;
                    // loop along K dimension
                    for (int m = 0; m < seq_cout; m++) {
                        float* src = reinterpret_cast<float*>(pMatMul0Out + m * rndup(param.key_seq_len * sizeof(float), 64));
                        uint8_t* dst = reinterpret_cast<uint8_t*>(pMatMul0Out + m * rndup(param.key_seq_len * sizeof(uint8_t), 64));
                        mul_add_f32_avx512(src, src, mul_scales, pAddIn1_aux + (m + seq_start) * param.key_seq_len, param.key_seq_len);
                        if (!_create_param.is_bloom)
                            mul_add_f32_avx512(src, src, mul_scales, pAddIn1_aux + (m + seq_start) * param.key_seq_len, param.key_seq_len);
                        else
                            // alibi shape: [batch, head_num, 1, key_seq_len]
                            mul_add2_f32_avx512(src, src, mul_scales,
                                alibi + i0 * _create_param.num_heads * param.key_seq_len + i1 * param.key_seq_len,
                                pAddIn1_aux + (m + seq_start) * param.key_seq_len,
                                param.key_seq_len);
                        softmax_avx512<uint8_t>(dst, src, param.key_seq_len, param.qk_quant);
                    }
                } else {
                    auto pAddIn1_aux = attn_masks + i0 * param.key_seq_len;
                    // loop along K dimension
                    size_t valid_softmax_items = causal_mask_offset_start + seq_start + 1;
                    for (int m = 0; m < seq_cout; m++) {
                        float* src = reinterpret_cast<float*>(pMatMul0Out + m * rndup(param.key_seq_len * sizeof(float), 64));
                        uint8_t* dst = reinterpret_cast<uint8_t*>(pMatMul0Out + m * rndup(param.key_seq_len * sizeof(uint8_t), 64));
                        if (!_create_param.is_bloom)
                            mul_add_f32_avx512(src, src, mul_scales, pAddIn1_aux, valid_softmax_items);
                        else
                            mul_add2_f32_avx512(src, src, mul_scales, 
                                alibi + i0 * _create_param.num_heads * param.key_seq_len + i1 * param.key_seq_len,
                                pAddIn1_aux,
                                valid_softmax_items);
                        softmax_avx512<uint8_t>(dst, src, valid_softmax_items, param.qk_quant);
                        // attn_scores = torch.where(causal_mask, attn_scores, mask_value)
                        if (param.key_seq_len > valid_softmax_items) {
                            auto *invalidPtr = dst + valid_softmax_items;
                            memset(invalidPtr, 0, (param.key_seq_len - valid_softmax_items) * get_precision_size(_create_param.qkv_precision));
                            valid_softmax_items = std::min(valid_softmax_items + 1, param.key_seq_len);
                        }
                    }
                }
                auto pOut_aux = pout + (i0 * batch_stride_in_attn + i1 * head_stride_in_attn
                    + seq_start * head_stride_in_attn * _create_param.num_heads) * outPrcSize;
                tensor2D<uint8_t> matQKI8(seq_cout, param.key_seq_len, reinterpret_cast<uint8_t*>(bufferMatMul0Out_local), rndup(param.key_seq_len * sizeof(uint8_t), 64));
                tensor2D<int8_t> matV(param.key_seq_len, _create_param.head_size, reinterpret_cast<int8_t*>(pVIn0_aux), v.m_strides[2]);
                tensor2D<float> matQKV(seq_cout, _create_param.head_size, reinterpret_cast<float*>(bufferMatMul1Out_local), head_size_aligned * sizeof(float));
                amx_kernel::PP::BiasGeluStore<float, amx_kernel::PP::Steps::NONE> pp2(matQKV);
                (*qKVGemm_ops[threadNum])(matQKI8, matV, 0, _create_param.head_size, pp2, prev_v == pVIn0_aux);
                prev_v = pVIn0_aux;
                // matmul1: [batch, num_heads, query_seq_len, head_size]
                // attn_output: [batch, query_seq_len, num_heads * head_size]
                memcpy2d_stride_avx512<int8_t>(reinterpret_cast<int8_t*>(pOut_aux), reinterpret_cast<float*>(bufferMatMul1Out_local), seq_cout,
                    _create_param.head_size, head_size_aligned * sizeof(float), _create_param.num_heads * _create_param.head_size, qkvQuantBuf.get());
                parallel_it_step(i0, param.batch, i1, _create_param.num_heads, seq, seq_cout_all);
            }
        });
    }
}

void mha_gpt_impl_amx::exec(const mha_gpt::exec_param& param) {
    if (param.q.m_rank != 4 || param.k.m_rank != 4 || param.v.m_rank != 4) {
        std::cout << "q,k,v rank does not equal 4.\n";
        return;
    }
    auto b = param.q.m_dims[0];
    auto hn = param.q.m_dims[1];
    auto qs = param.q.m_dims[2];
    auto hs = param.q.m_dims[3];
    auto ks = param.k.m_dims[2];

    if (!(b == param.k.m_dims[0] && b == param.v.m_dims[0] &&
          hn == param.k.m_dims[1] && hn == param.v.m_dims[1] &&
          ks == param.v.m_dims[2] &&
          hs == param.k.m_dims[3] && hs == param.v.m_dims[3])) {
        std::cout << "dim of q,k,v is error.\n";
        return;
    }

    if (_create_param.qkv_precision == dnnl_f32) {
        assert(false);
    } else if (_create_param.qkv_precision == dnnl_bf16) {
        mha_bf16(param);
    } else if (_create_param.qkv_precision == dnnl_s8) {
        mha_i8(param);
    } else {
        assert(false && "doesn't support provided input precisions");
    }
}

std::shared_ptr<mha_gpt::impl> new_impl_amx() {
    return std::make_shared<mha_gpt_impl_amx>();
}

}