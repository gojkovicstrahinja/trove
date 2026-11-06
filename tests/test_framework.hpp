// Minimal test harness: no external dependency, so `ctest` works on a machine
// with nothing but a compiler.
#pragma once

#include <functional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace trove_test {

struct TestCase {
  std::string name;
  std::function<void()> fn;
};

std::vector<TestCase>& Registry();

struct Registrar {
  Registrar(const char* name, std::function<void()> fn) { Registry().push_back(TestCase{name, std::move(fn)}); }
};

inline void Fail(const char* file, int line, const std::string& message) {
  std::ostringstream os;
  os << file << ":" << line << ": " << message;
  throw std::runtime_error(os.str());
}

}  // namespace trove_test

#define TEST(name)                                                        \
  static void name();                                                     \
  static ::trove_test::Registrar registrar_##name(#name, name);           \
  static void name()

#define CHECK(cond)                                                       \
  do {                                                                    \
    if (!(cond)) ::trove_test::Fail(__FILE__, __LINE__, "CHECK failed: " #cond); \
  } while (false)

#define CHECK_EQ(a, b)                                                    \
  do {                                                                    \
    const auto& _va = (a);                                                \
    const auto& _vb = (b);                                                \
    if (!(_va == _vb)) {                                                  \
      std::ostringstream _os;                                             \
      _os << "CHECK_EQ failed: " #a " == " #b " (" << _va << " vs " << _vb << ")"; \
      ::trove_test::Fail(__FILE__, __LINE__, _os.str());                   \
    }                                                                     \
  } while (false)
