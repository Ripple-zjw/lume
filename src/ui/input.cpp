#include "ui/input.h"

#include <algorithm>

namespace {

bool IsHighSurrogate(wchar_t ch) { return ch >= 0xD800 && ch <= 0xDBFF; }
bool IsLowSurrogate(wchar_t ch) { return ch >= 0xDC00 && ch <= 0xDFFF; }

bool IsPrintable(wchar_t ch) { return ch >= 0x20 && ch != 0x7F; }

void StripControlChars(std::wstring* text) {
  std::wstring cleaned;
  cleaned.reserve(text->size());
  for (wchar_t ch : *text) {
    if (ch == L'\r' || ch == L'\n' || ch == L'\t') {
      cleaned.push_back(L' ');
    } else if (IsPrintable(ch)) {
      cleaned.push_back(ch);
    }
  }
  *text = std::move(cleaned);
}

}  // namespace

void TextInput::SetText(std::wstring text) {
  text_ = std::move(text);
  caret_ = text_.size();
  composing_ = false;
  composition_.clear();
  compositionCaret_ = 0;
}

bool TextInput::Insert(std::wstring text) {
  if (text.empty()) return false;
  text_.insert(caret_, text);
  caret_ += text.size();
  return true;
}

bool TextInput::InsertChar(wchar_t ch) {
  if (composing_ || !IsPrintable(ch)) return false;
  return Insert(std::wstring(1, ch));
}

bool TextInput::PasteFromClipboard(HWND owner) {
  if (composing_ || !OpenClipboard(owner)) return false;
  bool changed = false;
  HANDLE data = GetClipboardData(CF_UNICODETEXT);
  if (data) {
    const auto* text = static_cast<const wchar_t*>(GlobalLock(data));
    if (text) {
      std::wstring pasted(text);
      GlobalUnlock(data);
      StripControlChars(&pasted);
      changed = Insert(pasted);
    }
  }
  CloseClipboard();
  return changed;
}

bool TextInput::Backspace() {
  if (composing_ || caret_ == 0) return false;
  const size_t previous = PrevCodePoint(caret_);
  text_.erase(previous, caret_ - previous);
  caret_ = previous;
  return true;
}

bool TextInput::DeleteForward() {
  if (composing_ || caret_ >= text_.size()) return false;
  const size_t next = NextCodePoint(caret_);
  text_.erase(caret_, next - caret_);
  return true;
}

bool TextInput::MoveLeft() {
  if (composing_) return false;
  if (caret_ == 0) return false;
  caret_ = PrevCodePoint(caret_);
  return true;
}

bool TextInput::MoveRight() {
  if (composing_) return false;
  if (caret_ >= text_.size()) return false;
  caret_ = NextCodePoint(caret_);
  return true;
}

bool TextInput::MoveHome() {
  if (composing_ || caret_ == 0) return false;
  caret_ = 0;
  return true;
}

bool TextInput::MoveEnd() {
  if (composing_ || caret_ == text_.size()) return false;
  caret_ = text_.size();
  return true;
}

void TextInput::BeginComposition() {
  composing_ = true;
  composition_.clear();
  compositionCaret_ = 0;
}

void TextInput::SetComposition(std::wstring text, size_t caret) {
  composing_ = true;
  composition_ = std::move(text);
  compositionCaret_ = std::min(caret, composition_.size());
}

void TextInput::CommitComposition(std::wstring text) {
  composing_ = false;
  composition_.clear();
  compositionCaret_ = 0;
  Insert(std::move(text));
}

void TextInput::CancelComposition() {
  composing_ = false;
  composition_.clear();
  compositionCaret_ = 0;
}

std::wstring TextInput::DisplayText() const {
  if (!composing_ || composition_.empty()) return text_;
  std::wstring display;
  display.reserve(text_.size() + composition_.size());
  display.append(text_, 0, caret_);
  display.append(composition_);
  display.append(text_, caret_, std::wstring::npos);
  return display;
}

size_t TextInput::DisplayCaret() const {
  return caret_ + (composing_ ? compositionCaret_ : 0);
}

size_t TextInput::PrevCodePoint(size_t pos) const {
  if (pos >= 2 && IsLowSurrogate(text_[pos - 1]) && IsHighSurrogate(text_[pos - 2])) {
    return pos - 2;
  }
  return pos - 1;
}

size_t TextInput::NextCodePoint(size_t pos) const {
  if (pos + 1 < text_.size() && IsHighSurrogate(text_[pos]) && IsLowSurrogate(text_[pos + 1])) {
    return pos + 2;
  }
  return pos + 1;
}
