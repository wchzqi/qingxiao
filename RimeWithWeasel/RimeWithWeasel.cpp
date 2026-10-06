#include "stdafx.h"
#include <logging.h>
#include <RimeWithWeasel.h>
#include <StringAlgorithm.hpp>
#include <WeaselConstants.h>
#include <WeaselUtility.h>

#include <filesystem>
#include <map>
#include <array>
#include <vector>
#include <regex>
#include <algorithm>
#include <rime_api.h>
#include <ClipboardManager.h>
#include <boost/archive/text_woarchive.hpp>
#include <sstream>
#include "EmojiProvider.h"
#include <NetworkGate.h>
#include "CloudCandidateService.h"





#define TRANSPARENT_COLOR 0x00000000
#define ARGB2ABGR(value)                                 \
  ((value & 0xff000000) | ((value & 0x000000ff) << 16) | \
   (value & 0x0000ff00) | ((value & 0x00ff0000) >> 16))
#define RGBA2ABGR(value)                                   \
  (((value & 0xff) << 24) | ((value & 0xff000000) >> 24) | \
   ((value & 0x00ff0000) >> 8) | ((value & 0x0000ff00) << 8))
typedef enum { COLOR_ABGR = 0, COLOR_ARGB, COLOR_RGBA } ColorFormat;

using namespace weasel;

static RimeApi* rime_api;
WeaselSessionId _GenerateNewWeaselSessionId(SessionStatusMap sm, DWORD pid) {
  if (sm.empty())
    return (WeaselSessionId)(pid + 1);
  return (WeaselSessionId)(sm.rbegin()->first + 1);
}

namespace {
// 面板分页大小：候选 label 1..9,0 共 10 条/页（基线 §2）。
// 定义置于文件前部：_HandleToolPick / _SendToolPanel 等多处先于旧定义位置使用。
constexpr size_t kToolPageSize = 10;
}  // namespace

int expand_ibus_modifier(int m) {
  return (m & 0xff) | ((m & 0xff00) << 16);
}

RimeWithWeaselHandler::RimeWithWeaselHandler(UI* ui)
    : m_ui(ui),
      m_active_session(0),
      m_disabled(true),
      m_current_dark_mode(false),
      m_global_ascii_mode(false),
      m_show_notifications_time(1200),
      _UpdateUICallback(NULL) {
  m_ui->InServer() = true;
  rime_api = rime_get_api();
  assert(rime_api);
  m_pid = GetCurrentProcessId();
  uint16_t msbit = 0;
  for (auto i = 31; i >= 0; i--) {
    if (m_pid & (1 << i)) {
      msbit = i;
      break;
    }
  }
  m_pid = (m_pid << (31 - msbit));
  _Setup();
}

RimeWithWeaselHandler::~RimeWithWeaselHandler() {
  m_show_notifications.clear();
  m_session_status_map.clear();
  m_app_options.clear();
}

bool add_session = false;
void _UpdateUIStyle(RimeConfig* config, UI* ui, bool initialize);
bool _UpdateUIStyleColor(RimeConfig* config,
                         UIStyle& style,
                         const std::string& color = std::string());
void _LoadAppOptions(RimeConfig* config, AppOptionsByAppName& app_options);

void _RefreshTrayIcon(const RimeSessionId session_id,
                      const std::function<void()> _UpdateUICallback) {
  // Dangerous, don't touch
  static char app_name[256] = {0};
  auto ret = rime_api->get_property(session_id, "client_app", app_name,
                                    sizeof(app_name) - 1);
  if (!ret || u8tow(app_name) == std::wstring(L"explorer.exe"))
    boost::thread th([=]() {
      ::Sleep(100);
      if (_UpdateUICallback)
        _UpdateUICallback();
    });
  else if (_UpdateUICallback)
    _UpdateUICallback();
}

void RimeWithWeaselHandler::_Setup() {
  RIME_STRUCT(RimeTraits, weasel_traits);
  std::string shared_dir = wtou8(WeaselSharedDataPath().wstring());
  std::string user_dir = wtou8(WeaselUserDataPath().wstring());
  weasel_traits.shared_data_dir = shared_dir.c_str();
  weasel_traits.user_data_dir = user_dir.c_str();
  weasel_traits.prebuilt_data_dir = weasel_traits.shared_data_dir;
  std::string distribution_name = wtou8(get_weasel_ime_name());
  weasel_traits.distribution_name = distribution_name.c_str();
  weasel_traits.distribution_code_name = WEASEL_CODE_NAME;
  weasel_traits.distribution_version = WEASEL_VERSION;
  weasel_traits.app_name = "rime.weasel";
  std::string log_dir = WeaselLogPath().u8string();
  weasel_traits.log_dir = log_dir.c_str();
  rime_api->setup(&weasel_traits);
  rime_api->set_notification_handler(&RimeWithWeaselHandler::OnNotify, this);
}

// ===== P2-D：AI 配置辅助（privacy 契约：key 不落 YAML 明文） =====
// 解析 ai/cloud_key_ref 指向的密钥：优先读环境变量 AI_CLOUD_KEY；
// Windows 生产环境建议改为 Credential Manager（CredRead），此处给环境变量位。
static std::string ResolveAiCloudKey() {
  char buf[512] = {0};
  DWORD n = GetEnvironmentVariableA("AI_CLOUD_KEY", buf, sizeof(buf) - 1);
  if (n == 0 || n >= sizeof(buf)) return "";  // 未设置 → 空，云端降级本地
  return std::string(buf, n);
}

// 解析 ai/actions 逗号分隔列表（如 "polish,translate"）→ AIActionType 向量。
// 空/缺键 → 空 vector（= 全部启用兜底，与 AIAssistant::ActionEnabled 约定一致）。
static std::vector<weasel::AIActionType> ParseAiActions(RimeConfig* config) {
  std::vector<weasel::AIActionType> out;
  char raw[512] = {0};
  if (!rime_api->config_get_string(config, "ai/actions", raw, sizeof(raw) - 1) || !raw[0])
    return out;  // 空 = 全部启用
  std::string s(raw);
  size_t pos = 0;
  while (pos <= s.size()) {
    size_t comma = s.find(',', pos);
    std::string tok = s.substr(pos, comma == std::string::npos ? std::string::npos
                                                               : comma - pos);
    size_t b = tok.find_first_not_of(" \t");
    size_t e = tok.find_last_not_of(" \t");
    if (b != std::string::npos) {
      tok = tok.substr(b, e - b + 1);
      if (!tok.empty()) {
        weasel::AIActionType a =
            weasel::AIActionFromString(u8tow(tok.c_str()));
        if (a != weasel::AIActionType::kUnknown) out.push_back(a);
      }
    }
    if (comma == std::string::npos) break;
    pos = comma + 1;
  }
  return out;
}

void RimeWithWeaselHandler::Initialize() {
  m_disabled = _IsDeployerRunning();
  if (m_disabled) {
    return;
  }

  LOG(INFO) << "Initializing la rime.";
  rime_api->initialize(NULL);
  if (rime_api->start_maintenance(/*full_check = */ False)) {
    m_disabled = true;
    rime_api->join_maintenance_thread();
  }

  // ===== P1-B1：剪贴板配置（定义于 config_open 块外，供块外启动监听使用） =====
  Bool clipboard_enabled = true;
  int clipboard_max = 50;

  RimeConfig config = {NULL};
  if (rime_api->config_open("weasel", &config)) {
    if (m_ui) {
      _UpdateUIStyle(&config, m_ui, true);
      _UpdateShowNotifications(&config, true);
      m_current_dark_mode = IsUserDarkMode();
      if (m_current_dark_mode) {
        const int BUF_SIZE = 255;
        char buffer[BUF_SIZE + 1] = {0};
        if (rime_api->config_get_string(&config, "style/color_scheme_dark",
                                        buffer, BUF_SIZE)) {
          std::string color_name(buffer);
          _UpdateUIStyleColor(&config, m_ui->style(), color_name);
        }
      }
      m_base_style = m_ui->style();
    }
    Bool global_ascii = false;
    if (rime_api->config_get_bool(&config, "global_ascii", &global_ascii))
      m_global_ascii_mode = !!global_ascii;
    if (!rime_api->config_get_int(&config, "show_notifications_time",
                                  &m_show_notifications_time))
      m_show_notifications_time = 1200;
    // ===== P1-B1：tool/clipboard/* 配置读取（基线 §5） =====
    // 未知键安全忽略；默认 enabled=true / max_entries=50。
    // GUI 设置中心（P1-B3 SettingsStore）落地前，此处即唯一读取点。
    rime_api->config_get_bool(&config, "tool/clipboard/enabled",
                              &clipboard_enabled);
    rime_api->config_get_int(&config, "tool/clipboard/max_entries",
                             &clipboard_max);

    // ===== P2：各联网/离线服务句柄统一初始化（懒构造） =====
    // 构造时 config 尚未打开，故在此 make_unique；后续 if(m_xxx) 块才成立。
    if (!m_net_gate) m_net_gate.reset(new weasel::NetworkGate());
    if (!m_ai) m_ai.reset(new weasel::AIAssistant());
    if (!m_translate) m_translate.reset(new weasel::TranslationService());
    if (!m_voice) m_voice.reset(new weasel::VoiceInputService());

    // ===== P2-A：隐私闸门从 weasel.yaml privacy/ 段读入 =====
    // 失败/缺键一律回退全关（本地优先）。m_net_gate 由 Handler 持有
    // （见 RimeWithWeasel.p2_a.h），供 P2-B/C/D/E 联网前 Gate 查询。
    if (m_net_gate) {
      m_net_gate->LoadFromConfig(&config);
      DLOG(INFO) << "NetworkGate loaded, master_enabled = "
                 << m_net_gate->MasterEnabled();
    }
    // ===== P2-D：AI 助手配置装载 + 接线隐私闸门 =====
    if (m_ai) {
      weasel::AIAssistant::Config aicfg;
      Bool ai_enabled = false;
      rime_api->config_get_bool(&config, "ai/enabled", &ai_enabled);
      aicfg.enabled = !!ai_enabled;

      char route_buf[256] = {0};
      rime_api->config_get_string(&config, "ai/route", route_buf, sizeof(route_buf) - 1);
      aicfg.route = u8tow(route_buf[0] ? route_buf : "local");

      char ep_buf[1024] = {0};
      rime_api->config_get_string(&config, "ai/cloud_endpoint", ep_buf, sizeof(ep_buf) - 1);
      aicfg.cloud_endpoint = ep_buf;

      // cloud_key：privacy 契约不落 YAML 明文——读环境变量 AI_CLOUD_KEY；
      // Windows 生产可改 Credential Manager（CredRead）。未设置 → 空，云端降级本地。
      aicfg.cloud_key = ResolveAiCloudKey();

      int timeout = 8000;
      rime_api->config_get_int(&config, "ai/timeout_ms", &timeout);
      aicfg.timeout_ms = timeout;

      // ai/actions 逗号分隔列表；空 = 全部启用（兜底）。
      aicfg.actions = ParseAiActions(&config);

      m_ai->Configure(aicfg);
      DLOG(INFO) << "AIAssistant configured, route = "
                 << wstring_to_string(aicfg.route);
    }
    // ===== P2-E：翻译服务从 weasel.yaml translate/ 段读入 =====
    // 与 NetworkGate 同段读 config；gate 借用 m_net_gate，供云端请求前 Gate。
    if (m_translate) {
      m_translate->LoadFromConfig(&config, m_net_gate.get());
      // 结果回调：worker 线程译完后，把译文写回对应会话的 pending_aux。
      // 空结果 = 无译文 -> 不清空 aux、不报错（无提示回退）。
      m_translate->SetResultCallback(
        [this](WeaselSessionId ipc, const std::string& result_u8) {
          if (result_u8.empty()) return;
          auto it = m_session_status_map.find(ipc);
          if (it == m_session_status_map.end()) return;
          std::lock_guard<std::mutex> lk(it->second.aux_mtx);
          it->second.pending_aux = u8tow(result_u8);
        });
    }
    // ===== P2-C：语音服务从 weasel.yaml voice/ 段读入配置 =====
    {
      wchar_t appbuf[MAX_PATH] = {0};
      DWORD alen = GetEnvironmentVariableW(L"APPDATA", appbuf, MAX_PATH);
      std::wstring model_dir = std::wstring(appbuf) + L"\\Rime\\models\\sherpa-zh";

      Bool voice_enabled = False;
      rime_api->config_get_bool(&config, "voice/enabled", &voice_enabled);
      if (voice_enabled && m_voice) {
        Bool vad_on = True;
        rime_api->config_get_bool(&config, "voice/vad_enabled", &vad_on);
        char model_path_buf[512] = {0};
        if (rime_api->config_get_string(&config, "voice/model_path",
                                         model_path_buf, sizeof(model_path_buf) - 1)
            && model_path_buf[0]) {
          model_dir = u8tow(model_path_buf);
        }
        m_voice->Configure(model_dir, !!vad_on, L"Ctrl+Alt+J");
      }
    }



    // ===== P2-B：装载 cloud/ 段，启动云候选服务（默认关） =====
    {
      weasel::CloudServiceConfig ccfg;
      Bool cloud_on = False;
      if (rime_api->config_get_bool(&config, "cloud/enabled", &cloud_on))
        ccfg.enabled = !!cloud_on;                     // 默认 false
      int threshold = 10;
      if (rime_api->config_get_int(&config, "cloud/threshold", &threshold) &&
          threshold > 0)
        ccfg.threshold = threshold;                    // 默认 10
      char buf[512] = {0};
      if (rime_api->config_get_string(&config, "cloud/source", buf,
                                      sizeof(buf) - 1))
        ccfg.source_id = buf;
      buf[0] = '\0';
      if (rime_api->config_get_string(&config, "cloud/custom_endpoint", buf,
                                      sizeof(buf) - 1))
        ccfg.custom_endpoint = buf;                    // 含 %PINYIN%
      int timeout_ms = 1500;
      if (rime_api->config_get_int(&config, "cloud/timeout_ms", &timeout_ms) &&
          timeout_ms > 0)
        ccfg.timeout_ms = timeout_ms;
      int cache_size = 128;
      if (rime_api->config_get_int(&config, "cloud/cache_size", &cache_size) &&
          cache_size > 0)
        ccfg.cache_size = cache_size;
      buf[0] = '\0';
      if (rime_api->config_get_string(&config, "cloud/proxy", buf,
                                      sizeof(buf) - 1))
        ccfg.proxy = buf;

      m_cloud_gate.reset(new CloudGateAdapter(m_net_gate.get()));
      m_cloud.reset(new weasel::CloudCandidateService(m_cloud_gate.get(),
                                                      nullptr /*WinHTTP 在 Windows 内置*/));
      m_cloud->Configure(ccfg);
      m_cloud->Start();   // enabled=false 时内部空操作（不起线程）
      DLOG(INFO) << "CloudCandidateService: enabled=" << ccfg.enabled
                 << " threshold=" << ccfg.threshold;
    }

    _LoadAppOptions(&config, m_app_options);
    rime_api->config_close(&config);
  }
  // ===== P1-B1：启动剪贴板监听（失败自动降级，不抛异常） =====
  if (clipboard_enabled && !m_clipboard) {
    m_clipboard.reset(new weasel::ClipboardManager());
    m_clipboard->Start(clipboard_max);
    DLOG(INFO) << "ClipboardManager started, max_entries = " << clipboard_max;
    m_emoji_provider.Load(weasel::EmojiProvider::DefaultDataPath());

  }

  m_last_schema_id.clear();
}

