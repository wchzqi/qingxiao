// ============================================================================
// 青筱（QingXiaoType）P2-A：NetworkGate 实现
// （对应头文件 NetworkGate.h；本批次不做任何真实联网，仅做闸门判定）
// ============================================================================
#include "stdafx.h"

#include "NetworkGate.h"

#include <algorithm>

namespace weasel {

namespace {
// 把 UTF-8 std::string 转 std::wstring（与 WeaselUtility.h 的 u8tow 等价；
// NetworkGate 不依赖 TSF/UI，这里给一个最小实现，避免引入 WeaselUtility）。
std::wstring U8ToW(const std::string& s) {
  if (s.empty())
    return std::wstring();
  int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
  std::wstring out(len, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], len);
  return out;
}
}  // namespace

NetworkGate::NetworkGate() {
  RegisterBuiltinModules();
}

NetworkGate::~NetworkGate() = default;

void NetworkGate::RegisterBuiltinModules() {
  // 出厂注册表：id / 中文名 / 数据最小化声明 / 默认开关。
  // 【硬约束】default_enabled 一律 false——本地优先，云端全可选。
  modules_.clear();
  modules_.push_back(PrivacyModuleInfo{
      "cloud_candidate", L"云候选 / 云联想",
      L"达到长度阈值时上传拼音串以获取云端候选，结果只进内存 LRU 缓存，不存储输入内容。",
      false, false, {}});
  modules_.push_back(PrivacyModuleInfo{
      "ai_assistant", L"AI 助手（润色/续写）",
      L"按用户主动唤起上传已输入文本调用大模型 API，不记录交互上下文。",
      false, false, {}});
  modules_.push_back(PrivacyModuleInfo{
      "translate", L"翻译（边写边译）",
      L"开启即明示上传待翻译文本，译文用完即弃，不存储原文与译文。",
      false, false, {}});
  modules_.push_back(PrivacyModuleInfo{
      "voice_cloud", L"在线语音识别",
      L"上传录音音频用于在线识别，用后不存；默认优先离线引擎。",
      false, false, {}});
}

void NetworkGate::LoadFromConfig(RimeConfig* config) {
  if (!config)
    return;

  // privacy/master_enabled —— 缺省 false（本地优先承诺）。
  Bool master = False;
  if (rime_get_api()->config_get_bool(config, "privacy/master_enabled", &master)) {
    master_enabled_ = !!master;
  } else {
    master_enabled_ = false;
  }

  // 逐模块读 privacy/modules.<id>/enabled 与 data_scope。
  // 未知模块 id 安全忽略；缺键回退 default_enabled（false）。
  for (auto& m : modules_) {
    std::string path_on = "privacy/modules/" + m.id + "/enabled";
    Bool on = False;
    if (rime_get_api()->config_get_bool(config, path_on.c_str(), &on)) {
      m.enabled = !!on;
    } else {
      m.enabled = m.default_enabled;
    }
    // data_scope 为字符串声明；允许用户在 YAML 覆盖文案（设置页展示用）。
    {
      std::string path_scope = "privacy/modules/" + m.id + "/data_scope";
      char buf[512] = {0};
      if (rime_get_api()->config_get_string(config, path_scope.c_str(), buf,
                                           sizeof(buf) - 1)) {
        m.data_scope = U8ToW(buf);
      }
    }
    // endpoints 白名单：YAML list。librime RimeConfig 无统一 list API，
    // 按约定逐个下标读（privacy/modules.<id>/endpoints/0、/1…），
    // 读到空串即停。空 list = 不做端点校验（仅看总开关 + 模块开关）。
    m.whitelist.clear();
    for (int i = 0; i < 16; ++i) {
      std::string path_ep =
          "privacy/modules/" + m.id + "/endpoints/" + std::to_string(i);
      char buf[512] = {0};
      if (rime_get_api()->config_get_string(config, path_ep.c_str(), buf,
                                            sizeof(buf) - 1) &&
          buf[0] != '\0') {
        m.whitelist.push_back(buf);
      } else {
        break;
      }
    }
  }
}

const PrivacyModuleInfo* NetworkGate::Find(const std::string& id) const {
  for (const auto& m : modules_) {
    if (m.id == id)
      return &m;
  }
  return nullptr;
}

bool NetworkGate::WhitelistAllows(const PrivacyModuleInfo& m,
                                  const std::string& endpoint) const {
  // 空白名单：只校验总开关 + 模块开关，端点放行。
  if (m.whitelist.empty())
    return true;
  if (endpoint.empty())
    return true;  // 未提供 endpoint 时不做端点校验（调用方应补全）
  for (const auto& allowed : m.whitelist) {
    if (!allowed.empty() &&
        endpoint.find(allowed) != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool NetworkGate::Gate(const std::string& module_id,
                       const std::string& endpoint) const {
  // 第一道：总开关。关闭则一律拒绝（本地优先）。
  if (!master_enabled_) {
    return false;
  }
  // 第二道：模块开关。
  const PrivacyModuleInfo* m = Find(module_id);
  if (!m) {
    return false;  // 未注册模块 = 不允许联网
  }
  if (!m->enabled) {
    return false;
  }
  // 第三道：端点白名单。
  if (!WhitelistAllows(*m, endpoint)) {
    return false;
  }
  return true;
}

std::wstring NetworkGate::GetPrivacySummary() const {
  if (!master_enabled_) {
    return L"隐私模式：总开关已关闭（本地优先，全部联网功能停用）";
  }
  int on = 0;
  for (const auto& m : modules_)
    if (m.enabled)
      ++on;
  if (on == 0) {
    return L"隐私模式：总开关已开启，但尚未启用任何联网模块";
  }
  return L"隐私模式：总开关已开启，已启用 " + std::to_wstring(on) +
         L" 个联网模块（见下方各模块数据说明）";
}

}  // namespace weasel
