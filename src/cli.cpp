// trove/cli.cpp — interactive shell for poking at a live database.
//
// This exists because a storage engine you cannot inspect by hand is a storage
// engine you cannot debug. `stats` shows where the data actually lives, and
// `flush`/`compact` let you force the interesting transitions instead of waiting
// for thresholds to trip.
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "trove/db.hpp"
#include "trove/util.hpp"

namespace {

constexpr size_t kMaxDisplayValue = 60;

std::vector<std::string> Tokenize(const std::string& line) {
  std::istringstream is(line);
  std::vector<std::string> out;
  std::string tok;
  while (is >> tok) out.push_back(tok);
  return out;
}

// Everything after the command and the key, verbatim — so values may contain
// spaces without needing quotes.
std::string ValuePart(const std::string& line, size_t skip_tokens) {
  std::istringstream is(line);
  std::string tok;
  size_t seen = 0;
  std::streampos pos = 0;
  while (seen < skip_tokens && (is >> tok)) {
    pos = is.tellg();
    ++seen;
  }
  if (seen < skip_tokens || pos < 0) return {};
  std::string rest = line.substr(static_cast<size_t>(pos));
  const size_t begin = rest.find_first_not_of(" \t");
  return begin == std::string::npos ? std::string{} : rest.substr(begin);
}

void PrintHelp(std::ostream& out) {
  out << "commands:\n"
      << "  put <key> <value>        insert or overwrite\n"
      << "  get <key>                point read\n"
      << "  del <key>                insert a tombstone\n"
      << "  scan [start] [end] [n]   half-open range, n=0 means unlimited\n"
      << "  stats                    where the bytes live\n"
      << "  flush                    force memtable -> L0\n"
      << "  compact                  force a full compaction\n"
      << "  seq                      last allocated sequence number\n"
      << "  help                     this list\n"
      << "  quit                     close the database\n";
}

void PrintStats(trove::DB& db, std::ostream& out) {
  const trove::Stats s = db.GetStats();
  out << "last sequence     " << s.last_sequence << "\n"
      << "memtable          " << trove::HumanBytes(s.memtable_bytes) << "  (" << s.memtable_entries
      << " entries)\n"
      << "immutable         " << trove::HumanBytes(s.immutable_bytes) << "\n"
      << "wal               " << trove::HumanBytes(s.wal_bytes) << "\n"
      << "sstables          " << trove::HumanBytes(s.sstable_bytes) << "  (" << s.num_files
      << " files, " << s.sstable_records << " records)\n"
      << "flushes           " << s.flushes << "\n"
      << "compactions       " << s.compactions << "\n";
  for (size_t level = 0; level < s.level_files_bytes.size(); ++level) {
    out << "  L" << level << "              " << s.level_files_bytes[level].first << " files, "
        << trove::HumanBytes(s.level_files_bytes[level].second) << "\n";
  }
}

// Returns false when the shell should exit.
bool RunCommand(trove::DB& db, const std::string& line, std::ostream& out, std::ostream& err) {
  const std::vector<std::string> tok = Tokenize(line);
  if (tok.empty()) return true;

  const std::string& cmd = tok[0];
  std::string error;

  if (cmd == "quit" || cmd == "exit") return false;
  if (cmd == "help" || cmd == "?") {
    PrintHelp(out);
    return true;
  }
  if (cmd == "put") {
    if (tok.size() < 3) {
      err << "usage: put <key> <value>\n";
      return true;
    }
    const std::string value = ValuePart(line, 2);
    if (!db.Put(tok[1], value, &error)) {
      err << "error: " << error << "\n";
    } else {
      out << "ok\n";
    }
    return true;
  }
  if (cmd == "get") {
    if (tok.size() < 2) {
      err << "usage: get <key>\n";
      return true;
    }
    std::string value;
    bool found = false;
    if (!db.Get(tok[1], &value, &found, &error)) {
      err << "error: " << error << "\n";
    } else if (!found) {
      out << "(not found)\n";
    } else {
      const std::string shown =
          value.size() > kMaxDisplayValue ? value.substr(0, kMaxDisplayValue) + "..." : value;
      out << trove::PrintableKey(shown) << "\n";
    }
    return true;
  }
  if (cmd == "del") {
    if (tok.size() < 2) {
      err << "usage: del <key>\n";
      return true;
    }
    if (!db.Delete(tok[1], &error)) {
      err << "error: " << error << "\n";
    } else {
      out << "ok\n";
    }
    return true;
  }
  if (cmd == "scan") {
    const std::string start = tok.size() > 1 ? tok[1] : "";
    const std::string end = tok.size() > 2 ? tok[2] : "";
    size_t limit = 50;
    if (tok.size() > 3) {
      try {
        limit = static_cast<size_t>(std::stoull(tok[3]));
      } catch (...) {
        err << "scan: limit must be a number\n";
        return true;
      }
    }
    std::vector<std::pair<std::string, std::string>> rows;
    if (!db.Scan(start, end, limit, &rows, &error)) {
      err << "error: " << error << "\n";
      return true;
    }
    for (const auto& [k, v] : rows) {
      const std::string shown =
          v.size() > kMaxDisplayValue ? v.substr(0, kMaxDisplayValue) + "..." : v;
      out << trove::PrintableKey(k) << "\t" << trove::PrintableKey(shown) << "\n";
    }
    out << "(" << rows.size() << " rows)\n";
    return true;
  }
  if (cmd == "stats") {
    PrintStats(db, out);
    return true;
  }
  if (cmd == "flush") {
    if (!db.Flush(&error)) {
      err << "error: " << error << "\n";
    } else {
      out << "flushed\n";
    }
    return true;
  }
  if (cmd == "compact") {
    if (!db.CompactAll(&error)) {
      err << "error: " << error << "\n";
    } else {
      out << "compacted\n";
    }
    return true;
  }
  if (cmd == "seq") {
    out << db.LastSequence() << "\n";
    return true;
  }

  err << "unknown command: " << cmd << "  (try: help)\n";
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "usage: trove <db-dir> [--sync] [--buffer BYTES] [--exec \"<command>\"]\n";
    return 2;
  }

  const std::string dir = argv[1];
  trove::Options opts;
  std::string script;
  bool have_script = false;

  for (int i = 2; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--sync") {
      opts.sync_writes = true;
    } else if (arg == "--buffer" && i + 1 < argc) {
      try {
        opts.write_buffer_size = static_cast<size_t>(std::stoull(argv[++i]));
      } catch (...) {
        std::cerr << "error: --buffer needs a byte count\n";
        return 2;
      }
    } else if (arg == "--exec" && i + 1 < argc) {
      script = argv[++i];
      have_script = true;
    } else {
      std::cerr << "error: unknown option " << arg << "\n";
      return 2;
    }
  }

  std::string error;
  auto db = trove::DB::Open(dir, opts, &error);
  if (db == nullptr) {
    std::cerr << "error: " << error << "\n";
    return 1;
  }

  if (have_script) {
    return RunCommand(*db, script, std::cout, std::cerr) ? 0 : 0;
  }

  std::cout << "trove \u2014 " << db->dir() << "\n(type 'help' for commands)\n";
  std::string line;
  while (true) {
    std::cout << "trove> " << std::flush;
    if (!std::getline(std::cin, line)) break;
    if (!RunCommand(*db, line, std::cout, std::cerr)) break;
  }
  return 0;
}
