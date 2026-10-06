// ============================================================================
// 青简（Qingjian）RimeWithWeasel.h —— P1-B1 完整替换版
//
// 本文件 = P0 完整替换版（weasel-p0-dev/src/RimeWithWeasel/RimeWithWeasel.h）
//          + P1-B1 追加块（见下方「===== P1-B1 追加 =====」标记）。
//
// P1-B1 追加内容：
//   1) 工具面板框架状态（标量成员，与 P1-B2 emoji 分支契约对齐）：
//      m_tool_type / m_tool_page / m_tool_entries / m_tool_comments。
//      工具模式不污染 librime session（不调用任何 rime_api 会话 API，
//      仅用 weasel 会话映射）。
//   2) ClipboardManager 持有（剪贴板历史缓存，见 ClipboardManager.h）。
// ============================================================================
#pragma once
#include <WeaselIPC.h>
#include <WeaselUI.h>
#include <map>
#include <memory>
#include <string>
#include <mutex>
#include <vector>

#include <rime_api.h>

#include "ClipboardManager.h"
#include "EmojiProvider.h"
#include "NetworkGate.h"
#include "TranslationService.h"
#include "VoiceInputService.h"


#include "CloudCandidateService.h"
#include "AIAssistant.h"


struct CaseInsensitiveCompare {
  bool operator()(const std::string& str1, const std::string& str2) const {
    std::string str1Lower, str2Lower;
    std::transform(str1.begin(), str1.end(), std::back_inserter(str1Lower),
                   [](char c) { return std::tolower(c); });
    std::transform(str2.begin(), str2.end(), std::back_inserter(str2Lower),
                   [](char c) { return std::tolower(c); });
    return str1Lower < str2Lower;
  }
};

typedef std::map<std::string, bool> AppOptions;
typedef std::map<std::string, AppOptions, CaseInsensitiveCompare>
    AppOptionsByAppName;

struct SessionStatus {
  SessionStatus() : style(weasel::UIStyle()), __synced(false), session_id(0) {
    RIME_STRUCT(RimeStatus, status);
  }
  weasel::UIStyle style;
  RimeStatus status;
  bool __synced;
  RimeSessionId session_id;
  // P2-A：待下发到 ctx.aux 的文本（P2-E 翻译/tips）。
  // _Respond 读取并经 ctx.aux=<文本> 下发；空串则不下发。
  std::wstring pending_aux;
  // P2-E：pending_aux 的写侧锁（worker 线程回写译文时持锁）。
  std::mutex aux_mtx;

  // P2-D：最近一次 AI 卡片结果快照（ai.assist 写入，ai.pick 回查）。
  std::vector<weasel::ExtraCard> pending_ai_cards;

};
typedef std::map<DWORD, SessionStatus> SessionStatusMap;
typedef DWORD WeaselSessionId;

// P2-B：NetworkGate -> IGateChecker 适配器（云候选模块 id 固定）。
class CloudGateAdapter : public weasel::IGateChecker {
 public:
  explicit CloudGateAdapter(weasel::NetworkGate* gate) : m_gate(gate) {}
  bool Allow(const std::string& module_id,
             const std::string& endpoint) const override {
    return m_gate && m_gate->Gate(module_id, endpoint);
  }
 private:
  weasel::NetworkGate* m_gate;  // 不拥有（P2-A 已由 Handler 持有）
};

class RimeWithWeaselHandler : public weasel::RequestHandler {
 public:
  RimeWithWeaselHandler(weasel::UI* ui);
  virtual ~RimeWithWeaselHandler();
  virtual void Initialize();
  virtual void Finalize();
  virtual DWORD FindSession(WeaselSessionId ipc_id);
  virtual DWORD AddSession(LPWSTR buffer, EatLine eat = 0);
  virtual DWORD RemoveSession(WeaselSessionId ipc_id);
  virtual BOOL ProcessKeyEvent(weasel::KeyEvent keyEvent,
                               WeaselSessionId ipc_id,
                               EatLine eat);
  virtual void CommitComposition(WeaselSessionId ipc_id);
  virtual void ClearComposition(WeaselSessionId ipc_id);
  virtual void SelectCandidateOnCurrentPage(size_t index,
                                            WeaselSessionId ipc_id);
  virtual bool HighlightCandidateOnCurrentPage(size_t index,
                                               WeaselSessionId ipc_id,
                                               EatLine eat);
  virtual bool ChangePage(bool backward, WeaselSessionId ipc_id, EatLine eat);
  virtual void FocusIn(DWORD param, WeaselSessionId ipc_id);
  virtual void FocusOut(DWORD param, WeaselSessionId ipc_id);
  virtual void UpdateInputPosition(RECT const& rc, WeaselSessionId ipc_id);
  virtual void StartMaintenance();
  virtual void EndMaintenance();
  virtual void SetOption(WeaselSessionId ipc_id,
                         const std::string& opt,
                         bool val);
  virtual void UpdateColorTheme(BOOL darkMode);
  // ===== P0 MVP 扩展通道覆写（最小握手子集） =====
  // 回写一行版本文本（server_info=weasel-<WEASEL_VERSION>+modern）。
  virtual void GetServerInfo(DWORD session_id, EatLine eat);
  // P0：message_id==L"ping" 回写 pong；L"server_info" 回写版本。
  // P1-B1 追加：tool.open / tool.pick / tool.cancel / tool.page 分支
  //           （见 RimeWithWeasel.p1_b1_patch.cpp）。
  virtual bool HandleExtension(const std::wstring& message_id,
                               const std::wstring& payload,
                               DWORD session_id,
                               EatLine eat);

