// ============================================================================
// 青简（Qingjian）P1-B2：emoji/符号面板数据查询 Provider
//
// 归属工程：RimeWithWeasel（见基线 §7 文件归属表）
// 职责：从数据文件 resource/emoji.txt（部署后位于共享数据目录，即安装包 data/
//       下）加载 emoji/颜文字条目，供 HandleExtension 的 tool.open(type=emoji)
//       分支填充候选。
//
// 数据格式（基线 §6）：每行一条，TAB 分隔三列——
//       分类 <TAB> emoji文本 <TAB> 名称关键字
//       例如：  笑脸 \U0001F600 开心 笑 笑脸
//       以 '#' 开头的行视为注释，空行忽略。
//
// 设计约束：
//   - 全程不抛异常、不依赖 rime_api；数据文件缺失或损坏时安全降级为空列表，
//     绝不导致服务端 crash（基线 §8 静态自检 + 硬约束"数据文件缺失时安全降级"）。
//   - 内部一律以 UTF-8（std::string）持有文本，与既有 rime 文本流一致；
//     上屏/填候选时由调用方经 u8tow() 转宽字符（见 _GetCandidateInfo 现有写法）。
//
// 参考：本 Provider 为青简自实现，不拷贝 rime-ice 代码（rime-ice 仅作 V/U lua
//       翻译器参考来源：https://github.com/iDvel/rime-ice）。
// ============================================================================
#ifndef WEASEL_EMOJI_PROVIDER_H_
#define WEASEL_EMOJI_PROVIDER_H_

#include <filesystem>
#include <string>
#include <vector>

namespace weasel {

// 单条 emoji/符号/颜文字条目。三个字段均为 UTF-8 字节串。
struct EmojiEntry {
  std::string category;  // 分类名（如 "笑脸"、"手势"、"颜文字"）
  std::string text;      // 上屏文本（一个或多个 emoji，或一段颜文字）
  std::string keywords;  // 名称关键字（空格分隔，供搜索；可与 text 互为补充）
};

// emoji 数据查询器。一个进程持有一个实例即可（Handler 构造时 Load 一次）。
// 线程模型：P1 工具模式在服务端单会话线程内同步调用，无并发读写下要求；
//           若后续需多线程，调用方自行加锁。
class EmojiProvider {
 public:
  EmojiProvider();
  ~EmojiProvider();

  // 禁止拷贝（持有内部缓冲，语义上为单例数据源）。
  EmojiProvider(const EmojiProvider&) = delete;
  EmojiProvider& operator=(const EmojiProvider&) = delete;

  // 默认数据文件路径：共享数据目录下的 emoji.txt。
  // 等价于 WeaselSharedDataPath() / "emoji.txt"（在 cpp 内组合，避免头文件暴露
  // filesystem 路径细节）。调用方可直接传入自定义路径做测试。
  static std::filesystem::path DefaultDataPath();

  // 加载（或重新加载）数据文件。
  //   - 文件存在且可解析：entries_ 被替换为解析结果，loaded_ = true，返回 true。
  //   - 文件不存在 / 打开失败 / 一行有效数据都没有：清空 entries_、loaded_=false，
  //     返回 false（不抛异常）。
  //   - 解析过程中跳过格式错误行（列数不足 3、注释、空行），不因坏行整体失败。
  bool Load(const std::filesystem::path& data_path);

  // 是否已成功加载过非空数据。tool.open(type=emoji) 应先据此判断可否进入面板。
  bool loaded() const { return loaded_; }

  // 接口一：按分类列表。返回文件中出现过的全部分类名（UTF-8），按首次出现顺序
  //         去重。未加载时返回空 vector。
  std::vector<std::string> Categories() const;

  // 接口二：按关键字搜索。跨分类对 keywords / category / text 做子串匹配，
  //         ASCII 字母大小写不敏感（中文无大小写，天然兼容）。keyword 为空时
  //         返回空（不返回全量，避免误把整个库灌进候选）。
  std::vector<EmojiEntry> Search(const std::string& keyword_utf8) const;

  // 接口三：按页取条目。取指定分类下的第 page 页（page 从 0 开始），
  //         每页 page_size 条（默认 10，对应 label 1..9,0）。
  //         分类不存在 / page 越界时返回空 vector（不抛异常）。
  std::vector<EmojiEntry> Page(const std::string& category_utf8,
                               size_t page,
                               size_t page_size = 10) const;

  // 某分类条目总数（供翻页判断是否末页，B1 框架可选用）。
  size_t CategorySize(const std::string& category_utf8) const;

 private:
  std::vector<EmojiEntry> entries_;
  bool loaded_ = false;
};

}  // namespace weasel

#endif  // WEASEL_EMOJI_PROVIDER_H_
