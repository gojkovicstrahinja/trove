// trove/merge_iterator.hpp — k-way merge with snapshot visibility.
//
// This is the single place where "which version of a key wins" is decided:
//   * sources are ordered by (key ASC, seq DESC)
//   * for each key the highest sequence <= snapshot wins
//   * a tombstone is a real answer ("deleted"), not "keep looking"
//
// Point reads could each roll their own logic, but then the flush path, the
// compaction path, and the scan path would drift apart. They share this.
#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "trove/iterator.hpp"

namespace trove {

class MergeIterator : public EntryIterator {
 public:
  MergeIterator(std::vector<std::unique_ptr<EntryIterator>> sources, uint64_t snapshot)
      : sources_(std::move(sources)), snapshot_(snapshot) {
    for (size_t i = 0; i < sources_.size(); ++i) {
      if (sources_[i] != nullptr && sources_[i]->Valid()) heap_.push_back(i);
    }
    const Cmp cmp{&sources_};
    std::make_heap(heap_.begin(), heap_.end(), cmp);
    AdvanceToNextVisibleKey();
  }

  bool Valid() const override { return valid_; }
  void Next() override { AdvanceToNextVisibleKey(); }

  std::string_view Key() const override { return key_; }
  uint64_t Seq() const override { return seq_; }
  std::string_view Value() const override { return value_; }
  bool Deleted() const override { return deleted_; }

 private:
  // Heap is a max-heap; "worst" element is the largest key, or the smallest
  // sequence among equal keys. Front therefore holds the entry we want first.
  struct Cmp {
    const std::vector<std::unique_ptr<EntryIterator>>* srcs;

    bool operator()(size_t a, size_t b) const {
      const auto* A = (*srcs)[a].get();
      const auto* B = (*srcs)[b].get();
      const int c = A->Key().compare(B->Key());
      if (c != 0) return c > 0;
      return A->Seq() < B->Seq();
    }
  };

  void AdvanceToNextVisibleKey() {
    const Cmp cmp{&sources_};

    while (!heap_.empty()) {
      const std::string group_key(sources_[heap_.front()]->Key());

      bool have_candidate = false;
      uint64_t best_seq = 0;
      bool best_deleted = false;
      std::string best_value;

      // Drain every version of this key. They are contiguous at the top of the
      // heap because the comparator sorts by key before sequence.
      while (!heap_.empty() && sources_[heap_.front()]->Key() == group_key) {
        const size_t idx = heap_.front();
        EntryIterator* src = sources_[idx].get();

        if (src->Seq() <= snapshot_) {
          if (!have_candidate || src->Seq() > best_seq) {
            have_candidate = true;
            best_seq = src->Seq();
            best_deleted = src->Deleted();
            best_value.assign(src->Value().data(), src->Value().size());
          }
        }

        src->Next();
        std::pop_heap(heap_.begin(), heap_.end(), cmp);
        heap_.pop_back();
        if (src->Valid()) {
          heap_.push_back(idx);
          std::push_heap(heap_.begin(), heap_.end(), cmp);
        }
      }

      if (have_candidate) {
        key_ = group_key;
        seq_ = best_seq;
        deleted_ = best_deleted;
        value_ = std::move(best_value);
        valid_ = true;
        return;
      }
      // Every version of this key is newer than the snapshot: skip the key.
    }

    valid_ = false;
    key_.clear();
    value_.clear();
    deleted_ = false;
    seq_ = 0;
  }

  std::vector<std::unique_ptr<EntryIterator>> sources_;
  std::vector<size_t> heap_;
  uint64_t snapshot_;

  std::string key_;
  std::string value_;
  uint64_t seq_ = 0;
  bool deleted_ = false;
  bool valid_ = false;
};

}  // namespace trove
