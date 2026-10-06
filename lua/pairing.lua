--[[
青简 P1-B7（W8.1）：标点智能配对  pairing.lua
==============================================================================
参考来源：函数命名与 librime-lua processor 用法参考 rime-ice（雾凇拼音）
         https://github.com/iDvel/rime-ice  （其标点/括号配对思路见 default.yaml
         的 punctuator 段与 lua/ 目录 filter 写法；本脚本为青简自写实现，未整文件拷贝）。

实现要点（务必先读）：
  1. 本脚本角色 = lua_processor（按键级拦截器），在 librime-lua 的 processor 管线
     里注册：schema.yaml 的 `engine/processors:` 追加 `lua_processor@pairing`。
  2. 行为一「自动补全」：当用户直接敲入一个左括号（《 （ 【 「 『 “ ‘ [ ( { ）且
     当前无正在编码的拼音串（context.input 为空），本脚本直接 commit_text(左..右)，
     即一次上屏成对符号，光标落在右括号之后。
  3. 行为二「右括号智能跳过」：当用户紧接着敲右括号、而光标前一字符正是配对的左括号
     （即刚自动补出的那对），本脚本吞掉这次按键（kAccepted），相当于光标"穿过"右括号，
     避免打出连续两个右括号。
  4. 与 librime 内建 punctuator/pairs 的关系：punctuator 方案本身也能做「选《→出《》」，
     二者取一即可；青简默认走本脚本（可在 schema 里摘除 lua_processor@pairing 回退
     到 punctuator/pairs）。详见《自定义短语与标点预置说明.md》。

返回值约定（librime-lua processor）：
  0 = kRejected（放行给后续处理器）  1 = kAccepted（吃掉）  2 = kNoop（不处理）
  新版 librime-lua 也接受字符串 "accepted"/"rejected"/"noop"；本脚本用数字，兼容性更广。
==============================================================================
]]

-- 左括号 -> 右括号 对照表（全角 + 半角常见成对符号）
local OPEN2CLOSE = {
  ["《"] = "》",
  ["（"] = "）",
  ["【"] = "】",
  ["「"] = "」",
  ["『"] = "』",
  ["“"] = "”",
  ["‘"] = "’",
  ["["]  = "]",
  ["("]  = ")",
  ["{"]  = "}",
}

-- 反向表：右括号 -> 左括号（用于"智能跳过"判断）
local CLOSE2OPEN = {}
for op, cl in pairs(OPEN2CLOSE) do
  CLOSE2OPEN[cl] = op
end

-- 安全地取"光标前最近一个字符"（正在编码串为空时退回最近一次上屏文本末尾）。
-- 不同 librime-lua 版本暴露的 context API 略有差异，全部做 nil 容错。
local function last_char(env)
  local ctx = env and env.engine and env.engine.context
  if not ctx then return nil end
  -- ① 正在编码的串末尾（如未上屏的字母/符号）
  local ok, input = pcall(function() return ctx.input end)
  if ok and input and #input > 0 then
    return input:sub(-1)
  end
  -- ② 退回最近一次 commit 的末字符
  local ch = nil
  pcall(function()
    local hist = ctx.commit_history
    if hist and hist.latest then
      local latest = hist:latest()
      if latest and latest.text then
        local t = latest.text
        ch = t:sub(-1)
      end
    end
  end)
  return ch
end

function pairing_processor(key, env)
  -- 只处理按下事件，忽略 release（key.release == true）
  if not key or key.release then return 2 end

  -- 取本次按键产出的字符：新版 librime-lua 给 key.text；旧版退回 key:repr()。
  local ch = nil
  pcall(function()
    if key.text and #key.text > 0 then ch = key.text end
  end)
  if not ch then
    local ok, repr = pcall(function() return key:repr() end)
    if ok then ch = repr end
  end
  if not ch or #ch == 0 then return 2 end

  local ctx = env.engine.context

  -- 行为一：敲左括号 -> 自动补右括号并成对上屏
  local closer = OPEN2CLOSE[ch]
  if closer then
    -- 仅在"无正在编码拼音"时自动补全；正在打字时把左括号交给 punctuator/方案本身。
    local input = ctx.input or ""
    if input == "" then
      pcall(function() ctx:commit_text(ch .. closer) end)
      return 1   -- kAccepted：吃掉本次按键，不再走后续流程
    end
    return 2
  end

  -- 行为二：敲右括号 -> 若光标前正是配对左括号，则跳过（吞键）
  local opener = CLOSE2OPEN[ch]
  if opener then
    local prev = last_char(env)
    if prev == opener then
      return 1   -- 吞掉：相当于光标右移一格穿过右括号
    end
    return 2
  end

  return 2  -- kNoop
end
