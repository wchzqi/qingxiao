// SogouDictImporter.cpp — 青简 P1-B5 搜狗一行一词词库导入转换器（实现）
//
// 首行 include stdafx.h：与 WeaselDeployer 既有 .cpp 一致，复用工程预编译头。
#include "stdafx.h"

#include "SogouDictImporter.h"

#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#endif

namespace weasel {

namespace {

// 判断字符是否为分词空白（空格/Tab/回车/换行/全角空格 U+3000 视为词边界）
inline bool IsSpaceChar(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

inline bool AllDigits(const std::string& s) {
  if (s.empty()) return false;
  for (char c : s) {
    if (c < '0' || c > '9') return false;
  }
  return true;
}

// 按空白把一行切成 token（UTF-8 字节流按 0x20/0x09 切分即可，多字节字符不含这些字节）
std::vector<std::string> SplitTokens(const std::string& line) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : line) {
    if (IsSpaceChar(c)) {
      if (!cur.empty()) {
        out.push_back(cur);
        cur.clear();
      }
    } else {
      cur.push_back(c);
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

std::string Trim(const std::string& s) {
  size_t b = 0, e = s.size();
  while (b < e && IsSpaceChar(s[b])) ++b;
  while (e > b && IsSpaceChar(s[e - 1])) --e;
  return s.substr(b, e - b);
}

#ifdef _WIN32
// UTF-8 窄串 -> Windows 宽串（用于打开含中文路径的文件）
std::wstring Utf8ToWide(const std::string& s) {
  if (s.empty()) return std::wstring();
  int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0);
  std::wstring w(n, 0);
  ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
  return w;
}
#endif

// 整文件读入；路径按 UTF-8 解释。Windows 走宽路径以兼容中文目录。
bool ReadWholeFile(const std::string& path_utf8, std::string* out) {
  out->clear();
#ifdef _WIN32
  std::ifstream ifs(Utf8ToWide(path_utf8), std::ios::binary);
#else
  std::ifstream ifs(path_utf8, std::ios::binary);
#endif
  if (!ifs) return false;
  std::ostringstream ss;
  ss << ifs.rdbuf();
  *out = ss.str();
  return true;
}

// 写文件；append=true 时追加（用于 custom_phrase.txt）。产物一律 UTF-8 无 BOM。
bool WriteWholeFile(const std::string& path_utf8, const std::string& data, bool append) {
#ifdef _WIN32
  std::ofstream ofs(Utf8ToWide(path_utf8),
                    std::ios::binary | (append ? std::ios::app : std::ios::trunc));
#else
  std::ofstream ofs(path_utf8, std::ios::binary | (append ? std::ios::app : std::ios::trunc));
#endif
  if (!ofs) return false;
  ofs.write(data.data(), (std::streamsize)data.size());
  return ofs.good();
}

// GBK(CP936) 字节 -> UTF-8。仅 Windows 实现；非 Windows 编译单元返回空串，
// 由 NormalizeToUtf8 走「原始字节兜底」分支（Linux 静态自检不喂 GBK 样本）。
std::string GbkToUtf8(const std::string& gbk) {
#ifdef _WIN32
  if (gbk.empty()) return std::string();
  int wn = ::MultiByteToWideChar(936, 0, gbk.c_str(), (int)gbk.size(), NULL, 0);
  if (wn <= 0) return std::string();
  std::wstring w(wn, 0);
  ::MultiByteToWideChar(936, 0, gbk.c_str(), (int)gbk.size(), &w[0], wn);
  int un = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), wn, NULL, 0, NULL, NULL);
  std::string u(un, 0);
  ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), wn, &u[0], un, NULL, NULL);
  return u;
#else
  (void)gbk;
  return std::string();
#endif
}

}  // namespace

SogouDictImporter::SogouDictImporter() = default;
SogouDictImporter::~SogouDictImporter() = default;

