// trove/util.hpp — low-level primitives shared across the engine.
//
// Everything here is deliberately dependency-free so the storage format can be
// reimplemented in another language for compatibility testing.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

namespace trove {

// ---------------------------------------------------------------------------
// Checksums
// ---------------------------------------------------------------------------

// CRC-32 (IEEE 802.3, reflected, polynomial 0xEDB88320).
// Used on every WAL record and SSTable block so silent corruption is caught at
// read time instead of being served to the caller.
uint32_t Crc32(const void* data, size_t len, uint32_t seed = 0);

inline uint32_t Crc32(std::string_view s, uint32_t seed = 0) {
  return Crc32(s.data(), s.size(), seed);
}

// ---------------------------------------------------------------------------
// Varint (LEB128) encoding — the same encoding protobuf uses.
// ---------------------------------------------------------------------------

void PutVarint(std::string* dst, uint64_t value);
void PutVarint32(std::string* dst, uint32_t value);

// Decodes one varint and advances *src past it. Returns false on truncated or
// overlong input rather than reading out of bounds.
bool GetVarint(std::string_view* src, uint64_t* value);
bool GetVarint32(std::string_view* src, uint32_t* value);

// ---------------------------------------------------------------------------
// Fixed-width little-endian helpers.
// Spelled out byte-by-byte so the on-disk format does not depend on the host.
// ---------------------------------------------------------------------------

void PutFixed32(std::string* dst, uint32_t value);
void PutFixed64(std::string* dst, uint64_t value);
bool GetFixed32(std::string_view* src, uint32_t* value);
bool GetFixed64(std::string_view* src, uint64_t* value);

// ---------------------------------------------------------------------------
// Durability
// ---------------------------------------------------------------------------

// Flushes a stream all the way to the platter (fsync/_commit), not just the
// libc buffer. Without this a WAL is only durable against process crashes, not
// against power loss — a distinction that matters and that a lot of toy
// engines get wrong.
bool SyncFile(std::FILE* file);

// On POSIX a newly created file is not durable until the containing directory
// entry is synced too. No-op on Windows.
bool SyncDir(const std::string& dir);

// ---------------------------------------------------------------------------
// Hashing / formatting
// ---------------------------------------------------------------------------

// FNV-1a 64. Used for the bloom filter double-hashing scheme.
uint64_t Hash64(std::string_view data, uint64_t seed = 0xcbf29ce484222325ULL);

// SplitMix64 finalizer: cheap avalanche for bloom probing.
uint64_t MixBits(uint64_t x);

std::string ToHex(std::string_view bytes);

// Human-readable byte count (e.g. "4.2 MiB").
std::string HumanBytes(uint64_t bytes);

// Escapes non-printable bytes so keys can be shown in the CLI without
// mangling the terminal.
std::string PrintableKey(std::string_view key);

}  // namespace trove