  void OnUpdateUI(std::function<void()> const& cb);

 private:
  void _Setup();
  bool _IsDeployerRunning();
  void _UpdateUI(WeaselSessionId ipc_id);
  void _LoadSchemaSpecificSettings(WeaselSessionId ipc_id,
                                   const std::string& schema_id);
  void _LoadAppInlinePreeditSet(WeaselSessionId ipc_id,
                                bool ignore_app_name = false);
  bool _ShowMessage(weasel::Context& ctx, weasel::Status& status);
  bool _Respond(WeaselSessionId ipc_id, EatLine eat);
  void _ReadClientInfo(WeaselSessionId ipc_id, LPWSTR buffer);
  void _GetCandidateInfo(weasel::CandidateInfo& cinfo, RimeContext& ctx);
  void _GetStatus(weasel::Status& stat,
                  WeaselSessionId ipc_id,
                  weasel::Context& ctx);
  void _GetContext(weasel::Context& ctx, RimeSessionId session_id);
  void _UpdateShowNotifications(RimeConfig* config, bool initialize = false);

  void _UpdateInlinePreeditStatus(WeaselSessionId ipc_id);

  // ===== P1-B1 追加：工具面板回写助手 =====
  // 把当前工具面板快照（m_tool_entries/m_tool_page）序列化为标准响应行：
  //   action=status,ctx,config / status.composing=1 / config.inline_preedit=0
  //   ctx.cand=<boost text_woarchive 序列化 CandidateInfo>（label 1..9,0）
  //   tool.mode=<m_tool_type>
  // 走 eat 回写 TSF 端 DoEditSession 的 ResponseParser，行格式与 _Respond
  // 保持一致（escape_string / boost 序列化）。
  bool _SendToolPanel(WeaselSessionId ipc_id, EatLine eat);
  // 收起工具面板（回写 status.composing=0 + tool.mode=none）。
  void _CloseToolPanel(EatLine eat);
  // 清空工具面板状态（clipboard 与 emoji 共用，P1-B2 仅追加读写）。
  void _ClearToolState();

  RimeSessionId to_session_id(WeaselSessionId ipc_id) {
    return m_session_status_map[ipc_id].session_id;
  }
  SessionStatus& get_session_status(WeaselSessionId ipc_id) {
    return m_session_status_map[ipc_id];
  }
  SessionStatus& new_session_status(WeaselSessionId ipc_id) {
    return m_session_status_map.emplace(ipc_id, SessionStatus()).first->second;
  }

  AppOptionsByAppName m_app_options;
  weasel::UI* m_ui;  // reference
  DWORD m_active_session;
  bool m_disabled;
  std::string m_last_schema_id;
  std::string m_last_app_name;
  weasel::UIStyle m_base_style;
  std::map<std::string, bool> m_show_notifications;
  std::map<std::string, bool> m_show_notifications_base;
  std::function<void()> _UpdateUICallback;

  static void OnNotify(void* context_object,
                       uintptr_t session_id,
                       const char* message_type,
                       const char* message_value);
  static std::string m_message_type;
  static std::string m_message_value;
  static std::string m_message_label;
  static std::string m_option_name;
  static std::mutex m_notifier_mutex;
  SessionStatusMap m_session_status_map;
  bool m_current_dark_mode;
  bool m_global_ascii_mode;
  int m_show_notifications_time;
  DWORD m_pid;

  // ===== P1-B1 追加：工具面板框架状态（与 P1-B2 分支契约对齐） =====
  // 工具模式全程不触 librime session；面板候选完全由以下快照驱动。
  // m_tool_type 空串 = 非面板；L"clipboard" / L"emoji"。
  std::wstring m_tool_type;
  int m_tool_page = 0;
  std::vector<std::string> m_tool_entries;    // 面板候选上屏文本（UTF-8）
  std::vector<std::string> m_tool_comments;   // 候选注释（UTF-8，可空）
  // 剪贴板历史缓存（Initialize 启动 / Finalize 停止）。
  std::unique_ptr<weasel::ClipboardManager> m_clipboard;
  // ---- P1-B2 emoji 数据源（共享数据目录 data/emoji.txt） ----
  weasel::EmojiProvider m_emoji_provider;
  // ===== P2-A 追加：隐私/联网闸门（Initialize 装载，P2-B/C/D/E 共用） =====
  std::unique_ptr<weasel::NetworkGate> m_net_gate;
  // ===== P2-E：翻译服务（Initialize 装载，worker 线程异步翻译） =====
  std::unique_ptr<weasel::TranslationService> m_translate;
  // ===== P2-C：离线语音输入服务（本地零上传） =====
  std::unique_ptr<weasel::VoiceInputService> m_voice;


  // ===== P2-B 追加：云候选服务（Initialize 装载 cloud/ 段并 Start） =====
  std::unique_ptr<CloudGateAdapter> m_cloud_gate;
  std::unique_ptr<weasel::CloudCandidateService> m_cloud;
  // 云卡片 action_id 自增编号（选中回 cloud.pick 时回选用）。
  int m_cloud_action_id = 0;
  // ===== P2-D 追加：AI 助手 =====
  std::unique_ptr<weasel::AIAssistant> m_ai;


};
