<!-- <NT>overview:clang/lib/Lex/ -->

# Clang Lex + Preprocessor 库导读 — `clang/lib/Lex/`

> 本文档梳理 `clang/lib/Lex/` 目录下所有源文件 (27 个 .cpp/.h + 1 个
> `CMakeLists.txt` = 28 个) 的职责、上下游与推荐阅读顺序。
>
> 目标读者: 想理解 **Clang 词法分析 + 预处理器** (字符 → Token →
> macro/include/conditional 指令展开) 的开发者, 以及要给 Clang 加新
> 预处理指令 / 新 pragma handler / 新模块加载机制 / 新编码支持的人。
>
> 所有路径相对 `clang/lib/Lex/`。同名公开头文件位于
> `clang/include/clang/Lex/` (如 `Lexer.h`、`Preprocessor.h`、
> `HeaderSearch.h`、`ModuleMap.h`、`PPCallbacks.h`、`MacroInfo.h` 等)。
> 本目录编译为 `clangLex` 库, 链接 `clangBasic` + LLVM Support。

---

## §0. Clang Lex/PP 库在编译流水线中的位置

`clang/lib/Lex` 提供 Clang **字符流 → Token 流** 的全部逻辑, 并在
Lexer 之上实现 **C/C++ 预处理器**: `#include` / `#define` / `#if` /
`#line` / `#embed` / `#pragma` / `_Pragma` / `__VA_OPT__` / 模块
`@import` 等等。它是 Parser 的输入供应者, 也是 Frontend
(`CompilerInstance::createPreprocessor`) 的核心装配对象。

它在 Clang 内部的层次:

```
┌─────────────────────────────────────────────────────────┐
│ 源文件: foo.c / foo.cpp / foo.m / foo.h / foo.modulemap│
└─────────────────────────────────────────────────────────┘
                │
                ▼
┌─────────────────────────────────────────────────────────┐
│ clang/lib/Lex  (本目录) ← 你在这里                       │
│   · Lexer: 字节 → Token (identifier / literal /         │
│     punctuator / keyword / comment / eof)               │
│   · LiteralSupport: 数字 / 字符 / 字符串字面量解析      │
│   · Preprocessor:                                         │
│     · Token 流调度 (Lex dispatch)                       │
│     · 指令处理 (#include / #define / #if / #else /      │
│       #elif / #endif / #line / #embed / #pragma /       │
│       _Pragma)                                           │
│     · 宏展开 (object-like / function-like / variadic    │
│       + __VA_OPT__)                                      │
│     · 头文件解析 (-I / -isystem / -iquote / -iframework  │
│       / -include / header maps / sysroot)               │
│     · 模块 (@import / module.modulemap)                 │
│     · 条件区域记录 (#if/#endif region)                  │
│     · 预处理事件回调 (PPCallbacks)                       │
│     · 文本编码 (-finput-charset / -fexec-charset)       │
└─────────────────────────────────────────────────────────┘
                │
                ▼
┌─────────────────────────────────────────────────────────┐
│ 上游 (谁调 Lex/PP):                                      │
│   · clang/lib/Frontend/CompilerInstance.cpp             │
│     (createPreprocessor + InitializePreprocessor)        │
│   · clang/lib/Frontend/ASTUnit.cpp                      │
│   · clang/lib/Frontend/PrecompiledPreamble.cpp          │
│   · clang/lib/Frontend/HeaderIncludeGen.cpp /           │
│     DependencyFile.cpp / DependencyGraph.cpp            │
│   · clang/lib/Serialization/ASTReader.cpp /            │
│     ASTWriter.cpp (PCH/PCM)                             │
│   · clang/tools/clang-scan-deps.cpp (依赖扫描)         │
│   · clang/lib/Frontend/Rewrite/InclusionRewriter.cpp    │
└─────────────────────────────────────────────────────────┘
                │
                ▼
┌─────────────────────────────────────────────────────────┐
│ 下游消费者:                                              │
│   · clang/lib/Parse/Parser.cpp (ConsumeToken /          │
│     ConsumeAnyToken)                                    │
│   · clang/lib/AST/ASTContext.cpp (PreprocessingRecord)  │
│   · clang/lib/Sema/Sema.cpp (PPCallbacks /              │
│     ConditionalDirectiveRecord)                         │
│   · clang/lib/CodeGen/ (PreprocessingRecord for         │
│     line table emission)                                │
│   · clang/lib/Rewrite/ (TokenRewriter / Inclusion       │
│     Rewriter / FixItRewriter)                           │
└─────────────────────────────────────────────────────────┘
```

### §0.1 公开接口 (`clang/include/clang/Lex/`)

本目录 `.cpp` 文件依赖的 **公开头** 在
[`clang/include/clang/Lex/`](../../include/clang/Lex/), 最关键的:

- [`Token.h`](../../include/clang/Lex/Token.h) — `class Token` +
  `enum tok::TokenKind` 词法单元。
- [`Lexer.h`](../../include/clang/Lex/Lexer.h) — `class Lexer` 字
  符级 token 化。
- [`Preprocessor.h`](../../include/clang/Lex/Preprocessor.h) —
  `class Preprocessor` 入口。
- [`PreprocessorLexer.h`](../../include/clang/Lex/PreprocessorLexer.h)
  — `class PreprocessorLexer` 包含文件子 lexer。
- [`HeaderSearch.h`](../../include/clang/Lex/HeaderSearch.h) —
  `class HeaderSearch` + `HeaderFileInfo` 包含路径解析。
- [`HeaderSearchOptions.h`](../../include/clang/Lex/HeaderSearchOptions.h)
  — `-I` / `-isystem` 等选项。
- [`ModuleMap.h`](../../include/clang/Lex/ModuleMap.h) + [`ModuleMapFile.h`](../../include/clang/Lex/ModuleMapFile.h)
  — 模块映射。
- [`ModuleLoader.h`](../../include/clang/Lex/ModuleLoader.h) —
  `class ModuleLoader` `@import` 回调。
- [`MacroInfo.h`](../../include/clang/Lex/MacroInfo.h) +
  [`MacroBase.h`](../../include/clang/Lex/MacroBase.h) +
  [`MacroArgs.h`](../../include/clang/Lex/MacroArgs.h) — 宏定义
  / 参数缓存。
- [`TokenLexer.h`](../../include/clang/Lex/TokenLexer.h) — 宏体
  re-lex。
- [`TokenConcatenation.h`](../../include/clang/Lex/TokenConcatenation.h)
  — `##` 防意外粘接表。
- [`VariadicMacroSupport.h`](../../include/clang/Lex/VariadicMacroSupport.h)
  — `__VA_OPT__` 范围保护。
- [`PPCallbacks.h`](../../include/clang/Lex/PPCallbacks.h) —
  预处理器观察者。
- [`PPConditionalDirectiveRecord.h`](../../include/clang/Lex/PPConditionalDirectiveRecord.h)
  — `#if` 区域记录。
- [`PreprocessingRecord.h`](../../include/clang/Lex/PreprocessingRecord.h)
  — 完整预处理事件流 (AST 用)。
- [`Pragma.h`](../../include/clang/Lex/Pragma.h) —
  `PragmaHandler` / `PragmaNamespace` 调度。
- [`PPDirectiveParameter.h`](../../include/clang/Lex/PPDirectiveParameter.h)
  + [`PPEmbedParameters.h`](../../include/clang/Lex/PPEmbedParameters.h)
  — `#pragma` / `#embed` 参数解析。
- [`LiteralSupport.h`](../../include/clang/Lex/LiteralSupport.h)
  — `NumericLiteralParser` / `CharLiteralParser` /
  `StringLiteralParser`。
- [`HeaderMap.h`](../../include/clang/Lex/HeaderMap.h) +
  [`HeaderMapTypes.h`](../../include/clang/Lex/HeaderMapTypes.h)
  — Apple-style `.hmap` 重映射。
- [`DirectoryLookup.h`](../../include/clang/Lex/DirectoryLookup.h)
  — 单条搜索路径条目。
- [`MultipleIncludeOpt.h`](../../include/clang/Lex/MultipleIncludeOpt.h)
  — header guard 检测。
- [`NoTrivialPPDirectiveTracer.h`](../../include/clang/Lex/NoTrivialPPDirectiveTracer.h)
  — "无 PP 指令文件" 检测。
- [`LexDiagnostic.h`](../../include/clang/Lex/LexDiagnostic.h) —
  Lex/PP 诊断 ID 定义。
- [`PreprocessorOptions.h`](../../include/clang/Lex/PreprocessorOptions.h)
  — `-D` / `-U` / `-include` 选项。
- [`ScratchBuffer.h`](../../include/clang/Lex/ScratchBuffer.h) —
  合成 token 用的小 buffer 池。
