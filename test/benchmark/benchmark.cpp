/**
 * @file main.cpp
 * @brief The benchmark program to generate the benchmark data for 
 * running different components in massive MIMO baseband processing.
 */

#include "comms-lib.h"
#include "config.hpp"
#include "control.hpp"
#include "logger.h"
#include "memory_manage.h"
#include "modulation.hpp"
#include "utils_ldpc.hpp"
#include "phy_ldpc_decoder_5gnr.h"
#include <armadillo>
#include <bitset>
#include <fstream>
// #include <gflags/gflags.h>
#include <immintrin.h>
#include <iostream>
#include <malloc.h>
#include <math.h>
#include <mkl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <cstdio>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <array>

static constexpr float kNoiseLevel = 1.0 / 200;
static constexpr float kBg = 1;
static constexpr float kZc = 72;
static constexpr float kNumIterations = 1000;
static constexpr float kZFIterations = 100;

// DEFINE_string(conf_file,
//     TOSTRING(PROJECT_DIRECTORY) "/data/tddconfig-sim-ul.json",
//     "Agora config filename");

Config *cfg;

/**
 * @brief Generate one information bit sequence and the corresponding
 * encoded bit sequence for one code block for the active LDPC configuration
 *
 * @param information The generated input bit sequence
 * @param encoded_codeword The generated encoded codeword bit sequence
 * @param ue_id ID of the UE that this codeblock belongs to
 */
void gen_codeblock_ul(std::vector<int8_t>& information,
    std::vector<int8_t>& encoded_codeword, size_t Bg, size_t Zc)
{
    FastRand fast_rand;
    size_t nRows = Bg == 1 ? 46 : 42;
    std::vector<int8_t> parity;
    parity.resize(ldpc_encoding_parity_buf_size(Bg, Zc));

    information.resize(ldpc_encoding_input_buf_size(Bg, Zc));
    encoded_codeword.resize(ldpc_encoding_encoded_buf_size(Bg, Zc));

    size_t num_input_bytes = bits_to_bytes(ldpc_num_input_bits(Bg, Zc));

    for (size_t i = 0; i < num_input_bytes; i++) {
        information[i] = static_cast<int8_t>(fast_rand.next_u32());
    }

    ldpc_encode_helper(Bg, Zc, nRows, &encoded_codeword[0], 
        &parity[0], &information[0]);

    information.resize(bits_to_bytes(ldpc_num_input_bits(Bg, Zc)));
    encoded_codeword.resize(bits_to_bytes(ldpc_num_encoded_bits(Bg, Zc, nRows)));
}

float rand_float_from_short(float min, float max)
{
    float rand_val = ((float(rand()) / float(RAND_MAX)) * (max - min)) + min;
    short rand_val_ushort = (short)(rand_val * 32768);
    rand_val = (float)rand_val_ushort / 32768;
    return rand_val;
}

/**
 * @brief Return the output of modulating the encoded codeword
 * @param encoded_codeword The encoded LDPC codeword bit sequence
 * @return An array of complex floats with OFDM_DATA_NUM elements
 */
std::vector<complex_float> get_modulation(
    const std::vector<int8_t>& encoded_codeword, size_t mod_order_bits,
    size_t Bg, size_t Zc)
{
    std::vector<complex_float> modulated_codeword(cfg->OFDM_DATA_NUM);
    std::vector<uint8_t> mod_input(cfg->OFDM_DATA_NUM);

    size_t nRows = Bg == 1 ? 46 : 42;
    size_t num_encoded_bits = bits_to_bytes(ldpc_num_encoded_bits(Bg, Zc, nRows));

    adapt_bits_for_mod(
        reinterpret_cast<const uint8_t*>(&encoded_codeword[0]),
        &mod_input[0], num_encoded_bits, mod_order_bits);

    for (size_t i = 0; i < cfg->OFDM_DATA_NUM; i++) {
        modulated_codeword[i]
            = mod_single_uint8(mod_input[i], cfg->mod_table);
    }
    return modulated_codeword;
}