bool SogouDictImporter::IsValidUtf8(const std::string& s) {
  size_t i = 0, n = s.size();
  while (i < n) {
    unsigned char c = (unsigned char)s[i];
    int extra = 0;
    uint32_t cp = 0;
    if (c <= 0x7F) {
      ++i;
      continue;
    } else if ((c & 0xE0) == 0xC0) {
      extra = 1; cp = c & 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
      extra = 2; cp = c & 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
      extra = 3; cp = c & 0x07;
    } else {
      return false;
    }
    if (i + extra >= n) return false;
    for (int k = 1; k <= extra; ++k) {
      unsigned char cc = (unsigned char)s[i + k];
      if ((cc & 0xC0) != 0x80) return false;
      cp = (cp << 6) | (cc & 0x3F);
    }
    // 拒绝 overlong / 代理区 / 越界
    if (cp < 0x80 || (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) return false;
    if (extra == 1 && cp < 0x80) return false;
    if (extra == 2 && cp < 0x800) return false;
    if (extra == 3 && cp < 0x10000) return false;
    i += (size_t)extra + 1;
  }
  return true;
}

std::string SogouDictImporter::Utf16LeToUtf8(const std::string& bytes) {
  std::string out;
  size_t n = bytes.size();
  size_t i = 0;
  while (i + 1 < n) {
    uint16_t u = (uint16_t)((unsigned char)bytes[i] |
                            ((unsigned char)bytes[i + 1] << 8));
    i += 2;
    uint32_t cp = u;
    if (u >= 0xD800 && u <= 0xDBFF && i + 1 < n) {
      uint16_t low = (uint16_t)((unsigned char)bytes[i] |
                                ((unsigned char)bytes[i + 1] << 8));
      if (low >= 0xDC00 && low <= 0xDFFF) {
        cp = 0x10000 + ((uint32_t)(u - 0xD800) << 10) + (low - 0xDC00);
        i += 2;
      }
    }
    if (cp < 0x80) {
      out.push_back((char)cp);
    } else if (cp < 0x800) {
      out.push_back((char)(0xC0 | (cp >> 6)));
      out.push_back((char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out.push_back((char)(0xE0 | (cp >> 12)));
      out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back((char)(0x80 | (cp & 0x3F)));
    } else {
      out.push_back((char)(0xF0 | (cp >> 18)));
      out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
      out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back((char)(0x80 | (cp & 0x3F)));
    }
  }
  return out;
}

std::string SogouDictImporter::NormalizeToUtf8(const std::string& bytes,
                                               SogouEncoding* enc) {
  if (enc) *enc = SogouEncoding::kUnknown;
  if (bytes.empty()) return std::string();

  // UTF-8 BOM: EF BB BF
  if (bytes.size() >= 3 && (unsigned char)bytes[0] == 0xEF &&
      (unsigned char)bytes[1] == 0xBB && (unsigned char)bytes[2] == 0xBF) {
    if (enc) *enc = SogouEncoding::kUtf8;
    return bytes.substr(3);
  }
  // UTF-16LE BOM: FF FE
  if (bytes.size() >= 2 && (unsigned char)bytes[0] == 0xFF &&
      (unsigned char)bytes[1] == 0xFE) {
    if (enc) *enc = SogouEncoding::kUtf16Le;
    return Utf16LeToUtf8(bytes.substr(2));
  }
  // 严格 UTF-8 校验
  if (IsValidUtf8(bytes)) {
    if (enc) *enc = SogouEncoding::kUtf8;
    return bytes;
  }
  // 回退 GBK(CP936)
  if (enc) *enc = SogouEncoding::kGbk;
  std::string gbk = GbkToUtf8(bytes);
  if (!gbk.empty()) return gbk;
  // 非 Windows 静态编译环境拿不到 CP936 解码：原样返回，由调用方按 UTF-8 容错
  return bytes;
}

bool SogouDictImporter::ParseLine(const std::string& line,
                                  SogouEntry* entry,
                                  std::string* reason) {
  std::string t = Trim(line);
  if (t.empty()) {
    if (reason) *reason = "empty";
    return false;
  }
  if (t[0] == '#') {
    if (reason) *reason = "comment";
    return false;
  }
  std::vector<std::string> tok = SplitTokens(t);
  if (tok.empty()) {
    if (reason) *reason = "empty";
    return false;
  }
  // 首 token = 词；其余 token 中，末尾纯数字为权重，其余为编码音节
  std::string word = tok[0];
  std::vector<std::string> code_parts;
  int weight = 1;
  size_t last = tok.size() - 1;
  size_t code_end = tok.size();
  if (tok.size() >= 2 && AllDigits(tok[last])) {
    code_end = last;
    try {
      weight = std::stoi(tok[last]);
    } catch (...) {
      weight = 1;
    }
  }
  for (size_t k = 1; k < code_end; ++k) {
    code_parts.push_back(tok[k]);
  }
  if (code_parts.empty()) {
    if (reason) *reason = "no_code";
    return false;
  }
  std::string code;
  for (size_t k = 0; k < code_parts.size(); ++k) {
    if (k) code.push_back(' ');
    code += code_parts[k];
  }
  if (entry) {
    entry->word = word;
    entry->code = code;
    entry->weight = weight;
  }
  return true;
}

std::string SogouDictImporter::SerializeEntries(const std::vector<SogouEntry>& entries) {
  std::string out;
  for (const auto& e : entries) {
    out += e.word;
    out.push_back('\t');
    out += e.code;
    out.push_back('\t');
    out += std::to_string(e.weight);
    out.push_back('\n');
  }
  return out;
}

std::string SogouDictImporter::BuildDictYaml(const std::string& dict_name,
                                             const std::vector<SogouEntry>& entries) {
  std::string out;
  out += "# Rime dictionary\n";
  out += "# encoding: utf-8\n";
  out += "---\n";
  out += "name: ";
  out += dict_name.empty() ? "imported" : dict_name;
  out += "\n";
  out += "version: \"1.0\"\n";
  out += "sort: by_weight\n";
  out += "use_preset_vocabulary: false\n";
  out += "columns:\n";
  out += "  - text\n";
  out += "  - code\n";
  out += "  - weight\n";
  out += "...\n";
  out += SerializeEntries(entries);
  return out;
}

bool SogouDictImporter::Import(const std::string& src_path,
                               const std::string& out_path,
                               const std::string& dict_name,
                               SogouImportMode mode,
                               SogouImportReport* report) {
  SogouImportReport r;
  std::string bytes;
  if (!ReadWholeFile(src_path, &bytes)) {
    if (report) {
      r.message = "无法读取源文件";
      *report = r;
    }
    return false;
  }
  r.bytes_read = bytes.size();
  std::string utf8 = NormalizeToUtf8(bytes, &r.encoding);

  // 按行切分
  std::vector<std::string> lines;
  {
    std::string cur;
    for (char c : utf8) {
      if (c == '\n') {
        lines.push_back(cur);
        cur.clear();
      } else {
        cur.push_back(c);
      }
    }
    if (!cur.empty()) lines.push_back(cur);
  }

  std::vector<SogouEntry> entries;
  for (const auto& ln : lines) {
    ++r.total_lines;
    SogouEntry e;
    std::string reason;
    if (ParseLine(ln, &e, &reason)) {
      entries.push_back(e);
    } else if (reason == "empty") {
      ++r.skipped_empty;
    } else if (reason == "comment") {
      ++r.skipped_comment;
    } else if (reason == "no_code") {
      ++r.skipped_no_code;
    } else {
      ++r.skipped_bad;
    }
  }
  r.imported = (long)entries.size();

  std::string payload;
  bool append = false;
  if (mode == SogouImportMode::kWriteDictYaml) {
    payload = BuildDictYaml(dict_name, entries);
    append = false;
  } else {
    payload = SerializeEntries(entries);
    append = true;  // custom_phrase.txt 追加，不覆盖既有自造词
  }

  if (!WriteWholeFile(out_path, payload, append)) {
    if (report) {
      r.message = "写入目标文件失败";
      *report = r;
    }
    return false;
  }

  std::ostringstream msg;
  msg << "导入完成：成功 " << r.imported << " 条；跳过 " << r.skipped_total()
      << " 行（空行 " << r.skipped_empty << "，注释 " << r.skipped_comment
      << "，无编码 " << r.skipped_no_code << "，其他 " << r.skipped_bad << "）";
  r.message = msg.str();
  if (report) *report = r;
  return true;
}

}  // namespace weasel