- [`TextEncoding.h`](../../include/clang/Lex/TextEncoding.h) —
  `-finput-charset` / `-fexec-charset` 编码转换。
- [`ExternalPreprocessorSource.h`](../../include/clang/Lex/ExternalPreprocessorSource.h)
  — 从 PCH/AST 懒加载 MacroInfo 接口。
- [`CodeCompletionHandler.h`](../../include/clang/Lex/CodeCompletionHandler.h)
  — 代码补全回调钩子。
- [`HLSLRootSignatureTokenKinds.def`](../../include/clang/Lex/HLSLRootSignatureTokenKinds.def)
  — HLSL root signature token X-macro。
- [`LexHLSLRootSignature.h`](../../include/clang/Lex/LexHLSLRootSignature.h)
  — HLSL root signature 词法分析。

### §0.2 与 [`clang/lib/Frontend/`](../Frontend/) 的关系

Frontend 的 `CompilerInstance::createPreprocessor` 调用
`InitializePreprocessor` (注: `InitializePreprocessor` 实现在
[`Frontend/InitPreprocessor.cpp`](../Frontend/InitPreprocessor.cpp))
填默认 builtin 宏, 然后返回的 `Preprocessor` 实例供
[`Frontend/FrontendAction.cpp`](../Frontend/FrontendAction.cpp) 的
`BeginSourceFile` 通过 `EnterMainSourceFile` 进入主源文件; Frontend
的 `PrecompiledPreamble` / `HeaderIncludeGen` / `DependencyFile` /
`DependencyGraph` 等也都挂 `PPCallbacks` 到本目录的 Preprocessor 上。

### §0.3 与 [`clang/lib/Parse/`](../Parse/) 的关系

[`Parser::ConsumeToken`](../Parse/Parser.cpp) +
`Parser::ConsumeAnyToken` 直接从 `Preprocessor::Lex` 取 token;
`Parser` 也通过 `Preprocessor::EnterSourceFile` /
`Preprocessor::EndSourceFile` 管理被 `#include` 的子文件
`SourceLocation`。`Parser` 还通过 `Preprocessor::EnableBacktrackAtThisPos`
/ `Backtrack` 做 token-level 回溯 (`-Wambiguous-reversed-operator` 之类
警告需要)。

### §0.4 与 [`clang/lib/AST/`](../AST/) / [`clang/lib/Sema/`](../Sema/) / [`clang/lib/Serialization/`](../Serialization/) 的关系

- `PreprocessingRecord` 由 AST 端的 `ASTContext::getPreprocessingRecord()`
  持有, 预处理器每发生 `#include` / `#define` / 宏展开就 push 一
  个 `PreprocessedEntity` (`InclusionDirective` /
  `MacroDefinition` / `MacroExpansion`)。AST Dumper / CrossTU /
  PCH 都需要它。
- `PPCallbacks` 被 Sema 装为 `PPConditionalDirectiveRecord` + 各
  Sema 自己的 hook; 也被 `Serialization/ASTWriter.cpp` /
  `ASTReader.cpp` 装为 PCH/PCM 序列化 hook。
- `ModuleMap` 与 `AST/Module.h` 中的 `Module` 互相依赖。

### §0.5 与 [`clang/lib/Basic/`](../Basic/) 的关系

`SourceManager` / `FileManager` / `LangOptions` / `TargetInfo` /
`IdentifierTable` / `DiagnosticsEngine` / `CharInfo` /
`SanitizerOptions` 等都来自 [`clang/lib/Basic/`](../Basic/)。
`Preprocessor` 持有 `SourceManager` / `IdentifierTable` /
`DiagnosticsEngine` 引用。`Lexer` 通过 `SourceLocation` 间接引用
`SourceManager`。`LangOptions` 决定关键字表 / 字符宽度 /
C++ 版本 (从而决定 `__VA_OPT__` 等是否可用)。

---

## §1. 编译流水线概览

```
                       源文件 foo.cpp / foo.h
                                  │
                                  ▼
┌─────────────────────────────────────────────────────────┐
│ CompilerInstance::createPreprocessor (Frontend)          │
│   · new Preprocessor (SourceManager / IdentifierTable / │
│     DiagnosticsEngine 持有)                            │
│   · HeaderSearch::Configure 各 DirectoryLookup          │
│   · InitializePreprocessor (Frontend) 填默认 builtin   │
│   · 注册全部 PragmaHandler (Frontend)                   │
└─────────────────────────────────────────────────────────┘
                │
                ▼
┌─────────────────────────────────────────────────────────┐
│ Preprocessor::EnterMainSourceFile (Preprocessor.cpp)     │
│   ├─ 找到 / 创建 main file 的 PreprocessorLexer         │
│   ├─ 进入 Lexer 栈                                      │
│   └─ 准备第一个 token                                    │
└─────────────────────────────────────────────────────────┘
                │
                ▼
                ┌──────────────────────────────────┐
                │ 循环: Preprocessor::Lex          │
                │ (Preprocessor.cpp + PPCaching)   │
                └──────────────────────────────────┘
                          │
                          ▼
┌─────────────────────────────────────────────────────────┐
│ Lexer::LexTokenInternal (Lexer.cpp)                      │
│   · 跳过空白 / 注释 / line continuation                 │
│   · LexIdentifierContinue / LexNumericConstant /       │
│     LexStringLiteral / LexCharConstant /                │
│     getCharAndSizeSlow                                   │
│   · 调 LiteralSupport (LiteralSupport.cpp) 解析字面量   │
│   · 调 TextEncoding (TextEncoding.cpp) 转码字面量        │
│   · 调 UnicodeCharSets 查 identifier 字符              │
│   · 返回一个 Token (Token.h)                            │
└─────────────────────────────────────────────────────────┘
                │
                ▼
┌─────────────────────────────────────────────────────────┐
│ Preprocessor::Lex (Preprocessor.cpp)                      │
│   · 若 identifier → HandleIdentifier                     │
│      ├─ 在 macro map 中找到 MacroInfo →                │
│      │   TokenLexer (TokenLexer.cpp) re-lex macro 体    │
│      │   · ExpandFunctionArguments (MacroArgs)          │
│      │   · pasteTokens (TokenConcatenation)             │
│      │   · __VA_OPT__ 范围 (VariadicMacroSupport)       │
│      │   · # stringify (#) → StringLiteralParser        │
│      ├─ builtin macro → ExpandBuiltinMacros             │
│      └─ 回调 PPCallbacks::MacroExpands                   │
│   · 若以 '#' 开头 → HandleDirective                     │
│      ├─ #include → HeaderSearch::LookupFile + EnterFile │
│      │    + 通知 PPCallbacks::InclusionDirective        │
│      ├─ #define / #undef → MacroInfo / ModuleMacro      │
│      ├─ #if / #ifdef / #ifndef / #else / #elif / #endif │
│      │    → PPExpressions 求值 → 切换 lexer              │
│      │    + PPConditionalDirectiveRecord                │
│      ├─ #line → 改 SourceManager 的 line table         │
│      ├─ #pragma → PragmaNamespace::HandlePragma         │
│      │    (Pragma.cpp) → 各 PragmaHandler              │
│      └─ #embed → PPEmbedParameters 解析                  │
│                 + 字节嵌入                              │
│   · 若 '# include' → PreprocessorLexer::LexIncludeFilename│
│   · 通知 PPCallbacks (PPCallbacks.cpp)                  │
│   · 若 PreprocessingRecord 启用 → 记 PreprocessedEntity │
└─────────────────────────────────────────────────────────┘
                │
                ▼
┌─────────────────────────────────────────────────────────┐
│ Parser::ConsumeToken (Parser.cpp) 消费 token            │
│   · 调 Sema 检查语义                                    │
│   · 触发 AST 构建                                       │
└─────────────────────────────────────────────────────────┘

旁路 (可挂入):
  · HeaderSearch (HeaderSearch.cpp) 解析 #include 时被调
  · ModuleMap (ModuleMap.cpp) 解析 module.modulemap / @import 时被调
  · InitHeaderSearch (InitHeaderSearch.cpp) 配置默认搜索路径
  · HeaderMap (HeaderMap.cpp) Apple .hmap 重映射
  · DependencyDirectivesScanner (DependencyDirectivesScanner.cpp)
    -M/-MD 快速扫描入口 (clang-scan-deps 也用)
```

---

## §2. 文件目录结构

```
clang/lib/Lex/  (28 文件, 0 子目录)
├── §3.1  Lexer (字符 → Token)                            (4 文件)
├── §3.2  Preprocessor 核心 (调度 / 状态 / 缓存 / 回调)   (6 文件)
├── §3.3  Preprocessor 指令处理 (#include/#define/#if)    (3 文件)
├── §3.4  Macro 展开管线                                  (4 文件)
├── §3.5  Header search / include path 解析              (3 文件)
├── §3.6  Module map 解析与加载                           (2 文件)
├── §3.7  Pragma 处理                                     (1 文件)
└── §3.8  Helper (编码 / scratch / 依赖扫描 / Unicode)   (4 文件 + CMakeLists)
```