/**
 * @param modulated_codeword The modulated codeword with OFDM_DATA_NUM
 * elements
 * @brief An array with OFDM_CA_NUM elements with the OFDM_DATA_NUM
 * modulated elements binned at the center
 */
std::vector<complex_float> bin_for_ifft(
    const std::vector<complex_float> modulated_codeword)
{
    std::vector<complex_float> pre_ifft_symbol(cfg->OFDM_CA_NUM); // Zeroed
    memcpy(&pre_ifft_symbol[cfg->OFDM_DATA_START], &modulated_codeword[0],
        cfg->OFDM_DATA_NUM * sizeof(complex_float));

    return pre_ifft_symbol;
}

/// Return the time-domain pilot symbol with OFDM_CA_NUM complex floats
std::vector<complex_float> get_common_pilot_time_domain()
{
    const std::vector<std::complex<float>> zc_seq
        = Utils::double_to_cfloat(CommsLib::getSequence(
            cfg->OFDM_DATA_NUM, CommsLib::LTE_ZADOFF_CHU));

    const std::vector<std::complex<float>> zc_common_pilot
        = CommsLib::seqCyclicShift(zc_seq, M_PI / 4.0); // Used in LTE SRS

    std::vector<complex_float> ret(cfg->OFDM_CA_NUM); // Zeroed
    for (size_t i = 0; i < cfg->OFDM_DATA_NUM; i++) {
        ret[i + cfg->OFDM_DATA_START]
            = { zc_common_pilot[i].real(), zc_common_pilot[i].imag() };
    }

    return ret;
}

void run_csi(Table<complex_float>& input, complex_float* output, size_t base_sc_id, size_t sc_block_size)
{
    complex_float converted_sc[kSCsPerCacheline];

    size_t sc_start = base_sc_id;
    size_t sc_end = base_sc_id + sc_block_size;
    sc_end = sc_end > cfg->OFDM_DATA_NUM ? cfg->OFDM_DATA_NUM : sc_end;

    for (size_t j = 0; j < cfg->BS_ANT_NUM; j++) {
        complex_float* sc_ptr = &input[0][j * cfg->OFDM_CA_NUM];
        for (size_t block_idx = sc_start / kTransposeBlockSize;
                block_idx < sc_end / kTransposeBlockSize;
                block_idx++) {

            const size_t block_base_offset
                = block_idx * (kTransposeBlockSize * cfg->BS_ANT_NUM);

            for (size_t sc_j = 0; sc_j < kTransposeBlockSize;
                    sc_j += kSCsPerCacheline) {
                const size_t sc_idx
                    = (block_idx * kTransposeBlockSize) + sc_j;

                memcpy(
                    reinterpret_cast<float*>(converted_sc),
                    reinterpret_cast<float*>(sc_ptr
                        + (cfg->OFDM_DATA_START + sc_idx)),
                    kSCsPerCacheline * sizeof(complex_float));

                const complex_float* src = converted_sc;
                complex_float* dst = output
                    + block_base_offset + (j * kTransposeBlockSize)
                    + sc_j;

                // With either of AVX-512 or AVX2, load one cacheline =
                // 16 float values = 8 subcarriers = kSCsPerCacheline
                // TODO: AVX512 complex multiply support below
                // size_t pilots_sgn_offset = cfg->bs_server_addr_idx
                //     * cfg->get_num_sc_per_server();
                size_t pilots_sgn_offset = 0;

                __m256 fft_result0 = _mm256_load_ps(
                    reinterpret_cast<const float*>(src));
                __m256 fft_result1 = _mm256_load_ps(
                    reinterpret_cast<const float*>(src + 4));
                __m256 pilot_tx0 = _mm256_set_ps(
                    cfg->pilots_sgn_[sc_idx + 3 + pilots_sgn_offset].im,
                    cfg->pilots_sgn_[sc_idx + 3 + pilots_sgn_offset].re,
                    cfg->pilots_sgn_[sc_idx + 2 + pilots_sgn_offset].im,
                    cfg->pilots_sgn_[sc_idx + 2 + pilots_sgn_offset].re,
                    cfg->pilots_sgn_[sc_idx + 1 + pilots_sgn_offset].im,
                    cfg->pilots_sgn_[sc_idx + 1 + pilots_sgn_offset].re,
                    cfg->pilots_sgn_[sc_idx + pilots_sgn_offset].im,
                    cfg->pilots_sgn_[sc_idx + pilots_sgn_offset].re);
                fft_result0 = CommsLib::__m256_complex_cf32_mult(
                    fft_result0, pilot_tx0, true);

                __m256 pilot_tx1 = _mm256_set_ps(
                    cfg->pilots_sgn_[sc_idx + 7 + pilots_sgn_offset].im,
                    cfg->pilots_sgn_[sc_idx + 7 + pilots_sgn_offset].re,
                    cfg->pilots_sgn_[sc_idx + 6 + pilots_sgn_offset].im,
                    cfg->pilots_sgn_[sc_idx + 6 + pilots_sgn_offset].re,
                    cfg->pilots_sgn_[sc_idx + 5 + pilots_sgn_offset].im,
                    cfg->pilots_sgn_[sc_idx + 5 + pilots_sgn_offset].re,
                    cfg->pilots_sgn_[sc_idx + 4 + pilots_sgn_offset].im,
                    cfg->pilots_sgn_[sc_idx + 4 + pilots_sgn_offset]
                        .re);
                fft_result1 = CommsLib::__m256_complex_cf32_mult(
                    fft_result1, pilot_tx1, true);
                _mm256_stream_ps(
                    reinterpret_cast<float*>(dst), fft_result0);
                _mm256_stream_ps(
                    reinterpret_cast<float*>(dst + 4), fft_result1);
            }
        }
    }
}

