#include "trove/db.hpp"

#include <algorithm>
#include <filesystem>
#include <limits>

#include "trove/manifest.hpp"
#include "trove/merge_iterator.hpp"

namespace trove {
namespace fs = std::filesystem;

namespace {
// FlushLocked is the real implementation; the public front door only adds a lock
// so that the write path (which already holds it) can call straight through.
}  // namespace

DB::~DB() {
  std::lock_guard<std::mutex> lock(mu_);
  if (wal_ != nullptr) wal_->Sync();  // best effort: WAL is the durability boundary
}

std::unique_ptr<DB> DB::Open(const std::string& dir, const Options& options, std::string* error) {
  std::unique_ptr<DB> db(new DB());
  db->dir_ = dir;
  db->opts_ = options;
  if (db->opts_.max_levels < 2) db->opts_.max_levels = 2;
  if (db->opts_.target_file_size == 0) db->opts_.target_file_size = 4u << 20;
  if (db->opts_.l0_compaction_trigger == 0) db->opts_.l0_compaction_trigger = 4;

  std::error_code ec;
  fs::create_directories(dir, ec);
  if (ec) {
    if (error != nullptr) *error = "cannot create database directory: " + dir + " (" + ec.message() + ")";
    return nullptr;
  }

  db->mem_ = std::make_unique<MemTable>();

  const bool had_manifest = fs::exists(db->FilePath("MANIFEST"), ec);
  if (!Manifest::Load(db->FilePath("MANIFEST"), &db->manifest_, error)) return nullptr;

  if (!had_manifest) {
    // No manifest but tables on disk means the metadata was lost. Opening as an
    // empty database would silently orphan (and eventually delete) good data, so
    // refuse and make the operator decide.
    size_t orphan_tables = 0;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
      if (ec) break;
      if (entry.path().extension() == ".sst") ++orphan_tables;
    }
    if (orphan_tables > 0) {
      if (error != nullptr) {
        *error = "MANIFEST is missing but " + std::to_string(orphan_tables) +
                 " SSTable(s) are present in " + dir + "; refusing to open and orphan them";
      }
      return nullptr;
    }
  }

  db->manifest_.levels.resize(static_cast<size_t>(db->opts_.max_levels));
  db->levels_.resize(static_cast<size_t>(db->opts_.max_levels));

  // Open every table the manifest claims is live. A missing file is fatal: it
  // means the manifest and disk disagree, and quietly continuing would serve
  // truncated data.
  for (size_t level = 0; level < db->manifest_.levels.size(); ++level) {
    for (const auto& meta : db->manifest_.levels[level]) {
      const std::string path = db->FilePath(db->SstName(meta.number));
      auto reader = SSTableReader::Open(path, error);
      if (reader == nullptr) return nullptr;
      db->levels_[level].push_back(FileHandle{meta, std::move(reader)});
    }
  }

  // The sequence counter must be reconstructed from BOTH places versions can
  // live. A rotated-away WAL has nothing to replay, so taking the max from the
  // log alone would restart at 0 and make every SSTable version look newer than
  // the snapshot — i.e. invisible. This is the recovery bug the compaction test
  // catches.
  uint64_t highest_seq = 0;
  for (const auto& level : db->levels_) {
    for (const auto& file : level) highest_seq = std::max(highest_seq, file.reader->max_seq());
  }
  db->last_seq_ = highest_seq;

  if (!db->Recover(error)) return nullptr;
  if (!db->SaveManifest(error)) return nullptr;
  return db;
}

