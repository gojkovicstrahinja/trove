// trove/sstable.hpp — immutable sorted string table.
//
// File layout:
//   [data block 0] [data block 1] ... [index block] [bloom block] [footer: 48B]
//
//   data block   u32 crc32 | series of records, sorted by (key ASC, seq DESC):
//                  varint klen | key | u64 seq | u8 deleted | varint vlen | value
//                (the CRC covers the block payload, so bit rot is caught when the
//                 block is read instead of being parsed into wrong values)
//   index block  varint count
//                varint klen | smallest key
//                varint klen | largest key
//                u64 min_seq | u64 max_seq
//                count * { varint klen | last key in block | u64 offset | u32 len }
//   bloom block  u32 num_bits | u32 num_hashes | bits
//   footer       "TROVESST" | u64 idx_off | u64 idx_len | u64 bloom_off | u64 bloom_len | u64 count
//
// A block never splits a record, so a point read is: bloom probe (usually "no"),
// binary search the sparse index, one block read, linear scan of that block.
#pragma once

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "trove/bloom.hpp"
#include "trove/iterator.hpp"
#include "trove/util.hpp"

namespace trove {

struct SSTableOptions {
  size_t block_size = 4096;
  double bloom_false_positive_rate = 0.01;
};

class SSTableIterator;

class SSTableWriter {
 public:
  ~SSTableWriter();

  static std::unique_ptr<SSTableWriter> Create(const std::string& path, const SSTableOptions& opts,
                                               std::string* error);

  // Records must arrive in (key ASC, seq DESC) order; the writer verifies this
  // instead of trusting the caller, because a silently unsorted table would only
  // fail much later and in a confusing place.
  bool Add(std::string_view key, uint64_t seq, std::string_view value, bool deleted);

  bool Finish(std::string* error);

  uint64_t record_count() const { return record_count_; }
  uint64_t file_size() const { return file_size_; }
  // Bytes committed plus bytes sitting in the current block buffer. Used to
  // decide when to roll a new output file during compaction.
  uint64_t estimated_size() const { return offset_ + block_.size(); }
  const std::string& largest_key() const { return largest_key_; }
  const std::string& smallest_key() const { return smallest_key_; }

 private:
  SSTableWriter() = default;

  struct IndexEntry {
    std::string last_key;
    uint64_t offset = 0;
    uint32_t len = 0;
  };

  bool FlushBlock(std::string* error);
  bool WriteAt(const std::string& data, std::string* error);

  std::FILE* file_ = nullptr;
  std::string path_;
  SSTableOptions opts_;
  bool finished_ = false;

  std::string block_;
  std::string last_block_key_;
  std::vector<IndexEntry> index_;
  std::vector<uint64_t> key_hashes_;

  std::string smallest_key_;
  std::string largest_key_;
  std::string prev_key_;
  uint64_t prev_seq_ = UINT64_MAX;
  bool have_prev_ = false;

  uint64_t offset_ = 0;
  uint64_t record_count_ = 0;
  uint64_t file_size_ = 0;
  uint64_t min_seq_ = UINT64_MAX;
  uint64_t max_seq_ = 0;
};

class SSTableReader {
 public:
  friend class SSTableIterator;

  ~SSTableReader();

  static std::unique_ptr<SSTableReader> Open(const std::string& path, std::string* error);

  // Point lookup at a snapshot. Returns false when the table holds no visible
  // version. *deleted is set when the newest visible version is a tombstone.
  bool Get(std::string_view key, uint64_t snapshot, std::string* value, bool* deleted) const;

  std::unique_ptr<EntryIterator> NewIterator() const;

  bool MightContain(std::string_view key) const { return bloom_.MayContain(key); }

  const std::string& path() const { return path_; }
  const std::string& smallest_key() const { return smallest_key_; }
  const std::string& largest_key() const { return largest_key_; }
  uint64_t min_seq() const { return min_seq_; }
  uint64_t max_seq() const { return max_seq_; }
  uint64_t record_count() const { return record_count_; }
  uint64_t file_size() const { return file_size_; }
  size_t num_blocks() const { return index_.size(); }
  size_t bloom_bytes() const { return bloom_.memory_bytes(); }

 private:
  SSTableReader() = default;

  struct IndexEntry {
    std::string last_key;
    uint64_t offset = 0;
    uint32_t len = 0;
  };

  bool ReadAt(uint64_t offset, size_t len, std::string* out) const;
  bool ReadBlock(size_t index, std::string* out) const;
  size_t FindBlock(std::string_view key) const;

  std::FILE* file_ = nullptr;
  std::string path_;
  std::vector<IndexEntry> index_;
  BloomFilter bloom_;
  std::string smallest_key_;
  std::string largest_key_;
  uint64_t min_seq_ = 0;
  uint64_t max_seq_ = 0;
  uint64_t record_count_ = 0;
  uint64_t file_size_ = 0;
};

// Block-level helpers shared with the compaction path and covered by tests.
struct BlockRecord {
  std::string_view key;
  uint64_t seq = 0;
  bool deleted = false;
  std::string_view value;
};

bool ParseBlockOffsets(std::string_view block, std::vector<std::pair<uint32_t, uint32_t>>* offsets);
bool DecodeBlockRecord(std::string_view block, uint32_t offset, uint32_t len, BlockRecord* out);

}  // namespace trove
