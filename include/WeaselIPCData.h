#pragma once

// ============================================================================
// 青筱（QingXiaoType）P2-A：WeaselIPCData.h 完整新版本
//
// 本文件 = P0 完整版本（weasel-p0-dev/include/WeaselIPCData.h）
//          + P2-A 追加块（见「===== P2-A 追加 =====」标记）。
//
// P2-A 追加内容（全部「追加不动既有」，向后兼容）：
//   1) ExtraCardType 枚举 + ExtraCard 结构体：候选窗「可组合卡片」数据单元
//      （亮点 5：emoji / 云候选 / AI 建议 / 翻译 / 普通卡片）。
//   2) CandidateInfo 末尾追加 std::vector<ExtraCard> extra_cards：
//      随既有 ctx.cand= 行一起 Boost 序列化下发（P2-B 云候选 / P2-D AI 复用）。
//   3) Boost serialize 补齐：ExtraCard 独立序列化函数；CandidateInfo 序列化
//      末尾追加 ar & s.extra_cards（不改既有字段顺序）。
//   4) Context.aux 文本区：P0 已存在（Text 类型），本阶段确认为 P2-E 翻译
//      下发通道；服务端 _Respond 补发 ctx.aux= 文本行（见
//      RimeWithWeasel.p2_a_patch.cpp，不在本头文件改动范围内）。
//   5) P2 message_id 注册表（见文件尾注释）：沿用 P1 扩展通道范式，
//      不新增 IPC 命令枚举。
//
// 兼容性硬约束（与 P0 §6.8 一致）：
//   - 既有枚举值（TextAttributeType / IconType / UIStyle::LayoutType 等）一律不改值。
//   - 既有结构体字段顺序、语义一律不改；新字段一律追加在结构体末尾与序列化末尾。
//   - Boost text archive：新服务端→老客户端时，老客户端的 serialize 只读其已知字段，
//     尾部 extra_cards 数据不被读取（文本 archive 不要求消费全部输入），安全；
//     老服务端→新客户端时，新客户端多写一个字段会在反序列化命中 archive 尾，
//     抛出 archive_exception，由 WeaselIPC/Deserializer.h TryDeserialize 捕获
//     （弹错但不崩溃）。→ 发布策略：TSF 客户端与 Server 同版本升级，不混跑。
// ============================================================================

#include <string>
#include <vector>
#include <boost/serialization/vector.hpp>
#include <boost/serialization/string.hpp>

