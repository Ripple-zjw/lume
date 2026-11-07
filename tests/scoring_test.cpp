#include <string>
#include <utility>

#include "search/app_entry.h"
#include "search/file_score.h"
#include "test_framework.h"

namespace {

AppEntry MakeApp(std::wstring title, std::wstring detail = L"", std::wstring target = L"") {
  AppEntry entry;
  entry.title = std::move(title);
  entry.detail = std::move(detail);
  entry.launchTarget = std::move(target);
  return entry;
}

}  // namespace

TEST(SubstringScoresMuchHigherThanFuzzy) {
  TextMatch match;
  const size_t score = ScoreLowerText(L"steamapps.txt", L"steam", &match);
  CHECK_TRUE(score > 1000);
  CHECK_TRUE(match.matched);
  CHECK_TRUE(match.contiguous);
}

TEST(GapPenaltyKeepsTightFuzzyAndDropsScattered) {
  // "steam" 在 StreamingBar 里只跳过一个字符：保留。
  CHECK_TRUE(ScoreLowerText(L"streamingbar.txt", L"steam") > 0);
  // "steam" 在 uninstall telegram 里跨了 9 个字符：按噪声丢弃。
  CHECK_EQ(ScoreLowerText(L"uninstall telegram.txt", L"steam"), static_cast<size_t>(0));
}

TEST(SingleCharOnlyMatchesWordStart) {
  CHECK_TRUE(ScoreLowerText(L"alpha.txt", L"a") > 0);
  CHECK_EQ(ScoreLowerText(L"data.txt", L"a"), static_cast<size_t>(0));
  CHECK_TRUE(ScoreLowerText(L"my-a.txt", L"a") > 0);
}

TEST(ShortQueriesDoNotFuzzyMatch) {
  CHECK_EQ(ScoreLowerText(L"chrome", L"cm"), static_cast<size_t>(0));
  CHECK_TRUE(ScoreLowerText(L"chrome", L"ch") > 0);
}

TEST(AcronymFuzzyStillMatches) {
  CHECK_TRUE(ScoreLowerText(L"visual studio code", L"vscode") > 0);
}

TEST(MultiWordFileNamesIgnoreWordOrder) {
  const FileNameScore forward =
      ScoreNameForQuery(L"visual studio code notes.txt", L"visual studio");
  const FileNameScore reverse =
      ScoreNameForQuery(L"visual studio code notes.txt", L"studio visual");
  CHECK_TRUE(forward.score > 0);
  CHECK_TRUE(reverse.score > 0);
  CHECK_EQ(reverse.quality, static_cast<uint8_t>(1));  // 两个词都在，但整串不连续

  // 空格不是文件名的一部分：chrome + setup 仍能命中 ChromeSetup。
  const FileNameScore joined = ScoreNameForQuery(L"chromsetup.exe", L"chrome setup");
  CHECK_TRUE(joined.score > 0);
}

TEST(AppQualitySeparatesExactPrefixSubstringFuzzy) {
  bool titleHit = false;
  uint8_t quality = 9;

  const AppEntry exact = MakeApp(L"Steam", L"", L"F:\\steam\\steam.exe");
  CHECK_TRUE(ScoreEntry(exact, L"steam", &titleHit, &quality) > 0);
  CHECK_TRUE(titleHit);
  CHECK_EQ(quality, static_cast<uint8_t>(4));

  const AppEntry prefix = MakeApp(L"Visual Studio Installer");
  CHECK_TRUE(ScoreEntry(prefix, L"visual studio", &titleHit, &quality) > 0);
  CHECK_EQ(quality, static_cast<uint8_t>(2));

  const AppEntry substring = MakeApp(L"EA Error Reporter");
  CHECK_TRUE(ScoreEntry(substring, L"report", &titleHit, &quality) > 0);
  CHECK_EQ(quality, static_cast<uint8_t>(1));

  // 子序列命中不能再伪装成普通子串。
  const AppEntry fuzzy = MakeApp(L"StreamingBar");
  CHECK_TRUE(ScoreEntry(fuzzy, L"steam", &titleHit, &quality) > 0);
  CHECK_TRUE(titleHit);
  CHECK_EQ(quality, static_cast<uint8_t>(0));

  const AppEntry noisy = MakeApp(L"Uninstall Telegram");
  CHECK_EQ(ScoreEntry(noisy, L"steam", &titleHit, &quality), static_cast<size_t>(0));
}

TEST(AppMultiWordTitleMatchesRegardlessOfOrder) {
  const AppEntry app = MakeApp(L"Visual Studio Code");
  bool titleHit = false;
  uint8_t quality = 0;
  CHECK_TRUE(ScoreEntry(app, L"studio visual", &titleHit, &quality) > 0);
  CHECK_TRUE(titleHit);
  CHECK_EQ(quality, static_cast<uint8_t>(1));
}

TEST(AppAuxPathFallbackNeedsAllWords) {
  const AppEntry app = MakeApp(L"Calculator", L"", L"C:\\Program Files\\WindowsApps\\Calc\\calc.exe");
  bool titleHit = false;
  uint8_t quality = 0;
  CHECK_TRUE(ScoreEntry(app, L"calc.exe", &titleHit, &quality) > 0);
  CHECK_FALSE(titleHit);
  CHECK_EQ(quality, static_cast<uint8_t>(0));
}