### 2.1 文件数量统计

| 区域 | .cpp | .h | 合计 |
|------|------|-----|------|
| Lexer | 4 | 0 | 4 |
| Preprocessor 核心 | 6 | 0 | 6 |
| 指令处理 | 3 | 0 | 3 |
| Macro 展开 | 4 | 0 | 4 |
| Header search | 3 | 0 | 3 |
| Module map | 2 | 0 | 2 |
| Pragma | 1 | 0 | 1 |
| Helper | 3 | 1 | 4 |
| Build | 1 | 0 | 1 (`CMakeLists.txt`) |
| **总计** | **~26** | **~1** | **~27** + `CMakeLists.txt` |

---

## §3. 文件详解

### 3.1 Lexer (字符 → Token)

[`Lexer.cpp`](Lexer.cpp) — 实现 `Lexer`: 字符流 → Token 流。负责
关键字、identifier、数字/字符/字符串字面量、操作符、注释、line
continuation; 通过 `LangOptions` / `SourceManager` /
`IdentifierTable` 决定 keyword 表 / identifier 字符范围。
- 上游: [`Frontend/CompilerInstance.cpp`](../Frontend/CompilerInstance.cpp),
  [`Parser/Parser.cpp`](../Parse/Parser.cpp),
  [`Frontend/PrintPreprocessedOutput.cpp`](../Frontend/PrintPreprocessedOutput.cpp)。
- 下游: [`Basic/SourceManager.h`](../../include/clang/Basic/SourceManager.h),
  [`Basic/IdentifierTable.h`](../../include/clang/Basic/IdentifierTable.h),
  [`Lex/Token.h`](../../include/clang/Lex/Token.h),
  [`Lex/LiteralSupport.h`](../../include/clang/Lex/LiteralSupport.h)。
- 关键类/函数: `Lexer`, `Token`, `Lexer::Lex`,
  `Lexer::LexTokenInternal`, `Lexer::getCharAndSizeSlow`,
  `Lexer::LexIdentifierContinue`, `Lexer::LexNumericConstant`,
  `Lexer::LexStringLiteral`。

[`LiteralSupport.cpp`](LiteralSupport.cpp) — 数字 / 字符 / 字符串字
面量解析; 处理后缀 (`u8` / `u` / `U` / `L` / `R"(...)"` raw)、
用户自定义字面量、C++23 `uz` / `z` 后缀、转义、UTF-8 校验。
- 上游: [`Lexer.cpp`](Lexer.cpp),
  [`Parse/ParseExpr.cpp`](../Parse/ParseExpr.cpp),
  [`Sema/SemaExpr.cpp`](../Sema/SemaExpr.cpp)。
- 下游: [`Basic/TargetInfo.h`](../../include/clang/Basic/TargetInfo.h),
  [`Basic/LangOptions.h`](../../include/clang/Basic/LangOptions.h),
  [`llvm/ADT/APInt.h`](../../../llvm/include/llvm/ADT/APInt.h),
  [`llvm/Support/ConvertUTF.h`](../../../llvm/include/llvm/Support/ConvertUTF.h)。
- 关键类/函数: `NumericLiteralParser`, `CharLiteralParser`,
  `StringLiteralParser`, `NumericLiteralParser::GetIntegerValue`,
  `StringLiteralParser::CopyStringFragment`。

[`LexHLSLRootSignature.cpp`](LexHLSLRootSignature.cpp) — 独立
lexer: HLSL root-signature 字符串片段 (寄存器、逗号、数字字面
量)。HLSL shader 的 root signature 用此 lexer 解析。
- 上游: [`Sema/SemaHLSL.cpp`](../Sema/SemaHLSL.cpp),
  [`Parse/ParseHLSL.cpp`](../Parse/ParseHLSL.cpp)。
- 下游: [`Lex/HLSLRootSignatureTokenKinds.def`](../../include/clang/Lex/HLSLRootSignatureTokenKinds.def),
  [`llvm/ADT/StringRef.h`](../../../llvm/include/llvm/ADT/StringRef.h)。
- 关键类/函数: `RootSignatureLexer`, `RootSignatureToken`,
  `RootSignatureLexer::lexToken`。

[`PreprocessorLexer.cpp`](PreprocessorLexer.cpp) — `PreprocessorLexer`
(`Lexer` 子类): 在 `#include` 文件内的 lexer, 处理 `LexIncludeFilename`
(把 `#include "x.h"` 的字符串精确解析为文件名字符串, 不走常规
identifier 路径)。
- 上游: [`Preprocessor.cpp`](Preprocessor.cpp),
  [`PPDirectives.cpp`](PPDirectives.cpp)。
- 下游: [`Basic/SourceManager.h`](../../include/clang/Basic/SourceManager.h),
  [`Lex/Preprocessor.h`](../../include/clang/Lex/Preprocessor.h),
  [`Lex/Token.h`](../../include/clang/Lex/Token.h)。
- 关键类/函数: `PreprocessorLexer`,
  `PreprocessorLexer::LexIncludeFilename`,
  `PreprocessorLexer::getFileEntry`。

### 3.2 Preprocessor 核心 (调度 / 状态 / 缓存 / 回调)

[`Preprocessor.cpp`](Preprocessor.cpp) — `Preprocessor` 主类实现:
顶层 `Lex` 分发, identifier 处理, 源文件 enter / exit,
Builtin macros 识别, 头文件 lexer 栈切换。**整个 Lex/PP 的调度
中心**。
- 上游:
  [`Frontend/CompilerInstance.cpp`](../Frontend/CompilerInstance.cpp),
  [`Parser/Parser.cpp`](../Parse/Parser.cpp),
  [`Frontend/FrontendActions.cpp`](../Frontend/FrontendActions.cpp)。
- 下游: [`Basic/SourceManager.h`](../../include/clang/Basic/SourceManager.h),
  [`Basic/IdentifierTable.h`](../../include/clang/Basic/IdentifierTable.h),
  [`Lex/HeaderSearch.h`](../../include/clang/Lex/HeaderSearch.h),
  [`Lex/Lexer.h`](../../include/clang/Lex/Lexer.h),
  [`Lex/Pragma.h`](../../include/clang/Lex/Pragma.h)。
- 关键类/函数: `Preprocessor`, `Preprocessor::Lex`,
  `Preprocessor::HandleIdentifier`,
  `Preprocessor::EnterMainSourceFile`, `Preprocessor::Initialize`,
  `Preprocessor::recomputeCurLexerKind`。

[`PPCaching.cpp`](PPCaching.cpp) — Token 回溯缓存 (`LookAhead`):
`EnableBacktrackAtThisPos` / `Backtrack` /
`CommitBacktrackedTokens` / `PeekAhead`; 给 `#if` 求值和 Parser 的
歧义消解用。
- 上游: [`Preprocessor.cpp`](Preprocessor.cpp),
  [`Parser/Parser.cpp`](../Parse/Parser.cpp)。
- 下游: [`Lex/Preprocessor.h`](../../include/clang/Lex/Preprocessor.h),
  [`Lex/Token.h`](../../include/clang/Lex/Token.h)。
- 关键类/函数: `Preprocessor::EnableBacktrackAtThisPos`,
  `Preprocessor::CachingLex`, `Preprocessor::Backtrack`,
  `Preprocessor::CommitBacktrackedTokens`,
  `Preprocessor::PeekAhead`。

[`PPLexerChange.cpp`](PPLexerChange.cpp) — Lexer 栈管理: 处理
`#include` 时进入新 `PreprocessorLexer`, `#endif` / 退出宏时返回
外层; 维护 `IncludeStackInfo`。
- 上游: [`Preprocessor.cpp`](Preprocessor.cpp),
  [`PPDirectives.cpp`](PPDirectives.cpp)。
- 下游: [`Basic/SourceManager.h`](../../include/clang/Basic/SourceManager.h),
  [`Basic/FileManager.h`](../../include/clang/Basic/FileManager.h),
  [`Lex/HeaderSearch.h`](../../include/clang/Lex/HeaderSearch.h)。
- 关键类/函数: `Preprocessor::EnterSourceFile`,
  `Preprocessor::getCurrentFileLexer`,
  `Preprocessor::isInPrimaryFile`, `IncludeStackInfo`。

[`PPCallbacks.cpp`](PPCallbacks.cpp) — `PPCallbacks` /
`PPChainedCallbacks` 默认空实现; 派生类只需覆写关心的钩子,
`PPChainedCallbacks` 把多个消费者串起来。
- 上游: [`PPDirectives.cpp`](PPDirectives.cpp),
  [`PPMacroExpansion.cpp`](PPMacroExpansion.cpp),
  [`AST/RawCommentList.cpp`](../AST/RawCommentList.cpp),
  [`AST/DeclBase.cpp`](../AST/DeclBase.cpp)。
