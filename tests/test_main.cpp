#include "test_framework.h"

#include <cstdio>
#include <vector>

namespace {

struct TestCase {
  const char* name;
  void (*fn)();
};

std::vector<TestCase>& Tests() {
  static std::vector<TestCase> tests;
  return tests;
}

int gChecks = 0;
int gFailures = 0;

}  // namespace

void RegisterTest(const char* name, void (*fn)()) { Tests().push_back({name, fn}); }

void ReportCheck() { ++gChecks; }

void ReportFailure(const char* file, int line, const std::string& message) {
  ++gFailures;
  std::printf("    FAIL %s:%d %s\n", file, line, message.c_str());
}

int wmain() {
  int failedTests = 0;
  for (const TestCase& test : Tests()) {
    const int before = gFailures;
    test.fn();
    if (gFailures == before) {
      std::printf("[PASS] %s\n", test.name);
    } else {
      std::printf("[FAIL] %s\n", test.name);
      ++failedTests;
    }
  }
  std::printf("%zu tests, %d checks, %d failures\n", Tests().size(), gChecks, gFailures);
  return failedTests == 0 ? 0 : 1;
}
