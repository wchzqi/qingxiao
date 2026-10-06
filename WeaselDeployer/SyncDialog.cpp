// SyncDialog.cpp — 青简 P1-B6 无账号同步：云盘同步目录图形化对话框（实现）
#include "stdafx.h"

#include "SyncDialog.h"

#include "Configurator.h"
#include <WeaselUtility.h>  // u8tow / wtou8 / WeaselUserDataPath
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#pragma warning(disable : 4005)
#include <rime_api.h>
#pragma warning(default : 4005)

namespace {

// 取当前生效的同步目录（UTF-8）：
//   1) 优先 installation.yaml 的 sync_dir（本对话框写入的云盘目录）
//   2) 缺失时回退 librime 默认（get_user_data_sync_dir，CP_ACP，同 DictManagementDialog:110）
std::string GetEffectiveSyncDirUtf8(const weasel::SyncOptions& options) {
  if (!options.sync_dir().empty()) {
    return options.sync_dir();
  }
  char dir[MAX_PATH] = {0};
  RimeApi* rime = rime_get_api();
  if (rime) {
    rime->get_user_data_sync_dir(dir, _countof(dir));
  }
  if (dir[0] != '\0') {
    // librime 该 API 在 Windows 下返回 ACP，转 UTF-8 统一存储/展示
    return wstring_to_string(string_to_wstring(dir, CP_ACP), CP_UTF8);
  }
  return "";
}

}  // namespace

SyncDialog::SyncDialog(Configurator* configurator)
    : configurator_(configurator) {}

SyncDialog::~SyncDialog() {}

LRESULT SyncDialog::OnInitDialog(UINT, WPARAM, LPARAM, BOOL&) {
  sync_dir_.Attach(GetDlgItem(IDC_SYNC_DIR));
  browse_.Attach(GetDlgItem(IDC_SYNC_BROWSE));
  sync_now_.Attach(GetDlgItem(IDC_SYNC_NOW));
  open_explorer_.Attach(GetDlgItem(IDC_SYNC_OPEN_EXPLORER));
  status_.Attach(GetDlgItem(IDC_SYNC_STATUS));

  options_.Load();
  options_.EnsureInstallationId();  // 缺则生成并写盘（幂等）
  Populate();

  CenterWindow();
  return TRUE;
}

LRESULT SyncDialog::OnClose(UINT, WPARAM, LPARAM, BOOL&) {
  EndDialog(IDCANCEL);
  return 0;
}

void SyncDialog::Populate() {
  // 只读框：当前生效同步目录
  std::string dir_utf8 = GetEffectiveSyncDirUtf8(options_);
  sync_dir_.SetWindowText(u8tow(dir_utf8).c_str());

  // 状态提示：本机 installation_id + 最近同步目录 + 多设备合并说明
  std::wstring id = u8tow(options_.installation_id());
  std::wstring dir_w = u8tow(dir_utf8);
  if (dir_w.empty()) {
    dir_w = L"（未设置，使用默认目录）";
  }
  std::wstring status =
      L"本机 ID：" + id + L"\n" +
      L"最近同步目录：" + dir_w + L"\n\n" +
      L"多设备使用同一云盘文件夹后点「立即同步」，Rime 会把词库快照\n"
      L"在各设备间自动合并（取并集，无需账号）。请在每台设备都选择同一个\n"
      L"云盘目录，然后分别执行一次同步。";
  status_.SetWindowText(status.c_str());

  // 未选目录时「在资源管理器中打开」置灰
  open_explorer_.EnableWindow(dir_utf8.empty() ? FALSE : TRUE);
}

std::string SyncDialog::PickFolderUtf8() {
  std::wstring picked;
  // 现代文件夹选择对话框（IFileOpenDialog + FOS_PICKFOLDERS），
  // 与 DictManagementDialog.cpp 的 DoFileDialog 同一 COM 风格（ATL::CComPtr）。
  CComPtr<IFileOpenDialog> spFolderDialog;
  if (SUCCEEDED(spFolderDialog.CoCreateInstance(__uuidof(FileOpenDialog)))) {
    DWORD flags = 0;
    spFolderDialog->GetOptions(&flags);
    spFolderDialog->SetOptions(flags | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    spFolderDialog->SetTitle(L"选择用于同步的云盘文件夹");
    if (SUCCEEDED(spFolderDialog->Show(m_hWnd))) {
      CComPtr<IShellItem> spResult;
      if (SUCCEEDED(spFolderDialog->GetResult(&spResult))) {
        PWSTR name = nullptr;
        if (SUCCEEDED(spResult->GetDisplayName(SIGDN_FILESYSPATH, &name))) {
          picked = name;
          CoTaskMemFree(name);
        }
      }
    }
  }
  if (picked.empty()) {
    return "";  // 用户取消
  }
  // 统一为原生分隔符再转 UTF-8 落盘
  std::wstring native = std::filesystem::path(picked).make_preferred().wstring();
  return wtou8(native);
}

LRESULT SyncDialog::OnBrowse(WORD, WORD, HWND, BOOL&) {
  std::string dir = PickFolderUtf8();
  if (dir.empty()) {
    return 0;  // 取消
  }
  // 写入 installation.yaml 的 sync_dir（保留其他字段，幂等）
  if (!options_.SetSyncDir(dir)) {
    MessageBox(L"写入 installation.yaml 失败，请检查用户目录权限。", L"青简",
               MB_OK | MB_ICONERROR);
    return 0;
  }
  Populate();
  return 0;
}

LRESULT SyncDialog::OnSyncNow(WORD, WORD, HWND, BOOL&) {
  if (!configurator_) {
    return 0;
  }
  // 复用 Configurator::SyncUserData()（Configurator.cpp:198）整条既有链路：
  //   Mutex -> StartMaintenance -> sync_user_data -> join_maintenance_thread -> EndMaintenance。
  // 多设备合并完全由 librime 完成，本处不实现任何合并算法。
  int rc = configurator_->SyncUserData();
  if (rc == 0) {
    MessageBox(L"已发起同步。词库快照将在后台合并完成。", L"青简",
               MB_OK | MB_ICONINFORMATION);
  } else {
    MessageBox(L"同步未能执行（可能有另一个部署任务正在进行）。", L"青简",
               MB_OK | MB_ICONWARNING);
  }
  Populate();
  return 0;
}

LRESULT SyncDialog::OnOpenExplorer(WORD, WORD, HWND, BOOL&) {
  std::string dir_utf8 = GetEffectiveSyncDirUtf8(options_);
  if (dir_utf8.empty()) {
    return 0;
  }
  std::wstring dir = u8tow(dir_utf8);
  // 目录可能尚不存在（刚选、未同步过）——先确保存在，再用资源管理器打开
  CreateDirectoryW(dir.c_str(), NULL);
  ShellExecuteW(m_hWnd, L"open", dir.c_str(), NULL, NULL, SW_SHOWNORMAL);
  return 0;
}
