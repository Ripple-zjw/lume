#pragma once

#include <windows.h>

#include <string>

// 单行输入状态：文本、光标、IME 组字。
class TextInput {
 public:
  void SetText(std::wstring text);
  const std::wstring& text() const { return text_; }
  size_t caret() const { return caret_; }
  bool composing() const { return composing_; }
  const std::wstring& composition() const { return composition_; }
  size_t compositionCaret() const { return compositionCaret_; }
  size_t compositionStart() const { return caret_; }

  bool InsertChar(wchar_t ch);
  bool PasteFromClipboard(HWND owner);
  bool Backspace();
  bool DeleteForward();
  bool MoveLeft();
  bool MoveRight();
  bool MoveHome();
  bool MoveEnd();

  void BeginComposition();
  void SetComposition(std::wstring text, size_t caret);
  void CommitComposition(std::wstring text);
  void CancelComposition();

  // 组字串已插入到光标处后的显示文本与显示光标位置（UTF-16 索引）。
  std::wstring DisplayText() const;
  size_t DisplayCaret() const;

 private:
  bool Insert(std::wstring text);
  size_t PrevCodePoint(size_t pos) const;
  size_t NextCodePoint(size_t pos) const;

  std::wstring text_;
  size_t caret_ = 0;
  bool composing_ = false;
  std::wstring composition_;
  size_t compositionCaret_ = 0;
};