// Gather data of one symbol from partially-transposed buffer
// produced by dofft
static inline void partial_transposeGather(
    size_t cur_sc_id, float* src, float*& dst, size_t bs_ant_num)
{
    // The SIMD and non-SIMD methods are equivalent.
    size_t ant_start = 0;
    if (bs_ant_num >= 4) {
        __m256i index = _mm256_setr_epi32(0, 1, kTransposeBlockSize * 2,
            kTransposeBlockSize * 2 + 1, kTransposeBlockSize * 4,
            kTransposeBlockSize * 4 + 1, kTransposeBlockSize * 6,
            kTransposeBlockSize * 6 + 1);

        const size_t transpose_block_id = cur_sc_id / kTransposeBlockSize;
        const size_t sc_inblock_idx = cur_sc_id % kTransposeBlockSize;
        const size_t offset_in_src_buffer
            = transpose_block_id * bs_ant_num * kTransposeBlockSize
            + sc_inblock_idx;

        src = src + offset_in_src_buffer * 2;
        for (size_t ant_idx = 0; ant_idx < bs_ant_num; ant_idx += 4) {
            // fetch 4 complex floats for 4 ants
            __m256 t = _mm256_i32gather_ps(src, index, 4);
            _mm256_storeu_ps(dst, t);
            src += 8 * kTransposeBlockSize;
            dst += 8;
        }
        // Set the of the remaining antennas to use non-SIMD gather
        ant_start = bs_ant_num / 4 * 4;
    }
    if (ant_start < bs_ant_num) {
        const size_t pt_base_offset = (cur_sc_id / kTransposeBlockSize)
            * (kTransposeBlockSize * bs_ant_num);
        complex_float* cx_src = (complex_float*)src;
        complex_float* cx_dst = (complex_float*)dst + ant_start;
        for (size_t ant_i = ant_start; ant_i < bs_ant_num; ant_i++) {
            *cx_dst = cx_src[pt_base_offset + (ant_i * kTransposeBlockSize)
                + (cur_sc_id % kTransposeBlockSize)];
            cx_dst++;
        }
    }
}

