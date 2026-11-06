// trove/manifest.hpp — the durable description of which SSTables are live.
//
// Deliberately a plain-text file: when someone's engine loses data at 2am, a
// format you can read with `cat` is worth more than a compact binary one. It is
// replaced atomically (write tmp + fsync + rename), so a crash leaves either the
// old manifest or the new one, never a half-written list.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace trove {

struct FileMeta {
  uint64_t number = 0;
  uint64_t file_size = 0;
  uint64_t record_count = 0;
};

class Manifest {
 public:
  static constexpr int kVersion = 1;

  uint64_t next_file_number = 1;
  std::vector<std::vector<FileMeta>> levels;  // levels[0] = newest (L0)

  static bool Load(const std::string& path, Manifest* out, std::string* error);
  bool Save(const std::string& path, std::string* error) const;

  uint64_t total_files() const;
  uint64_t total_bytes() const;
};

}  // namespace trove
