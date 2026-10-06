// Branding.h
// ~~~~~~~~~~
// P0 MVP 品牌化集中头文件：产品名 / 版本 / 版权 / 自动更新 appcast URL。
//
// 设计原则：
//   1. 本头文件只做“集中声明”，不重复定义 VERSION_MAJOR/MINOR/PATCH 等
//      数字分量——这些分量仍由构建脚本注入（build.bat / xbuild.bat →
//      render.js → weasel.props → ClCompile.PreprocessorDefinitions；
//      xmake 根脚本 add_defines），避免出现第二处版本数字来源。
//   2. 版本字符串直接复用 include/WeaselConstants.h 已有的 WEASEL_VERSION
//      （其定义为 VERSION_STR(VERSION_MAJOR.VERSION_MINOR.VERSION_PATCH)，
//      在 RimeWithWeasel.cpp 的 _Setup() 中已用于 distribution_version）。
//   3. 所有取值均与仓库现状核对一致（见文末“取值来源对照表”注释块）。
//      若将来 fork/换皮：只需改本头文件 + WeaselServer.rc 中的
//      FEEDURL(APPCAST) 资源 + output/install.nsi 的品牌文案。
//
// 取值来源对照表（基线 commit：build.bat 默认 VERSION 0.17.4）：
//   | 宏                                  | 取值                              | 仓库来源                                              |
//   |------------------------------------|----------------------------------|------------------------------------------------------|
//   | WEASEL_PRODUCT_NAME                | L"青筱"                           | 2026-10-06 更名「青简」→「青筱」；青简作为同系列姊妹名保留于文案（见「产品标识」注释） |
//   | WEASEL_PRODUCT_NAME_EN             | L"QingXiaoType"                   | 2026-10-06 更名主英文代号（原 Qingjian，正式对外署名）   |
//   | WEASEL_PRODUCT_ALIAS_EN            | L"BambooIM"                       | 2026-10-06 项目代号/品牌别名，与主代号择一使用            |
//   | WEASEL_PRODUCT_DISPLAY_NAME        | L"青筱输入法（QingXiaoType）"      | 2026-10-06 更名（原 L"青简输入法（Qingjian）"）          |
//   | WEASEL_PRODUCT_VERSION_STR         | "0.17.4"（=WEASEL_VERSION）      | build.bat L11-13 / xbuild.bat：MAJOR=0 MINOR=17 PATCH=4 |
//   | WEASEL_COPYRIGHT                   | "Copyleft RIME Developers"       | WeaselServer.rc VERSIONINFO(EN) LegalCopyright       |
//   | WEASEL_COMPANY_NAME                | "Shistshuang"                    | WeaselServer.rc VERSIONINFO(EN) CompanyName           |
//   | WEASEL_APPCAST_URL                 | http://rime.github.io/release/weasel/appcast.xml           | WeaselServer.rc FEEDURL(APPCAST) 资源（UTF-16LE 解码）+ update/appcast.xml <link> + update/bump-version.ps1:121 三处互证 |
//   | WEASEL_APPCAST_TESTING_URL         | http://rime.github.io/testing/weasel/appcast.xml           | WeaselServer.rc TESTINGFEEDURL(APPCAST) 资源 + update/testing-appcast.xml <link> 两处互证 |
//
// 部署提醒：P0 阶段 WEASEL_APPCAST_URL 与官方仓库 .rc 资源保持一致；
// 若改为自有分发通道，请同步替换 WeaselServer.rc 中 FEEDURL /
// MANUALUPDATEFEEDURL / TESTINGFEEDURL / TESTINGMANUALUPDATEFEEDURL 四项
// APPCAST 资源（运行时 check_update() 仍优先读资源段，见 WeaselServerApp.h）。

#pragma once

#include "WeaselConstants.h"  // 提供 WEASEL_VERSION（由注入的 VERSION_* 宏 stringize 而来）

// ---- 宽字符 token 拼接辅助（内部使用，不对外暴露） -------------------------
#define _BRAND_WIDE2(x) L##x
#define _BRAND_WIDE(x) _BRAND_WIDE2(x)

