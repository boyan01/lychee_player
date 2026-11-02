#include "media/text_encoding.h"

#include <windows.h>

#include <cctype>
#include <climits>
#include <string>

namespace lychee {

namespace {

// Windows names encodings by code page.
UINT CodePage(const std::string& encoding) {
  std::string name;
  for (char c : encoding) {
    name += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  static constexpr struct {
    const char* name;
    UINT code_page;
  } kCodePages[] = {
      {"gb18030", 54936},     {"gbk", 936},           {"gb2312", 936},
      {"big5", 950},          {"shift_jis", 932},     {"euc-jp", 20932},
      {"euc-kr", 949},        {"koi8-r", 20866},      {"windows-874", 874},
      {"windows-1250", 1250}, {"windows-1251", 1251}, {"windows-1252", 1252},
      {"windows-1253", 1253}, {"windows-1254", 1254}, {"windows-1255", 1255},
      {"windows-1256", 1256}, {"windows-1257", 1257}, {"windows-1258", 1258},
      {"iso-8859-1", 28591},  {"iso-8859-2", 28592},  {"iso-8859-5", 28595},
      {"iso-8859-7", 28597},  {"iso-8859-9", 28599},  {"iso-8859-15", 28605},
      {"utf-8", CP_UTF8},
  };
  for (const auto& entry : kCodePages) {
    if (name == entry.name) {
      return entry.code_page;
    }
  }
  return 0;
}

}  // namespace

bool ConvertToUtf8(std::string_view data,
                   const std::string& encoding,
                   std::string* text) {
  const UINT code_page = CodePage(encoding);
  if (code_page == 0 || !IsValidCodePage(code_page)) {
    return false;
  }
  text->clear();
  if (data.empty()) {
    return true;
  }
  if (data.size() > INT_MAX) {
    return false;
  }
  const int size = static_cast<int>(data.size());
  // Without MB_ERR_INVALID_CHARS, bytes the code page cannot read become
  // U+FFFD.
  const int units =
      MultiByteToWideChar(code_page, 0, data.data(), size, nullptr, 0);
  if (units <= 0) {
    return false;
  }
  std::wstring wide(static_cast<size_t>(units), L'\0');
  MultiByteToWideChar(code_page, 0, data.data(), size, wide.data(), units);
  const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide.data(), units, nullptr,
                                        0, nullptr, nullptr);
  if (bytes <= 0) {
    return false;
  }
  text->resize(static_cast<size_t>(bytes));
  WideCharToMultiByte(CP_UTF8, 0, wide.data(), units, text->data(), bytes,
                      nullptr, nullptr);
  return true;
}

}  // namespace lychee
