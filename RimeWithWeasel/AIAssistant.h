// ============================================================================
// 青筱（QingXiaoType）P2-D：AI 助手（AIAssistant）双路线 —— 本地模板 + 可插拔云端 API
//
// 归属工程：RimeWithWeasel/（与 NetworkGate / ClipboardManager 同工程，由
//           RimeWithWeaselHandler 构造并持有 m_ai_assistant）。
//
// 设计基线（必读，按序）：
//   - P2-A 契约：include/WeaselIPCData.h（ExtraCardType::CARD_AI=3 / ExtraCard /
//     CandidateInfo.extra_cards / ExtensionMessage message_id=ai.*）。
//   - P2-A 隐私闸门：NetworkGate::Gate("ai_assistant", endpoint)。
//   - 主文档亮点 22：= 键唤起 / OpenAI 兼容协议 / 用户自带 endpoint+key /
//     结果以卡片展示 / 不记录交互 / 【绝不改 librime 候选排序】。
//
// 本文件头设计原则：
//   1) 纯逻辑层（本地模板引擎）只用 std::wstring / STL，不依赖 Windows / librime，
//      可在 Linux 下用 g++ 直接单测（见 test_ai_assistant.cpp）。
//   2) 云端路线只定义「可插拔」抽象：AIAssistantProvider 基类 + CloudApiProvider
//      + IHttpPost 接口。真实 WinHTTP 联网实现列为已知缺口（Windows 侧补），
//      本批次默认装配 NullHttpPost（调用即失败 → 自动降级本地模板）。
//   3) 隐私边界：云端路线在真正发包前必须过 NetworkGate；本地路线零上传、不过 Gate。
//   4) 结果承载：统一产出 std::vector<ExtraCard>（CARD_AI），由服务端补丁
//      （RimeWithWeasel.p2_d_patch.cpp）挂到 CandidateInfo.extra_cards 下发，
//      绝不回写 candies 排序。
// ============================================================================
#pragma once

#include <string>
#include <vector>
#include <memory>

// P2-A 契约：ExtraCard / ExtraCardType / Text。
// 在 Windows 工程里它来自 weasel-p2-dev/include/WeaselIPCData.h；
// 为了让本头在 Linux g++ 单测时也能独立编译，下方用「最小类型镜像」兜底：
// 单测宏 AIA_SelfTest 定义时，不真正 include WeaselIPCData.h，改用本文件末尾的
// 极简镜像；正式编译（Windows）时走真正的 WeaselIPCData.h。
#ifndef AIA_SelfTest
#include "WeaselIPCData.h"  // 提供 weasel::ExtraCard / ExtraCardType / Text
#else
namespace weasel {
struct Text {
  Text() = default;
  Text(std::wstring const& s) : str(s) {}
  std::wstring str;
};
enum ExtraCardType { CARD_NONE = 0, CARD_AI = 3, CARD_TYPE_LAST };
struct ExtraCard {
  ExtraCard() : type(CARD_AI), action_id(0) {}
  ExtraCardType type;
  std::wstring title;
  std::wstring body;
  std::vector<Text> items;
  std::wstring action;
  int action_id;
};
}  // namespace weasel
#endif

namespace weasel {

// --------------------------------------------------------------------------
// 动作类型（ai/actions 启用列表与 payload action_type 的枚举镜像）。
// 字符串 ↔ 枚举的转换在 .cpp 内集中实现，避免散落。
// --------------------------------------------------------------------------
enum class AIActionType {
  kUnknown = 0,
  kPolish,     // 润色：规范化标点/空格/措辞
  kContinue,   // 续写：基于结尾补全候选
  kTranslate,  // 翻译：本地小词典（云端路线才是真 MT）
  kProofread,  // 纠错：常见叠字/标点/夹空格修正
};

// 动作类型 ↔ 字符串（payload / 配置键）互转。
AIActionType AIActionFromString(const std::wstring& s);
std::wstring AIActionToString(AIActionType a);
std::wstring AIActionZhName(AIActionType a);  // 卡片标题用中文名

// --------------------------------------------------------------------------
// 一次 AI 助手请求的入参。
//   text        用户要处理的文本（TSF 经 payload 传入，或服务端从 preedit 兜底）。
//   action      动作类型。
// --------------------------------------------------------------------------
struct AIRequest {
  std::wstring text;
  AIActionType action{AIActionType::kUnknown};
};

// --------------------------------------------------------------------------
// 一次 AI 助手结果（Provider 内部产物）。
//   ok          是否成功产出（失败 → 上层降级/提示）。
//   title       卡片标题（如「AI 润色」）。
//   primary     单行主结果（写 ExtraCard.body）。
//   suggestions 多行建议（写 ExtraCard.items）；空则不写 items。
//   note        提示/降级说明（写进 body 末尾或 aux），如「已切本地模板」。
// --------------------------------------------------------------------------
struct AIResult {
  bool ok{false};
  std::wstring title;
  std::wstring primary;
  std::vector<std::wstring> suggestions;
  std::wstring note;
};

// --------------------------------------------------------------------------
// Provider 抽象（可插拔核心）。
// 路线 A（云端）= CloudApiProvider；路线 B（本地模板）= LocalTemplateProvider。
// AIAssistant 仅依赖本基类，运行期按 ai/route 配置装配其一（或云端失败回落本地）。
// --------------------------------------------------------------------------
class AIAssistantProvider {
 public:
  virtual ~AIAssistantProvider() = default;