// ===========================================================================
// 产品标识
// ===========================================================================
// 产品正式定名：青筱（Qīngxiǎo），英文主代号 QingXiaoType。
// 【2026-10-06 更名】由「青简 / Qingjian」更名为「青筱 / QingXiaoType」：
//   - 青简（Qingjian）作为同系列姊妹名保留，仅用于文案系列表述
//     （如「与青简同属一脉」），不进入代码宏；
//   - QingXiaoType 为正式对外英文主代号（UI / 安装包 / 更新通道署名用）；
//   - BambooIM 为项目代号/意象别名（意象：细竹藏简），与主代号择一使用，
//     多见于内部工程命名与文档，不对外主展示。
// 品牌 Slogan（文案参考，不入代码宏）：
//   主 Slogan：青简载文，青筱生辞
//   副标语：细竹藏简，静字随心
// 说明：本头文件为品牌化集中点，产品展示名统一使用「青筱」；
// 技术基座为 rime/weasel（GPLv3），代码命名空间 weasel 予以保留，
// 以最小化与上游的派生差异（见设计文档「命名约定」）。

// 产品名（宽字符串）：供 Win32 UI、WinSparkle app_details、托盘关于弹窗使用。
#define WEASEL_PRODUCT_NAME L"青筱"

// 产品英文主代号（宽字符串）：正式对外署名用。
#define WEASEL_PRODUCT_NAME_EN L"QingXiaoType"

// 品牌别名/项目代号（宽字符串）：意象别名，与主代号 QingXiaoType 择一使用；
// 需要竹意象的内部工程命名或文案处使用，不作为对外主展示名。
#define WEASEL_PRODUCT_ALIAS_EN L"BambooIM"

// 面向中文用户的展示名（宽字符串）。
#define WEASEL_PRODUCT_DISPLAY_NAME L"青筱输入法（QingXiaoType）"

// 产品代号（窄字符串）：与 WeaselConstants.h 中 WEASEL_CODE_NAME 同义，
// 此处仅做品牌化别名，保持引用入口集中。
#define WEASEL_PRODUCT_CODE_NAME "QingXiaoType"

// ===========================================================================
// 版本
// ===========================================================================

// 版本字符串（窄字符串，UTF-8/ASCII）：
// 直接复用 WeaselConstants.h 的 WEASEL_VERSION，当前展开为 "0.17.4"。
// 构建脚本（build.bat / xbuild.bat）修改 VERSION_MAJOR/MINOR/PATCH 后自动跟随。
#define WEASEL_PRODUCT_VERSION_STR WEASEL_VERSION

// 版本字符串（宽字符串）：供 MessageBoxW 等宽字符 API 直接拼接。
// 展开后为 L"0.17.4"。
#define WEASEL_PRODUCT_VERSION_W _BRAND_WIDE(WEASEL_PRODUCT_VERSION_STR)

// ===========================================================================
// 版权与厂商
// ===========================================================================

#define WEASEL_COPYRIGHT "Copyleft RIME Developers"
#define WEASEL_COPYRIGHT_W L"Copyleft RIME Developers"

#define WEASEL_COMPANY_NAME "Shistshuang"

// ===========================================================================
// 自动更新（WinSparkle）appcast
// ===========================================================================
// 注意：win_sparkle_set_appcast_url() 接受 const char*（UTF-8），
// 因此 URL 一律用窄字符串。

// 正式通道 appcast URL（与 WeaselServer.rc 的 FEEDURL 资源同源；
// 注意 scheme 为 http，GitHub Pages 会 301 到 https，WinSparkle 可跟随）。
#define WEASEL_APPCAST_URL \
  "http://rime.github.io/release/weasel/appcast.xml"

// 测试通道 appcast URL（与 WeaselServer.rc 的 TESTINGFEEDURL 资源同源；
// 注意路径是 /testing/weasel/ 而非 /release/weasel/testing/）。
// 当注册表 HKCU\Software\Rime\Weasel\UpdateChannel == "testing" 时由
// WeaselServerApp::check_update() 选用（见 WeaselServerApp.h 现有逻辑）。
#define WEASEL_APPCAST_TESTING_URL \
  "http://rime.github.io/testing/weasel/appcast.xml"
