// ============================================================================
// 青简（Qingjian）P1-B1：ClipboardManager 实现
// 见 ClipboardManager.h 头部说明。
// ============================================================================

#include "stdafx.h"

#include "ClipboardManager.h"

#include <cstring>

namespace weasel {

ClipboardManager::ClipboardManager() = default;

ClipboardManager::~ClipboardManager() {
  // 进程退出兜底：Stop 内 join，绝不 detach 后访问已析构对象。
  try {
    Stop();
  } catch (...) {
  }
  if (m_ready_event) {
    CloseHandle(m_ready_event);
    m_ready_event = NULL;
  }
}

// ---------------------------------------------------------------------------
void ClipboardManager::Start(int max_entries) {
  if (m_thread.joinable()) {
    return;  // 已在运行
  }
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (max_entries > 0) {
      m_max_entries = max_entries;
    }
  }

  // 窗口创建完成事件：Start() 阻塞等待至多 2s，避免“窗口未建好就 Stop”。
  if (!m_ready_event) {
    m_ready_event = CreateEventW(NULL, TRUE, FALSE, NULL);
  }
  if (m_ready_event) {
    ResetEvent(m_ready_event);
  }

  try {
    m_thread = std::thread([this]() { _ThreadMain(); });
  } catch (...) {
    // 线程起不来：保持降级模式，不影响服务进程。
    m_listening = false;
    return;
  }

  if (m_ready_event) {
    WaitForSingleObject(m_ready_event, 2000);
  }
}

// ---------------------------------------------------------------------------
void ClipboardManager::Stop() {
  if (!m_thread.joinable()) {
    return;
  }
  // 通知消息循环退出：WM_CLOSE -> DestroyWindow -> WM_DESTROY -> PostQuitMessage。
  if (m_hwnd) {
    PostMessageW(m_hwnd, WM_CLOSE, 0, 0);
  }
  try {
    m_thread.join();
  } catch (...) {
  }
  m_hwnd = NULL;
  m_listening = false;
}

// ---------------------------------------------------------------------------
std::vector<std::wstring> ClipboardManager::History(size_t max) const {
  std::lock_guard<std::mutex> lock(m_mutex);
  std::vector<std::wstring> out;
  // 降级模式：监听未生效时，打开面板即时读取一次当前剪贴板置顶展示。
  if (!m_listening) {
    try {
      std::wstring cur = _ReadClipboardText();
      if (!cur.empty()) {
        out.push_back(cur);
      }
    } catch (...) {
    }
  }
  for (size_t i = 0; i < m_entries.size(); ++i) {
    if (max > 0 && out.size() >= max) {
      break;
    }
    // 降级模式下若缓存首条与当前剪贴板重复则跳过（轻量去重）。
    if (!out.empty() && m_entries[i] == out.front()) {
      continue;
    }
    out.push_back(m_entries[i]);
  }
  return out;
}

// ---------------------------------------------------------------------------
void ClipboardManager::Promote(const std::wstring& text) {
  if (text.empty()) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    _PushLocked(text);
  }

  // 回写系统剪贴板（SetClipboardData）。失败仅记日志位（DLOG 在调用方），
  // 不影响上屏——commit 已由服务端响应行直接驱动 TSF 插入。
  if (!OpenClipboard(NULL)) {
    return;
  }
  EmptyClipboard();
  const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
  HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
  if (h) {
    void* p = GlobalLock(h);
    if (p) {
      std::memcpy(p, text.c_str(), bytes);
      GlobalUnlock(h);
      if (!SetClipboardData(CF_UNICODETEXT, h)) {
        // SetClipboardData 失败时剪贴板已接管所有权前失败：释放内存。
        GlobalFree(h);
      }
      // 成功时 h 归系统所有，不可再 Free。
    } else {
      GlobalFree(h);
    }
  }
  CloseClipboard();
}

// ---------------------------------------------------------------------------
void ClipboardManager::Clear() {
  std::lock_guard<std::mutex> lock(m_mutex);
  m_entries.clear();
}

