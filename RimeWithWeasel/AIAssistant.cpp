// ============================================================================
// 青筱 P2-D：AIAssistant 实现
//   - 路线 B 本地模板引擎（润色/续写/翻译/纠错）：纯 std::wstring 规则，零上传。
//   - 路线 A 云端 OpenAI 兼容：请求体拼装 + 响应抽取（真实 WinHTTP 为已知缺口）。
//   - 门面 HandleRequest：路由分发 + Gate 检查 + 失败降级 + ExtraCard(AI) 组装。
//
// Linux 自测：定义 AIA_SelfTest 时，本文件不 include NetworkGate.h / Windows 头，
// 仅编译纯逻辑部分（见 test_ai_assistant.cpp / run_tests.sh）。
// ============================================================================

#include "AIAssistant.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>

// 闸门经 IAiGateChecker 窄接口注入（生产适配器在 RimeWithWeasel 侧包 NetworkGate），
// 本核心不直接 include NetworkGate.h，故 Linux g++ 可独立编译。

namespace weasel {

// --------------------------------------------------------------------------
// 动作类型字符串表
// --------------------------------------------------------------------------
AIActionType AIActionFromString(const std::wstring& s) {
  if (s == L"polish") return AIActionType::kPolish;
  if (s == L"continue") return AIActionType::kContinue;
  if (s == L"translate") return AIActionType::kTranslate;
  if (s == L"proofread") return AIActionType::kProofread;
  return AIActionType::kUnknown;
}

std::wstring AIActionToString(AIActionType a) {
  switch (a) {
    case AIActionType::kPolish: return L"polish";
    case AIActionType::kContinue: return L"continue";
    case AIActionType::kTranslate: return L"translate";
    case AIActionType::kProofread: return L"proofread";
    default: return L"unknown";
  }
}

std::wstring AIActionZhName(AIActionType a) {
  switch (a) {
    case AIActionType::kPolish: return L"AI 润色";
    case AIActionType::kContinue: return L"AI 续写";
    case AIActionType::kTranslate: return L"AI 翻译";
    case AIActionType::kProofread: return L"AI 纠错";
    default: return L"AI 助手";
  }
}

// ===========================================================================
// 通用小工具（纯 wstring）
// ===========================================================================
namespace {

inline bool IsSpace(wchar_t c) {
  return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n';
}
inline bool IsCjkIdeograph(wchar_t c) {
  return c >= 0x4E00 && c <= 0x9FFF;
}
inline bool IsCjkPunct(wchar_t c) {
  // CJK 符号标点 + 全角形式区段
  return (c >= 0x3000 && c <= 0x303F) || (c >= 0xFF00 && c <= 0xFFEF);
}
inline bool IsCjk(wchar_t c) { return IsCjkIdeograph(c) || IsCjkPunct(c); }

std::wstring Trim(const std::wstring& s) {
  size_t b = 0, e = s.size();
  while (b < e && IsSpace(s[b])) ++b;
  while (e > b && IsSpace(s[e - 1])) --e;
  return s.substr(b, e - b);
}

// 折叠连续 ASCII 空格为单个
std::wstring CollapseSpaces(const std::wstring& s) {
  std::wstring out;
  bool prev_space = false;
  for (wchar_t c : s) {
    if (c == L' ') {
      if (!prev_space) out.push_back(c);
      prev_space = true;
    } else {
      out.push_back(c);
      prev_space = false;
    }
  }
  return out;
}

// 连续同类终止标点折叠为单个（。。。→。 / ！！→！ / ？？→？ / ，，→，）
std::wstring CollapseRepeatedPunct(const std::wstring& s) {
  const std::set<wchar_t> kTerminal = {L'。', L'！', L'？', L'，', L'.', L'!', L'?'};
  std::wstring out;
  for (wchar_t c : s) {
    if (!out.empty() && kTerminal.count(c) && out.back() == c) continue;
    out.push_back(c);
  }
  return out;
}

// "..." / "。。。" 省略号归一为 "……"
std::wstring NormalizeEllipsis(const std::wstring& s) {
  std::wstring out;
  size_t i = 0;
  while (i < s.size()) {
    if ((s[i] == L'.' && i + 2 < s.size() && s[i+1] == L'.' && s[i+2] == L'.')) {
      out += L"……";
      i += 3;
    } else {
      out.push_back(s[i]);
      ++i;
    }
  }
  return out;
}

}  // namespace

// ===========================================================================
// 路线 B：本地模板 Provider
// ===========================================================================

// ---- 润色：标点/空格规范化（确定性，不改措辞语义） ----
std::wstring LocalTemplateProvider::Polish(const std::wstring& text) {
  std::wstring s = Trim(text);
  s = NormalizeEllipsis(s);
  s = CollapseRepeatedPunct(s);
  s = CollapseSpaces(s);
  // 夹在两个 CJK 字符之间的半角逗号/句号，转正全角（轻量规范化）
  std::wstring out;
  for (size_t i = 0; i < s.size(); ++i) {
    wchar_t c = s[i];
    wchar_t prev = (i > 0) ? s[i - 1] : 0;
    wchar_t next = (i + 1 < s.size()) ? s[i + 1] : 0;
    if ((c == L',' || c == L'.') && IsCjkIdeograph(prev) && IsCjkIdeograph(next)) {
      out.push_back(c == L',' ? L'，' : L'。');
    } else {
      out.push_back(c);
    }
  }
  return out;
}

// ---- 续写：基于结尾给 3 条补全候选 ----
std::vector<std::wstring> LocalTemplateProvider::Continue(
    const std::wstring& text) {
  std::wstring base = Trim(text);
  std::vector<std::wstring> out;
  wchar_t last = base.empty() ? 0 : base.back();

  std::vector<std::wstring> tails;
  if (last == L'，' || last == L',' || last == L'、') {
    tails = {L"让每一天都值得期待", L"我们一起把它做好", L"未来可期"};
  } else if (last == L'你' || last == L'妳') {
    tails = {L"好", L"最近怎么样", L"辛苦了"};
  } else if (last == L'？' || last == L'?') {
    tails = {L"是的", L"不一定", L"再想想看"};
  } else if (last == L'！' || last == L'!') {
    tails = {L"加油", L"一起努力", L"冲冲冲"};
  } else {
    tails = {L"。", L"，继续", L"！"};
  }
  for (const auto& t : tails) out.push_back(base + t);
  return out;
}

// ---- 翻译：内置小词典（本地模板版；真正 MT 走云端路线） ----
std::wstring LocalTemplateProvider::Translate(const std::wstring& text) {
  static const std::map<std::wstring, std::wstring> kZh2En = {
      {L"你好", L"Hello"},        {L"谢谢", L"Thank you"},
      {L"再见", L"Goodbye"},      {L"我爱你", L"I love you"},
      {L"早上好", L"Good morning"}, {L"晚上好", L"Good evening"},
      {L"请", L"Please"},         {L"对不起", L"Sorry"},
  };
  static const std::map<std::wstring, std::wstring> kEn2Zh = {
      {L"hello", L"你好"},   {L"thanks", L"谢谢"},     {L"thank you", L"谢谢你"},
      {L"goodbye", L"再见"}, {L"good morning", L"早上好"}, {L"sorry", L"对不起"},
  };
  std::wstring s = Trim(text);
  // 中→英：整词命中
  auto it = kZh2En.find(s);
  if (it != kZh2En.end()) return it->second;
  // 英→中：小写后命中
  std::wstring low;
  for (wchar_t c : s) low.push_back((wchar_t)std::tolower(c));
  auto it2 = kEn2Zh.find(low);
  if (it2 != kEn2Zh.end()) return it2->second;
  // 未命中：原样返回，由上层 note 提示
  return s;
}

// ---- 纠错：叠字/夹空格/重复标点修正 ----
std::pair<std::wstring, std::wstring> LocalTemplateProvider::Proofread(
    const std::wstring& text) {
  std::wstring s = Trim(text);
  std::vector<std::wstring> fixes;

  // 1) 叠字删除：连续两个相同 CJK 字，删一个（的的/了了/是是/我我...）
  std::wstring out;
  for (size_t i = 0; i < s.size(); ++i) {
    if (!out.empty() && out.back() == s[i] && IsCjkIdeograph(s[i])) {
      fixes.push_back(L"叠字「" + std::wstring(1, s[i]) + L"」已去重");
      continue;  // 跳过重复字
    }
    out.push_back(s[i]);
  }
  s = out;

  // 2) 两个 CJK 字符之间夹的空格删除
  out.clear();
  for (size_t i = 0; i < s.size(); ++i) {
    wchar_t c = s[i];
    if (c == L' ' && i > 0 && i + 1 < s.size() &&
        IsCjkIdeograph(s[i - 1]) && IsCjkIdeograph(s[i + 1])) {
      fixes.push_back(L"汉字间多余空格已删除");
      continue;
    }
    out.push_back(c);
  }
  s = out;

  // 3) 重复标点折叠
  std::wstring before = s;
  s = CollapseRepeatedPunct(s);
  if (s != before) fixes.push_back(L"重复标点已合并");

  std::wstring note;
  for (size_t i = 0; i < fixes.size(); ++i) {
    if (i) note += L"；";
    note += fixes[i];
  }
  if (note.empty()) note = L"未发现可修正项（已检查叠字/夹空格/重复标点）";
  return {s, note};
}

AIResult LocalTemplateProvider::Run(const AIRequest& req) {
  AIResult r;
  r.title = AIActionZhName(req.action);
  switch (req.action) {
    case AIActionType::kPolish:
      r.primary = Polish(req.text);
      r.ok = !r.primary.empty();
      break;
    case AIActionType::kContinue:
      r.suggestions = Continue(req.text);
      r.primary = r.suggestions.empty() ? L"" : r.suggestions.front();
      r.ok = !r.suggestions.empty();
      break;
    case AIActionType::kTranslate: {
      r.primary = Translate(req.text);
      r.ok = !r.primary.empty();
      if (r.primary == Trim(req.text)) {
        r.note = L"（本地词典未收录该句；云端路线可获得完整翻译）";
      }
      break;
    }
    case AIActionType::kProofread: {
      auto p = Proofread(req.text);
      r.primary = p.first;
      r.note = p.second;
      r.ok = true;
      break;
    }
    default:
      r.ok = false;
      r.note = L"未知动作类型";
      break;
  }
  return r;
}

// ===========================================================================
// 路线 A：云端 OpenAI 兼容 Provider（框架）
// ===========================================================================
namespace {
std::string JsonEscape(const std::wstring& w) {
  // 极简 JSON 字符串转义（仅转 \ " 控制符）。UTF-8 转换由调用方在 Windows 侧
  // 经 wtou8 完成；本处仅处理 ASCII 转义，保持 Linux 单测可独立编译。
  std::string out;
  for (wchar_t c : w) {
    if (c == L'"') out += "\\\"";
    else if (c == L'\\') out += "\\\\";
    else if (c == L'\n') out += "\\n";
    else if (c == L'\r') out += "\\r";
    else if (c == L'\t') out += "\\t";
    else {
      // 非 ASCII 直接按 UTF-8 编码（Windows 正式环境用 wtou8；此处给一个
      // 最小 UTF-8 编码器供 Linux 自测中文用）。
      unsigned u = (unsigned)c;
      if (u < 0x80) out.push_back((char)u);
      else if (u < 0x800) {
        out.push_back((char)(0xC0 | (u >> 6)));
        out.push_back((char)(0x80 | (u & 0x3F)));
      } else {
        out.push_back((char)(0xE0 | (u >> 12)));
        out.push_back((char)(0x80 | ((u >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (u & 0x3F)));
      }
    }
  }
  return out;
}
}  // namespace

CloudApiProvider::CloudApiProvider(const std::string& endpoint,
                                   const std::string& key_utf8,
                                   std::unique_ptr<IHttpPost> http)
    : endpoint_(endpoint), key_(key_utf8), http_(std::move(http)) {}

bool CloudApiProvider::IsAvailable() const {
  return !endpoint_.empty() && !key_.empty();
}

std::string CloudApiProvider::BuildRequestBody(const AIRequest& req) {
  // 系统提示按动作类型区分；用户内容即待处理文本。
  std::wstring sys;
  switch (req.action) {
    case AIActionType::kPolish: sys = L"请润色以下文本，输出润色结果："; break;
    case AIActionType::kContinue: sys = L"请续写以下文本，给出一个自然的续写："; break;
    case AIActionType::kTranslate: sys = L"请将以下文本在中英之间互译，输出译文："; break;
    case AIActionType::kProofread: sys = L"请纠正以下文本的错别字与标点，输出修正后文本："; break;
    default: sys = L"请处理以下文本："; break;
  }
  std::string body =
      "{\"model\":\"gpt-3.5-turbo\",\"messages\":[{"
      "\"role\":\"system\",\"content\":\"" + JsonEscape(sys) + "\"},{"
      "\"role\":\"user\",\"content\":\"" + JsonEscape(req.text) + "\"}],"
      "\"temperature\":0.7}";
  return body;
}

std::wstring CloudApiProvider::ExtractContent(const std::string& response_body) {
  // 极简解析：定位 "content"，读到其后第一个字符串值。
  const std::string key = "\"content\"";
  size_t pos = response_body.find(key);
  if (pos == std::string::npos) return L"";
  pos = response_body.find(':', pos + key.size());
  if (pos == std::string::npos) return L"";
  pos = response_body.find('"', pos);
  if (pos == std::string::npos) return L"";
  ++pos;
  std::string out;
  while (pos < response_body.size()) {
    char c = response_body[pos];
    if (c == '\\' && pos + 1 < response_body.size()) {
      char n = response_body[pos + 1];
      if (n == '"') { out.push_back('"'); pos += 2; continue; }
      if (n == 'n') { out.push_back('\n'); pos += 2; continue; }
      out.push_back(c); out.push_back(n); pos += 2; continue;
    }
    if (c == '"') break;
    out.push_back(c);
    ++pos;
  }
  // 最小 UTF-8 → wstring（Linux 自测用；Windows 正式侧用 utf8towcs）
  std::wstring w;
  size_t i = 0;
  while (i < out.size()) {
    unsigned char c = (unsigned char)out[i];
    if (c < 0x80) { w.push_back((wchar_t)c); ++i; }
    else if ((c >> 5) == 0x6 && i + 1 < out.size()) {
      unsigned cp = ((c & 0x1F) << 6) | ((unsigned char)out[i+1] & 0x3F);
      w.push_back((wchar_t)cp); i += 2;
    } else if ((c >> 4) == 0xE && i + 2 < out.size()) {
      unsigned cp = ((c & 0x0F) << 12) | (((unsigned char)out[i+1] & 0x3F) << 6) |
                    ((unsigned char)out[i+2] & 0x3F);
      w.push_back((wchar_t)cp); i += 3;
    } else { ++i; }
  }
  return w;
}

AIResult CloudApiProvider::Run(const AIRequest& req) {
  AIResult r;
  r.title = AIActionZhName(req.action);
  if (!IsAvailable() || !http_) {
    r.ok = false;
    r.note = L"云端未配置 endpoint/key";
    return r;
  }
  // [NETWORK-GATE] 真正发包前必须已由上层 AIAssistant::HandleRequest 过 Gate。
  // 这里只做 HTTP：body 仅含本次待处理文本，用完即弃，不记录、不留上下文。
  std::string body = BuildRequestBody(req);
  IHttpPost::Response resp = http_->Post(endpoint_, body, timeout_ms_);
  if (resp.status_code != 200) {
    r.ok = false;
    r.note = L"云端请求失败/超时，已降级本地";
    return r;
  }
  r.primary = ExtractContent(resp.body);
  r.ok = !r.primary.empty();
  if (!r.ok) r.note = L"云端响应解析失败，已降级本地";
  return r;
}

// ===========================================================================
// 门面 AIAssistant
// ===========================================================================
AIAssistant::AIAssistant()
    : local_(std::make_unique<LocalTemplateProvider>()),
      http_(std::make_unique<NullHttpPost>()) {}

AIAssistant::~AIAssistant() = default;

void AIAssistant::Configure(const Config& cfg) {
  cfg_ = cfg;
  // 按路由重建云端 Provider（endpoint/key 变更时）。
  // 真实 WinHTTP IHttpPost 在 Windows 侧注入；本批次保持 NullHttpPost。
}

bool AIAssistant::ActionEnabled(AIActionType a) const {
  if (cfg_.actions.empty()) return true;  // 空 = 全部启用（兜底）
  for (auto x : cfg_.actions) if (x == a) return true;
  return false;
}

ExtraCard AIAssistant::MakeCard(const AIResult& r, AIActionType action,
                                const std::wstring& route_note) {
  ExtraCard card;
  card.type = CARD_AI;
  card.title = r.title;
  // body = 主结果（润色/翻译/纠错的单行结果）+ 降级提示。
  card.body = r.primary;
  if (!r.note.empty()) {
    if (!card.body.empty()) card.body += L"\n";
    card.body += r.note;
  }
  if (!route_note.empty()) {
    if (!card.body.empty()) card.body += L"\n";
    card.body += route_note;
  }
  // items = 多行建议（续写的多条候选）。
  for (const auto& s : r.suggestions) card.items.emplace_back(Text(s));
  // action 语义：选中卡片某条 → 上屏。续写/翻译默认 replace（替换原文本），
  // 润色/纠错默认 replace；若 TSF 侧希望插入而非替换，可在 ui 层切换。
  card.action = (action == AIActionType::kContinue) ? L"insert" : L"replace";
  card.action_id = static_cast<int>(action);  // 关联动作类型，供 ai.pick 回查
  return card;
}

std::vector<ExtraCard> AIAssistant::HandleRequest(const std::wstring& text,
                                                  AIActionType action) {
  std::vector<ExtraCard> empty;
  if (!cfg_.enabled) return empty;                 // ai/enabled=false → 静默不服务
  if (action == AIActionType::kUnknown) return empty;
  if (!ActionEnabled(action)) return empty;         // 未在 ai/actions 列表
  std::wstring trimmed = text;
  // trim
  size_t b = 0, e = trimmed.size();
  while (b < e && (trimmed[b]==L' '||trimmed[b]==L'\t')) ++b;
  while (e > b && (trimmed[e-1]==L' '||trimmed[e-1]==L'\t')) --e;
  trimmed = trimmed.substr(b, e - b);
  if (trimmed.empty()) return empty;

  AIRequest req{trimmed, action};
  AIResult result;
  std::wstring route_note;

  bool use_cloud = (cfg_.route == L"cloud");
  if (use_cloud) {
    // [NETWORK-GATE] 云端路线发包前必查：总开关 + ai_assistant 模块开关 + 白名单。
    // gate_ 为借指针；Linux 单测时 gate_ 为空 → 视为未过（安全侧：拒绝联网）。
    bool gate_ok = false;
    if (gate_) gate_ok = gate_->Allow("ai_assistant", cfg_.cloud_endpoint);
    // gate_ 为空（生产未注入/单测）→ 视为未过（安全侧：拒绝联网，降级本地）。
    if (!gate_ok) {
      // 未过 Gate：绝不发请求，直接降级本地模板，并明示用户。
      route_note = L"（隐私闸门未放行/云端未开启，已用本地模板，未上传任何文本）";
      result = local_->Run(req);
    } else {
      CloudApiProvider cloud(cfg_.cloud_endpoint, cfg_.cloud_key,
                             std::make_unique<NullHttpPost>());
      result = cloud.Run(req);
      if (!result.ok) {
        route_note = L"（云端不可用/超时，已降级本地模板）";
        AIResult local = local_->Run(req);
        if (local.ok) result = local;
      } else {
        route_note = L"（来自云端；本次文本仅本次请求使用，不记录）";
      }
    }
  } else {
    // 路线 B：纯本地，零上传，不过 Gate。
    route_note = L"（本地模板，未联网）";
    result = local_->Run(req);
  }

  if (!result.ok) return empty;
  ExtraCard card = MakeCard(result, action, route_note);
  return {card};
}

std::wstring AIAssistant::PickItem(const std::vector<ExtraCard>& cards,
                                    int action_id,
                                    int item_index) {
  for (const auto& c : cards) {
    if (c.type == CARD_AI && c.action_id == action_id) {
      if (item_index >= 0 && item_index < (int)c.items.size()) {
        return c.items[item_index].str;
      }
      return c.body;  // 无 items 时回退 body
    }
  }
  return L"";
}

}  // namespace weasel
