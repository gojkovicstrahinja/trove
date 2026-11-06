// trove/skiplist.hpp — lock-free-ish ordered container backing the memtable.
//
// Ordering is (user_key ASC, sequence DESC). That single choice makes the two
// hot operations cheap:
//   * newest-version-wins is "leftmost entry for this key"
//   * a snapshot read for (key, snapshot) is SeekGE(key, snapshot), which lands
//     exactly on the newest version at or below the snapshot
//
// This is the same ordering trick LevelDB/RocksDB use for MVCC.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace trove {

template <class Key, class Value, class Compare = std::less<Key>>
class SkipList {
 public:
  // Fixed upper bound so the per-insert predecessor array can live on the stack
  // with a compile-time size (a runtime-sized array would be a VLA, which is not
  // standard C++ and is not portable to MSVC).
  static constexpr int kMaxHeight = 32;

  struct Node {
    Key key;
    uint64_t seq;
    Value value;
    bool deleted;
    std::vector<Node*> next;

    Node(int height, uint64_t s) : seq(s), deleted(false), next(static_cast<size_t>(height), nullptr) {}
  };

  explicit SkipList(int max_height = 16)
      : max_height_(max_height > 1 ? (max_height < kMaxHeight ? max_height : kMaxHeight) : 2),
        height_(1),
        rng_(0x9e3779b97f4a7c15ULL) {
    head_ = NewNode(max_height_, 0);
  }

  SkipList(const SkipList&) = delete;
  SkipList& operator=(const SkipList&) = delete;

  // Inserts a versioned entry. Returns the node, or nullptr if (key, seq)
  // already existed (which would be a bug in the caller's sequence allocation).
  Node* Insert(const Key& key, uint64_t seq, const Value& value, bool deleted) {
    std::array<Node*, kMaxHeight> prev{};
    FindPredecessors(key, seq, prev);

    Node* found = prev[0]->next[0];
    if (found != nullptr && !Before(found->key, found->seq, key, seq) &&
        !Before(key, seq, found->key, found->seq)) {
      found->value = value;  // exact (key, seq) replay: idempotent overwrite
      found->deleted = deleted;
      return found;
    }

    const int height = RandomHeight();
    if (height > height_) {
      for (int i = height_; i < height; ++i) prev[i] = head_;
      height_ = height;
    }

    Node* node = NewNode(height, seq);
    node->key = key;
    node->value = value;
    node->deleted = deleted;
    for (int i = 0; i < height; ++i) {
      node->next[i] = prev[i]->next[i];
      prev[i]->next[i] = node;
    }
    approx_bytes_ += key.size() + value.size() + sizeof(Node) + static_cast<size_t>(height) * sizeof(Node*);
    ++count_;
    return node;
  }

  // First node at or after (key, seq) in (key ASC, seq DESC) order.
  Node* SeekGE(const Key& key, uint64_t seq) const {
    Node* x = head_;
    for (int i = height_ - 1; i >= 0; --i) {
      Node* next = x->next[i];
      while (next != nullptr && Before(next->key, next->seq, key, seq)) {
        x = next;
        next = x->next[i];
      }
    }
    return x->next[0];
  }

  Node* First() const { return head_->next[0]; }

  bool Empty() const { return head_->next[0] == nullptr; }
  size_t size() const { return count_; }
  size_t ApproxBytes() const { return approx_bytes_; }
  int peak_height() const { return height_; }

 private:
  // Strict weak order: key ASC, then seq DESC.
  static bool Before(const Key& ak, uint64_t as, const Key& bk, uint64_t bs) {
    const Compare cmp;
    if (cmp(ak, bk)) return true;
    if (cmp(bk, ak)) return false;
    return as > bs;
  }

  void FindPredecessors(const Key& key, uint64_t seq, std::array<Node*, kMaxHeight>& prev) const {
    Node* x = head_;
    for (int i = height_ - 1; i >= 0; --i) {
      Node* next = x->next[i];
      while (next != nullptr && Before(next->key, next->seq, key, seq)) {
        x = next;
        next = x->next[i];
      }
      prev[i] = x;
    }
  }

  Node* NewNode(int height, uint64_t seq) {
    arena_.push_back(std::make_unique<Node>(height, seq));
    return arena_.back().get();
  }

  int RandomHeight() {
    // xorshift64*; geometric distribution with p = 1/2, capped at max_height_.
    int height = 1;
    while (height < max_height_) {
      rng_ ^= rng_ >> 12;
      rng_ ^= rng_ << 25;
      rng_ ^= rng_ >> 27;
      const uint64_t r = rng_ * 0x2545F4914F6CDD1DULL;
      if ((r >> 60) & 1ULL) break;
      ++height;
    }
    return height;
  }

  int max_height_;
  int height_;
  uint64_t rng_;
  Node* head_;
  size_t count_ = 0;
  size_t approx_bytes_ = 0;
  // Nodes are never removed individually (versions die wholesale on flush), so
  // an arena gives us pointer stability plus a single-shot destructor.
  std::vector<std::unique_ptr<Node>> arena_;
};

}  // namespace trove