void RimeWithWeaselHandler::Finalize() {
  m_active_session = 0;
  m_disabled = true;
  m_session_status_map.clear();
  LOG(INFO) << "Finalizing la rime.";
  rime_api->finalize();
  // ===== P1-B1：停止剪贴板监听（join 消息线程，绝不 detach 遗留） =====
  _ClearToolState();
  if (m_clipboard) {
    m_clipboard->Stop();
  }
  if (m_cloud) m_cloud->Stop();


}

DWORD RimeWithWeaselHandler::FindSession(WeaselSessionId ipc_id) {
  if (m_disabled)
    return 0;
  Bool found = rime_api->find_session(to_session_id(ipc_id));
  DLOG(INFO) << "Find session: session_id = " << to_session_id(ipc_id)
             << ", found = " << found;
  return found ? (ipc_id) : 0;
}

DWORD RimeWithWeaselHandler::AddSession(LPWSTR buffer, EatLine eat) {
  if (m_disabled) {
    DLOG(INFO) << "Trying to resume service.";
    EndMaintenance();
    if (m_disabled)
      return 0;
  }
  RimeSessionId session_id = (RimeSessionId)rime_api->create_session();
  if (m_global_ascii_mode) {
    for (const auto& pair : m_session_status_map) {
      if (pair.first) {
        rime_api->set_option(session_id, "ascii_mode",
                             !!pair.second.status.is_ascii_mode);
        break;
      }
    }
  }

  WeaselSessionId ipc_id =
      _GenerateNewWeaselSessionId(m_session_status_map, m_pid);
  DLOG(INFO) << "Add session: created session_id = " << session_id
             << ", ipc_id = " << ipc_id;
  SessionStatus& session_status = new_session_status(ipc_id);
  session_status.style = m_base_style;
  session_status.session_id = session_id;
  _ReadClientInfo(ipc_id, buffer);

  RIME_STRUCT(RimeStatus, status);
  if (rime_api->get_status(session_id, &status)) {
    std::string schema_id = status.schema_id;
    m_last_schema_id = schema_id;
    _LoadSchemaSpecificSettings(ipc_id, schema_id);
    _LoadAppInlinePreeditSet(ipc_id, true);
    _UpdateInlinePreeditStatus(ipc_id);
    _RefreshTrayIcon(session_id, _UpdateUICallback);
    session_status.status = status;
    session_status.__synced = false;
    rime_api->free_status(&status);
  }
  m_ui->style() = session_status.style;
  // show session's welcome message :-) if any
  if (eat) {
    _Respond(ipc_id, eat);
  }
  add_session = true;
  _UpdateUI(ipc_id);
  add_session = false;
  m_active_session = ipc_id;
  return ipc_id;
}

DWORD RimeWithWeaselHandler::RemoveSession(WeaselSessionId ipc_id) {
  if (m_ui)
    m_ui->Hide();
  if (m_disabled)
    return 0;
  DLOG(INFO) << "Remove session: session_id = " << to_session_id(ipc_id);
  // TODO: force committing? otherwise current composition would be lost
  rime_api->destroy_session(to_session_id(ipc_id));
  m_session_status_map.erase(ipc_id);
  _ClearToolState();

  m_active_session = 0;
  return 0;
}

void RimeWithWeaselHandler::UpdateColorTheme(BOOL darkMode) {
  RimeConfig config = {NULL};
  if (rime_api->config_open("weasel", &config)) {
    if (m_ui) {
      _UpdateUIStyle(&config, m_ui, true);
      m_current_dark_mode = darkMode;
      if (darkMode) {
        const int BUF_SIZE = 255;
        char buffer[BUF_SIZE + 1] = {0};
        if (rime_api->config_get_string(&config, "style/color_scheme_dark",
                                        buffer, BUF_SIZE)) {
          std::string color_name(buffer);
          _UpdateUIStyleColor(&config, m_ui->style(), color_name);
        }
      }
      m_base_style = m_ui->style();
    }
    rime_api->config_close(&config);
  }

  for (auto& pair : m_session_status_map) {
    RIME_STRUCT(RimeStatus, status);
    if (rime_api->get_status(to_session_id(pair.first), &status)) {
      _LoadSchemaSpecificSettings(pair.first, std::string(status.schema_id));
      _LoadAppInlinePreeditSet(pair.first, true);
      _UpdateInlinePreeditStatus(pair.first);
      pair.second.status = status;
      pair.second.__synced = false;
      rime_api->free_status(&status);
    }
  }
  m_ui->style() = get_session_status(m_active_session).style;
}

BOOL RimeWithWeaselHandler::ProcessKeyEvent(KeyEvent keyEvent,
                                            WeaselSessionId ipc_id,
                                            EatLine eat) {
  DLOG(INFO) << "Process key event: keycode = " << keyEvent.keycode
             << ", mask = " << keyEvent.mask << ", ipc_id = " << ipc_id;
  if (m_disabled)
    return FALSE;
  // P2-C：voice 识别结果上屏（worker 线程写入 pending，本按键消费并 commit）。
  {
    SessionStatus& sst = get_session_status(ipc_id);
    std::wstring voice_text;
    {
      std::lock_guard<std::mutex> lk(sst.aux_mtx);
      voice_text.swap(sst.pending_voice_commit);
    }
    if (!voice_text.empty()) {
      std::wstring body;
      body.append(L"action=commit,status\n");
      body.append(L"commit=").append(escape_string(voice_text)).append(L"\n");
      body.append(L"status.composing=0\n.\n");
      eat(body);
      _UpdateUI(ipc_id);
      m_active_session = ipc_id;
      return TRUE;
    }
  }
  RimeSessionId session_id = to_session_id(ipc_id);
  Bool handled = rime_api->process_key(session_id, keyEvent.keycode,
                                       expand_ibus_modifier(keyEvent.mask));
  // vim_mode when keydown only
  if (!handled && !(keyEvent.mask & ibus::Modifier::RELEASE_MASK)) {
    bool isVimBackInCommandMode =
        (keyEvent.keycode == ibus::Keycode::Escape) ||
        ((keyEvent.mask & (1 << 2)) &&
         (keyEvent.keycode == ibus::Keycode::XK_c ||
          keyEvent.keycode == ibus::Keycode::XK_C ||
          keyEvent.keycode == ibus::Keycode::XK_bracketleft));
    if (isVimBackInCommandMode &&
        rime_api->get_option(session_id, "vim_mode") &&
        !rime_api->get_option(session_id, "ascii_mode")) {
      rime_api->set_option(session_id, "ascii_mode", True);
    }
  }
  _Respond(ipc_id, eat);
  _UpdateUI(ipc_id);
  m_active_session = ipc_id;
  return (BOOL)handled;
}

void RimeWithWeaselHandler::CommitComposition(WeaselSessionId ipc_id) {
  DLOG(INFO) << "Commit composition: ipc_id = " << ipc_id;
  if (m_disabled)
    return;
  rime_api->commit_composition(to_session_id(ipc_id));
  _UpdateUI(ipc_id);
  m_active_session = ipc_id;
}

void RimeWithWeaselHandler::ClearComposition(WeaselSessionId ipc_id) {
  DLOG(INFO) << "Clear composition: ipc_id = " << ipc_id;
  if (m_disabled)
    return;
  rime_api->clear_composition(to_session_id(ipc_id));
  _UpdateUI(ipc_id);
  m_active_session = ipc_id;
}

void RimeWithWeaselHandler::SelectCandidateOnCurrentPage(
    size_t index,
    WeaselSessionId ipc_id) {
  DLOG(INFO) << "select candidate on current page, ipc_id = " << ipc_id
             << ", index = " << index;
  if (m_disabled)
    return;
  rime_api->select_candidate_on_current_page(to_session_id(ipc_id), index);
}

bool RimeWithWeaselHandler::HighlightCandidateOnCurrentPage(
    size_t index,
    WeaselSessionId ipc_id,
    EatLine eat) {
  DLOG(INFO) << "highlight candidate on current page, ipc_id = " << ipc_id
             << ", index = " << index;
  bool res = rime_api->highlight_candidate_on_current_page(
      to_session_id(ipc_id), index);
  _Respond(ipc_id, eat);
  _UpdateUI(ipc_id);
  return res;
}

bool RimeWithWeaselHandler::ChangePage(bool backward,
                                       WeaselSessionId ipc_id,
                                       EatLine eat) {
  DLOG(INFO) << "change page, ipc_id = " << ipc_id
             << (backward ? "backward" : "foreward");
  bool res = rime_api->change_page(to_session_id(ipc_id), backward);
  _Respond(ipc_id, eat);
  _UpdateUI(ipc_id);
  return res;
}

void RimeWithWeaselHandler::GetServerInfo(WeaselSessionId ipc_id, EatLine eat) {
  // P0 握手：回写一行版本 + 布局能力标记。
  // 版本直接复用仓库现成宏 WEASEL_VERSION（WeaselConstants.h，经
  // VERSION_STR 展开为窄字符串字面量，如 "0.15.0"），不另设版本常量。
  // "+modern" 声明本服务端支持 UIStyle::LAYOUT_MODERN 候选窗布局。
  // 该行是静态文本，不依赖 rime 会话，m_disabled 时也能应答。
  DLOG(INFO) << "GetServerInfo: ipc_id = " << ipc_id;
  if (!eat)
    return;
  std::wstring line = L"server_info=weasel-";
  line += u8tow(WEASEL_VERSION);
  line += L"+modern\n.\n";
  eat(line);
}