- 下游: [`Lex/PPCallbacks.h`](../../include/clang/Lex/PPCallbacks.h)。
- 关键类/函数: `PPCallbacks`, `PPChainedCallbacks`,
  `PPCallbacks::~PPCallbacks`。

[`PPConditionalDirectiveRecord.cpp`](PPConditionalDirectiveRecord.cpp)
— 跟踪 `#if` / `#ifdef` / `#else` / `#elif` / `#endif` 区域; 给
Sema 的 `-Wunused-macros` / `#pragma poison` / AST 范围查询用。
- 上游: [`Preprocessor.cpp`](Preprocessor.cpp),
  [`Sema/Sema.cpp`](../Sema/Sema.cpp)。
- 下游: [`Basic/SourceManager.h`](../../include/clang/Basic/SourceManager.h),
  [`Lex/PPConditionalDirectiveRecord.h`](../../include/clang/Lex/PPConditionalDirectiveRecord.h)。
- 关键类/函数: `PPConditionalDirectiveRecord`, `CondDirectiveLoc`,
  `PPConditionalDirectiveRecord::If`,
  `PPConditionalDirectiveRecord::Endif`,
  `PPConditionalDirectiveRecord::rangeIntersectsConditionalDirective`。

[`PreprocessingRecord.cpp`](PreprocessingRecord.cpp) — 可选的完整
预处理事件记录器 (`InclusionDirective` / `MacroDefinition` /
`MacroExpansion`); 给 AST Dumper / CrossTU / PCH 还原预处理历史
用。
- 上游: [`PPDirectives.cpp`](PPDirectives.cpp),
  [`PPMacroExpansion.cpp`](PPMacroExpansion.cpp),
  [`AST/ASTContext.cpp`](../AST/ASTContext.cpp)。
- 下游: [`Basic/SourceManager.h`](../../include/clang/Basic/SourceManager.h),
  [`Lex/MacroInfo.h`](../../include/clang/Lex/MacroInfo.h),
  [`Lex/Token.h`](../../include/clang/Lex/Token.h)。
- 关键类/函数: `PreprocessingRecord`, `InclusionDirective`,
  `MacroExpansion`, `MacroDefinition`,
  `PreprocessingRecord::addPreprocessedEntity`,
  `PreprocessingRecord::MacroExpands`。

### 3.3 Preprocessor 指令处理

[`PPDirectives.cpp`](PPDirectives.cpp) — 顶层 `HandleDirective` 分
发 + `#include` / `#define` / `#if` / `#line` / `#embed` /
`#pragma` / `#endif` 处理器实现; **所有预处理指令的入口**。
- 上游: [`Preprocessor.cpp`](Preprocessor.cpp),
  [`Frontend/GeneratePCH.cpp`](../Frontend/GeneratePCH.cpp)。
- 下游: [`Basic/SourceManager.h`](../../include/clang/Basic/SourceManager.h),
  [`Lex/HeaderSearch.h`](../../include/clang/Lex/HeaderSearch.h),
  [`Lex/ModuleMap.h`](../../include/clang/Lex/ModuleMap.h),
  [`Lex/MacroInfo.h`](../../include/clang/Lex/MacroInfo.h),
  [`Lex/Pragma.h`](../../include/clang/Lex/Pragma.h)。
- 关键类/函数: `Preprocessor::HandleDirective`,
  `Preprocessor::HandleIncludeDirective`,
  `Preprocessor::HandleDefineDirective`,
  `Preprocessor::HandleIfdefDirective`,
  `Preprocessor::HandleIfDirective`,
  `Preprocessor::HandleEmbedDirective`,
  `Preprocessor::HandleEndifDirective`。

[`PPExpressions.cpp`](PPExpressions.cpp) — `#if` 整型常量表达式求
值; 处理 `defined()` / `__has_include(...)` /
`__has_embed(...)` / `__has_attribute(...)` 等。
- 上游: [`PPDirectives.cpp`](PPDirectives.cpp)。
- 下游: [`Basic/IdentifierTable.h`](../../include/clang/Basic/IdentifierTable.h),
  [`Lex/MacroInfo.h`](../../include/clang/Lex/MacroInfo.h),
  [`Lex/PPCallbacks.h`](../../include/clang/Lex/PPCallbacks.h),
  [`llvm/ADT/APSInt.h`](../../../llvm/include/llvm/ADT/APSInt.h)。
- 关键类/函数: `Preprocessor::EvaluateDirectiveExpression`,
  `DefinedTracker`, `PPValue`, `EvaluateDirectiveSubExpr`。

[`PPMacroExpansion.cpp`](PPMacroExpansion.cpp) — 顶层宏展开:
`ExpandBuiltinMacros` 处理 `__FILE__` / `__LINE__` / `__func__` 等
builtin 宏; `HandleMacroExpandedIdentifier` 处理普通 `#define` /
`#undef` 宏; 宏目录 (`#define X Y` 链式)。
- 上游: [`Preprocessor.cpp`](Preprocessor.cpp),
  [`Parser/Parser.cpp`](../Parse/Parser.cpp)。
- 下游: [`Basic/IdentifierTable.h`](../../include/clang/Basic/IdentifierTable.h),
  [`Lex/HeaderSearch.h`](../../include/clang/Lex/HeaderSearch.h),
  [`Lex/MacroInfo.h`](../../include/clang/Lex/MacroInfo.h),
  [`Lex/TokenLexer.h`](../../include/clang/Lex/TokenLexer.h)。
- 关键类/函数: `Preprocessor::ExpandBuiltinMacros`,
  `Preprocessor::HandleMacroExpandedIdentifier`,
  `Preprocessor::appendMacroDirective`,
  `Preprocessor::getMacroDefinition`。

### 3.4 Macro 展开管线

[`MacroInfo.cpp`](MacroInfo.cpp) — `MacroInfo` / `MacroDirective`
(`DefMacroDirective` / `UndefMacroDirective` /
`VisibilityMacroDirective`) / `ModuleMacro` 定义、比较、`isIdenticalTo`
用于 PCH 校验。
- 上游: [`PPMacroExpansion.cpp`](PPMacroExpansion.cpp),
  [`PPDirectives.cpp`](PPDirectives.cpp),
  [`Serialization/ASTReader.cpp`](../Serialization/ASTReader.cpp)。
- 下游: [`Basic/IdentifierTable.h`](../../include/clang/Basic/IdentifierTable.h),
  [`Basic/SourceManager.h`](../../include/clang/Basic/SourceManager.h),
  [`Lex/Token.h`](../../include/clang/Lex/Token.h)。
- 关键类/函数: `MacroInfo`, `MacroDirective`,
  `DefMacroDirective`, `UndefMacroDirective`,
  `VisibilityMacroDirective`, `ModuleMacro`,
  `MacroInfo::isIdenticalTo`, `MacroDirective::getDefinition`。

[`MacroArgs.cpp`](MacroArgs.cpp) — 函数式宏实参 token 列表的收集
/ 缓存; `getUnexpArgument` / `getPreExpArgument` 两个版本; 经
`MacroArgCache` 复用避免重复解析。
- 上游: [`PPMacroExpansion.cpp`](PPMacroExpansion.cpp),
  [`TokenLexer.cpp`](TokenLexer.cpp)。
- 下游: [`Lex/MacroInfo.h`](../../include/clang/Lex/MacroInfo.h),
  [`Lex/LexDiagnostic.h`](../../include/clang/Lex/LexDiagnostic.h),
  [`llvm/Support/SaveAndRestore.h`](../../../llvm/include/llvm/Support/SaveAndRestore.h)。
- 关键类/函数: `MacroArgs`, `MacroArgs::create`,
  `MacroArgs::destroy`, `MacroArgs::getUnexpArgument`,
  `MacroArgs::getPreExpArgument`。

[`TokenLexer.cpp`](TokenLexer.cpp) — Macro body re-lexer: 替换形
参 → 实参, 处理 `#x` stringification, `a ## b` 粘接
(`TokenConcatenation` 验合法性), `__VA_OPT__(...)` 范围保护
(`VariadicMacroSupport`), `__VA_ARGS__` 展开。
- 上游: [`PPMacroExpansion.cpp`](PPMacroExpansion.cpp),
  [`Preprocessor.cpp`](Preprocessor.cpp)。
- 下游: [`Lex/Preprocessor.h`](../../include/clang/Lex/Preprocessor.h),
  [`Lex/MacroArgs.h`](../../include/clang/Lex/MacroArgs.h),
  [`Lex/MacroInfo.h`](../../include/clang/Lex/MacroInfo.h),
  [`Lex/VariadicMacroSupport.h`](../../include/clang/Lex/VariadicMacroSupport.h)。