void compute_precoder(const arma::cx_fmat& mat_csi, complex_float* _mat_ul_zf,
    complex_float* _mat_dl_zf)
{
    arma::cx_fmat mat_ul_zf(reinterpret_cast<arma::cx_float*>(_mat_ul_zf),
        cfg->UE_NUM, cfg->BS_ANT_NUM, false);
    try {
        mat_ul_zf = arma::inv_sympd(mat_csi.t() * mat_csi) * mat_csi.t();
    } catch (std::runtime_error) {
        MLPD_WARN(
            "Failed to invert channel matrix, falling back to pinv()\n");
        // std::cout << mat_csi << std::endl;
        rt_assert(false);
        arma::pinv(mat_ul_zf, mat_csi, 1e-2, "dc");
    }

    arma::cx_fmat mat_dl_zf(reinterpret_cast<arma::cx_float*>(_mat_dl_zf),
        cfg->UE_NUM, cfg->BS_ANT_NUM, false);
    mat_dl_zf = mat_ul_zf;
    // We should be scaling the beamforming matrix, so the IFFT
    // output can be scaled with OFDM_CA_NUM across all antennas.
    // See Argos paper (Mobicom 2012) Sec. 3.4 for details.
    mat_dl_zf /= abs(mat_dl_zf).max();
}

void run_zf(Table<complex_float>& csi_buffer, complex_float* csi_gather_buffer, 
    Table<complex_float>& ul_zf_matrices,
    Table<complex_float>& dl_zf_matrices, size_t base_sc_id)
{
    // Gather CSIs from partially-transposed CSIs
    for (size_t i = 0; i < cfg->UE_NUM; i ++) {
        const size_t cur_sc_id = base_sc_id + i;
        float* dst_csi_ptr = (float*)(csi_gather_buffer + cfg->BS_ANT_NUM * i);
        partial_transposeGather(cur_sc_id, (float*)csi_buffer[0],
            dst_csi_ptr, cfg->BS_ANT_NUM);
    }

    arma::cx_fmat mat_csi(reinterpret_cast<arma::cx_float*>(csi_gather_buffer),
        cfg->BS_ANT_NUM, cfg->UE_NUM, false);

    compute_precoder(mat_csi,
        ul_zf_matrices[cfg->get_zf_sc_id(base_sc_id)],
        dl_zf_matrices[cfg->get_zf_sc_id(base_sc_id)]);
}