bool RimeWithWeaselHandler::HandleExtension(const std::wstring& message_id,
                                            const std::wstring& payload,
                                            WeaselSessionId ipc_id,
                                            EatLine eat) {
  DLOG(INFO) << "HandleExtension: message_id = " << message_id
             << ", ipc_id = " << ipc_id;
  if (!eat)
    return false;

  // P0 最小握手子集：仅 ping / server_info。
  if (message_id == L"ping") {
    std::wstring resp = L"extension.pong=pong\n.\n";
    eat(resp);
    return true;
  }
  if (message_id == L"server_info") {
    std::wstring line = L"extension.server_info=weasel-";
    line += u8tow(WEASEL_VERSION);
    line += L"+modern\n.\n";
    eat(line);
    return true;
  }
  // ===== P1-B1：tool.* 工具面板分支（剪贴板历史） =====
  // 注意：工具模式不调用任何 rime_api 会话 API（ProcessKeyEvent /
  // SelectCandidateOnCurrentPage 均不进入），仅通过 eat 回写标准响应行
  // 驱动 TSF 端候选窗与上屏。
  if (message_id == L"tool.open") {
    // payload: type=clipboard | type=emoji
    std::wstring type;
    {
      static const std::wstring kType = L"type=";
      if (payload.compare(0, kType.size(), kType) == 0)
        type = payload.substr(kType.size());
    }
    DLOG(INFO) << "tool.open: type = " << type.c_str()
               << ", ipc_id = " << ipc_id;
    if (type == L"clipboard") {
      _ClearToolState();
      m_tool_type = L"clipboard";
      m_tool_page = 0;
      // History() 内部已做“监听失败降级”：未监听时即时读当前剪贴板置顶。
      std::vector<std::wstring> hist =
          m_clipboard ? m_clipboard->History(0) : std::vector<std::wstring>();
      m_tool_entries.reserve(hist.size());
      for (const auto& w : hist) {
        m_tool_entries.push_back(wtou8(w));
      }
      return _SendToolPanel(ipc_id, eat);
    }
    // >>> P1-B2 emoji panel open anchor >>>
    // B2 块 1 在以下分支位置贴入：type==L"emoji" 时经 EmojiProvider
    // 填充 m_tool_type/m_tool_page/m_tool_entries/m_tool_comments。
    // P1-B1 当前：emoji 数据未就绪，暂 not-supported（TSF 端收到 false
    // 即清模式，见 WeaselTSF.p1_b1_patch.cpp）。
    if (type == L"emoji") {
      DLOG(INFO) << "tool.open: emoji, ipc_id = " << ipc_id;
      _ClearToolState();
      // 数据未就绪：安全收起面板（返回 true，TSF 端自行结束，不 not-supported）。
      if (!m_emoji_provider.loaded()) {
        _CloseToolPanel(eat);
        return true;
      }
      std::vector<std::string> cats = m_emoji_provider.Categories();
      if (cats.empty()) {
        _CloseToolPanel(eat);
        return true;
      }
      // P1 无分类切换键：固定首个分类，把整类条目平铺进 m_tool_entries，
      // 由 B1 _SendToolPanel 按 kToolPageSize=10 分页（与 clipboard 历史同构）。
      const std::string cat = cats.front();
      const size_t total = m_emoji_provider.CategorySize(cat);
      std::vector<weasel::EmojiEntry> all =
          m_emoji_provider.Page(cat, /*page=*/0, total ? total : 1);

      m_tool_type = L"emoji";
      m_tool_page = 0;
      m_tool_entries.clear();
      m_tool_comments.clear();
      m_tool_entries.reserve(all.size());
      for (const auto& e : all) {
        // provider 内部即 UTF-8 窄串；_SendToolPanel 内部统一 u8tow()，
        // 此处直接 push UTF-8，不再转宽串。
        m_tool_entries.push_back(e.text);
        m_tool_comments.push_back(e.keywords);  // 注释可空，与 entries 等长
      }
      return _SendToolPanel(ipc_id, eat);

      return false;
    }
    return false;
  }

  if (message_id == L"tool.pick") {
    // payload: index=N（0..9，本页第 N 条；0 表示第 10 条）
    int index = -1;
    {
      static const std::wstring kIdx = L"index=";
      if (payload.compare(0, kIdx.size(), kIdx) == 0)
        index = _wtoi(payload.substr(kIdx.size()).c_str());
    }
    if (index < 0 || (int)index >= (int)kToolPageSize) {
      return false;
    }
    // >>> P1-B2 emoji panel pick anchor >>>
    // B2 块 2 在以下位置贴入：m_tool_type==L"emoji" 时直接回写 commit= 行
    // 并 _ClearToolState()、return true。
    if (m_tool_type == L"clipboard") {
      const size_t global =
          (size_t)m_tool_page * kToolPageSize + (size_t)index;
      if (global >= m_tool_entries.size()) {
        // 页码漂移（历史已变）：收起面板。
        _ClearToolState();
        _CloseToolPanel(eat);
        return true;
      }
      const std::string& utf8_text = m_tool_entries[global];
      // 剪贴板：回写系统剪贴板置顶（基线 §2/§4 Promote 语义）。
      if (m_clipboard) {
        m_clipboard->Promote(u8tow(utf8_text));
      }
      DLOG(INFO) << "tool.pick: index = " << index
                 << ", ipc_id = " << ipc_id;
      // 上屏：直接回写 commit= 行（TSF DoEditSession 经 _InsertText 插入），
      // 与 _Respond 的 commit 路径（基线 line 750-755）格式一致。
      std::wstring body;
      body.append(L"action=commit,status\n");
      body.append(L"commit=").append(escape_string(u8tow(utf8_text))).append(L"\n");
      body.append(L"status.composing=0\n");
      body.append(L"tool.mode=none\n.\n");
      _ClearToolState();
      eat(body);
      return true;
    }
    if (m_tool_type == L"emoji") {
      const size_t global =
          (size_t)m_tool_page * kToolPageSize + (size_t)index;
      if (global >= m_tool_entries.size()) {
        // 页码漂移：收起面板。
        _ClearToolState();
        _CloseToolPanel(eat);
        return true;
      }
      const std::string& utf8_text = m_tool_entries[global];
      DLOG(INFO) << "tool.pick(emoji): index = " << index
                 << ", ipc_id = " << ipc_id;
      std::wstring body;
      body.append(L"action=commit,status\n");
      body.append(L"commit=")
          .append(escape_string(u8tow(utf8_text)))
          .append(L"\n");
      body.append(L"status.composing=0\n");
      body.append(L"tool.mode=none\n.\n");
      _ClearToolState();
      eat(body);
      return true;
    }
    return false;
  }

  if (message_id == L"tool.cancel") {
    DLOG(INFO) << "tool.cancel: ipc_id = " << ipc_id;
    _ClearToolState();
    _CloseToolPanel(eat);
    return true;
  }

  if (message_id == L"tool.page") {
    if (m_tool_type.empty()) {
      return false;
    }
    // >>> P1-B2 emoji panel page anchor >>>
    // B2 块 3 在以下位置贴入：m_tool_type==L"emoji" 时按 EmojiProvider
    // 在分类内翻页并重填 m_tool_entries。
    if (m_tool_type == L"clipboard") {
      if (payload != L"backward" && payload != L"forward") {
        return false;
      }
      const int total = (int)m_tool_entries.size();
      const int total_pages = (total + (int)kToolPageSize - 1) / (int)kToolPageSize;
      if (payload == L"forward") {
        m_tool_page =
            std::min(m_tool_page + 1, std::max(0, total_pages - 1));
      } else {
        m_tool_page = std::max(m_tool_page - 1, 0);
      }
      DLOG(INFO) << "tool.page: " << payload.c_str()
                 << ", page = " << m_tool_page;
    if (m_tool_type == L"emoji") {
      if (payload != L"backward" && payload != L"forward") {
        return false;
      }
      const int total = (int)m_tool_entries.size();
      const int total_pages = (total + (int)kToolPageSize - 1) / (int)kToolPageSize;
      if (payload == L"forward") {
        m_tool_page =
            std::min(m_tool_page + 1, std::max(0, total_pages - 1));
      } else {
        m_tool_page = std::max(m_tool_page - 1, 0);
      }
      DLOG(INFO) << "tool.page(emoji): " << payload.c_str()
                 << ", page = " << m_tool_page;
      return _SendToolPanel(ipc_id, eat);
    }

      return _SendToolPanel(ipc_id, eat);
    }
    return false;
  }

  // ===== P2-A：privacy.status / privacy.summary 分支 =====
  // 回写当前隐私开关全景，供 TSF/Deployer 在 UI 展示「隐私模式状态」。
  // 回写行协议（eat，行尾 \n.\n，与 P0 ping 分支同范式）：
  //   privacy.master=on|off
  //   privacy.module.<id>=on|off        （逐模块一行）
  //   privacy.summary=<一行中文摘要>
  if (message_id == L"privacy.status" || message_id == L"privacy.summary") {
    if (!m_net_gate) {
      return false;
    }
    std::wstring resp;
    resp += L"privacy.master=";
    resp += m_net_gate->MasterEnabled() ? L"on" : L"off";
    resp += L"\n";
    for (const auto& m : m_net_gate->Modules()) {
      resp += L"privacy.module.";
      resp += u8tow(m.id);
      resp += m.enabled ? L"=on\n" : L"=off\n";
    }
    resp += L"privacy.summary=";
    resp += m_net_gate->GetPrivacySummary();
    resp += L"\n.\n";
    eat(resp);
    DLOG(INFO) << "privacy.status: master = "
               << m_net_gate->MasterEnabled();
    return true;
  }

  // ===== P2-B：cloud.query / cloud.pick / cloud.cache_clear 分支 =====
  if (message_id == L"cloud.query") {
    // payload: pinyin=<拼音键>  —— TSF 主动刷新（调试 / 结果就绪后拉取）。
    std::wstring pinyin_w;
    {
      static const std::wstring kPre = L"pinyin=";
      if (payload.compare(0, kPre.size(), kPre) == 0)
        pinyin_w = payload.substr(kPre.size());
    }
    if (m_cloud) {
      m_cloud->Invalidate(wtou8(pinyin_w));  // 强制重发
    }
    return true;  // 下一帧 _Respond 即带最新卡片
  }
  if (message_id == L"cloud.pick") {
    // payload: pinyin=<拼音键>  —— 用户选中云卡片后回报，用于 LRU 提升。
    // 真正上屏文本 card.action 由 TSF 直接 commit（不经 librime）。
    std::wstring pinyin_w;
    {
      static const std::wstring kPre = L"pinyin=";
      if (payload.compare(0, kPre.size(), kPre) == 0)
        pinyin_w = payload.substr(kPre.size());
    }
    if (m_cloud)
      m_cloud->Promote(wtou8(pinyin_w));
    return true;
  }
  if (message_id == L"cloud.cache_clear") {
    if (m_cloud)
      m_cloud->ClearCache();
    return true;
  }

  // ===== P2-D：ai.assist —— AI 助手唤起 =====
  // payload: action_type=polish|continue|translate|proofread[&text=...]
  //   text 可缺省：缺省时服务端从当前 librime preedit 兜底取。
  if (message_id == L"ai.assist") {
    // 解析 action_type
    std::wstring action_s;
    std::wstring payload_text;
    // 极简键值解析（payload 用 '&' 分隔，value 内不允许 '='）：
    {
      size_t pos = 0;
      while (pos < payload.size()) {
        size_t amp = payload.find(L'&', pos);
        std::wstring kv = payload.substr(pos, amp == std::wstring::npos
                                                   ? std::wstring::npos
                                                   : amp - pos);
        size_t eq = kv.find(L'=');
        if (eq != std::wstring::npos) {
          std::wstring k = kv.substr(0, eq);
          std::wstring v = kv.substr(eq + 1);
          if (k == L"action_type") action_s = v;
          else if (k == L"text") payload_text = v;
        }
        if (amp == std::wstring::npos) break;
        pos = amp + 1;
      }
    }
    AIActionType act = AIActionFromString(action_s);
    if (act == AIActionType::kUnknown) return false;

    // text 缺省 → 从当前 librime context 取 preedit/commit preview。
    std::wstring text = payload_text;
    if (text.empty()) {
      RIME_STRUCT(RimeContext, ctx);
      RimeSessionId sid = get_session_status(ipc_id).session_id;
      if (rime_api->get_context(sid, &ctx)) {
        if (ctx.commit_text_preview) text = u8tow(ctx.commit_text_preview);
        else if (ctx.composition.preedit) text = u8tow(ctx.composition.preedit);
        rime_api->free_context(&ctx);
      }
    }

    std::vector<ExtraCard> cards = m_ai->HandleRequest(text, act);
    SessionStatus& sst = get_session_status(ipc_id);
    sst.pending_ai_cards = cards;  // 快照，供 ai.pick 回查

    // 无卡（未启用/空文本/动作未开）→ not-supported，TSF 静默降级。
    if (cards.empty()) return false;

    // 直接构造 CandidateInfo 下发：复用当前 librime 候选（不改序）+ extra_cards。
    CandidateInfo cinfo;
    RIME_STRUCT(RimeContext, ctx);
    RimeSessionId sid = get_session_status(ipc_id).session_id;
    if (rime_api->get_context(sid, &ctx)) {
      if (ctx.menu.num_candidates > 0) _GetCandidateInfo(cinfo, ctx);
      rime_api->free_context(&ctx);
    }
    for (const auto& c : cards) cinfo.extra_cards.push_back(c);

    std::wstring body;
    body.append(L"action=status,ctx,config\n");
    body.append(L"status.composing=1\n");
    body.append(L"config.inline_preedit=0\n");
    body.append(L"ai.mode=assisting\n");  // parser 无 ai 反序列化器，安全忽略
    {
      std::wstringstream ss;
      boost::archive::text_woarchive oa(ss);
      oa << cinfo;
      body.append(L"ctx.cand=").append(ss.str()).append(L"\n");
    }
    body.append(L".\n");
    return eat(body);
  }

  // ===== P2-D：ai.pick —— 选中某条 AI 建议上屏 =====
  // payload: action_id=<n>&item=<m>
  if (message_id == L"ai.pick") {
    int action_id = -1, item = 0;
    {
      size_t pos = 0;
      while (pos < payload.size()) {
        size_t amp = payload.find(L'&', pos);
        std::wstring kv = payload.substr(pos, amp == std::wstring::npos
                                                   ? std::wstring::npos
                                                   : amp - pos);
        size_t eq = kv.find(L'=');
        if (eq != std::wstring::npos) {
          std::wstring k = kv.substr(0, eq);
          std::wstring v = kv.substr(eq + 1);
          if (k == L"action_id") action_id = _wtoi(v.c_str());
          else if (k == L"item") item = _wtoi(v.c_str());
        }
        if (amp == std::wstring::npos) break;
        pos = amp + 1;
      }
    }
    SessionStatus& sst = get_session_status(ipc_id);
    std::wstring chosen =
        AIAssistant::PickItem(sst.pending_ai_cards, action_id, item);
    if (chosen.empty()) {
      sst.pending_ai_cards.clear();
      return false;
    }
    // 上屏：复用 _Respond commit 路径格式（基线 line 750-755）。
    std::wstring body;
    body.append(L"action=commit,status\n");
    body.append(L"commit=").append(escape_string(chosen)).append(L"\n");
    body.append(L"status.composing=0\n");
    body.append(L"ai.mode=none\n.\n");
    sst.pending_ai_cards.clear();  // 一次消费后清快照，不保留交互
    return eat(body);
  }

  // ===== P2-E：translate.request 分支（划词翻译/手动触发） =====
  // payload（键值文本行，与 P1 tool.* 同范式）：
  //   text=<待译文本(UTF-8, 需 TSF 侧做 %xx 转义)>
  //   target_lang=en           （可省，省则用 translate/target_lang）
  // 语义：服务端立即接受并异步翻译；译文经 pending_aux -> ctx.aux= 下发。
  if (message_id == L"translate.request") {
    std::wstring text_w, lang_w;
    std::wstring p = payload;
    auto get_param = [](const std::wstring& s, const wchar_t* key) -> std::wstring {
      std::wstring k = key;
      size_t pos = s.find(k);
      if (pos == std::wstring::npos) return L"";
      pos += k.size();
      size_t amp = s.find(L'&', pos);
      return s.substr(pos, amp == std::wstring::npos ? amp : amp - pos);
    };
    text_w = get_param(p, L"text=");
    lang_w = get_param(p, L"target_lang=");
    if (text_w.empty()) {
      std::wstring empty_msg(L"translate.ok=0\n.\n");
      eat(empty_msg);
      return true;  // 空文本：静默接受，不译
    }
    // wstring -> utf8 交给服务端翻译（数据最小化：只传这一段文本）。
    std::string text_u8 = wtou8(text_w);
    std::string lang_u8 = lang_w.empty() ? std::string() : wtou8(lang_w);
    if (m_translate) {
      m_translate->Submit(ipc_id, text_u8, lang_u8);
    }
    // 立即回执（译文稍后经 ctx.aux= 异步到候选窗上方，非本响应体内）。
    std::wstring msg983(L"translate.ok=1\n.\n"); eat(msg983);
    return true;
  }

  // ===== P2-C：voice.start / voice.stop / voice.status 分支 =====
  // 离线语音本地零上传，不过 NetworkGate（识别本身不联网）。
  if (message_id == L"voice.start") {
    if (!m_voice) {
      return false;  // 未配置 / 模型缺失
    }
    bool ok = m_voice->Start(
      [this](const std::string& text_utf8) {
        // 识别结果回调：写入 pending_voice_commit，由主线程按键时经
        // weasel 既有 commit 消息链路上屏（librime 无公开的提交指定文本 API）。
        if (!text_utf8.empty()) {
          SessionStatus& sst = get_session_status(m_active_session);
          std::lock_guard<std::mutex> lk(sst.aux_mtx);
          sst.pending_voice_commit = u8tow(text_utf8);
        }
      });
    if (!ok) {
      std::wstring msg1002(L"voice.error=no_microphone_or_model\n.\n"); eat(msg1002);
    } else {
      std::wstring msg1004(L"voice.state=recording\n.\n"); eat(msg1004);
    }
    return true;
  }

  if (message_id == L"voice.stop") {
    if (m_voice) {
      m_voice->Stop();
    }
    std::wstring msg1013(L"voice.state=idle\n.\n"); eat(msg1013);
    return true;
  }

  if (message_id == L"voice.status") {
    if (!m_voice) {
      std::wstring msg1019(L"voice.state=unavailable\nvoice.model=not_configured\n.\n"); eat(msg1019);
      return true;
    }
    std::wstring resp = L"voice.state=";
    switch (m_voice->GetState()) {
      case VoiceState::IDLE:         resp += L"idle"; break;
      case VoiceState::RECORDING:    resp += L"recording"; break;
      case VoiceState::RECOGNIZING:  resp += L"recognizing"; break;
      case VoiceState::ERROR_NOMIC:  resp += L"error_nomic"; break;
      case VoiceState::ERROR_NOMODEL:resp += L"error_nomodel"; break;
      case VoiceState::ERROR_FAIL:    resp += L"error_fail"; break;
    }
    resp += L"\nvoice.model=";
    resp += m_voice->IsModelReady() ? L"ready" : L"missing";
    resp += L"\n.\n";
    eat(resp);
    return true;
  }

  // P1 云候选 / P2 AI / 设置等 message_id 一律 not-supported。
  return false;
}


