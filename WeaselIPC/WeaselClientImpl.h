#pragma once
#include <WeaselIPC.h>
#include <PipeChannel.h>

namespace weasel {

class ClientImpl {
 public:
  ClientImpl();
  ~ClientImpl();

  bool Connect(ServerLauncher const& launcher);
  void Disconnect();
  void ShutdownServer();
  void StartSession();
  void EndSession();
  void StartMaintenance();
  void EndMaintenance();
  bool Echo();
  bool ProcessKeyEvent(KeyEvent const& keyEvent);
  bool CommitComposition();
  bool ClearComposition();
  bool SelectCandidateOnCurrentPage(size_t index);
  bool HighlightCandidateOnCurrentPage(size_t index);
  bool ChangePage(bool backward);
  void UpdateInputPosition(RECT const& rc);
  void FocusIn();
  void FocusOut();
  void TrayCommand(UINT menuId);
  bool GetResponseData(ResponseHandler const& handler);

  // ===== P0 MVP 扩展通道（最小握手子集） =====
  // 查询服务端版本与能力。发送 WEASEL_IPC_GET_SERVER_INFO 后，
  // 返回值 != 0 表示服务端已受理；回写的文本行需通过 GetResponseData() 读取。
  bool GetServerInfo();
  // 通用扩展请求。message_id/payload 以 key=value 文本行写入管道请求体后，
  // 发送 WEASEL_IPC_EXTENSION；回写文本同样通过 GetResponseData() 读取。
  // 返回值 != 0 表示服务端识别并处理了该 message_id（否则视为 not-supported）。
  bool ExtensionRequest(const std::wstring& message_id,
                        const std::wstring& payload);

 protected:
  void _InitializeClientInfo();
  bool _WriteClientInfo();

  LRESULT _SendMessage(WEASEL_IPC_COMMAND Msg, DWORD wParam, DWORD lParam);

  bool _Connected() const { return channel.Connected(); }
  bool _Active() const { return channel.Connected() && session_id != 0; }

 private:
  UINT session_id;
  std::wstring app_name;
  bool is_ime;

  PipeChannel<PipeMessage> channel;
};

}  // namespace weasel
