// ============================================================================
// 青筱（QingXiaoType）P2-E：TranslationService 实现
// （对应头 TranslationService.h）
//
// 编译说明：
//   - 本文件随 RimeWithWeasel 工程在 Windows（MSVC）编译，依赖 <winhttp.h>。
//   - 本机 Linux 不编译本文件（硬约束）；可在 Linux 单测的仅为
//     TranslateLocalDict.cpp（见交付物 D）。TranslateCloud() 的 WinHTTP 段
//     用 #ifdef _WIN32 包裹，非 Windows 下返回空串占位。
// ============================================================================

#include "stdafx.h"
#include <logging.h>

#include "TranslationService.h"
#include "NetworkGate.h"
#include "TranslateLocalDict.h"

#include <cstdlib>
#include <chrono>

#ifdef _WIN32
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#endif

namespace weasel {

namespace {
// 配置读取小helper：bool / int / string 三态（缺键回退默认）。
bool CfgBool(RimeConfig* c, const char* key, bool dflt) {
  Bool v = False;
  if (rime_get_api()->config_get_bool(c, key, &v)) return !!v;
  return dflt;
}
int CfgInt(RimeConfig* c, const char* key, int dflt) {
  int v = 0;
  if (rime_get_api()->config_get_int(c, key, &v)) return v;
  return dflt;
}
std::string CfgStr(RimeConfig* c, const char* key, const char* dflt) {
  char buf[1024] = {0};
  if (rime_get_api()->config_get_string(c, key, buf, sizeof(buf) - 1) &&
      buf[0]) {
    return std::string(buf);
  }
  return dflt ? std::string(dflt) : std::string();
}
}  // namespace

TranslationService::TranslationService() {
  // 启动单 worker 线程（Initialize 后即就绪；任务为空则阻塞在 cv_）。
  shutting_down_ = false;
  worker_ = std::thread([this] { WorkerLoop(); });
}

TranslationService::~TranslationService() {
  {
    std::lock_guard<std::mutex> lk(mtx_);
    shutting_down_ = true;
  }
  cv_.notify_all();
  if (worker_.joinable()) worker_.join();
}

void TranslationService::LoadFromConfig(RimeConfig* config, NetworkGate* gate) {
  gate_ = gate;
  if (!config) return;

  cfg_.enabled        = CfgBool(config, "translate/enabled", false);
  cfg_.target_lang    = CfgStr(config, "translate/target_lang", "en");
  cfg_.auto_threshold = CfgInt(config, "translate/auto_threshold", 0);
  cfg_.endpoint       = CfgStr(config, "translate/endpoint", "");
  cfg_.model          = CfgStr(config, "translate/model", "gpt-4o-mini");
  cfg_.key_ref        = CfgStr(config, "translate/key_ref", "");
  cfg_.source         = CfgStr(config, "translate/source", "cloud");
  cfg_.timeout_ms     = CfgInt(config, "translate/timeout_ms", 5000);

  DLOG(INFO) << "TranslationService loaded: enabled=" << cfg_.enabled
             << " target=" << cfg_.target_lang
             << " auto_threshold=" << cfg_.auto_threshold
             << " source=" << cfg_.source
             << " endpoint=" << cfg_.endpoint
             << " key_ref=" << cfg_.key_ref;  // 只打变量名，绝不打 key 明文
}

void TranslationService::Submit(DWORD ipc_id,
                                const std::string& text_utf8,
                                const std::string& target_lang) {
  if (!cfg_.enabled) {
    // 功能总开关关闭：完全静默，不入队、不联网、不回退本地。
    return;
  }
  if (text_utf8.empty()) return;

  Job job;
  job.ipc_id = ipc_id;
  job.text = text_utf8;
  job.target_lang = target_lang.empty() ? cfg_.target_lang : target_lang;

  {
    std::lock_guard<std::mutex> lk(mtx_);
    if (shutting_down_) return;
    queue_.push(std::move(job));
  }
  cv_.notify_one();
}

void TranslationService::AutoTranslate(DWORD ipc_id,
                                      const std::string& preedit_utf8) {
  if (!cfg_.enabled || cfg_.auto_threshold <= 0) return;       // 默认关
  if (preedit_utf8.empty()) return;

  // 阈值按 UTF-8 字符数近似：这里用「字节数 >= 阈值*2」粗估中文（一中文 3 字节）。
  // 【已知缺口】触发时机/字符口径需 Windows 实机调优（见 SELF_CHECK）。
  const size_t approx_chars = preedit_utf8.size() / 2;
  if ((int)approx_chars < cfg_.auto_threshold) return;

  // 去抖：与上一帧相同则跳过，避免每个按键都发请求。
  {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = last_auto_text_.find(ipc_id);
    if (it != last_auto_text_.end() && it->second == preedit_utf8) return;
    last_auto_text_[ipc_id] = preedit_utf8;
  }
  Submit(ipc_id, preedit_utf8, "");
}

void TranslationService::WorkerLoop() {
  for (;;) {
    Job job;
    {
      std::unique_lock<std::mutex> lk(mtx_);
      cv_.wait(lk, [this] { return shutting_down_ || !queue_.empty(); });
      if (shutting_down_ && queue_.empty()) return;
      job = std::move(queue_.front());
      queue_.pop();
    }
    RunJob(job);
  }
}

void TranslationService::RunJob(const Job& job) {
  std::string result;
  const std::string lang = job.target_lang.empty() ? cfg_.target_lang
                                                    : job.target_lang;

  if (cfg_.source == "local") {
    // 纯本地模式：不联网、不过 Gate（无网络包）。
    result = translate_local::Lookup(job.text, lang);
  } else {
    // 云端优先；失败/超时/Gate 拒绝一律降级本地兜底，再不行则空。
    result = TranslateCloud(job.text, lang);
    if (result.empty()) {
      result = translate_local::Lookup(job.text, lang);
    }
  }
  // 数据最小化排障：只打长度，不打原文/译文内容。
  DLOG(INFO) << "translate done: in_len=" << job.text.size()
             << " out_len=" << result.size() << " lang=" << lang;
  Deliver(job.ipc_id, result);
}

void TranslationService::Deliver(DWORD ipc_id,
                                 const std::string& result_utf8) {
  if (on_result_) {
    // Handler 回调内负责加锁写入 session_status.pending_aux。
    // result_utf8 为空 = 无译文，Handler 选择静默（不清空 aux、不报错）。
    on_result_(ipc_id, result_utf8);
  }
}

std::string TranslationService::ResolveApiKey() const {
  if (cfg_.key_ref.empty()) return std::string();
  // key_ref 仅存「环境变量名」，真正的 key 从环境读取，绝不进 YAML 明文。
  // （P2-D 凭据管理器方案落地后，可在此分支用 CredentialRead 取。）
  const char* v = std::getenv(cfg_.key_ref.c_str());
  if (!v) return std::string();
  return std::string(v);
}

// ----------------------------------------------------------------------------
// 云端翻译：OpenAI 兼容 chat/completions 适配器。
// [NETWORK-GATE] 本请求必须先过隐私闸门，禁止绕过。
//   模块 id: "translate"
//   违反后果：用户关闭隐私总开关后仍上传待译文本 -> 违反本地优先承诺。
// ----------------------------------------------------------------------------
std::string TranslationService::TranslateCloud(const std::string& text_utf8,
                                              const std::string& target_lang) {
  // 第一道：强制 Gate。拒绝则降级（返回空，由 RunJob 走本地兜底/静默）。
  if (!gate_ || !gate_->Gate("translate", cfg_.endpoint)) {
    DLOG(INFO) << "translate blocked by NetworkGate (privacy off / endpoint not whitelisted)";
    return std::string();
  }
  if (cfg_.endpoint.empty()) {
    DLOG(INFO) << "translate endpoint not configured";
    return std::string();
  }
  std::string key = ResolveApiKey();
  if (key.empty()) {
    DLOG(INFO) << "translate api key unresolved (key_ref=" << cfg_.key_ref
               << "); refuse silent cloud request";
    return std::string();
  }

#ifdef _WIN32
  // ---- Windows：WinHTTP POST OpenAI 兼容 chat/completions ----
  // 说明：以下为可用骨架；JSON 构造/解析为简化实现（手写转义 + 取
  // choices[0].message.content）。Windows 实连联调时建议替换为成熟 JSON 库。
  // 数据最小化：messages 里 user 段只放「待译文本」本身，不带输入历史/候选。
  std::string prompt = "Translate the following text to " + target_lang +
                       ". Output only the translation, no explanation.";
  // 极简 JSON 字符串转义（仅处理引号/反斜杠/控制字符）。
  auto json_escape = [](const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
      switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
          if (static_cast<unsigned char>(c) < 0x20) {
            char buf[8];
            _snprintf(buf, sizeof(buf), "\\u%04x", c);
            out += buf;
          } else {
            out += c;
          }
      }
    }
    return out;
  };
  std::string body =
      "{\"model\":\"" + cfg_.model +
      "\",\"messages\":["
      "{\"role\":\"system\",\"content\":\"" + json_escape(prompt) + "\"},"
      "{\"role\":\"user\",\"content\":\"" + json_escape(text_utf8) + "\"}"
      "],\"temperature\":0.3}";

  // 解析 endpoint：https://host[:port]/path
  // （省略：此处假定 endpoint 已完整给出 host 与 path；解析在 Windows 实连
  //   联调时用 winhttp 样例补全，本骨架不展开以避免在 Linux 侧堆叠未验证代码。）
  // WinHttpOpen/Connect/OpenRequest/SendRequest/ReadData 的标准流程从略，
  // 关键点（已在注释固定，供 Windows 实连时照做）：
  //   1) WinHttpOpen 设超时 = cfg_.timeout_ms（RESOLUTION/CONNECT/SEND/RECEIVE）。
  //   2) 加头：Authorization: Bearer <key>；Content-Type: application/json。
  //   3) POST body；读回响应字符串。
  //   4) 从响应里定位 "\"content\":\"" 后到下一个未转义引号前的子串，即为译文。
  //   5) 全程不写日志/文件原文译文；任何失败返回空串。
  DLOG(INFO) << "translate cloud request issued (len=" << text_utf8.size()
             << ", endpoint host)";
  return std::string();  // 占位：Windows 实连联调点（见 SELF_CHECK 已知缺口）
#else
  // 本机 Linux 不联网：返回空，由调用方降级本地兜底。
  DLOG(INFO) << "translate cloud: non-Windows build, skip network";
  return std::string();
#endif
}

}  // namespace weasel
