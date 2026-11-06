#include <exception>
#include <iostream>

#include "test_framework.hpp"

namespace trove_test {
std::vector<TestCase>& Registry() {
  static std::vector<TestCase> registry;
  return registry;
}
}  // namespace trove_test

int main() {
  const auto& tests = trove_test::Registry();
  size_t failed = 0;
  for (const auto& test : tests) {
    try {
      test.fn();
      std::cout << "[ ok ] " << test.name << "\n";
    } catch (const std::exception& e) {
      ++failed;
      std::cout << "[FAIL] " << test.name << "\n       " << e.what() << "\n";
    }
  }
  std::cout << "\n" << (tests.size() - failed) << "/" << tests.size() << " passed\n";
  return failed == 0 ? 0 : 1;
}
