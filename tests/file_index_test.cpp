#include <string>
#include <vector>

#include "search/file_index.h"
#include "test_framework.h"

namespace {

bool HasPathContaining(const std::vector<FileHit>& hits, const std::wstring& needle) {
  for (const FileHit& hit : hits) {
    if (hit.path.find(needle) != std::wstring::npos) return true;
  }
  return false;
}

std::vector<std::wstring> Paths(const std::vector<FileHit>& hits) {
  std::vector<std::wstring> paths;
  paths.reserve(hits.size());
  for (const FileHit& hit : hits) paths.push_back(hit.path);
  return paths;
}

}  // namespace

TEST(FileSearchPrefersRealMatchesAndDropsScatteredFuzzy) {
  FileIndex index;
  index.LoadPathsForTest({
      L"C:\\work\\steamapps.txt",
      L"C:\\work\\steam_setup.exe",
      L"C:\\work\\my-steam-notes.txt",
      L"C:\\work\\StreamingBar.txt",
      L"C:\\work\\uninstall telegram.txt",
  });
  const std::vector<FileHit> hits = index.Search(L"steam", 20, nullptr);
  CHECK_TRUE(hits.size() >= 3);
  CHECK_EQ(hits[0].path, std::wstring(L"C:\\work\\steamapps.txt"));
  CHECK_TRUE(hits[1].path == L"C:\\work\\steam_setup.exe" ||
             hits[1].path == L"C:\\work\\my-steam-notes.txt");
  // 跨 9 个字符的松散模糊命中（uninstall telegram）应被 gap 惩罚丢弃。
  CHECK_FALSE(HasPathContaining(hits, L"uninstall telegram"));
  // 紧凑模糊命中（StreamingBar）仍保留，但排在真实命中之后。
  CHECK_TRUE(HasPathContaining(hits, L"StreamingBar"));
  for (const FileHit& hit : hits) {
    if (hit.path.find(L"StreamingBar") != std::wstring::npos) {
      CHECK_EQ(hit.quality, static_cast<uint8_t>(0));
    }
  }
}

TEST(FileSearchMultiWordIgnoresWordOrder) {
  FileIndex index;
  index.LoadPathsForTest({
      L"C:\\work\\visual studio code notes.txt",
      L"C:\\work\\code studio visual.txt",
      L"C:\\work\\chromesetup.exe",
      L"C:\\work\\readme.md",
  });
  const std::vector<FileHit> forward = index.Search(L"visual studio", 10, nullptr);
  const std::vector<FileHit> reverse = index.Search(L"studio visual", 10, nullptr);
  CHECK_EQ(forward.size(), static_cast<size_t>(2));
  CHECK_EQ(reverse.size(), static_cast<size_t>(2));
  for (const FileHit& hit : reverse) {
    CHECK_EQ(hit.quality, static_cast<uint8_t>(1));
  }

  const std::vector<FileHit> joined = index.Search(L"chrome setup", 10, nullptr);
  CHECK_EQ(joined.size(), static_cast<size_t>(1));
  CHECK_TRUE(joined[0].path.find(L"chromesetup.exe") != std::wstring::npos);
  CHECK_EQ(joined[0].quality, static_cast<uint8_t>(1));
}

TEST(FileSearchSingleCharOnlyWordStart) {
  FileIndex index;
  index.LoadPathsForTest({
      L"C:\\work\\alpha.txt",
      L"C:\\work\\data.txt",
      L"C:\\work\\my-alphabet.txt",
  });
  const std::vector<FileHit> hits = index.Search(L"a", 10, nullptr);
  CHECK_EQ(hits.size(), static_cast<size_t>(2));
  CHECK_TRUE(HasPathContaining(hits, L"alpha.txt"));
  CHECK_TRUE(HasPathContaining(hits, L"my-alphabet.txt"));
  CHECK_FALSE(HasPathContaining(hits, L"data.txt"));
}

TEST(FileSearchFallsBackToPathWhenNameHitsAreFew) {
  FileIndex index;
  index.LoadPathsForTest({
      L"C:\\projects\\lume\\readme.md",
      L"C:\\other\\notes.txt",
  });
  const std::vector<FileHit> hits = index.Search(L"projects", 10, nullptr);
  CHECK_EQ(hits.size(), static_cast<size_t>(1));
  CHECK_TRUE(hits[0].pathOnly);
  CHECK_TRUE(hits[0].path.find(L"projects") != std::wstring::npos);
}

TEST(FileSearchOrderIsStableAcrossRepeatedSearches) {
  FileIndex index;
  index.LoadPathsForTest({
      L"C:\\a\\report.txt",
      L"C:\\b\\report.txt",
      L"C:\\c\\report-final.txt",
      L"C:\\d\\quarterly report.docx",
  });
  const std::vector<std::wstring> first = Paths(index.Search(L"report", 20, nullptr));
  const std::vector<std::wstring> second = Paths(index.Search(L"report", 20, nullptr));
  CHECK_EQ(first.size(), static_cast<size_t>(second.size()));
  for (size_t i = 0; i < first.size(); ++i) CHECK_EQ(first[i], second[i]);
}
