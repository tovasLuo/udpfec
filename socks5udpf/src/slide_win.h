#pragma once
#include <cstdint>

class slide_win
{
public:
    slide_win();
    ~slide_win();

    void reset(uint16_t pkg_sn = UINT16_MAX);
    bool pkg_sn_valid(uint16_t pkg_sn, bool only_check = false);

private:
    void first_pkg_sn_valid(uint16_t pkg_sn);

    uint16_t get_slide_step(uint16_t pkg_sn);
    bool pkg_sn_in_win(uint16_t pkg_sn);

    void win_boundary_moving(uint16_t sliding_step);
    void clear_bit_range(uint16_t start_bit, uint16_t num_bits);

    uint16_t get_win_pos_idx(uint16_t pkg_sn);

private:
    static const uint16_t k_slide_win_size = 4096;
    static const uint16_t k_slide_win_buf_size = k_slide_win_size >> 3;
    static const uint16_t k_slide_win_max_step = k_slide_win_size >> 2;

    uint8_t   slide_win_buf_[k_slide_win_buf_size];  //滑窗缓存
    uint16_t  win_pos_idx_lower_;                    //滑窗下边界位置索引
    uint16_t  win_pos_idx_upper_;                    //滑窗上边界位置索引

    uint16_t  win_pkg_sn_lower_;                     //滑窗下边界对应的包序列号
    uint16_t  win_pkg_sn_upper_;                     //滑窗上边界对应的包序列号

    bool      first_pkg_sn_;
};