// ---------------------------------------------------------------------------
bool ClipboardManager::IsListening() const { return m_listening; }

// ---------------------------------------------------------------------------
void ClipboardManager::_PushLocked(const std::wstring& text) {
  // 调用方持锁。空串/超长一律忽略。
  if (text.empty() || text.size() > kMaxChars) {
    return;
  }
  // 去重：相同文本先从原位置摘下。
  for (size_t i = 0; i < m_entries.size(); ++i) {
    if (m_entries[i] == text) {
      m_entries.erase(m_entries.begin() + i);
      break;
    }
  }
  m_entries.insert(m_entries.begin(), text);
  _TrimLocked();
}

void ClipboardManager::_TrimLocked() {
  while ((int)m_entries.size() > m_max_entries) {
    m_entries.pop_back();
  }
}

// ---------------------------------------------------------------------------
std::wstring ClipboardManager::_ReadClipboardText() {
  if (!OpenClipboard(NULL)) {
    return std::wstring();
  }
  std::wstring result;
  HGLOBAL h = GetClipboardData(CF_UNICODETEXT);
  if (h) {
    LPCWSTR p = static_cast<LPCWSTR>(GlobalLock(h));
    if (p) {
      // 上限 kMaxChars：超长条目视为“忽略”，这里截断读取（不入库）。
      size_t len = 0;
      while (len < kMaxChars && p[len] != L'\0') {
        ++len;
      }
      result.assign(p, len);
      GlobalUnlock(h);
    }
  }
  CloseClipboard();
  return result;
}

// ---------------------------------------------------------------------------
LRESULT CALLBACK ClipboardManager::_WndProc(HWND hwnd,
                                            UINT msg,
                                            WPARAM wp,
                                            LPARAM lp) {
  switch (msg) {
    case WM_CLIPBOARDUPDATE: {
      ClipboardManager* self =
          reinterpret_cast<ClipboardManager*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
      if (self) {
        try {
          std::wstring text = _ReadClipboardText();
          if (!text.empty()) {
            std::lock_guard<std::mutex> lock(self->m_mutex);
            self->_PushLocked(text);
          }
        } catch (...) {
          // 绝不允许监听线程因单条剪贴板内容异常而退出。
        }
      }
      return 0;
    }
    case WM_CLOSE:
      DestroyWindow(hwnd);
      return 0;
    case WM_DESTROY:
      RemoveClipboardFormatListener(hwnd);
      PostQuitMessage(0);
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------
void ClipboardManager::_ThreadMain() {
  try {
    static const wchar_t kWndClass[] = L"QingjianClipboardListenerWnd";
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &ClipboardManager::_WndProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = kWndClass;
    // 重复注册返回 0（ERROR_CLASS_ALREADY_EXISTS）属正常，忽略。
    RegisterClassExW(&wc);

    // message-only 窗口：不显示、不进任务栏，仅接收剪贴板广播。
    m_hwnd = CreateWindowExW(0, kWndClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE,
                             NULL, wc.hInstance, NULL);
    if (!m_hwnd) {
      if (m_ready_event) {
        SetEvent(m_ready_event);
      }
      return;  // 降级模式
    }
    SetWindowLongPtrW(m_hwnd, GWLP_USERDATA,
                      reinterpret_cast<LONG_PTR>(this));
    m_listening = !!AddClipboardFormatListener(m_hwnd);

    // 启动时先吸收一次当前剪贴板，避免空面板。
    if (m_listening) {
      try {
        std::wstring cur = _ReadClipboardText();
        if (!cur.empty()) {
          std::lock_guard<std::mutex> lock(m_mutex);
          _PushLocked(cur);
        }
      } catch (...) {
      }
    }
  } catch (...) {
    m_listening = false;
  }

  if (m_ready_event) {
    SetEvent(m_ready_event);
  }

  // 消息循环：Stop() 经 WM_CLOSE -> PostQuitMessage 退出。
  MSG msg;
  while (GetMessageW(&msg, NULL, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
}

}  // namespace weasel
