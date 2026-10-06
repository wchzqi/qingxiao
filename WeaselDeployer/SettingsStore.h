// SettingsStore.h — 青简 P1-B3 设置中心数据层
//
// 职责：
//   1. 设置项定义表（kSettingsTable）：项ID × weasel.yaml 键路径 × 类型 × 默认值 × 说明。
//      工具段键路径与《基线约定-P1工具模式协议.md》§5 逐键对齐，不得自行增删键名。
//   2. 读写封装：LoadAll()/SaveAll()/Get*/Set*。
//      读取走与 WeaselDeployer/UIStyleSettings.cpp 同源的 rime_levers_api：
//        find_module("levers") -> RimeLeversApi -> custom_settings_init("weasel", ...)
//        -> load_settings -> settings_get_config -> rime_api->config_get_*
//      服务端（RimeWithWeaselHandler，P1-B1/B2）经
//        rime_api->config_open("weasel") + config_get_* 读取同一批键，两侧键路径一致。
//   3. 双向同步：
//      GUI->YAML：Set* 改内存 -> SaveAll() 调 customize_* 写补丁 -> save_settings()
//                 落盘 weasel.custom.yaml -> 由 Configurator::Run 触发 UpdateWorkspace()
//                 （即 rime->deploy() + deploy_config_file("weasel.yaml")）。
//      YAML->GUI：每次打开设置对话框先调 LoadAll()，重读合并后的 weasel.yaml，
//                 外部手改 YAML 的结果即被看到（无内存缓存跨次残留）。
//
// 注意：本类不直接触发部署；SaveAll 只负责把补丁写盘，部署由调用方
//       （Configurator::configure_* 模式）在对话框 IDOK 后统一触发。
//
// 命名空间 weasel 保留（派生最小化）。
#pragma once

#include <map>
#include <string>
#include <vector>

#pragma warning(disable : 4005)
#include <rime_api.h>
#include <rime_levers_api.h>
#pragma warning(default : 4005)

namespace weasel {

// 设置值类型
enum class SettingsType {
  kInt = 0,     // 整型
  kBool = 1,    // 布尔
  kString = 2,  // 字符串（含热键、scheme id）
  kColor = 3,   // 颜色，写成 "0xRRGGBB" / "#RRGGBB" 字符串（服务端 _RimeGetColor 解析）
  kEnum = 4,    // 枚举，字符串取值，允许值见 enum_values
};

// 设置分组（供设置对话框分页/分组）
enum class SettingsGroup {
  kAppearance = 0,  // 外观
  kTool = 1,        // 工具（剪贴板/emoji/V模式/热键）
  kGeneral = 2,     // 通用
};

// 单个设置项的静态定义
struct SettingsItem {
  const char* item_id;     // 项ID（英文短横线，全局唯一，如 "clipboard.enabled"）
  const char* yaml_path;   // weasel.yaml 键路径（/ 分隔，如 "tool/clipboard/enabled"）
  SettingsType type;       // 值类型
  SettingsGroup group;     // 所属分组
  const char* desc_zh;     // 中文说明（界面 tooltip / 列表展示）
  // 默认值（按 type 取用；未配置或读取失败时回退）
  int default_int;
  bool default_bool;
  const char* default_string;
  // kEnum 允许值（逗号分隔，如 "vertical,horizontal,vertical_text,..."）；其余类型留空
  const char* enum_values;
};

// 运行时取值（变体）
struct SettingValue {
  SettingsType type{SettingsType::kString};
  int int_value{0};
  bool bool_value{false};
  std::string string_value;

  static SettingValue MakeInt(int v) {
    SettingValue x;
    x.type = SettingsType::kInt;
    x.int_value = v;
    return x;
  }
  static SettingValue MakeBool(bool v) {
    SettingValue x;
    x.type = SettingsType::kBool;
    x.bool_value = v;
    return x;
  }
  static SettingValue MakeString(const std::string& v) {
    SettingValue x;
    x.type = SettingsType::kString;
    x.string_value = v;
    return x;
  }
};

class SettingsStore {
 public:
  SettingsStore();
  ~SettingsStore();

  SettingsStore(const SettingsStore&) = delete;
  SettingsStore& operator=(const SettingsStore&) = delete;

  // 打开/关闭 weasel 自定义配置（对应 UIStyleSettings 的生命周期）。
  // 失败时返回 false，IsReady() 为 false，后续 Get*/Set* 回退默认值。
  bool Open();
  void Close();
  bool IsReady() const { return api_ && settings_; }

  // 每次打开设置对话框时调用：load_settings + 把全部项读入内存缓存。
  // 外部手改 weasel.custom.yaml 后，下一次 LoadAll 即生效（双向同步 YAML->GUI）。
  bool LoadAll();

  // 把内存中被 Set* 改动过的项写补丁并 save_settings 落盘。
  // 返回 true 表示补丁已写盘；调用方据此决定是否触发重新部署。
  bool SaveAll();

  // 定义表（只读）
  static const std::vector<SettingsItem>& Table();

  // 按项ID取值（未读到则返回该项默认值）
  SettingValue Get(const std::string& item_id) const;
  int GetInt(const std::string& item_id) const;
  bool GetBool(const std::string& item_id) const;
  std::string GetString(const std::string& item_id) const;

  // 按项ID改内存（仅标记 dirty，不写盘；统一在 SaveAll 落盘）。
  // 返回 false 表示项ID不存在/类型不匹配。
  bool SetInt(const std::string& item_id, int v);
  bool SetBool(const std::string& item_id, bool v);
  bool SetString(const std::string& item_id, const std::string& v);

  bool IsDirty() const { return dirty_; }

  // 便捷：取某分组下的项（供对话框一次性 populate）
  std::vector<const SettingsItem*> ItemsByGroup(SettingsGroup g) const;

 private:
  const SettingsItem* FindItem(const std::string& item_id) const;
  // 用合并后的 RimeConfig 读单项，写入缓存
  void ReadOneFromConfig(const SettingsItem* item, RimeConfig* config);

  RimeLeversApi* api_{nullptr};
  RimeCustomSettings* settings_{nullptr};
  std::map<std::string, SettingValue> cache_;  // item_id -> 当前值
  std::map<std::string, SettingValue> pending_;  // item_id -> 待写值（dirty 集合）
  bool dirty_{false};                            // 是否有未写盘改动
};

}  // namespace weasel