void RimeWithWeaselHandler::_ClearToolState() {
  m_tool_type.clear();
  m_tool_page = 0;
  m_tool_entries.clear();
  m_tool_comments.clear();
}

bool RimeWithWeaselHandler::_SendToolPanel(WeaselSessionId ipc_id,
                                           EatLine eat) {
  if (!eat) {
    return false;
  }
  if (m_tool_entries.empty()) {
    _CloseToolPanel(eat);
    return true;
  }
  const int total = (int)m_tool_entries.size();
  const int total_pages = (total + (int)kToolPageSize - 1) / (int)kToolPageSize;
  int page = m_tool_page;
  if (page < 0) {
    page = 0;
  }
  if (page >= total_pages) {
    page = total_pages - 1;
  }
  const size_t begin = (size_t)page * kToolPageSize;
  const size_t end = std::min(begin + kToolPageSize, (size_t)total);

  // 直接构造 Context.cinfo（Text 列表 + label 1..9,0），不经过 librime。
  CandidateInfo cinfo;
  for (size_t i = begin; i < end; ++i) {
    cinfo.candies.emplace_back(Text(escape_string(u8tow(m_tool_entries[i]))));
    // label 与 _GetCandidateInfo 兜底分支一致：(i+1)%10（1..9,0）
    cinfo.labels.emplace_back(Text(std::to_wstring((i - begin + 1) % 10)));
    if (i < m_tool_comments.size() && !m_tool_comments[i].empty()) {
      cinfo.comments.emplace_back(
          Text(escape_string(u8tow(m_tool_comments[i]))));
    } else {
      cinfo.comments.emplace_back(Text());
    }
  }
  cinfo.highlighted = 0;
  cinfo.currentPage = page;
  cinfo.totalPages = total_pages;
  cinfo.is_last_page = (page >= total_pages - 1);

  std::wstring body;
  body.append(L"action=status,ctx,config\n");
  body.append(L"status.composing=1\n");
  body.append(L"status.ascii_mode=0\n");
  body.append(L"config.inline_preedit=0\n");
  // 基线 §3：回写 tool.mode=<type>。ResponseParser 无 "tool" 反序列化器，
  // 该行被安全忽略，仅作协议回显。
  body.append(L"tool.mode=")
      .append(m_tool_type.empty() ? L"none" : m_tool_type)
      .append(L"\n");
  {
    std::wstringstream ss;
    boost::archive::text_woarchive oa(ss);
    oa << cinfo;
    body.append(L"ctx.cand=").append(ss.str()).append(L"\n");
  }
  body.append(L".\n");
  return eat(body);
}

void RimeWithWeaselHandler::_CloseToolPanel(EatLine eat) {
  if (!eat) {
    return;
  }
  eat(std::wstring(L"action=status\n"
                   L"status.composing=0\n"
                   L"tool.mode=none\n"
                   L".\n"));
}

void RimeWithWeaselHandler::FocusIn(DWORD client_caps, WeaselSessionId ipc_id) {
  DLOG(INFO) << "Focus in: ipc_id = " << ipc_id
             << ", client_caps = " << client_caps;
  if (m_disabled)
    return;
  _UpdateUI(ipc_id);
  m_active_session = ipc_id;
}

void RimeWithWeaselHandler::FocusOut(DWORD param, WeaselSessionId ipc_id) {
  DLOG(INFO) << "Focus out: ipc_id = " << ipc_id;
  if (m_ui)
    m_ui->Hide();
  m_active_session = 0;
}

void RimeWithWeaselHandler::UpdateInputPosition(RECT const& rc,
                                                WeaselSessionId ipc_id) {
  DLOG(INFO) << "Update input position: (" << rc.left << ", " << rc.top
             << "), ipc_id = " << ipc_id
             << ", m_active_session = " << m_active_session;
  if (m_ui)
    m_ui->UpdateInputPosition(rc);
  if (m_disabled)
    return;
  if (m_active_session != ipc_id) {
    _UpdateUI(ipc_id);
    m_active_session = ipc_id;
  }
}

std::string RimeWithWeaselHandler::m_message_type;
std::string RimeWithWeaselHandler::m_message_value;
std::string RimeWithWeaselHandler::m_message_label;
std::string RimeWithWeaselHandler::m_option_name;
std::mutex RimeWithWeaselHandler::m_notifier_mutex;

void RimeWithWeaselHandler::OnNotify(void* context_object,
                                     uintptr_t session_id,
                                     const char* message_type,
                                     const char* message_value) {
  // may be running in a thread when deploying rime
  RimeWithWeaselHandler* self =
      reinterpret_cast<RimeWithWeaselHandler*>(context_object);
  if (!self || !message_type || !message_value)
    return;
  std::lock_guard<std::mutex> lock(m_notifier_mutex);
  m_message_type = message_type;
  m_message_value = message_value;
  if (RIME_API_AVAILABLE(rime_api, get_state_label) &&
      !strcmp(message_type, "option")) {
    Bool state = message_value[0] != '!';
    const char* option_name = message_value + !state;
    m_option_name = option_name;
    const char* state_label =
        rime_api->get_state_label(session_id, option_name, state);
    if (state_label) {
      m_message_label = std::string(state_label);
    }
  }
}

