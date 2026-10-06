// CustomPhraseDialog.h — 青简 P1-B7（W8.1）：自定义短语图形管理对话框
//
// 【职责】图形化管理用户目录下的 custom_phrase.txt（Rime 预置词表/自定义短语）。
//   - 解析：每行 `词<TAB>编码<TAB>权重`（权重可省）；UTF-8；`#` 开头注释行原样保留；
//     空行/格式异常行原样保留（不丢数据）。
//   - 展示：ListView（报表模式）三列：词 / 编码 / 权重。
//   - 编辑：新增 / 修改 / 删除当前行。
//   - 保存：写回 custom_phrase.txt，保留原有注释与行顺序；保存前备份为 custom_phrase.txt.bak。
//   - 导出：另存为 UTF-8 .txt（仅数据行）。
//   - 导入：选择 .txt，按 (词,编码) 去重后合并追加到末尾。
//
// 【硬约束】
//   - 独立对话框，不改动既有 DictManagementDialog（B5 在其上加词库下载区域，避免冲突）。
//   - 不碰 librime 内部 API：只读写用户词典文本文件；部署交给既有托盘/Configurator 触发。
//   - 不新增 IPC 命令 / 数据结构。
//
// 【资源 ID】（见 resource.h 追加段，数值避开既有 1000~1011 与 B3 的 50001~50011）：
//   IDD_CUSTOM_PHRASE          200
//   IDC_CUSTOM_PHRASE_LIST     50031   // CListViewCtrl
//   IDC_CUSTOM_PHRASE_WORD      50032   // CEdit 词
//   IDC_CUSTOM_PHRASE_CODE      50033   // CEdit 编码
//   IDC_CUSTOM_PHRASE_WEIGHT    50034   // CEdit 权重
//   IDC_CP_ADD                  50035   // 新增
//   IDC_CP_MODIFY               50036   // 修改
//   IDC_CP_DELETE               50037   // 删除
//   IDC_CP_EXPORT               50038   // 导出
//   IDC_CP_IMPORT               50039   // 导入
//   IDC_OPEN_CUSTOM_PHRASE      50040   // 入口按钮（挂在词库管理对话框/Configurator）
#pragma once

#include "resource.h"

#include <string>
#include <vector>

// WTL 控件基类均来自 stdafx.h（wtl/atlctrls.h），此处不再重复 include。
class CustomPhraseDialog : public CDialogImpl<CustomPhraseDialog> {
 public:
  enum { IDD = IDD_CUSTOM_PHRASE };

  CustomPhraseDialog();
  ~CustomPhraseDialog();

  // 关闭后供调用方（Configurator）判断是否发生过保存：true=写盘成功过，应触发重新部署。
  bool dirty() const { return dirty_; }

 protected:
  BEGIN_MSG_MAP(CustomPhraseDialog)
  MESSAGE_HANDLER(WM_INITDIALOG, OnInitDialog)
  MESSAGE_HANDLER(WM_CLOSE, OnClose)
  COMMAND_ID_HANDLER(IDOK, OnSaveAndClose)
  COMMAND_ID_HANDLER(IDCANCEL, OnCancel)
  COMMAND_ID_HANDLER(IDC_CP_ADD, OnAdd)
  COMMAND_ID_HANDLER(IDC_CP_MODIFY, OnModify)
  COMMAND_ID_HANDLER(IDC_CP_DELETE, OnDelete)
  COMMAND_ID_HANDLER(IDC_CP_EXPORT, OnExport)
  COMMAND_ID_HANDLER(IDC_CP_IMPORT, OnImport)
  NOTIFY_HANDLER(IDC_CUSTOM_PHRASE_LIST, LVN_ITEMCHANGED, OnListItemChanged)
  END_MSG_MAP()

  LRESULT OnInitDialog(UINT, WPARAM, LPARAM, BOOL&);
  LRESULT OnClose(UINT, WPARAM, LPARAM, BOOL&);
  LRESULT OnSaveAndClose(WORD, WORD, HWND, BOOL&);
  LRESULT OnCancel(WORD, WORD, HWND, BOOL&);
  LRESULT OnAdd(WORD, WORD, HWND, BOOL&);
  LRESULT OnModify(WORD, WORD, HWND, BOOL&);
  LRESULT OnDelete(WORD, WORD, HWND, BOOL&);
  LRESULT OnExport(WORD, WORD, HWND, BOOL&);
  LRESULT OnImport(WORD, WORD, HWND, BOOL&);
  LRESULT OnListItemChanged(int idCtrl, LPNMHDR pnmh, BOOL& bHandled);

  // ---- 数据模型 ----------------------------------------------------------
  // 一条数据行：词 / 编码 / 权重（权重可能缺省）。
  struct PhraseRow {
    std::string word;       // UTF-8
    std::string code;       // UTF-8
    std::string weight;     // UTF-8 数字字符串；空串=权重缺省
    bool has_weight = false;
  };

  // 原始行：注释/空行/异常行原样保留；数据行直接内嵌 PhraseRow。
  // ListView 的第 k 行 = lines_ 中第 k 个 is_data==true 的行（一一对应，删除即整行移除，
  // 不再维护"行下标->rows_下标"的映射，避免删除后下标漂移）。
  struct RawLine {
    bool is_data = false;
    std::string verbatim;   // is_data=false 时的原文（UTF-8，不含行尾换行）
    PhraseRow row;          // is_data=true 时的短语数据
  };

  // ---- 文件读写（UTF-8 字节流，不经过 wfstream 的编码层）------------------
  std::wstring DataFilePath() const;              // <user_data>/custom_phrase.txt
  bool LoadFile(const std::wstring& path);        // 解析到 lines_（保序/保注释）
  bool SaveFile(const std::wstring& path);         // 写回（先备份 .bak）
  static std::vector<std::string> SplitLines(const std::string& content);
  static bool ParseDataLine(const std::string& line, PhraseRow& out);
  static std::string EncodeDataLine(const PhraseRow& row);

  // ---- ListView 刷新 / 控件取值 -----------------------------------------
  void RefreshList();
  void UpdateRowButtons();
  bool GetEdits(std::string& word, std::string& code, std::string& weight);
  // 取 ListView 第 list_index 个数据行在 lines_ 中的下标；找不到返回 -1。
  int DataLineIndexOfListViewItem(int list_index) const;
  // 按 (词,编码) 判重；ignore_line>=0 时跳过该行（修改场景）。
  bool IsDuplicate(const std::string& word, const std::string& code,
                   int ignore_line) const;

  // ---- 控件绑定成员（与既有对话框命名风格一致：下划线结尾）---------------
  CListViewCtrl phrase_list_;
  CEdit word_edit_;
  CEdit code_edit_;
  CEdit weight_edit_;
  CButton add_;
  CButton modify_;
  CButton delete_;
  CButton export_;
  CButton import_;

  std::vector<RawLine> lines_;        // 全部原始行（保序/保注释/数据行内嵌）
  bool dirty_ = false;                 // 是否有未部署的写盘
};
