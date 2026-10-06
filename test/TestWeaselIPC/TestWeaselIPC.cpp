// TestWeaselIPC.cpp : Defines the entry point for the console application.
//

#include "stdafx.h"
#include <WeaselIPC.h>
#include <RimeWithWeasel.h>

#include <boost/interprocess/streams/bufferstream.hpp>
using namespace boost::interprocess;

#include <iostream>
#include <memory>

CAppModule _Module;

int console_main();
int client_main();
int server_main();

// usage: TestWeaselIPC.exe [/start | /stop | /console]

int _tmain(int argc, _TCHAR* argv[]) {
  if (argc == 1)  // no args
  {
    return client_main();
  } else if (argc > 1 && !wcscmp(L"/start", argv[1])) {
    return server_main();
  } else if (argc > 1 && !wcscmp(L"/stop", argv[1])) {
    weasel::Client client;
    if (!client.Connect()) {
      std::cerr << "server not running." << std::endl;
      return 0;
    }
    client.ShutdownServer();
    return 0;
  } else if (argc > 1 && !wcscmp(L"/console", argv[1])) {
    return console_main();
    return 0;
  }

  return -1;
}

bool launch_server() {
  int ret = (int)ShellExecute(NULL, L"open", L"TestWeaselIPC.exe", L"/start",
                              NULL, SW_NORMAL);
  if (ret <= 32) {
    std::cerr << "failed to launch server." << std::endl;
    return false;
  }
  return true;
}

bool read_buffer(LPWSTR buffer, UINT length, LPWSTR dest) {
  wbufferstream bs(buffer, length);
  bs.read(dest, WEASEL_IPC_BUFFER_LENGTH);
  return bs.good();
}
static bool read_all(LPWSTR buffer, UINT length, std::wstring& out) {
  out.assign(buffer, length);
  // buffer 以 NUL 结尾，截断到第一个 NUL
  size_t pos = out.find(L'\0');
  if (pos != std::wstring::npos)
    out.resize(pos);
  return true;
}



const char* wcstomb(const wchar_t* wcs) {
  const int buffer_len = 8192;
  static char buffer[buffer_len];
  WideCharToMultiByte(CP_OEMCP, NULL, wcs, -1, buffer, buffer_len, NULL, FALSE);
  return buffer;
}

int console_main() {
  weasel::Client client;
  if (!client.Connect()) {
    std::cerr << "failed to connect to server." << std::endl;
    return -2;
  }
  client.StartSession();
  if (!client.Echo()) {
    std::cerr << "failed to start session." << std::endl;
    return -3;
  }

  while (std::cin.good()) {
    int ch = std::cin.get();
    if (!std::cin.good())
      break;
    bool eaten = client.ProcessKeyEvent(weasel::KeyEvent(ch, 0));
    std::cout << "server replies: " << eaten << std::endl;
    if (eaten) {
      WCHAR response[WEASEL_IPC_BUFFER_LENGTH];
      bool ret = client.GetResponseData(
          std::bind<bool>(read_buffer, std::placeholders::_1,
                          std::placeholders::_2, std::ref(response)));
      std::cout << "get response data: " << ret << std::endl;
      std::cout << "buffer reads: " << std::endl
                << wcstomb(response) << std::endl;
    }
  }

  client.EndSession();

  return 0;
}

int client_main() {
  // launch_server();
  Sleep(1000);
  weasel::Client client;
  if (!client.Connect()) {
    std::cerr << "failed to connect to server." << std::endl;
    return -2;
  }
  client.StartSession();
  if (!client.Echo()) {
    std::cerr << "failed to login." << std::endl;
    return -3;
  }
  // ===== P0 MVP 扩展通道：握手往返 =====
  // D-3.1 GET_SERVER_INFO 往返
  if (client.GetServerInfo()) {
    std::wstring info;
    client.GetResponseData(std::bind<bool>(read_all, std::placeholders::_1,
                                           std::placeholders::_2,
                                           std::ref(info)));
    std::wcout << L"[handshake] server_info: " << info << std::endl;
  } else {
    std::cerr << "[handshake] GetServerInfo failed." << std::endl;
  }

  // D-3.2 EXTENSION ping -> pong 往返
  if (client.ExtensionRequest(L"ping", L"")) {
    std::wstring pong;
    client.GetResponseData(std::bind<bool>(read_all, std::placeholders::_1,
                                           std::placeholders::_2,
                                           std::ref(pong)));
    std::wcout << L"[extension] ping -> " << pong << std::endl;
  } else {
    std::cerr << "[extension] ping: not supported." << std::endl;
  }

  // D-3.3 EXTENSION server_info 往返（顺带覆盖第二个 message_id）
  if (client.ExtensionRequest(L"server_info", L"")) {
    std::wstring info2;
    client.GetResponseData(std::bind<bool>(read_all, std::placeholders::_1,
                                           std::placeholders::_2,
                                           std::ref(info2)));
    std::wcout << L"[extension] server_info -> " << info2 << std::endl;
  } else {
    std::cerr << "[extension] server_info: not supported." << std::endl;
  }

  // D-3.4 反例：未支持的 message_id 应返回 false（not-supported）
  bool unknown = client.ExtensionRequest(L"cloud_candidate", L"{}");
  std::cerr << "[extension] cloud_candidate (expect false): " << unknown
            << std::endl;

  bool eaten = client.ProcessKeyEvent(weasel::KeyEvent(L'a', 0));
  std::cout << "server replies: " << eaten << std::endl;
  if (eaten) {
    WCHAR response[WEASEL_IPC_BUFFER_LENGTH];
    bool ret = client.GetResponseData(
        std::bind<bool>(read_buffer, std::placeholders::_1,
                        std::placeholders::_2, std::ref(response)));
    std::cout << "get response data: " << ret << std::endl;
    std::cout << "buffer reads: " << std::endl
              << wcstomb(response) << std::endl;
  }
  // ===== P1-B1：tool 协议往返测试 =====
  {
    struct ToolCase {
      const wchar_t* id;
      const wchar_t* payload;
    } cases[] = {
      {L"tool.open", L"type=clipboard"},
      {L"tool.pick", L"index=0"},
      {L"tool.page", L"forward"},
      {L"tool.cancel", L""},
      {L"tool.unknown", L""},  // 预期返回 false
    };
    for (const auto& c : cases) {
      bool ok = client.ExtensionRequest(c.id, c.payload);
      std::wcout << L"[client] " << c.id << L" -> " << (ok ? L"ok" : L"no")
                 << std::endl;
      if (ok) {
        WCHAR response[WEASEL_IPC_BUFFER_LENGTH] = {0};
        client.GetResponseData(
            std::bind<bool>(read_buffer, std::placeholders::_1,
                            std::placeholders::_2, std::ref(response)));
        std::wcout << L"[client] response: " << response << std::endl;
      }
    }
  }

  client.EndSession();

  system("pause");
  return 0;
}

