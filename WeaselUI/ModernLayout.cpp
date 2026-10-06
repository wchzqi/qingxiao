#include "stdafx.h"
#include "ModernLayout.h"

using namespace weasel;

// ModernLayout::DoLayout
//
// 与 HorizontalLayout::DoLayout 保持一致的横排排版几何（preedit / aux / 分页指示 /
// label+text+comment 逐候选累计、按 max_width 折行、垂直对齐、内容尺寸与状态图标），
// 两处有意差异（详见 ModernLayout.h）：
//   (a) 不做“每行末位候选右缘拉伸到窗口宽度”——卡片按内容收紧，体现紧凑卡片风；
//   (b) 尾段不调用 _PrepareRoundInfo(dc)（会 tmp[LAYOUT_MODERN] 越界），
//       直接把每个候选/预编辑高亮矩形置为独立全圆角卡片。
void ModernLayout::DoLayout(CDCHandle dc, PDWR pDWR) {
  CSize size;
  int width = offsetX + real_margin_x, height = offsetY + real_margin_y;
  int w = offsetX + real_margin_x;

  /* calc mark_text sizes */
  if ((_style.hilited_mark_color & 0xff000000)) {
    CSize sg;
    if (candidates_count) {
      if (_style.mark_text.empty())
        GetTextSizeDW(L"|", 1, pDWR->pTextFormat, pDWR, &sg);
      else
        GetTextSizeDW(_style.mark_text, _style.mark_text.length(),
                      pDWR->pTextFormat, pDWR, &sg);
    }

    mark_width = sg.cx;
    mark_height = sg.cy;
    if (_style.mark_text.empty()) {
      mark_width = mark_height / 7;
      if (_style.linespacing && _style.baseline)
        mark_width =
            (int)((float)mark_width / ((float)_style.linespacing / 100.0f));
      mark_width = max(mark_width, 6);
    }
    mark_gap = (_style.mark_text.empty()) ? mark_width
                                          : mark_width + _style.hilite_spacing;
  }
  int base_offset = ((_style.hilited_mark_color & 0xff000000)) ? mark_gap : 0;

  // calc page indicator
  CSize pgszl, pgszr;
  if (!IsInlinePreedit()) {
    GetTextSizeDW(pre, pre.length(), pDWR->pPreeditTextFormat, pDWR, &pgszl);
    GetTextSizeDW(next, next.length(), pDWR->pPreeditTextFormat, pDWR, &pgszr);
  }
  bool page_en = (_style.prevpage_color & 0xff000000) &&
                 (_style.nextpage_color & 0xff000000);
  int pgw = page_en ? (pgszl.cx + pgszr.cx + _style.hilite_spacing +
                       _style.hilite_padding_x * 2)
                    : 0;
  int pgh = page_en ? max(pgszl.cy, pgszr.cy) : 0;

  /* Preedit */
  if (!IsInlinePreedit() && !_context.preedit.str.empty()) {
    size = GetPreeditSize(dc, _context.preedit, pDWR->pPreeditTextFormat, pDWR);
    int szx = pgw, szy = max(size.cy, pgh);
    // icon size higher then preedit text
    int yoffset = (STATUS_ICON_SIZE >= szy && ShouldDisplayStatusIcon())
                      ? (STATUS_ICON_SIZE - szy) / 2
                      : 0;
    _preeditRect.SetRect(w, height + yoffset, w + size.cx,
                         height + yoffset + size.cy);
    height += szy + 2 * yoffset + _style.spacing;
    width = max(width, real_margin_x * 2 + size.cx + szx);
    if (ShouldDisplayStatusIcon())
      width += STATUS_ICON_SIZE;
  }

  /* Auxiliary */
  if (!_context.aux.str.empty()) {
    size = GetPreeditSize(dc, _context.aux, pDWR->pPreeditTextFormat, pDWR);
    // icon size higher then auxiliary text
    int yoffset = (STATUS_ICON_SIZE >= size.cy && ShouldDisplayStatusIcon())
                      ? (STATUS_ICON_SIZE - size.cy) / 2
                      : 0;
    _auxiliaryRect.SetRect(w, height + yoffset, w + size.cx,
                           height + yoffset + size.cy);
    height += size.cy + 2 * yoffset + _style.spacing;
    width = max(width, real_margin_x * 2 + size.cx);
  }

  int row_cnt = 0;
  int max_width_of_rows = 0;
  int height_of_rows[MAX_CANDIDATES_COUNT] = {0};    // height of every row
  int row_of_candidate[MAX_CANDIDATES_COUNT] = {0};  // row info of every cand
  int mintop_of_rows[MAX_CANDIDATES_COUNT] = {0};
  // only when there are candidates
  if (candidates_count) {
    w = offsetX + real_margin_x;
    for (auto i = 0; i < candidates_count && i < MAX_CANDIDATES_COUNT; ++i) {
      int current_cand_width = 0;
      if (i > 0)
        w += _style.candidate_spacing;
      if (id == i)
        w += base_offset;
      /* Label */
      std::wstring label =
          GetLabelText(labels, i, _style.label_text_format.c_str());
      GetTextSizeDW(label, label.length(), pDWR->pLabelTextFormat, pDWR, &size);
      _candidateLabelRects[i].SetRect(w, height, w + size.cx * labelFontValid,
                                      height + size.cy);
      w += size.cx * labelFontValid;
      current_cand_width += size.cx * labelFontValid;

      /* Text */
      w += _style.hilite_spacing;
      const std::wstring& text = candidates.at(i).str;
      GetTextSizeDW(text, text.length(), pDWR->pTextFormat, pDWR, &size);
      _candidateTextRects[i].SetRect(w, height, w + size.cx * textFontValid,
                                     height + size.cy);
      w += size.cx * textFontValid;
      current_cand_width += (size.cx + _style.hilite_spacing) * textFontValid;

      /* Comment */
      bool cmtFontNotTrans =
          (i == id && (_style.hilited_comment_text_color & 0xff000000)) ||
          (i != id && (_style.comment_text_color & 0xff000000));
      if (!comments.at(i).str.empty() && cmtFontValid && cmtFontNotTrans) {
        const std::wstring& comment = comments.at(i).str;
        GetTextSizeDW(comment, comment.length(), pDWR->pCommentTextFormat,
                      pDWR, &size);
        w += _style.hilite_spacing;
        _candidateCommentRects[i].SetRect(w, height,
                                          w + size.cx * cmtFontValid,
                                          height + size.cy);
        w += size.cx * cmtFontValid;
        current_cand_width += (size.cx + _style.hilite_spacing) * cmtFontValid;
      } else /* Used for highlighted candidate calculation below */
        _candidateCommentRects[i].SetRect(w, height, w, height + size.cy);

      int base_left = (i == id) ? _candidateLabelRects[i].left - base_offset
                                : _candidateLabelRects[i].left;
      // if not the first candidate of current row, and current candidate's
      // right > _style.max_width
      if (_style.max_width > 0 && (base_left > real_margin_x + offsetX) &&
          (_candidateCommentRects[i].right - offsetX + real_margin_x >
           _style.max_width)) {
        // max_width_of_rows current row
        max_width_of_rows =
            max(max_width_of_rows, _candidateCommentRects[i - 1].right);
        w = offsetX + real_margin_x + (i == id ? base_offset : 0);
        int ofx = w - _candidateLabelRects[i].left;
        int ofy = height_of_rows[row_cnt] + _style.candidate_spacing;
        // offset rects to next row
        _candidateLabelRects[i].OffsetRect(ofx, ofy);
        _candidateTextRects[i].OffsetRect(ofx, ofy);
        _candidateCommentRects[i].OffsetRect(ofx, ofy);
        // max width of next row, if it's the last candidate, make sure
        // max_width_of_rows calc right
        max_width_of_rows =
            max(max_width_of_rows, _candidateCommentRects[i].right);
        mintop_of_rows[row_cnt] = height;
        height += ofy;
        // re calc rect position, decrease offsetX for origin
        w += current_cand_width;
        row_cnt++;
      } else
        max_width_of_rows = max(max_width_of_rows, w);
      // calculate height of current row is the max of three rects
      mintop_of_rows[row_cnt] = height;
      height_of_rows[row_cnt] =
          max(height_of_rows[row_cnt], _candidateLabelRects[i].Height());
      height_of_rows[row_cnt] =
          max(height_of_rows[row_cnt], _candidateTextRects[i].Height());
      height_of_rows[row_cnt] =
          max(height_of_rows[row_cnt], _candidateCommentRects[i].Height());
      // set row info of current candidate
      row_of_candidate[i] = row_cnt;
    }

    // reposition for alignment, exp when rect height not equal to
    // height_of_rows
    for (auto i = 0; i < candidates_count && i < MAX_CANDIDATES_COUNT; ++i) {
      int base_left = (i == id) ? _candidateLabelRects[i].left - base_offset
                                : _candidateLabelRects[i].left;
      _candidateRects[i].SetRect(base_left, mintop_of_rows[row_of_candidate[i]],
                                 _candidateCommentRects[i].right,
                                 mintop_of_rows[row_of_candidate[i]] +
                                     height_of_rows[row_of_candidate[i]]);
      int ol = 0, ot = 0, oc = 0;
      if (_style.align_type == UIStyle::ALIGN_CENTER) {
        ol = (height_of_rows[row_of_candidate[i]] -
              _candidateLabelRects[i].Height()) /
             2;
        ot = (height_of_rows[row_of_candidate[i]] -
              _candidateTextRects[i].Height()) /
             2;
        oc = (height_of_rows[row_of_candidate[i]] -
              _candidateCommentRects[i].Height()) /
             2;
      } else if (_style.align_type == UIStyle::ALIGN_BOTTOM) {
        ol = (height_of_rows[row_of_candidate[i]] -
              _candidateLabelRects[i].Height());
        ot = (height_of_rows[row_of_candidate[i]] -
              _candidateTextRects[i].Height());
        oc = (height_of_rows[row_of_candidate[i]] -
              _candidateCommentRects[i].Height());
      }
      _candidateLabelRects[i].OffsetRect(0, ol);
      _candidateTextRects[i].OffsetRect(0, ot);
      _candidateCommentRects[i].OffsetRect(0, oc);
    }
    height = mintop_of_rows[row_cnt] + height_of_rows[row_cnt] - offsetY;
    width = max(width, max_width_of_rows);
  } else {
    height -= _style.spacing + offsetY;
    width += _style.hilite_spacing + _style.border;
  }

  width += real_margin_x;
  height += real_margin_y;

  if (candidates_count) {
    width = max(width, _style.min_width);
    height = max(height, _style.min_height);
  }
  // 现代卡片布局刻意不做 HorizontalLayout 中“每行末位候选右缘拉伸到窗口宽度”的
  // 处理：卡片按自身内容收紧，配合 candidate_spacing 得到紧凑卡片观感。
  _highlightRect = _candidateRects[id];
  UpdateStatusIconLayout(&width, &height);
  _contentSize.SetSize(width + offsetX, height + 2 * offsetY);
  _contentRect.SetRect(0, 0, _contentSize.cx, _contentSize.cy);

  // calc page indicator
  if (page_en && candidates_count && !_style.inline_preedit) {
    int _prex = _contentSize.cx - offsetX - real_margin_x +
                _style.hilite_padding_x - pgw;
    int _prey = (_preeditRect.top + _preeditRect.bottom) / 2 - pgszl.cy / 2;
    _prePageRect.SetRect(_prex, _prey, _prex + pgszl.cx, _prey + pgszl.cy);
    _nextPageRect.SetRect(_prePageRect.right + _style.hilite_spacing, _prey,
                          _prePageRect.right + _style.hilite_spacing + pgszr.cx,
                          _prey + pgszr.cy);
    if (ShouldDisplayStatusIcon()) {
      _prePageRect.OffsetRect(-STATUS_ICON_SIZE, 0);
      _nextPageRect.OffsetRect(-STATUS_ICON_SIZE, 0);
    }
  }

  // prepare temp rect _bgRect（保留与其它布局一致的约定，供 GDI+ 路径裁剪参考）
  CopyRect(_bgRect, _contentRect);
  _bgRect.DeflateRect(offsetX + 1, offsetY + 1);

  // 现代化卡片：每个候选高亮矩形都是独立圆角卡片。
  // 不调用 StandardLayout::_PrepareRoundInfo(dc)——该函数内部
  // `const int tmp[5] = {...}; int t = tmp[_style.layout_type];`
  // 硬编码 5 元素表，而 LAYOUT_MODERN == 5 会数组越界读取。
  // 现代布局语义为“每张卡片四角全圆角、彼此独立”，直接赋值：
  // Hemispherical=false 时 WeaselPanel::_HighlightText 走
  // GraphicsRoundRectPath(rc, round_corner) 分支，忽略四角开关，
  // 绘制统一圆角矩形卡片。
  for (auto i = 0; i < candidates_count && i < MAX_CANDIDATES_COUNT; ++i) {
    _roundInfo[i].IsTopLeftNeedToRound = true;
    _roundInfo[i].IsTopRightNeedToRound = true;
    _roundInfo[i].IsBottomLeftNeedToRound = true;
    _roundInfo[i].IsBottomRightNeedToRound = true;
    _roundInfo[i].Hemispherical = false;
  }
  // 预编辑高亮同样作为独立圆角卡片。
  _textRoundInfo.IsTopLeftNeedToRound = true;
  _textRoundInfo.IsTopRightNeedToRound = true;
  _textRoundInfo.IsBottomLeftNeedToRound = true;
  _textRoundInfo.IsBottomRightNeedToRound = true;
  _textRoundInfo.Hemispherical = false;

  // truely draw content size calculation
  _contentRect.DeflateRect(offsetX, offsetY);
}
