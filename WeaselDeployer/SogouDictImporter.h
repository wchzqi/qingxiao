// SogouDictImporter.h — 青简 P1-B5 搜狗一行一词词库导入转换器
//
// 职责：
//   解析搜狗拼音导出的「一行一词」纯文本词库(.txt)，转换为 Rime 可直接使用的两类产物：
//     1) 追加到用户 custom_phrase.txt（自定义短语，格式：词<TAB>编码<TAB>权重）；
//     2) 生成一个 .dict.yaml 片段（带 Rime 词典头：name/version/sort/columns）。
//
// 输入行格式（搜狗导出 txt，TAB 或任意空白分隔）：
//     词[<TAB|空格>编码音节...][<TAB|空格>权重数字]
//   例：
//     你好<TAB>ni hao<TAB>12345
//     黑洞 hei dong 88
//     纯词无编码          -> 无编码无法成 Rime 条目，跳过并计数
//     # 注释行           -> 跳过
//
// 硬约束（P1-B5）：
//   - 不调用 librime 内部 API，只读写用户词典文本文件（custom_phrase.txt / *.dict.yaml）；
//   - 产物一律 UTF-8；无法解析的行跳过并计入 report.skipped_*，不抛异常；
//   - 编码探测：UTF-8 BOM / UTF-16LE BOM / 严格 UTF-8 校验 / 回退 GBK(CP936)。
//
// 命名空间 weasel 保留；与 SettingsStore 等同属 WeaselDeployer 模块。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace weasel {

// 产物输出模式
enum class SogouImportMode {
  kAppendCustomPhrase = 0,  // 追加到 custom_phrase.txt
  kWriteDictYaml = 1,       // 写独立 .dict.yaml
};

// 探测到的文本编码
enum class SogouEncoding {
  kUtf8 = 0,       // 无 BOM 但通过严格 UTF-8 校验，或带 UTF-8 BOM
  kUtf16Le = 1,    // 带 FF FE BOM
  kGbk = 2,        // UTF-8 校验失败，按 GBK(CP936) 解码
  kUnknown = 3,
};

// 解析后的单条 Rime 词典条目（UTF-8）
struct SogouEntry {
  std::string word;    // 词（汉字/中英混合）
  std::string code;    // 编码（音节以单空格连接，如 "ni hao"）
  int weight{1};       // 权重；缺失时默认 1
};

// 导入结果报告
struct SogouImportReport {
  long total_lines{0};        // 物理行总数
  long imported{0};           // 成功转换条目数
  long skipped_empty{0};      // 空行
  long skipped_comment{0};    // # 注释行
  long skipped_no_code{0};     // 无编码（无法成 Rime 条目）
  long skipped_bad{0};         // 其他无法解析
  std::size_t bytes_read{0};   // 读取字节数
  SogouEncoding encoding{SogouEncoding::kUnknown};
  std::string message;         // 面向用户的中文结论（含计数）

  long skipped_total() const {
    return skipped_empty + skipped_comment + skipped_no_code + skipped_bad;
  }
};

class SogouDictImporter {
 public:
  SogouDictImporter();
  ~SogouDictImporter();

  SogouDictImporter(const SogouDictImporter&) = delete;
  SogouDictImporter& operator=(const SogouDictImporter&) = delete;

  // 完整流程：读文件 -> 探测编码并归一为 UTF-8 -> 解析 -> 写产物。
  //   src_path        : 搜狗导出 .txt 的完整路径（UTF-8 窄串，Windows 上按 UTF-8 转宽）。
  //   out_path        : 目标路径。kAppendCustomPhrase 时为 custom_phrase.txt；
  //                     kWriteDictYaml 时为要生成的 .dict.yaml。
  //   dict_name       : .dict.yaml 的 name: 字段（kWriteDictYaml 有效；kAppendCustomPhrase 忽略）。
  //   mode            : 输出模式。
  // 返回 false 表示文件读取/写入失败；解析出的「跳过行」不计入失败，体现在 report 中。
  bool Import(const std::string& src_path,
              const std::string& out_path,
              const std::string& dict_name,
              SogouImportMode mode,
              SogouImportReport* report);

  // ---- 可单测的纯标准库核心（不碰文件/Windows API）----

  // 对原始字节做编码探测 + 归一化，返回 UTF-8 文本；*enc 写出探测结果。
  // bytes 为整文件内容；遇到 BOM 相应剥离。
  static std::string NormalizeToUtf8(const std::string& bytes, SogouEncoding* enc);

  // 把一行 UTF-8 文本解析成条目；无法解析时返回 false 并在 reason 给出原因码
  // (empty/comment/no_code/bad)。纯函数，便于 Linux g++ 自测。
  static bool ParseLine(const std::string& line, SogouEntry* entry, std::string* reason);

  // 把条目序列序列化为 Rime 文本行（词\t编码\t权重\n），用于追加/写文件。
  static std::string SerializeEntries(const std::vector<SogouEntry>& entries);

  // 生成 .dict.yaml 完整内容（含 YAML 头）。dict_name 为 name: 字段。
  static std::string BuildDictYaml(const std::string& dict_name,
                                   const std::vector<SogouEntry>& entries);

  // 严格 UTF-8 校验（合法返回 true）。
  static bool IsValidUtf8(const std::string& s);

  // UTF-16LE 字节流 -> UTF-8（含代理对处理）。纯标准库实现。
  static std::string Utf16LeToUtf8(const std::string& bytes);
};

}  // namespace weasel