- 关键类/函数: `TokenLexer`, `TokenLexer::Init`, `TokenLexer::Lex`,
  `TokenLexer::ExpandFunctionArguments`, `TokenLexer::pasteTokens`,
  `TokenLexer::stringifyVAOPTContents`。

[`TokenConcatenation.cpp`](TokenConcatenation.cpp) —
`TokenConcatenation::AvoidConcat` 表: 给 `##` 粘接做合法性检查,
避免粘出非法 / 非标识符 / 字符串 token; 也提供
`IsIdentifierStringPrefix` 判定 LHS/RHS。
- 上游: [`TokenLexer.cpp`](TokenLexer.cpp),
  [`PPMacroExpansion.cpp`](PPMacroExpansion.cpp)。
- 下游: [`Basic/CharInfo.h`](../../include/clang/Basic/CharInfo.h),
  [`Lex/Preprocessor.h`](../../include/clang/Lex/Preprocessor.h)。
- 关键类/函数: `TokenConcatenation`,
  `TokenConcatenation::AvoidConcat`,
  `TokenConcatenation::IsIdentifierStringPrefix`。

### 3.5 Header search / include path 解析

[`HeaderSearch.cpp`](HeaderSearch.cpp) — `HeaderSearch` 实现: 用
`DirectoryLookup` 列表 (`Normal` / `System` / `Framework` /
`HeaderMap` / `ModuleMap`) 解析 `#include` / `@import` 名, 跟踪
每文件的 `HeaderFileInfo` (是否已 include / 是否 module header / 是
否 textual)。
- 上游: [`PPDirectives.cpp`](PPDirectives.cpp),
  [`Frontend/CompilerInstance.cpp`](../Frontend/CompilerInstance.cpp),
  [`Frontend/InitPreprocessor.cpp`](../Frontend/InitPreprocessor.cpp)。
- 下游: [`Basic/FileManager.h`](../../include/clang/Basic/FileManager.h),
  [`Basic/SourceManager.h`](../../include/clang/Basic/SourceManager.h),
  [`Lex/ModuleMap.h`](../../include/clang/Lex/ModuleMap.h),
  [`Lex/HeaderMap.h`](../../include/clang/Lex/HeaderMap.h)。
- 关键类/函数: `HeaderSearch`, `HeaderFileInfo`,
  `HeaderSearch::LookupFile`,
  `HeaderSearch::ShouldEnterIncludeFile`,
  `HeaderSearch::findUsableModuleForHeader`,
  `HeaderSearch::MarkFileModuleHeader`。

[`InitHeaderSearch.cpp`](InitHeaderSearch.cpp) — `InitHeaderSearch`
实现: 按 target triple / `--sysroot` /
`HeaderSearchOptions` 填默认 C / C++ / ObjC / CUDA include 路径
(`/usr/include` / `/usr/include/c++/v1` / framework dir 等)。
- 上游: [`Frontend/InitPreprocessor.cpp`](../Frontend/InitPreprocessor.cpp),
  [`Frontend/CompilerInstance.cpp`](../Frontend/CompilerInstance.cpp)。
- 下游: [`Basic/FileManager.h`](../../include/clang/Basic/FileManager.h),
  [`Lex/HeaderSearch.h`](../../include/clang/Lex/HeaderSearch.h),
  [`Lex/HeaderSearchOptions.h`](../../include/clang/Lex/HeaderSearchOptions.h),
  [`llvm/TargetParser/Triple.h`](../../../llvm/include/llvm/TargetParser/Triple.h)。
- 关键类/函数: `InitHeaderSearch`,
  `InitHeaderSearch::AddDefaultCIncludePaths`,
  `InitHeaderSearch::AddDefaultCPlusPlusIncludePaths`,
  `InitHeaderSearch::AddDefaultIncludePaths`。

[`HeaderMap.cpp`](HeaderMap.cpp) — Apple-style `.hmap` 文件读写:
字符串 → 字符串的文件名重映射 (例如 `Cocoa/Cocoa.h` →
`/System/Library/Frameworks/Cocoa.framework/Headers/Cocoa.h`)。
- 上游: [`HeaderSearch.cpp`](HeaderSearch.cpp),
  [`InitHeaderSearch.cpp`](InitHeaderSearch.cpp)。
- 下游: [`Basic/FileManager.h`](../../include/clang/Basic/FileManager.h),
  [`Lex/HeaderMap.h`](../../include/clang/Lex/HeaderMap.h),
  [`Lex/HeaderMapTypes.h`](../../include/clang/Lex/HeaderMapTypes.h),
  [`llvm/Support/MemoryBuffer.h`](../../../llvm/include/llvm/Support/MemoryBuffer.h)。
- 关键类/函数: `HeaderMap`, `HeaderMap::Create`,
  `HeaderMap::LookupFile`, `HMapHeader`, `checkHeader`。

### 3.6 Module map 解析与加载

[`ModuleMap.cpp`](ModuleMap.cpp) — `ModuleMap` 实现: 维护 Module
层级, header → module 映射, umbrella / exports / textual / private
header 分类, `findModule` / `parseAndLoadModuleMapFile` /
`addHeader`, `Module::HeaderKind`。
- 上游: [`HeaderSearch.cpp`](HeaderSearch.cpp),
  [`PPDirectives.cpp`](PPDirectives.cpp),
  [`Frontend/CompilerInstance.cpp`](../Frontend/CompilerInstance.cpp)。
- 下游: [`Basic/Module.h`](../../include/clang/Basic/Module.h),
  [`Basic/SourceManager.h`](../../include/clang/Basic/SourceManager.h),
  [`Lex/HeaderSearch.h`](../../include/clang/Lex/HeaderSearch.h),
  [`Lex/ModuleMapFile.h`](../../include/clang/Lex/ModuleMapFile.h)。
- 关键类/函数: `ModuleMap`, `Module`,
  `ModuleMap::findModule`,
  `ModuleMap::parseAndLoadModuleMapFile`,
  `ModuleMap::addHeader`, `Module::HeaderKind`。

[`ModuleMapFile.cpp`](ModuleMapFile.cpp) — `modulemap::ModuleMapFile`
实现: 词法分析 + 解析 `module.modulemap` 语法
(`module Foo { header "x.h" requires cplusplus20 }`) 为
ModuleMapFile AST。
- 上游: [`ModuleMap.cpp`](ModuleMap.cpp),
  [`Frontend/FrontendActions.cpp`](../Frontend/FrontendActions.cpp)。
- 下游: [`Basic/Diagnostic.h`](../../include/clang/Basic/Diagnostic.h),
  [`Lex/Lexer.h`](../../include/clang/Lex/Lexer.h),
  [`Lex/ModuleMap.h`](../../include/clang/Lex/ModuleMap.h)。
- 关键类/函数: `modulemap::ModuleMapFile`,
  `modulemap::parseModuleMapFile`, `MMToken`, `ModuleDecl`。

### 3.7 Pragma 处理

[`Pragma.cpp`](Pragma.cpp) — `PragmaHandler` 注册表 +
`PragmaNamespace` (root + 命名子树) 查找 + `#pragma` /
`_Pragma` 解析器派发; 全部 `#pragma` 处理通过此注册中心分给各
PragmaHandler 子类。
- 上游: [`PPDirectives.cpp`](PPDirectives.cpp),
  [`Frontend/CompilerInstance.cpp`](../Frontend/CompilerInstance.cpp),
  所有 clang Frontend pragma 文件 (e.g.
  `Frontend/CompilerInvocation.cpp` 注册 builtin pragma)。
- 下游: [`Basic/Diagnostic.h`](../../include/clang/Basic/Diagnostic.h),
  [`Lex/Preprocessor.h`](../../include/clang/Lex/Preprocessor.h),
  [`Lex/Token.h`](../../include/clang/Lex/Token.h)。
- 关键类/函数: `PragmaHandler`, `PragmaNamespace`,
  `EmptyPragmaHandler`, `PragmaHandlerRegistry`,
  `PragmaNamespace::FindHandler`,
  `PragmaNamespace::HandlePragma`。

### 3.8 Helper (编码 / scratch / 依赖扫描 / Unicode)

[`TextEncoding.cpp`](TextEncoding.cpp) — `TextEncoding` 实现: 内
部 UTF-8 与 `-fexec-charset` 字面量编码之间的转换器; 给 `-E` 输
出 / 字符串字面量使用。
- 上游: [`Lexer.cpp`](Lexer.cpp),
  [`LiteralSupport.cpp`](LiteralSupport.cpp),
  [`Frontend/CompilerInstance.cpp`](../Frontend/CompilerInstance.cpp)。
- 下游: [`llvm/Support/TextEncoding.h`](../../../llvm/include/llvm/Support/TextEncoding.h),
  [`Basic/LangOptions.h`](../../include/clang/Basic/LangOptions.h)。
