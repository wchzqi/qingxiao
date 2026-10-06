// CustomPhraseDialog.cpp — 青简 P1-B7（W8.1）：自定义短语图形管理对话框（实现）
//
// 设计要点见 CustomPhraseDialog.h。本文件不依赖 DictManagementDialog，不碰 librime 内部 API。
#include "stdafx.h"
#include "CustomPhraseDialog.h"

#include <WeaselUtility.h>   // WeaselUserDataPath() / u8tow() / wtou8()

#include <windows.h>
#include <shobjidl.h>
#include <shellapi.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <system_error>

namespace {

// 与 DictManagementDialog.cpp 同源的文件选择对话框助手（本对话框独立复制一份，
// 避免跨 .cpp 暴露内部 static；行为完全一致）。
template <typename T, typename U>
std::wstring DoFileDialog(HWND hwndOwner, LPCWSTR title, UINT filterSize,
                          COMDLG_FILTERSPEC filter[], LPCWSTR filename,
                          LPCWSTR defExt) {
  std::wstring path;
  CoInitialize(NULL);
  CComPtr<T> spFileDialog;
  if (SUCCEEDED(spFileDialog.CoCreateInstance(__uuidof(U)))) {
    spFileDialog->SetFileTypes(filterSize, filter);
    spFileDialog->SetTitle(title);
    if (filename)
      spFileDialog->SetFileName(filename);
    spFileDialog->SetDefaultExtension(defExt);
    if (SUCCEEDED(spFileDialog->Show(hwndOwner))) {
      CComPtr<IShellItem> spResult;
      if (SUCCEEDED(spFileDialog->GetResult(&spResult))) {
        wchar_t* name = nullptr;
        if (SUCCEEDED(spResult->GetDisplayName(SIGDN_FILESYSPATH, &name))) {
          path = name;
          CoTaskMemFree(name);
        }
      }
    }
  }
  CoUninitialize();
  return path;
}

void ShowMessage(HWND owner, const std::wstring& text, UINT flags) {
  ::MessageBoxW(owner, text.c_str(), L"青简输入法 · 自定义短语", flags);
}

}  // namespace

// ===========================================================================
// 构造 / 路径
// ===========================================================================

CustomPhraseDialog::CustomPhraseDialog() = default;
CustomPhraseDialog::~CustomPhraseDialog() = default;

std::wstring CustomPhraseDialog::DataFilePath() const {
  // WeaselUserDataPath() == %AppData%\Rime （与 Configurator.cpp 同源）。
  return WeaselUserDataPath() / L"custom_phrase.txt";
}

// ===========================================================================
// 解析 / 编码
// ===========================================================================

