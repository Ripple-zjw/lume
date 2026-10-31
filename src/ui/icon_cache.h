#pragma once

#include <d2d1_3.h>
#include <windows.h>

#include <string>
#include <unordered_map>

#include "core/com_ptr.h"

class Renderer;

// shell 图标缓存：key 为 SHCreateItemFromParsingName 可解析的路径/AUMID 名称。
// 失败结果也会缓存，避免每帧重复尝试。
class IconCache {
 public:
  bool Init(Renderer* renderer);
  ID2D1Bitmap1* Get(const std::wstring& parsingName, UINT size = 32);

 private:
  Renderer* renderer_ = nullptr;
  std::unordered_map<std::wstring, ComPtr<ID2D1Bitmap1>> cache_;
};
