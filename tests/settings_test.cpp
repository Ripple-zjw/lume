#include <string>

#include "app/settings.h"
#include "test_framework.h"

TEST(SettingsDefaultsAreStable) {
  const AppSettings settings;
  CHECK_EQ(settings.theme, std::wstring(L"system"));
  CHECK_EQ(settings.hotkey, std::wstring(L"Alt+Space"));
  CHECK_FALSE(settings.autostart);
  CHECK_TRUE(settings.tray);
  CHECK_EQ(settings.indexSizeMB, 512);
  CHECK_EQ(settings.fileSearch, std::wstring(L"local"));
  CHECK_EQ(settings.logLevel, std::wstring(L"info"));
  CHECK_EQ(settings.indexDir.size(), static_cast<size_t>(0));
  CHECK_EQ(settings.fileRoots.size(), static_cast<size_t>(0));
}

TEST(SettingsCarriesDefaultFilterSet) {
  const AppSettings settings;
  CHECK_EQ(settings.filters.groups.size(), static_cast<size_t>(6));
  CHECK_EQ(settings.filters.active, 0);
  CHECK_TRUE(settings.filters.enabled);
  const std::vector<std::wstring> labels = FilterChipLabels(settings.filters);
  CHECK_EQ(labels.size(), static_cast<size_t>(7));
}
