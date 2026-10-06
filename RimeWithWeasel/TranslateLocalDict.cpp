// ============================================================================
// 青筱（QingXiaoType）P2-E：翻译本地兜底词典实现
// （对应头 TranslateLocalDict.h；纯标准库，零平台依赖，Linux g++ 可直接编译）
// ============================================================================

#include "stdafx.h"

#include "TranslateLocalDict.h"

#include <cctype>
#include <mutex>
#include <string>
#include <unordered_map>

namespace weasel {
namespace translate_local {
namespace {

// ----------------------------------------------------------------------------
// 内置对照（简化实现，刻意保持小而常见）。
// 方向 zh -> en。覆盖：问候 / 致谢 / 道歉 / 常见单字。
// ----------------------------------------------------------------------------
const std::pair<const char*, const char*> kZhEnTable[] = {
    {"你好", "hello"},
    {"您好", "hello"},
    {"早上好", "good morning"},
    {"下午好", "good afternoon"},
    {"晚上好", "good evening"},
    {"再见", "goodbye"},
    {"谢谢", "thank you"},
    {"感谢", "thanks"},
    {"不客气", "you're welcome"},
    {"对不起", "sorry"},
    {"没关系", "it's okay"},
    {"请", "please"},
    {"是", "yes"},
    {"不是", "no"},
    {"好", "good"},
    {"不好", "bad"},
    {"我爱你", "I love you"},
    {"早安", "good morning"},
    {"晚安", "good night"},
    {"请问", "excuse me"},
    {"麻烦", "trouble"},
    {"现在几点", "what time is it"},
    {"多少钱", "how much"},
    {"水", "water"},
    {"茶", "tea"},
    {"谢谢大家", "thank you everyone"},
};
const size_t kZhEnCount = sizeof(kZhEnTable) / sizeof(kZhEnTable[0]);

// en -> zh（英文短语/单词 -> 中文）。与上表部分互为翻译，此处单列常用词。
const std::pair<const char*, const char*> kEnZhTable[] = {
    {"hello", "你好"},
    {"hi", "你好"},
    {"good morning", "早上好"},
    {"good afternoon", "下午好"},
    {"good evening", "晚上好"},
    {"goodbye", "再见"},
    {"bye", "再见"},
    {"thank you", "谢谢"},
    {"thanks", "谢谢"},
    {"sorry", "对不起"},
    {"please", "请"},
    {"yes", "是"},
    {"no", "不是"},
    {"good", "好"},
    {"water", "水"},
    {"tea", "茶"},
    {"good night", "晚安"},
    {"how much", "多少钱"},
};
const size_t kEnZhCount = sizeof(kEnZhTable) / sizeof(kEnZhTable[0]);

// 判定文本是否「主要是 ASCII」（英文字母/数字/空格/标点为主）。
// 简化实现：只要首字节不是 UTF-8 多字节（即 < 0x80 的 ASCII），就视为英文输入。
bool LooksLikeAscii(const std::string& s) {
  for (char c : s) {
    if (static_cast<unsigned char>(c) >= 0x80)
      return false;  // 含多字节 UTF-8（中文等）
  }
  return true;
}

// 去首尾空白（ASCII 空白）。
std::string Trim(const std::string& s) {
  size_t b = 0, e = s.size();
  while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
  return s.substr(b, e - b);
}

// 惰性构造两张 hash 表（首次查询时构建一次，线程安全）。
const std::unordered_map<std::string, std::string>& ZhEnMap() {
  static std::unordered_map<std::string, std::string> m;
  static std::once_flag once;
  std::call_once(once, [] {
    for (size_t i = 0; i < kZhEnCount; ++i) {
      m[kZhEnTable[i].first] = kZhEnTable[i].second;
    }
  });
  return m;
}
const std::unordered_map<std::string, std::string>& EnZhMap() {
  static std::unordered_map<std::string, std::string> m;
  static std::once_flag once;
  std::call_once(once, [] {
    for (size_t i = 0; i < kEnZhCount; ++i) {
      m[kEnZhTable[i].first] = kEnZhTable[i].second;
    }
  });
  return m;
}

}  // namespace

std::string Lookup(const std::string& text_utf8,
                   const std::string& target_lang) {
  if (text_utf8.empty())
    return std::string();

  const std::string key = Trim(text_utf8);
  if (key.empty())
    return std::string();

  // 目标语言归一（小写）。
  std::string lang;
  lang.reserve(target_lang.size());
  for (char c : target_lang) lang.push_back((char)std::tolower((unsigned char)c));

  const bool input_ascii = LooksLikeAscii(key);

  std::unordered_map<std::string, std::string>::const_iterator it;

  if (lang == "en") {
    // 目标英文：期望输入是中文 -> 查 zh->en。
    if (!input_ascii) {
      it = ZhEnMap().find(key);
      if (it != ZhEnMap().end()) return it->second;
    }
    return std::string();  // 未命中 / 方向不符
  }
  if (lang == "zh" || lang == "zh-cn" || lang == "zh_cn") {
    // 目标中文：期望输入是英文 -> 查 en->zh。
    if (input_ascii) {
      it = EnZhMap().find(key);
      if (it != EnZhMap().end()) return it->second;
    }
    return std::string();
  }

  // 其它目标语言：本地兜底不支持，返回空（交云端/降级）。
  return std::string();
}

size_t EntryCount() {
  return kZhEnCount + kEnZhCount;
}

}  // namespace translate_local
}  // namespace weasel
