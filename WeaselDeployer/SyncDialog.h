// SyncDialog.h — 青简 P1-B6 无账号同步：云盘同步目录图形化对话框
//
// 独立对话框（CDialogImpl），与 B5（扩 DictManagementDialog）、B7（另建独立对话框）
// 三者不共文件、不共 ID 段：
//   - B5：改 WeaselDeployer/DictManagementDialog.{h,cpp}（既有词库对话框）
//   - B7：另建（如 PhraseDialog.*）
//   - B6（本文件）：新建 SyncDialog.{h,cpp} + SyncOptions.{h,cpp}，
//                   控件 ID 独占 50051~50055 段（见 resource.h 补丁）。
//
// 控件：
//   [同步目录:]  [只读框 IDC_SYNC_DIR            ] [选择文件夹… IDC_SYNC_BROWSE]
//               [状态提示 IDC_SYNC_STATUS（多行静态文本）]
//               [立即同步 IDC_SYNC_NOW] [在资源管理器中打开 IDC_SYNC_OPEN_EXPLORER]
//
// 「立即同步」复用 Configurator::SyncUserData()（Configurator.cpp:198）整条既有链路：
//   CreateMutex(WeaselDeployerMutex) -> Client::StartMaintenance ->
//   rime->sync_user_data() -> rime->join_maintenance_thread() -> EndMaintenance。
//   本对话框不自造合并算法——词库快照的多设备合并完全由 librime 内置
//   （installation.yaml + sync_dir 快照 + RimeApi::sync_user_data）完成。
#pragma once

#include "resource.h"

#include "SyncOptions.h"

class Configurator;  // 前向声明：仅借其 SyncUserData()，不引入 Configurator.h 重依赖

// WTL CDialogImpl / CEdit / CButton / CStatic 由 stdafx.h（atlcontrols）带入，
// 与 DictManagementDialog.h 同样用法，无需额外 include。

class SyncDialog : public CDialogImpl<SyncDialog> {
 public:
  enum { IDD = IDD_SYNC_SETTING };

  explicit SyncDialog(Configurator* configurator);
  ~SyncDialog();

 protected:
  BEGIN_MSG_MAP(SyncDialog)
  MESSAGE_HANDLER(WM_INITDIALOG, OnInitDialog)
  MESSAGE_HANDLER(WM_CLOSE, OnClose)
  COMMAND_ID_HANDLER(IDC_SYNC_BROWSE, OnBrowse)
  COMMAND_ID_HANDLER(IDC_SYNC_NOW, OnSyncNow)
  COMMAND_ID_HANDLER(IDC_SYNC_OPEN_EXPLORER, OnOpenExplorer)
  END_MSG_MAP()

  LRESULT OnInitDialog(UINT, WPARAM, LPARAM, BOOL&);
  LRESULT OnClose(UINT, WPARAM, LPARAM, BOOL&);
  LRESULT OnBrowse(WORD, WORD code, HWND, BOOL&);
  LRESULT OnSyncNow(WORD, WORD code, HWND, BOOL&);
  LRESULT OnOpenExplorer(WORD, WORD code, HWND, BOOL&);

 private:
  // 把 options_ 的当前值刷到只读框与状态文本
  void Populate();
  // 调 librime 弹出「选择文件夹」对话框，返回所选目录（UTF-8）；取消返回空串
  std::string PickFolderUtf8();

  CEdit sync_dir_;              // 只读：当前同步目录
  CButton browse_;              // 选择文件夹…
  CButton sync_now_;            // 立即同步
  CButton open_explorer_;       // 在资源管理器中打开
  CStatic status_;              // 多行状态提示

  Configurator* configurator_;   // 非空：借其 SyncUserData()
  weasel::SyncOptions options_;  // installation.yaml 读写
};
