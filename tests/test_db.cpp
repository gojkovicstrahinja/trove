// Tests for the engine as a whole: durability, recovery, scans and compaction.
#include <cstdio>
#include <filesystem>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "test_framework.hpp"
#include "trove/db.hpp"

using namespace trove;
namespace fs = std::filesystem;

namespace {

std::string TempDir(const std::string& tag) {
  static std::mt19937_64 rng(98765);
  const std::string name = "trove-test-" + tag + "-" + std::to_string(rng());
  const fs::path path = fs::temp_directory_path() / name;
  std::error_code ec;
  fs::remove_all(path, ec);
  fs::create_directories(path, ec);
  return path.string();
}

}  // namespace

TEST(db_put_get_delete) {
  const std::string dir = TempDir("db");
  std::string error;
  Options opts;
  auto db = DB::Open(dir, opts, &error);
  CHECK(db != nullptr);

  CHECK(db->Put("alpha", "one", &error));
  CHECK(db->Put("beta", "two", &error));

  std::string value;
  bool found = false;
  CHECK(db->Get("alpha", &value, &found, &error));
  CHECK(found);
  CHECK_EQ(value, std::string("one"));

  CHECK(db->Get("nope", &value, &found, &error));
  CHECK(!found);

  CHECK(db->Put("alpha", "one-updated", &error));
  CHECK(db->Get("alpha", &value, &found, &error));
  CHECK_EQ(value, std::string("one-updated"));

  CHECK(db->Delete("beta", &error));
  CHECK(db->Get("beta", &value, &found, &error));
  CHECK(!found);

  db.reset();
  fs::remove_all(dir);
}

TEST(db_survives_reopen_via_wal) {
  const std::string dir = TempDir("dbreopen");
  std::string error;
  Options opts;
  opts.write_buffer_size = 1u << 20;  // keep everything in the memtable

  {
    auto db = DB::Open(dir, opts, &error);
    CHECK(db != nullptr);
    for (int i = 0; i < 500; ++i) {
      CHECK(db->Put("key-" + std::to_string(i), "value-" + std::to_string(i), &error));
    }
    CHECK(db->Delete("key-42", &error));
    // No flush: destruction must leave enough behind for recovery.
  }

  {
    auto db = DB::Open(dir, opts, &error);
    CHECK(db != nullptr);
    std::string value;
    bool found = false;
    for (int i = 0; i < 500; ++i) {
      if (i == 42) continue;
      CHECK(db->Get("key-" + std::to_string(i), &value, &found, &error));
      CHECK(found);
      CHECK_EQ(value, "value-" + std::to_string(i));
    }
    CHECK(db->Get("key-42", &value, &found, &error));
    CHECK(!found);
  }

  fs::remove_all(dir);
}

TEST(db_flush_moves_data_to_sstable) {
  const std::string dir = TempDir("dbflush");
  std::string error;
  Options opts;
  auto db = DB::Open(dir, opts, &error);
  CHECK(db != nullptr);

  for (int i = 0; i < 100; ++i) CHECK(db->Put("k" + std::to_string(i), "v" + std::to_string(i), &error));
  CHECK(db->Flush(&error));

  Stats stats = db->GetStats();
  CHECK_EQ(stats.num_files, static_cast<uint64_t>(1));
  CHECK_EQ(stats.memtable_entries, static_cast<uint64_t>(0));
  CHECK(stats.sstable_records >= 100);

  std::string value;
  bool found = false;
  CHECK(db->Get("k50", &value, &found, &error));
  CHECK(found);
  CHECK_EQ(value, std::string("v50"));

  db.reset();
  fs::remove_all(dir);
}

TEST(db_flush_then_delete_is_not_resurrected) {
  const std::string dir = TempDir("dbtomb");
  std::string error;
  Options opts;
  auto db = DB::Open(dir, opts, &error);
  CHECK(db != nullptr);

  for (int i = 0; i < 50; ++i) CHECK(db->Put("k" + std::to_string(i), "old", &error));
  CHECK(db->Flush(&error));

  // Tombstone in the memtable must shadow the older version sitting in L0.
  for (int i = 0; i < 50; i += 2) CHECK(db->Delete("k" + std::to_string(i), &error));

  std::string value;
  bool found = false;
  for (int i = 0; i < 50; ++i) {
    CHECK(db->Get("k" + std::to_string(i), &value, &found, &error));
    if (i % 2 == 0) {
      CHECK(!found);
    } else {
      CHECK(found);
      CHECK_EQ(value, std::string("old"));
    }
  }
  db.reset();
  fs::remove_all(dir);
}

TEST(db_scan_returns_sorted_half_open_range) {
  const std::string dir = TempDir("dbscan");
  std::string error;
  Options opts;
  auto db = DB::Open(dir, opts, &error);
  CHECK(db != nullptr);

  for (int i = 0; i < 20; ++i) {
    char key[16];
    std::snprintf(key, sizeof(key), "k%02d", i);
    CHECK(db->Put(key, "v" + std::to_string(i), &error));
  }
  CHECK(db->Delete("k05", &error));
  CHECK(db->Flush(&error));

  std::vector<std::pair<std::string, std::string>> rows;
  CHECK(db->Scan("k03", "k08", 0, &rows, &error));
  std::vector<std::string> keys;
  for (const auto& [k, v] : rows) keys.push_back(k);
  // k05 was deleted, end is exclusive at k08.
  CHECK_EQ(keys.size(), static_cast<size_t>(4));
  CHECK_EQ(keys.front(), std::string("k03"));
  CHECK_EQ(keys.back(), std::string("k07"));

  // Limit is respected.
  rows.clear();
  CHECK(db->Scan("", "", 3, &rows, &error));
  CHECK_EQ(rows.size(), static_cast<size_t>(3));

  db.reset();
  fs::remove_all(dir);
}