namespace weasel {

enum TextAttributeType { NONE = 0, HIGHLIGHTED, LAST_TYPE };

struct TextRange {
  TextRange() : start(0), end(0), cursor(-1) {}
  TextRange(int _start, int _end, int _cursor)
      : start(_start), end(_end), cursor(_cursor) {}
  bool operator==(const TextRange& tr) {
    return (start == tr.start && end == tr.end && cursor == tr.cursor);
  }
  bool operator!=(const TextRange& tr) {
    return (start != tr.start || end != tr.end || cursor != tr.cursor);
  }
  int start;
  int end;
  int cursor;
};

struct TextAttribute {
  TextAttribute() : type(NONE) {}
  TextAttribute(int _start, int _end, TextAttributeType _type)
      : range(_start, _end, -1), type(_type) {}
  bool operator==(const TextAttribute& ta) {
    return (range == ta.range && type == ta.type);
  }
  bool operator!=(const TextAttribute& ta) {
    return (range != ta.range || type != ta.type);
  }
  TextRange range;
  TextAttributeType type;
};

struct Text {
  Text() : str(L"") {}
  Text(std::wstring const& _str) : str(_str) {}
  void clear() {
    str.clear();
    attributes.clear();
  }
  bool empty() const { return str.empty(); }
  bool operator==(const Text& txt) {
    if (str != txt.str || (attributes.size() != txt.attributes.size()))
      return false;
    for (size_t i = 0; i < attributes.size(); i++) {
      if ((attributes[i] != txt.attributes[i]))
        return false;
    }
    return true;
  }
  bool operator!=(const Text& txt) {
    if (str != txt.str || (attributes.size() != txt.attributes.size()))
      return true;
    for (size_t i = 0; i < attributes.size(); i++) {
      if ((attributes[i] != txt.attributes[i]))
        return true;
    }
    return false;
  }
  std::wstring str;
  std::vector<TextAttribute> attributes;
};

// ===== P2-A 追加：候选窗「可组合卡片」类型（亮点 5） =====
// 追加在独立枚举末尾，不影响任何既有枚举数值。
// 语义：卡片是挂在主候选列下方的一条横向/纵向扩展区，承载 emoji、云候选、
//       AI 建议、翻译等旁路结果；绝不参与 librime 候选排序（亮点 22 硬约束）。
enum ExtraCardType {
  CARD_NONE = 0,     // 占位/空卡片
  CARD_EMOJI = 1,    // emoji / 花样字卡片（P1 emoji 面板结果可复用此通道渲染）
  CARD_CLOUD = 2,    // 云候选 / 云联想（P2-B）：加载中显示「…」占位，命中后排第 2
  CARD_AI = 3,       // AI 建议卡片（P2-D）：润色/续写/风格切换，走 items 多行建议
  CARD_TRANSLATE = 4,  // 翻译卡片（P2-E）：译文走 body 单行
  CARD_PLAIN = 5,    // 普通/自义卡片（通用兜底）
  CARD_TYPE_LAST
};

// ===== P2-A 追加：单张卡片数据单元 =====
// 字段说明：
//   type      卡片类型（ExtraCardType），决定 WeaselUI 渲染样式与可点击语义。
//   title     卡片标题（可空 wstring），如「云候选」「AI 续写」「翻译」。
//   body      卡片正文单行（可空）：翻译结果、云候选「…」占位、单条建议。
//   items     卡片多行建议项（vector<Text>，可空）：AI 润色/续写的多个候选项。
//   action    动作标识（可空）：用户选中本卡片后上屏的文本或命令串。
//   action_id 动作编号（可选）：服务端回选用；0 表示无编号。
// 序列化：字段顺序即 Boost serialize 顺序，后续新增字段一律追加在 action_id 之后。
struct ExtraCard {
  ExtraCard() : type(CARD_NONE), action_id(0) {}
  ExtraCardType type;
  std::wstring title;
  std::wstring body;
  std::vector<Text> items;
  std::wstring action;
  int action_id;
};

struct CandidateInfo {
  CandidateInfo() {
    currentPage = 0;
    totalPages = 0;
    highlighted = 0;
    is_last_page = false;
  }
  void clear() {
    currentPage = 0;
    totalPages = 0;
    highlighted = 0;
    is_last_page = false;
    candies.clear();
    labels.clear();
    comments.clear();
    extra_cards.clear();  // P2-A：清空卡片区
  }
  bool empty() const { return candies.empty(); }
  bool operator==(const CandidateInfo& ci) {
    if (currentPage != ci.currentPage || totalPages != ci.totalPages ||
        highlighted != ci.highlighted || is_last_page != ci.is_last_page ||
        notequal(candies, ci.candies) || notequal(comments, ci.comments) ||
        notequal(labels, ci.labels))
      return false;
    return true;
  }
  bool operator!=(const CandidateInfo& ci) {
    if (currentPage != ci.currentPage || totalPages != ci.totalPages ||
        highlighted != ci.highlighted || is_last_page != ci.is_last_page ||
        notequal(candies, ci.candies) || notequal(comments, ci.comments) ||
        notequal(labels, ci.labels))
      return true;
    return false;
  }
  bool notequal(std::vector<Text> txtSrc, std::vector<Text> txtDst) {
    if (txtSrc.size() != txtDst.size())
      return true;
    for (size_t i = 0; i < txtSrc.size(); i++) {
      if (txtSrc[i] != txtDst[i])
        return true;
    }
    return false;
  }
  int currentPage;
  bool is_last_page;
  int totalPages;
  int highlighted;
  std::vector<Text> candies;
  std::vector<Text> comments;
  std::vector<Text> labels;
  // P2-A 追加：可组合卡片列表（追加在既有字段末尾）。
  // 空 vector 时 WeaselUI 走老路径（不渲染卡片条），向后兼容。
  std::vector<ExtraCard> extra_cards;
};

struct Context {
  Context() {}
  void clear() {
    preedit.clear();
    aux.clear();
    cinfo.clear();
  }
  bool empty() const { return preedit.empty() && aux.empty() && cinfo.empty(); }
  bool operator==(const Context& ctx) {
    if (preedit == ctx.preedit && aux == ctx.aux && cinfo == ctx.cinfo)
      return true;
    return false;
  }
  bool operator!=(const Context& ctx) { return !(operator==(ctx)); }

