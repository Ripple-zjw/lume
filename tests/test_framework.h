#pragma once

#include <string>

// 极简单测框架：不引第三方依赖，任何断言失败都会让测试进程返回非 0。
void RegisterTest(const char* name, void (*fn)());
void ReportCheck();
void ReportFailure(const char* file, int line, const std::string& message);

#define TEST(name)                                       \
  static void name();                                    \
  [[maybe_unused]] static const bool name##_registered = \
      (RegisterTest(#name, name), true);                 \
  static void name()

#define CHECK_TRUE(expr)                                   \
  do {                                                     \
    ReportCheck();                                         \
    if (!(expr)) ReportFailure(__FILE__, __LINE__, #expr); \
  } while (0)

#define CHECK_FALSE(expr) CHECK_TRUE(!(expr))

#define CHECK_EQ(actual, expected)                                                 \
  do {                                                                             \
    ReportCheck();                                                                 \
    const auto actualValue_ = (actual);                                            \
    const auto expectedValue_ = (expected);                                        \
    if (!(actualValue_ == expectedValue_)) {                                       \
      ReportFailure(__FILE__, __LINE__, std::string(#actual) + " != " + #expected); \
    }                                                                              \
  } while (0)
