#include "trove/sstable.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <limits>

namespace trove {
namespace {

constexpr char kMagic[8] = {'T', 'R', 'O', 'V', 'E', 'S', 'S', 'T'};
constexpr size_t kFooterSize = 8 + 5 * 8;  // magic + idx_off + idx_len + bloom_off + bloom_len + count

void AppendRecord(std::string* dst, std::string_view key, uint64_t seq, std::string_view value, bool deleted) {
  PutVarint32(dst, static_cast<uint32_t>(key.size()));
  dst->append(key.data(), key.size());
  PutFixed64(dst, seq);
  dst->push_back(static_cast<char>(deleted ? 1 : 0));
  PutVarint32(dst, static_cast<uint32_t>(value.size()));
  dst->append(value.data(), value.size());
}

}  // namespace

// ---------------------------------------------------------------------------
// Block helpers
// ---------------------------------------------------------------------------

bool ParseBlockOffsets(std::string_view block, std::vector<std::pair<uint32_t, uint32_t>>* offsets) {
  offsets->clear();
  size_t pos = 0;
  while (pos < block.size()) {
    const size_t start = pos;
    std::string_view rest = block.substr(pos);
    uint32_t klen = 0;
    if (!GetVarint32(&rest, &klen)) return false;
    if (rest.size() < klen) return false;
    rest = rest.substr(klen);
    if (rest.size() < 8 + 1) return false;  // seq + deleted byte
    rest = rest.substr(8 + 1);
    uint32_t vlen = 0;
    if (!GetVarint32(&rest, &vlen)) return false;
    if (rest.size() < vlen) return false;
    rest = rest.substr(vlen);
    const size_t consumed = block.size() - pos - rest.size();
    pos += consumed;
    offsets->emplace_back(static_cast<uint32_t>(start), static_cast<uint32_t>(consumed));
  }
  return pos == block.size();
}

bool DecodeBlockRecord(std::string_view block, uint32_t offset, uint32_t len, BlockRecord* out) {
  if (static_cast<size_t>(offset) + len > block.size()) return false;
  std::string_view rest = block.substr(offset, len);

  uint32_t klen = 0;
  if (!GetVarint32(&rest, &klen) || rest.size() < klen) return false;
  out->key = rest.substr(0, klen);
  rest = rest.substr(klen);

  if (!GetFixed64(&rest, &out->seq)) return false;
  if (rest.empty()) return false;
  out->deleted = static_cast<uint8_t>(rest[0]) != 0;
  rest = rest.substr(1);

  uint32_t vlen = 0;
  if (!GetVarint32(&rest, &vlen) || rest.size() < vlen) return false;
  out->value = rest.substr(0, vlen);
  return true;
}

// ---------------------------------------------------------------------------
// Writer
// ---------------------------------------------------------------------------

SSTableWriter::~SSTableWriter() {
  if (file_ != nullptr) {
    std::fclose(file_);
    file_ = nullptr;
    if (!finished_) {
      // A half-written table must never be visible to a reader.
      std::error_code ec;
      std::filesystem::remove(path_, ec);
    }
  }
}

std::unique_ptr<SSTableWriter> SSTableWriter::Create(const std::string& path, const SSTableOptions& opts,
                                                     std::string* error) {
  auto writer = std::unique_ptr<SSTableWriter>(new SSTableWriter());
  writer->path_ = path;
  writer->opts_ = opts;
  writer->file_ = std::fopen(path.c_str(), "wb");
  if (writer->file_ == nullptr) {
    if (error != nullptr) *error = "cannot create SSTable: " + path;
    return nullptr;
  }
  return writer;
}

bool SSTableWriter::Add(std::string_view key, uint64_t seq, std::string_view value, bool deleted) {
  if (file_ == nullptr || finished_) return false;

  // Enforce (key ASC, seq DESC). A violation is a caller bug, not a data
  // condition, so fail at the boundary where it is cheap to diagnose.
  if (have_prev_) {
    if (key < prev_key_) return false;
    if (key == prev_key_ && seq >= prev_seq_) return false;
  }

  if (!have_prev_) smallest_key_.assign(key.data(), key.size());
  largest_key_.assign(key.data(), key.size());
  prev_key_.assign(key.data(), key.size());
  prev_seq_ = seq;
  have_prev_ = true;

  min_seq_ = std::min(min_seq_, seq);
  max_seq_ = std::max(max_seq_, seq);

  AppendRecord(&block_, key, seq, value, deleted);
  last_block_key_.assign(key.data(), key.size());
  key_hashes_.push_back(Hash64(key));
  ++record_count_;

  if (block_.size() >= opts_.block_size) {
    std::string error;
    if (!FlushBlock(&error)) return false;
  }
  return true;
}

bool SSTableWriter::WriteAt(const std::string& data, std::string* error) {
  if (data.empty()) return true;
  if (std::fwrite(data.data(), 1, data.size(), file_) != data.size()) {
    if (error != nullptr) *error = "short write to SSTable: " + path_;
    return false;
  }
  offset_ += data.size();
  return true;
}

bool SSTableWriter::FlushBlock(std::string* error) {
  if (block_.empty()) return true;
  // Frame the block with its checksum; the index offset/len include both.
  std::string framed;
  PutFixed32(&framed, Crc32(block_));
  framed += block_;
  index_.push_back(IndexEntry{last_block_key_, offset_, static_cast<uint32_t>(framed.size())});
  if (!WriteAt(framed, error)) return false;
  block_.clear();
  return true;
}

bool SSTableWriter::Finish(std::string* error) {
  if (file_ == nullptr || finished_) {
    if (error != nullptr) *error = "SSTable::Finish called twice or on a closed writer";
    return false;
  }
  if (record_count_ == 0) {
    if (error != nullptr) *error = "refusing to write an empty SSTable";
    return false;
  }
  if (!FlushBlock(error)) return false;

  // Index block
  std::string index_block;
  PutVarint32(&index_block, static_cast<uint32_t>(index_.size()));
  PutVarint32(&index_block, static_cast<uint32_t>(smallest_key_.size()));
  index_block += smallest_key_;
  PutVarint32(&index_block, static_cast<uint32_t>(largest_key_.size()));
  index_block += largest_key_;
  PutFixed64(&index_block, min_seq_);
  PutFixed64(&index_block, max_seq_);
  for (const auto& e : index_) {
    PutVarint32(&index_block, static_cast<uint32_t>(e.last_key.size()));
    index_block += e.last_key;
    PutFixed64(&index_block, e.offset);
    PutFixed32(&index_block, e.len);
  }
  const uint64_t index_offset = offset_;
  const uint64_t index_len = index_block.size();
  if (!WriteAt(index_block, error)) return false;

  // Bloom block
  BloomFilter bloom = BloomFilter::Create(key_hashes_.size(), opts_.bloom_false_positive_rate);
  for (uint64_t h : key_hashes_) bloom.AddHash(h);
  const std::string bloom_block = bloom.Serialize();
  const uint64_t bloom_offset = offset_;
  const uint64_t bloom_len = bloom_block.size();
  if (!WriteAt(bloom_block, error)) return false;

  // Footer
  std::string footer;
  footer.append(kMagic, sizeof(kMagic));
  PutFixed64(&footer, index_offset);
  PutFixed64(&footer, index_len);
  PutFixed64(&footer, bloom_offset);
  PutFixed64(&footer, bloom_len);
  PutFixed64(&footer, record_count_);
  if (footer.size() != kFooterSize) {
    if (error != nullptr) *error = "internal: footer size mismatch";
    return false;
  }
  if (!WriteAt(footer, error)) return false;

  if (!SyncFile(file_)) {
    if (error != nullptr) *error = "fsync failed for SSTable: " + path_;
    return false;
  }
  std::fclose(file_);
  file_ = nullptr;
  finished_ = true;
  file_size_ = offset_;
  key_hashes_.clear();
  key_hashes_.shrink_to_fit();
  return true;
}

// ---------------------------------------------------------------------------
// Reader
// ---------------------------------------------------------------------------

SSTableReader::~SSTableReader() {
  if (file_ != nullptr) {
    std::fclose(file_);
    file_ = nullptr;
  }
}

bool SSTableReader::ReadAt(uint64_t offset, size_t len, std::string* out) const {
  if (file_ == nullptr) return false;
  if (std::fseek(file_, static_cast<long>(offset), SEEK_SET) != 0) return false;
  out->resize(len);
  if (len == 0) return true;
  return std::fread(out->data(), 1, len, file_) == len;
}

std::unique_ptr<SSTableReader> SSTableReader::Open(const std::string& path, std::string* error) {
  auto reader = std::unique_ptr<SSTableReader>(new SSTableReader());
  reader->path_ = path;
  reader->file_ = std::fopen(path.c_str(), "rb");
  if (reader->file_ == nullptr) {
    if (error != nullptr) *error = "cannot open SSTable: " + path;
    return nullptr;
  }

  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  if (ec || size < kFooterSize) {
    if (error != nullptr) *error = "SSTable too small or unreadable: " + path;
    return nullptr;
  }
  reader->file_size_ = size;

  std::string footer;
  if (!reader->ReadAt(size - kFooterSize, kFooterSize, &footer)) {
    if (error != nullptr) *error = "cannot read SSTable footer: " + path;
    return nullptr;
  }
  if (std::memcmp(footer.data(), kMagic, sizeof(kMagic)) != 0) {
    if (error != nullptr) *error = "bad SSTable magic (not a trove table?): " + path;
    return nullptr;
  }

  std::string_view fv = footer;
  fv = fv.substr(sizeof(kMagic));
  uint64_t idx_off = 0, idx_len = 0, bloom_off = 0, bloom_len = 0;
  if (!GetFixed64(&fv, &idx_off) || !GetFixed64(&fv, &idx_len) || !GetFixed64(&fv, &bloom_off) ||
      !GetFixed64(&fv, &bloom_len) || !GetFixed64(&fv, &reader->record_count_)) {
    if (error != nullptr) *error = "truncated SSTable footer: " + path;
    return nullptr;
  }

  std::string index_block;
  if (!reader->ReadAt(idx_off, static_cast<size_t>(idx_len), &index_block)) {
    if (error != nullptr) *error = "cannot read SSTable index: " + path;
    return nullptr;
  }

  std::string_view iv = index_block;
  uint32_t num_blocks = 0;
  if (!GetVarint32(&iv, &num_blocks)) {
    if (error != nullptr) *error = "corrupt SSTable index: " + path;
    return nullptr;
  }

  auto read_str = [&iv](std::string* dst) -> bool {
    uint32_t len = 0;
    if (!GetVarint32(&iv, &len) || iv.size() < len) return false;
    dst->assign(iv.data(), len);
    iv = iv.substr(len);
    return true;
  };

  if (!read_str(&reader->smallest_key_) || !read_str(&reader->largest_key_)) {
    if (error != nullptr) *error = "corrupt SSTable index keys: " + path;
    return nullptr;
  }
  if (!GetFixed64(&iv, &reader->min_seq_) || !GetFixed64(&iv, &reader->max_seq_)) {
    if (error != nullptr) *error = "corrupt SSTable index sequence bounds: " + path;
    return nullptr;
  }

  reader->index_.reserve(num_blocks);
  for (uint32_t i = 0; i < num_blocks; ++i) {
    IndexEntry e;
    uint32_t klen = 0;
    if (!GetVarint32(&iv, &klen) || iv.size() < klen) {
      if (error != nullptr) *error = "corrupt SSTable index entry: " + path;
      return nullptr;
    }
    e.last_key.assign(iv.data(), klen);
    iv = iv.substr(klen);
    if (!GetFixed64(&iv, &e.offset) || !GetFixed32(&iv, &e.len)) {
      if (error != nullptr) *error = "corrupt SSTable index entry offsets: " + path;
      return nullptr;
    }
    reader->index_.push_back(std::move(e));
  }

  std::string bloom_block;
  if (!reader->ReadAt(bloom_off, static_cast<size_t>(bloom_len), &bloom_block)) {
    if (error != nullptr) *error = "cannot read SSTable bloom block: " + path;
    return nullptr;
  }
  if (!BloomFilter::Deserialize(bloom_block, &reader->bloom_)) {
    if (error != nullptr) *error = "corrupt SSTable bloom block: " + path;
    return nullptr;
  }
  return reader;
}

bool SSTableReader::ReadBlock(size_t idx, std::string* out) const {
  if (idx >= index_.size()) return false;
  std::string framed;
  if (!ReadAt(index_[idx].offset, index_[idx].len, &framed)) return false;
  if (framed.size() < 4) return false;

  std::string_view head(framed.data(), 4);
  uint32_t expected = 0;
  if (!GetFixed32(&head, &expected)) return false;
  const std::string_view body(framed.data() + 4, framed.size() - 4);
  if (Crc32(body) != expected) return false;  // refuse to serve corrupt bytes

  out->assign(framed, 4, std::string::npos);  // single copy off the framed buffer
  return true;
}

size_t SSTableReader::FindBlock(std::string_view key) const {
  // First block whose last key is >= the target key.
  size_t lo = 0, hi = index_.size();
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    if (index_[mid].last_key < key) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return lo;
}

bool SSTableReader::Get(std::string_view key, uint64_t snapshot, std::string* value, bool* deleted) const {
  if (key < smallest_key_ || key > largest_key_) return false;  // outside range: no I/O at all
  if (!bloom_.MayContain(key)) return false;                    // negative lookup: no I/O at all

  const size_t block_idx = FindBlock(key);
  if (block_idx >= index_.size()) return false;

  std::string block;
  if (!ReadBlock(block_idx, &block)) return false;

  std::vector<std::pair<uint32_t, uint32_t>> offsets;
  if (!ParseBlockOffsets(block, &offsets)) return false;

  const std::string_view bv = block;
  for (const auto& [off, len] : offsets) {
    BlockRecord rec;
    if (!DecodeBlockRecord(bv, off, len, &rec)) return false;
    const int cmp = rec.key.compare(key);
    if (cmp < 0) continue;
    if (cmp > 0) return false;  // passed the key: no visible version here
    if (rec.seq <= snapshot) {  // (key ASC, seq DESC) => first match is newest visible
      if (value != nullptr) value->assign(rec.value.data(), rec.value.size());
      if (deleted != nullptr) *deleted = rec.deleted;
      return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// Iterator
// ---------------------------------------------------------------------------

class SSTableIterator : public EntryIterator {
 public:
  explicit SSTableIterator(const SSTableReader* reader) : reader_(reader) {
    if (reader_->index_.empty()) {
      valid_ = false;
      return;
    }
    LoadBlock(0);
  }

  bool Valid() const override { return valid_; }

  void Next() override {
    if (!valid_) return;
    ++rec_idx_;
    if (rec_idx_ >= offsets_.size()) {
      LoadBlock(block_idx_ + 1);
      return;
    }
    LoadCurrent();
  }

  std::string_view Key() const override { return key_; }
  uint64_t Seq() const override { return seq_; }
  std::string_view Value() const override { return value_; }
  bool Deleted() const override { return deleted_; }

 private:
  void LoadBlock(size_t idx) {
    while (idx < reader_->index_.size()) {
      std::string block;
      if (!reader_->ReadBlock(idx, &block)) {
        valid_ = false;
        return;
      }
      std::vector<std::pair<uint32_t, uint32_t>> offsets;
      if (!ParseBlockOffsets(block, &offsets)) {
        valid_ = false;
        return;
      }
      block_idx_ = idx;
      block_ = std::move(block);
      offsets_ = std::move(offsets);
      rec_idx_ = 0;
      if (!offsets_.empty()) {
        LoadCurrent();
        return;
      }
      ++idx;  // defensive: an empty block should never be written
    }
    valid_ = false;
  }

  void LoadCurrent() {
    const auto& [off, len] = offsets_[rec_idx_];
    BlockRecord rec;
    if (!DecodeBlockRecord(block_, off, len, &rec)) {
      valid_ = false;  // corruption surfaces as end-of-stream rather than garbage
      return;
    }
    key_.assign(rec.key.data(), rec.key.size());
    value_.assign(rec.value.data(), rec.value.size());
    seq_ = rec.seq;
    deleted_ = rec.deleted;
    valid_ = true;
  }

  const SSTableReader* reader_;
  size_t block_idx_ = 0;
  std::string block_;
  std::vector<std::pair<uint32_t, uint32_t>> offsets_;
  size_t rec_idx_ = 0;
  std::string key_;
  std::string value_;
  uint64_t seq_ = 0;
  bool deleted_ = false;
  bool valid_ = false;
};

std::unique_ptr<EntryIterator> SSTableReader::NewIterator() const {
  return std::make_unique<SSTableIterator>(this);
}

}  // namespace trove
