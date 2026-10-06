--[[
青简 P1-B7（W8.1）：有序列表自动续号  autolist.lua
==============================================================================
参考来源：思路与 librime-lua processor 用法参考 rime-ice（雾凇拼音）
         https://github.com/iDvel/rime-ice  （其 lua/ 目录中以 processor/filter 处理
         回车与上屏历史的写法；本脚本为青简自写实现，未整文件拷贝）。

实现要点：
  1. 角色 = lua_processor，注册：schema.yaml `engine/processors:` 追加
     `lua_processor@autolist`。
  2. 触发：按下回车（Return / 小键盘 Enter）。
     前提：当前没有正在编码的拼音串（context.input 为空），避免打断正常选词上屏。
  3. 判定：看最近一次上屏（commit_history.latest.text）的末行，若形如
        `1.` / `1. ` / `1. 买牛奶`  —— 即以 "数字+英文点" 开头的列表项，
     则自动 commit 一个换行 + `2. `（数字+1），实现「1. 回车 → 2. 回车 → 3. …」。
  4. 不匹配（最近不是编号列表项）就放行（kNoop），绝不影响正常回车行为。
  5. 只认英文句点 `.`；中文圈号 `1、` / `1）` 不在本批范围内（可在 §扩展 自行加模式）。

返回值约定（同 pairing.lua）：0=kRejected 1=kAccepted 2=kNoop。
==============================================================================
]]

local KEY_Return     = 0xFF0D   -- 65293 主键盘回车
local KEY_KP_Enter   = 0xFF8D   -- 65421 小键盘回车

-- 取最近一次上屏文本（做足容错：不同 librime-lua 版本 commit_history 形状略有差异）。
local function latest_commit_text(env)
  local ctx = env and env.engine and env.engine.context
  if not ctx then return nil end
  local text = nil
  pcall(function()
    local hist = ctx.commit_history
    if hist and hist.latest then
      local c = hist:latest()
      if c and c.text then text = c.text end
    end
  end)
  return text
end

function autolist_processor(key, env)
  if not key or key.release then return 2 end
  if key.keycode ~= KEY_Return and key.keycode ~= KEY_KP_Enter then
    return 2
  end

  local ctx = env.engine.context
  -- 正在编码时不接管回车（正常选词/确认）
  local input = ctx.input or ""
  if input ~= "" then return 2 end

  local last = latest_commit_text(env)
  if not last or last == "" then return 2 end

  -- 取末行（可能上屏串里含 \n）
  local line = last:match("[^\r\n]*$") or last

  -- 匹配 `数字.`（后可跟空格与正文），捕获数字
  local nstr = line:match("^%s*(%d+)%.%s")
  if not nstr then
    -- 兼容末行正好是 `数字.`（后面没内容）的情况
    nstr = line:match("^%s*(%d+)%.$")
  end
  if not nstr then return 2 end

  local n = tonumber(nstr)
  if not n or n < 0 or n > 99999 then return 2 end

  -- 自动续号：换行 + (n+1) + ". "
  local next_item = "\n" .. tostring(n + 1) .. ". "
  local ok = pcall(function() ctx:commit_text(next_item) end)
  if ok then
    return 1   -- kAccepted：吃掉本次回车，已代为上屏续号行
  end
  return 2
end