void run_demul(complex_float* input, complex_float* data_gather_buffer,
    complex_float* equaled_buffer_temp, complex_float* equaled_buffer_temp_transposed,
    Table<complex_float>& ul_zf_matrices, void** jitter,
    cgemm_jit_kernel_t* mkl_jit_cgemm, Table<int8_t>& output, size_t base_sc_id)
{
    size_t max_sc_ite;
    max_sc_ite = std::min(cfg->demul_block_size, cfg->OFDM_DATA_NUM - base_sc_id);
    assert(max_sc_ite % kSCsPerCacheline == 0);

    complex_float tmp[kSCsPerCacheline];
    for (size_t i = 0; i < max_sc_ite; i += kSCsPerCacheline) {
        for (size_t j = 0; j < cfg->BS_ANT_NUM; j++) {
            complex_float *src = input + j * cfg->OFDM_CA_NUM + i + base_sc_id;
            memcpy(tmp, src, kSCsPerCacheline * sizeof(complex_float));
            for (size_t t = 0; t < kSCsPerCacheline; t++) {
                complex_float* dst = data_gather_buffer
                    + (base_sc_id + i + t) * cfg->BS_ANT_NUM + j;
                *dst = tmp[t];
            }
        }
    }

    for (size_t i = 0; i < max_sc_ite; i++) {
        size_t cur_sc_id = base_sc_id + i;

        arma::cx_float* equal_ptr = nullptr;
        equal_ptr = (arma::cx_float*)(&equaled_buffer_temp[(cur_sc_id - base_sc_id)
            * cfg->UE_NUM]);
        arma::cx_fmat mat_equaled(equal_ptr, cfg->UE_NUM, 1, false);

        arma::cx_float* data_ptr = reinterpret_cast<arma::cx_float*>(
            &data_gather_buffer[cur_sc_id * cfg->BS_ANT_NUM]);
        arma::cx_float* ul_zf_ptr = reinterpret_cast<arma::cx_float*>(
            ul_zf_matrices[cfg->get_zf_sc_id(cur_sc_id)]);

        mkl_jit_cgemm[cfg->UE_NUM](jitter[cfg->UE_NUM], (MKL_Complex8*)ul_zf_ptr, (MKL_Complex8*)data_ptr,
            (MKL_Complex8*)equal_ptr);
    }

    __m256i index2 = _mm256_setr_epi32(0, 1, cfg->UE_NUM * 2,
        cfg->UE_NUM * 2 + 1, cfg->UE_NUM * 4, cfg->UE_NUM * 4 + 1,
        cfg->UE_NUM * 6, cfg->UE_NUM * 6 + 1);
    float* equal_T_ptr = (float*)(equaled_buffer_temp_transposed);

    for (size_t i = 0; i < cfg->UE_NUM; i ++) {
        float* equal_ptr = (float*)(equaled_buffer_temp + i);
        size_t kNumDoubleInSIMD256 = sizeof(__m256) / sizeof(double); // == 4
        for (size_t j = 0; j < max_sc_ite / kNumDoubleInSIMD256; j++) {
            __m256 equal_T_temp = _mm256_i32gather_ps(equal_ptr, index2, 4);
            _mm256_store_ps(equal_T_ptr, equal_T_temp);
            equal_T_ptr += 8;
            equal_ptr += cfg->UE_NUM * kNumDoubleInSIMD256 * 2;
        }
        equal_T_ptr = (float*)(equaled_buffer_temp_transposed);

        int8_t* demul_ptr = output[i]
            + (cfg->mod_order_bits * base_sc_id);

        switch (cfg->mod_order_bits) {
        case (CommsLib::QAM16):
            demod_16qam_soft_avx2(equal_T_ptr, demul_ptr, max_sc_ite);
            break;
        case (CommsLib::QAM64):
            demod_64qam_soft_avx2(equal_T_ptr, demul_ptr, max_sc_ite);
            break;
        default:
            printf("Demodulation: modulation type %s not supported!\n",
                cfg->modulation.c_str());
        }
    }
}