class TestRequestHandler : public weasel::RequestHandler {
 public:
  TestRequestHandler() : m_counter(0) {
    std::cerr << "handler ctor." << std::endl;
  }
  virtual ~TestRequestHandler() {
    std::cerr << "handler dtor: " << m_counter << std::endl;
  }
  virtual UINT FindSession(UINT session_id) {
    std::cerr << "FindSession: " << session_id << std::endl;
    return (session_id <= m_counter ? session_id : 0);
  }
  virtual UINT AddSession(LPWSTR buffer) {
    std::cerr << "AddSession: " << m_counter + 1 << std::endl;
    return ++m_counter;
  }
  virtual UINT RemoveSession(UINT session_id) {
    std::cerr << "RemoveClient: " << session_id << std::endl;
    return 0;
  }
  virtual BOOL ProcessKeyEvent(weasel::KeyEvent keyEvent,
                               UINT session_id,
                               EatLine eat) {
    std::cerr << "ProcessKeyEvent: " << session_id
              << " keycode: " << keyEvent.keycode << " mask: " << keyEvent.mask
              << std::endl;
    eat(std::wstring(L"Greeting=Hello, 小狼毫.\n"));
    return TRUE;
  }

  virtual void GetServerInfo(DWORD session_id, EatLine eat) {
    std::cerr << "GetServerInfo: session_id = " << session_id << std::endl;
    if (eat)
      eat(std::wstring(L"server_info=weasel-test+modern\n.\n"));
  }
  virtual bool HandleExtension(const std::wstring& message_id,
                               const std::wstring& payload,
                               DWORD session_id,
                               EatLine eat) {
    std::cerr << "HandleExtension: " << message_id << std::endl;
    if (!eat)
      return false;
    if (message_id == L"ping") {
      eat(std::wstring(L"extension.pong=pong\n.\n"));
      return true;
    }
    if (message_id == L"server_info") {
      eat(std::wstring(L"server_info=weasel-test+modern\n.\n"));
      return true;
    }
    if (message_id == L"tool.open") {
      // 模拟剪贴板面板：回写 status.composing=1 + 两行假候选。
      // 注意：单测不做 boost 序列化，只验证文本行往返（真实 ctx.cand 行
      // 由 RimeWithWeasel 服务端产生，Windows 端联调时观察）。
      eat(std::wstring(L"action=status\n"
                       L"status.composing=1\n"
                       L"tool.mode=clipboard\n"
                       L".\n"));
      return true;
    }
    if (message_id == L"tool.pick") {
      // 模拟选中上屏：回写 commit 行。
      eat(std::wstring(L"action=commit,status\n"
                       L"commit=测试上屏文本\n"
                       L"status.composing=0\n"
                       L"tool.mode=none\n"
                       L".\n"));
      return true;
    }
    if (message_id == L"tool.cancel" || message_id == L"tool.page") {
      eat(std::wstring(L"action=status\n"
                       L"status.composing=0\n"
                       L"tool.mode=none\n"
                       L".\n"));
      return true;
    }

    return false;  // 其它 message_id 一律 not-supported
  }

 private:
  unsigned int m_counter;
};

int server_main() {
  HRESULT hRes = _Module.Init(NULL, GetModuleHandle(NULL));
  ATLASSERT(SUCCEEDED(hRes));

  weasel::Server server;
  // weasel::UI ui;
  // const std::unique_ptr<weasel::RequestHandler> handler(new
  // RimeWithWeaselHandler(&ui));
  const std::unique_ptr<weasel::RequestHandler> handler(new TestRequestHandler);

  server.SetRequestHandler(handler.get());
  if (!server.Start())
    return -4;
  std::cerr << "server running." << std::endl;
  int ret = server.Run();
  std::cerr << "server quitting." << std::endl;
  return ret;
}
