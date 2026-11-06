// trove/memtable.hpp — the in-memory write buffer.
//
// A MemTable is immutable once it has been rotated out; the engine swaps a fresh
// one in and flushes the old one to an SSTable. Keeping "immutable" as an
// explicit state (rather than a flag checked under a lock) is what makes the
// read path race-free without holding a lock during iteration.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "trove/iterator.hpp"
#include "trove/skiplist.hpp"

namespace trove {

class MemTable {
 public:
  class Iterator : public EntryIterator {
   public:
    using Node = typename SkipList<std::string, std::string>::Node;

    explicit Iterator(const Node* node) : node_(node) {}
    bool Valid() const override { return node_ != nullptr; }
    void Next() override { node_ = node_ ? node_->next[0] : nullptr; }
    std::string_view Key() const override { return node_->key; }
    uint64_t Seq() const override { return node_->seq; }
    std::string_view Value() const override { return node_->value; }
    bool Deleted() const override { return node_->deleted; }

   private:
    const Node* node_;
  };

  MemTable() = default;

  void Insert(uint64_t seq, std::string_view key, std::string_view value, bool deleted) {
    table_.Insert(std::string(key), seq, std::string(value), deleted);
  }

  // Point lookup at a snapshot sequence. Returns false if the key has no version
  // visible at `snapshot`; sets *deleted when the newest visible version is a
  // tombstone (caller must treat that as "not found", but must NOT fall through
  // to older SSTables, which is the whole point of tombstones).
  bool Get(std::string_view key, uint64_t snapshot, std::string* value, bool* deleted,
           uint64_t* found_seq = nullptr) const {
    using Table = SkipList<std::string, std::string>;
    const Table::Node* node = table_.SeekGE(std::string(key), snapshot);
    if (node == nullptr || node->key != key) return false;
    if (value != nullptr) *value = node->value;
    if (deleted != nullptr) *deleted = node->deleted;
    if (found_seq != nullptr) *found_seq = node->seq;
    return true;
  }

  std::unique_ptr<EntryIterator> NewIterator() const {
    return std::make_unique<Iterator>(table_.First());
  }

  size_t ApproxBytes() const { return table_.ApproxBytes(); }
  size_t Count() const { return table_.size(); }
  bool Empty() const { return table_.Empty(); }
  int peak_height() const { return table_.peak_height(); }

 private:
  SkipList<std::string, std::string> table_;
};

}  // namespace trove
