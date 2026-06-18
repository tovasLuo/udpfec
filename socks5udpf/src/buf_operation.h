#pragma once
#include <cstdint>

inline void set_buf_bit(uint8_t* buf, uint16_t buf_size, uint16_t pos_idx)
{
    assert(buf != nullptr && buf_size != 0);
    const uint16_t byte_idx = (uint16_t)((pos_idx >> 3) % buf_size);
    const uint8_t bit_idx = (uint8_t)(pos_idx & 0x07);
    buf[byte_idx] |= (uint8_t)(0x01 << bit_idx);
}

inline void clr_buf_bit(uint8_t* buf, uint16_t buf_size, uint16_t pos_idx)
{
    assert(buf != nullptr && buf_size != 0);
    const uint16_t byte_idx = (uint16_t)((pos_idx >> 3) % buf_size);
    const uint8_t bit_idx = (uint8_t)(pos_idx & 0x07);
    buf[byte_idx] &= (uint8_t)~(0x01 << bit_idx);
}

inline bool get_buf_bit(uint8_t* buf, uint16_t buf_size, uint16_t pos_idx)
{
    assert(buf != nullptr && buf_size != 0);
    const uint16_t byte_idx = (uint16_t)((pos_idx >> 3) % buf_size);
    const uint8_t bit_idx = (uint8_t)(pos_idx & 0x07);
    return (buf[byte_idx] & (uint8_t)(0x01 << bit_idx)) != 0;
}

inline void clr_buf_bit_mask(uint8_t* buf, uint16_t buf_size, uint16_t start_bit, uint8_t mask)
{
    assert(buf != nullptr && buf_size != 0);
    const uint16_t byte_idx = (uint16_t)((start_bit >> 3) % buf_size);
    buf[byte_idx] &= (uint8_t)(~mask);
}