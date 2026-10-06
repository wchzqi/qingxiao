#pragma once

#include "StandardLayout.h"

namespace weasel {

// P0 MVP：现代化候选窗布局 ModernLayout（圆角卡片式候选、紧凑间距、明暗双主题友好）。
//
// 设计说明：
//   1. 本类与 HorizontalLayout 一样仅重写 DoLayout()，其余 Layout/StandardLayout
//      的虚函数实现（GetContentSize/GetCandidateRect/...）全部复用基类，不重复实现。
//   2. 与 HorizontalLayout 的唯一差异在“高亮语义”：横排候选的每一项都渲染成一张
//      独立的圆角卡片——
//        - 卡片尺寸 = GetCandidateRect(i)，由 WeaselPanel 在绘制时按
//          hilite_padding_x / hilite_padding_y 向外膨胀；
//        - 卡片圆角半径 = UIStyle::round_corner（既有字段）；
//        - 卡片间距 = UIStyle::candidate_spacing（既有字段）；
//        - 卡片内 label/text/comment 间隔 = UIStyle::hilite_spacing（既有字段）。
//   3. 明暗双主题完全复用现有配色字段（back_color / candidate_back_color /
//      candidate_text_color / hilited_candidate_back_color /
//      hilited_candidate_text_color 等），不新增任何 UIStyle 字段（契约冻结）。
//   4. 刻意不调用 StandardLayout::_PrepareRoundInfo(dc)：该函数内部用硬编码的
//      5 元素表 `tmp[_style.layout_type]` 索引，而 LAYOUT_MODERN == 5 会数组越界；
//      现代布局的语义是“每张卡片四角全圆角、彼此独立”，改为直接给 _roundInfo[i]
//      赋值（Hemispherical=false，四角全 true）。此时 WeaselPanel::_HighlightText
//      走 GraphicsRoundRectPath(rc, round_corner) 分支，绘制统一圆角矩形卡片。
class ModernLayout : public StandardLayout {
 public:
  ModernLayout(const UIStyle& style,
               const Context& context,
               const Status& status,
               PDWR pDWR)
      : StandardLayout(style, context, status, pDWR) {}

  virtual void DoLayout(CDCHandle dc, PDWR pDWR = NULL);
};

};  // namespace weasel