bool DB::Recover(std::string* error) {
  std::vector<std::pair<uint64_t, std::string>> wals;
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(dir_, ec)) {
    if (ec) break;
    const std::string name = entry.path().filename().string();
    if (name.rfind("wal-", 0) != 0) continue;
    const auto dash = name.find('-');
    const auto dot = name.find('.');
    if (dash == std::string::npos || dot == std::string::npos || dot < dash) continue;
    uint64_t number = 0;
    try {
      number = std::stoull(name.substr(dash + 1, dot - dash - 1));
    } catch (...) {
      continue;
    }
    wals.emplace_back(number, name);
  }
  std::sort(wals.begin(), wals.end());

  uint64_t max_seq = last_seq_;  // seed from SSTables; WAL may only push it higher
  bool any_torn = false;
  for (const auto& [number, name] : wals) {
    bool torn = false;
    const bool ok = WAL::Replay(
        FilePath(name),
        [this](const WalRecord& rec) {
          mem_->Insert(rec.seq, rec.key, rec.value, rec.type == RecordType::kDelete);
        },
        &max_seq, &torn, error);
    if (!ok) return false;
    any_torn = any_torn || torn;
  }
  last_seq_ = max_seq;
  (void)any_torn;

  if (wals.size() == 1) {
    // Normal case: keep appending to the existing log. Replaying it into the
    // memtable and immediately re-flushing would be wasted work.
    wal_number_ = wals.front().first;
    wal_ = WAL::Open(FilePath(WalName(wal_number_)), error);
    if (wal_ == nullptr) return false;
    return true;
  }

  if (!wals.empty()) {
    // Multiple logs (interrupted rotation). Fold everything into a table before
    // discarding the logs, or the replayed data would only exist in RAM.
    if (!mem_->Empty() && !FlushLocked(error)) return false;
    for (const auto& [number, name] : wals) {
      const std::string path = FilePath(name);
      fs::remove(path, ec);
    }
  }

  wal_number_ = manifest_.next_file_number++;
  wal_ = WAL::Open(FilePath(WalName(wal_number_)), error);
  return wal_ != nullptr;
}

bool DB::SaveManifest(std::string* error) { return manifest_.Save(FilePath("MANIFEST"), error); }

bool DB::Put(std::string_view key, std::string_view value, std::string* error) {
  std::lock_guard<std::mutex> lock(mu_);
  if (wal_ == nullptr) {
    if (error != nullptr) *error = "database is closed";
    return false;
  }
  const uint64_t seq = ++last_seq_;
  // Log before mutating memory: a crash between the two is recoverable, the
  // reverse order silently loses an acknowledged write.
  if (!wal_->Append(seq, RecordType::kPut, key, value)) {
    if (error != nullptr) *error = "WAL append failed";
    return false;
  }
  if (opts_.sync_writes && !wal_->Sync()) {
    if (error != nullptr) *error = "WAL fsync failed";
    return false;
  }
  mem_->Insert(seq, key, value, false);
  return MaybeFlush(error);
}

bool DB::Delete(std::string_view key, std::string* error) {
  std::lock_guard<std::mutex> lock(mu_);
  if (wal_ == nullptr) {
    if (error != nullptr) *error = "database is closed";
    return false;
  }
  const uint64_t seq = ++last_seq_;
  if (!wal_->Append(seq, RecordType::kDelete, key, {})) {
    if (error != nullptr) *error = "WAL append failed";
    return false;
  }
  if (opts_.sync_writes && !wal_->Sync()) {
    if (error != nullptr) *error = "WAL fsync failed";
    return false;
  }
  mem_->Insert(seq, key, {}, true);
  return MaybeFlush(error);
}

bool DB::Get(std::string_view key, std::string* value, bool* found, std::string* error) {
  std::lock_guard<std::mutex> lock(mu_);
  if (found != nullptr) *found = false;
  if (value != nullptr) value->clear();
  (void)error;

  const uint64_t snapshot = last_seq_;
  std::string v;
  bool deleted = false;

  auto accept = [&](bool hit) {
    if (!hit) return false;
    if (!deleted) {
      if (value != nullptr) *value = std::move(v);
      if (found != nullptr) *found = true;
    }
    return true;  // hit, including a tombstone: stop searching
  };

  if (accept(mem_->Get(key, snapshot, &v, &deleted))) return true;
  if (imm_ != nullptr) {
    v.clear();
    if (accept(imm_->Get(key, snapshot, &v, &deleted))) return true;
  }

  // L0 overlaps, so newest file number must win.
  const auto& l0 = levels_[0];
  for (auto it = l0.rbegin(); it != l0.rend(); ++it) {
    v.clear();
    if (accept(it->reader->Get(key, snapshot, &v, &deleted))) return true;
  }
  for (size_t level = 1; level < levels_.size(); ++level) {
    for (auto it = levels_[level].rbegin(); it != levels_[level].rend(); ++it) {
      v.clear();
      if (accept(it->reader->Get(key, snapshot, &v, &deleted))) return true;
    }
  }
  return true;
}