std::vector<std::string> CustomPhraseDialog::SplitLines(
    const std::string& content) {
  std::vector<std::string> out;
  std::string cur;
  out.reserve(64);
  for (size_t i = 0; i < content.size(); ++i) {
    char c = content[i];
    if (c == '\n') {
      // 兼容 CRLF：剥掉行尾 '\r'
      if (!cur.empty() && cur.back() == '\r')
        cur.pop_back();
      out.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  if (!cur.empty()) {
    if (cur.back() == '\r')
      cur.pop_back();
    out.push_back(cur);
  }
  return out;
}

bool CustomPhraseDialog::ParseDataLine(const std::string& line,
                                        PhraseRow& out) {
  // 必须含 TAB；列：词 <TAB> 编码 [ <TAB> 权重 ]
  // 权重缺省容错：只有两列也接受。
  std::vector<std::string> cols;
  std::string cell;
  for (size_t i = 0; i <= line.size(); ++i) {
    if (i == line.size() || line[i] == '\t') {
      cols.push_back(cell);
      cell.clear();
    } else {
      cell.push_back(line[i]);
    }
  }
  if (cols.size() < 2)
    return false;             // 没有 TAB：交给上层当注释/异常行原样保留
  if (cols[0].empty() || cols[1].empty())
    return false;             // 词或编码为空：不是合法数据行，原样保留
  out.word = cols[0];
  out.code = cols[1];
  if (cols.size() >= 3 && !cols[2].empty()) {
    out.weight = cols[2];
    out.has_weight = true;
  } else {
    out.weight.clear();
    out.has_weight = false;
  }
  return true;
}

std::string CustomPhraseDialog::EncodeDataLine(const PhraseRow& row) {
  std::string s = row.word;
  s.push_back('\t');
  s += row.code;
  if (row.has_weight && !row.weight.empty()) {
    s.push_back('\t');
    s += row.weight;
  }
  return s;
}

// ===========================================================================
// 读 / 写文件（UTF-8 字节流；BOM 自动剥离；CRLF 写盘）
// ===========================================================================

bool CustomPhraseDialog::LoadFile(const std::wstring& path) {
  lines_.clear();
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    // 文件不存在：以一条默认注释开头，等待用户新增。
    RawLine hdr;
    hdr.is_data = false;
    hdr.verbatim = "# 青简自定义短语：每行 词<TAB>编码<TAB>权重（权重可省）";
    lines_.push_back(hdr);
    return true;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  std::string content = ss.str();
  // 剥离 UTF-8 BOM（EF BB BF）
  if (content.size() >= 3 &&
      (unsigned char)content[0] == 0xEF &&
      (unsigned char)content[1] == 0xBB &&
      (unsigned char)content[2] == 0xBF) {
    content.erase(0, 3);
  }
  std::vector<std::string> raw = SplitLines(content);
  for (const auto& line : raw) {
    RawLine rl;
    bool blank = line.find_first_not_of(" \t\r") == std::string::npos;
    if (line.empty() || blank || line[0] == '#') {
      rl.is_data = false;
      rl.verbatim = line;                       // 注释/空行原样保留
    } else {
      PhraseRow row;
      if (ParseDataLine(line, row)) {
        rl.is_data = true;
        rl.row = row;
      } else {
        rl.is_data = false;
        rl.verbatim = line;                     // 异常行原样保留，不丢数据
      }
    }
    lines_.push_back(rl);
  }
  return true;
}

bool CustomPhraseDialog::SaveFile(const std::wstring& path) {
  // 1) 先备份 .bak（覆盖式）。
  std::error_code ec;
  if (std::filesystem::exists(path, ec)) {
    std::wstring bak = path + L".bak";
    std::filesystem::copy_file(path, bak,
                               std::filesystem::copy_options::overwrite_existing,
                               ec);
    // 备份失败仅记录，不阻断保存（ec 已被静默吞掉；Windows 侧可在调试器观察）。
  }
  // 2) 按 lines_ 原序写回：注释行原样，数据行按当前 PhraseRow 重编码。
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out)
    return false;
  for (const auto& rl : lines_) {
    std::string line = rl.is_data ? EncodeDataLine(rl.row) : rl.verbatim;
    out << line << "\r\n";          // CRLF，Windows Rime 兼容
  }
  out.flush();
  return out.good();
}

// ===========================================================================
// ListView 辅助
// ===========================================================================

int CustomPhraseDialog::DataLineIndexOfListViewItem(int list_index) const {
  int seen = -1;
  for (size_t i = 0; i < lines_.size(); ++i) {
    if (!lines_[i].is_data)
      continue;
    ++seen;
    if (seen == list_index)
      return static_cast<int>(i);
  }
  return -1;
}

bool CustomPhraseDialog::IsDuplicate(const std::string& word,
                                     const std::string& code,
                                     int ignore_line) const {
  for (int i = 0; i < static_cast<int>(lines_.size()); ++i) {
    if (!lines_[i].is_data || i == ignore_line)
      continue;
    if (lines_[i].row.word == word && lines_[i].row.code == code)
      return true;
  }
  return false;
}

void CustomPhraseDialog::RefreshList() {
  phrase_list_.SetRedraw(FALSE);
  phrase_list_.DeleteAllItems();
  int item = 0;
  for (const auto& rl : lines_) {
    if (!rl.is_data)
      continue;
    std::wstring wword = u8tow(rl.row.word);
    std::wstring wcode = u8tow(rl.row.code);
    std::wstring wweight =
        rl.row.has_weight ? u8tow(rl.row.weight) : std::wstring(L"");
    phrase_list_.InsertItem(item, wword.c_str());
    phrase_list_.SetItemText(item, 1, wcode.c_str());
    phrase_list_.SetItemText(item, 2, wweight.c_str());
    ++item;
  }
  phrase_list_.SetRedraw(TRUE);
  UpdateRowButtons();
}

void CustomPhraseDialog::UpdateRowButtons() {
  int sel = phrase_list_.GetNextItem(-1, LVNI_SELECTED);
  BOOL has_sel = (sel >= 0) ? TRUE : FALSE;
  modify_.EnableWindow(has_sel);
  delete_.EnableWindow(has_sel);
}

bool CustomPhraseDialog::GetEdits(std::string& word, std::string& code,
                                   std::string& weight) {
  wchar_t buf[512] = {0};
  word_edit_.GetWindowText(buf, _countof(buf));
  std::wstring wword = buf;
  code_edit_.GetWindowText(buf, _countof(buf));
  std::wstring wcode = buf;
  weight_edit_.GetWindowText(buf, _countof(buf));
  std::wstring wweight = buf;

  // 剥掉首尾空白
  auto trim = [](std::wstring& s) {
    size_t b = s.find_first_not_of(L" \t");
    size_t e = s.find_last_not_of(L" \t");
    s = (b == std::wstring::npos) ? L"" : s.substr(b, e - b + 1);
  };
  trim(wword);
  trim(wcode);
  trim(wweight);

  if (wword.empty() || wcode.empty())
    return false;                       // 词 / 编码必填
  word = wtou8(wword);
  code = wtou8(wcode);
  weight = wtou8(wweight);
  return true;
}

// ===========================================================================
// 消息处理
// ===========================================================================

LRESULT CustomPhraseDialog::OnInitDialog(UINT, WPARAM, LPARAM, BOOL&) {
  phrase_list_.Attach(GetDlgItem(IDC_CUSTOM_PHRASE_LIST));
  word_edit_.Attach(GetDlgItem(IDC_CUSTOM_PHRASE_WORD));
  code_edit_.Attach(GetDlgItem(IDC_CUSTOM_PHRASE_CODE));
  weight_edit_.Attach(GetDlgItem(IDC_CUSTOM_PHRASE_WEIGHT));
  add_.Attach(GetDlgItem(IDC_CP_ADD));
  modify_.Attach(GetDlgItem(IDC_CP_MODIFY));
  delete_.Attach(GetDlgItem(IDC_CP_DELETE));
  export_.Attach(GetDlgItem(IDC_CP_EXPORT));
  import_.Attach(GetDlgItem(IDC_CP_IMPORT));

  // ListView 报表列
  phrase_list_.SetExtendedListViewStyle(
      LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
  phrase_list_.InsertColumn(0, L"词", LVCFMT_LEFT, 180);
  phrase_list_.InsertColumn(1, L"编码", LVCFMT_LEFT, 120);
  phrase_list_.InsertColumn(2, L"权重", LVCFMT_LEFT, 80);

  LoadFile(DataFilePath());
  RefreshList();

  CenterWindow();
  BringWindowToTop();
  return TRUE;
}

LRESULT CustomPhraseDialog::OnClose(UINT, WPARAM, LPARAM, BOOL&) {
  EndDialog(IDCANCEL);
  return 0;
}

LRESULT CustomPhraseDialog::OnCancel(WORD, WORD, HWND, BOOL&) {
  EndDialog(IDCANCEL);
  return 0;
}

LRESULT CustomPhraseDialog::OnSaveAndClose(WORD, WORD, HWND, BOOL&) {
  // IDOK = 保存并关闭：把当前内存模型写回 custom_phrase.txt。
  if (SaveFile(DataFilePath()))
    dirty_ = true;
  else
    ShowMessage(m_hWnd, L"写入 custom_phrase.txt 失败，请检查文件权限。",
                MB_OK | MB_ICONERROR);
  EndDialog(IDOK);
  return 0;
}

LRESULT CustomPhraseDialog::OnAdd(WORD, WORD, HWND, BOOL&) {
  std::string word, code, weight;
  if (!GetEdits(word, code, weight)) {
    ShowMessage(m_hWnd, L"请填写「词」和「编码」，权重可留空。",
                MB_OK | MB_ICONINFORMATION);
    return 0;
  }
  if (IsDuplicate(word, code, -1)) {
    ShowMessage(m_hWnd, L"相同的「词 + 编码」已存在，请勿重复添加。",
                MB_OK | MB_ICONINFORMATION);
    return 0;
  }
  PhraseRow row;
  row.word = word;
  row.code = code;
  if (!weight.empty()) {
    row.weight = weight;
    row.has_weight = true;
  }
  RawLine rl;
  rl.is_data = true;
  rl.row = row;
  lines_.push_back(rl);
  RefreshList();
  // 选中刚追加的最后一行
  int last = phrase_list_.GetItemCount() - 1;
  if (last >= 0)
    phrase_list_.SetItemState(last, LVIS_SELECTED | LVIS_FOCUSED,
                              LVIS_SELECTED | LVIS_FOCUSED);
  return 0;
}

LRESULT CustomPhraseDialog::OnModify(WORD, WORD, HWND, BOOL&) {
  int sel = phrase_list_.GetNextItem(-1, LVNI_SELECTED);
  int line = DataLineIndexOfListViewItem(sel);
  if (line < 0)
    return 0;
  std::string word, code, weight;
  if (!GetEdits(word, code, weight)) {
    ShowMessage(m_hWnd, L"请填写「词」和「编码」，权重可留空。",
                MB_OK | MB_ICONINFORMATION);
    return 0;
  }
  if (IsDuplicate(word, code, line)) {
    ShowMessage(m_hWnd, L"相同的「词 + 编码」已存在于其他行。",
                MB_OK | MB_ICONINFORMATION);
    return 0;
  }
  lines_[line].row.word = word;
  lines_[line].row.code = code;
  if (weight.empty()) {
    lines_[line].row.weight.clear();
    lines_[line].row.has_weight = false;
  } else {
    lines_[line].row.weight = weight;
    lines_[line].row.has_weight = true;
  }
  RefreshList();
  if (sel >= 0 && sel < phrase_list_.GetItemCount())
    phrase_list_.SetItemState(sel, LVIS_SELECTED | LVIS_FOCUSED,
                              LVIS_SELECTED | LVIS_FOCUSED);
  return 0;
}

LRESULT CustomPhraseDialog::OnDelete(WORD, WORD, HWND, BOOL&) {
  int sel = phrase_list_.GetNextItem(-1, LVNI_SELECTED);
  int line = DataLineIndexOfListViewItem(sel);
  if (line < 0)
    return 0;
  lines_.erase(lines_.begin() + line);
  RefreshList();
  return 0;
}

LRESULT CustomPhraseDialog::OnExport(WORD, WORD, HWND, BOOL&) {
  const std::wstring txt_name = L"文本文件 (*.txt)";
  const std::wstring all_name = L"所有文件 (*.*)";
  COMDLG_FILTERSPEC filter[2] = {
      {txt_name.c_str(), L"*.txt"},
      {all_name.c_str(), L"*.*"}};
  std::wstring path = DoFileDialog<IFileSaveDialog, FileSaveDialog>(
      m_hWnd, L"导出自定义短语", ARRAYSIZE(filter), filter,
      L"custom_phrase_export.txt", L"txt");
  if (path.empty())
    return 0;
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    ShowMessage(m_hWnd, L"导出失败：无法写入目标文件。", MB_OK | MB_ICONERROR);
    return 0;
  }
  int count = 0;
  for (const auto& rl : lines_) {
    if (!rl.is_data)
      continue;
    out << EncodeDataLine(rl.row) << "\r\n";
    ++count;
  }
  out.flush();
  ShowMessage(m_hWnd, (L"已导出 " + std::to_wstring(count) + L" 条短语。")
                          .c_str(),
              MB_OK | MB_ICONINFORMATION);
  return 0;
}

LRESULT CustomPhraseDialog::OnImport(WORD, WORD, HWND, BOOL&) {
  const std::wstring txt_name = L"文本文件 (*.txt)";
  const std::wstring all_name = L"所有文件 (*.*)";
  COMDLG_FILTERSPEC filter[2] = {
      {txt_name.c_str(), L"*.txt"},
      {all_name.c_str(), L"*.*"}};
  std::wstring path = DoFileDialog<IFileOpenDialog, FileOpenDialog>(
      m_hWnd, L"导入自定义短语（合并追加，自动去重）", ARRAYSIZE(filter),
      filter, nullptr, L"txt");
  if (path.empty())
    return 0;

  std::ifstream in(path, std::ios::binary);
  if (!in) {
    ShowMessage(m_hWnd, L"导入失败：无法打开目标文件。", MB_OK | MB_ICONERROR);
    return 0;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  std::string content = ss.str();
  if (content.size() >= 3 &&
      (unsigned char)content[0] == 0xEF &&
      (unsigned char)content[1] == 0xBB &&
      (unsigned char)content[2] == 0xBF) {
    content.erase(0, 3);
  }
  int added = 0;
  for (const auto& line : SplitLines(content)) {
    if (line.empty() || line[0] == '#')
      continue;
    PhraseRow row;
    if (!ParseDataLine(line, row))
      continue;
    if (IsDuplicate(row.word, row.code, -1))
      continue;                       // (词,编码) 已存在则跳过
    RawLine rl;
    rl.is_data = true;
    rl.row = row;
    lines_.push_back(rl);
    ++added;
  }
  RefreshList();
  ShowMessage(m_hWnd, (L"导入完成：新增 " + std::to_wstring(added) +
                       L" 条（重复项已自动跳过）。")
                          .c_str(),
              MB_OK | MB_ICONINFORMATION);
  return 0;
}

LRESULT CustomPhraseDialog::OnListItemChanged(int, LPNMHDR, BOOL&) {
  UpdateRowButtons();
  return 0;
}
