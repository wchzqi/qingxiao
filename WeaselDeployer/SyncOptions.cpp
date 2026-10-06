// SyncOptions.cpp — 青简 P1-B6 无账号同步：installation.yaml 读写（实现）
#include "stdafx.h"

#include "SyncOptions.h"

#include <WeaselUtility.h>  // WeaselUserDataPath / wtou8 / u8tow

#include <objbase.h>    // CoCreateGuid
#include <fstream>
#include <sstream>
#include <vector>

namespace weasel {

namespace {

// trim 两端空白（含 \r / \t / 空格）
std::string Trim(const std::string& s) {
  size_t b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos)
    return "";
  size_t e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

// 去掉 YAML 标量两端可能的引号（' 或 "）
std::string Unquote(const std::string& v) {
  std::string t = Trim(v);
  if (t.size() >= 2 &&
      ((t.front() == '"' && t.back() == '"') ||
       (t.front() == '\'' && t.back() == '\''))) {
    t = t.substr(1, t.size() - 2);
  }
  return t;
}

// 判断一行是否为 `key:` 键行（忽略前导空白；# 注释行不算）
// 返回 true 时把该行 key 部分写入 out_key。
bool MatchKeyLine(const std::string& line, const char* key, std::string* out_key) {
  std::string s = Trim(line);
  if (s.empty() || s.front() == '#')
    return false;
  size_t colon = s.find(':');
  if (colon == std::string::npos)
    return false;
  std::string k = Trim(s.substr(0, colon));
  if (k != key)
    return false;
  if (out_key)
    *out_key = k;
  return true;
}

// 从一行里取 `key: value` 的 value 部分
std::string ExtractValue(const std::string& line) {
  size_t colon = line.find(':');
  if (colon == std::string::npos)
    return "";
  return Unquote(line.substr(colon + 1));
}

}  // namespace

SyncOptions::SyncOptions() {
  // installation.yaml 固定位于用户数据目录（%AppData%\Rime\installation.yaml）
  file_path_ = WeaselUserDataPath() / L"installation.yaml";
}

SyncOptions::~SyncOptions() = default;

bool SyncOptions::Load() {
  installation_id_.clear();
  sync_dir_.clear();

  std::ifstream in(file_path_, std::ios::binary);
  if (!in) {
    // 文件不存在 = 首次使用，按空配置处理（不报错、不建文件）。
    return true;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  std::string content = ss.str();

  std::istringstream line_reader(content);
  std::string line;
  bool first_line = true;
  while (std::getline(line_reader, line)) {
    // 去掉首行可能的 UTF-8 BOM（EF BB BF），避免首键名被污染
    if (first_line && line.size() >= 3 &&
        (unsigned char)line[0] == 0xEF && (unsigned char)line[1] == 0xBB &&
        (unsigned char)line[2] == 0xBF) {
      line.erase(0, 3);
    }
    first_line = false;
    if (MatchKeyLine(line, "installation_id", nullptr)) {
      installation_id_ = ExtractValue(line);
    } else if (MatchKeyLine(line, "sync_dir", nullptr)) {
      sync_dir_ = ExtractValue(line);
    }
  }
  return true;
}

std::string SyncOptions::EnsureInstallationId() {
  if (!installation_id_.empty())
    return installation_id_;

  // CoCreateGuid 在 ole32（工程已链接），无需引入 rpcrt4。
  GUID g = {0};
  if (CoCreateGuid(&g) != S_OK) {
    return installation_id_;  // 失败：保持空，下次再试
  }
  char buf[64] = {0};
  snprintf(buf, sizeof(buf),
           "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
           (unsigned long)g.Data1, (unsigned)g.Data2, (unsigned)g.Data3,
           (unsigned)g.Data4[0], (unsigned)g.Data4[1], (unsigned)g.Data4[2],
           (unsigned)g.Data4[3], (unsigned)g.Data4[4], (unsigned)g.Data4[5],
           (unsigned)g.Data4[6], (unsigned)g.Data4[7]);
  installation_id_ = buf;
  Flush();
  return installation_id_;
}

bool SyncOptions::SetSyncDir(const std::string& utf8_dir) {
  sync_dir_ = utf8_dir;
  return Flush();
}

bool SyncOptions::Flush() {
  // 1) 读现有全部行（保留顺序与原样）
  std::vector<std::string> lines;
  {
    std::ifstream in(file_path_, std::ios::binary);
    if (in) {
      std::ostringstream ss;
      ss << in.rdbuf();
      std::istringstream lr(ss.str());
      std::string line;
      while (std::getline(lr, line)) {
        lines.push_back(line);
      }
    }
  }

  // 2) 逐行替换命中的键；未命中则记录待追加
  bool have_id = false, have_dir = false;
  for (auto& line : lines) {
    if (!have_id && MatchKeyLine(line, "installation_id", nullptr)) {
      line = "installation_id: " + installation_id_;
      have_id = true;
    } else if (!have_dir && MatchKeyLine(line, "sync_dir", nullptr)) {
      line = "sync_dir: " + sync_dir_;
      have_dir = true;
    }
  }
  if (!have_id && !installation_id_.empty())
    lines.push_back("installation_id: " + installation_id_);
  if (!have_dir && !sync_dir_.empty())
    lines.push_back("sync_dir: " + sync_dir_);

  // 3) 整文件重写（UTF-8 无 BOM，LF 行尾；YAML 兼容）
  std::ofstream out(file_path_, std::ios::binary | std::ios::trunc);
  if (!out)
    return false;
  for (size_t i = 0; i < lines.size(); ++i) {
    out << lines[i] << "\n";
  }
  out.flush();
  return out.good();
}

}  // namespace weasel
