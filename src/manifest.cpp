#include "trove/manifest.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>

#include "trove/util.hpp"

namespace trove {
namespace fs = std::filesystem;

bool Manifest::Load(const std::string& path, Manifest* out, std::string* error) {
  std::error_code ec;
  if (!fs::exists(path, ec)) return true;  // brand-new database

  std::ifstream in(path, std::ios::binary);
  if (!in) {
    if (error != nullptr) *error = "cannot read manifest: " + path;
    return false;
  }

  std::string line;
  if (!std::getline(in, line)) {
    if (error != nullptr) *error = "empty manifest: " + path;
    return false;
  }
  if (line.rfind("trove-manifest", 0) != 0) {
    if (error != nullptr) *error = "not a trove manifest (bad header): " + path;
    return false;
  }

  while (std::getline(in, line)) {
    if (line.empty()) continue;
    std::istringstream ls(line);
    std::string tag;
    ls >> tag;
    if (tag == "next_file") {
      ls >> out->next_file_number;
    } else if (!tag.empty() && tag[0] == 'L') {
      int level = -1;
      try {
        level = std::stoi(tag.substr(1));
      } catch (...) {
        if (error != nullptr) *error = "corrupt level tag in manifest: " + tag;
        return false;
      }
      if (level < 0 || level > 64) {
        if (error != nullptr) *error = "level out of range in manifest: " + tag;
        return false;
      }
      FileMeta meta;
      ls >> meta.number >> meta.file_size >> meta.record_count;
      if (!ls) {
        if (error != nullptr) *error = "truncated manifest record: " + line;
        return false;
      }
      if (static_cast<size_t>(level) >= out->levels.size()) out->levels.resize(level + 1);
      out->levels[level].push_back(meta);
    }
  }
  return true;
}

bool Manifest::Save(const std::string& path, std::string* error) const {
  const std::string tmp = path + ".tmp";

  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) {
      if (error != nullptr) *error = "cannot write manifest: " + tmp;
      return false;
    }
    out << "trove-manifest v" << kVersion << "\n";
    out << "next_file " << next_file_number << "\n";
    for (size_t level = 0; level < levels.size(); ++level) {
      for (const auto& meta : levels[level]) {
        out << 'L' << level << ' ' << meta.number << ' ' << meta.file_size << ' ' << meta.record_count << "\n";
      }
    }
    out.flush();
    if (!out) {
      if (error != nullptr) *error = "manifest write failed: " + tmp;
      return false;
    }
  }

  std::error_code ec;
  fs::remove(path, ec);  // rename() will not clobber on every platform
  fs::rename(tmp, path, ec);
  if (ec) {
    if (error != nullptr) *error = "cannot install manifest (rename failed): " + ec.message();
    return false;
  }
  SyncDir(fs::path(path).parent_path().string());
  return true;
}

uint64_t Manifest::total_files() const {
  uint64_t n = 0;
  for (const auto& level : levels) n += level.size();
  return n;
}

uint64_t Manifest::total_bytes() const {
  uint64_t n = 0;
  for (const auto& level : levels) {
    for (const auto& meta : level) n += meta.file_size;
  }
  return n;
}

}  // namespace trove
