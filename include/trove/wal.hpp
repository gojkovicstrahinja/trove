// trove/wal.hpp — write-ahead log.
//
// Record framing:  u32 payload_len | u32 crc32(payload) | payload
// payload:         u64 seq | u8 type | varint klen | key | varint vlen | value
//
// The length prefix lets replay skip anything intact; the CRC tells us whether
// what we skipped was real. A torn tail (power loss mid-write) is detected and
// truncated on open — the engine recovers to the last durable record instead of
// refusing to start, which is what you actually want in production.
#pragma once

#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace trove {

enum class RecordType : uint8_t { kPut = 1, kDelete = 2 };

struct WalRecord {
  uint64_t seq = 0;
  RecordType type = RecordType::kPut;
  std::string key;
  std::string value;
};

class WAL {
 public:
  ~WAL();

  WAL(const WAL&) = delete;
  WAL& operator=(const WAL&) = delete;

  // Opens (creating if needed) for append. Fails loudly rather than silently
  // running without a log.
  static std::unique_ptr<WAL> Open(const std::string& path, std::string* error);

  bool Append(uint64_t seq, RecordType type, std::string_view key, std::string_view value);
  bool Sync();

  // Replays every intact record in file order. On a torn/corrupt tail, truncates
  // the file at the last good boundary and reports it via *recovered_from_torn.
  static bool Replay(const std::string& path,
                     const std::function<void(const WalRecord&)>& fn,
                     uint64_t* max_seq,
                     bool* recovered_from_torn,
                     std::string* error);

  uint64_t FileSize() const { return size_; }

 private:
  WAL() = default;

  std::FILE* file_ = nullptr;
  std::string path_;
  uint64_t size_ = 0;
  uint64_t unsynced_ = 0;
};

}  // namespace trove
