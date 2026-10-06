// ============================================================================
// 青简（Qingjian）P1-B2：EmojiProvider 实现
// 详见 EmojiProvider.h 头部说明。本文件不依赖 rime_api，仅用 C++17 标准库 +
// WeaselUtility.h 提供的共享数据目录路径助手（WeaselSharedDataPath）。
// ============================================================================
#ifndef QINGXIAO_LINUX_SELFTEST
#include "stdafx.h"
#endif

#include "EmojiProvider.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

#include <WeaselUtility.h>  // WeaselSharedDataPath()（既有接口，include/WeaselUtility.h:35）

namespace weasel {
namespace {

// 去除行尾 '\r'（兼容 CRLF）与首尾空白。
std::string Trim(const std::string& s) {
  size_t b = 0;
  size_t e = s.size();
  while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n'))
    ++b;
  while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' ||
                   s[e - 1] == '\n'))
    --e;
  return s.substr(b, e - b);
}

// 按 TAB 切分一行（保留字段内的空格，因为关键字之间用空格分隔）。
std::vector<std::string> SplitTab(const std::string& line) {
  std::vector<std::string> cols;
  std::string cur;
  for (char ch : line) {
    if (ch == '\t') {
      cols.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(ch);
    }
  }
  cols.push_back(cur);
  return cols;
}

// ASCII 小写化（仅用于关键字比较；UTF-8 多字节序列的高位字节不受影响，
// 因为 std::tolower 对 >=0x80 的 char 此处按处理，中文/字节原样保留，
// 子串匹配在字节层面仍正确）。
std::string AsciiLower(std::string s) {
  for (char& ch : s) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  return s;
}

// 大小写不敏感的子串包含（字节层面）。
bool ContainsCi(const std::string& haystack_lower, const std::string& needle_lower) {
  if (needle_lower.empty())
    return false;
  return haystack_lower.find(needle_lower) != std::string::npos;
}

}  // namespace

EmojiProvider::EmojiProvider() = default;
EmojiProvider::~EmojiProvider() = default;

std::filesystem::path EmojiProvider::DefaultDataPath() {
  // 与 _Setup() 中 weasel_traits.shared_data_dir 同源（WeaselUtility.h:35）。
  return WeaselSharedDataPath() / "emoji.txt";
}

bool EmojiProvider::Load(const std::filesystem::path& data_path) {
  // 失败一律降级：清空后返回 false，绝不抛异常。
  entries_.clear();
  loaded_ = false;

  std::ifstream ifs(data_path, std::ios::binary);
  if (!ifs.is_open()) {
    // 数据文件尚未部署：安全降级（P1-B2 硬约束）。
    return false;
  }

  std::ostringstream ss;
  ss << ifs.rdbuf();
  const std::string content = ss.str();
  if (content.empty()) {
    return false;
  }

  std::vector<EmojiEntry> parsed;
  std::istringstream line_stream(content);
  std::string line;
  while (std::getline(line_stream, line)) {
    // 统一去掉 CRLF 的 '\r'。
    if (!line.empty() && line.back() == '\r')
      line.pop_back();

    const std::string trimmed = Trim(line);
    if (trimmed.empty())
      continue;
    if (trimmed.front() == '#')  // 注释行（格式头注释）
      continue;

    std::vector<std::string> cols = SplitTab(trimmed);
    // 期望至少 3 列：分类 / emoji文本 / 名称关键字。
    // 列数不足则跳过该行，不影响其它条目。
    if (cols.size() < 3)
      continue;

    EmojiEntry entry;
    entry.category = Trim(cols[0]);
    entry.text = Trim(cols[1]);
    // 第 3 列起皆视为关键字（兼容将来扩展列，但 P1 只用前 3 列）。
    std::string keywords;
    for (size_t i = 2; i < cols.size(); ++i) {
      if (i > 2)
        keywords.push_back(' ');
      keywords += Trim(cols[i]);
    }
    entry.keywords = keywords;

    if (entry.category.empty() || entry.text.empty())
      continue;  // 分类或上屏文本为空的行无意义。

    parsed.push_back(std::move(entry));
  }

  if (parsed.empty()) {
    // 文件存在但无有效行：仍视为降级。
    return false;
  }

  entries_ = std::move(parsed);
  loaded_ = true;
  return true;
}

std::vector<std::string> EmojiProvider::Categories() const {
  std::vector<std::string> cats;
  for (const auto& e : entries_) {
    if (std::find(cats.begin(), cats.end(), e.category) == cats.end()) {
      cats.push_back(e.category);
    }
  }
  return cats;
}

std::vector<EmojiEntry> EmojiProvider::Search(const std::string& keyword_utf8) const {
  const std::string needle = AsciiLower(keyword_utf8);
  if (needle.empty())
    return {};

  std::vector<EmojiEntry> result;
  for (const auto& e : entries_) {
    const bool hit =
        ContainsCi(AsciiLower(e.keywords), needle) ||
        ContainsCi(AsciiLower(e.category), needle) ||
        ContainsCi(AsciiLower(e.text), needle);
    if (hit)
      result.push_back(e);
  }
  return result;
}

size_t EmojiProvider::CategorySize(const std::string& category_utf8) const {
  size_t n = 0;
  for (const auto& e : entries_) {
    if (e.category == category_utf8)
      ++n;
  }
  return n;
}

std::vector<EmojiEntry> EmojiProvider::Page(const std::string& category_utf8,
                                            size_t page,
                                            size_t page_size) const {
  if (page_size == 0)
    page_size = 10;

  std::vector<EmojiEntry> result;
  size_t index = 0;
  const size_t begin = page * page_size;
  const size_t end = begin + page_size;
  for (const auto& e : entries_) {
    if (e.category != category_utf8)
      continue;
    if (index >= begin && index < end)
      result.push_back(e);
    ++index;
    if (index >= end)
      break;
  }
  return result;
}

}  // namespace weasel
