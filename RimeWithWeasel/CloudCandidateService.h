// ============================================================================
// 青筱（QingXiaoType）P2-B：云候选服务 CloudCandidateService（可移植核心）
//
// 归属工程：RimeWithWeasel.vcxproj（与 NetworkGate / ClipboardManager 同工程）。
//
// 设计目标（对齐主文档亮点 20【fcitx5 cloudpinyin 范式，已查证】与 P2-A 契约）：
//   - 默认关闭（cloud/enabled=false）；达到拼音长度阈值（默认 10）才异步发请求；
//   - 数据最小化：只上传「归一化后的拼音键」（小写 a-z），绝不上传候选文本/上下文；
//   - 异步旁路：独立 worker 线程 + 请求队列，绝不阻塞 librime 解码主链路与
//     _Respond 回写；结果只进内存 LRU 缓存（默认 128 条），用完即弃不存储；
//   - 降级静默：超时/断网/解析失败一律不弹错误、不打断打字，回退纯本地候选；
//   - 占位符：在途未返回时，TSF 侧渲染 CARD_CLOUD 卡片 body="…"；
//   - Gate 强制：worker 线程在「真正发网络包前」必过
//       NetworkGate::Gate("cloud_candidate", endpoint)，失败即丢弃请求。
//
// 【可移植性硬约束】本头文件与对应 .cpp 不 #include <windows.h>、不 #include
//   <rime_api.h>，仅用 C++ 标准库。网络层与闸门经抽象接口注入：
//     - ICloudNetworkRequester：同步 HTTP 请求（Windows 用 WinHTTP 实现，
//       Linux 单测注入 Stub）。
//     - IGateChecker：闸门查询（生产用 NetworkGateAdapter 包 NetworkGate，
//       Linux 单测注入 FakeGate）。
//   这样同一套核心逻辑可在 Linux 用 g++ 跑纯逻辑单测（阈值/LRU/解析/降级），
//   Windows 侧真实网络/代理验证列为已知缺口（见 SELF_CHECK_P2_B.md）。
// ============================================================================
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace weasel {

// ===== 注入接口：闸门（生产包 NetworkGate，测试包 FakeGate） =====
// 只暴露服务需要的最窄能力，避免服务核心依赖 windows.h / rime_api.h。
class IGateChecker {
 public:
  virtual ~IGateChecker() = default;
  // 返回 true 才允许联网。等价 NetworkGate::Gate(module_id, endpoint)。
  virtual bool Allow(const std::string& module_id,
                     const std::string& endpoint) const = 0;
};

// ===== 注入接口：同步网络请求（worker 线程内调用，阻塞无妨） =====
class ICloudNetworkRequester {
 public:
  virtual ~ICloudNetworkRequester() = default;
  // 同步 GET url；timeout_ms 为超时上限。
  // 成功返回 UTF-8 响应体；任何失败（超时/断网/TLS 错误）返回空串，不得抛异常。
  virtual std::string Fetch(const std::string& url, int timeout_ms) = 0;
};

// ===== 服务运行配置（Initialize 读 weasel.yaml cloud/ 段后填入） =====
// 键路径（weasel.yaml，privacy 段之外的新增 cloud/ 段）：
//   cloud/enabled        bool   默认 false
//   cloud/threshold      int    默认 10（拼音字母长度，不是汉字数）
//   cloud/source         string 默认 "custom"（=出厂不内置可用端点）
//   cloud/custom_endpoint string 示例模板：https://host/api?q=%PINYIN%
//   cloud/timeout_ms     int    默认 1500
//   cloud/cache_size     int    默认 128
//   cloud/proxy          string 空=读系统代理；非空=curl 格式 host:port
struct CloudServiceConfig {
  bool enabled = false;
  int threshold = 10;
  std::string source_id = "custom";
  std::string custom_endpoint;
  int timeout_ms = 1500;
  int cache_size = 128;
  std::string proxy;
};

class CloudCandidateService {
 public:
  // gate 与 network 均为「不拥有」的注入对象，生命周期由调用方（Handler）保证。
  // network 传 nullptr 时：Windows 侧启用内置 WinHTTP 实现（#ifdef _WIN32），
  // Linux 侧为空（单测必须显式注入 Stub）。
  CloudCandidateService(IGateChecker* gate,
                        ICloudNetworkRequester* network = nullptr);
  ~CloudCandidateService();

  CloudCandidateService(const CloudCandidateService&) = delete;
  CloudCandidateService& operator=(const CloudCandidateService&) = delete;

  void Configure(const CloudServiceConfig& cfg);
  bool enabled() const;

  // 启动/停止 worker 线程。enabled=false 时 Start 为空操作（直接返回）。
  // Finalize 前由 Handler 调用 Stop()，安全排空队列、join 线程。
  void Start();
  void Stop();

  // 主链路回调（_Respond 内同步调用，绝不阻塞）：
  //   pinyin_key 为调用方经 ExtractPinyinKey 归一化后的小写拼音键。
  // 返回当前应渲染的云卡片状态，并在 kReady 时经 out_candidates 带出候选。
  //   kNone    ：不渲染云卡片（未达阈值 / Gate 关 / 缓存与在途皆无 / 降级）。
  //   kLoading ：已在途请求，TSF 渲染 body="…" 占位卡片。
  //   kReady   ：缓存命中，out_candidates 为云端候选（通常 1 条）。
  enum class CardState { kNone, kLoading, kReady };
  CardState OnPreedit(const std::string& pinyin_key,
                      std::vector<std::string>* out_candidates);

  // 用户选中某云候选后调用：把该拼音键提升为 LRU 最近使用（命中即缓存）。
  void Promote(const std::string& pinyin_key);
  // 清空内存 LRU（cloud.cache_clear）。
  void ClearCache();
  // 主动失效一个键（cloud.query 强制刷新用）：删缓存并重发。
  void Invalidate(const std::string& pinyin_key);

  // ===== 纯逻辑静态工具（供 Linux g++ 单测直接覆盖，无任何依赖） =====
  // 从 preedit 原文（UTF-8）提取拼音键：只保留 [a-z]，其余（空格/声调/标点/
  // 汉字）全部丢弃，并转小写。例如 "Ni Hao shi jie!" -> "nihaoshijie"。
  static std::string ExtractPinyinKey(const std::string& preedit_utf8);
  // 解析响应体（默认契约：每行一个候选词，UTF-8，空行跳过）。
  // 接入真实 JSON API 时替换本函数为 JSON 解析（建议 rapidjson），返回去重后的
  // 候选词向量（按源置信序）。失败一律返回空 vector（调用方据此降级）。
  static std::vector<std::string> ParseResponse(const std::string& body);

 private:
  void _WorkerMain();
  // 拼接请求 URL：把模板中的 %PINYIN% 替换为 URL-编码后的拼音键。
  std::string _BuildUrl(const std::string& pinyin_key) const;
  // 入队一个待请求键（去重：已在途/队列已有则跳过）。
  void _Enqueue(const std::string& pinyin_key);
  // 缓存读写（调方持 m_mtx）。
  std::vector<std::string> _CacheGetLocked(const std::string& key);
  void _CachePutLocked(const std::string& key,
                       const std::vector<std::string>& cands);

  IGateChecker* m_gate;                 // 不拥有
  ICloudNetworkRequester* m_net;        // 不拥有（或 Windows 内置）
  CloudServiceConfig m_cfg;

  std::thread m_worker;
  std::mutex m_mtx;
  std::condition_variable m_cv;
  bool m_running = false;               // 受 m_mtx 保护（配合 cv）
  std::deque<std::string> m_queue;     // 待请求键
  std::vector<std::string> m_inflight;   // 在途键（去重）

  // 内存 LRU：front = 最近使用。m_lru_it 存每个 key 在 m_lru_order 中的迭代器。
  std::list<std::string> m_lru_order;
  std::unordered_map<std::string, std::vector<std::string>> m_cache;
  std::unordered_map<std::string, std::list<std::string>::iterator> m_lru_it;
};

}  // namespace weasel
