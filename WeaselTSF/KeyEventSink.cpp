#include "stdafx.h"
#include "WeaselIPC.h"
#include "WeaselTSF.h"
#include <KeyEvent.h>
#include "CandidateList.h"

static weasel::KeyEvent prevKeyEvent;
static BOOL prevfEaten = FALSE;
static int keyCountToSimulate = 0;

void WeaselTSF::_ProcessKeyEvent(WPARAM wParam, LPARAM lParam, BOOL* pfEaten) {
  // when _IsKeyboardDisabled don't eat the key,
  // when keyboard closable and keyboard closed, don't eat the key
  if ((_isToOpenClose && !_IsKeyboardOpen()) || _IsKeyboardDisabled()) {
    *pfEaten = FALSE;
    return;
  }

  // if server connection is Not OK, don't eat it.
  if (!_EnsureServerConnected()) {
    *pfEaten = FALSE;
    return;
  }
  weasel::KeyEvent ke;
  GetKeyboardState(_lpbKeyState);
  if (!ConvertKeyEvent(static_cast<UINT>(wParam), lParam, _lpbKeyState, ke)) {
    /* Unknown key event */
    *pfEaten = FALSE;
  } else {
    // cheet key code when vertical auto reverse happened, swap up and down
    // ===== P1-B1：工具模式按键分流（不进 librime） =====
    try {
      if (_HandleToolKeyEvent(ke, pfEaten)) {
        // 响应行已由服务端写回管道缓冲；OnTestKeyDown/OnKeyDown 调用方随后
        // 的 _UpdateComposition(pContext) -> DoEditSession 会消费它们
        // （候选窗刷新 / commit 上屏），与正常按键链路一致。
        return;
      }
    } catch (...) {
      // TSF 稳定性优先：异常一律退出工具模式，恢复正常按键链。
      m_tool_mode = TOOL_MODE_NONE;
    }
    // ===== P2-E：划词翻译热键分流（不进 librime） =====
    try {
      if (_HandleTranslateHotkey(ke, pfEaten)) {
        return;  // 译文由服务端经 ctx.aux= 异步下发，本帧后续刷新自然消费
      }
    } catch (...) {
      // TSF 稳定性优先：异常一律放过，不影响正常输入。
    }
    // ===== P2-D：AI 卡片按键分流（工具模式之后） =====
    try {
      if (_HandleAiKeyEvent(ke, pfEaten)) {
        return;  // 响应行由随后的 _UpdateComposition -> DoEditSession 消费
      }
    } catch (...) {
      m_ai_mode = false;  // TSF 稳定性优先：异常退出 AI 面板
    }



    if (_cand->GetIsReposition()) {
      if (ke.keycode == ibus::Up)
        ke.keycode = ibus::Down;
      else if (ke.keycode == ibus::Down)
        ke.keycode = ibus::Up;
    }
    if (!keyCountToSimulate)
      *pfEaten = (BOOL)m_client.ProcessKeyEvent(ke);

    if (ke.keycode == ibus::Caps_Lock) {
      if (prevKeyEvent.keycode == ibus::Caps_Lock && prevfEaten == TRUE &&
          (ke.mask & ibus::RELEASE_MASK) && (!keyCountToSimulate)) {
        if ((GetKeyState(VK_CAPITAL) & 0x01)) {
          if (_committed || (!*pfEaten && _status.composing)) {
            keyCountToSimulate = 2;
            INPUT inputs[2];
            inputs[0].type = INPUT_KEYBOARD;
            inputs[0].ki = {VK_CAPITAL, 0, 0, 0, 0};
            inputs[1].type = INPUT_KEYBOARD;
            inputs[1].ki = {VK_CAPITAL, 0, KEYEVENTF_KEYUP, 0, 0};
            ::SendInput(sizeof(inputs) / sizeof(INPUT), inputs, sizeof(INPUT));
          }
        }
        *pfEaten = TRUE;
      }
      if (keyCountToSimulate)
        keyCountToSimulate--;
    }

    prevfEaten = *pfEaten;
    prevKeyEvent = ke;
  }
}