  bool operator!() {
    if (preedit.str.empty() && aux.str.empty() && cinfo.candies.empty() &&
        cinfo.labels.empty() && cinfo.comments.empty())
      return true;
    else
      return false;
  }
  Text preedit;
  // P2-A 确认：aux 即「辅助文本带 / tips 行」，P0 已存在（Text 类型）。
  // P2-E 翻译结果经本字段下发：服务端把译文写入 aux.str，
  // 经 ctx.aux=<文本> 行（ContextUpdater.cpp:31 已解析）到 TSF，
  // 再由 WeaselUI 各 Layout 既有的 aux 渲染分支画出。
  // 本阶段不改动本结构体字段；仅在服务端 _Respond 补发 ctx.aux= 行
  // （见 RimeWithWeasel.p2_a_patch.cpp 改动 D-3）。
  Text aux;
  CandidateInfo cinfo;
};
// for icon type in tip
enum IconType { SCHEMA, FULL_SHAPE };
// 由ime管理
struct Status {
  Status()
      : type(SCHEMA),
        ascii_mode(false),
        composing(false),
        disabled(false),
        full_shape(false) {}
  void reset() {
    schema_name.clear();
    schema_id.clear();
    ascii_mode = false;
    composing = false;
    disabled = false;
    full_shape = false;
    type = SCHEMA;
  }
  bool operator==(const Status status) {
    return (status.schema_name == schema_name &&
            status.schema_id == schema_id && status.ascii_mode == ascii_mode &&
            status.composing == composing && status.disabled == disabled &&
            status.full_shape == full_shape && status.type == type);
  }
  // 輸入方案
  std::wstring schema_name;
  // 輸入方案 id
  std::wstring schema_id;
  // 轉換開關
  bool ascii_mode;
  // 寫作狀態
  bool composing;
  // 維護模式（暫停輸入功能）
  bool disabled;
  // 全角状态
  bool full_shape;
  // 图标类型, schema/full_shape
  IconType type;
};

// 用於向前端告知設置信息
struct Config {
  Config() : inline_preedit(false) {}
  void reset() { inline_preedit = false; }
  bool inline_preedit;
};

struct UIStyle {
  enum AntiAliasMode {
    DEFAULT = 0,
    CLEARTYPE = 1,
    GRAYSCALE = 2,
    ALIASED = 3,
    FORCE_DWORD = 0xffffffff
  };

  enum PreeditType { COMPOSITION, PREVIEW, PREVIEW_ALL };
  enum HoverType { NONE, SEMI_HILITE, HILITE };
  enum LayoutType {
    LAYOUT_VERTICAL = 0,
    LAYOUT_HORIZONTAL,
    LAYOUT_VERTICAL_TEXT,
    LAYOUT_VERTICAL_FULLSCREEN,
    LAYOUT_HORIZONTAL_FULLSCREEN,
    // P0 MVP 新增：现代化候选窗布局（圆角卡片式候选、紧凑间距、明暗双主题）。
    // 追加在 LAYOUT_TYPE_LAST 之前，旧枚举值不变，Boost 序列化保持兼容。
    LAYOUT_MODERN,
    LAYOUT_TYPE_LAST
  };

  enum LayoutAlignType { ALIGN_BOTTOM = 0, ALIGN_CENTER, ALIGN_TOP };

  // font face and font point settings
  std::wstring font_face;
  std::wstring label_font_face;
  std::wstring comment_font_face;
  int font_point;
  int label_font_point;
  int comment_font_point;
  int candidate_abbreviate_length;

  bool inline_preedit;
  bool display_tray_icon;
  bool ascii_tip_follow_cursor;
  bool paging_on_scroll;
  bool enhanced_position;
  bool click_to_capture;
  HoverType hover_type;
  AntiAliasMode antialias_mode;
  PreeditType preedit_type;
  // custom icon settings
  std::wstring current_zhung_icon;
  std::wstring current_ascii_icon;
  std::wstring current_half_icon;
  std::wstring current_full_icon;
  // label format and mark_text
  std::wstring label_text_format;
  std::wstring mark_text;
  // layout relative parameters
  LayoutType layout_type;
  LayoutAlignType align_type;
  bool vertical_text_left_to_right;
  bool vertical_text_with_wrap;
  // layout, with key name like style/layout/...
  int min_width;
  int max_width;
  int min_height;
  int max_height;
  int border;
  int margin_x;
  int margin_y;
  int spacing;
  int candidate_spacing;
  int hilite_spacing;
  int hilite_padding_x;
  int hilite_padding_y;
  int round_corner;
  int round_corner_ex;
  int shadow_radius;
  int shadow_offset_x;
  int shadow_offset_y;
  bool vertical_auto_reverse;
  // color scheme
  int text_color;
  int candidate_text_color;
  int candidate_back_color;
  int candidate_shadow_color;
  int candidate_border_color;
  int label_text_color;
  int comment_text_color;
  int back_color;
  int shadow_color;
  int border_color;
  int hilited_text_color;
  int hilited_back_color;
  int hilited_shadow_color;
  int hilited_candidate_text_color;
  int hilited_candidate_back_color;
  int hilited_candidate_shadow_color;
  int hilited_candidate_border_color;
  int hilited_label_text_color;
  int hilited_comment_text_color;
  int hilited_mark_color;
  int prevpage_color;
  int nextpage_color;
  // per client
  int client_caps;
  int baseline;
  int linespacing;

