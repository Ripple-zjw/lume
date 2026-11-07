#include <string>
#include <utility>
#include <vector>

#include "search/merge.h"
#include "test_framework.h"

namespace {

AppEntry MakeApp(std::wstring title, std::wstring target) {
  AppEntry entry;
  entry.title = std::move(title);
  entry.launchTarget = std::move(target);
  return entry;
}

FileHit MakeFileHit(std::wstring path, bool isDirectory, size_t score, bool pathOnly,
                    uint8_t quality) {
  FileHit hit;
  hit.path = std::move(path);
  hit.isDirectory = isDirectory;
  hit.score = score;
  hit.pathOnly = pathOnly;
  hit.quality = quality;
  return hit;
}

}  // namespace

TEST(SubstringFileRanksAboveFuzzyApp) {
  const std::vector<AppEntry> apps{MakeApp(L"StreamingBar", L"C:\\apps\\streamingbar.exe")};
  const std::vector<FileHit> files{
      MakeFileHit(L"C:\\work\\my-steam-notes.txt", false, 1000, false, 1)};
  const std::vector<ResultRow> rows =
      BuildMergedRows(apps, files, nullptr, L"steam", 10, false, nullptr);
  CHECK_TRUE(rows.size() >= 2);
  // 文件是普通子串（quality=1），应用是子序列模糊（quality=0），文件必须在前。
  CHECK_EQ(rows[0].title, std::wstring(L"my-steam-notes.txt"));
}

TEST(SameQualityAndScoreOrderIsDeterministic) {
  const std::vector<FileHit> files{
      MakeFileHit(L"C:\\z\\same.txt", false, 900, false, 1),
      MakeFileHit(L"C:\\a\\same.txt", false, 900, false, 1)};
  const std::vector<ResultRow> rows =
      BuildMergedRows({}, files, nullptr, L"same", 10, false, nullptr);
  CHECK_EQ(rows.size(), static_cast<size_t>(2));
  CHECK_EQ(rows[0].subtitle, std::wstring(L"C:\\a\\same.txt"));
  CHECK_EQ(rows[1].subtitle, std::wstring(L"C:\\z\\same.txt"));
}

TEST(DirectoryPrefixRanksWithFiles) {
  const std::vector<FileHit> files{
      MakeFileHit(L"C:\\work\\photos.txt", false, 1200, false, 2),
      MakeFileHit(L"C:\\work\\photos", true, 1200, false, 2)};
  const std::vector<ResultRow> rows =
      BuildMergedRows({}, files, nullptr, L"photos", 10, false, nullptr);
  CHECK_EQ(rows.size(), static_cast<size_t>(2));
  // 目录名同样是前缀命中：提到与文件同级后，靠标题长度/全序比较排在前面。
  CHECK_EQ(rows[0].title, std::wstring(L"photos"));
}

TEST(PathOnlyHitsRankLast) {
  const std::vector<FileHit> files{
      MakeFileHit(L"C:\\projects\\work\\notes.txt", false, 200, true, 0),
      MakeFileHit(L"C:\\work\\notes.txt", false, 1000, false, 1)};
  const std::vector<ResultRow> rows =
      BuildMergedRows({}, files, nullptr, L"notes", 10, false, nullptr);
  CHECK_EQ(rows.size(), static_cast<size_t>(2));
  CHECK_EQ(rows[0].subtitle, std::wstring(L"C:\\work\\notes.txt"));
  CHECK_EQ(rows[1].tier, static_cast<uint8_t>(3));
}

TEST(PlaceholderOnlyWhenQueryHasNoHits) {
  const std::vector<ResultRow> rows =
      BuildMergedRows({}, {}, nullptr, L"zzz", 10, true, nullptr);
  CHECK_EQ(rows.size(), static_cast<size_t>(1));
  CHECK_TRUE(rows[0].action == RowAction::None);

  const std::vector<ResultRow> emptyQuery =
      BuildMergedRows({}, {}, nullptr, L"", 10, true, nullptr);
  CHECK_EQ(emptyQuery.size(), static_cast<size_t>(0));
}