  // 路由名（"local" / "cloud"），日志与卡片 note 回显用。
  virtual const wchar_t* RouteName() const = 0;

  // 本 Provider 当前是否可用（本地恒 true；云端看 endpoint/key 是否配齐）。
  virtual bool IsAvailable() const = 0;

  // 同步执行一次请求。注意：云端实现【必须】由调用方放到工作线程，
  // 不得阻塞 IPC/按键线程；本地实现为纯内存变换，瞬时返回。
  virtual AIResult Run(const AIRequest& req) = 0;
};

// --------------------------------------------------------------------------
// HTTP 客户端抽象（可插拔）。
// P2-B 尚未交付统一 HTTP 层；本批次先定义最小接口：POST(endpoint, json_body)
// 返回 HTTP 状态码与响应体。真实实现（WinHTTP / libcurl）在 Windows 侧补，
// 列为已知缺口；本批次默认装配 NullHttpPost（恒返回失败 → 降级）。
// --------------------------------------------------------------------------
class IHttpPost {
 public:
  virtual ~IHttpPost() = default;
  struct Response {
    int status_code{0};      // 0 = 连接/超时失败
    std::string body;        // UTF-8 响应体
  };
  // timeout_ms：超时上限（默认由 AIAssistant 配置，如 8000ms）。
  virtual Response Post(const std::string& endpoint,
                        const std::string& json_utf8,
                        int timeout_ms) = 0;
};

// 空实现：Linux 单测 / Windows 未接 WinHTTP 时兜底，Post 恒失败。
class NullHttpPost : public IHttpPost {
 public:
  Response Post(const std::string&, const std::string&, int) override {
    return Response{0, ""};
  }
};

// --------------------------------------------------------------------------
// 路线 B：本地模板 Provider（纯规则引擎，零上传，不过 Gate，可离线）。
// 至少 4 类动作：润色 / 续写 / 翻译 / 纠错。全部为确定性规则，g++ 可自测。
// --------------------------------------------------------------------------
class LocalTemplateProvider : public AIAssistantProvider {
 public:
  const wchar_t* RouteName() const override { return L"local"; }
  bool IsAvailable() const override { return true; }  // 恒可用
  AIResult Run(const AIRequest& req) override;

  // 以下为各动作的纯函数实现（public 以便单测直接命中）。
  static std::wstring Polish(const std::wstring& text);
  static std::vector<std::wstring> Continue(const std::wstring& text);
  static std::wstring Translate(const std::wstring& text);
  static std::pair<std::wstring, std::wstring> Proofread(
      const std::wstring& text);  // → (修正后文本, 修改说明)
};

// --------------------------------------------------------------------------
// 路线 A：云端 OpenAI 兼容 API Provider（框架）。
//   - endpoint 形如 https://api.example.com/v1/chat/completions
//   - key 由外部（Windows Credential Manager / 环境变量）注入，【不落 YAML 明文】。
//   - Run() 内构造 OpenAI Chat Completions JSON，经 IHttpPost::Post 发出；
//     真实 TLS/WinHTTP 为已知缺口，本批次用 NullHttpPost → Run 返回 !ok，
//     由上层 AIAssistant 降级到 LocalTemplateProvider。
//   - 隐私约束：调用前【必须】由上层（AIAssistant::HandleRequest）先过
//     NetworkGate::Gate("ai_assistant", endpoint)；本类不自行联网绕过 Gate。
// --------------------------------------------------------------------------
class CloudApiProvider : public AIAssistantProvider {
 public:
  CloudApiProvider(const std::string& endpoint,
                   const std::string& key_utf8,
                   std::unique_ptr<IHttpPost> http);

