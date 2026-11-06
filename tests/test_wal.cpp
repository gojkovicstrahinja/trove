// Tests for the write-ahead log: framing, replay, and torn-tail recovery.
#include <cstdio>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include "test_framework.hpp"
#include "trove/wal.hpp"

using namespace trove;
namespace fs = std::filesystem;

namespace {

std::string TempDir(const std::string& tag) {
  static std::mt19937_64 rng(12345);
  const std::string name = "trove-test-" + tag + "-" + std::to_string(rng());
  const fs::path path = fs::temp_directory_path() / name;
  std::error_code ec;
  fs::remove_all(path, ec);
  fs::create_directories(path, ec);
  return path.string();
}

}  // namespace

TEST(wal_roundtrip_preserves_records) {
  const std::string dir = TempDir("wal");
  const std::string path = dir + "/wal-1.log";

  {
    std::string error;
    auto wal = WAL::Open(path, &error);
    CHECK(wal != nullptr);
    CHECK(wal->Append(1, RecordType::kPut, "alpha", "one"));
    CHECK(wal->Append(2, RecordType::kPut, "beta", "two"));
    CHECK(wal->Append(3, RecordType::kDelete, "alpha", ""));
    CHECK(wal->Sync());
  }

  std::vector<WalRecord> records;
  uint64_t max_seq = 0;
  bool torn = false;
  std::string error;
  CHECK(WAL::Replay(path, [&](const WalRecord& r) { records.push_back(r); }, &max_seq, &torn, &error));
  CHECK(!torn);
  CHECK_EQ(records.size(), static_cast<size_t>(3));
  CHECK_EQ(max_seq, static_cast<uint64_t>(3));
  CHECK_EQ(records[0].key, std::string("alpha"));
  CHECK_EQ(records[0].value, std::string("one"));
  CHECK(records[2].type == RecordType::kDelete);

  fs::remove_all(dir);
}

TEST(wal_truncates_torn_tail_and_keeps_prefix) {
  const std::string dir = TempDir("waltorn");
  const std::string path = dir + "/wal-1.log";

  {
    std::string error;
    auto wal = WAL::Open(path, &error);
    CHECK(wal != nullptr);
    CHECK(wal->Append(1, RecordType::kPut, "keep", "me"));
    CHECK(wal->Append(2, RecordType::kPut, "also", "keep"));
    CHECK(wal->Sync());
  }

  // Simulate a power loss mid-append: a length prefix followed by a truncated body.
  {
    std::FILE* f = std::fopen(path.c_str(), "ab");
    CHECK(f != nullptr);
    const unsigned char junk[11] = {0x40, 0x00, 0x00, 0x00, 0xde, 0xad, 0xbe, 0xef, 0x01, 0x02, 0x03};
    CHECK(std::fwrite(junk, 1, sizeof(junk), f) == sizeof(junk));
    std::fclose(f);
  }

  std::vector<WalRecord> records;
  uint64_t max_seq = 0;
  bool torn = false;
  std::string error;
  CHECK(WAL::Replay(path, [&](const WalRecord& r) { records.push_back(r); }, &max_seq, &torn, &error));
  CHECK(torn);  // we must notice the damage
  CHECK_EQ(records.size(), static_cast<size_t>(2));

  // The next append must land on a clean boundary and remain replayable.
  {
    auto wal = WAL::Open(path, &error);
    CHECK(wal != nullptr);
    CHECK(wal->Append(3, RecordType::kPut, "after", "recovery"));
    CHECK(wal->Sync());
  }
  std::vector<WalRecord> second;
  torn = false;
  CHECK(WAL::Replay(path, [&](const WalRecord& r) { second.push_back(r); }, &max_seq, &torn, &error));
  CHECK(!torn);
  CHECK_EQ(second.size(), static_cast<size_t>(3));

  fs::remove_all(dir);
}
