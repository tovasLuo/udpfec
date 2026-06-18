#include "slide_win.h"
#include <cassert>
#include <cstring>
#include "buf_operation.h"

slide_win::slide_win()
{
    reset();
}

slide_win::~slide_win()
{
}

void slide_win::reset(uint16_t pkg_sn/* = UINT16_MAX*/)
{
    // 计算初始下边界：确保窗口包含 [pkg_sn - k_slide_win_size + 1, pkg_sn] 范围
    const uint32_t tmp_pkg_sn = pkg_sn + UINT16_MAX + 1 - (k_slide_win_size - 1);
    win_pkg_sn_lower_ = (uint16_t)(tmp_pkg_sn & UINT16_MAX);
    win_pkg_sn_upper_ = pkg_sn;

    win_pos_idx_lower_ = win_pkg_sn_lower_ & (k_slide_win_size - 1); // & (k_slide_win_size - 1) 等价于 % k_slide_win_size，但前者效率更高
    win_pos_idx_upper_ = win_pkg_sn_upper_ & (k_slide_win_size - 1);

    memset(slide_win_buf_, 0, sizeof(slide_win_buf_));
    first_pkg_sn_ = true;
}

bool slide_win::pkg_sn_valid(uint16_t pkg_sn, bool only_check/* = false*/)
{
    if (first_pkg_sn_)
    {
        first_pkg_sn_valid(pkg_sn);
    }

    uint16_t slide_step = get_slide_step(pkg_sn);
    if (slide_step >= k_slide_win_max_step)
    {
        first_pkg_sn_valid(pkg_sn);
        slide_step = 1/*get_sliding_step(pkg_sn)*/;
    }

    if (slide_step != 0)
    {
        assert(slide_step < k_slide_win_max_step);
        win_pkg_sn_lower_ = (uint16_t)((win_pkg_sn_lower_ + slide_step) & UINT16_MAX);
        win_pkg_sn_upper_ = pkg_sn; //(uint16_t)((win_pkg_sn_upper_ + sliding_step) & UINT16_MAX);

        win_boundary_moving(slide_step);
    }

    // 判断是否为重复包
    uint16_t win_pos_idx_ = get_win_pos_idx(pkg_sn);
    if (get_buf_bit(slide_win_buf_, k_slide_win_buf_size, win_pos_idx_))
    {
        return false;
    }

    if (!only_check)
    {
        // 置包序号对应bit
        set_buf_bit(slide_win_buf_, k_slide_win_buf_size, win_pos_idx_);
    }

    return true;
}

void slide_win::first_pkg_sn_valid(uint16_t pkg_sn)
{
    // 初始窗口以上一个序列号为基准（若pkg_sn为0则取UINT16_MAX）
    const uint16_t prev_pkg_sn = pkg_sn != 0 ? (uint16_t)(pkg_sn - 1) : UINT16_MAX;
    reset(prev_pkg_sn);
    first_pkg_sn_ = false;
}

uint16_t slide_win::get_slide_step(uint16_t pkg_sn)
{
    assert(win_pkg_sn_upper_ != win_pkg_sn_lower_);

    // 序列号在滑窗内
    if (pkg_sn_in_win(pkg_sn))
    {
        return 0;
    }

    // 序列号在滑窗外
#if 0
    uint16_t slide_step = 0;
    if (win_pkg_sn_upper_ > win_pkg_sn_lower_)
    {
        assert(win_pkg_sn_upper_ < pkg_sn || pkg_sn < win_pkg_sn_lower_);

        if (win_pkg_sn_upper_ < pkg_sn) // 序号在滑窗上边界外
        {
            slide_step = (uint16_t)(pkg_sn - win_pkg_sn_upper_);
        }
        else// if (pkg_sn < win_pkg_sn_lower_) // 序号在滑窗下边界外
        {
            slide_step = (uint16_t)(UINT16_MAX + 1 - win_pkg_sn_upper_ + pkg_sn);
        }
    }
    else// if (win_pkg_sn_upper_ < win_pkg_sn_lower_)
    {
        assert(win_pkg_sn_upper_ < pkg_sn && pkg_sn < win_pkg_sn_lower_);
        slide_step = (uint16_t)(pkg_sn - win_pkg_sn_upper_);
    }

    return slide_step;

#else
    // 计算相对于上边界的偏移（处理16位循环）
    int32_t slide_step = pkg_sn - win_pkg_sn_upper_;
    if (slide_step < 0) // win_pkg_sn_upper_ > win_pkg_sn_lower_ > pkg_sn
    {
        slide_step += UINT16_MAX + 1;
    }
    return (uint16_t)slide_step;

#endif
}

bool slide_win::pkg_sn_in_win(uint16_t pkg_sn)
{
    assert(win_pkg_sn_upper_ != win_pkg_sn_lower_);

    if (win_pkg_sn_upper_ > win_pkg_sn_lower_)
    {
        // 窗口未跨边界：[lower, upper]
        return win_pkg_sn_upper_ >= pkg_sn && pkg_sn >= win_pkg_sn_lower_;
    }

    assert(win_pkg_sn_upper_ < win_pkg_sn_lower_);
    // 窗口跨边界：[lower, 65535] 或 [0, upper]
    return win_pkg_sn_upper_ >= pkg_sn || pkg_sn >= win_pkg_sn_lower_;
}