- 关键类/函数: `TextEncoding`, `TextEncoding::getConverter`,
  `TextEncoding::setConvertersFromOptions`, `ConversionAction`。

[`ScratchBuffer.cpp`](ScratchBuffer.cpp) — `ScratchBuffer` 实现:
有界 `MemoryBuffer` 池, 用于 materialize 合成 token
(builtin macro 展开结果、`_Pragma` 字符串等)。
- 上游: [`Preprocessor.cpp`](Preprocessor.cpp),
  [`PPMacroExpansion.cpp`](PPMacroExpansion.cpp)。
- 下游: [`Basic/SourceManager.h`](../../include/clang/Basic/SourceManager.h),
  [`llvm/Support/MemoryBuffer.h`](../../../llvm/include/llvm/Support/MemoryBuffer.h)。
- 关键类/函数: `ScratchBuffer`, `ScratchBuffer::getToken`,
  `ScratchBuffer::AllocScratchBuffer`。

[`DependencyDirectivesScanner.cpp`](DependencyDirectivesScanner.cpp)
— 高速原始扫描器: 不走完整 Preprocessor, 直接扫描源文件只收集
`#include` / `#define` / `#if` / `#endif` 等指令。
给 `-M` / `-MD` 依赖生成 / `clang-scan-deps` 工具 /
`PreprocessStash` 复用 (避免完整 lex + parse)。
- 上游: [`Frontend/DependencyFile.cpp`](../Frontend/DependencyFile.cpp),
  [`Frontend/CompilerInstance.cpp`](../Frontend/CompilerInstance.cpp),
  [`tools/clang-scan-deps`](../../tools/clang-scan-deps/)。
- 下游: [`Lex/Lexer.h`](../../include/clang/Lex/Lexer.h),
  [`Lex/Pragma.h`](../../include/clang/Lex/Pragma.h),
  [`Basic/CharInfo.h`](../../include/clang/Basic/CharInfo.h)。
- 关键类/函数: `Scanner`, `DirectiveKind`,
  `CXX20ModuleDirectiveKind`,
  `scanSourceForDependencyDirectives`,
  `minimizeSourceToDependencyDirectives`。

[`UnicodeCharSets.h`](UnicodeCharSets.h) — 内部头: Unicode
`XID_Start` / `XID_Continue` 字符范围表; 给 `Lexer.cpp` 查
identifier 字符用 (C++ 标准要求标识符字符符合
`XID_Start` / `XID_Continue`)。
- 上游: [`Lexer.cpp`](Lexer.cpp) (编译期包含)。
- 下游:
  [`llvm/Support/UnicodeCharRanges.h`](../../../llvm/include/llvm/Support/UnicodeCharRanges.h)。
- 关键内容: `XIDStartRanges`, `XIDContinueRanges`。

[`CMakeLists.txt`](CMakeLists.txt) — `clangLex` 库定义: 把 27 个
`.cpp` 加入 `clangLex` 目标, 链接 `clangBasic`。

---

## §4. 关键调用链

### 4.1 字符 → Token (Lexer 主链)

```
源文件字节流
  │
  ▼
Lexer::Lex (Lexer.cpp)
  └─ Lexer::LexTokenInternal
       ├─ 跳空白 + 跳注释 (Lexer.cpp)
       ├─ 分派:
       │    ├─ 标识符起始字符 → LexIdentifierContinue
       │    │    └─ 查 UnicodeCharSets.h XID_Continue
       │    │    └─ 查 IdentifierTable (Basic) → tok::identifier / tok::keyword
       │    ├─ 数字 → LexNumericConstant
       │    │    └─ NumericLiteralParser (LiteralSupport.cpp)
       │    │         └─ GetIntegerValue / GetFloatValue
       │    ├─ 'u8'/'u'/'U'/'L'/'"'/'R' → LexStringLiteral
       │    │    └─ StringLiteralParser (LiteralSupport.cpp)
       │    │         └─ TextEncoding 转码 (TextEncoding.cpp)
       │    ├─ "'" → LexCharConstant
       │    │    └─ CharLiteralParser (LiteralSupport.cpp)
       │    ├─ ':' / '?' / '*' 等 → 直接 punctuator
       │    └─ EOF → tok::eof
       ├─ 调 ScratchBuffer::getToken 把合成的字面量挂 SourceManager
       └─ 返回 Token { kind, location, identifierInfo, literalData }
            │
            ▼
Preprocessor::Lex (Preprocessor.cpp)
  ├─ 若 token 是 identifier:
  │    ├─ 查 MacroMap → 命中 MacroInfo
  │    │    └─ 进 TokenLexer (TokenLexer.cpp) re-lex macro body
  │    │         ├─ ExpandFunctionArguments (MacroArgs)
  │    │         │    └─ 形参 → 实参 token 替换
  │    │         ├─ 'x' → stringification (#x)
  │    │         ├─ a ## b → pasteTokens (TokenConcatenation)
  │    │         ├─ __VA_OPT__(...) → 范围保护 (VariadicMacroSupport)
  │    │         └─ 递归触发 Preprocessor::Lex 处理内部宏
  │    ├─ builtin macro → ExpandBuiltinMacros
  │    │    └─ __FILE__ / __LINE__ / __func__ / __DATE__ / __TIME__
  │    └─ 通知 PPCallbacks::MacroExpands
  ├─ 若 token 以 '#' 开头:
  │    └─ HandleDirective (PPDirectives.cpp)
  │         ├─ '#' + 'include' → HandleIncludeDirective
  │         │    └─ HeaderSearch::LookupFile (HeaderSearch.cpp)
  │         │         ├─ 遍历 DirectoryLookup (Normal / System / Framework / HeaderMap / ModuleMap)
  │         │         └─ 命中 → 调 EnterSourceFile (PPLexerChange.cpp)
  │         │              └─ push 新 PreprocessorLexer (PreprocessorLexer.cpp)
  │         ├─ '#' + 'define'/'undef' → HandleDefineDirective
  │         │    └─ 解析 macro body → MacroInfo (MacroInfo.cpp)
  │         ├─ '#' + 'if'/'ifdef'/'ifndef' → HandleIfDirective
  │         │    ├─ PPExpressions 求值 (PPExpressions.cpp)
  │         │    └─ PPConditionalDirectiveRecord (PPConditionalDirectiveRecord.cpp)
  │         ├─ '#' + 'embed' → HandleEmbedDirective
  │         │    └─ PPEmbedParameters 解析 + 字节嵌入
  │         ├─ '#' + 'line' → 改 SourceManager line table
  │         └─ '#' + 'pragma' → PragmaNamespace::HandlePragma (Pragma.cpp)
  │              └─ 分发给各 PragmaHandler
  └─ 通知 PPCallbacks (PPCallbacks.cpp)
       │
       ▼
Parser::ConsumeToken (Parser.cpp) 消费 token → Sema 检查 → AST 构建
```

### 4.2 #include 解析链

```
Preprocessor::HandleIncludeDirective (PPDirectives.cpp)
  │
  ▼
HeaderSearch::LookupFile (HeaderSearch.cpp)
  ├─ 区分 <> 与 ""
  ├─ 遍历 DirectoryLookup 列表:
  │    ├─ LookupType::Normal: 在目录里直接打开
  │    ├─ LookupType::System: 在系统目录里打开
  │    ├─ LookupType::Framework: 解析 Apple framework header
  │    ├─ LookupType::HeaderMap: HeaderMap::LookupFile (HeaderMap.cpp)
  │    └─ LookupType::ModuleMap: 找 module 对应的 umbrella header
  ├─ 若文件名已知为 module header:
  │    └─ HeaderSearch::findUsableModuleForHeader
  │         └─ ModuleMap::findModule (ModuleMap.cpp)
  └─ 返回 Optional<FileEntryRef>

命中文件:
  │
  ▼
Preprocessor::EnterSourceFile (PPLexerChange.cpp)
  ├─ SourceManager::createFileID + line table
  ├─ HeaderSearch::ShouldEnterIncludeFile
  │    └─ MultipleIncludeOpt::UpdateState (header guard 检测)
  │         ├─ 已 include 且 guard → 跳过 (返回 null)
  │         └─ 否则继续
  ├─ 构造 PreprocessorLexer (PreprocessorLexer.cpp)
  │    └─ LexIncludeFilename 在该 lexer 中精确取文件名
  ├─ push 进 Lexer 栈
  └─ 通知 PPCallbacks::InclusionDirective

Module 路径触发:
  └─ 若 #include / @import 命中模块
       └─ ModuleLoader::loadModule (ModuleLoader.h)
            └─ 调 CompilerInstance 实现 (Frontend/CompilerInstance.cpp)
                 └─ GenerateModuleAction 触发模块构建
```

### 4.3 宏展开链

