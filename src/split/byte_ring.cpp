#include "byte_ring.h"

#include <algorithm>
#include <cstring>

namespace radiosplit {

ByteRing::ByteRing(std::size_t capacity) : data_(capacity == 0 ? 1 : capacity) {}

void ByteRing::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    read_ = 0;
    size_ = 0;
}

void ByteRing::Write(const void* src, std::size_t bytes) {
    if (src == nullptr || bytes == 0 || data_.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const auto* in = static_cast<const std::uint8_t*>(src);
    if (bytes >= data_.size()) {
        in += bytes - data_.size();
        bytes = data_.size();
        read_ = 0;
        size_ = 0;
    }
    const std::size_t overflow = (size_ + bytes > data_.size()) ? (size_ + bytes - data_.size()) : 0;
    if (overflow > 0) {
        read_ = (read_ + overflow) % data_.size();
        size_ -= overflow;
    }
    std::size_t write = (read_ + size_) % data_.size();
    const std::size_t first = std::min(bytes, data_.size() - write);
    std::memcpy(data_.data() + write, in, first);
    if (bytes > first) {
        std::memcpy(data_.data(), in + first, bytes - first);
    }
    size_ += bytes;
}

std::size_t ByteRing::Read(void* dst, std::size_t bytes) {
    if (dst == nullptr || bytes == 0) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const std::size_t take = std::min(bytes, size_);
    auto* out = static_cast<std::uint8_t*>(dst);
    const std::size_t first = std::min(take, data_.size() - read_);
    std::memcpy(out, data_.data() + read_, first);
    if (take > first) {
        std::memcpy(out + first, data_.data(), take - first);
    }
    read_ = (read_ + take) % data_.size();
    size_ -= take;
    return take;
}

}  // namespace radiosplit