void run_decode(Table<int8_t>& input, Table<int8_t>& output,
    int16_t* resp_var_nodes, 
    size_t ue_id, size_t Bg, size_t Zc)
{
    LDPCconfig LDPC_config = cfg->LDPC_config;

    struct bblib_ldpc_decoder_5gnr_request ldpc_decoder_5gnr_request {
    };
    struct bblib_ldpc_decoder_5gnr_response ldpc_decoder_5gnr_response {
    };

    // Decoder setup
    int16_t numFillerBits = 0;
    size_t nRows = Bg == 1 ? 46 : 42;
    uint32_t cbCodewLen = ldpc_num_encoded_bits(Bg, Zc, nRows);
    uint32_t cbLen = ldpc_num_input_bits(Bg, Zc);
    int16_t numChannelLlrs = cbCodewLen;

    ldpc_decoder_5gnr_request.numChannelLlrs = numChannelLlrs;
    ldpc_decoder_5gnr_request.numFillerBits = numFillerBits;
    ldpc_decoder_5gnr_request.maxIterations = LDPC_config.decoderIter;
    ldpc_decoder_5gnr_request.enableEarlyTermination
        = LDPC_config.earlyTermination;
    ldpc_decoder_5gnr_request.Zc = Zc;
    ldpc_decoder_5gnr_request.baseGraph = Bg;
    ldpc_decoder_5gnr_request.nRows = nRows;

    int numMsgBits = cbLen - numFillerBits;
    ldpc_decoder_5gnr_response.numMsgBits = numMsgBits;
    ldpc_decoder_5gnr_response.varNodes = resp_var_nodes;

    auto* llr_buffer_ptr = input[ue_id];

    uint8_t* decoded_buffer_ptr
        = reinterpret_cast<uint8_t*>(output[ue_id]);

    ldpc_decoder_5gnr_request.varNodes = llr_buffer_ptr;
    ldpc_decoder_5gnr_response.compactedMessageBytes = decoded_buffer_ptr;

    bblib_ldpc_decoder_5gnr(
        &ldpc_decoder_5gnr_request, &ldpc_decoder_5gnr_response);
}

