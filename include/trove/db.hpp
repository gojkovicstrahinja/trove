// trove/db.hpp — the storage engine facade.
//
// Concurrency model: one mutex, writer-serialised, readers serialised against
// writers. That is a deliberate trade — correctness over throughput — and it is
// what makes a crash-recovery path easy to reason about. See docs/DESIGN.md for
// what a production engine would do instead (group commit, lock-free memtable).
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "trove/manifest.hpp"
#include "trove/memtable.hpp"
#include "trove/sstable.hpp"
#include "trove/wal.hpp"

namespace trove {

struct Options {
  // Rotate the memtable once it reaches this many bytes.
  size_t write_buffer_size = 1u << 20;  // 1 MiB

  // L0 -> L1 -> L2. Three levels keeps the read amplification story simple.
  int max_levels = 3;

  // Compact L0 once this many files exist.
  size_t l0_compaction_trigger = 4;

  // Each level may hold this multiple of the previous level's file count
  // before it is compacted downward.
  size_t level_size_multiplier = 8;

  // Roll a new output file during compaction past this size.
  uint64_t target_file_size = 4u << 20;  // 4 MiB

  // fsync every write. Off by default: WAL replay already survives a process
  // crash, and per-write fsync costs an order of magnitude in throughput.
  bool sync_writes = false;

  SSTableOptions sstable;
};

struct Stats {
  uint64_t last_sequence = 0;
  uint64_t memtable_bytes = 0;
  uint64_t memtable_entries = 0;
  uint64_t immutable_bytes = 0;
  uint64_t wal_bytes = 0;
  uint64_t sstable_bytes = 0;
  uint64_t sstable_records = 0;
  uint64_t num_files = 0;
  uint64_t flushes = 0;
  uint64_t compactions = 0;
  std::vector<std::pair<uint64_t, uint64_t>> level_files_bytes;  // (files, bytes) per level
};

class DB {
 public:
  static std::unique_ptr<DB> Open(const std::string& dir, const Options& options, std::string* error);

  ~DB();

  DB(const DB&) = delete;
  DB& operator=(const DB&) = delete;

  bool Put(std::string_view key, std::string_view value, std::string* error);
  bool Delete(std::string_view key, std::string* error);

  // Sets *found=false for tombstones as well as absent keys — callers never see
  // a tombstone, they just see "not there".
  bool Get(std::string_view key, std::string* value, bool* found, std::string* error);

  // Half-open range [start, end). Empty `end` means "to infinity". `limit` of 0
  // means unlimited. start must be <= end.
  bool Scan(std::string_view start, std::string_view end, size_t limit,
            std::vector<std::pair<std::string, std::string>>* out, std::string* error);

  bool Flush(std::string* error);
  bool CompactAll(std::string* error);

  Stats GetStats();
  uint64_t LastSequence();
  const std::string& dir() const { return dir_; }

 private:
  DB() = default;

  struct FileHandle {
    FileMeta meta;
    std::unique_ptr<SSTableReader> reader;
  };

  bool Recover(std::string* error);
  bool FlushLocked(std::string* error);  // caller holds mu_
  bool MaybeFlush(std::string* error);
  bool MaybeCompact(std::string* error);
  bool CompactLevel(int level, std::string* error);
  bool SaveManifest(std::string* error);

  std::string FilePath(const std::string& name) const { return dir_ + "/" + name; }
  std::string SstName(uint64_t number) const { return "sst-" + std::to_string(number) + ".sst"; }
  std::string WalName(uint64_t number) const { return "wal-" + std::to_string(number) + ".log"; }

  std::string dir_;
  Options opts_;

  mutable std::mutex mu_;

  std::unique_ptr<MemTable> mem_;
  std::unique_ptr<MemTable> imm_;
  std::unique_ptr<WAL> wal_;

  Manifest manifest_;
  std::vector<std::vector<FileHandle>> levels_;

  uint64_t wal_number_ = 0;
  uint64_t last_seq_ = 0;
  uint64_t flushes_ = 0;
  uint64_t compactions_ = 0;
};

}  // namespace trove
