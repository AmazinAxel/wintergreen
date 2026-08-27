#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace wintergreen {

// Compact pooled string storage for many small strings.
// Stores data in fixed-size chunks so large contiguous heap blocks are never
// required — important on ESP32 where the heap can be heavily fragmented after
// EPUB rendering.  StringRef offsets are global across all chunks; get() walks
// the chunk list to find the right one (O(n_chunks), typically < 15 iterations).
//
// Invariant: no single string ever spans two chunks.

struct StringRef {
  uint32_t off = 0;
  uint16_t len = 0;

  std::string_view view(const class StringPool& pool) const;
  std::string to_string(const class StringPool& pool) const;
};

class StringPool {
 public:
  StringRef add(std::string_view s) {
    StringRef r;
    r.len = static_cast<uint16_t>(s.size());
    r.off = static_cast<uint32_t>(total_size_);
    if (!s.empty()) {
      // Start a new chunk if this string would not fit in the current one.
      if (chunks_.empty() || chunks_.back().size() + s.size() > kChunkSize) {
        chunks_.emplace_back();
        chunks_.back().reserve(s.size() > kChunkSize ? s.size() : kChunkSize);
      }
      chunks_.back().append(s.data(), s.size());
      total_size_ += s.size();
    }
    return r;
  }

  std::string_view get(const StringRef& ref) const {
    if (ref.len == 0) return {};
    size_t offset = ref.off;
    for (const auto& chunk : chunks_) {
      if (offset < chunk.size())
        return {chunk.data() + offset, ref.len};
      offset -= chunk.size();
    }
    return {};
  }

  void reserve(size_t n) {
    // Pre-reserve the chunk vector capacity; the first chunk is created lazily.
    chunks_.reserve((n + kChunkSize - 1) / kChunkSize + 1);
  }

  // Force-release all backing allocations.
  void reset() {
    std::vector<std::string> tmp;
    chunks_.swap(tmp);
    total_size_ = 0;
  }

 private:
  // **Deliberately not a power of two.** std::string::reserve(n) allocates
  // n + 1 bytes for the NUL terminator, so a chunk size of 8192 asks the
  // allocator for 8193 — one byte past the block it would otherwise fit in
  // exactly. On a fragmented heap that fails with plenty free: opening the
  // Odyssey aborted in StringPool::add with 30,828 bytes free and a largest
  // block of exactly 8,192.
  //
  // Backing off by 64 leaves room for the terminator and the allocator's own
  // per-block header, so a chunk lands inside one block instead of straddling
  // two. Any size whose +1 crosses a boundary reintroduces this.
  static constexpr size_t kChunkSize = 8192 - 64;
  std::vector<std::string> chunks_;
  size_t total_size_ = 0;
};

inline std::string_view StringRef::view(const StringPool& pool) const {
  if (len == 0) return {};
  return pool.get(*this);
}

inline std::string StringRef::to_string(const StringPool& pool) const {
  return std::string(view(pool));
}

}  // namespace wintergreen
