// SettingsStore.cpp — 青简 P1-B3 设置中心数据层（实现）
#include "stdafx.h"

#include "SettingsStore.h"

#include <WeaselUtility.h>

namespace weasel {

// 设置项定义表（静态）。
// ---------------------------------------------------------------------------
// 【硬约束】tool/ 段键路径必须与《基线约定-P1工具模式协议.md》§5 逐键一致：
//   tool/clipboard/enabled
//   tool/clipboard/max_entries
//   tool/emoji/enabled
//   tool/v_mode/enabled
//   tool/hotkey/clipboard
//   tool/hotkey/emoji
// 外观段键路径与 RimeWithWeasel.cpp::_UpdateUIStyle 的读取路径逐一对齐。
// 修改本表前，请先核对上述两处读取代码，避免键名漂移。
// ---------------------------------------------------------------------------
static const std::vector<SettingsItem> BuildTable() {
  return {
      // ===== 外观（style/*，键路径对齐 _UpdateUIStyle）=====
      {"style.font_point", "style/font_point", SettingsType::kInt,
       SettingsGroup::kAppearance, "候选窗字号（磅）",
       12, false, "12", ""},
      {"style.label_font_point", "style/label_font_point", SettingsType::kInt,
       SettingsGroup::kAppearance, "候选编号字号（0=跟随候选字号）",
       0, false, "0", ""},
      {"style.comment_font_point", "style/comment_font_point", SettingsType::kInt,
       SettingsGroup::kAppearance, "候选注释字号（0=跟随候选字号）",
       0, false, "0", ""},
      {"style.color_scheme", "style/color_scheme", SettingsType::kEnum,
       SettingsGroup::kAppearance, "浅色主题（下拉项来自 preset_color_schemes）",
       0, false, "aqua", ""},
      {"style.color_scheme_dark", "style/color_scheme_dark", SettingsType::kEnum,
       SettingsGroup::kAppearance, "深色主题（留空=未配置，系统深色时回退内置深色方案）",
       0, false, "", ""},
      {"style.layout.type", "style/layout/type", SettingsType::kEnum,
       SettingsGroup::kAppearance,
       "布局方向（modern 为青简 P1 预留，0.17.4 基线未注册，需服务端扩展枚举后生效）",
       0, false, "",
       "vertical,horizontal,vertical_text,vertical+fullscreen,horizontal+fullscreen,modern"},
      {"style.layout.round_corner", "style/layout/round_corner", SettingsType::kInt,
       SettingsGroup::kAppearance, "圆角半径（别名 hilited_corner_radius）",
       4, false, "4", ""},

      // ===== 工具（tool/*，逐键对齐基线 §5）=====
      {"tool.clipboard.enabled", "tool/clipboard/enabled", SettingsType::kBool,
       SettingsGroup::kTool, "剪贴板历史开关",
       0, true, "true", ""},
      {"tool.clipboard.max_entries", "tool/clipboard/max_entries", SettingsType::kInt,
       SettingsGroup::kTool, "剪贴板历史条数上限",
       50, false, "50", ""},
      {"tool.emoji.enabled", "tool/emoji/enabled", SettingsType::kBool,
       SettingsGroup::kTool, "emoji/符号面板开关",
       0, true, "true", ""},
      {"tool.v_mode.enabled", "tool/v_mode/enabled", SettingsType::kBool,
       SettingsGroup::kTool, "V/U 模式（日期/计算/大写等）开关",
       0, true, "true", ""},
      {"tool.hotkey.clipboard", "tool/hotkey/clipboard", SettingsType::kString,
       SettingsGroup::kTool, "剪贴板面板热键",
       0, false, "Ctrl+Alt+V", ""},
      {"tool.hotkey.emoji", "tool/hotkey/emoji", SettingsType::kString,
       SettingsGroup::kTool, "emoji 面板热键",
       0, false, "Ctrl+Alt+.", ""},

      // ===== 通用 =====
      {"style.inline_preedit", "style/inline_preedit", SettingsType::kBool,
       SettingsGroup::kGeneral, "行内编辑（在录入窗口显示编码）",
       0, false, "false", ""},
      {"style.enhanced_position", "style/enhanced_position", SettingsType::kBool,
       SettingsGroup::kGeneral, "候选窗跟随光标位置（多屏/高分屏建议开）",
       0, true, "true", ""},
      {"global_ascii", "global_ascii", SettingsType::kBool,
       SettingsGroup::kGeneral, "全局 ASCII 模式",
       0, false, "false", ""},
  };
}

const std::vector<SettingsItem>& SettingsStore::Table() {
  static const std::vector<SettingsItem> table = BuildTable();
  return table;
}

SettingsStore::SettingsStore() {
  // 与 UIStyleSettings 完全同源：find_module("levers") 取 RimeLeversApi。
  RimeApi* rime = rime_get_api();
  if (!rime)
    return;
  RimeModule* levers = rime->find_module("levers");
  if (!levers)
    return;
  api_ = (RimeLeversApi*)levers->get_api();
  if (!api_)
    return;
  // 配置文件名 "weasel"；补丁名取独立命名空间，落盘到 weasel.custom.yaml 的 patch: 段。
  settings_ = api_->custom_settings_init("weasel", "Weasel::SettingsStore");
}

SettingsStore::~SettingsStore() {
  Close();
}

bool SettingsStore::Open() {
  if (!IsReady())
    return false;
  // load_settings 读取 weasel.custom.yaml 补丁到内存（与 Configurator::configure_ui 一致）。
  return !!api_->load_settings(settings_);
}

void SettingsStore::Close() {
  if (api_ && settings_) {
    api_->custom_settings_destroy(settings_);
  }
  settings_ = nullptr;
  api_ = nullptr;
  cache_.clear();
  pending_.clear();
  dirty_ = false;
}

const SettingsItem* SettingsStore::FindItem(const std::string& item_id) const {
  for (const auto& item : Table()) {
    if (item_id == item.item_id)
      return &item;
  }
  return nullptr;
}

void SettingsStore::ReadOneFromConfig(const SettingsItem* item, RimeConfig* config) {
  const char* path = item->yaml_path;
  SettingValue v;
  switch (item->type) {
    case SettingsType::kInt: {
      int n = 0;
      if (rime_get_api()->config_get_int(config, path, &n)) {
        v = SettingValue::MakeInt(n);
      } else {
        v = SettingValue::MakeInt(item->default_int);
      }
      break;
    }
    case SettingsType::kBool: {
      Bool b = False;
      if (rime_get_api()->config_get_bool(config, path, &b)) {
        v = SettingValue::MakeBool(!!b);
      } else {
        v = SettingValue::MakeBool(item->default_bool);
      }
      break;
    }
    case SettingsType::kString:
    case SettingsType::kColor:
    case SettingsType::kEnum: {
      // config_get_string 读取（与 RimeWithWeasel.cpp 的 style/* 读取同一 API）。
      const int BUF_SIZE = 511;
      char buf[BUF_SIZE + 1] = {0};
      if (rime_get_api()->config_get_string(config, path, buf, BUF_SIZE)) {
        v = SettingValue::MakeString(std::string(buf));
        v.type = item->type;
      } else {
        v = SettingValue::MakeString(item->default_string ? item->default_string : "");
        v.type = item->type;
      }
      break;
    }
  }
  cache_[item->item_id] = v;
}

bool SettingsStore::LoadAll() {
  if (!IsReady())
    return false;
  // 每次打开都 load_settings，保证外部手改 weasel.custom.yaml 后重开 GUI 能读到。
  if (!api_->load_settings(settings_))
    return false;
  RimeConfig config = {0};
  api_->settings_get_config(settings_, &config);
  cache_.clear();
  pending_.clear();
  dirty_ = false;
  for (const auto& item : Table()) {
    ReadOneFromConfig(&item, &config);
  }
  return true;
}

bool SettingsStore::SaveAll() {
  if (!IsReady())
    return false;
  // 无改动直接成功，不触碰文件。
  if (!dirty_)
    return true;
  for (const auto& kv : pending_) {
    const SettingsItem* item = FindItem(kv.first);
    if (!item)
      continue;
    const SettingValue& v = kv.second;
    switch (item->type) {
      case SettingsType::kBool:
        // customize_bool: RimeBool (True/False)
        api_->customize_bool(settings_, item->yaml_path,
                             v.bool_value ? True : False);
        break;
      case SettingsType::kInt:
        api_->customize_int(settings_, item->yaml_path, v.int_value);
        break;
      case SettingsType::kString:
      case SettingsType::kColor:
      case SettingsType::kEnum:
        api_->customize_string(settings_, item->yaml_path, v.string_value.c_str());
        break;
    }
  }
  // save_settings 落盘 weasel.custom.yaml；随后由 Configurator 触发重新部署。
  bool ok = !!api_->save_settings(settings_);
  if (ok) {
    pending_.clear();
    dirty_ = false;
  }
  return ok;
}

SettingValue SettingsStore::Get(const std::string& item_id) const {
  auto it = cache_.find(item_id);
  if (it != cache_.end())
    return it->second;
  const SettingsItem* item = FindItem(item_id);
  if (!item)
    return SettingValue();
  // 未加载/未命中：按类型返回默认值
  switch (item->type) {
    case SettingsType::kInt:
      return SettingValue::MakeInt(item->default_int);
    case SettingsType::kBool:
      return SettingValue::MakeBool(item->default_bool);
    default:
      return SettingValue::MakeString(item->default_string ? item->default_string : "");
  }
}

int SettingsStore::GetInt(const std::string& item_id) const {
  return Get(item_id).int_value;
}

bool SettingsStore::GetBool(const std::string& item_id) const {
  return Get(item_id).bool_value;
}

std::string SettingsStore::GetString(const std::string& item_id) const {
  return Get(item_id).string_value;
}

bool SettingsStore::SetInt(const std::string& item_id, int v) {
  const SettingsItem* item = FindItem(item_id);
  if (!item || item->type != SettingsType::kInt)
    return false;
  SettingValue nv = SettingValue::MakeInt(v);
  cache_[item_id] = nv;
  pending_[item_id] = nv;
  dirty_ = true;
  return true;
}

bool SettingsStore::SetBool(const std::string& item_id, bool v) {
  const SettingsItem* item = FindItem(item_id);
  if (!item || item->type != SettingsType::kBool)
    return false;
  SettingValue nv = SettingValue::MakeBool(v);
  cache_[item_id] = nv;
  pending_[item_id] = nv;
  dirty_ = true;
  return true;
}

bool SettingsStore::SetString(const std::string& item_id, const std::string& v) {
  const SettingsItem* item = FindItem(item_id);
  if (!item || (item->type != SettingsType::kString &&
                item->type != SettingsType::kColor &&
                item->type != SettingsType::kEnum))
    return false;
  SettingValue nv = SettingValue::MakeString(v);
  nv.type = item->type;
  cache_[item_id] = nv;
  pending_[item_id] = nv;
  dirty_ = true;
  return true;
}

std::vector<const SettingsItem*> SettingsStore::ItemsByGroup(SettingsGroup g) const {
  std::vector<const SettingsItem*> out;
  for (const auto& item : Table()) {
    if (item.group == g)
      out.push_back(&item);
  }
  return out;
}

}  // namespace weasel
