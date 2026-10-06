// ============================================================================
// 青筱（QingXiaoType）P2-B：RimeWithWeasel.h 追加块
//
// 叠加顺序：本文件 = P1-B1 完整替换版（p1_b1.h）
//          + P2-A 追加（p2_a.h：NetworkGate 持有 + pending_aux）
//          + 本文件 P2-B 追加（云候选服务持有 + Gate 适配器）。
//
// P2-B 追加内容：
//   1) include <CloudCandidateService.h>
//   2) CloudGateAdapter：把 P2-A NetworkGate 适配成服务需要的 IGateChecker
//      （模块 id 固定 "cloud_candidate"）。
//   3) Handler 持有 std::unique_ptr<CloudCandidateService> m_cloud
//      + adapter 成员 + 卡片 action_id 自增计数器。
// ============================================================================

// ----------------------------------------------------------------------------
// 改动 H-B1：顶部 include 追加。
// 贴入位置：p2_a.h 现有
//     #include "NetworkGate.h"        之后追加：
// ----------------------------------------------------------------------------
//   #include "CloudCandidateService.h"

// ----------------------------------------------------------------------------
// 改动 H-B2：CloudGateAdapter（紧跟 include 之后、class RimeWithWeaselHandler
//   之前定义）。把 P2-A 的 NetworkGate::Gate 适配为服务核心的 IGateChecker，
//   模块 id 固定写死 "cloud_candidate"，调用方不必关心模块 id。
// ----------------------------------------------------------------------------
//   // P2-B：NetworkGate -> IGateChecker 适配器（云候选模块 id 固定）。
//   class CloudGateAdapter : public weasel::IGateChecker {
//    public:
//     explicit CloudGateAdapter(weasel::NetworkGate* gate) : m_gate(gate) {}
//     bool Allow(const std::string& module_id,
//                const std::string& endpoint) const override {
//       return m_gate && m_gate->Gate(module_id, endpoint);
//     }
//    private:
//     weasel::NetworkGate* m_gate;  // 不拥有（P2-A 已由 Handler 持有）
//   };

// ----------------------------------------------------------------------------
// 改动 H-B3：Handler 类私有成员区追加（贴入 p2_a.h 现有
//     std::unique_ptr<weasel::NetworkGate> m_net_gate;   之后）。
// ----------------------------------------------------------------------------
//   // ===== P2-B 追加：云候选服务（Initialize 装载 cloud/ 段并 Start） =====
//   // 异步 worker 线程；默认关（cloud/enabled=false）。所有联网经
//   // m_cloud_gate -> NetworkGate::Gate("cloud_candidate", url) 闸门。
//   std::unique_ptr<CloudGateAdapter> m_cloud_gate;
//   std::unique_ptr<weasel::CloudCandidateService> m_cloud;
//   // 云卡片 action_id 自增编号（选中回 cloud.pick 时回选用）。
//   int m_cloud_action_id = 0;
//
// 构造/析构说明：
//   - m_cloud / m_cloud_gate 在 Initialize() 内 make_unique（依赖 m_net_gate
//     已 LoadFromConfig），不在构造列表（构造时 config 尚未打开）。
//   - Finalize() 与 StartMaintenance() 需补 m_cloud->Stop()（见 p2_b_patch.cpp
//     改动 C-3），安全 join worker 线程，避免进程退出时线程悬挂。