// ===== P1-B1：工具模式按键分流实现 =====
bool WeaselTSF::_HandleToolKeyEvent(const weasel::KeyEvent& ke,
                                   BOOL* pfEaten) {
  const bool is_keyup = (ke.mask & ibus::RELEASE_MASK) != 0;
  // 无修饰键的掩码集合（keyup 位除外）。
  const UINT no_mod_mask = ibus::MODIFIER_MASK & ~ibus::RELEASE_MASK;

  if (m_tool_mode == TOOL_MODE_NONE) {
    // ---- 触发热键：Ctrl+Alt+V / Ctrl+Alt+. ----
    const bool ctrl_alt =
        (ke.mask & (ibus::CONTROL_MASK | ibus::ALT_MASK)) ==
        (ibus::CONTROL_MASK | ibus::ALT_MASK);
    if (is_keyup || !ctrl_alt) {
      return false;  // 普通按键：放行给 librime
    }
    if (ke.keycode == L'v' || ke.keycode == L'V') {
      bool ok = m_client.ExtensionRequest(L"tool.open", L"type=clipboard");
      // 基线 §2：tool.open 失败（服务端旧版/不支持）立即清模式并放过热键。
      m_tool_mode = ok ? TOOL_MODE_CLIPBOARD : TOOL_MODE_NONE;
      *pfEaten = ok ? TRUE : FALSE;
      return true;
    }
    if (ke.keycode == L'j' || ke.keycode == L'J') {
      // P2-C：语音输入热键 Ctrl+Alt+J → ExtensionRequest voice.start
      // 失败（服务端旧版/无模型/无麦克风）：放过热键，不阻塞用户。
      bool ok = m_client.ExtensionRequest(L"voice.start", L"");
      *pfEaten = ok ? TRUE : FALSE;
      return true;
    }

    if (ke.keycode == L'.') {
      // P1-B2 预留：emoji 数据/EmojiProvider 未就绪，暂不启用；
      // 收到 false 时服务端 HandleExtension 也会返回 not-supported。
      // bool ok = m_client.ExtensionRequest(L"tool.open", L"type=emoji");
      // m_tool_mode = ok ? TOOL_MODE_EMOJI : TOOL_MODE_NONE;
      // *pfEaten = ok ? TRUE : FALSE;
      // return true;
      return false;  // 暂放行给系统/librime
    }
    return false;
  }

  // ---- 工具模式中：所有按键一律吃掉，不进 librime ----
  *pfEaten = TRUE;
  if (is_keyup) {
    return true;  // keyup 不触发动作，仅吃掉
  }

  switch (ke.keycode) {
    case ibus::Escape:
      m_client.ExtensionRequest(L"tool.cancel", L"");
      m_tool_mode = TOOL_MODE_NONE;
      return true;
    case ibus::Page_Up:  // == ibus::Prior
      if (!m_client.ExtensionRequest(L"tool.page", L"backward")) {
        m_tool_mode = TOOL_MODE_NONE;  // 通道失败：清模式防卡死
      }
      return true;
    case ibus::Page_Down:  // == ibus::Next
      if (!m_client.ExtensionRequest(L"tool.page", L"forward")) {
        m_tool_mode = TOOL_MODE_NONE;
      }
      return true;
    default:
      break;
  }

  // 数字 1..9,0（要求无 Ctrl/Alt/Shift 等修饰）：tool.pick index=N。
  // 基线 §2：0 表示第 10 条。
  if ((ke.mask & no_mod_mask) == 0 && ke.keycode >= L'0' &&
      ke.keycode <= L'9') {
    const int index = (ke.keycode == L'0') ? 9 : (ke.keycode - L'1');
    m_client.ExtensionRequest(L"tool.pick",
                              L"index=" + std::to_wstring(index));
    m_tool_mode = TOOL_MODE_NONE;  // 选中后必退出工具状态
    return true;
  }

  // 其余按键：吃掉但不动作，保持工具面板。
  return true;
}

