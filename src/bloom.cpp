#include "trove/bloom.hpp"

#include <cmath>
#include <vector>

namespace trove {

BloomFilter BloomFilter::Create(size_t expected_keys, double false_positive_rate) {
  if (false_positive_rate <= 0.0 || false_positive_rate >= 1.0) false_positive_rate = 0.01;
  if (expected_keys == 0) expected_keys = 1;

  const double ln2 = 0.693147180559945;
  double bits_per_key = -std::log(false_positive_rate) / (ln2 * ln2);
  if (bits_per_key < 1.0) bits_per_key = 1.0;
  if (bits_per_key > 64.0) bits_per_key = 64.0;  // -ln(1e-19) ceiling; keeps index small

  BloomFilter out;
  out.num_bits_ = static_cast<uint32_t>(expected_keys * bits_per_key);
  if (out.num_bits_ < 64) out.num_bits_ = 64;
  out.num_hashes_ = static_cast<uint32_t>(bits_per_key * ln2);
  if (out.num_hashes_ < 1) out.num_hashes_ = 1;
  if (out.num_hashes_ > 30) out.num_hashes_ = 30;
  out.bits_.assign((out.num_bits_ + 7) / 8, '\0');
  return out;
}

void BloomFilter::Add(std::string_view key) { AddHash(Hash64(key)); }

void BloomFilter::AddHash(uint64_t key_hash) {
  if (num_bits_ == 0) return;
  uint64_t h = key_hash;
  const uint64_t delta = (h >> 17) | (h << 15);
  for (uint32_t i = 0; i < num_hashes_; ++i) {
    const uint32_t bit = static_cast<uint32_t>(h % num_bits_);
    bits_[bit >> 3] = static_cast<char>(bits_[bit >> 3] | (1 << (bit & 7)));
    h += delta;
  }
}

bool BloomFilter::MayContain(std::string_view key) const {
  if (num_bits_ == 0) return true;  // no filter recorded: assume it might
  uint64_t h, delta;
  Probe(key, &h, &delta);
  for (uint32_t i = 0; i < num_hashes_; ++i) {
    const uint32_t bit = static_cast<uint32_t>(h % num_bits_);
    if ((bits_[bit >> 3] & (1 << (bit & 7))) == 0) return false;
    h += delta;
  }
  return true;
}

std::string BloomFilter::Serialize() const {
  std::string out;
  PutFixed32(&out, num_bits_);
  PutFixed32(&out, num_hashes_);
  out += bits_;
  return out;
}

bool BloomFilter::Deserialize(std::string_view in, BloomFilter* out) {
  std::string_view src = in;
  uint32_t bits = 0, hashes = 0;
  if (!GetFixed32(&src, &bits)) return false;
  if (!GetFixed32(&src, &hashes)) return false;
  if (src.size() != static_cast<size_t>((bits + 7) / 8)) return false;
  out->num_bits_ = bits;
  out->num_hashes_ = hashes;
  out->bits_.assign(src.data(), src.size());
  return true;
}

double BloomFilter::measure_fpr(const std::vector<std::string>& present,
                               const std::vector<std::string>& absent) const {
  size_t false_positives = 0;
  for (const auto& k : absent) {
    if (MayContain(k)) ++false_positives;
  }
  size_t missed = 0;
  for (const auto& k : present) {
    if (!MayContain(k)) ++missed;  // must never happen; surfaces hashing bugs
  }
  if (!absent.empty()) {
    const double fpr = static_cast<double>(false_positives) / static_cast<double>(absent.size());
    return missed == 0 ? fpr : -1.0;
  }
  return missed == 0 ? 0.0 : -1.0;
}

}  // namespace trove
