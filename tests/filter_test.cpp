#include <string>
#include <vector>

#include "search/filter.h"
#include "test_framework.h"

TEST(DefaultFilterSetIsValid) {
  FilterSet set = DefaultFilterSet();
  CHECK_EQ(set.groups.size(), static_cast<size_t>(6));
  CHECK_EQ(set.active, 0);
  CHECK_TRUE(set.enabled);

  FilterSet validated = set;
  std::wstring error;
  CHECK_TRUE(SanitizeFilterSet(&validated, &error));
  CHECK_EQ(error.size(), static_cast<size_t>(0));

  const std::vector<std::wstring> labels = FilterChipLabels(set);
  CHECK_EQ(labels.size(), static_cast<size_t>(7));
  CHECK_EQ(labels[0], std::wstring(L"全部"));
}

TEST(ParseExtensionListNormalizes) {
  const std::vector<std::wstring> extensions = ParseExtensionList(L"*.PNG, .jpg; mp4  7z");
  CHECK_EQ(extensions.size(), static_cast<size_t>(4));
  CHECK_EQ(extensions[0], std::wstring(L"png"));
  CHECK_EQ(extensions[1], std::wstring(L"jpg"));
  CHECK_EQ(extensions[2], std::wstring(L"mp4"));
  CHECK_EQ(extensions[3], std::wstring(L"7z"));
}

TEST(FilterGroupEncodeDecodeRoundTrip) {
  FilterSet set = DefaultFilterSet();
  for (const FilterGroup& group : set.groups) {
    FilterGroup decoded;
    CHECK_TRUE(DecodeFilterGroup(EncodeFilterGroup(group), &decoded));
    CHECK_EQ(decoded.name, group.name);
    CHECK_EQ(decoded.extensions.size(), group.extensions.size());
    CHECK_EQ(decoded.includeApps, group.includeApps);
    CHECK_EQ(decoded.includeFolders, group.includeFolders);
    CHECK_EQ(decoded.includeNoExtension, group.includeNoExtension);
  }
}

TEST(SanitizeFilterGroupClampsInput) {
  FilterGroup group;
  group.name = L"  图片|图标  ";
  group.extensions = {L"*.PNG", L"png", L"JPG", L".jpeg", L"b?d", L""};
  const FilterGroup clean = SanitizeFilterGroup(group);
  CHECK_EQ(clean.name, std::wstring(L"图片图标"));
  CHECK_EQ(clean.extensions.size(), static_cast<size_t>(3));
  CHECK_EQ(clean.extensions[0], std::wstring(L"png"));
  CHECK_EQ(clean.extensions[1], std::wstring(L"jpg"));
  CHECK_EQ(clean.extensions[2], std::wstring(L"jpeg"));
}

TEST(FilterMatchesPathByExtensionAndSwitches) {
  FilterGroup group;
  group.name = L"图片";
  group.extensions = {L"png", L"jpg"};
  CHECK_TRUE(FilterMatchesPath(group, L"C:\\pics\\a.PNG", false));
  CHECK_FALSE(FilterMatchesPath(group, L"C:\\pics\\a.txt", false));
  CHECK_FALSE(FilterMatchesPath(group, L"C:\\pics\\folder", true));

  group.includeFolders = true;
  CHECK_TRUE(FilterMatchesPath(group, L"C:\\pics\\folder", true));

  FilterGroup filesOnly;
  filesOnly.name = L"文件";
  filesOnly.extensions = {L"png"};
  filesOnly.includeNoExtension = true;
  CHECK_TRUE(FilterMatchesPath(filesOnly, L"C:\\pics\\README", false));
  CHECK_FALSE(FilterMatchesPath(filesOnly, L"C:\\pics\\README", true));
}

TEST(ParseFilterSpecAcceptsAllForms) {
  FilterSet set = DefaultFilterSet();
  bool enabled = false;
  int index = -1;

  CHECK_TRUE(ParseFilterSpec(set, L"all", &enabled, &index));
  CHECK_TRUE(enabled);
  CHECK_EQ(index, 0);

  CHECK_TRUE(ParseFilterSpec(set, L"off", &enabled, &index));
  CHECK_FALSE(enabled);
  CHECK_EQ(index, 0);

  CHECK_TRUE(ParseFilterSpec(set, L"2", &enabled, &index));
  CHECK_TRUE(enabled);
  CHECK_EQ(index, 1);

  CHECK_TRUE(ParseFilterSpec(set, L"图片", &enabled, &index));
  CHECK_EQ(index, 2);

  CHECK_FALSE(ParseFilterSpec(set, L"不存在", &enabled, &index));
}

TEST(EverythingPushdownOnlyForPureExtensionGroups) {
  FilterGroup group;
  group.name = L"图片";
  group.extensions = {L"png", L"jpg"};
  CHECK_TRUE(FilterIsExtensionWhitelist(group));
  CHECK_EQ(FilterEverythingSpec(group), std::wstring(L"png;jpg"));

  FilterGroup withFolders = group;
  withFolders.includeFolders = true;
  CHECK_FALSE(FilterIsExtensionWhitelist(withFolders));

  FilterGroup withNoExtension = group;
  withNoExtension.includeNoExtension = true;
  CHECK_FALSE(FilterIsExtensionWhitelist(withNoExtension));
}

TEST(ClampFilterActiveKeepsValidIndex) {
  FilterSet set = DefaultFilterSet();
  set.active = 99;
  ClampFilterActive(&set);
  CHECK_EQ(set.active, static_cast<int>(set.groups.size()));
  set.active = -3;
  ClampFilterActive(&set);
  CHECK_EQ(set.active, 0);

  CHECK_TRUE(ActiveFilterGroup(set) == nullptr);
  set.active = 1;
  CHECK_TRUE(ActiveFilterGroup(set) != nullptr);
}