// ===== P2-E：划词翻译热键（Ctrl+Alt+T）实现 =====
// 热键命中即吃掉并向服务端发 translate.request；译文经 ctx.aux= 异步下发。
bool WeaselTSF::_HandleTranslateHotkey(const weasel::KeyEvent& ke,
                                       BOOL* pfEaten) {
  const bool is_keyup = (ke.mask & ibus::RELEASE_MASK) != 0;
  const bool ctrl_alt =
      (ke.mask & (ibus::CONTROL_MASK | ibus::ALT_MASK)) ==
      (ibus::CONTROL_MASK | ibus::ALT_MASK);
  // 热键：Ctrl+Alt+T（keydown；与 P1-B1 的 Ctrl+Alt+V / Ctrl+Alt+. 不冲突）。
  if (is_keyup || !ctrl_alt) return false;
  if (ke.keycode != L't' && ke.keycode != L'T') return false;

  // ---- 获取目标应用中「已选中文本」（划词翻译的输入）----
  // 本批次骨架先给最小占位（路线 A UIA / 路线 B 剪贴板法 实机验证后替换）。
  std::wstring selected = L"";
  // TODO(P2-E-windows): 实现 GetSelectedTextViaClipboard() / UIA 版。
  // 取空时：若无选区，则翻译「当前预编辑串」（边写边译的手动版）。
  if (selected.empty()) {
    selected = m_composition_text;  // WeaselTSF 内既有预编辑缓冲（若有）
  }
  if (selected.empty()) {
    *pfEaten = TRUE;  // 无内容：吃掉热键，不发请求
    return true;
  }
  // payload：text=<utf8 百分号转义>；target_lang 用服务端默认即可省略。
  std::wstring payload = L"text=" + selected;  // 实机联调时加 %xx 转义
  bool ok = m_client.ExtensionRequest(L"translate.request", payload);
  *pfEaten = ok ? TRUE : FALSE;
  return true;
}
// ===== P2-D：AI 卡片按键分流实现 =====
bool WeaselTSF::_HandleAiKeyEvent(const weasel::KeyEvent& ke, BOOL* pfEaten) {
  const bool is_keyup = (ke.mask & ibus::RELEASE_MASK) != 0;
  const UINT no_mod_mask = ibus::MODIFIER_MASK & ~ibus::RELEASE_MASK;
  const bool no_mod = ((ke.mask & no_mod_mask) == 0);

  if (!m_ai_mode) {
    // ---- 唤起：裸 '='（无修饰键、key down） ----
    if (is_keyup || !no_mod) return false;  // 带修饰/keyup 一律放行
    // 默认动作：polish（可后续接 ai/default_action 配置）。
    bool ok = m_client.ExtensionRequest(L"ai.assist", L"action_type=polish");
    m_ai_mode = ok;                 // 服务端不支持/未启用 → ok=false → 放过
    *pfEaten = ok ? TRUE : FALSE;   // 未唤起则不吞键
    return true;
  }

  // ---- AI 面板打开中：吃掉所有按键 ----
  *pfEaten = TRUE;
  if (is_keyup) return true;

  if (ke.keycode == ibus::Escape) {
    m_client.ExtensionRequest(L"ai.pick", L"action_id=-1&item=-1");  // 取消
    m_ai_mode = false;
    return true;
  }
  // 数字 1..9,0（无修饰）：选第 item 条建议。
  if (no_mod && ke.keycode >= L'0' && ke.keycode <= L'9') {
    const int item = (ke.keycode == L'0') ? 9 : (ke.keycode - L'1');
    std::wstring payload = L"action_id=" + std::to_wstring(m_ai_action) +
                           L"&item=" + std::to_wstring(item);
    m_client.ExtensionRequest(L"ai.pick", payload);
    m_ai_mode = false;  // 选中即收面板
    return true;
  }
  // 其余键：吃掉，保持面板（不翻页/不进 librime）。
  return true;
}


STDMETHODIMP WeaselTSF::OnSetFocus(BOOL fForeground) {
  if (fForeground)
    m_client.FocusIn();
  else {
    m_client.FocusOut();
    _AbortComposition();
  }

  return S_OK;
}

/* Some apps sends strange OnTestKeyDown/OnKeyDown combinations:
 *  Some sends OnKeyDown() only. (QQ2012)
 *  Some sends multiple OnTestKeyDown() for a single key event. (MS WORD 2010
 * x64)
 *
 * We assume every key event will eventually cause a OnKeyDown() call.
 * We use _fTestKeyDownPending to omit multiple OnTestKeyDown() calls,
 *  and for OnKeyDown() to check if the key has already been sent to the server.
 */

