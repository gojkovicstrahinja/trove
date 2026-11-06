#include <string>
#include <vector>

#include "test_framework.hpp"
#include "trove/bloom.hpp"
#include "trove/memtable.hpp"
#include "trove/skiplist.hpp"
#include "trove/util.hpp"

using namespace trove;

TEST(crc32_known_vectors) {
  // The canonical check value from the CRC-32/IEEE specification.
  CHECK_EQ(Crc32(std::string_view("123456789")), 0xCBF43926u);
  CHECK_EQ(Crc32(std::string_view("")), 0u);
  // Single-bit flip must change the checksum.
  const std::string a = "the quick brown fox";
  std::string b = a;
  b[3] = static_cast<char>(b[3] ^ 0x01);
  CHECK(Crc32(a) != Crc32(b));
}

TEST(varint_roundtrip) {
  const std::vector<uint64_t> values = {0,     1,      127,      128,         300,
                                        16383, 16384,  1u << 21, 1ull << 32,  UINT64_MAX};
  for (uint64_t v : values) {
    std::string buf;
    PutVarint(&buf, v);
    std::string_view view = buf;
    uint64_t out = 0;
    CHECK(GetVarint(&view, &out));
    CHECK_EQ(out, v);
    CHECK(view.empty());
  }
}

TEST(varint_rejects_truncated_input) {
  std::string buf;
  PutVarint(&buf, 0xFFFFFFFFu);
  buf.pop_back();  // chop the terminator
  std::string_view view = buf;
  uint64_t out = 0;
  CHECK(!GetVarint(&view, &out));
}

TEST(fixed_width_is_little_endian) {
  std::string buf;
  PutFixed64(&buf, 0x0102030405060708ull);
  CHECK_EQ(static_cast<int>(static_cast<uint8_t>(buf[0])), 0x08);
  CHECK_EQ(static_cast<int>(static_cast<uint8_t>(buf[7])), 0x01);
  std::string_view view = buf;
  uint64_t out = 0;
  CHECK(GetFixed64(&view, &out));
  CHECK_EQ(out, 0x0102030405060708ull);
}

TEST(bloom_has_no_false_negatives) {
  BloomFilter bloom = BloomFilter::Create(1000, 0.01);
  std::vector<std::string> present;
  for (int i = 0; i < 1000; ++i) {
    const std::string key = "key-" + std::to_string(i);
    present.push_back(key);
    bloom.Add(key);
  }
  for (const auto& key : present) CHECK(bloom.MayContain(key));
}

TEST(bloom_false_positive_rate_matches_target) {
  BloomFilter bloom = BloomFilter::Create(2000, 0.01);
  std::vector<std::string> present, absent;
  for (int i = 0; i < 2000; ++i) {
    const std::string key = "present-" + std::to_string(i);
    present.push_back(key);
    bloom.Add(key);
  }
  for (int i = 0; i < 20000; ++i) absent.push_back("absent-" + std::to_string(i));
  const double fpr = bloom.measure_fpr(present, absent);
  CHECK(fpr >= 0.0);          // negative means a present key was missed: hashing bug
  CHECK(fpr < 0.05);          // 5x headroom over the 1% target
}

TEST(bloom_survives_serialization) {
  BloomFilter original = BloomFilter::Create(500, 0.01);
  for (int i = 0; i < 500; ++i) original.Add("k" + std::to_string(i));
  const std::string bytes = original.Serialize();

  BloomFilter restored;
  CHECK(BloomFilter::Deserialize(bytes, &restored));
  for (int i = 0; i < 500; ++i) CHECK(restored.MayContain("k" + std::to_string(i)));
  CHECK_EQ(restored.num_hashes(), original.num_hashes());
}

TEST(skiplist_orders_by_key_then_sequence_desc) {
  SkipList<std::string, std::string> list;
  list.Insert("b", 5, "b5", false);
  list.Insert("a", 2, "a2", false);
  list.Insert("a", 7, "a7", false);
  list.Insert("a", 3, "a3", false);
  list.Insert("c", 1, "c1", false);

  std::vector<std::pair<std::string, uint64_t>> seen;
  for (auto* node = list.First(); node != nullptr; node = node->next[0]) {
    seen.emplace_back(node->key, node->seq);
  }
  CHECK_EQ(seen.size(), static_cast<size_t>(5));
  // "a" first, newest version first.
  CHECK_EQ(seen[0].first, std::string("a"));
  CHECK_EQ(seen[0].second, static_cast<uint64_t>(7));
  CHECK_EQ(seen[1].second, static_cast<uint64_t>(3));
  CHECK_EQ(seen[2].second, static_cast<uint64_t>(2));
  CHECK_EQ(seen[3].first, std::string("b"));
  CHECK_EQ(seen[4].first, std::string("c"));
}

TEST(skiplist_seekge_respects_snapshot) {
  SkipList<std::string, std::string> list;
  list.Insert("k", 10, "v10", false);
  list.Insert("k", 20, "v20", false);
  list.Insert("k", 30, "v30", false);

  // Snapshot 25 must land on the seq<=25 version, i.e. seq 20.
  auto* node = list.SeekGE("k", 25);
  CHECK(node != nullptr);
  CHECK_EQ(node->seq, static_cast<uint64_t>(20));

  // Snapshot newer than everything returns the newest.
  node = list.SeekGE("k", 100);
  CHECK_EQ(node->seq, static_cast<uint64_t>(30));

  // Snapshot older than everything: nothing visible.
  node = list.SeekGE("k", 5);
  CHECK(node == nullptr || node->key != "k");
}

TEST(memtable_reports_tombstones_without_hiding_them) {
  MemTable table;
  table.Insert(1, "gone", "value", false);
  table.Insert(2, "gone", "", true);

  std::string value;
  bool deleted = false;
  CHECK(table.Get("gone", 10, &value, &deleted));
  CHECK(deleted);  // caller must see the tombstone and stop searching

  // At snapshot 1 the delete is not yet visible.
  CHECK(table.Get("gone", 1, &value, &deleted));
  CHECK(!deleted);
  CHECK_EQ(value, std::string("value"));

  CHECK(!table.Get("absent", 10, &value, &deleted));
}

TEST(human_bytes_formats_sensibly) {
  CHECK_EQ(HumanBytes(512), std::string("512 B"));
  CHECK_EQ(HumanBytes(1024), std::string("1.0 KiB"));
  CHECK_EQ(HumanBytes(1536), std::string("1.5 KiB"));
}