void RimeWithWeaselHandler::_ReadClientInfo(WeaselSessionId ipc_id,
                                            LPWSTR buffer) {
  std::string app_name;
  // parse request text
  wbufferstream bs(buffer, WEASEL_IPC_BUFFER_LENGTH);
  std::wstring line;
  while (bs.good()) {
    std::getline(bs, line);
    if (!bs.good())
      break;
    // file ends
    if (line == L".")
      break;
    const std::wstring kClientAppKey = L"session.client_app=";
    if (starts_with(line, kClientAppKey)) {
      std::wstring lwr = line;
      to_lower(lwr);
      app_name = wtou8(lwr.substr(kClientAppKey.length()));
    }
  }
  SessionStatus& session_status = get_session_status(ipc_id);
  RimeSessionId session_id = session_status.session_id;
  // set app specific options
  if (!app_name.empty()) {
    rime_api->set_property(session_id, "client_app", app_name.c_str());

    auto it = m_app_options.find(app_name);
    if (it != m_app_options.end()) {
      AppOptions& options(m_app_options[it->first]);
      for (const auto& pair : options) {
        DLOG(INFO) << "set app option: " << pair.first << " = " << pair.second;
        rime_api->set_option(session_id, pair.first.c_str(), Bool(pair.second));
      }
    }
  }
  // inline preedit
  bool inline_preedit = session_status.style.inline_preedit;
  rime_api->set_option(session_id, "inline_preedit", Bool(inline_preedit));
  // show soft cursor on weasel panel but not inline
  rime_api->set_option(session_id, "soft_cursor", Bool(!inline_preedit));
}

void RimeWithWeaselHandler::_GetCandidateInfo(CandidateInfo& cinfo,
                                              RimeContext& ctx) {
  cinfo.candies.resize(ctx.menu.num_candidates);
  cinfo.comments.resize(ctx.menu.num_candidates);
  cinfo.labels.resize(ctx.menu.num_candidates);
  for (int i = 0; i < ctx.menu.num_candidates; ++i) {
    cinfo.candies[i].str = escape_string(u8tow(ctx.menu.candidates[i].text));
    if (ctx.menu.candidates[i].comment) {
      cinfo.comments[i].str =
          escape_string(u8tow(ctx.menu.candidates[i].comment));
    }
    if (RIME_STRUCT_HAS_MEMBER(ctx, ctx.select_labels) && ctx.select_labels) {
      cinfo.labels[i].str = escape_string(u8tow(ctx.select_labels[i]));
    } else if (ctx.menu.select_keys) {
      cinfo.labels[i].str =
          escape_string(std::wstring(1, ctx.menu.select_keys[i]));
    } else {
      cinfo.labels[i].str = std::to_wstring((i + 1) % 10);
    }
  }
  cinfo.highlighted = ctx.menu.highlighted_candidate_index;
  cinfo.currentPage = ctx.menu.page_no;
  cinfo.is_last_page = ctx.menu.is_last_page;
}

void RimeWithWeaselHandler::StartMaintenance() {
  m_session_status_map.clear();
  if (m_cloud) m_cloud->Stop();

  Finalize();
  _UpdateUI(0);
}

void RimeWithWeaselHandler::EndMaintenance() {
  if (m_disabled) {
    Initialize();
    _UpdateUI(0);
  }
  m_session_status_map.clear();
}

void RimeWithWeaselHandler::SetOption(WeaselSessionId ipc_id,
                                      const std::string& opt,
                                      bool val) {
  // from no-session client, not actual typing session
  if (!ipc_id) {
    if (m_global_ascii_mode && opt == "ascii_mode") {
      for (auto& pair : m_session_status_map)
        rime_api->set_option(to_session_id(pair.first), "ascii_mode", val);
    } else {
      rime_api->set_option(to_session_id(m_active_session), opt.c_str(), val);
    }
  } else {
    rime_api->set_option(to_session_id(ipc_id), opt.c_str(), val);
  }
  // refresh UI (and tray icon) so the option change takes effect immediately,
  // e.g. when toggling ascii_mode from the TSF language bar
  _UpdateUI(ipc_id ? ipc_id : m_active_session);
}

void RimeWithWeaselHandler::OnUpdateUI(std::function<void()> const& cb) {
  _UpdateUICallback = cb;
}

bool RimeWithWeaselHandler::_IsDeployerRunning() {
  HANDLE hMutex = CreateMutex(NULL, TRUE, L"WeaselDeployerMutex");
  bool deployer_detected = hMutex && GetLastError() == ERROR_ALREADY_EXISTS;
  if (hMutex) {
    CloseHandle(hMutex);
  }
  return deployer_detected;
}

void RimeWithWeaselHandler::_UpdateUI(WeaselSessionId ipc_id) {
  // if m_ui nullptr, _UpdateUI meaningless
  if (!m_ui)
    return;

  Status& weasel_status = m_ui->status();
  Context weasel_context;

  RimeSessionId session_id = to_session_id(ipc_id);

  if (ipc_id == 0)
    weasel_status.disabled = m_disabled;

  _GetStatus(weasel_status, ipc_id, weasel_context);

  SessionStatus& session_status = get_session_status(ipc_id);
  if (rime_api->get_option(session_id, "inline_preedit"))
    session_status.style.client_caps |= INLINE_PREEDIT_CAPABLE;
  else
    session_status.style.client_caps &= ~INLINE_PREEDIT_CAPABLE;

  if (!_ShowMessage(weasel_context, weasel_status)) {
    m_ui->Hide();
    m_ui->Update(weasel_context, weasel_status);
  }

  _RefreshTrayIcon(session_id, _UpdateUICallback);

  {
    std::lock_guard<std::mutex> lock(m_notifier_mutex);
    m_message_type.clear();
    m_message_value.clear();
    m_message_label.clear();
    m_option_name.clear();
  }
}

void RimeWithWeaselHandler::_LoadSchemaSpecificSettings(
    WeaselSessionId ipc_id,
    const std::string& schema_id) {
  if (!m_ui)
    return;
  RimeConfig config;
  if (!rime_api->schema_open(schema_id.c_str(), &config))
    return;
  _UpdateShowNotifications(&config);
  m_ui->style() = m_base_style;
  _UpdateUIStyle(&config, m_ui, false);
  SessionStatus& session_status = get_session_status(ipc_id);
  session_status.style = m_ui->style();
  UIStyle& style = session_status.style;
  // load schema color style config
  const int BUF_SIZE = 255;
  char buffer[BUF_SIZE + 1] = {0};
  const auto update_color_scheme = [&]() {
    std::string color_name(buffer);
    RimeConfigIterator preset = {0};
    if (rime_api->config_begin_map(
            &preset, &config, ("preset_color_schemes/" + color_name).c_str())) {
      _UpdateUIStyleColor(&config, style, color_name);
      rime_api->config_end(&preset);
    } else {
      RimeConfig weaselconfig;
      if (rime_api->config_open("weasel", &weaselconfig)) {
        _UpdateUIStyleColor(&weaselconfig, style, color_name);
        rime_api->config_close(&weaselconfig);
      }
    }
  };
  const char* key =
      m_current_dark_mode ? "style/color_scheme_dark" : "style/color_scheme";
  if (rime_api->config_get_string(&config, key, buffer, BUF_SIZE))
    update_color_scheme();
  // load schema icon start
  {
    const auto load_icon = [](RimeConfig& config, const char* key1,
                              const char* key2) {
      const auto user_dir = WeaselUserDataPath();
      const auto shared_dir = WeaselSharedDataPath();
      const int BUF_SIZE = 255;
      char buffer[BUF_SIZE + 1] = {0};
      if (rime_api->config_get_string(&config, key1, buffer, BUF_SIZE) ||
          (key2 != NULL &&
           rime_api->config_get_string(&config, key2, buffer, BUF_SIZE))) {
        auto resource = u8tow(buffer);
        if (fs::is_regular_file(user_dir / resource))
          return (user_dir / resource).wstring();
        else if (fs::is_regular_file(shared_dir / resource))
          return (shared_dir / resource).wstring();
      }
      return std::wstring();
    };
    style.current_zhung_icon =
        load_icon(config, "schema/icon", "schema/zhung_icon");
    style.current_ascii_icon = load_icon(config, "schema/ascii_icon", NULL);
    style.current_full_icon = load_icon(config, "schema/full_icon", NULL);
    style.current_half_icon = load_icon(config, "schema/half_icon", NULL);
  }
  // load schema icon end
  rime_api->config_close(&config);
}

void RimeWithWeaselHandler::_LoadAppInlinePreeditSet(WeaselSessionId ipc_id,
                                                     bool ignore_app_name) {
  SessionStatus& session_status = get_session_status(ipc_id);
  RimeSessionId session_id = session_status.session_id;
  static char _app_name[50];
  rime_api->get_property(session_id, "client_app", _app_name,
                         sizeof(_app_name) - 1);
  std::string app_name(_app_name);
  if (!ignore_app_name && m_last_app_name == app_name)
    return;
  m_last_app_name = app_name;
  bool inline_preedit = session_status.style.inline_preedit;
  bool found = false;
  if (!app_name.empty()) {
    auto it = m_app_options.find(app_name);
    if (it != m_app_options.end()) {
      AppOptions& options(m_app_options[it->first]);
      for (const auto& pair : options) {
        if (pair.first == "inline_preedit") {
          rime_api->set_option(session_id, pair.first.c_str(),
                               Bool(pair.second));
          session_status.style.inline_preedit = Bool(pair.second);
          found = true;
          break;
        }
      }
    }
  }
  if (!found) {
    session_status.style.inline_preedit = m_base_style.inline_preedit;
    // load from schema.
    RIME_STRUCT(RimeStatus, status);
    if (rime_api->get_status(session_id, &status)) {
      std::string schema_id = status.schema_id;
      RimeConfig config;
      if (rime_api->schema_open(schema_id.c_str(), &config)) {
        Bool value = False;
        if (rime_api->config_get_bool(&config, "style/inline_preedit",
                                      &value)) {
          session_status.style.inline_preedit = value;
        }
        rime_api->config_close(&config);
      }
      rime_api->free_status(&status);
    }
  }
  if (session_status.style.inline_preedit != inline_preedit)
    _UpdateInlinePreeditStatus(ipc_id);
}

bool RimeWithWeaselHandler::_ShowMessage(Context& ctx, Status& status) {
  std::lock_guard<std::mutex> lock(m_notifier_mutex);
  if (m_message_type.empty() || m_message_value.empty())
    return m_ui->IsCountingDown();
  // show as auxiliary string
  std::wstring& tips(ctx.aux.str);
  bool show_icon = false;
  if (m_message_type == "deploy") {
    if (m_message_value == "start")
      if (GetThreadUILanguage() == MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US))
        tips = L"Deploying RIME";
      else
        tips = L"正在部署 RIME";
    else if (m_message_value == "success")
      if (GetThreadUILanguage() == MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US))
        tips = L"Deployed";
      else
        tips = L"部署完成";
    else if (m_message_value == "failure") {
      if (GetThreadUILanguage() ==
          MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_TRADITIONAL))
        tips = L"有錯誤，請查看日誌 %TEMP%\\rime.weasel\\rime.weasel.*.INFO";
      else if (GetThreadUILanguage() ==
               MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED))
        tips = L"有错误，请查看日志 %TEMP%\\rime.weasel\\rime.weasel.*.INFO";
      else
        tips =
            L"There is an error, please check the logs "
            L"%TEMP%\\rime.weasel\\rime.weasel.*.INFO";
    }
  } else if (m_message_type == "schema") {
    tips = /*L"【" + */ status.schema_name /* + L"】"*/;
  } else if (m_message_type == "option") {
    status.type = SCHEMA;
    if (m_message_value == "!ascii_mode") {
      show_icon = true;
    } else if (m_message_value == "ascii_mode") {
      show_icon = true;
    } else
      tips = u8tow(m_message_label);

    if (m_message_value == "full_shape" || m_message_value == "!full_shape")
      status.type = FULL_SHAPE;
  }
  auto counter = m_ui->IsCountingDown();
  if (!show_icon && counter)
    return counter;
  auto foption = m_show_notifications.find(m_option_name);
  auto falways = m_show_notifications.find("always");
  if ((!add_session && (foption != m_show_notifications.end() ||
                        falways != m_show_notifications.end())) ||
      m_message_type == "deploy") {
    m_ui->Update(ctx, status);
    if (m_show_notifications_time)
      m_ui->ShowWithTimeout(m_show_notifications_time);
    return true;
  } else {
    return m_ui->IsCountingDown();
  }
}
inline std::string _GetLabelText(const std::vector<Text>& labels,
                                 int id,
                                 const wchar_t* format) {
  wchar_t buffer[128];
  swprintf_s<128>(buffer, format, labels.at(id).str.c_str());
  return wtou8(std::wstring(buffer));
}

