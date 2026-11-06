// Tests for the SSTable format: round-trip, snapshots, ordering, corruption.
#include <cstdio>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include "test_framework.hpp"
#include "trove/sstable.hpp"

using namespace trove;
namespace fs = std::filesystem;

namespace {

std::string TempDir(const std::string& tag) {
  static std::mt19937_64 rng(54321);
  const std::string name = "trove-test-" + tag + "-" + std::to_string(rng());
  const fs::path path = fs::temp_directory_path() / name;
  std::error_code ec;
  fs::remove_all(path, ec);
  fs::create_directories(path, ec);
  return path.string();
}

}  // namespace

TEST(sstable_roundtrip_and_point_lookup) {
  const std::string dir = TempDir("sst");
  const std::string path = dir + "/sst-1.sst";
  std::string error;

  constexpr int kCount = 5000;
  {
    auto writer = SSTableWriter::Create(path, SSTableOptions{}, &error);
    CHECK(writer != nullptr);
    for (int i = 0; i < kCount; ++i) {
      char key[32];
      std::snprintf(key, sizeof(key), "key-%06d", i);
      CHECK(writer->Add(key, static_cast<uint64_t>(kCount - i), "value-" + std::to_string(i), false));
    }
    CHECK(writer->Finish(&error));
    CHECK_EQ(writer->record_count(), static_cast<uint64_t>(kCount));
    CHECK(writer->file_size() > 0);
  }

  auto reader = SSTableReader::Open(path, &error);
  CHECK(reader != nullptr);
  CHECK_EQ(reader->record_count(), static_cast<uint64_t>(kCount));
  CHECK_EQ(reader->smallest_key(), std::string("key-000000"));
  CHECK_EQ(reader->largest_key(), std::string("key-004999"));
  CHECK(reader->num_blocks() > 1);

  // Hits
  for (int i = 0; i < kCount; i += 137) {
    char key[32];
    std::snprintf(key, sizeof(key), "key-%06d", i);
    std::string value;
    bool deleted = true;
    CHECK(reader->Get(key, UINT64_MAX, &value, &deleted));
    CHECK(!deleted);
    CHECK_EQ(value, "value-" + std::to_string(i));
  }

  // Misses below, inside and above the key range.
  for (const char* key : {"aaa", "key-000000x", "key-0025005", "zzz"}) {
    std::string value;
    bool deleted = false;
    CHECK(!reader->Get(key, UINT64_MAX, &value, &deleted));
  }

  // Full iteration must be strictly ordered.
  std::string previous;
  size_t seen = 0;
  for (auto it = reader->NewIterator(); it->Valid(); it->Next()) {
    if (seen > 0) CHECK(previous < std::string(it->Key()));
    previous.assign(it->Key().data(), it->Key().size());
    ++seen;
  }
  CHECK_EQ(seen, static_cast<size_t>(kCount));

  reader.reset();  // Windows will not unlink a file that still has an open handle
  fs::remove_all(dir);
}

TEST(sstable_honours_snapshot_sequence) {
  const std::string dir = TempDir("sstsnap");
  const std::string path = dir + "/sst-1.sst";
  std::string error;

  {
    auto writer = SSTableWriter::Create(path, SSTableOptions{}, &error);
    CHECK(writer != nullptr);
    CHECK(writer->Add("k", 30, "v30", false));
    CHECK(writer->Add("k", 20, "v20", false));
    CHECK(writer->Add("k", 10, "v10", false));
    CHECK(writer->Finish(&error));
  }

  auto reader = SSTableReader::Open(path, &error);
  CHECK(reader != nullptr);

  std::string value;
  bool deleted = false;
  CHECK(reader->Get("k", 25, &value, &deleted));
  CHECK_EQ(value, std::string("v20"));
  CHECK(reader->Get("k", 5, &value, &deleted) == false);

  reader.reset();  // Windows will not unlink a file that still has an open handle
  fs::remove_all(dir);
}

TEST(sstable_rejects_unsorted_input) {
  const std::string dir = TempDir("sstunsorted");
  const std::string path = dir + "/sst-1.sst";
  std::string error;

  auto writer = SSTableWriter::Create(path, SSTableOptions{}, &error);
  CHECK(writer != nullptr);
  CHECK(writer->Add("b", 2, "x", false));
  CHECK(!writer->Add("a", 1, "y", false));  // key went backwards
  CHECK(!writer->Add("b", 3, "z", false));  // same key with a newer seq must come first

  writer.reset();  // Windows will not unlink a file that still has an open handle
  fs::remove_all(dir);
}

TEST(sstable_detects_block_corruption) {
  const std::string dir = TempDir("sstcorrupt");
  const std::string path = dir + "/sst-1.sst";
  std::string error;

  {
    auto writer = SSTableWriter::Create(path, SSTableOptions{}, &error);
    CHECK(writer != nullptr);
    for (int i = 0; i < 200; ++i) {
      char key[32];
      std::snprintf(key, sizeof(key), "key-%06d", i);
      CHECK(writer->Add(key, static_cast<uint64_t>(i + 1), "value-" + std::to_string(i), false));
    }
    CHECK(writer->Finish(&error));
  }

  // Flip one bit inside the first block's payload (past its 4-byte CRC).
  {
    std::FILE* f = std::fopen(path.c_str(), "r+b");
    CHECK(f != nullptr);
    CHECK(std::fseek(f, 16, SEEK_SET) == 0);
    int byte = std::fgetc(f);
    CHECK(byte != EOF);
    CHECK(std::fseek(f, 16, SEEK_SET) == 0);
    CHECK(std::fputc(byte ^ 0x01, f) != EOF);
    std::fclose(f);
  }

  auto reader = SSTableReader::Open(path, &error);
  CHECK(reader != nullptr);  // footer/index still intact; only the payload is damaged

  // The damaged block must not be served. A CRC failure surfaces as "no value"
  // rather than as silently wrong bytes.
  std::string value;
  bool deleted = false;
  CHECK(!reader->Get("key-000000", UINT64_MAX, &value, &deleted));

  // Iteration stops at the damaged block instead of yielding garbage.
  size_t seen = 0;
  for (auto it = reader->NewIterator(); it->Valid(); it->Next()) ++seen;
  CHECK(seen == 0);

  reader.reset();
  fs::remove_all(dir);
}
