#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <string>
#include <utility>
#include <vector>

#include "search/app_entry.h"
#include "search/merge.h"
#include "search/settings_pages.h"
#include "test_framework.h"

namespace {

bool HasUri(const std::vector<AppEntry>& pages, const wchar_t* uri) {
  for (const AppEntry& page : pages) {
    if (_wcsicmp(page.launchTarget.c_str(), uri) == 0) return true;
  }
  return false;
}

const AppEntry* FindPage(const std::vector<AppEntry>& pages, const wchar_t* uri) {
  for (const AppEntry& page : pages) {
    if (_wcsicmp(page.launchTarget.c_str(), uri) == 0) return &page;
  }
  return nullptr;
}

AppEntry MakeApp(std::wstring title, std::wstring target) {
  AppEntry entry;
  entry.title = std::move(title);
  entry.launchTarget = std::move(target);
  return entry;
}

}  // namespace

TEST(SettingsCatalogHasUniqueTitlesAndUris) {
  const std::vector<AppEntry> pages = CollectSettingsPages();
  CHECK_TRUE(pages.size() >= 50);
  for (size_t i = 0; i < pages.size(); ++i) {
    CHECK_TRUE(!pages[i].title.empty());
    CHECK_TRUE(pages[i].IsSettings());
    CHECK_TRUE(pages[i].launchTarget.rfind(L"ms-settings:", 0) == 0);
    CHECK_TRUE(!pages[i].detail.empty());
    CHECK_TRUE(!pages[i].keywords.empty());
    for (const std::wstring& keyword : pages[i].keywords) {
      CHECK_TRUE(!keyword.empty());
      CHECK_TRUE(_wcsicmp(keyword.c_str(), pages[i].title.c_str()) != 0);
    }
    for (size_t j = i + 1; j < pages.size(); ++j) {
      CHECK_TRUE(_wcsicmp(pages[i].title.c_str(), pages[j].title.c_str()) != 0);
      CHECK_TRUE(_wcsicmp(pages[i].launchTarget.c_str(), pages[j].launchTarget.c_str()) != 0);
    }
  }
}

TEST(SettingsCatalogExcludesHardwareOnlyPages) {
  // 无对应硬件的机器上这些页面会落到设置首页，所以目录里不收录。
  const std::vector<AppEntry> pages = CollectSettingsPages();
  const wchar_t* banned[] = {
      L"ms-settings:batterysaver",  L"ms-settings:devices-touchpad",
      L"ms-settings:devices-touch", L"ms-settings:wheel",
      L"ms-settings:pen-button",    L"ms-settings:maps",
      L"ms-settings:network-dialup", L"ms-settings:wifi-provisioning",
      L"ms-settings:privacy-eyetracker"};
  for (const wchar_t* uri : banned) CHECK_FALSE(HasUri(pages, uri));
}

TEST(SettingsTitleExactMatchWins) {
  const std::vector<AppEntry> pages = CollectSettingsPages();
  const AppEntry* page = FindPage(pages, L"ms-settings:startupapps");
  CHECK_TRUE(page != nullptr);
  if (!page) return;
  bool titleHit = false;
  uint8_t quality = 9;
  CHECK_TRUE(ScoreEntry(*page, L"启动应用", &titleHit, &quality) > 0);
  CHECK_TRUE(titleHit);
  CHECK_EQ(quality, static_cast<uint8_t>(4));
}

TEST(SettingsKeywordMatchesUninstall) {
  const std::vector<AppEntry> pages = CollectSettingsPages();
  const AppEntry* page = FindPage(pages, L"ms-settings:appsfeatures");
  CHECK_TRUE(page != nullptr);
  if (!page) return;
  bool titleHit = false;
  uint8_t quality = 0;
  CHECK_TRUE(ScoreEntry(*page, L"卸载", &titleHit, &quality) > 0);
  CHECK_TRUE(titleHit);
  CHECK_TRUE(quality >= 2 && quality <= 3);

  bool installHit = false;
  CHECK_TRUE(ScoreEntry(*page, L"uninstall", &installHit, nullptr) > 0);
  CHECK_TRUE(installHit);
}

TEST(SettingsNamespaceWordIsStripped) {
  const std::vector<AppEntry> pages = CollectSettingsPages();
  const AppEntry* bluetooth = FindPage(pages, L"ms-settings:bluetooth");
  CHECK_TRUE(bluetooth != nullptr);
  if (!bluetooth) return;
  bool titleHit = false;
  uint8_t quality = 0;
  // 「设置 蓝牙」剥掉命名空间词后等于标题本身：按标题精确命中。
  CHECK_TRUE(ScoreEntry(*bluetooth, L"设置 蓝牙", &titleHit, &quality) > 0);
  CHECK_TRUE(titleHit);
  CHECK_EQ(quality, static_cast<uint8_t>(4));

  // 英文关键词同理。
  uint8_t englishQuality = 0;
  CHECK_TRUE(ScoreEntry(*bluetooth, L"settings bluetooth", nullptr, &englishQuality) > 0);
  CHECK_EQ(englishQuality, static_cast<uint8_t>(3));
}