bool RimeWithWeaselHandler::_Respond(WeaselSessionId ipc_id, EatLine eat) {
  std::wstring body;
  body.reserve(4096);
  std::vector<const char*> actions;
  actions.reserve(8);

  SessionStatus& session_status = get_session_status(ipc_id);
  // ===== P1-B8 游戏兼容：读 suppress_panel 会话选项 =====
  // 该选项由既有 _ReadClientInfo 的 app_options 循环（基线 line 437-440）
  // 在游戏进程会话建立时自动 set_option 写入；此处仅读回。
  // true = 抑制候选窗/内嵌回写，但 commit 上屏键仍正常放行（不丢键）。
  const bool suppress_panel =
      !!rime_api->get_option(session_id, "suppress_panel");

  RimeSessionId session_id = session_status.session_id;
  RIME_STRUCT(RimeCommit, commit);
  if (rime_api->get_commit(session_id, &commit)) {
    actions.push_back("commit");
    std::wstring commit_text_w = escape_string(u8tow(commit.text));
    body.append(L"commit=").append(commit_text_w).append(L"\n");
    rime_api->free_commit(&commit);
  }

  bool is_composing = false;
  RIME_STRUCT(RimeStatus, status);
  static const std::wstring Bool_wstring[] = {L"0", L"1"};
  if (rime_api->get_status(session_id, &status)) {
    is_composing = !!status.is_composing;
    actions.push_back("status");
    body.append(L"status.ascii_mode=")
        .append(Bool_wstring[!!status.is_ascii_mode])
        .append(L"\n")
        .append(L"status.composing=")
        .append(Bool_wstring[suppress_panel ? 0 : (int)!!status.is_composing])
        .append(L"\n")
        .append(L"status.disabled=")
        .append(Bool_wstring[!!status.is_disabled])
        .append(L"\n")
        .append(L"status.full_shape=")
        .append(Bool_wstring[!!status.is_full_shape])
        .append(L"\n")
        .append(L"status.schema_id=")
        .append(status.schema_id ? u8tow(status.schema_id) : std::wstring())
        .append(L"\n");
    if (m_global_ascii_mode &&
        (session_status.status.is_ascii_mode != status.is_ascii_mode)) {
      for (auto& pair : m_session_status_map) {
        if (pair.first != ipc_id)
          rime_api->set_option(to_session_id(pair.first), "ascii_mode",
                               !!status.is_ascii_mode);
      }
    }
    session_status.status = status;
    rime_api->free_status(&status);
  }

  RIME_STRUCT(RimeContext, ctx);
  if (rime_api->get_context(session_id, &ctx)) {
    bool has_candidates = ctx.menu.num_candidates > 0;
    CandidateInfo cinfo;
    if (!suppress_panel && has_candidates) {
      _GetCandidateInfo(cinfo, ctx);
    }
    if (!suppress_panel && is_composing) {
      const auto& preedit = ctx.composition.preedit;
      const auto& start = ctx.composition.sel_start;
      const auto& end = ctx.composition.sel_end;
      const auto& cursor = ctx.composition.cursor_pos;
      static const auto u8towstring = [](const char* u8str, int len = 0) {
        return std::to_wstring(utf8towcslen(u8str, len));
      };
      actions.push_back("ctx");
      switch (session_status.style.preedit_type) {
        case UIStyle::PREVIEW: {
          if (ctx.commit_text_preview) {
            const char* first_utf8 = ctx.commit_text_preview;
            const size_t first_len = std::strlen(first_utf8);
            const std::wstring first_w = escape_string(u8tow(first_utf8));
            const std::wstring tmp = u8towstring(first_utf8, (int)first_len);
            body.append(L"ctx.preedit=")
                .append(first_w)
                .append(L"\n")
                .append(L"ctx.preedit.cursor=")
                .append(u8towstring(first_utf8, 0))
                .append(L",")
                .append(tmp)
                .append(L",")
                .append(tmp)
                .append(L"\n");
            break;
          }
          // no preview, fall back to composition
        }
        case UIStyle::COMPOSITION: {
          body.append(L"ctx.preedit=")
              .append(escape_string(u8tow(preedit)))
              .append(L"\n");
          if (start <= end) {
            body.append(L"ctx.preedit.cursor=")
                .append(u8towstring(preedit, start))
                .append(L",")
                .append(u8towstring(preedit, end))
                .append(L",")
                .append(u8towstring(preedit, cursor))
                .append(L"\n");
          }
          break;
        }
        case UIStyle::PREVIEW_ALL: {
          body.append(L"ctx.preedit=")
              .append(escape_string(u8tow(preedit)))
              .append(L"  [");
          auto label_valid = session_status.style.label_font_point > 0;
          auto comment_valid = session_status.style.comment_font_point > 0;
          const std::wstring mark_text_w =
              session_status.style.mark_text.empty()
                  ? std::wstring(L"*")
                  : session_status.style.mark_text;
          for (auto i = 0; i < ctx.menu.num_candidates; i++) {
            std::wstring label_w;
            if (label_valid) {
              wchar_t buf_lbl[128];
              swprintf_s<128>(buf_lbl,
                              session_status.style.label_text_format.c_str(),
                              cinfo.labels.at(i).str.c_str());
              label_w = std::wstring(buf_lbl);
            }
            std::wstring comment_w =
                comment_valid ? cinfo.comments.at(i).str : std::wstring();
            std::wstring prefix_w = (i != ctx.menu.highlighted_candidate_index)
                                        ? std::wstring()
                                        : mark_text_w;
            body.append(L" ")
                .append(prefix_w)
                .append(escape_string(label_w))
                .append(escape_string(u8tow(ctx.menu.candidates[i].text)))
                .append(L" ")
                .append(escape_string(comment_w));
          }
          body.append(L" ]\n");
          if (start <= end) {
            body.append(L"ctx.preedit.cursor=")
                .append(u8towstring(preedit, start))
                .append(L",")
                .append(u8towstring(preedit, end))
                .append(L",")
                .append(u8towstring(preedit, cursor))
                .append(L"\n");
          }
          break;
        }
      }
    }
    // ===== P2-B：云候选旁路（异步查表 + 入队，绝不阻塞） =====
    // 只在中文 composing 态触发；结果以 CARD_CLOUD 卡片挂到 cinfo.extra_cards，
    // 由下方既有 ctx.cand= 序列化随 extra_cards 一起下发（P2-A 契约）。
    if (m_cloud && is_composing) {
      std::string key = weasel::CloudCandidateService::ExtractPinyinKey(
          ctx.composition.preedit ? ctx.composition.preedit : "");
      std::vector<std::string> cloud_cands;
      auto cst = m_cloud->OnPreedit(key, &cloud_cands);
      if (cst == weasel::CloudCandidateService::CardState::kLoading) {
        weasel::ExtraCard card;
        card.type = weasel::CARD_CLOUD;
        card.title = L"云候选";
        card.body = L"…";                 // 加载中占位（亮点 20）
        cinfo.extra_cards.push_back(std::move(card));
      } else if (cst == weasel::CloudCandidateService::CardState::kReady &&
                 !cloud_cands.empty()) {
        weasel::ExtraCard card;
        card.type = weasel::CARD_CLOUD;
        card.title = L"云候选";
        const std::wstring word = weasel::u8tow(cloud_cands.front());
        card.body = word;                  // 云端词单行展示
        card.action = word;                // 选中后 commit 文本（TSF 直提）
        card.action_id = ++m_cloud_action_id;
        cinfo.extra_cards.push_back(std::move(card));
      }
      // kNone：不渲染卡片，本地候选照常。
    }

      // ===== P2-D：把 AI 卡片挂进 extra_cards（绝不改 candies 排序） =====
      {
        SessionStatus& ss2 = get_session_status(ipc_id);
        if (!ss2.pending_ai_cards.empty()) {
          for (const auto& card : ss2.pending_ai_cards) {
            cinfo.extra_cards.push_back(card);  // CARD_AI
          }
          // 注意：candies 顺序/数量完全不动，仅追加卡片。
        }
      }

    if (has_candidates || !cinfo.extra_cards.empty()) {
      std::wstringstream ss;
      boost::archive::text_woarchive oa(ss);

      oa << cinfo;

      auto s = ss.str();
      body.append(L"ctx.cand=").append(std::move(s)).append(L"\n");
    }
    // ===== P2-A：补发 ctx.aux= 文本行（翻译/tips 通道） =====
    // weasel_context.aux 由 _ShowMessage 或 P2-E 翻译服务写入。
    // 空则不写行（保持老行为）；非空才追加，老 TSF 解析安全。
    {
      std::lock_guard<std::mutex> lk(session_status.aux_mtx);
      const std::wstring& aux_text = session_status.pending_aux;  // P2-E 填充
      if (!aux_text.empty()) {
        body.append(L"ctx.aux=").append(escape_string(aux_text)).append(L"\n");
        if (std::find(actions.begin(), actions.end(), "ctx") == actions.end())
      // ===== P2-E：边写边译（预编辑串达到阈值自动提交，默认关） =====
    // auto_threshold=0 时 TranslationService 内部直接返回，零开销。
    if (m_translate && is_composing && ctx.composition.preedit) {
      m_translate->AutoTranslate(ipc_id, ctx.composition.preedit);
    }

        actions.push_back("ctx");
      }
    }
    rime_api->free_context(&ctx);
  }

  // configuration information
  actions.push_back("config");
  body.append(L"config.inline_preedit=")
      .append(std::to_wstring((int)session_status.style.inline_preedit))
      .append(L"\n");

  // style
  if (!session_status.__synced) {
    std::wstringstream ss;
    boost::archive::text_woarchive oa(ss);
    oa << session_status.style;

    actions.push_back("style");
    body.append(L"style=").append(ss.str()).append(L"\n");
    session_status.__synced = true;
  }

  // summarize: send header first to avoid vector head-insert cost
  std::wstring header;
  if (actions.empty()) {
    header = L"action=noop\n";
  } else {
    std::string actionList;
    actionList.reserve(64);
    for (size_t i = 0; i < actions.size(); ++i) {
      if (i > 0)
        actionList += ',';
      actionList += actions[i];
    }
    header = std::wstring(L"action=") + u8tow(actionList) + L"\n";
  }
  if (!eat(header))
    return false;

  body.append(L".\n");
  if (!eat(body))
    return false;

  return true;
}

// Blend foreground and background ARGB colors taking alpha into account.
// Returns an ABGR COLORREF with premultiplied alpha blended result.
static inline COLORREF blend_colors(COLORREF fcolor, COLORREF bcolor) {
  // Extract ARGB channels from both colors.
  BYTE fA = (fcolor >> 24) & 0xFF;
  BYTE fB = (fcolor >> 16) & 0xFF;
  BYTE fG = (fcolor >> 8) & 0xFF;
  BYTE fR = fcolor & 0xFF;
  BYTE bA = (bcolor >> 24) & 0xFF;
  BYTE bB = (bcolor >> 16) & 0xFF;
  BYTE bG = (bcolor >> 8) & 0xFF;
  BYTE bR = bcolor & 0xFF;
  // Convert alpha to [0,1]
  float fAlpha = fA / 255.0f;
  float bAlpha = bA / 255.0f;
  // Result alpha
  float retAlpha = fAlpha + (1 - fAlpha) * bAlpha;
  if (retAlpha <= 1e-6f) {
    // Fully transparent result — return background unchanged as fallback.
    return bcolor;
  }
  auto mix = [&](float fc, float bc) -> BYTE {
    return static_cast<BYTE>((fc * fAlpha + bc * bAlpha * (1 - fAlpha)) /
                             retAlpha);
  };
  BYTE retR = mix(fR, bR);
  BYTE retG = mix(fG, bG);
  BYTE retB = mix(fB, bB);
  BYTE outA = static_cast<BYTE>(retAlpha * 255.0f);
  return (static_cast<COLORREF>(outA) << 24) | (retB << 16) | (retG << 8) |
         retR;
}
// parse color value, with fallback value
static Bool _RimeGetColor(RimeConfig* config,
                          const std::string& key,
                          int& value,
                          const ColorFormat& fmt,
                          const unsigned int& fallback) {
  char color[256] = {0};
  if (!rime_api->config_get_string(config, key.c_str(), color, 256)) {
    value = fallback;
    return False;
  }
  const auto color_str = std::string(color);
  // adjudge if str is 0x 0X # hex color format, return trimmed hex part
  // out part is 6 or 8 length hex string without white space
  const auto parse_color_code = [](const std::string& str, std::string& out) {
    if (str.empty())
      return false;
    size_t start = 0;
    if (str[0] == '#') {
      start = 1;
    } else if (str.size() >= 2 &&
               (str.compare(0, 2, "0x") == 0 || str.compare(0, 2, "0X") == 0)) {
      start = 2;
    } else {
      return false;
    }
    const std::string hex_part = str.substr(start);
    if (hex_part.empty())
      return false;
    if ((start == 1 || start == 2) && hex_part.length() != 3 &&
        hex_part.length() != 4 && hex_part.length() != 6 &&
        hex_part.length() != 8) {
      return false;
    }
    for (char c : hex_part) {
      if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
            (c >= 'A' && c <= 'F')))
        return false;
    }
    out = str.substr(start).substr(0, 8);