```
Preprocessor::HandleMacroExpandedIdentifier (PPMacroExpansion.cpp)
  │
  ▼
MacroInfo lookup (Preprocessor.cpp + MacroInfo.cpp)
  ├─ 命中 → getMacroDefinition (PPMacroExpansion.cpp)
  └─ 通知 PPCallbacks::MacroExpands

Macro 展开:
  │
  ▼
TokenLexer::Init + Lex (TokenLexer.cpp)
  ├─ 把 replacement tokens 复制到 working buffer
  ├─ ExpandFunctionArguments (TokenLexer.cpp)
  │    └─ MacroArgs::getPreExpArgument / getUnexpArgument (MacroArgs.cpp)
  │         ├─ Stringify (#x) → StringLiteralParser::CopyStringFragment
  │         ├─ Paste (a ## b) → TokenConcatenation::AvoidConcat 检查
  │         │    ├─ 合规 → 合并 token
  │         │    └─ 违规 → 错误诊断
  │         └─ __VA_OPT__(...) → VariadicMacroSupport 范围保护
  │              └─ 若 __VA_ARGS__ 非空, 展开括号内 token
  │                   └─ 递归触发 Preprocessor::Lex
  ├─ 把展开后 token 流喂回 Lexer 栈顶
  └─ 通知 PPCallbacks::MacroExpands

PCH 校验链:
  └─ MacroInfo::isIdenticalTo (MacroInfo.cpp)
       └─ 两个 MacroInfo 的 body / parameter list / flags 完全相等
            (用于 ASTReader 加载 PCH 时校验宏一致性)
```

### 4.4 模块解析链

```
源码遇到 @import Foo; 或 #include "Foo.h"
  │
  ▼
HeaderSearch::LookupFile / findUsableModuleForHeader (HeaderSearch.cpp)
  │
  ▼
ModuleMap::findModule (ModuleMap.cpp)
  ├─ 查 in-memory 已加载 module map
  ├─ 若未命中:
  │    ├─ 在头文件所在目录 / 上溯目录找 module.modulemap
  │    │    └─ 调 ModuleMapFile::parseModuleMapFile (ModuleMapFile.cpp)
  │    │         └─ Lex → Parse → AST
  │    ├─ 加载 inferred module map (没显式 modulemap, 头自动归入)
  │    └─ 加载 builtin module map (Darwin 等)
  ├─ ModuleMap::addHeader (ModuleMap.cpp)
  │    ├─ 把每个 header 按 Normal/Private/Textual/Umbrella 分类
  │    └─ HeaderSearch::MarkFileModuleHeader
  └─ 返回 Module* 对象

模块构建:
  │
  ▼
ModuleLoader::loadModule (ModuleLoader.h)
  └─ 调 CompilerInstance::loadModule (Frontend)
       ├─ 复用 GenerateModuleAction 触发模块编译
       ├─ 输出 .pcm 文件
       └─ ASTReader 读 .pcm → 装入当前 ASTContext
```

### 4.5 条件编译与 #if 求值链

```
Preprocessor::HandleIfDirective (PPDirectives.cpp)
  │
  ▼
Preprocessor::EvaluateDirectiveExpression (PPExpressions.cpp)
  ├─ TokenStream → APSInt 求值
  ├─ 处理 defined(X) → DefinedTracker
  ├─ 处理 __has_include(X) / __has_embed(...)
  │    └─ 调 HeaderSearch::LookupFile (HeaderSearch.cpp)
  ├─ 处理 __has_attribute(...) → Basic/Attr.td 查表
  └─ 返回 PPValue (i64 / identifier / 未声明 flag)

判定结果:
  ├─ true → 进入 if 分支, 通知 PPCallbacks::If
  │    └─ 调 Preprocessor::recomputeCurLexerKind (PPLexerChange.cpp)
  │         └─ 跳过当前 if block 内 disabled 区段
  ├─ false → 进入 else 分支 (若有), 通知 PPCallbacks::Else
  └─ Endif → 通知 PPCallbacks::Endif

PPConditionalDirectiveRecord (PPConditionalDirectiveRecord.cpp):
  ├─ 记录每个 #if/#ifdef 的 SourceRange
  ├─ Sema 用它查 "该 SourceRange 是否在某 #if 内部"
  │    └─ -Wunused-macros: 宏定义在 false branch → 警告
  │    └─ #pragma poison 在 false branch → 警告
  └─ AST 保存 → PCH 还原
```

### 4.6 编码转换链

```
源文件按 -finput-charset 解码为内部 UTF-8:
  SourceManager / FileManager 读文件 → 内部 UTF-8 (Lex/TextEncoding.cpp 不参与)

字面量按 -fexec-charset 编码输出:
  │
  ▼
Lexer::LexStringLiteral / LexCharConstant (Lexer.cpp)
  └─ StringLiteralParser / CharLiteralParser (LiteralSupport.cpp)
       └─ 输入是 UTF-8 字节流
            └─ TextEncoding::getConverter (TextEncoding.cpp)
                 ├─ 转换 CharSet → CharSet (例如 UTF-8 → UTF-16)
                 └─ CopyStringFragment 写到 target buffer

UTF-8 校验:
  └─ llvm::ConvertUTF.h 的 ConvertUTF8toUTF16 / hasUTF8ByteOrderMark
       └─ 检测 invalid sequence → 诊断
```

---

## §5. 推荐阅读顺序

### 阶段 1: 基础类型与字符级 lexing (1 小时)
- [`Lexer.cpp`](Lexer.cpp) — 字节 → token 主类
- [`Token.h`](../../include/clang/Lex/Token.h) — Token 数据结构
- [`UnicodeCharSets.h`](UnicodeCharSets.h) — identifier 字符
- [`TextEncoding.cpp`](TextEncoding.cpp) — 编码转换

### 阶段 2: Lexer 辅助 (1 小时)
- [`LiteralSupport.cpp`](LiteralSupport.cpp) — 字面量解析
- [`ScratchBuffer.cpp`](ScratchBuffer.cpp) — 合成 token
- [`LexHLSLRootSignature.cpp`](LexHLSLRootSignature.cpp) — HLSL

### 阶段 3: Preprocessor 核心 (2 小时)
- [`Preprocessor.cpp`](Preprocessor.cpp) — 顶层 Lex 调度
- [`PreprocessorLexer.cpp`](PreprocessorLexer.cpp) — include 子 lexer
- [`PPLexerChange.cpp`](PPLexerChange.cpp) — Lexer 栈切换

### 阶段 4: 指令处理 (2 小时)
- [`PPDirectives.cpp`](PPDirectives.cpp) — 所有指令入口
- [`PPExpressions.cpp`](PPExpressions.cpp) — `#if` 求值
- [`PPCallbacks.cpp`](PPCallbacks.cpp) — 回调 hook
- [`PPConditionalDirectiveRecord.cpp`](PPConditionalDirectiveRecord.cpp)
  — `#if` 区域记录
- [`PreprocessingRecord.cpp`](PreprocessingRecord.cpp) — 预处理事件流

### 阶段 5: 宏展开管线 (2 小时)
- [`MacroInfo.cpp`](MacroInfo.cpp) — 宏定义 / 链式 / module 宏
- [`MacroArgs.cpp`](MacroArgs.cpp) — 实参缓存
- [`TokenConcatenation.cpp`](TokenConcatenation.cpp) — `##` 防意外粘
- [`TokenLexer.cpp`](TokenLexer.cpp) — macro body re-lex
- [`PPMacroExpansion.cpp`](PPMacroExpansion.cpp) — 顶层宏展开
- [`PPCaching.cpp`](PPCaching.cpp) — token 回溯

### 阶段 6: 头文件解析 (1.5 小时)
- [`HeaderSearch.cpp`](HeaderSearch.cpp) — `#include` 解析核心
- [`InitHeaderSearch.cpp`](InitHeaderSearch.cpp) — 默认搜索路径
- [`HeaderMap.cpp`](HeaderMap.cpp) — Apple `.hmap` 重映射

### 阶段 7: 模块 (1 小时)
- [`ModuleMapFile.cpp`](ModuleMapFile.cpp) — `modulemap` 语法解析
- [`ModuleMap.cpp`](ModuleMap.cpp) — Module 层级维护

### 阶段 8: Pragma / 依赖扫描 (按需)
[`Pragma.cpp`](Pragma.cpp) +
[`DependencyDirectivesScanner.cpp`](DependencyDirectivesScanner.cpp)
+ [`CMakeLists.txt`](CMakeLists.txt)

---

## §6. 自定义扩展指南

### 6.1 添加新 `#pragma X(...)`

1. 在
   [`clang/include/clang/Lex/Pragma.h`](../../include/clang/Lex/Pragma.h)
   派生 `class PragmaXHandler : public PragmaHandler`。
