#include "trove/util.hpp"

#include <array>
#include <cstring>

#if defined(_WIN32)
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace trove {
namespace {

struct Crc32Tables {
  uint32_t t[8][256]{};
  Crc32Tables() {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) {
        c = (c & 1U) ? (0xEDB88320U ^ (c >> 1)) : (c >> 1);
      }
      t[0][i] = c;
    }
    // Slicing-by-8: each additional table folds one more input byte per lookup,
    // which roughly quadruples CRC throughput on block-sized buffers.
    for (int slice = 1; slice < 8; ++slice) {
      for (uint32_t i = 0; i < 256; ++i) {
        t[slice][i] = (t[slice - 1][i] >> 8) ^ t[0][t[slice - 1][i] & 0xFFU];
      }
    }
  }
};

const Crc32Tables& Tables() {
  static const Crc32Tables tables;
  return tables;
}

}  // namespace

uint32_t Crc32(const void* data, size_t len, uint32_t seed) {
  const auto& t = Tables().t;
  const auto* p = static_cast<const uint8_t*>(data);
  uint32_t c = seed ^ 0xFFFFFFFFU;

#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
  // Slicing-by-8 needs an unaligned little-endian 32-bit load; we copy into an
  // integer to stay clear of strict-aliasing and alignment traps.
  while (len >= 8) {
    uint32_t one;
    std::memcpy(&one, p, 4);
    one ^= c;
    c = t[7][one & 0xFFU] ^ t[6][(one >> 8) & 0xFFU] ^ t[5][(one >> 16) & 0xFFU] ^
        t[4][(one >> 24) & 0xFFU] ^ t[3][p[4]] ^ t[2][p[5]] ^ t[1][p[6]] ^ t[0][p[7]];
    p += 8;
    len -= 8;
  }
#endif

  while (len-- > 0) {
    c = t[0][(c ^ *p++) & 0xFFU] ^ (c >> 8);
  }
  return c ^ 0xFFFFFFFFU;
}

void PutVarint(std::string* dst, uint64_t value) {
  while (value >= 0x80U) {
    dst->push_back(static_cast<char>((value & 0x7FU) | 0x80U));
    value >>= 7;
  }
  dst->push_back(static_cast<char>(value));
}

void PutVarint32(std::string* dst, uint32_t value) {
  PutVarint(dst, static_cast<uint64_t>(value));
}

bool GetVarint(std::string_view* src, uint64_t* value) {
  uint64_t result = 0;
  for (int shift = 0, i = 0; i < 10; ++i, shift += 7) {
    if (src->empty()) return false;
    const uint8_t byte = static_cast<uint8_t>((*src)[0]);
    *src = src->substr(1);
    if (byte & 0x80U) {
      result |= static_cast<uint64_t>(byte & 0x7FU) << shift;
    } else {
      result |= static_cast<uint64_t>(byte) << shift;
      *value = result;
      return true;
    }
  }
  return false;  // more than 10 bytes: not a valid 64-bit varint
}

bool GetVarint32(std::string_view* src, uint32_t* value) {
  uint64_t wide = 0;
  if (!GetVarint(src, &wide)) return false;
  if (wide > 0xFFFFFFFFULL) return false;
  *value = static_cast<uint32_t>(wide);
  return true;
}

void PutFixed32(std::string* dst, uint32_t value) {
  for (int i = 0; i < 4; ++i) {
    dst->push_back(static_cast<char>((value >> (8 * i)) & 0xFFU));
  }
}

void PutFixed64(std::string* dst, uint64_t value) {
  for (int i = 0; i < 8; ++i) {
    dst->push_back(static_cast<char>((value >> (8 * i)) & 0xFFU));
  }
}

bool GetFixed32(std::string_view* src, uint32_t* value) {
  if (src->size() < 4) return false;
  uint32_t out = 0;
  for (int i = 0; i < 4; ++i) {
    out |= static_cast<uint32_t>(static_cast<uint8_t>((*src)[i])) << (8 * i);
  }
  *src = src->substr(4);
  *value = out;
  return true;
}

bool GetFixed64(std::string_view* src, uint64_t* value) {
  if (src->size() < 8) return false;
  uint64_t out = 0;
  for (int i = 0; i < 8; ++i) {
    out |= static_cast<uint64_t>(static_cast<uint8_t>((*src)[i])) << (8 * i);
  }
  *src = src->substr(8);
  *value = out;
  return true;
}

bool SyncFile(std::FILE* file) {
  if (file == nullptr) return false;
  if (std::fflush(file) != 0) return false;
#if defined(_WIN32)
  return _commit(_fileno(file)) == 0;
#else
  return ::fsync(::fileno(file)) == 0;
#endif
}

bool SyncDir(const std::string& dir) {
#if defined(_WIN32)
  (void)dir;
  return true;  // no directory-entry durability primitive on Win32
#else
  const int fd = ::open(dir.c_str(), O_RDONLY);
  if (fd < 0) return false;
  const bool ok = ::fsync(fd) == 0;
  ::close(fd);
  return ok;
#endif
}

uint64_t Hash64(std::string_view data, uint64_t seed) {
  uint64_t h = seed;
  for (unsigned char c : data) {
    h ^= static_cast<uint64_t>(c);
    h *= 0x100000001b3ULL;
  }
  return h;
}

uint64_t MixBits(uint64_t x) {
  x += 0x9e3779b97f4a7c15ULL;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
  return x ^ (x >> 31);
}

std::string ToHex(std::string_view bytes) {
  static const char* kDigits = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (unsigned char c : bytes) {
    out.push_back(kDigits[c >> 4]);
    out.push_back(kDigits[c & 0x0F]);
  }
  return out;
}

std::string HumanBytes(uint64_t bytes) {
  static const char* kUnits[] = {"B", "KiB", "MiB", "GiB", "TiB"};
  double v = static_cast<double>(bytes);
  int unit = 0;
  while (v >= 1024.0 && unit < 4) {
    v /= 1024.0;
    ++unit;
  }
  char buf[64];
  if (unit == 0) {
    std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(bytes));
  } else {
    std::snprintf(buf, sizeof(buf), "%.1f %s", v, kUnits[unit]);
  }
  return std::string(buf);
}

std::string PrintableKey(std::string_view key) {
  std::string out;
  out.reserve(key.size());
  for (unsigned char c : key) {
    if (c >= 0x20 && c < 0x7F && c != '\\') {
      out.push_back(static_cast<char>(c));
    } else if (c == '\\') {
      out += "\\\\";
    } else {
      char buf[8];
      std::snprintf(buf, sizeof(buf), "\\x%02x", c);
      out += buf;
    }
  }
  return out;
}

}  // namespace trove
