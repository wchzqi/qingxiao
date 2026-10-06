// ============================================================================
// 青筱（QingXiaoType）P2-B：CloudCandidateService 实现（可移植核心）
// 对应头文件 CloudCandidateService.h。本文件不依赖 windows.h / rime_api.h，
// 可在 Linux 用 g++ 直接编译做纯逻辑单测（见 test/p2_b_cloud_test.cpp）。
// ============================================================================

#include "stdafx.h"

#include "CloudCandidateService.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <utility>

namespace weasel {

// ----------------------------------------------------------------------------
// 内置 Windows 网络实现（WinHTTP）。本块仅在 _WIN32 下编译；Linux 单测走注入
// 的 Stub。此处只给「声明 + 骨架」：真实 WinHTTP 请求体在 Windows 工程内补全，
// 出厂默认关闭态下不会被调用。未做实机可用性验证（见 SELF_CHECK 已知缺口）。
// ----------------------------------------------------------------------------
#ifdef _WIN32
#pragma message("CloudCandidateService: WinHTTP requester compiles on Windows only")
// 生产实现（RimeWithWeasel 工程内）：WinHttpOpen / WinHttpConnect /
// WinHttpOpenRequest(GET) / WinHttpReceiveResponse，支持 cloud/proxy 代理与
// TLS。超时取 m_cfg.timeout_ms。失败返回空串。骨架：
//
//   class WinHttpCloudRequester : public ICloudNetworkRequester {
//    public:
//     WinHttpCloudRequester(const std::string& proxy) : m_proxy(proxy) {}
//     std::string Fetch(const std::string& url, int timeout_ms) override {
//       // TODO(win): WinHTTP GET url, honor m_proxy & timeout_ms;
//       //            return body or "" on any failure. Never throw.
//       return std::string();
//     }
//    private:
//     std::string m_proxy;
//   };
#endif

namespace {
// 百分号编码（拼音键已是 a-z，此函数对其它字符兜底，供 custom 模板复用）。
std::string UrlEncode(const std::string& s) {
  static const char* hex = "0123456789ABCDEF";
  std::string out;
  out.reserve(s.size() * 3);
  for (unsigned char c : s) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out.push_back((char)c);
    } else {
      out.push_back('%');
      out.push_back(hex[(c >> 4) & 0xF]);
      out.push_back(hex[c & 0xF]);
    }
  }
  return out;
}
}  // namespace

CloudCandidateService::CloudCandidateService(IGateChecker* gate,
                                             ICloudNetworkRequester* network)
    : m_gate(gate), m_net(network) {}

CloudCandidateService::~CloudCandidateService() { Stop(); }

void CloudCandidateService::Configure(const CloudServiceConfig& cfg) {
  std::lock_guard<std::mutex> lk(m_mtx);
  m_cfg = cfg;
  if (m_cfg.cache_size <= 0)
    m_cfg.cache_size = 128;
}

bool CloudCandidateService::enabled() const { return m_cfg.enabled; }

void CloudCandidateService::Start() {
  if (!m_cfg.enabled) {
    return;  // 默认关：不起线程、不发任何请求。
  }
  std::lock_guard<std::mutex> lk(m_mtx);
  if (m_running)
    return;
  m_running = true;
  m_worker = std::thread([this] { _WorkerMain(); });
}

void CloudCandidateService::Stop() {
  {
    std::lock_guard<std::mutex> lk(m_mtx);
    if (!m_running)
      return;
    m_running = false;
  }
  m_cv.notify_all();
  if (m_worker.joinable())
    m_worker.join();
}

std::string CloudCandidateService::ExtractPinyinKey(
    const std::string& preedit_utf8) {
  // 只保留 ASCII 字母并转小写。UTF-8 多字节（汉字/声调符号）首字节 >= 0x80，
  // 被本过滤丢弃——这正是「数据最小化」：上传的只是拼音字母串。
  std::string key;
  key.reserve(preedit_utf8.size());
  for (unsigned char c : preedit_utf8) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
      key.push_back((char)std::tolower(c));
    }
    // 空格/数字/声调/标点/汉字 一律丢弃。
  }
  return key;
}