bool DB::Scan(std::string_view start, std::string_view end, size_t limit,
              std::vector<std::pair<std::string, std::string>>* out, std::string* error) {
  std::lock_guard<std::mutex> lock(mu_);
  (void)error;
  if (out == nullptr) return false;
  out->clear();

  std::vector<std::unique_ptr<EntryIterator>> sources;
  sources.push_back(mem_->NewIterator());
  if (imm_ != nullptr) sources.push_back(imm_->NewIterator());
  for (auto& level : levels_) {
    for (auto& file : level) sources.push_back(file.reader->NewIterator());
  }

  MergeIterator merge(std::move(sources), last_seq_);
  for (; merge.Valid(); merge.Next()) {
    if (!start.empty() && merge.Key() < start) continue;
    if (!end.empty() && merge.Key() >= end) break;
    if (merge.Deleted()) continue;  // tombstones resolve to "absent", never emitted
    out->emplace_back(std::string(merge.Key()), std::string(merge.Value()));
    if (limit > 0 && out->size() >= limit) break;
  }
  return true;
}

bool DB::FlushLocked(std::string* error) {
  if (mem_ == nullptr || mem_->Empty()) return true;

  imm_ = std::move(mem_);
  mem_ = std::make_unique<MemTable>();

  const uint64_t number = manifest_.next_file_number++;
  const std::string sst_path = FilePath(SstName(number));
  auto writer = SSTableWriter::Create(sst_path, opts_.sstable, error);
  if (writer == nullptr) {
    mem_ = std::move(imm_);  // restore so writes keep working
    return false;
  }

  for (auto it = imm_->NewIterator(); it->Valid(); it->Next()) {
    if (!writer->Add(it->Key(), it->Seq(), it->Value(), it->Deleted())) {
      if (error != nullptr) *error = "flush: memtable iterator produced unsorted output";
      mem_ = std::move(imm_);
      return false;
    }
  }
  if (!writer->Finish(error)) {
    mem_ = std::move(imm_);
    return false;
  }

  FileMeta meta{number, writer->file_size(), writer->record_count()};
  writer.reset();

  auto reader = SSTableReader::Open(sst_path, error);
  if (reader == nullptr) return false;

  levels_[0].push_back(FileHandle{meta, std::move(reader)});
  manifest_.levels[0].push_back(meta);
  if (!SaveManifest(error)) return false;

  // Only now is it safe to retire the old log: its contents are on disk in a
  // table that the manifest already points at.
  const uint64_t new_wal_number = manifest_.next_file_number++;
  auto new_wal = WAL::Open(FilePath(WalName(new_wal_number)), error);
  if (new_wal == nullptr) return false;

  const std::string old_wal_path = FilePath(WalName(wal_number_));
  wal_->Sync();
  wal_ = std::move(new_wal);
  wal_number_ = new_wal_number;

  std::error_code ec;
  fs::remove(old_wal_path, ec);

  imm_.reset();
  ++flushes_;
  return SaveManifest(error) && MaybeCompact(error);
}

bool DB::Flush(std::string* error) {
  std::lock_guard<std::mutex> lock(mu_);
  return FlushLocked(error);
}

bool DB::MaybeFlush(std::string* error) {
  if (mem_->ApproxBytes() < opts_.write_buffer_size) return true;
  return FlushLocked(error) && MaybeCompact(error);
}

bool DB::MaybeCompact(std::string* error) {
  if (levels_.size() < 2) return true;
  if (levels_[0].size() >= opts_.l0_compaction_trigger) {
    if (!CompactLevel(0, error)) return false;
  }
  for (size_t level = 1; level + 1 < levels_.size(); ++level) {
    if (levels_[level].size() >= opts_.level_size_multiplier) {
      if (!CompactLevel(static_cast<int>(level), error)) return false;
    }
  }
  return true;
}

bool DB::CompactAll(std::string* error) {
  std::lock_guard<std::mutex> lock(mu_);
  if (!FlushLocked(error)) return false;
  // Push each level down once; a second pass catches newly oversized levels.
  for (int pass = 0; pass < 2; ++pass) {
    for (size_t level = 0; level + 1 < levels_.size(); ++level) {
      if (levels_[level].empty()) continue;
      if (!CompactLevel(static_cast<int>(level), error)) return false;
    }
  }
  return true;
}

