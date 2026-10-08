#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace rehab { namespace drivers {
inline int32_t sign_extend_24(uint32_t value) {
    value &= 0x00ffffffu;
    return value & 0x00800000u ? static_cast<int32_t>(value) - 0x01000000 : static_cast<int32_t>(value);
}
inline uint16_t read_be16(const uint8_t* bytes) {
    return static_cast<uint16_t>((static_cast<uint16_t>(bytes[0]) << 8) | bytes[1]);
}
inline void write_be16(uint8_t* bytes, uint16_t value) {
    bytes[0] = static_cast<uint8_t>(value >> 8); bytes[1] = static_cast<uint8_t>(value);
}
inline size_t ring_chunk(uint16_t pointer, size_t length, size_t capacity = 4096) {
    const size_t until_wrap = capacity - (pointer % capacity);
    return length < until_wrap ? length : until_wrap;
}

// Single task ownership: the network task alone calls poll/send/receive.
template <size_t Capacity> class ByteQueue {
public:
    size_t size() const { return size_; }
    size_t free() const { return Capacity - size_; }
    void clear() { head_ = size_ = 0; }
    size_t push(const uint8_t* data, size_t length) {
        if (!data) return 0;
        if (length > free()) length = free();
        for (size_t i = 0; i < length; ++i) data_[(head_ + size_ + i) % Capacity] = data[i];
        size_ += length;
        return length;
    }
    size_t peek(uint8_t* data, size_t length) const {
        if (!data) return 0;
        if (length > size_) length = size_;
        for (size_t i = 0; i < length; ++i) data[i] = data_[(head_ + i) % Capacity];
        return length;
    }
    void discard(size_t length) {
        if (length > size_) length = size_;
        head_ = (head_ + length) % Capacity; size_ -= length;
    }
    size_t pop(uint8_t* data, size_t length) {
        length = peek(data, length); discard(length); return length;
    }
private:
    uint8_t data_[Capacity]{};
    size_t head_ = 0, size_ = 0;
};
} }
