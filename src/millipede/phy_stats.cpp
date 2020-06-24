#include "phy_stats.hpp"

PhyStats::PhyStats(Config* cfg)
    : config_(cfg)
{
    const size_t task_buffer_symbol_num_ul
        = cfg->ul_data_symbol_num_perframe * TASK_BUFFER_FRAME_NUM;
    decoded_bits_count_.calloc(cfg->UE_NUM, task_buffer_symbol_num_ul, 64);
    bit_error_count_.calloc(cfg->UE_NUM, task_buffer_symbol_num_ul, 64);

    decoded_blocks_count_.calloc(cfg->UE_NUM, task_buffer_symbol_num_ul, 64);
    block_error_count_.calloc(cfg->UE_NUM, task_buffer_symbol_num_ul, 64);

    evm_buffer_.calloc(TASK_BUFFER_FRAME_NUM, cfg->UE_ANT_NUM, 64);
    cx_float* ul_iq_f_ptr = (cx_float*)cfg->ul_iq_f[cfg->UL_PILOT_SYMS];
    cx_fmat ul_iq_f_mat(ul_iq_f_ptr, cfg->OFDM_CA_NUM, cfg->UE_ANT_NUM, false);
    ul_gt_mat = ul_iq_f_mat.st();
    ul_gt_mat = ul_gt_mat.cols(cfg->OFDM_DATA_START, cfg->OFDM_DATA_STOP - 1);
}

void PhyStats::print_phy_stats()
{
    auto& cfg = config_;
    const size_t task_buffer_symbol_num_ul
        = cfg->ul_data_symbol_num_perframe * TASK_BUFFER_FRAME_NUM;
    for (size_t ue_id = 0; ue_id < cfg->UE_NUM; ue_id++) {
        size_t total_decoded_bits(0);
        size_t total_bit_errors(0);
        size_t total_decoded_blocks(0);
        size_t total_block_errors(0);
        for (size_t i = 0; i < task_buffer_symbol_num_ul; i++) {
            total_decoded_bits += decoded_bits_count_[ue_id][i];
            total_bit_errors += bit_error_count_[ue_id][i];
            total_decoded_blocks += decoded_blocks_count_[ue_id][i];
            total_block_errors += block_error_count_[ue_id][i];
        }
        std::cout << "UE " << ue_id << ": bit errors (BER) " << total_bit_errors
                  << "/" << total_decoded_bits << "("
                  << 1.0 * total_bit_errors / total_decoded_bits
                  << "), block errors (BLER) " << total_block_errors << "/"
                  << total_decoded_blocks << " ("
                  << 1.0 * total_block_errors / total_decoded_blocks << ")"
                  << std::endl;
    }
}

void PhyStats::print_evm_stats(size_t frame_id)
{
    fmat evm_mat(evm_buffer_[frame_id % TASK_BUFFER_FRAME_NUM], config_->UE_NUM,
        1, false);
    evm_mat = sqrt(evm_mat) / config_->OFDM_DATA_NUM;
    std::stringstream ss;
    ss << "Frame " << frame_id << ":\n"
       << "  EVM " << 100 * evm_mat.st() << ", SNR "
       << -10 * log10(evm_mat.st());
    std::cout << ss.str();
}

void PhyStats::update_evm_stats(size_t frame_id, size_t sc_id, cx_fmat eq)
{
    //assert(eq.size() == ul_gt_mat.col(sc_id).size());
    fmat evm = abs(eq - ul_gt_mat.col(sc_id));
    fmat cur_evm_mat(evm_buffer_[frame_id % TASK_BUFFER_FRAME_NUM],
        config_->UE_NUM, 1, false);
    cur_evm_mat += evm % evm;
}

void PhyStats::update_bit_errors(
    size_t ue_id, size_t offset, uint8_t tx_byte, uint8_t rx_byte)
{
    uint8_t xor_byte(tx_byte ^ rx_byte);
    size_t bit_errors = 0;
    for (size_t j = 0; j < 8; j++) {
        bit_errors += xor_byte & 1;
        xor_byte >>= 1;
    }
    bit_error_count_[ue_id][offset] += bit_errors;
}

void PhyStats::update_decoded_bits(
    size_t ue_id, size_t offset, size_t new_bits_num)
{
    decoded_bits_count_[ue_id][offset] += new_bits_num;
}

void PhyStats::update_block_errors(
    size_t ue_id, size_t offset, size_t block_error_count)
{
    block_error_count_[ue_id][offset] += (block_error_count > 0);
}

void PhyStats::increment_decoded_blocks(size_t ue_id, size_t offset)
{
    decoded_blocks_count_[ue_id][offset]++;
}

PhyStats::~PhyStats()
{
    decoded_bits_count_.free();
    bit_error_count_.free();

    decoded_blocks_count_.free();
    block_error_count_.free();

    evm_buffer_.free();
}
