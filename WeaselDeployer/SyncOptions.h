// SyncOptions.h — 青简 P1-B6 无账号同步：installation.yaml 读写
//
// 职责：
//   读写 %AppData%\Rime\installation.yaml（即 WeaselUserDataPath()/installation.yaml）
//   中的两个键：
//     installation_id : <uuid>        本机实例 ID（多设备合并时区分来源）
//     sync_dir        : <path>        云盘同步目录（由本对话框图形化选择）
//
// 为什么不用 yaml-cpp / rime_levers_api：
//   - WeaselDeployer 只链 rime.lib，工程里没有 yaml-cpp（见 WeaselDeployer.vcxproj
//     AdditionalDependencies）；引入新依赖违反「禁止凭空引入新依赖」硬约束。
//   - rime_levers_api 的 custom_settings 面向 weasel.yaml/switcher，不管 installation.yaml。
//   - installation.yaml 是 librime installation 模块生成的**扁平** YAML（只有顶层
//     key: value，无嵌套、无列表、无锚点），用轻量「逐行 key: value」解析即可完整覆盖，
//     与 Configurator.cpp 里 CreateFileIfNotExist 那种最小文件操作风格一致。
//
// 写盘策略（幂等、可重入、不破坏既有字段）：
//   - 读：整文件按行读，命中 `^\s*<key>\s*:` 即取冒号后值（去引号、trim）；
//         其余行原样保留。
//   - 写：在内存里逐行处理——命中已有键则**原地替换该行的值**，未命中则在文件末尾
//         `追加一行 key: value`；任何其他字段（含注释、空行、未知键）逐字节保留。
//   - 整文件以 UTF-8（无 BOM）重写。sync_dir 以 UTF-8 写盘，与 librime 读取编码一致。
//
// 注意：本类只做文件 I/O，不触碰 librime、不持有 mutex、不触发同步；
//       真正的「立即同步」由 Configurator::SyncUserData() 完成。
#pragma once

#include <filesystem>
#include <string>

namespace weasel {

class SyncOptions {
 public:
  SyncOptions();
  ~SyncOptions();

  SyncOptions(const SyncOptions&) = delete;
  SyncOptions& operator=(const SyncOptions&) = delete;

  // 读 installation.yaml 到内存。文件不存在视为空配置（不报错），返回 true。
  // 磁盘损坏/不可读时返回 false，此后各 Get* 返回空串。
  bool Load();

  // installation_id（UTF-8）。空串表示尚未生成。
  const std::string& installation_id() const { return installation_id_; }
  // sync_dir（UTF-8，Windows 原生路径）。空串表示未配置（用 librime 默认目录）。
  const std::string& sync_dir() const { return sync_dir_; }

  // 把选定的云盘目录写回 installation.yaml 的 sync_dir（UTF-8）。
  // 幂等：目录相同也重写一次（保证落盘）；不破坏其他字段。返回是否写盘成功。
  bool SetSyncDir(const std::string& utf8_dir);

  // 若 installation_id 缺失，生成一个新的 UUID（小写连字符形式，与 librime 一致）
  // 并写盘；已存在则不改动。返回当前（生成后的）installation_id。
  std::string EnsureInstallationId();

  // installation.yaml 完整路径（供「在资源管理器中打开」/调试）。
  const std::filesystem::path& file_path() const { return file_path_; }

 private:
  // 把内存中的 installation_id_ / sync_dir_ 整体写回磁盘（逐行保留其他内容）。
  bool Flush();

  std::filesystem::path file_path_;
  std::string installation_id_;
  std::string sync_dir_;
};

}  // namespace weasel