  const wchar_t* RouteName() const override { return L"cloud"; }
  bool IsAvailable() const override;  // endpoint 与 key 均非空才可用
  AIResult Run(const AIRequest& req) override;

  // 构造 OpenAI Chat Completions 请求体（纯函数，可单测 JSON 拼装）。
  static std::string BuildRequestBody(const AIRequest& req);
  // 从 OpenAI 响应体抽取 choices[0].message.content（极简解析，可单测）。
  static std::wstring ExtractContent(const std::string& response_body);

 private:
  std::string endpoint_;
  std::string key_;  // 内存中持有，析构即释放；不写盘
  std::unique_ptr<IHttpPost> http_;
  int timeout_ms_{8000};
};

// --------------------------------------------------------------------------
// 闸门窄接口（与 P2-B CloudCandidateService::IGateChecker 同范式）。
// 生产侧用 3 行小适配器包 NetworkGate（见 RimeWithWeasel.p2_d_patch.cpp），
// Linux 单测注入 FakeGate。这样 AIAssistant 核心不依赖 windows.h / rime_api.h，
// 可在 g++ 下独立编译。
// --------------------------------------------------------------------------
class IAiGateChecker {
 public:
  virtual ~IAiGateChecker() = default;
  // 返回 true 才允许联网。等价 NetworkGate::Gate(module_id, endpoint)。
  virtual bool Allow(const std::string& module_id,
                     const std::string& endpoint) const = 0;
};

// --------------------------------------------------------------------------
// 门面：AIAssistant。由 RimeWithWeaselHandler 持有。
//   - Configure()：从 weasel.yaml ai/ 段读路由/端点/超时/启用动作。
//   - SetGate()：注入 IAiGateChecker（生产为包 NetworkGate 的适配器）。
//   - HandleRequest()：分发到当前 Provider；云端先过 Gate，失败/超时降级本地；
//     结果统一组装成 std::vector<ExtraCard>（CARD_AI），供服务端挂 extra_cards。
// --------------------------------------------------------------------------

class AIAssistant {
 public:
  AIAssistant();
  ~AIAssistant();

  // 配置入参（由 Handler Initialize 读 weasel.yaml 后注入，避免本类直接依赖 rime_api）。
  struct Config {
    bool enabled{false};                 // ai/enabled，默认 false
    std::wstring route{L"local"};       // ai/route：local | cloud
    std::string cloud_endpoint;          // ai/cloud_endpoint
    std::string cloud_key;               // ai/cloud_key_ref 解析后的明文 key（仅内存）
    std::vector<AIActionType> actions;  // ai/actions 启用的动作列表
    int timeout_ms{8000};                // ai/timeout_ms
    bool gate_allow_cloud{false};        // 由 SetGate + HandleRequest 实时查询填充
  };

  void Configure(const Config& cfg);
  void SetGate(IAiGateChecker* gate) { gate_ = gate; }  // 不拥有，借用

  // 动作是否在 ai/actions 启用列表内（空列表 = 全部启用，兜底）。
  bool ActionEnabled(AIActionType a) const;

  // 核心分发：返回 0..N 张 CARD_AI 卡片。text 为空/动作未启用 → 返回空 vector。
  // 云端路线：未过 Gate / 失败 / 超时 → note 标注并自动回落本地模板 Provider。
  std::vector<ExtraCard> HandleRequest(const std::wstring& text,
                                       AIActionType action);

  // 供 ai.pick 回查：根据卡片 action_id 取该卡片内第 idx 条建议文本。
  // cards 为 HandleRequest 产物的最近一次快照（由服务端存于 session_status）。
  static std::wstring PickItem(const std::vector<ExtraCard>& cards,
                               int action_id,
                               int item_index);

 private:
  // 用一次 AIResult 组装一张 CARD_AI（action=insert/replace 语义见设计文档）。
  static ExtraCard MakeCard(const AIResult& r, AIActionType action,
                            const std::wstring& route_note);

  Config cfg_;
  IAiGateChecker* gate_{nullptr};  // 不拥有，借用 Handler 注入的适配器
  std::unique_ptr<LocalTemplateProvider> local_;
  std::unique_ptr<IHttpPost> http_;  // 默认 NullHttpPost
};

}  // namespace weasel