std::vector<std::string> CloudCandidateService::ParseResponse(
    const std::string& body) {
  // 默认契约：每行一个候选词（UTF-8）。空行跳过；最多取前 8 条防爆。
  std::vector<std::string> out;
  std::string cur;
  for (char c : body) {
    if (c == '\n' || c == '\r') {
      if (!cur.empty()) {
        out.push_back(cur);
        if (out.size() >= 8)
          break;
      }
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  if (!cur.empty() && out.size() < 8)
    out.push_back(cur);
  // 去重（保持顺序）。
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

std::string CloudCandidateService::_BuildUrl(
    const std::string& pinyin_key) const {
  std::string tmpl = m_cfg.custom_endpoint;
  if (tmpl.empty()) {
    return std::string();  // 未配置端点：无法请求（出厂默认）。
  }
  const std::string ph = "%PINYIN%";
  size_t pos = tmpl.find(ph);
  if (pos == std::string::npos) {
    return tmpl;  // 模板未放占位符：原样请求（容错）。
  }
  return tmpl.substr(0, pos) + UrlEncode(pinyin_key) + tmpl.substr(pos + ph.size());
}

void CloudCandidateService::_Enqueue(const std::string& pinyin_key) {
  // 调方持 m_mtx。去重：队列/在途已有则跳过。
  for (const auto& k : m_queue)
    if (k == pinyin_key)
      return;
  for (const auto& k : m_inflight)
    if (k == pinyin_key)
      return;
  m_queue.push_back(pinyin_key);
}

std::vector<std::string> CloudCandidateService::_CacheGetLocked(
    const std::string& key) {
  auto it = m_cache.find(key);
  if (it == m_cache.end())
    return {};
  // 命中即提升为最近使用。
  m_lru_order.erase(m_lru_it[key]);
  m_lru_order.push_front(key);
  m_lru_it[key] = m_lru_order.begin();
  return it->second;
}

void CloudCandidateService::_CachePutLocked(
    const std::string& key, const std::vector<std::string>& cands) {
  if (cands.empty())
    return;  // 空结果不缓存（避免把「失败/无结果」缓存成长期降级）。
  auto it = m_cache.find(key);
  if (it != m_cache.end()) {
    it->second = cands;
    m_lru_order.erase(m_lru_it[key]);
  } else {
    m_cache[key] = cands;
  }
  m_lru_order.push_front(key);
  m_lru_it[key] = m_lru_order.begin();
  // 超容量淘汰最久未用（back）。
  while ((int)m_cache.size() > m_cfg.cache_size) {
    auto victim = m_lru_order.back();
    m_lru_order.pop_back();
    m_cache.erase(victim);
    m_lru_it.erase(victim);
  }
}

CloudCandidateService::CardState CloudCandidateService::OnPreedit(
    const std::string& pinyin_key,
    std::vector<std::string>* out_candidates) {
  if (out_candidates)
    out_candidates->clear();
  // 未启用 / 键太短 → 不渲染、不请求。
  if (!m_cfg.enabled || (int)pinyin_key.size() < m_cfg.threshold) {
    return CardState::kNone;
  }

  std::lock_guard<std::mutex> lk(m_mtx);

  // 1) 缓存命中 → ready。
  auto it = m_cache.find(pinyin_key);
  if (it != m_cache.end()) {
    if (out_candidates)
      *out_candidates = it->second;
    m_lru_order.erase(m_lru_it[pinyin_key]);
    m_lru_order.push_front(pinyin_key);
    m_lru_it[pinyin_key] = m_lru_order.begin();
    return CardState::kReady;
  }

  // 2) 已在途 → loading 占位。
  for (const auto& k : m_inflight)
    if (k == pinyin_key)
      return CardState::kLoading;

  // 3) 新键：先过 Gate（同步只读，便宜）。不过 Gate 就不入队——避免隐私关时
  //    队列空转；worker 线程在真正发请求前会再过一次（强制检查点双保险）。
  std::string url = _BuildUrl(pinyin_key);
  if (url.empty())
    return CardState::kNone;  // 未配置端点：静默降级。
  if (m_gate && !m_gate->Allow("cloud_candidate", url)) {
    return CardState::kNone;  // [NETWORK-GATE] 隐私关：拒绝联网。
  }
  _Enqueue(pinyin_key);
  m_cv.notify_one();
  return CardState::kLoading;  // 入队后即显示占位，下一帧回来取结果。
}

void CloudCandidateService::Promote(const std::string& pinyin_key) {
  std::lock_guard<std::mutex> lk(m_mtx);
  (void)_CacheGetLocked(pinyin_key);  // 提升 LRU；无缓存则空操作。
}

void CloudCandidateService::ClearCache() {
  std::lock_guard<std::mutex> lk(m_mtx);
  m_cache.clear();
  m_lru_order.clear();
  m_lru_it.clear();
}

void CloudCandidateService::Invalidate(const std::string& pinyin_key) {
  std::lock_guard<std::mutex> lk(m_mtx);
  auto it = m_cache.find(pinyin_key);
  if (it != m_cache.end()) {
    m_lru_order.erase(m_lru_it[pinyin_key]);
    m_cache.erase(it);
    m_lru_it.erase(pinyin_key);
  }
  _Enqueue(pinyin_key);  // 强制重发
  m_cv.notify_one();
}

void CloudCandidateService::_WorkerMain() {
  for (;;) {
    std::string key;
    {
      std::unique_lock<std::mutex> lk(m_mtx);
      m_cv.wait_for(lk, std::chrono::milliseconds(200),
                    [this] { return !m_running || !m_queue.empty(); });
      if (!m_running && m_queue.empty())
        return;
      if (m_queue.empty())
        continue;
      key = m_queue.front();
      m_queue.pop_front();
      m_inflight.push_back(key);
    }

    // ===== [NETWORK-GATE] 真正发网络包前的强制检查点 =====
    // 模块 id 固定 "cloud_candidate"；endpoint 为本次完整 URL。
    // 任一不过即丢弃请求（静默降级，绝不弹错、绝不打断打字）。
    std::string url = _BuildUrl(key);
    bool allowed = !url.empty() && m_gate &&
                   m_gate->Allow("cloud_candidate", url);

    std::vector<std::string> cands;
    if (allowed && m_net) {
      // 同步阻塞调用（worker 线程，不影响主链路）。
      std::string body;
      try {
        body = m_net->Fetch(url, m_cfg.timeout_ms);
      } catch (...) {
        body.clear();  // 网络层兜底：任何异常都视为失败。
      }
      cands = ParseResponse(body);
    }
    // 失败/空结果：cands 为空 → 不写缓存（_CachePutLocked 跳过空），静默降级。

    {
      std::lock_guard<std::mutex> lk(m_mtx);
      m_inflight.erase(std::remove(m_inflight.begin(), m_inflight.end(), key),
                       m_inflight.end());
      _CachePutLocked(key, cands);
    }
    // 注意：结果就绪后不主动回推 TSF（Weasel IPC 为请求-响应范式）。
    // 下一帧 _Respond / TSF cloud.query 拉取时，OnPreedit 即返回 kReady。
  }
}

}  // namespace weasel
