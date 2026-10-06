// ============================================================================
// 青筱（QingXiaoType）P2-E：TranslationService 翻译服务
//
// 归属：RimeWithWeasel/（与 NetworkGate / ClipboardManager 同工程，由
//       RimeWithWeaselHandler 构造并持有；主文档亮点 13 映射模块）。
//
// 职责：
//   1) 划词翻译 / 边写边译：把「待译文本」经可配置翻译 API（OpenAI 兼容
//      chat/completions 适配器，用户自带 endpoint/key）译为目标语言，
//      译文经回调写回 session_status.pending_aux，再由 _Respond 以
//      ctx.aux=<译文> 下发到候选窗上方（P2-A 已打通线上行）。
//   2) 异步：所有真实网络请求在独立 worker 线程执行，绝不阻塞 librime 解码
//      与服务端消息线程（HandleExtension/_Respond 立即返回）。
//   3) 隐私硬约束（与 P2-A 隐私基线一致）：
//        - 每次云端请求前必须 Gate("translate", endpoint)（强制检查点）；
//        - 数据最小化：请求体只放「待译文本」，不带输入历史/候选上下文；
//        - 响应不落盘、不记录原文与译文（仅记录长度/状态用于排障）；
//        - key 不写 YAML 明文：translate/key_ref 指向环境变量名（或凭据管理器
//          目标名，见 P2-D 建议），运行时读取；
//        - 失败/超时/Gate 拒绝一律「无提示回退」（返回空，UI 不占位不报错）。
//   4) 本地兜底：TranslateLocalDict（离线简化表）在未联网 / Gate 拒绝时
//      尝试命中；命中即返回译文，未命中返回空。
//
// 线程模型：
//   - Submit()/AutoTranslate() 在服务端消息线程调用，只做入队，立即返回。
//   - WorkerLoop() 单线程串行消费队列；真实 HTTPS 在此阻塞（带超时）。
//   - 完成后经 on_result_ 回调回写 pending_aux（由 Handler 加锁写入）。
// ============================================================================
#pragma once

#include <functional>
#include <map>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <string>
#include <thread>

#pragma warning(disable : 4005)
#include <rime_api.h>
#pragma warning(default : 4005)

namespace weasel {

class NetworkGate;  // 前向声明（不持有，借用 Handler 的闸门）

// translate/ 段配置（weasel.yaml）。键路径见交付物 C 配置文档。
struct TranslationConfig {
  bool enabled = false;          // translate/enabled，默认 false（本地优先）
  std::string target_lang = "en";  // translate/target_lang，默认 en
  int auto_threshold = 0;        // translate/auto_threshold，0=关闭边写边译
  std::string endpoint;          // translate/endpoint，OpenAI 兼容端点
  std::string model = "gpt-4o-mini";  // translate/model（可选）
  std::string key_ref;           // translate/key_ref：环境变量名/凭据目标名（非 key 明文）
  std::string source = "cloud";  // translate/source：cloud | local
  int timeout_ms = 5000;         // translate/timeout_ms（可选）
};

// 结果回调：参数 (ipc_id, result_utf8)。result_utf8 为空表示「无译文/降级」。
// Handler 据此把译文写入对应会话的 pending_aux（P2-A 已打通的下发缓冲）。
using TranslateResultCallback =
    std::function<void(DWORD /*ipc_id*/, const std::string& /*result_utf8*/)>;

class TranslationService {
 public:
  TranslationService();
  ~TranslationService();

  // 初始化：从 weasel.yaml translate/ 段读配置；gate 为借用的隐私闸门。
  // 在 Initialize() 的 config_open("weasel") 之后调用（与 NetworkGate 同时）。
  void LoadFromConfig(RimeConfig* config, NetworkGate* gate);

  // 注册结果回调（Handler 在构造后立即设置，把译文送进 pending_aux）。
  void SetResultCallback(TranslateResultCallback cb) {
    on_result_ = std::move(cb);
  }

  // 划词翻译/手动触发：异步提交一次翻译，立即返回（不阻塞解码线程）。
  // text_utf8 为待译文本（数据最小化：调用方保证只传这一段，不带上下文）。
  // target_lang 留空则用配置默认。
  void Submit(DWORD ipc_id,
              const std::string& text_utf8,
              const std::string& target_lang = "");

  // 边写边译：由 _Respond 在每帧用当前预编辑串调用。
  // 仅当 auto_threshold>0、预编辑串长度>=阈值、且与上次提交内容不同时入队；
  // 否则直接返回（去抖，避免每帧刷屏请求）。
  void AutoTranslate(DWORD ipc_id, const std::string& preedit_utf8);

  // 当前配置（供设置页/调试查询，只读）。
  const TranslationConfig& config() const { return cfg_; }

 private:
  struct Job {
    DWORD ipc_id = 0;
    std::string text;
    std::string target_lang;
  };

  void WorkerLoop();
  void RunJob(const Job& job);

  // 投递结果（线程安全地调 on_result_）。空结果也投递，便于调用方决定静默。
  void Deliver(DWORD ipc_id, const std::string& result_utf8);

  // 云端翻译（Windows 真实联网；Linux 编译单元不参与）。
  // [NETWORK-GATE] 必须先过 Gate("translate", endpoint)。
  // 成功返回译文；失败/超时/Gate 拒绝返回空串。
  std::string TranslateCloud(const std::string& text_utf8,
                            const std::string& target_lang);

  // 从环境变量（或凭据管理器，见 P2-D）取 API key；key_ref 仅存变量名。
  // 取不到返回空串（TranslateCloud 据此拒绝请求，绝不使用空 key 静默联网）。
  std::string ResolveApiKey() const;

  TranslationConfig cfg_;
  NetworkGate* gate_ = nullptr;  // 借用，不持有
  TranslateResultCallback on_result_;

  // 单 worker 线程 + 任务队列。
  std::thread worker_;
  std::mutex mtx_;
  std::condition_variable cv_;
  std::queue<Job> queue_;
  bool shutting_down_ = false;

  // 边写边译去抖：每会话记录上次已提交的预编辑串。
  std::map<DWORD, std::string> last_auto_text_;
};

}  // namespace weasel