TEST(SettingsNamespaceOnlyQueryDoesNotDumpCatalog) {
  const std::vector<AppEntry> pages = CollectSettingsPages();
  size_t single = 0;
  size_t both = 0;
  for (const AppEntry& page : pages) {
    if (ScoreEntry(page, L"设置") > 0) ++single;
    if (ScoreEntry(page, L"设置 settings") > 0) ++both;
  }
  CHECK_EQ(single, static_cast<size_t>(0));
  CHECK_EQ(both, static_cast<size_t>(0));
}

TEST(SettingsKeywordIsNotFuzzy) {
  const std::vector<AppEntry> pages = CollectSettingsPages();
  const AppEntry* bluetooth = FindPage(pages, L"ms-settings:bluetooth");
  CHECK_TRUE(bluetooth != nullptr);
  if (!bluetooth) return;
  // "bluto" 是 bluetooth 的子序列，但关键词只认连续命中。
  CHECK_EQ(ScoreEntry(*bluetooth, L"bluto"), static_cast<size_t>(0));
  CHECK_TRUE(ScoreEntry(*bluetooth, L"bluetooth") > 0);
}

TEST(SettingsUriDoesNotMatchByLaunchTarget) {
  const std::vector<AppEntry> pages = CollectSettingsPages();
  size_t matched = 0;
  for (const AppEntry& page : pages) {
    if (ScoreEntry(page, L"ms-settings") > 0) ++matched;
  }
  CHECK_EQ(matched, static_cast<size_t>(0));
}

TEST(SettingsRowsGetOpenSettingsAction) {
  std::vector<AppEntry> entries = CollectSettingsPages();
  const std::vector<ResultRow> rows =
      BuildMergedRows(entries, {}, nullptr, L"开机自启", 5, false, nullptr);
  CHECK_TRUE(!rows.empty());
  if (rows.empty()) return;
  CHECK_TRUE(rows[0].action == RowAction::OpenSettings);
  CHECK_EQ(rows[0].target, std::wstring(L"ms-settings:startupapps"));
  CHECK_EQ(rows[0].tier, static_cast<uint8_t>(0));
  CHECK_EQ(rows[0].quality, static_cast<uint8_t>(3));  // 关键词完全命中
}

TEST(SettingsExactTitleRanksAboveKeyword) {
  std::vector<AppEntry> entries = CollectSettingsPages();
  // 「卸载」只命中「应用和功能」的关键词；同名应用应靠质量 4 排在它前面。
  entries.push_back(MakeApp(L"卸载", L"C:\\tools\\uninstall.exe"));
  const std::vector<ResultRow> rows =
      BuildMergedRows(entries, {}, nullptr, L"卸载", 5, false, nullptr);
  CHECK_TRUE(rows.size() >= 2);
  if (rows.size() < 2) return;
  CHECK_EQ(rows[0].title, std::wstring(L"卸载"));
  CHECK_TRUE(rows[0].action == RowAction::LaunchApp);
  CHECK_TRUE(rows[1].action == RowAction::OpenSettings);
}

TEST(SettingsPagesAvailableOnWindows) {
  CHECK_TRUE(SettingsProtocolAvailable());
  CHECK_TRUE(!CollectSettingsPages().empty());
}

TEST(SettingsEmptyQueryReturnsEverything) {
  const std::vector<AppEntry> pages = CollectSettingsPages();
  const std::vector<const AppEntry*> all = FilterEntries(pages, L"", 200);
  CHECK_EQ(all.size(), pages.size());
}

TEST(StripNamespaceWordsKeepsRealWords) {
  CHECK_EQ(StripNamespaceWords(L"设置 蓝牙"), std::wstring(L"蓝牙"));
  CHECK_EQ(StripNamespaceWords(L"settings bluetooth"), std::wstring(L"bluetooth"));
  CHECK_EQ(StripNamespaceWords(L"设置"), std::wstring(L"设置"));
  CHECK_EQ(StripNamespaceWords(L"设置 settings"), std::wstring(L"设置 settings"));
  CHECK_EQ(StripNamespaceWords(L"卸载 软件"), std::wstring(L"卸载 软件"));
}
