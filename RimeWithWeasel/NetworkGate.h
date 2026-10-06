// ============================================================================
// 青筱（QingXiaoType）P2-A：NetworkGate 隐私/联网治理闸门
//
// 归属工程：RimeWithWeasel/（与 ClipboardManager 同工程，由
//           RimeWithWeaselHandler 构造并持有；与基线约定 §7「旁路服务归
//           RimeWithWeasel」一致）。
//
// 职责（本批次只做「闸门 + 注册表 + 摘要」，不做任何真实联网）：
//   1) 模块注册表：id / 中文名 / 数据最小化声明 / 默认状态（默认全部 false）。
//   2) Gate(module_id, endpoint) 查询：
//        - privacy/master_enabled=false      → 一律拒绝（本地优先承诺收口）
//        - 模块开关 privacy/modules.<id>.enabled=false → 拒绝
//        - endpoint 非空且不在模块白名单      → 拒绝
//        - 否则放行
//   3) GetPrivacySummary()：供 Deployer 设置页 / privacy.status 回写展示。
//   4) 启动时从 weasel.yaml 的 privacy/ 段读入（rime_api RimeConfig）。
//
// 【强制检查点约定（P2-B/C/D/E 必须遵守）】
//   任何新增联网代码（HTTPS 请求 / WebSocket / DNS 解析前）必须在发起前调用：
//       if (!g_net_gate->Gate("cloud_candidate", "https://api.example.com")) {
//         // 拒绝联网：降级为纯本地行为，绝不静默发请求
//         return;
//       }
//   未过 Gate 的网络请求 = 违反隐私总开关 = 验收不通过。
//   新增模块时：① 在下方 kBuiltinModules 注册表登记一行；
//             ② 在 privacy/modules.<id>/ 下补 enabled + data_scope + endpoints。
// ============================================================================
#pragma once

#include <string>
#include <vector>

#pragma warning(disable : 4005)
#include <rime_api.h>
#pragma warning(default : 4005)

namespace weasel {

// 单个联网模块的注册信息与运行态
struct PrivacyModuleInfo {
  std::string id;             // 模块 id（小写，键路径片段），如 "cloud_candidate"
  std::wstring name_zh;      // 中文名（设置页列表展示）
  std::wstring data_scope;    // 数据最小化声明（静态文本，明示上传什么、是否存储）
  bool default_enabled;       // 出厂默认（一律 false，本地优先）
  bool enabled;               // 当前运行态（从 weasel.yaml 读入）
  std::vector<std::string> whitelist;  // 允许的端点/域名白名单（空=仅看总开关与模块开关）
};

class NetworkGate {
 public:
  NetworkGate();
  ~NetworkGate();

  // 启动时由 RimeWithWeaselHandler::Initialize 在 config_open("weasel") 后调用。
  // config 为已打开的 weasel 配置句柄；读失败/缺键一律回退默认（全关）。
  void LoadFromConfig(RimeConfig* config);

  // 闸门查询（核心）。endpoint 留空表示只校验总开关 + 模块开关（如离线语音本地推理）。
  // 返回 true 才允许发起网络请求。
  bool Gate(const std::string& module_id,
            const std::string& endpoint = "") const;

  // 总开关当前态
  bool MasterEnabled() const { return master_enabled_; }

  // 只读访问模块注册表（供 Deployer 设置页渲染开关列表 / privacy.status 回写）
  const std::vector<PrivacyModuleInfo>& Modules() const { return modules_; }
  const PrivacyModuleInfo* Find(const std::string& id) const;

  // 供 UI 展示的一行摘要（如：「隐私模式：总关（全部联网已停用）」）。
  std::wstring GetPrivacySummary() const;

 private:
  // 内置模块注册表（出厂默认）。新增模块在此追加一行。
  void RegisterBuiltinModules();
  // endpoint 是否命中模块白名单（子串匹配域名部分；空白名单视为放行端点校验）。
  bool WhitelistAllows(const PrivacyModuleInfo& m,
                       const std::string& endpoint) const;

  bool master_enabled_{false};  // privacy/master_enabled，默认 false
  std::vector<PrivacyModuleInfo> modules_;
};

}  // namespace weasel
