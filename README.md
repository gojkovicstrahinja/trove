# trove

An LSM-tree key-value storage engine in C++20. Roughly 3.6k lines, no external
dependencies, buildable with any C++20 compiler.

I wrote this to understand the parts of a database that you cannot learn from a
blog post: what actually happens to an acknowledged write when the power goes
out, why tombstones have to outlive the data they delete, and where the
amplification comes from when you bolt an index onto a log.

```
                       write path

  Put/Delete ──► WAL (CRC32 framing, optional fsync)
                   │
                   ▼
              MemTable (lock-free skip list, ordered by key ASC / seq DESC)
                   │  exceeds write_buffer_size
                   ▼
              immutable memtable ──► flush ──► L0 table ──┐
                                                          │ compaction
              L0 ──► L1 ──► L2  (size-tiered, merge iterators)

                       read path

  Get ──► MemTable ─► immutable ─► L0 (newest file first) ─► L1 ─► L2
          stop at the first visible version; a tombstone terminates the search
```

## Status

| | |
|---|---|
| Tests | 26 passing (`ctest`) |
| Warnings | clean under `-Wall -Wextra -Wpedantic -Wshadow` |
| CI | Linux (GCC, Clang) + Windows (MSVC) |
| Dependencies | none (standard library only) |

## Build and run

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Binaries: `trove` (interactive shell), `trove_tests`, `trove_bench`.

```
$ ./build/trove ./demo
trove — ./demo
(type 'help' for commands)
trove> put user:1 alice
ok
trove> put user:2 bob
ok
trove> del user:1
ok
trove> get user:1
(not found)
trove> scan
user:2	bob
(1 rows)
trove> stats
last sequence     3
memtable          378 B  (3 entries)
wal               83 B
sstables          0 B  (0 files, 0 records)
flushes           0
compactions       0
  L0              0 files, 0 B
  ...
```

`stats` is there because "where do my bytes actually live right now" is the
question you ask constantly while building one of these. `flush` and `compact`
force the interesting state transitions instead of waiting for thresholds.

## Benchmarks

Single-threaded, 200k keys, 64-byte values, 4 MiB write buffer. AMD Ryzen 5
3600, Windows 11, GCC 16.1.0 (`-O3`), data served from the page cache.

| workload | ops | time | throughput |
|---|---:|---:|---:|
| sequential put | 200,000 | 0.72 s | 276,284 ops/s |
| random overwrite | 50,000 | 0.14 s | 354,502 ops/s |
| random get (hit) | 200,000 | 2.10 s | 95,130 ops/s |
| random get (miss) | 100,000 | 0.03 s | 3,663,822 ops/s |
| full ordered scan | 200,000 | 0.09 s | 2,329,352 rows/s (171 MiB/s) |

After the run: 200,000 records occupy 17.1 MiB in 3 tables, reached through 12
flushes and 4 compactions.

Two things worth reading off that table:

- **A miss is ~38x cheaper than a hit.** Bloom filters plus key-range checks mean
  a lookup for an absent key touches no data block at all.
- **Writes are much faster than reads.** That is the LSM trade in one line: the
  write path appends to a log and an in-memory skip list and never seeks, while
  the read path may have to consult several files.

Reproduce with `./build/trove_bench --n 200000`.

## Design decisions worth arguing about

**Ordering is `(key ASC, sequence DESC)`.** One comparator does three jobs:
newest-version-wins is "leftmost entry", a snapshot read is a seek at
`(key, snapshot)`, and the merge iterator resolves duplicates without any
per-caller logic. MVCC falls out of the sort order instead of being bolted on.

**The write path logs before it mutates memory.** A crash between the two is
recoverable; the reverse order loses a write the client was told succeeded.

**Tombstones are data.** A delete writes a record, and compaction may only drop
tombstones when no deeper level could hold an older version of that key —
otherwise deleted keys rise from the dead the moment the tombstone is compacted
away.

**The manifest is plain text and replaced atomically** (write `.tmp`, fsync,
rename). At 2am a format you can `cat` beats a compact one.

**One mutex.** Writer-serialised, readers serialised against writers. This is a
deliberate correctness-over-throughput choice, and it is the first thing I would
change (see below).

**Everything on disk is checksummed.** WAL records carry a CRC32 and every data
block does too, so corruption surfaces as "cannot serve this" instead of as
plausible-looking wrong bytes. The CRC is slicing-by-8, which matters: the
byte-at-a-time version halved point-read throughput.

## Known limitations

Listed because a storage engine that claims to be complete is usually lying:

- **No concurrency.** One mutex; no concurrent readers, no group commit, no
  background compaction thread (compaction runs on the writer, which is why
  `random overwrite` above is faster than it looks).
- **No compression.** Data blocks are uncompressed, and no prefix compression is
  applied even though these are sorted strings.
- **Scans re-merge every source.** There is no `Seek()` on an iterator, so a
  small range read still opens a cursor on every file.
- **Snapshots are per-operation.** `Get`/`Scan` pin a sequence internally; there
  is no long-lived snapshot handle and no transactions.
- **A CRC failure looks like "key absent".** Corruption is not reported as a
  distinct error, which is the wrong trade for a production engine.
- **Single manifest file.** No `MANIFEST-<n>` history, so a torn manifest has no
  fallback (it is written atomically, so the window is narrow, not zero). If the
  manifest disappears entirely the engine refuses to open rather than orphaning
  tables.

## Repository layout

```
include/trove/    util, skiplist, memtable, bloom, wal, sstable, manifest,
                  merge_iterator, db  (headers carry the format documentation)
src/              implementations + CLI
tests/            dependency-free harness, 26 tests
bench/            workload harness
docs/DESIGN.md    format spec, recovery protocol, invariants
```

## License

MIT. See `LICENSE`.
