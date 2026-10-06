// ============================================================================
// 青筱（QingXiaoType）P2-E：翻译本地兜底词典（平台中立核心）
//
// 归属：RimeWithWeasel/（TranslationService 的离线降级组件）。
//
// 设计说明（为何独立成文件）：
//   - TranslationService.cpp 依赖 <windows.h>/<winhttp.h>/<rime_api.h>，只能在
//     Windows 编译；而本地兜底表是纯算法 + 静态数据，无任何平台依赖。
//   - 单独拆出本头/源，便于在本机 Linux 用 g++ 直接单测（交付物 D），
//     也便于 Windows 侧 TranslationService.cpp 直接 include 复用。
//
// 语义（简化实现，明示）：
//   - 这是一份【很小的内置常见短语/单字对照】，仅作离线兜底与「未联网/被 Gate
//     拒绝时」的最小可用，绝不追求覆盖度。命中即返回译文；未命中返回空串，
//     由调用方决定降级（云端或无提示回退）。
//   - 方向：默认中→英（zh→en）；若原文是 ASCII（英文字母/数字为主），则按
//     英→中（en→zh）查反向表。
//   - 查表策略：整串精确匹配优先；未命中则尝试「去首尾空白后精确匹配」。
//     不做模糊/分词/最长前缀匹配（避免误译，保持可预期）。
//
// 隐私：本模块零联网、零文件 IO、零日志；译文不落盘。
// ============================================================================
#pragma once

#include <string>

namespace weasel {
namespace translate_local {

// 查询本地兜底翻译。
//   text_utf8    待译文本（UTF-8）。
//   target_lang  目标语言码（如 "en"/"zh"）。当前简化实现只支持 en↔zh。
// 返回：命中时返回译文（UTF-8）；未命中/方向不支持时返回空串。
// 注意：本函数不抛异常（表为静态 const，查询只读）。
std::string Lookup(const std::string& text_utf8,
                   const std::string& target_lang);

// 仅供单测/调试：内置表条目总数（便于自检表已加载）。
size_t EntryCount();

}  // namespace translate_local
}  // namespace weasel