#define _2C(c) std::string(2, c)
    if (out.size() == 3)
      out = _2C(out[0]) + _2C(out[1]) + _2C(out[2]);
    else if (out.size() == 4)
      out = _2C(out[0]) + _2C(out[1]) + _2C(out[2]) + _2C(out[3]);
#undef _2C
    return true;
  };
  auto hex_color = std::string();
  if (parse_color_code(color_str, hex_color)) {
    value = std::stoul(hex_color, 0, 16);
    if (hex_color.length() == 6)
      value = (fmt != COLOR_RGBA) ? (value | 0xff000000)
                                  : (((unsigned int)value << 8) | 0x000000ff);
  } else {
    if (!rime_api->config_get_int(config, key.c_str(), &value)) {
      value = fallback;
      return False;
    }
    if (value <= 0xffffff)
      value = (fmt != COLOR_RGBA) ? (value | 0xff000000)
                                  : (((unsigned int)value << 8) | 0x000000ff);
    else if (value > 0xffffffff)
      value &= 0xffffffff;
  }
  if (fmt == COLOR_ARGB)
    value = ARGB2ABGR(value);
  else if (fmt == COLOR_RGBA)
    value = RGBA2ABGR(value);
  value &= 0xffffffff;
  return True;
}

template <typename T, size_t N>
using Array = std::array<std::pair<const char*, T>, N>;

// parset bool type configuration to T type value trueValue / falseValue
template <typename T>
void _RimeGetBool(RimeConfig* config,
                  const char* key,
                  bool cond,
                  T& value,
                  const T& trueValue = true,
                  const T& falseValue = false) {
  Bool tempb = False;
  if (rime_api->config_get_bool(config, key, &tempb) || cond)
    value = (!!tempb) ? trueValue : falseValue;
}
// parse string option to T type value, with fallback
template <typename T, size_t N>
void _RimeParseStringOptWithFallback(RimeConfig* config,
                                     const char* key,
                                     T& value,
                                     const Array<T, N>& arr,
                                     const T& fallback) {
  char str_buff[256] = {0};
  if (rime_api->config_get_string(config, key, str_buff, 255)) {
    for (size_t i = 0; i < N; ++i) {
      if (strcmp(arr[i].first, str_buff) == 0) {
        value = arr[i].second;
        return;
      }
    }
  }
  value = fallback;
}

template <typename T>
void _RimeGetIntStr(RimeConfig* config,
                    const char* key,
                    T& value,
                    const char* fb_key = nullptr,
                    const void* fb_value = nullptr,
                    const std::function<void(T&)>& func = nullptr) {
  if constexpr (std::is_same<T, int>::value) {
    if (!rime_api->config_get_int(config, key, &value) && fb_key != 0)
      rime_api->config_get_int(config, fb_key, &value);
  } else if constexpr (std::is_same<T, std::wstring>::value) {
    const int BUF_SIZE = 2047;
    char buffer[BUF_SIZE + 1] = {0};
    if (rime_api->config_get_string(config, key, buffer, BUF_SIZE) ||
        rime_api->config_get_string(config, fb_key, buffer, BUF_SIZE)) {
      value = u8tow(buffer);
    } else if (fb_value) {
      value = *(T*)fb_value;
    }
  }
  if (func)
    func(value);
}

// Helper to iterate a Rime map and invoke callback with key/path
static void ForEachRimeMap(
    RimeConfig* config,
    const std::string& path,
    const std::function<void(const char* key, const char* child_path)>& cb) {
  RimeConfigIterator iter;
  if (!rime_api->config_begin_map(&iter, config, path.c_str()))
    return;
  while (rime_api->config_next(&iter)) {
    cb(iter.key, iter.path);
  }
  rime_api->config_end(&iter);
}

// Helper to iterate a Rime list and invoke callback with item path
static void ForEachRimeList(
    RimeConfig* config,
    const std::string& path,
    const std::function<void(const char* item_path)>& cb) {
  RimeConfigIterator iter;
  if (!rime_api->config_begin_list(&iter, config, path.c_str()))
    return;
  while (rime_api->config_next(&iter)) {
    cb(iter.path);
  }
  rime_api->config_end(&iter);
}

void RimeWithWeaselHandler::_UpdateShowNotifications(RimeConfig* config,
                                                     bool initialize) {
  Bool show_notifications = true;
  if (initialize)
    m_show_notifications_base.clear();
  m_show_notifications.clear();

  if (rime_api->config_get_bool(config, "show_notifications",
                                &show_notifications)) {
    // config read as bool, for global all on or off
    if (show_notifications)
      m_show_notifications["always"] = true;
    if (initialize)
      m_show_notifications_base = m_show_notifications;
  } else {
    // read as list using helper
    ForEachRimeList(config, "show_notifications", [&](const char* item_path) {
      char buffer[256] = {0};
      if (rime_api->config_get_string(config, item_path, buffer, 256))
        m_show_notifications[std::string(buffer)] = true;
    });
    if (initialize)
      m_show_notifications_base = m_show_notifications;
    if (m_show_notifications.empty()) {
      // not configured, or incorrect type
      if (initialize)
        m_show_notifications_base["always"] = true;
      m_show_notifications = m_show_notifications_base;
    }
  }
}