bool DB::CompactLevel(int level, std::string* error) {
  if (level < 0 || static_cast<size_t>(level) >= levels_.size()) return true;
  const size_t src_level = static_cast<size_t>(level);
  const size_t dst_level = std::min(src_level + 1, levels_.size() - 1);

  if (levels_[src_level].empty()) return true;

  // Tombstones are only safe to drop when nothing deeper can resurrect the key.
  bool has_deeper = false;
  for (size_t deeper = dst_level + 1; deeper < levels_.size(); ++deeper) {
    if (!levels_[deeper].empty()) has_deeper = true;
  }
  const bool drop_tombstones = !has_deeper;

  std::vector<std::string> input_paths;
  std::vector<FileHandle> outputs;

  {
    std::vector<std::unique_ptr<EntryIterator>> sources;
    for (auto& file : levels_[src_level]) sources.push_back(file.reader->NewIterator());
    if (dst_level != src_level) {
      for (auto& file : levels_[dst_level]) sources.push_back(file.reader->NewIterator());
    }
    MergeIterator merge(std::move(sources), std::numeric_limits<uint64_t>::max());

    std::unique_ptr<SSTableWriter> writer;
    uint64_t number = 0;

    auto open_writer = [&]() -> bool {
      number = manifest_.next_file_number++;
      writer = SSTableWriter::Create(FilePath(SstName(number)), opts_.sstable, error);
      return writer != nullptr;
    };
    auto close_writer = [&]() -> bool {
      if (writer == nullptr) return true;
      if (writer->record_count() == 0) {
        writer.reset();  // everything was dropped; destructor removes the stub
        return true;
      }
      if (!writer->Finish(error)) return false;
      FileMeta meta{number, writer->file_size(), writer->record_count()};
      auto reader = SSTableReader::Open(FilePath(SstName(number)), error);
      if (reader == nullptr) return false;
      outputs.push_back(FileHandle{meta, std::move(reader)});
      writer.reset();
      return true;
    };

    if (!open_writer()) return false;
    for (; merge.Valid(); merge.Next()) {
      if (merge.Deleted() && drop_tombstones) continue;
      if (!writer->Add(merge.Key(), merge.Seq(), merge.Value(), merge.Deleted())) {
        if (error != nullptr) *error = "compaction: entry rejected by writer (ordering invariant broken)";
        return false;
      }
      if (writer->estimated_size() >= opts_.target_file_size) {
        if (!close_writer()) return false;
        if (!open_writer()) return false;
      }
    }
    if (!close_writer()) return false;
  }  // merge (and its cursors into the input readers) destroyed here

  for (auto& file : levels_[src_level]) input_paths.push_back(FilePath(SstName(file.meta.number)));
  if (dst_level != src_level) {
    for (auto& file : levels_[dst_level]) input_paths.push_back(FilePath(SstName(file.meta.number)));
  }

  levels_[src_level].clear();
  if (dst_level != src_level) levels_[dst_level].clear();

  std::vector<FileMeta> out_metas;
  out_metas.reserve(outputs.size());
  for (const auto& handle : outputs) out_metas.push_back(handle.meta);

  manifest_.levels[src_level].clear();
  manifest_.levels[dst_level] = out_metas;
  levels_[dst_level] = std::move(outputs);

  if (!SaveManifest(error)) return false;

  std::error_code ec;
  for (const auto& path : input_paths) fs::remove(path, ec);

  ++compactions_;
  return true;
}

Stats DB::GetStats() {
  std::lock_guard<std::mutex> lock(mu_);
  Stats s;
  s.last_sequence = last_seq_;
  s.memtable_bytes = mem_ != nullptr ? mem_->ApproxBytes() : 0;
  s.memtable_entries = mem_ != nullptr ? mem_->Count() : 0;
  s.immutable_bytes = imm_ != nullptr ? imm_->ApproxBytes() : 0;
  s.wal_bytes = wal_ != nullptr ? wal_->FileSize() : 0;
  s.flushes = flushes_;
  s.compactions = compactions_;

  s.level_files_bytes.reserve(levels_.size());
  for (size_t level = 0; level < levels_.size(); ++level) {
    uint64_t bytes = 0;
    for (const auto& file : levels_[level]) {
      bytes += file.meta.file_size;
      s.sstable_records += file.meta.record_count;
    }
    s.sstable_bytes += bytes;
    s.num_files += levels_[level].size();
    s.level_files_bytes.emplace_back(static_cast<uint64_t>(levels_[level].size()), bytes);
  }
  return s;
}

uint64_t DB::LastSequence() {
  std::lock_guard<std::mutex> lock(mu_);
  return last_seq_;
}

}  // namespace trove
