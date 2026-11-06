// trove/bloom.hpp — bloom filter used to skip SSTables that cannot contain a key.
//
// Probing scheme is RocksDB's "legacy" double hashing: one 64-bit hash per key,
// then h += delta per probe. Cheaper than k independent hashes and empirically
// within a few percent of theory for the false-positive rates we target.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "trove/util.hpp"

namespace trove {

class BloomFilter {
 public:
  BloomFilter() = default;

  // bits_per_key = -ln(fp) / (ln2)^2, probes = bits_per_key * ln2
  static BloomFilter Create(size_t expected_keys, double false_positive_rate);

  void Add(std::string_view key);
  // Same as Add, but takes a precomputed Hash64(key) so the writer does not
  // rehash every key during flush.
  void AddHash(uint64_t key_hash);
  bool MayContain(std::string_view key) const;

  // Layout: u32 num_bits | u32 num_hashes | ceil(num_bits/8) bytes
  std::string Serialize() const;
  static bool Deserialize(std::string_view in, BloomFilter* out);

  uint32_t num_bits() const { return num_bits_; }
  uint32_t num_hashes() const { return num_hashes_; }
  size_t memory_bytes() const { return bits_.size(); }
  // Empirical FPR against a sample; used by the bench target only.
  double measure_fpr(const std::vector<std::string>& present,
                     const std::vector<std::string>& absent) const;

 private:
  void Probe(std::string_view key, uint64_t* h, uint64_t* delta) const {
    *h = Hash64(key);
    // Rotate so the low bits (which we modulo) differ from the first hash.
    *delta = (*h >> 17) | (*h << 15);
  }

  std::string bits_;
  uint32_t num_bits_ = 0;
  uint32_t num_hashes_ = 0;
};

}  // namespace trove
