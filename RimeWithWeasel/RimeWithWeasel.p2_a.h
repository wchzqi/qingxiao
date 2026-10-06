// ============================================================================
// 青筱（QingXiaoType）P2-A：RimeWithWeasel.h 追加块
//
// 本文件不是完整替换版，而是「贴入位置 + 代码块」补丁，叠加在
// P1-B1 完整替换版（weasel-p1-dev/src/RimeWithWeasel/RimeWithWeasel.p1_b1.h）之上。
// P2-A 追加内容：
//   1) include <NetworkGate.h>
//   2) SessionStatus 追加 pending_aux（P2-E 翻译结果缓冲，经 ctx.aux= 下发）
//   3) Handler 持有 std::unique_ptr<weasel::NetworkGate> m_net_gate
// ============================================================================

// ----------------------------------------------------------------------------
// 改动 H-1：顶部 include 追加。
// 贴入位置：p1_b1.h 现有
//     #include "ClipboardManager.h"      （line 25）
// 之后追加：
// ----------------------------------------------------------------------------
//   #include "NetworkGate.h"

// ----------------------------------------------------------------------------
// 改动 H-2：SessionStatus 追加 pending_aux 成员。
// 贴入位置：p1_b1.h 的 SessionStatus 结构体（line 42-50），在
//     RimeSessionId session_id;
//   之后追加：
// ----------------------------------------------------------------------------
//   // P2-A：待下发到 ctx.aux 的文本（P2-E 翻译/tips）。
//   // _Respond 读取并经 ctx.aux=<文本> 下发；空串则不下发。
//   std::wstring pending_aux;

// ----------------------------------------------------------------------------
// 改动 H-3：Handler 类追加 NetworkGate 持有。
// 贴入位置：p1_b1.h 类私有成员区，现有
//     std::unique_ptr<weasel::ClipboardManager> m_clipboard;   （line 173）
// 之后追加：
// ----------------------------------------------------------------------------
//   // ===== P2-A 追加：隐私/联网闸门（Initialize 装载，P2-B/C/D/E 共用） =====
//   std::unique_ptr<weasel::NetworkGate> m_net_gate;
//
// 构造函数（RimeWithWeasel.cpp RimeWithWeaselHandler::RimeWithWeaselHandler）
// 需补一行初始化（贴入既有初始化列表/函数体首行）：
//     m_net_gate(std::make_unique<weasel::NetworkGate>())
// Finalize() 无需特殊处理（NetworkGate 纯内存、无线程，随 Handler 析构释放）。