  UIStyle()
      : font_face(),
        label_font_face(),
        comment_font_face(),
        font_point(0),
        label_font_point(0),
        comment_font_point(0),
        candidate_abbreviate_length(0),
        inline_preedit(false),
        display_tray_icon(false),
        ascii_tip_follow_cursor(false),
        paging_on_scroll(false),
        enhanced_position(false),
        click_to_capture(false),
        hover_type(NONE),
        antialias_mode(DEFAULT),
        preedit_type(COMPOSITION),
        current_zhung_icon(),
        current_ascii_icon(),
        current_half_icon(),
        current_full_icon(),
        label_text_format(L"%s."),
        mark_text(),
        layout_type(LAYOUT_VERTICAL),
        align_type(ALIGN_BOTTOM),
        vertical_text_left_to_right(false),
        vertical_text_with_wrap(false),
        min_width(0),
        max_width(0),
        min_height(0),
        max_height(0),
        border(0),
        margin_x(0),
        margin_y(0),
        spacing(0),
        candidate_spacing(0),
        hilite_spacing(0),
        hilite_padding_x(0),
        hilite_padding_y(0),
        round_corner(0),
        round_corner_ex(0),
        shadow_radius(0),
        shadow_offset_x(0),
        shadow_offset_y(0),
        vertical_auto_reverse(false),
        text_color(0),
        candidate_text_color(0),
        candidate_back_color(0),
        candidate_shadow_color(0),
        candidate_border_color(0),
        label_text_color(0),
        comment_text_color(0),
        back_color(0),
        shadow_color(0),
        border_color(0),
        hilited_text_color(0),
        hilited_back_color(0),
        hilited_shadow_color(0),
        hilited_candidate_text_color(0),
        hilited_candidate_back_color(0),
        hilited_candidate_shadow_color(0),
        hilited_candidate_border_color(0),
        hilited_label_text_color(0),
        hilited_comment_text_color(0),
        hilited_mark_color(0),
        prevpage_color(0),
        nextpage_color(0),
        baseline(0),
        linespacing(0),
        client_caps(0) {}
  bool operator!=(const UIStyle& st) {
    return (
        align_type != st.align_type || antialias_mode != st.antialias_mode ||
        preedit_type != st.preedit_type || layout_type != st.layout_type ||
        vertical_text_left_to_right != st.vertical_text_left_to_right ||
        vertical_text_with_wrap != st.vertical_text_with_wrap ||
        paging_on_scroll != st.paging_on_scroll || font_face != st.font_face ||
        label_font_face != st.label_font_face ||
        comment_font_face != st.comment_font_face ||
        hover_type != st.hover_type || font_point != st.font_point ||
        label_font_point != st.label_font_point ||
        comment_font_point != st.comment_font_point ||
        candidate_abbreviate_length != st.candidate_abbreviate_length ||
        inline_preedit != st.inline_preedit || mark_text != st.mark_text ||
        display_tray_icon != st.display_tray_icon ||
        ascii_tip_follow_cursor != st.ascii_tip_follow_cursor ||
        current_zhung_icon != st.current_zhung_icon ||
        current_ascii_icon != st.current_ascii_icon ||
        current_half_icon != st.current_half_icon ||
        current_full_icon != st.current_full_icon ||
        enhanced_position != st.enhanced_position ||
        click_to_capture != st.click_to_capture ||
        label_text_format != st.label_text_format ||
        min_width != st.min_width || max_width != st.max_width ||
        min_height != st.min_height || max_height != st.max_height ||
        border != st.border || margin_x != st.margin_x ||
        margin_y != st.margin_y || spacing != st.spacing ||
        candidate_spacing != st.candidate_spacing ||
        hilite_spacing != st.hilite_spacing ||
        hilite_padding_x != st.hilite_padding_x ||
        hilite_padding_y != st.hilite_padding_y ||
        round_corner != st.round_corner ||
        round_corner_ex != st.round_corner_ex ||
        shadow_radius != st.shadow_radius ||
        shadow_offset_x != st.shadow_offset_x ||
        shadow_offset_y != st.shadow_offset_y ||
        vertical_auto_reverse != st.vertical_auto_reverse ||
        baseline != st.baseline || linespacing != st.linespacing ||
        text_color != st.text_color ||
        candidate_text_color != st.candidate_text_color ||
        candidate_back_color != st.candidate_back_color ||
        candidate_shadow_color != st.candidate_shadow_color ||
        candidate_border_color != st.candidate_border_color ||
        hilited_candidate_border_color != st.hilited_candidate_border_color ||
        label_text_color != st.label_text_color ||
        comment_text_color != st.comment_text_color ||
        back_color != st.back_color || shadow_color != st.shadow_color ||
        border_color != st.border_color ||
        hilited_text_color != st.hilited_text_color ||
        hilited_back_color != st.hilited_back_color ||
        hilited_shadow_color != st.hilited_shadow_color ||
        hilited_candidate_text_color != st.hilited_candidate_text_color ||
        hilited_candidate_back_color != st.hilited_candidate_back_color ||
        hilited_candidate_shadow_color != st.hilited_candidate_shadow_color ||
        hilited_label_text_color != st.hilited_label_text_color ||
        hilited_comment_text_color != st.hilited_comment_text_color ||
        hilited_mark_color != st.hilited_mark_color ||
        prevpage_color != st.prevpage_color ||
        nextpage_color != st.nextpage_color);
  }
};

// ===== P0 MVP 新增：通用扩展通道消息（预留设计） =====
// 为 P1 云候选 / P2 AI 候选、语音、设置中心等提供统一的结构化请求/响应载荷。
// P0 阶段仅启用 message_id 为 L"ping" / L"server_info" 的最小握手子集，
// 完整云端逻辑在后续阶段实现。
//
// ===== P2-A：P2 message_id 注册表（沿用 P1 扩展通道范式） =====
// 全部走 WEASEL_IPC_EXTENSION 命令 + RequestHandler::HandleExtension +
// Client::ExtensionRequest，【不新增 IPC 命令枚举】。payload 为键值文本行
// （P1 范式：type=clipboard / index=N）；结构化数据后续约定为 JSON 文本。
//
//   前缀        message_id 系列                 责任批次   说明
//   ----------  -----------------------------   --------   ------------------------------
//   cloud.*     cloud.query / cloud.done        P2-B       云候选/云联想；服务端异步旁路，
//               cloud.cache_clear                          结果经 CandidateInfo.extra_cards
//                                                          (CARD_CLOUD) 下发
//   ai.*        ai.assist / ai.pick             P2-D       AI 润色/续写；结果经 extra_cards
//                                                          (CARD_AI) 下发，不改 librime 排序
//   translate.* translate.query / translate.done  P2-E    边写边译；译文经 Context.aux 下发
//   voice.*     voice.start / voice.stop        P2-C       离线语音（本地，原则上不联网）；
//               voice.cloud_query                          在线 ASR 走 NetworkGate 白名单
//   privacy.*   privacy.status                  P2-A       查询隐私总开关与各模块状态
//               privacy.summary                            （本批次已落地服务端分支，见
//                                                          RimeWithWeasel.p2_a_patch.cpp）
//
// 命名约定：小写点分；请求动词放第二段（query/pick/start/stop）。
// 未注册 message_id 一律返回 false（HandleExtension 默认 not-supported），
// TSF 端收到 false 即降级为本地行为，绝不崩溃。
struct ExtensionMessage {
  ExtensionMessage() : sequence(0) {}
  // 消息类型标识，见上方注册表
  std::wstring message_id;
  // 结构化载荷：P0/P1 为键值文本行；P2 起约定为 JSON 文本
  std::wstring payload;
  // 请求序号：由请求方自增，用于异步响应配对
  int sequence;
};
}  // namespace weasel
namespace boost {
namespace serialization {
template <typename Archive>
void serialize(Archive& ar, weasel::UIStyle& s, const unsigned int version) {
  ar & s.font_face;
  ar & s.label_font_face;
  ar & s.comment_font_face;
  ar & s.hover_type;
  ar & s.font_point;
  ar & s.label_font_point;
  ar & s.comment_font_point;
  ar & s.candidate_abbreviate_length;
  ar & s.inline_preedit;
  ar & s.align_type;
  ar & s.antialias_mode;
  ar & s.mark_text;
  ar & s.preedit_type;
  ar & s.display_tray_icon;
  ar & s.ascii_tip_follow_cursor;
  ar & s.current_zhung_icon;
  ar & s.current_ascii_icon;
  ar & s.current_half_icon;
  ar & s.current_full_icon;
  ar & s.enhanced_position;
  ar & s.click_to_capture;
  ar & s.label_text_format;
  // layout
  ar & s.layout_type;
  ar & s.vertical_text_left_to_right;
  ar & s.vertical_text_with_wrap;
  ar & s.paging_on_scroll;
  ar & s.min_width;
  ar & s.max_width;
  ar & s.min_height;
  ar & s.max_height;
  ar & s.border;
  ar & s.margin_x;
  ar & s.margin_y;
  ar & s.spacing;
  ar & s.candidate_spacing;
  ar & s.hilite_spacing;
  ar & s.hilite_padding_x;
  ar & s.hilite_padding_y;
  ar & s.round_corner;
  ar & s.round_corner_ex;
  ar & s.shadow_radius;
  ar & s.shadow_offset_x;
  ar & s.shadow_offset_y;
  ar & s.vertical_auto_reverse;
  // color scheme
  ar & s.text_color;
  ar & s.candidate_text_color;
  ar & s.candidate_back_color;
  ar & s.candidate_shadow_color;
  ar & s.candidate_border_color;
  ar & s.label_text_color;
  ar & s.comment_text_color;
  ar & s.back_color;
  ar & s.shadow_color;
  ar & s.border_color;
  ar & s.hilited_text_color;
  ar & s.hilited_back_color;
  ar & s.hilited_shadow_color;
  ar & s.hilited_candidate_text_color;
  ar & s.hilited_candidate_back_color;
  ar & s.hilited_candidate_shadow_color;
  ar & s.hilited_candidate_border_color;
  ar & s.hilited_label_text_color;
  ar & s.hilited_comment_text_color;
  ar & s.hilited_mark_color;
  ar & s.prevpage_color;
  ar & s.nextpage_color;
  // per client
  ar & s.client_caps;
  ar & s.baseline;
  ar & s.linespacing;
}

template <typename Archive>
void serialize(Archive& ar,
               weasel::CandidateInfo& s,
               const unsigned int version) {
  ar & s.currentPage;
  ar & s.totalPages;
  ar & s.highlighted;
  ar & s.is_last_page;
  ar & s.candies;
  ar & s.comments;
  ar & s.labels;
  // P2-A 追加：卡片列表。追加在既有 7 个字段之后，
  // 顺序固定，后续新增字段一律再往后加。空 vector 序列化后为长度 0，
  // 老逻辑反序列化时（新字段缺失）由 TryDeserialize 兜底。
  ar & s.extra_cards;
}

// ===== P2-A 追加：ExtraCard 独立序列化函数 =====
// ExtraCardType 为枚举（int），随 type 字段一并读写。
template <typename Archive>
void serialize(Archive& ar, weasel::ExtraCard& s, const unsigned int version) {
  ar & s.type;
  ar & s.title;
  ar & s.body;
  ar & s.items;
  ar & s.action;
  ar & s.action_id;
}

template <typename Archive>
void serialize(Archive& ar, weasel::Text& s, const unsigned int version) {
  ar & s.str;
  ar & s.attributes;
}
template <typename Archive>
void serialize(Archive& ar,
               weasel::TextAttribute& s,
               const unsigned int version) {
  ar & s.range;
  ar & s.type;
}
template <typename Archive>
void serialize(Archive& ar, weasel::TextRange& s, const unsigned int version) {
  ar & s.start;
  ar & s.end;
  ar & s.cursor;
}
template <typename Archive>
void serialize(Archive& ar,
               weasel::ExtensionMessage& s,
               const unsigned int version) {
  ar & s.message_id;
  ar & s.payload;
  ar & s.sequence;
}
}  // namespace serialization
}  // namespace boost