int main(int argc, char **argv)
{
    int opt;
    std::string conf_file = TOSTRING(PROJECT_DIRECTORY) "/data/tddconfig-sim-ul.json";
    while ((opt = getopt(argc, argv, "c:")) != -1) {
        switch (opt) {
            case 'c':
                conf_file = optarg;
                break;
            default:
                std::cerr << "Usage: " << argv[0] << " [-c conf_file]" << std::endl;
                exit(EXIT_FAILURE);
        }
    }

    // gflags::ParseCommandLineFlags(&argc, &argv, true);
    cfg = new Config(conf_file.c_str());
    cfg->genData();

    const size_t num_codeblocks = cfg->UE_NUM;

    std::vector<std::vector<int8_t>> information(num_codeblocks);
    std::vector<std::vector<int8_t>> encoded_codewords(num_codeblocks);

    for (size_t i = 0; i < num_codeblocks; i++) {
        gen_codeblock_ul(information[i], encoded_codewords[i], kBg, kZc);    
    }

    std::vector<std::vector<complex_float>> modulated_codewords(num_codeblocks);

    for (size_t i = 0; i < num_codeblocks; i++) {
        modulated_codewords[i] = get_modulation(
            encoded_codewords[i], cfg->mod_order_bits, kBg, kZc);
    }

    std::vector<std::vector<complex_float>> pre_ifft_data_syms(cfg->UE_NUM);

    for (size_t i = 0; i < cfg->UE_NUM; i++) {
        pre_ifft_data_syms[i] = bin_for_ifft(modulated_codewords[i]);
    }

    std::vector<complex_float> pilot_td
        = get_common_pilot_time_domain();

    Table<complex_float> tx_data_all_symbols;
    tx_data_all_symbols.calloc(2, cfg->OFDM_CA_NUM * cfg->UE_NUM, 64);

    for (size_t i = 0; i < cfg->UE_NUM; i++) {
        std::vector<complex_float> pilots_t_ue(cfg->OFDM_CA_NUM); // Zeroed
        for (size_t k = cfg->OFDM_DATA_START; 
            k < cfg->OFDM_DATA_START + cfg->OFDM_DATA_NUM; k += cfg->UE_NUM) {
            pilots_t_ue[i + k] = pilot_td[i + k];
        }
        // Load pilot to the second symbol
        // The first symbol is reserved for beacon
        memcpy(tx_data_all_symbols[0] + i * cfg->OFDM_CA_NUM,
            &pilots_t_ue[0], cfg->OFDM_CA_NUM * sizeof(complex_float));
    }

    for (size_t i = 0; i < cfg->UE_NUM; i++) {
        memcpy(tx_data_all_symbols[1] + i * cfg->OFDM_CA_NUM,
            &pre_ifft_data_syms[i][0], cfg->OFDM_CA_NUM * sizeof(complex_float));
    }

    // Generate CSI matrix
    Table<complex_float> csi_matrices;
    csi_matrices.calloc(
        cfg->OFDM_CA_NUM, cfg->UE_ANT_NUM * cfg->BS_ANT_NUM, 32);
    for (size_t i = 0; i < cfg->UE_ANT_NUM * cfg->BS_ANT_NUM; i++) {
        complex_float csi
            = { rand_float_from_short(-1, 1), rand_float_from_short(-1, 1) };
        for (size_t j = 0; j < cfg->OFDM_CA_NUM; j++) {
            complex_float noise = { rand_float_from_short(-1, 1) * kNoiseLevel,
                rand_float_from_short(-1, 1) * kNoiseLevel };
            csi_matrices[j][i].re = csi.re + noise.re;
            csi_matrices[j][i].im = csi.im + noise.im;
        }
    }

    // Generate RX data received by base station after going through channels
    Table<complex_float> rx_data_all_symbols;
    rx_data_all_symbols.calloc(2, cfg->OFDM_CA_NUM * cfg->BS_ANT_NUM, 64);

    for (size_t i = 0; i < 2; i ++) {
        arma::cx_fmat mat_input_data(
            reinterpret_cast<arma::cx_float*>(tx_data_all_symbols[i]),
            cfg->OFDM_CA_NUM, cfg->UE_ANT_NUM, false);
        arma::cx_fmat mat_output(
            reinterpret_cast<arma::cx_float*>(rx_data_all_symbols[i]),
            cfg->OFDM_CA_NUM, cfg->BS_ANT_NUM, false);

        for (size_t j = 0; j < cfg->OFDM_CA_NUM; j++) {
            arma::cx_fmat mat_csi(
                reinterpret_cast<arma::cx_float*>(csi_matrices[j]),
                cfg->BS_ANT_NUM, cfg->UE_ANT_NUM);
            mat_output.row(j) = mat_input_data.row(j) * mat_csi.st();
        }
    }

    double freq_ghz = measure_rdtsc_freq();
    // All the data is prepared, now start the simulation
    printf("Running CSI: ");
    Table<complex_float> csi_buffer;
    csi_buffer.calloc(cfg->UE_NUM, cfg->OFDM_DATA_NUM * cfg->BS_ANT_NUM, 64);

    size_t start_tsc = rdtsc();
    for (size_t iter = 0; iter < kNumIterations; iter ++) {
        const size_t csi_sc_block_size = 32;
        for (size_t basc_sc_id = 0; basc_sc_id < cfg->OFDM_DATA_NUM; basc_sc_id += csi_sc_block_size) {
            run_csi(rx_data_all_symbols, csi_buffer[0], basc_sc_id, csi_sc_block_size);
        }
    }
    size_t end_tsc = rdtsc();
    printf("%lf subcarriers/sec\n", (double)kNumIterations * 1000000.0f * cfg->OFDM_DATA_NUM / cycles_to_us(end_tsc - start_tsc, freq_ghz));

    printf("Running ZF: ");
    Table<complex_float> ul_zf_matrices;
    Table<complex_float> dl_zf_matrices;
    ul_zf_matrices.calloc(cfg->OFDM_DATA_NUM, cfg->BS_ANT_NUM * cfg->UE_NUM, 64);
    dl_zf_matrices.calloc(cfg->OFDM_DATA_NUM, cfg->UE_NUM * cfg->BS_ANT_NUM, 64);

    complex_float* csi_gather_buffer = reinterpret_cast<complex_float*>(
        memalign(64, kMaxAntennas * kMaxUEs * sizeof(complex_float)));
    start_tsc = rdtsc();
    for (size_t iter = 0; iter < kZFIterations; iter ++) {
        for (size_t base_sc_id = 0; base_sc_id < cfg->OFDM_DATA_NUM; base_sc_id += cfg->UE_NUM) {
            run_zf(csi_buffer, csi_gather_buffer, ul_zf_matrices, dl_zf_matrices, base_sc_id);
        }
    }
    end_tsc = rdtsc();
    free(csi_gather_buffer);
    printf("%lf times/sec\n", (double)kZFIterations * 1000000.0f * cfg->OFDM_DATA_NUM / cfg->UE_NUM / cycles_to_us(end_tsc - start_tsc, freq_ghz));

    printf("Running Demul: ");    
    Table<int8_t> demod_buffer;
    demod_buffer.calloc(cfg->UE_NUM, kMaxModType * cfg->OFDM_DATA_NUM, 64);

    complex_float* data_gather_buffer = reinterpret_cast<complex_float*>(
        memalign(64, cfg->OFDM_DATA_NUM * kMaxAntennas * sizeof(complex_float)));
    complex_float* equaled_buffer_temp = reinterpret_cast<complex_float*>(
        memalign(64, cfg->demul_block_size * kMaxUEs * sizeof(complex_float)));
    complex_float* equaled_buffer_temp_transposed = reinterpret_cast<complex_float*>(
        memalign(64, cfg->demul_block_size * kMaxUEs * sizeof(complex_float)));
    void* jitter[kMaxUEs];
    cgemm_jit_kernel_t mkl_jit_cgemm[kMaxUEs];
    MKL_Complex8 alpha = { 1, 0 };
    MKL_Complex8 beta = { 0, 0 };
    for (size_t i = 1; i <= cfg->UE_NUM; i ++) {
        mkl_jit_status_t status = mkl_jit_create_cgemm(&jitter[i], MKL_COL_MAJOR,
            MKL_NOTRANS, MKL_NOTRANS, i, 1, cfg->BS_ANT_NUM, &alpha,
            i, cfg->BS_ANT_NUM, &beta, i);
        if (MKL_JIT_ERROR == status) {
            fprintf(stderr,
                "Error: insufficient memory to JIT and store the DGEMM kernel\n");
            exit(1);
        }
        mkl_jit_cgemm[i] = mkl_jit_get_cgemm_ptr(jitter[i]);
    }

    start_tsc = rdtsc();
    for (size_t iter = 0; iter < kNumIterations; iter ++) {
        for (size_t base_sc_id = 0; base_sc_id < cfg->OFDM_DATA_NUM; base_sc_id += cfg->demul_block_size) {
            run_demul(rx_data_all_symbols[1], data_gather_buffer, equaled_buffer_temp,
                equaled_buffer_temp_transposed, ul_zf_matrices, jitter, mkl_jit_cgemm,
                demod_buffer, base_sc_id);
        }
    }
    end_tsc = rdtsc();
    printf("%lf subcarriers/sec\n", (double)kNumIterations * 1000000.0f * cfg->OFDM_DATA_NUM / cycles_to_us(end_tsc - start_tsc, freq_ghz));

    printf("Running Decode: ");
    Table<int8_t> decoded_buffer;
    decoded_buffer.calloc(cfg->UE_NUM, roundup<64>(cfg->num_bytes_per_cb), 64);

    int16_t* resp_var_nodes = (int16_t*)memalign(64, 1024 * 1024 * sizeof(int16_t));
    start_tsc = rdtsc();
    for (size_t iter = 0; iter < kNumIterations; iter ++) {
        for (size_t ue_id = 0; ue_id < cfg->UE_NUM; ue_id ++) {
            run_decode(demod_buffer, decoded_buffer, resp_var_nodes, ue_id, kBg, kZc);
        }
    }
    end_tsc = rdtsc();
    free(resp_var_nodes);
    printf("%lf users/sec\n", (double)kNumIterations * 1000000.0f * cfg->UE_NUM / cycles_to_us(end_tsc - start_tsc, freq_ghz));

    return 0;
}