# Design notes

The details that do not fit in a README: on-disk formats, the recovery protocol,
and the invariants each component is allowed to assume.

## On-disk layout

```
<db-dir>/
  MANIFEST            plain text, replaced atomically
  wal-<n>.log         append-only write-ahead log
  sst-<n>.sst         immutable sorted table
```

File numbers come from a single monotonic counter in the manifest, so a WAL and
an SSTable can never share a number.

### WAL record

```
u32 payload_len | u32 crc32(payload) | payload
payload = u64 seq | u8 type | varint klen | key | varint vlen | value
```

`type` is 1 (put) or 2 (delete). Replay reads frames until the file ends; a short
read, a length above 64 MiB, or a CRC mismatch marks a torn tail. The file is
then truncated back to the last intact boundary so the next append starts clean.

### SSTable

```
[block 0][block 1] ... [index block] [bloom block] [footer: 48 bytes]
```

Data block:

```
u32 crc32 | { varint klen | key | u64 seq | u8 deleted | varint vlen | value }*
```

A block is cut at roughly 4 KiB and never splits a record, so a bounded read is
one block.

Index block:

```
varint block_count
varint klen | smallest key
varint klen | largest key
u64 min_seq | u64 max_seq
block_count * { varint klen | last key in block | u64 offset | u32 len }
```

The `offset`/`len` pair covers the block's CRC and payload. Sparse (one entry per
block) so it stays small enough to keep resident.

Bloom block: `u32 num_bits | u32 num_hashes | bits`, sized for a target false
positive rate of 1 % (~9.6 bits/key, 7 probes) using double hashing.

Footer: `"TROVESST" | idx_off | idx_len | bloom_off | bloom_len | record_count`,
all fixed-width little-endian.

### Point lookup

1. Reject if the key falls outside `[smallest_key, largest_key]` — no I/O.
2. Bloom probe — answers "definitely absent" for most misses, no I/O.
3. Binary search the sparse index for the first block whose last key is `>= key`.
4. Read that block, verify its CRC, scan records until `key >= target`.

Because blocks are ordered `(key ASC, seq DESC)`, the first record with a
matching key and `seq <= snapshot` is by construction the newest visible version.

## Recovery protocol

`DB::Open` does, in order:

1. Load the manifest. If it is absent but `.sst` files exist, refuse to open —
   otherwise the engine presents an empty database and the next compaction
   deletes real data.
2. Open every table the manifest lists. A missing file is fatal: manifest and
   disk disagreeing means the truth is unknown.
3. Set `last_seq` to the maximum sequence across **all** tables.
4. Replay the WAL, folding records into the memtable and raising `last_seq`.
5. Reuse the existing WAL for appends (normal case), or fold a multi-log state
   into a table and start a fresh log.

Step 3 is not decorative. An earlier version reconstructed the sequence counter
from the WAL alone; after a flush rotates the WAL to an empty file, recovery
started the counter at 0, every table version looked "newer than the snapshot",
and reads returned nothing. The compaction test in `tests/test_db.cpp` caught it.
That is the entire argument for having a test that reopens a compacted database.

Flush ordering exists to make crash points benign:

```
write SSTable ─► fsync ─► update MANIFEST (atomic rename) ─► new WAL ─► delete old WAL
```

A crash before the manifest update leaves unreferenced tables (waste, not loss).
A crash after it leaves the old WAL replayable, so records get re-applied
idempotently — same key, same sequence, same value.

## Compaction

Size-tiered across three levels. L0 is triggered at 4 files, deeper levels at 8x
the previous level's file count.

Each compaction merges all files from level `n` with all files from level `n+1`
through a `MergeIterator`, which yields exactly one entry per user key — the
newest — and drops the rest. Output rolls to a new file at 4 MiB.

Tombstone handling is the subtle part. Dropping a tombstone is only safe when no
deeper level can hold an older version of that key, because the tombstone is the
only record that shadows it. `CompactLevel` therefore drops tombstones only when
every level below the destination is empty.

## Invariants

The writer refuses to violate these, which turns a class of silent corruption
into an immediate error:

- Records reach `SSTableWriter::Add` in `(key ASC, seq DESC)` order. Violations
  are rejected, not sorted around.
- An `SSTableWriter` that is destroyed without `Finish()` deletes its partial
  file, so a half-written table is never visible.
- Empty tables are never written.
- A block whose CRC does not match is not decoded; the read fails instead.
- The memtable is append-only until it is rotated out; a rotated memtable is
  immutable, so the read path never needs a lock to iterate one.

## What I would do next, in order

1. **Background compaction + group commit.** The single mutex is the dominant
   limit; it serialises fsyncs and makes compaction latency visible to writers.
2. **Block compression (LZ4) and prefix compression.** These are sorted strings;
   shared prefixes are free space.
3. **Iterator `Seek()`.** Range scans currently pay for every file.
4. **Error taxonomy.** Distinguish corruption from absence, and surface it.
5. **Manifest generations.** `MANIFEST-<n>` plus a `CURRENT` pointer, so a
   damaged manifest has a fallback instead of being fatal.
