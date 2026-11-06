// trove/bench.cpp — small workload harness.
//
// Deliberately not a YCSB clone. The point is to produce honest numbers for the
// README: throughput of the write path, the read path at several hit rates, and
// how much space the data actually occupies after compaction.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include "trove/db.hpp"
#include "trove/util.hpp"

namespace fs = std::filesystem;

namespace {

using Clock = std::chrono::steady_clock;

double SecondsSince(Clock::time_point start) {
  return std::chrono::duration<double>(Clock::now() - start).count();
}

uint64_t DirectoryBytes(const std::string& dir) {
  uint64_t total = 0;
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(dir, ec)) {
    if (ec) break;
    if (entry.is_regular_file(ec)) total += entry.file_size(ec);
  }
  return total;
}

void Report(const char* name, size_t ops, double seconds, uint64_t bytes) {
  const double per_sec = seconds > 0 ? static_cast<double>(ops) / seconds : 0.0;
  const double mb_per_sec = seconds > 0 ? static_cast<double>(bytes) / seconds / (1024.0 * 1024.0) : 0.0;
  std::printf("%-34s %9zu ops  %8.2f s  %12.0f ops/s  %8.1f MiB/s\n", name, ops, seconds, per_sec,
              mb_per_sec);
}

std::string MakeKey(int i) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "user:%08d", i);
  return std::string(buf);
}

}  // namespace

int main(int argc, char** argv) {
  size_t n = 200000;
  for (int i = 1; i < argc; ++i) {
    if (std::string(argv[i]) == "--n" && i + 1 < argc) {
      n = static_cast<size_t>(std::stoull(argv[++i]));
    }
  }

  const fs::path dir = fs::temp_directory_path() / "trove-bench";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);

  trove::Options opts;
  opts.write_buffer_size = 4u << 20;
  opts.l0_compaction_trigger = 4;
  opts.target_file_size = 8u << 20;

  std::string error;
  auto db = trove::DB::Open(dir.string(), opts, &error);
  if (db == nullptr) {
    std::fprintf(stderr, "open failed: %s\n", error.c_str());
    return 1;
  }

  const std::string value(64, 'x');
  const uint64_t value_bytes = value.size();

  std::printf("trove benchmark  n=%zu  value=%lluB  write_buffer=%s\n\n", n,
              static_cast<unsigned long long>(value_bytes), trove::HumanBytes(opts.write_buffer_size).c_str());

  // 1. Sequential fill.
  {
    const auto start = Clock::now();
    bool ok = true;
    for (size_t i = 0; i < n && ok; ++i) ok = db->Put(MakeKey(static_cast<int>(i)), value, &error);
    const double seconds = SecondsSince(start);
    if (!ok) {
      std::fprintf(stderr, "put failed: %s\n", error.c_str());
      return 1;
    }
    Report("sequential put", n, seconds, n * (value_bytes + 16));
  }

  // 2. Random overwrite of the existing key space.
  {
    std::mt19937 rng(7);
    std::uniform_int_distribution<int> dist(0, static_cast<int>(n) - 1);
    const size_t ops = n / 4;
    const auto start = Clock::now();
    for (size_t i = 0; i < ops; ++i) db->Put(MakeKey(dist(rng)), value, &error);
    Report("random overwrite", ops, SecondsSince(start), ops * (value_bytes + 16));
  }

  db->Flush(&error);

  // 3. Random point reads that hit.
  {
    std::mt19937 rng(11);
    std::uniform_int_distribution<int> dist(0, static_cast<int>(n) - 1);
    const size_t ops = n;
    size_t hits = 0;
    const auto start = Clock::now();
    for (size_t i = 0; i < ops; ++i) {
      std::string out;
      bool found = false;
      db->Get(MakeKey(dist(rng)), &out, &found, &error);
      if (found) ++hits;
    }
    const double seconds = SecondsSince(start);
    Report("random get (hit)", ops, seconds, ops * value_bytes);
    std::printf("%-34s %9zu hits\n", "", hits);
  }

  // 4. Random point reads that miss (exercises the bloom filters).
  {
    const size_t ops = n / 2;
    const auto start = Clock::now();
    for (size_t i = 0; i < ops; ++i) {
      std::string out;
      bool found = false;
      db->Get("absent:" + MakeKey(static_cast<int>(i)), &out, &found, &error);
    }
    Report("random get (miss, bloom)", ops, SecondsSince(start), 0);
  }

  // 5. Full ordered scan.
  {
    const auto start = Clock::now();
    std::vector<std::pair<std::string, std::string>> rows;
    db->Scan("", "", 0, &rows, &error);
    const double seconds = SecondsSince(start);
    uint64_t bytes = 0;
    for (const auto& [k, v] : rows) bytes += k.size() + v.size();
    Report("full scan", rows.size(), seconds, bytes);
    std::printf("%-34s %9zu rows returned\n", "", rows.size());
  }

  db->CompactAll(&error);

  const trove::Stats stats = db->GetStats();
  std::printf("\nengine state\n");
  std::printf("  last sequence      %llu\n", static_cast<unsigned long long>(stats.last_sequence));
  std::printf("  sstables           %llu files, %s, %llu records\n",
              static_cast<unsigned long long>(stats.num_files),
              trove::HumanBytes(stats.sstable_bytes).c_str(),
              static_cast<unsigned long long>(stats.sstable_records));
  std::printf("  memtable           %s\n", trove::HumanBytes(stats.memtable_bytes).c_str());
  std::printf("  flushes            %llu\n", static_cast<unsigned long long>(stats.flushes));
  std::printf("  compactions        %llu\n", static_cast<unsigned long long>(stats.compactions));
  for (size_t level = 0; level < stats.level_files_bytes.size(); ++level) {
    std::printf("  L%-17zu %llu files, %s\n", level,
                static_cast<unsigned long long>(stats.level_files_bytes[level].first),
                trove::HumanBytes(stats.level_files_bytes[level].second).c_str());
  }
  std::printf("  on-disk total      %s\n", trove::HumanBytes(DirectoryBytes(dir.string())).c_str());

  db.reset();
  fs::remove_all(dir, ec);
  return 0;
}