// update ui's style parameters, ui has been check before referenced
static void _UpdateUIStyle(RimeConfig* config, UI* ui, bool initialize) {
  UIStyle& style(ui->style());
  const std::function<void(std::wstring&)> rmspace = [](std::wstring& str) {
    str = std::regex_replace(str, std::wregex(L"\\s*(,|:|^|$)\\s*"), L"$1");
  };
  const std::function<void(int&)> _abs = [](int& value) { value = abs(value); };
  // get font faces
  _RimeGetIntStr(config, "style/font_face", style.font_face, 0, 0, rmspace);
  std::wstring* const pFallbackFontFace = initialize ? &style.font_face : NULL;
  _RimeGetIntStr(config, "style/label_font_face", style.label_font_face, 0,
                 pFallbackFontFace, rmspace);
  _RimeGetIntStr(config, "style/comment_font_face", style.comment_font_face, 0,
                 pFallbackFontFace, rmspace);
  // able to set label font/comment font empty, force fallback to font face.
  if (style.label_font_face.empty())
    style.label_font_face = style.font_face;
  if (style.comment_font_face.empty())
    style.comment_font_face = style.font_face;
  // get font points
  _RimeGetIntStr(config, "style/font_point", style.font_point);
  if (style.font_point <= 0)
    style.font_point = 12;
  _RimeGetIntStr(config, "style/label_font_point", style.label_font_point,
                 "style/font_point", 0, _abs);
  _RimeGetIntStr(config, "style/comment_font_point", style.comment_font_point,
                 "style/font_point", 0, _abs);
  _RimeGetIntStr(config, "style/candidate_abbreviate_length",
                 style.candidate_abbreviate_length, 0, 0, _abs);
  _RimeGetBool(config, "style/inline_preedit", initialize,
               style.inline_preedit);
  _RimeGetBool(config, "style/vertical_auto_reverse", initialize,
               style.vertical_auto_reverse);
  static constexpr Array<UIStyle::PreeditType, 3> _preeditArr = {
      {{"composition", UIStyle::COMPOSITION},
       {"preview", UIStyle::PREVIEW},
       {"preview_all", UIStyle::PREVIEW_ALL}}};
  _RimeParseStringOptWithFallback(config, "style/preedit_type",
                                  style.preedit_type, _preeditArr,
                                  style.preedit_type);
  static constexpr Array<UIStyle::AntiAliasMode, 5> _aliasModeArr = {
      {{"force_dword", UIStyle::FORCE_DWORD},
       {"cleartype", UIStyle::CLEARTYPE},
       {"grayscale", UIStyle::GRAYSCALE},
       {"aliased", UIStyle::ALIASED},
       {"default", UIStyle::DEFAULT}}};
  _RimeParseStringOptWithFallback(config, "style/antialias_mode",
                                  style.antialias_mode, _aliasModeArr,
                                  style.antialias_mode);
  static constexpr Array<UIStyle::HoverType, 3> _hoverTypeArr = {
      {{"none", UIStyle::HoverType::NONE},
       {"semi_hilite", UIStyle::HoverType::SEMI_HILITE},
       {"hilite", UIStyle::HoverType::HILITE}}};
  _RimeParseStringOptWithFallback(config, "style/hover_type", style.hover_type,
                                  _hoverTypeArr, style.hover_type);
  static constexpr Array<UIStyle::LayoutAlignType, 3> _alignType = {
      {{"top", UIStyle::ALIGN_TOP},
       {"center", UIStyle::ALIGN_CENTER},
       {"bottom", UIStyle::ALIGN_BOTTOM}}};
  _RimeParseStringOptWithFallback(config, "style/layout/align_type",
                                  style.align_type, _alignType,
                                  style.align_type);
  _RimeGetBool(config, "style/display_tray_icon", initialize,
               style.display_tray_icon);
  _RimeGetBool(config, "style/ascii_tip_follow_cursor", initialize,
               style.ascii_tip_follow_cursor);
  _RimeGetBool(config, "style/horizontal", initialize, style.layout_type,
               UIStyle::LAYOUT_HORIZONTAL, UIStyle::LAYOUT_VERTICAL);
  _RimeGetBool(config, "style/paging_on_scroll", initialize,
               style.paging_on_scroll);
  _RimeGetBool(config, "style/click_to_capture", initialize,
               style.click_to_capture, true, false);
  _RimeGetBool(config, "style/fullscreen", false, style.layout_type,
               ((style.layout_type == UIStyle::LAYOUT_HORIZONTAL)
                    ? UIStyle::LAYOUT_HORIZONTAL_FULLSCREEN
                    : UIStyle::LAYOUT_VERTICAL_FULLSCREEN),
               style.layout_type);
  _RimeGetBool(config, "style/vertical_text", false, style.layout_type,
               UIStyle::LAYOUT_VERTICAL_TEXT, style.layout_type);
  _RimeGetBool(config, "style/vertical_text_left_to_right", false,
               style.vertical_text_left_to_right);
  _RimeGetBool(config, "style/vertical_text_with_wrap", false,
               style.vertical_text_with_wrap);
  static constexpr Array<bool, 2> _text_orientation = {
      {{"horizontal", false}, {"vertical", true}}};
  bool _text_orientation_bool = false;
  _RimeParseStringOptWithFallback(config, "style/text_orientation",
                                  _text_orientation_bool, _text_orientation,
                                  _text_orientation_bool);
  if (_text_orientation_bool)
    style.layout_type = UIStyle::LAYOUT_VERTICAL_TEXT;
  _RimeGetIntStr(config, "style/label_format", style.label_text_format);
  _RimeGetIntStr(config, "style/mark_text", style.mark_text);
  _RimeGetIntStr(config, "style/layout/baseline", style.baseline, 0, 0, _abs);
  _RimeGetIntStr(config, "style/layout/linespacing", style.linespacing, 0, 0,
                 _abs);
  _RimeGetIntStr(config, "style/layout/min_width", style.min_width, 0, 0, _abs);
  _RimeGetIntStr(config, "style/layout/max_width", style.max_width, 0, 0, _abs);
  _RimeGetIntStr(config, "style/layout/min_height", style.min_height, 0, 0,
                 _abs);
  _RimeGetIntStr(config, "style/layout/max_height", style.max_height, 0, 0,
                 _abs);
  // layout (alternative to style/horizontal)
  static constexpr Array<UIStyle::LayoutType, 5> _layoutArr = {
      {{"vertical", UIStyle::LAYOUT_VERTICAL},
       {"horizontal", UIStyle::LAYOUT_HORIZONTAL},
       {"vertical_text", UIStyle::LAYOUT_VERTICAL_TEXT},
       {"vertical+fullscreen", UIStyle::LAYOUT_VERTICAL_FULLSCREEN},
       {"horizontal+fullscreen", UIStyle::LAYOUT_HORIZONTAL_FULLSCREEN}}};
  _RimeParseStringOptWithFallback(config, "style/layout/type",
                                  style.layout_type, _layoutArr,
                                  style.layout_type);
  // disable max_width when full screen
  if (style.layout_type == UIStyle::LAYOUT_HORIZONTAL_FULLSCREEN ||
      style.layout_type == UIStyle::LAYOUT_VERTICAL_FULLSCREEN) {
    style.max_width = 0;
    style.inline_preedit = false;
  }
  _RimeGetIntStr(config, "style/layout/border", style.border,
                 "style/layout/border_width", 0, _abs);
  _RimeGetIntStr(config, "style/layout/margin_x", style.margin_x);
  _RimeGetIntStr(config, "style/layout/margin_y", style.margin_y);
  _RimeGetIntStr(config, "style/layout/spacing", style.spacing, 0, 0, _abs);
  _RimeGetIntStr(config, "style/layout/candidate_spacing",
                 style.candidate_spacing, 0, 0, _abs);
  _RimeGetIntStr(config, "style/layout/hilite_spacing", style.hilite_spacing, 0,
                 0, _abs);
  _RimeGetIntStr(config, "style/layout/hilite_padding_x",
                 style.hilite_padding_x, "style/layout/hilite_padding", 0,
                 _abs);
  _RimeGetIntStr(config, "style/layout/hilite_padding_y",
                 style.hilite_padding_y, "style/layout/hilite_padding", 0,
                 _abs);
  _RimeGetIntStr(config, "style/layout/shadow_radius", style.shadow_radius, 0,
                 0, _abs);
  // disable shadow for fullscreen layout
  style.shadow_radius *=
      (!(style.layout_type == UIStyle::LAYOUT_HORIZONTAL_FULLSCREEN ||
         style.layout_type == UIStyle::LAYOUT_VERTICAL_FULLSCREEN));
  _RimeGetIntStr(config, "style/layout/shadow_offset_x", style.shadow_offset_x);
  _RimeGetIntStr(config, "style/layout/shadow_offset_y", style.shadow_offset_y);
  // round_corner as alias of hilited_corner_radius
  _RimeGetIntStr(config, "style/layout/hilited_corner_radius",
                 style.round_corner, "style/layout/round_corner", 0, _abs);
  // corner_radius not set, fallback to round_corner
  _RimeGetIntStr(config, "style/layout/corner_radius", style.round_corner_ex,
                 "style/layout/round_corner", 0, _abs);
  // fix padding and spacing settings
  if (style.layout_type != UIStyle::LAYOUT_VERTICAL_TEXT) {
    // hilite_padding vs spacing
    // if hilite_padding over spacing, increase spacing
    style.spacing = std::max(style.spacing, style.hilite_padding_y * 2);
    // hilite_padding vs candidate_spacing
    if (style.layout_type == UIStyle::LAYOUT_VERTICAL_FULLSCREEN ||
        style.layout_type == UIStyle::LAYOUT_VERTICAL) {
      // vertical, if hilite_padding_y over candidate spacing,
      // increase candidate spacing
      style.candidate_spacing =
          std::max(style.candidate_spacing, style.hilite_padding_y * 2);
    } else {
      // horizontal, if hilite_padding_x over candidate
      // spacing, increase candidate spacing
      style.candidate_spacing =
          std::max(style.candidate_spacing, style.hilite_padding_x * 2);
    }
    // hilite_padding_x vs hilite_spacing
    if (!style.inline_preedit)
      style.hilite_spacing = std::max(style.hilite_spacing, style.hilite_padding_x);
  } else  // LAYOUT_VERTICAL_TEXT
  {
    // hilite_padding_x vs spacing
    // if hilite_padding over spacing, increase spacing
    style.spacing = std::max(style.spacing, style.hilite_padding_x * 2);
    // hilite_padding vs candidate_spacing
    // if hilite_padding_x over candidate
    // spacing, increase candidate spacing
    style.candidate_spacing =
        std::max(style.candidate_spacing, style.hilite_padding_x * 2);
    // vertical_text_with_wrap and hilite_padding_y over candidate_spacing
    if (style.vertical_text_with_wrap)
      style.candidate_spacing =
          std::max(style.candidate_spacing, style.hilite_padding_y * 2);
    // hilite_padding_y vs hilite_spacing
    if (!style.inline_preedit)
      style.hilite_spacing = std::max(style.hilite_spacing, style.hilite_padding_y);
  }
  // fix padding and margin settings
  int scale = style.margin_x < 0 ? -1 : 1;
  style.margin_x = scale * std::max(style.hilite_padding_x, abs(style.margin_x));
  scale = style.margin_y < 0 ? -1 : 1;
  style.margin_y = scale * std::max(style.hilite_padding_y, abs(style.margin_y));
  // get enhanced_position
  _RimeGetBool(config, "style/enhanced_position", initialize,
               style.enhanced_position, true, false);
  // get color scheme
  const int BUF_SIZE = 255;
  char buffer[BUF_SIZE + 1] = {0};
  if (initialize && rime_api->config_get_string(config, "style/color_scheme",
                                                buffer, BUF_SIZE))
    _UpdateUIStyleColor(config, style);
}
// load color configs to style, by "style/color_scheme" or specific scheme name
// "color" which is default empty
static bool _UpdateUIStyleColor(RimeConfig* config,
                                UIStyle& style,
                                const std::string& color) {
  const int BUF_SIZE = 255;
  char buffer[BUF_SIZE + 1] = {0};
  std::string color_mark = "style/color_scheme";
  // color scheme
  if (rime_api->config_get_string(config, color_mark.c_str(), buffer,
                                  BUF_SIZE) ||
      !color.empty()) {
    std::string prefix("preset_color_schemes/");
    prefix += (color.empty()) ? buffer : color;
    // define color format, default abgr if not set
    ColorFormat fmt = COLOR_ABGR;
    static constexpr Array<ColorFormat, 3> _colorFmt = {
        {{"argb", COLOR_ARGB}, {"rgba", COLOR_RGBA}, {"abgr", COLOR_ABGR}}};
    _RimeParseStringOptWithFallback(config, (prefix + "/color_format").c_str(),
                                    fmt, _colorFmt, COLOR_ABGR);
#define COLOR(key, value, fallback) \
  _RimeGetColor(config, (prefix + "/" + key), value, fmt, fallback)
    COLOR("back_color", style.back_color, 0xffffffff);
    COLOR("shadow_color", style.shadow_color, 0);
    COLOR("prevpage_color", style.prevpage_color, 0);
    COLOR("nextpage_color", style.nextpage_color, 0);
    COLOR("text_color", style.text_color, 0xff000000);
    COLOR("candidate_text_color", style.candidate_text_color, style.text_color);
    COLOR("candidate_back_color", style.candidate_back_color, 0);
    COLOR("border_color", style.border_color, style.text_color);
    COLOR("hilited_text_color", style.hilited_text_color, style.text_color);
    COLOR("hilited_back_color", style.hilited_back_color, style.back_color);
    COLOR("hilited_candidate_text_color", style.hilited_candidate_text_color,
          style.hilited_text_color);
    COLOR("hilited_candidate_back_color", style.hilited_candidate_back_color,
          style.hilited_back_color);
    COLOR("hilited_candidate_shadow_color",
          style.hilited_candidate_shadow_color, 0);
    COLOR("hilited_shadow_color", style.hilited_shadow_color, 0);
    COLOR("candidate_shadow_color", style.candidate_shadow_color, 0);
    COLOR("candidate_border_color", style.candidate_border_color, 0);
    COLOR("hilited_candidate_border_color",
          style.hilited_candidate_border_color, 0);
    COLOR("label_color", style.label_text_color,
          blend_colors(style.candidate_text_color, style.candidate_back_color));
    COLOR("hilited_label_color", style.hilited_label_text_color,
          blend_colors(style.hilited_candidate_text_color,
                       style.hilited_candidate_back_color));
    COLOR("comment_text_color", style.comment_text_color,
          style.label_text_color);
    COLOR("hilited_comment_text_color", style.hilited_comment_text_color,
          style.hilited_label_text_color);
    COLOR("hilited_mark_color", style.hilited_mark_color, 0);
#undef COLOR
    return true;
  }
  return false;
}
static void _LoadAppOptions(RimeConfig* config,
                            AppOptionsByAppName& app_options) {
  app_options.clear();
  ForEachRimeMap(
      config, "app_options", [&](const char* app_key, const char* app_path) {
        AppOptions& options(app_options[app_key]);
        ForEachRimeMap(
            config, app_path, [&](const char* opt_key, const char* opt_path) {
              Bool value = False;
              if (rime_api->config_get_bool(config, opt_path, &value)) {
                options[opt_key] = !!value;
              }
            });
      });
}

void RimeWithWeaselHandler::_GetStatus(Status& stat,
                                       WeaselSessionId ipc_id,
                                       Context& ctx) {
  SessionStatus& session_status = get_session_status(ipc_id);
  RimeSessionId session_id = session_status.session_id;
  RIME_STRUCT(RimeStatus, status);
  if (rime_api->get_status(session_id, &status)) {
    std::string schema_id = "";
    if (status.schema_id)
      schema_id = status.schema_id;
    stat.schema_name = u8tow(status.schema_name);
    stat.schema_id = u8tow(status.schema_id);
    stat.ascii_mode = !!status.is_ascii_mode;
    stat.composing = !!status.is_composing;
    stat.disabled = !!status.is_disabled;
    stat.full_shape = !!status.is_full_shape;
    if (schema_id != m_last_schema_id) {
      session_status.__synced = false;
      m_last_schema_id = schema_id;
      if (schema_id != ".default") {  // don't load for schema select menu
        bool inline_preedit = session_status.style.inline_preedit;
        _LoadSchemaSpecificSettings(ipc_id, schema_id);
        _LoadAppInlinePreeditSet(ipc_id, true);
        if (session_status.style.inline_preedit != inline_preedit)
          // in case of inline_preedit set in schema
          _UpdateInlinePreeditStatus(ipc_id);
        // refresh icon after schema changed
        _RefreshTrayIcon(session_id, _UpdateUICallback);
        m_ui->style() = session_status.style;
        if (m_show_notifications.find("schema") != m_show_notifications.end() &&
            m_show_notifications_time > 0) {
          ctx.aux.str = stat.schema_name;
          m_ui->Update(ctx, stat);
          m_ui->ShowWithTimeout(m_show_notifications_time);
        }
      }
    }
    rime_api->free_status(&status);
  }
}

void RimeWithWeaselHandler::_GetContext(Context& weasel_context,
                                        RimeSessionId session_id) {
  RIME_STRUCT(RimeContext, ctx);
  if (rime_api->get_context(session_id, &ctx)) {
    if (ctx.composition.length > 0) {
      weasel_context.preedit.str = u8tow(ctx.composition.preedit);
      if (ctx.composition.sel_start < ctx.composition.sel_end) {
        TextAttribute attr;
        attr.type = HIGHLIGHTED;
        attr.range.start =
            utf8towcslen(ctx.composition.preedit, ctx.composition.sel_start);
        attr.range.end =
            utf8towcslen(ctx.composition.preedit, ctx.composition.sel_end);

        weasel_context.preedit.attributes.push_back(attr);
      }
    }
    if (ctx.menu.num_candidates) {
      CandidateInfo& cinfo(weasel_context.cinfo);
      _GetCandidateInfo(cinfo, ctx);
    }
    rime_api->free_context(&ctx);
  }
}

void RimeWithWeaselHandler::_UpdateInlinePreeditStatus(WeaselSessionId ipc_id) {
  if (!m_ui)
    return;
  SessionStatus& session_status = get_session_status(ipc_id);
  RimeSessionId session_id = session_status.session_id;
  // set inline_preedit option
  bool inline_preedit = session_status.style.inline_preedit;
  rime_api->set_option(session_id, "inline_preedit", Bool(inline_preedit));
  // show soft cursor on weasel panel but not inline
  rime_api->set_option(session_id, "soft_cursor", Bool(!inline_preedit));
}