2. 实现 `HandlePragma(Preprocessor &, PragmaIntroducer, Token &)`。
3. 在 [`clang/lib/Frontend/CompilerInstance.cpp`](../Frontend/CompilerInstance.cpp)
   的 `createPreprocessor` 末尾注册:
   `PP.RegisterPragmaHandler("namespace", new PragmaXHandler());`。
4. 更新
   [`clang/include/clang/Options/Options.td`](../../include/clang/Options/Options.td)
   (driver 端)。
5. 测试: `clang/test/Preprocessor/pragma-x.c`。

### 6.2 添加新 builtin macro (`__FOO__`)

1. 在 [`PPMacroExpansion.cpp`](PPMacroExpansion.cpp) 的
   `ExpandBuiltinMacros` 函数中加识别
   (查找 `IdentifierInfo->getName()` == "__FOO__")。
2. 用 [`ScratchBuffer.cpp`](ScratchBuffer.cpp) 的
   `ScratchBuffer::getToken` materialize token, 通过
   `Preprocessor::CacheToken` 缓存。
3. 在 [`Frontend/InitPreprocessor.cpp`](../Frontend/InitPreprocessor.cpp)
   的 `InitializePreprocessor` 不需要加; builtin 宏是按需展开。
4. 测试: `clang/test/Preprocessor/builtin-foo.c`。

### 6.3 添加新编码支持

1. 扩展 [`llvm/include/llvm/Support/TextEncoding.h`](../../../llvm/include/llvm/Support/TextEncoding.h)
   (LLVM 端)。
2. 在 [`TextEncoding.cpp`](TextEncoding.cpp) 的
   `TextEncoding::getConverter` 加新 `ConversionAction` case。
3. 测试: `clang/test/Lexer/text-encoding-charset.c`。

### 6.4 添加新预处理指令

1. 在 [`PPDirectives.cpp`](PPDirectives.cpp) 的
   `HandleDirective` switch 加新 case。
2. 实现 `HandleXxxDirective(Preprocessor &)`: lex + parse
   参数 (经 `PPDirectiveParameter` 或 `PPEmbedParameters` 类
   似 helper), 触发效果。
3. 通知 `PPCallbacks::Xxx` (在
   [`PPCallbacks.h`](../../include/clang/Lex/PPCallbacks.h)
   加新 hook)。
4. 更新 [`Driver/Options.td`](../../include/clang/Driver/Options.td)
   (driver 端)。
5. 测试: `clang/test/Preprocessor/xxx-directive.c`。

### 6.5 自定义 PPCallbacks

1. 派生
   [`PPCallbacks`](../../include/clang/Lex/PPCallbacks.h)
   (或继承
   [`PPChainedCallbacks`](../../include/clang/Lex/PPCallbacks.h)),
   覆写关心的钩子
   (e.g. `InclusionDirective` / `MacroExpands` / `Defined` /
   `Ifdef` / `FileChanged` / `PragmaDirective`)。
2. 在 Frontend / AST / Sema / Serialization 调用方构造实例, 用
   `Preprocessor::addPPCallbacks(...)` 装入。
3. 测试: `clang/test/AST/pp-callbacks-*.cpp`。

### 6.6 自定义 Module map 扩展

1. 在 [`ModuleMap.cpp`](ModuleMap.cpp) 加新的 header kind 或
   `requires` 检查 (e.g. C++20 module 特定)。
2. 在 [`ModuleMapFile.cpp`](ModuleMapFile.cpp) 加新
   `modulemap::ModuleDecl` 子类 + 新 token 识别。
3. 测试: `clang/test/Modules/xxx-module.m`。

### 6.7 自定义 dependency 扫描

1. 派生自
   [`DependencyDirectivesScanner`](../../include/clang/Lex/DependencyDirectivesScanner.h)
   (它是个 free function + DirectiveKind enum)。
2. 用 `Lexer` 直接 raw 扫描, 调 `minimizeSourceToDependencyDirectives`
   把源文件最小化到只剩 `#include` / `#define` / `#if` 指令。
3. 测试: `clang/test/Frontend/dependency-scan-xx.c`。

### 6.8 调试 Lex/PP 行为

1. 用 `clang -E foo.c` 看预处理结果。
2. 用 `clang -E -dD foo.c` 保留 `#define` 宏。
3. 用 `clang -E -H foo.c` 看 include 树。
4. 用 `clang -M foo.c` 看 Make depfile。
5. 用 `clang -Xclang -print-stats foo.c` 看 Lex/PP 统计。
6. 用 `clang -Xclang -ast-dump foo.c` 配合
   `PreprocessingRecord` 看 include 树。
7. 临时在 [`Preprocessor.cpp`](Preprocessor.cpp) 的 `Lex` 开头
   加 `llvm::errs() << ...` trace token。

---

## §7. NT 注释索引

当前 `clang/lib/Lex/` 下尚无 `// <NT>` 注释。已建立目录索引,
姊妹 overview:

- [`clang/lib/AST/0-overview.md`](../AST/0-overview.md) — Clang AST
  层
- [`clang/lib/Basic/0-overview.md`](../Basic/0-overview.md) — Clang 基
  础设置 + 共享数据
- [`clang/lib/CodeGen/0-overview.md`](../CodeGen/0-overview.md) —
  Clang AST → LLVM IR
- [`clang/lib/Driver/0-overview.md`](../Driver/0-overview.md) — Clang
  编译驱动
- [`clang/lib/Frontend/0-overview.md`](../Frontend/0-overview.md) —
  Clang 前端桥接
- `clang/lib/Parse/` — Clang Parser (待写)
- [`clang/lib/Sema/0-overview.md`](../Sema/0-overview.md) — Clang 语义
  分析

按 "少而精" 原则, 加 NT 注释建议优先级:

1. [`Preprocessor.cpp`](Preprocessor.cpp) — 8-12 段 (整个 Lex/PP
   调度中心)
2. [`Lexer.cpp`](Lexer.cpp) — 6-10 段 (字符 → token 主循环)
3. [`PPDirectives.cpp`](PPDirectives.cpp) — 6-10 段 (指令入口)
4. [`PPMacroExpansion.cpp`](PPMacroExpansion.cpp) — 5-8 段 (宏展开)
5. [`HeaderSearch.cpp`](HeaderSearch.cpp) — 5-8 段 (`#include` 解析)
6. [`TokenLexer.cpp`](TokenLexer.cpp) — 4-6 段 (宏体 re-lex)
7. [`MacroInfo.cpp`](MacroInfo.cpp) — 3-5 段 (宏定义存储)
8. [`ModuleMap.cpp`](ModuleMap.cpp) — 3-5 段 (模块)
9. [`InitHeaderSearch.cpp`](InitHeaderSearch.cpp) — 3-5 段 (默认
   include 路径)
10. [`PPExpressions.cpp`](PPExpressions.cpp) — 3-5 段 (`#if` 求值)
11. [`PPConditionalDirectiveRecord.cpp`](PPConditionalDirectiveRecord.cpp)
    — 3-4 段 (`#if` 区域)
12. [`Pragma.cpp`](Pragma.cpp) — 3-5 段 (`#pragma` 分发)
13. [`PreprocessingRecord.cpp`](PreprocessingRecord.cpp) — 3-5 段
    (预处理事件流)
14. [`LiteralSupport.cpp`](LiteralSupport.cpp) — 3-5 段 (字面量)
15. [`MacroArgs.cpp`](MacroArgs.cpp) +
    [`TokenConcatenation.cpp`](TokenConcatenation.cpp) +
    [`VariadicMacroSupport.h`](../../include/clang/Lex/VariadicMacroSupport.h)
    — 各 2-3 段
16. [`HeaderMap.cpp`](HeaderMap.cpp) +
    [`ModuleMapFile.cpp`](ModuleMapFile.cpp) — 各 2-3 段
17. [`DependencyDirectivesScanner.cpp`](DependencyDirectivesScanner.cpp)
    — 2-3 段

---

**姊妹文档**: 本目录对应 LLVM 流水线中的 **Clang 词法 + 预处理层**,
是 Parser 的 token 供应者, 也是 PreprocessingRecord 的制造者。它与
[`clang/lib/Basic/`](../Basic/0-overview.md) 配合定义 `SourceManager`
+ `LangOptions`, 与 [`clang/lib/Frontend/`](../Frontend/0-overview.md)
配合装配 `CompilerInstance::createPreprocessor`, 与
[`clang/lib/Parse/`](../Parse/) 配合通过 `ConsumeToken` 供 token,
与 [`clang/lib/AST/`](../AST/0-overview.md) /
[`clang/lib/Sema/`](../Sema/0-overview.md) /
[`clang/lib/Serialization/`](../Serialization/) 配合通过
`PPCallbacks` + `PreprocessingRecord` + `ConditionalDirectiveRecord`
观察预处理事件。