TEST(db_compaction_preserves_every_key) {
  const std::string dir = TempDir("dbcompact");
  std::string error;
  Options opts;
  opts.write_buffer_size = 2048;   // tiny: force many flushes
  opts.l0_compaction_trigger = 2;  // and force compaction
  opts.level_size_multiplier = 2;
  opts.target_file_size = 4096;

  auto db = DB::Open(dir, opts, &error);
  CHECK(db != nullptr);

  constexpr int kKeys = 400;
  for (int round = 0; round < 3; ++round) {
    for (int i = 0; i < kKeys; ++i) {
      CHECK(db->Put("key-" + std::to_string(i), "round-" + std::to_string(round), &error));
    }
  }
  CHECK(db->CompactAll(&error));

  std::string value;
  bool found = false;
  for (int i = 0; i < kKeys; ++i) {
    CHECK(db->Get("key-" + std::to_string(i), &value, &found, &error));
    CHECK(found);
    CHECK_EQ(value, std::string("round-2"));
  }

  Stats stats = db->GetStats();
  CHECK(stats.compactions > 0);
  CHECK(stats.num_files >= 1);

  db.reset();

  // Everything must still be there after a reopen of the compacted layout.
  {
    auto reopened = DB::Open(dir, opts, &error);
    CHECK(reopened != nullptr);
    for (int i = 0; i < kKeys; i += 17) {
      CHECK(reopened->Get("key-" + std::to_string(i), &value, &found, &error));
      CHECK(found);
      CHECK_EQ(value, std::string("round-2"));
    }
  }

  fs::remove_all(dir);
}

TEST(db_random_ops_match_reference_model) {
  const std::string dir = TempDir("dbfuzz");
  std::string error;
  Options opts;
  opts.write_buffer_size = 1024;
  opts.l0_compaction_trigger = 2;
  opts.level_size_multiplier = 2;
  opts.target_file_size = 2048;

  auto db = DB::Open(dir, opts, &error);
  CHECK(db != nullptr);

  std::map<std::string, std::string> model;
  std::mt19937 rng(2024);
  std::uniform_int_distribution<int> key_dist(0, 199);
  std::uniform_int_distribution<int> op_dist(0, 99);

  for (int step = 0; step < 4000; ++step) {
    const std::string key = "key-" + std::to_string(key_dist(rng));
    const int roll = op_dist(rng);

    if (roll < 55) {
      const std::string value = "v-" + std::to_string(step);
      CHECK(db->Put(key, value, &error));
      model[key] = value;
    } else if (roll < 80) {
      CHECK(db->Delete(key, &error));
      model.erase(key);
    } else {
      std::string value;
      bool found = false;
      CHECK(db->Get(key, &value, &found, &error));
      const auto it = model.find(key);
      CHECK_EQ(found, it != model.end());
      if (found) CHECK_EQ(value, it->second);
    }
  }

  // Full cross-check through the merge path.
  std::vector<std::pair<std::string, std::string>> rows;
  CHECK(db->Scan("", "", 0, &rows, &error));
  CHECK_EQ(rows.size(), model.size());
  auto expected = model.begin();
  for (const auto& row : rows) {
    CHECK_EQ(row.first, expected->first);
    CHECK_EQ(row.second, expected->second);
    ++expected;
  }

  db.reset();
  fs::remove_all(dir);
}

TEST(db_sync_writes_mode_is_durable_and_correct) {
  const std::string dir = TempDir("dbsync");
  std::string error;
  Options opts;
  opts.sync_writes = true;
  auto db = DB::Open(dir, opts, &error);
  CHECK(db != nullptr);
  for (int i = 0; i < 20; ++i) CHECK(db->Put("s" + std::to_string(i), "v", &error));

  std::string value;
  bool found = false;
  CHECK(db->Get("s7", &value, &found, &error));
  CHECK(found);
  db.reset();
  fs::remove_all(dir);
}

TEST(db_refuses_to_open_when_manifest_is_missing) {
  const std::string dir = TempDir("dbnomanifest");
  std::string error;
  Options opts;

  {
    auto db = DB::Open(dir, opts, &error);
    CHECK(db != nullptr);
    for (int i = 0; i < 20; ++i) CHECK(db->Put("k" + std::to_string(i), "v", &error));
    CHECK(db->Flush(&error));
  }

  // Losing the manifest must not silently present an empty database whose
  // tables then get garbage-collected by the next compaction.
  CHECK(fs::remove(dir + "/MANIFEST"));

  auto db = DB::Open(dir, opts, &error);
  CHECK(db == nullptr);
  CHECK(error.find("MANIFEST") != std::string::npos);

  fs::remove_all(dir);
}
