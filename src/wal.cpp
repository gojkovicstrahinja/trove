#include "trove/wal.hpp"

#include <cstring>
#include <filesystem>
#include <vector>

#include "trove/util.hpp"

namespace trove {
namespace {

constexpr uint32_t kMaxRecordSize = 64U * 1024U * 1024U;  // sanity bound on replay
constexpr size_t kFrameHeader = 8;                        // len + crc

std::string EncodeRecord(uint64_t seq, RecordType type, std::string_view key, std::string_view value) {
  std::string payload;
  payload.reserve(key.size() + value.size() + 16);
  PutFixed64(&payload, seq);
  payload.push_back(static_cast<char>(type));
  PutVarint32(&payload, static_cast<uint32_t>(key.size()));
  payload.append(key.data(), key.size());
  PutVarint32(&payload, static_cast<uint32_t>(value.size()));
  payload.append(value.data(), value.size());
  return payload;
}

bool DecodeRecord(std::string_view payload, WalRecord* rec) {
  if (!GetFixed64(&payload, &rec->seq)) return false;
  if (payload.empty()) return false;
  const auto type = static_cast<uint8_t>(payload[0]);
  payload = payload.substr(1);
  if (type != static_cast<uint8_t>(RecordType::kPut) && type != static_cast<uint8_t>(RecordType::kDelete)) {
    return false;
  }
  rec->type = static_cast<RecordType>(type);

  uint32_t klen = 0;
  if (!GetVarint32(&payload, &klen) || payload.size() < klen) return false;
  rec->key.assign(payload.data(), klen);
  payload = payload.substr(klen);

  uint32_t vlen = 0;
  if (!GetVarint32(&payload, &vlen) || payload.size() < vlen) return false;
  rec->value.assign(payload.data(), vlen);
  payload = payload.substr(vlen);

  return payload.empty();  // trailing garbage means we mis-framed: reject
}

}  // namespace

WAL::~WAL() {
  if (file_ != nullptr) {
    std::fclose(file_);
    file_ = nullptr;
  }
}

std::unique_ptr<WAL> WAL::Open(const std::string& path, std::string* error) {
  auto wal = std::unique_ptr<WAL>(new WAL());
  wal->path_ = path;
  wal->file_ = std::fopen(path.c_str(), "ab");
  if (wal->file_ == nullptr) {
    if (error != nullptr) *error = "failed to open WAL for append: " + path;
    return nullptr;
  }
  std::error_code ec;
  wal->size_ = std::filesystem::exists(path, ec) ? std::filesystem::file_size(path, ec) : 0;
  if (ec) wal->size_ = 0;
  return wal;
}

bool WAL::Append(uint64_t seq, RecordType type, std::string_view key, std::string_view value) {
  if (file_ == nullptr) return false;
  const std::string payload = EncodeRecord(seq, type, key, value);
  std::string frame;
  frame.reserve(payload.size() + kFrameHeader);
  PutFixed32(&frame, static_cast<uint32_t>(payload.size()));
  PutFixed32(&frame, Crc32(payload));
  frame += payload;
  if (std::fwrite(frame.data(), 1, frame.size(), file_) != frame.size()) return false;
  size_ += frame.size();
  unsynced_ += frame.size();
  return true;
}

bool WAL::Sync() {
  if (file_ == nullptr) return false;
  if (unsynced_ == 0) return true;
  if (!SyncFile(file_)) return false;
  unsynced_ = 0;
  return true;
}

bool WAL::Replay(const std::string& path,
                 const std::function<void(const WalRecord&)>& fn,
                 uint64_t* max_seq,
                 bool* recovered_from_torn,
                 std::string* error) {
  if (recovered_from_torn != nullptr) *recovered_from_torn = false;
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) return true;  // nothing to replay

  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) {
    if (error != nullptr) *error = "cannot open WAL for replay: " + path;
    return false;
  }

  uint64_t good_offset = 0;
  bool torn = false;
  std::string payload;

  for (;;) {
    unsigned char header[kFrameHeader];
    const size_t got = std::fread(header, 1, kFrameHeader, file);
    if (got == 0) break;  // clean EOF at a record boundary
    if (got != kFrameHeader) {
      torn = true;  // partial length/crc: interrupted write
      break;
    }
    uint32_t len = 0, crc = 0;
    std::memcpy(&len, header, 4);
    std::memcpy(&crc, header + 4, 4);
    if (len == 0 || len > kMaxRecordSize) {
      torn = true;
      break;
    }
    payload.resize(len);
    if (std::fread(payload.data(), 1, len, file) != len) {
      torn = true;
      break;
    }
    if (Crc32(payload) != crc) {
      torn = true;  // bit rot or a torn multi-sector write
      break;
    }
    WalRecord rec;
    if (!DecodeRecord(payload, &rec)) {
      torn = true;
      break;
    }
    fn(rec);
    if (max_seq != nullptr && rec.seq > *max_seq) *max_seq = rec.seq;
    good_offset += kFrameHeader + len;
  }
  std::fclose(file);

  if (torn) {
    // Truncate so the next append starts on a clean boundary. Without this the
    // engine would keep appending after a hole and lose everything past it.
    std::error_code rec_ec;
    std::filesystem::resize_file(path, good_offset, rec_ec);
    if (rec_ec) {
      if (error != nullptr) *error = "WAL is corrupt and could not be truncated";
      return false;
    }
    if (recovered_from_torn != nullptr) *recovered_from_torn = true;
  }
  return true;
}

}  // namespace trove