STDMETHODIMP WeaselTSF::OnTestKeyDown(ITfContext* pContext,
                                      WPARAM wParam,
                                      LPARAM lParam,
                                      BOOL* pfEaten) {
  _fTestKeyUpPending = FALSE;
  if (_fTestKeyDownPending) {
    *pfEaten = TRUE;
    return S_OK;
  }
  _ProcessKeyEvent(wParam, lParam, pfEaten);
  _UpdateComposition(pContext);
  if (*pfEaten)
    _fTestKeyDownPending = TRUE;
  return S_OK;
}

STDMETHODIMP WeaselTSF::OnKeyDown(ITfContext* pContext,
                                  WPARAM wParam,
                                  LPARAM lParam,
                                  BOOL* pfEaten) {
  _fTestKeyUpPending = FALSE;
  if (_fTestKeyDownPending) {
    _fTestKeyDownPending = FALSE;
    *pfEaten = TRUE;
  } else {
    _ProcessKeyEvent(wParam, lParam, pfEaten);
    _UpdateComposition(pContext);
  }
  return S_OK;
}

STDMETHODIMP WeaselTSF::OnTestKeyUp(ITfContext* pContext,
                                    WPARAM wParam,
                                    LPARAM lParam,
                                    BOOL* pfEaten) {
  _fTestKeyDownPending = FALSE;
  if (_fTestKeyUpPending) {
    *pfEaten = TRUE;
    return S_OK;
  }
  _ProcessKeyEvent(wParam, lParam, pfEaten);
  _UpdateComposition(pContext);
  if (*pfEaten)
    _fTestKeyUpPending = TRUE;
  return S_OK;
}

STDMETHODIMP WeaselTSF::OnKeyUp(ITfContext* pContext,
                                WPARAM wParam,
                                LPARAM lParam,
                                BOOL* pfEaten) {
  _fTestKeyDownPending = FALSE;
  if (_fTestKeyUpPending) {
    _fTestKeyUpPending = FALSE;
    *pfEaten = TRUE;
  } else {
    _ProcessKeyEvent(wParam, lParam, pfEaten);
    if (!_async_edit)
      _UpdateComposition(pContext);
  }
  return S_OK;
}

STDMETHODIMP WeaselTSF::OnPreservedKey(ITfContext* pContext,
                                       REFGUID rguid,
                                       BOOL* pfEaten) {
  *pfEaten = FALSE;
  return S_OK;
}

BOOL WeaselTSF::_InitKeyEventSink() {
  com_ptr<ITfKeystrokeMgr> pKeystrokeMgr;
  HRESULT hr;

  if (_pThreadMgr->QueryInterface(&pKeystrokeMgr) != S_OK)
    return FALSE;

  hr = pKeystrokeMgr->AdviseKeyEventSink(_tfClientId, (ITfKeyEventSink*)this,
                                         TRUE);

  return (hr == S_OK);
}

void WeaselTSF::_UninitKeyEventSink() {
  com_ptr<ITfKeystrokeMgr> pKeystrokeMgr;

  if (_pThreadMgr->QueryInterface(&pKeystrokeMgr) != S_OK)
    return;

  pKeystrokeMgr->UnadviseKeyEventSink(_tfClientId);
}

BOOL WeaselTSF::_InitPreservedKey() {
  return TRUE;
#if 0
	com_ptr<ITfKeystrokeMgr> pKeystrokeMgr;
	if (_pThreadMgr->QueryInterface(pKeystrokeMgr.GetAddressOf()) != S_OK)
	{
		return FALSE;
	}
	TF_PRESERVEDKEY preservedKeyImeMode;

	/* Define SHIFT ONLY for now */
	preservedKeyImeMode.uVKey = VK_SHIFT;
	preservedKeyImeMode.uModifiers = TF_MOD_ON_KEYUP;

	auto hr = pKeystrokeMgr->PreserveKey(
		_tfClientId,
		GUID_IME_MODE_PRESERVED_KEY,
		&preservedKeyImeMode, L"", 0);
	
	return SUCCEEDED(hr);
#endif
}

void WeaselTSF::_UninitPreservedKey() {}
