#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace radiosplit {

// Byte queue shared by the game watcher and the output callback.
// A full write drops the oldest bytes so the tap stays near the play cursor.
class ByteRing {
public:
    explicit ByteRing(std::size_t capacity);

    void Clear();
    void Write(const void* src, std::size_t bytes);
    std::size_t Read(void* dst, std::size_t bytes);

private:
    std::mutex mutex_;
    std::vector<std::uint8_t> data_;
    std::size_t read_ = 0;
    std::size_t size_ = 0;
};

}  // namespace radiosplit
