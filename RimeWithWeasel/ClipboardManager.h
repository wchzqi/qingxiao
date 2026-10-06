// ============================================================================
// 青简（Qingjian）P1-B1：服务端剪贴板历史缓存 ClipboardManager
//
// 归属工程：RimeWithWeasel.vcxproj（与 RimeWithWeaselHandler 同工程）。
//
// 实现要点（基线约定-P1工具模式协议 §4）：
//   - AddClipboardFormatListener 需要窗口句柄 + 消息泵：本类自建 message-only
//     隐藏窗口 + 独立消息循环线程（WM_GETMESSAGE 循环），不依赖 WeaselServer
//     主消息循环，监听崩溃/失败不影响服务进程。
//   - WM_CLIPBOARDUPDATE 时读取 CF_UNICODETEXT；
//   - 环形缓存：默认 50 条、去重（相同文本移到最新）、单条 <= 4096 WCHAR、
//     仅文本格式（超长/非文本一律忽略）。
//   - 异常兜底：Start() 失败（窗口创建/监听注册失败）时自动降级为
//     “监听未启用”模式——History() 每次打开面板时即时读取一次当前剪贴板置顶
//     展示；任何路径都不允许异常逃逸到服务进程主流程。
//
// 接口语义与基线 §4 对齐（Start 形参微调：基线给的 HWND 可选托管未采用——
// 自建消息线程更解耦，语义等价）：
//   - void Start(int max_entries = 50)
//   - void Stop()
//   - std::vector<std::wstring> History(size_t max = 0)  // 最新在前
//   - void Promote(const std::wstring& text)             // 置顶 + 回写剪贴板
//   - void Clear()
// ============================================================================
#pragma once

#include <windows.h>

#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace weasel {

class ClipboardManager {
 public:
  ClipboardManager();
  ~ClipboardManager();

  ClipboardManager(const ClipboardManager&) = delete;
  ClipboardManager& operator=(const ClipboardManager&) = delete;

  // 启动监听线程。max_entries 环形缓存上限（默认 50，0 取默认）。
  // 任何失败都内部降级，不抛异常。
  void Start(int max_entries = 50);

  // 停止监听线程并回收窗口。可重复调用。进程退出前应由 Handler 调用。
  void Stop();

  // 历史快照（最新在前）。max <= 0 表示返回全部。
  // 降级模式（监听未生效）下：自动把“当前剪贴板文本”作为第一条合并返回。
  std::vector<std::wstring> History(size_t max = 0) const;

  // 选中条目上屏时调用：缓存内置顶（去重）+ 回写系统剪贴板
  // （SetClipboardData(CF_UNICODETEXT)），即“回写置顶”。
  void Promote(const std::wstring& text);

  // 清空缓存（不改动系统剪贴板）。
  void Clear();

  // 当前是否处于监听状态（日志/调试用）。
  bool IsListening() const;

 private:
  // 监听线程主函数：注册窗口 → AddClipboardFormatListener → 消息循环。
  void _ThreadMain();

  // 调方持 m_mutex。去重后置顶，超出 m_max_entries 则截断尾部。
  void _PushLocked(const std::wstring& text);
  void _TrimLocked();

  static LRESULT CALLBACK _WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

  // 读取系统剪贴板的 Unicode 文本；失败/非文本返回空串。
  // 读取长度上限 kMaxChars（含截断保护，不依赖源串长度）。
  static std::wstring _ReadClipboardText();

  // 单条文本上限（WCHAR 个数，不含结尾 NUL）。
  static const size_t kMaxChars = 4096;

  mutable std::mutex m_mutex;
  std::vector<std::wstring> m_entries;  // 最新在前
  int m_max_entries = 50;

  std::thread m_thread;
  HWND m_hwnd = NULL;
  HANDLE m_ready_event = NULL;  // 窗口创建完成事件（手动复位）
  bool m_listening = false;
};

}  // namespace weasel
