// trove/iterator.hpp — the cursor interface every read path speaks.
//
// Every source of entries (memtable, SSTable, and the merge of several of them)
// yields raw versioned entries ordered by (key ASC, seq DESC). Duplicate-key
// resolution happens in exactly one place — MergeIterator — so tombstones and
// snapshot visibility cannot be forgotten by one code path but not another.
#pragma once

#include <cstdint>
#include <string_view>

namespace trove {

class EntryIterator {
 public:
  virtual ~EntryIterator() = default;

  virtual bool Valid() const = 0;
  virtual void Next() = 0;

  virtual std::string_view Key() const = 0;
  virtual uint64_t Seq() const = 0;
  virtual std::string_view Value() const = 0;
  virtual bool Deleted() const = 0;
};

}  // namespace trove