void slide_win::win_boundary_moving(uint16_t sliding_step)
{
    assert(sliding_step != 0);

    uint16_t new_win_pos_idx_lower_ = (uint16_t)(win_pos_idx_lower_ + sliding_step);
    if (new_win_pos_idx_lower_ < k_slide_win_size) // 本次滑动后下边界未翻转
    {
        clear_bit_range(win_pos_idx_lower_, sliding_step);
    }
    else// if (win_pos_idx >= k_slide_win_size) // 本次滑动后下边已翻转
    {
        clear_bit_range(win_pos_idx_lower_, (uint16_t)(k_slide_win_size - win_pos_idx_lower_));
        new_win_pos_idx_lower_ &= (k_slide_win_size - 1);
        clear_bit_range(0, (uint16_t)(sliding_step - (k_slide_win_size - win_pos_idx_lower_)));
    }

    win_pos_idx_lower_ = new_win_pos_idx_lower_;
    win_pos_idx_upper_ = (uint16_t)(win_pos_idx_lower_ != 0 ? win_pos_idx_lower_ - 1 : k_slide_win_size - 1);
}

void slide_win::clear_bit_range(uint16_t start_bit, uint16_t num_bits)
{
    if (num_bits == 0)
    {
        return;
    }

    if (num_bits == 1)
    {
        clr_buf_bit(slide_win_buf_, k_slide_win_buf_size, start_bit);
        return;
    }

    uint16_t bits_remaining = num_bits;
    uint16_t current_bit = start_bit & (k_slide_win_size - 1); // 确保起点合法

    // 处理起始不完整字节
    uint8_t first_byte_start_bit = current_bit & 0x07;
    if (first_byte_start_bit != 0)
    {
        uint8_t first_byte_clear_bits = (uint8_t)(bits_remaining > 8 - first_byte_start_bit ? 8 - first_byte_start_bit : bits_remaining);
        uint8_t mask = (uint8_t)((0xFF >> (8 - first_byte_clear_bits)) << first_byte_start_bit);
        clr_buf_bit_mask(slide_win_buf_, k_slide_win_buf_size, current_bit, mask);

        bits_remaining = (uint16_t)(bits_remaining - first_byte_clear_bits);
        if (bits_remaining == 0)
        {
            return;
        }

        current_bit = (uint16_t)((current_bit + first_byte_clear_bits) & (k_slide_win_size - 1));
    }

    // 处理完整字节（批量清零）
    uint16_t full_bytes = bits_remaining / 8;
    if (full_bytes != 0)
    {
        uint16_t byte_start = current_bit / 8;
        if (byte_start + full_bytes <= k_slide_win_buf_size)
        {
            memset(&slide_win_buf_[byte_start], 0, full_bytes);
        }
        else // 处理回绕
        {
            const uint16_t first_part = (uint16_t)(k_slide_win_buf_size - byte_start);
            memset(&slide_win_buf_[byte_start], 0, first_part);
            memset(slide_win_buf_, 0, full_bytes - first_part);
        }

        bits_remaining = (uint16_t)(bits_remaining - full_bytes * 8);
        current_bit = (uint16_t)((current_bit + full_bytes * 8) & (k_slide_win_size - 1));
    }

    // 处理剩余不完整字节
    if (bits_remaining > 0)
    {
        uint8_t mask = (uint8_t)(~(0xFF << bits_remaining));
        clr_buf_bit_mask(slide_win_buf_, k_slide_win_buf_size, current_bit, mask);
    }
}

uint16_t slide_win::get_win_pos_idx(uint16_t pkg_sn)
{
#if 0
#if 0
    assert(win_pkg_sn_upper_ != win_pkg_sn_lower_);

    uint16_t win_pkg_sn_idx = 0;
    if (win_pkg_sn_upper_ > win_pkg_sn_lower_)
    {
        assert(win_pkg_sn_upper_ >= pkg_sn && pkg_sn >= win_pkg_sn_lower_);
        win_pkg_sn_idx = (uint16_t)(pkg_sn - win_pkg_sn_lower_);
    }
    else if (win_pkg_sn_upper_ < win_pkg_sn_lower_)
    {
        assert(win_pkg_sn_upper_ >= pkg_sn || pkg_sn >= win_pkg_sn_lower_);

        if (win_pkg_sn_upper_ >= pkg_sn)
        {
            win_pkg_sn_idx = (uint16_t)(UINT16_MAX + 1 - win_pkg_sn_lower_ + pkg_sn);
        }
        else// if (pkg_sn >= win_pkg_sn_lower_)
        {
            win_pkg_sn_idx = (uint16_t)(pkg_sn - win_pkg_sn_lower_);
        }
    }

#else
    int32_t win_pkg_sn_idx = pkg_sn - win_pkg_sn_lower_;
    if (win_pkg_sn_idx < 0) // win_pkg_sn_upper_ < win_pkg_sn_lower_ <= pkg_sn
    {
        win_pkg_sn_idx += UINT16_MAX + 1;
    }

#endif
    return (win_pos_idx_lower_ + win_pkg_sn_idx) & (k_slide_win_size - 1);

#else
    return pkg_sn & (k_slide_win_size - 1);

#endif
}
