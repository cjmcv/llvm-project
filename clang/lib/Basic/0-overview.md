<!-- <NT>overview:clang/lib/Basic/ -->

# Clang Basic 库导读 — `clang/lib/Basic/`

> 本文档梳理 `clang/lib/Basic/` 目录下所有源文件 (48 个顶层 .cpp/.h
> + 54 个 Targets/ 子目录 .cpp/.h = 102 个) 的职责、上下游与推荐阅读
> 顺序。
>
> 目标读者: 想理解 **Clang 基础设置层** (目标架构 + 文件/源位置 +
> 诊断 + 内建 builtin 表 + 语言/编译选项) 的开发者, 以及要给 Clang
> 加新架构 / 新 builtin / 新诊断 / 新 sanitizer / 新 warning group 的
> 人。
>
> 所有路径相对 `clang/lib/Basic/`。同名公开头文件位于
> `clang/include/clang/Basic/` (如 `TargetInfo.h`、`SourceManager.h`、
> `Builtins.h` 等); TableGen 源文件全部在
> `clang/include/clang/Basic/*.td`, 由 `clang-tblgen` 生成 `.inc` 后
> 被本目录 `.cpp` 文件 `#include`。

---

## §0. Clang Basic 库在编译流水线中的位置

`clang/lib/Basic` 是 Clang 的 **基础设置 + 共享数据层**, 提供所有
Clang 子模块 (Lexer / Parser / AST / Sema / CodeGen / Frontend / Driver)
都要用的 **目标无关** 数据结构和 **目标相关** (per-arch) 实现。它不
属于任何单一流水线节点, 而是被所有节点依赖, 处于"最底层"。

它在 Clang 内部的层次:

```
┌────────────────────────────────────────────────────────┐
│ clang/lib/Driver (命令行解析, clang/lib/Driver/)       │
│   输出: Triple (arch+os+vendor+env), FeatureMap,        │
│         Sanitizer args, XRay args, ...                  │
└────────────────────────────────────────────────────────┘
                │
                ▼
┌────────────────────────────────────────────────────────┐
│ clang/lib/Basic  (本目录) ← 你在这里                    │
│   · TargetInfo + Targets/*.cpp: 每个 arch 的 TargetInfo│
│     (cpu/feature/ABI/define/max-atomic/address-space)   │
│   · FileManager / SourceManager / FileEntry: 文件源   │
│   · IdentifierTable / TokenKinds / CharInfo: 词法原料 │
│   · Diagnostic + DiagnosticIDs + Warnings: 诊断机制    │
│   · Builtins + BuiltinTraits: __builtin 表 + trait    │
│   · Module / ASTSourceDescriptor: 模块信息             │
│   · Attributes / ParsedAttrInfo: 属性表                │
│   · OpenMPKinds / OpenCLOptions / Cuda / OffloadArch /│
│     ObjCRuntime / DarwinSDKInfo: 语言/方言特定选项     │
│   · Sanitizers / SanitizerSpecialCaseList / XRayInstr /│
│     ProfileList / NoSanitizeList: 插桩机制             │
│   · LangOptions / LangStandards / CodeGenOptions /     │
│     DiagnosticOptions: 编译选项                        │
│   · Stack / StackExhaustionHandler / AtomicLineLogger / │
│     MakeSupport / Version / SourceMgrAdapter: 杂项     │
└────────────────────────────────────────────────────────┘
                │ (被所有其他 Clang lib 依赖)
                ▼
┌────────────────────────────────────────────────────────┐
│ clang/lib/Lex + clang/lib/Parse + clang/lib/AST +     │
│ clang/lib/Sema + clang/lib/CodeGen + clang/lib/Frontend│
│   用 Basic 提供的 TargetInfo / SourceManager /         │
│   DiagnosticsEngine / IdentifierTable / Builtin::Info │
└────────────────────────────────────────────────────────┘
```

### §0.1 公开接口 (`clang/include/clang/Basic/`)

本目录 `.cpp` 文件对应的 **公开头** 在
[`clang/include/clang/Basic/`](../../include/clang/Basic/), 最关键的:

- [`TargetInfo.h`](../../include/clang/Basic/TargetInfo.h) — `class
  TargetInfo` 基类接口。
- [`SourceManager.h`](../../include/clang/Basic/SourceManager.h) —
  `class SourceManager`, `class FileID`, `class ContentCache`。
- [`FileManager.h`](../../include/clang/Basic/FileManager.h) —
  `class FileManager`, `class FileEntry`, `class FileEntryRef`。
- [`SourceLocation.h`](../../include/clang/Basic/SourceLocation.h) —
  `class SourceLocation`, `class SourceRange`, `class FullSourceLoc`。
- [`Diagnostic.h`](../../include/clang/Basic/Diagnostic.h) +
  [`DiagnosticIDs.h`](../../include/clang/Basic/DiagnosticIDs.h) +
  [`DiagnosticOptions.h`](../../include/clang/Basic/DiagnosticOptions.h)
  — 诊断三层。
- [`Builtins.h`](../../include/clang/Basic/Builtins.h) +
  [`BuiltinTraits.h`](../../include/clang/Basic/BuiltinTraits.h) —
  builtin 信息。
- [`LangOptions.h`](../../include/clang/Basic/LangOptions.h) +
  [`LangStandard.h`](../../include/clang/Basic/LangStandard.h) +
  [`CodeGenOptions.h`](../../include/clang/Basic/CodeGenOptions.h) —
  编译选项。
- [`IdentifierTable.h`](../../include/clang/Basic/IdentifierTable.h) +
  [`TokenKinds.h`](../../include/clang/Basic/TokenKinds.h) +
  [`CharInfo.h`](../../include/clang/Basic/CharInfo.h) — 词法原料。
- [`Module.h`](../../include/clang/Basic/Module.h) +
  [`ASTSourceDescriptor.h`](../../include/clang/Basic/ASTSourceDescriptor.h)
  — 模块信息。
- [`Sanitizers.h`](../../include/clang/Basic/Sanitizers.h) +
  [`XRayInstr.h`](../../include/clang/Basic/XRayInstr.h) +
  [`OffloadArch.h`](../../include/clang/Basic/OffloadArch.h) +
  [`Cuda.h`](../../include/clang/Basic/Cuda.h) +
  [`ObjCRuntime.h`](../../include/clang/Basic/ObjCRuntime.h) +
  [`OpenMPKinds.h`](../../include/clang/Basic/OpenMPKinds.h) +
  [`OpenCLOptions.h`](../../include/clang/Basic/OpenCLOptions.h) +
  [`DarwinSDKInfo.h`](../../include/clang/Basic/DarwinSDKInfo.h) — 方言
  / 工具特定选项。

### §0.2 与 [`clang/lib/Frontend/`](../Frontend/) 的关系

[`Frontend/`](../Frontend/) 在初始化时
([`CompilerInstance.cpp`](../Frontend/CompilerInstance.cpp)) 构造:
1. `FileManager` / `SourceManager` (本目录
   [`FileManager.cpp`](FileManager.cpp) /
   [`SourceManager.cpp`](SourceManager.cpp))
2. `DiagnosticsEngine` + `DiagnosticIDs` +
   `DiagnosticOptions` (本目录
   [`Diagnostic.cpp`](Diagnostic.cpp) /
   [`DiagnosticIDs.cpp`](DiagnosticIDs.cpp))
3. `TargetInfo` (经 [`Targets.cpp`](Targets.cpp) →
   [`Targets/X86.cpp`](Targets/X86.cpp) 等具体 arch)
4. `LangOptions` / `CodeGenOptions` /
   `LangStandard` (本目录
   [`LangOptions.cpp`](LangOptions.cpp) /
   [`LangStandards.cpp`](LangStandards.cpp) /
   [`CodeGenOptions.cpp`](CodeGenOptions.cpp))

### §0.3 与 [`clang/lib/Driver/`](../Driver/) 的关系

[`Driver/`](../Driver/) 解析命令行 (如 `-target x86_64-linux-gnu`、
`-fsanitize=address`、`-mx86-feature` 等), 产生 `Triple`、
`SanitizerArgs` 等。然后把这些传给 Frontend/CompilerInstance,
Basic 层据此:
- 选 arch ([`Targets.cpp::AllocateTarget`](Targets.cpp) 唯一分发点)
- 选 sanitizers ([`Sanitizers.cpp::parseSanitizerValue`](Sanitizers.cpp))
- 选 offload arch ([`OffloadArch.cpp`](OffloadArch.cpp))
- 选 runtime ([`ObjCRuntime.cpp`](ObjCRuntime.cpp))

### §0.4 与 [`clang/lib/Lex/`](../Lex/) + [`clang/lib/Parse/`](../Parse/)
### 的关系

Lex/Parse 直接调用本目录的 `IdentifierTable` / `TokenKinds` /
`CharInfo` / `SourceManager` / `FileManager` / `Diagnostic` 等。

### §0.5 与 [`clang/lib/AST/`](../AST/) + [`clang/lib/Sema/`](../Sema/)
### + [`clang/lib/CodeGen/`](../CodeGen/) 的关系

AST/Sema/CodeGen **只读** `TargetInfo`、`Builtins`、`LangOptions`、
`DiagnosticsEngine` 等, 不修改。

---

## §1. 编译流水线概览

```
                            clang -X -Y ... → Driver 解析
                                          │
                                          ▼
┌─────────────────────────────────────────────────────────┐
│ Frontend/CompilerInstance 初始化 (clang/lib/Frontend)    │
│   1. 构造 FileManager (Basic/FileManager.cpp)            │
│   2. 构造 SourceManager (Basic/SourceManager.cpp)        │
│   3. 构造 DiagnosticsEngine                              │
│        (Basic/Diagnostic.cpp + DiagnosticIDs.cpp +       │
│         Warnings.cpp 解析 -W/-Werror 等)                │
│   4. 构造 TargetInfo                                      │
│        (Basic/TargetInfo.cpp + Targets.cpp →              │
│         Targets/X86.cpp 等具体 arch)                    │
│   5. 构造 LangOptions / CodeGenOptions / LangStandard    │
│        (Basic/LangOptions.cpp + CodeGenOptions.cpp +     │
│         LangStandards.cpp)                              │
└─────────────────────────────────────────────────────────┘
                │
                ▼ (全 Clang 编译期间使用)
┌─────────────────────────────────────────────────────────┐
│ clang/lib/Basic  (本目录) — 提供所有共享数据             │
│   · File entry / source location / identifier lookup     │
│   · 诊断 (Diag → DiagnosticsEngine → DiagnosticIDs)      │
│   · builtin 表 (Builtins.cpp + BuiltinTraits.cpp)        │
│   · per-arch feature/ABI/define (TargetInfo + Targets/*) │
│   · Sanitizer / XRay / ProfileList (instrumentation)     │
│   · OpenMP/OpenCL/ObjC/CUDA/Offload/DarwinSDK/Stack/     │
│     MakeSupport/Version/Sarif/Attributes 等             │
└─────────────────────────────────────────────────────────┘
                │
                ├─► clang/lib/Lex (用 IdentifierTable + SourceManager)
                ├─► clang/lib/Parse (用 TokenKinds + CharInfo)
                ├─► clang/lib/AST (用 LangOptions + TargetInfo + Builtins)
                ├─► clang/lib/Sema (用 Diagnostic + Builtins + LangOptions
                │                + OpenMPKinds + OpenCLOptions)
                ├─► clang/lib/CodeGen (用 TargetInfo + Builtins + LangOptions
                │                      + CodeGenOptions + Sanitizers/XRay/
                │                      ProfileList + NoSanitizeList)
                └─► clang/lib/Serialization (用 TargetInfo + Module + Diagnostic)
                │
                ▼
   AST → clang/lib/CodeGen → llvm/lib/CodeGen → LLVM IR
```

辅助入口:

- **公开 API**: [`clang/include/clang/Basic/`](../../include/clang/Basic/)
  (每个 `.cpp` 的同名 `.h`)。
- **TableGen 源**: [`clang/include/clang/Basic/*.td`](../../include/clang/Basic/)
  (`Builtins.td`、`Diagnostic.td`、`Attr.td`、`Sanitizers.def`、
  `arm_neon.td`、`riscv_vector.td` 等), `clang-tblgen` 生成 `.inc` 后
  被 `.cpp` 直接 `#include`。
- **单一分发点**: [`Targets.cpp::AllocateTarget`](Targets.cpp) 根据
  `llvm::Triple::ArchType` + OS 选具体 arch。

---

## §2. 文件目录结构

```
clang/lib/Basic/  (102 文件, 1 顶层子目录 Targets/)
├── §3.1  Source & file management                    (6 文件)
├── §3.2  Diagnostics & warnings                       (6 文件)
├── §3.3  Language & identifiers                       (6 文件)
├── §3.4  Module system                                (2 文件)
├── §3.5  Attribute system                             (3 文件)
├── §3.6  Target architecture core                    (6 文件)
├── §3.7  Builtins & CodeGen                            (3 文件)
├── §3.8  Sanitizers & instrumentation lists          (6 文件)
├── §3.9  Offload / GPU / CUDA                         (2 文件)
├── §3.10 OpenMP / OpenCL / ObjC                       (3 文件)
├── §3.11 OS / SDK / stack / misc                      (5 文件)
├── Targets/  (27 个 arch, 54 文件, 见 §3.12)
└── §3.13 Build / misc                                  (1 文件 CMakeLists.txt)
```

### 2.1 文件数量统计

| 区域 | .cpp | .h | 合计 |
|------|------|-----|------|
| Source & file management | 6 | 0 | 6 |
| Diagnostics & warnings | 6 | 0 | 6 |
| Language & identifiers | 6 | 0 | 6 |
| Module system | 2 | 0 | 2 |
| Attribute system | 2 | 1 | 3 |
| Target architecture core | 5 | 1 | 6 |
| Builtins & CodeGen | 3 | 0 | 3 |
| Sanitizers & instrumentation | 6 | 0 | 6 |
| Offload / GPU / CUDA | 2 | 0 | 2 |
| OpenMP / OpenCL / ObjC | 3 | 0 | 3 |
| OS / SDK / stack / misc | 5 | 0 | 5 |
| Targets/ (27 arch) | 27 | 27 | 54 |
| Build / misc | 1 (`CMakeLists.txt`) | 0 | 1 |
| **总计** | **~73** | **~29** | **~102** + `CMakeLists.txt` |

---

## §3. 文件详解

### 3.1 Source & file management

[`FileEntry.cpp`](FileEntry.cpp) — 实现 `FileEntry` / `FileEntryRef`
句柄, VFS 缓存中的文件条目引用。
- 上游: [`CompilerInstance.cpp`](../Frontend/CompilerInstance.cpp),
  [`ASTWriter.cpp`](../Serialization/ASTWriter.cpp)。
- 下游: [`FileEntry.h`](../../include/clang/Basic/FileEntry.h),
  [`llvm/Support/VirtualFileSystem.h`](../../../llvm/include/llvm/Support/VirtualFileSystem.h)。
- 关键类/函数: `class FileEntry`, `class FileEntryRef`,
  `FileEntry::closeFile`。

[`FileManager.cpp`](FileManager.cpp) — 实现 `FileManager`, 缓存
目录/文件查询并通过 vfs 接口访问文件系统。
- 上游: [`CompilerInstance.cpp`](../Frontend/CompilerInstance.cpp),
  [`HeaderSearch.cpp`](../Lex/HeaderSearch.cpp),
  [`SourceManager.cpp`](SourceManager.cpp)。
- 下游: [`FileManager.h`](../../include/clang/Basic/FileManager.h),
  [`llvm/Support/VirtualFileSystem.h`](../../../llvm/include/llvm/Support/VirtualFileSystem.h),
  [`llvm/Support/Path.h`](../../../llvm/include/llvm/Support/Path.h)。
- 关键类/函数: `class FileManager`, `class FileSystemOptions`,
  `getFile`, `getDirectory`, `normalizeCacheKey`。

[`SourceManager.cpp`](SourceManager.cpp) — 实现 `SourceManager`,
`FileID` 映射、`ContentCache`、宏展开与 line-table 跟踪。
- 上游: [`Preprocessor.cpp`](../Lex/Preprocessor.cpp),
  [`Parser.cpp`](../Parse/Parser.cpp),
  [`ASTContext.cpp`](../AST/ASTContext.cpp)。
- 下游: [`SourceManager.h`](../../include/clang/Basic/SourceManager.h),
  [`SourceManagerInternals.h`](../../include/clang/Basic/SourceManagerInternals.h),
  [`FileManager.cpp`](FileManager.cpp)。
- 关键类/函数: `class SourceManager`, `class FileID`,
  `class ContentCache`, `getExpansionLoc`, `getSpellingLoc`。

[`SourceLocation.cpp`](SourceLocation.cpp) — `FullSourceLoc` 访问器,
`SourceLocation` 哈希/打印, 与 pretty-stack-trace 输出。
- 上游: [`ASTContext.cpp`](../AST/ASTContext.cpp),
  [`ASTReader.cpp`](../Serialization/ASTReader.cpp)。
- 下游: [`SourceLocation.h`](../../include/clang/Basic/SourceLocation.h),
  [`SourceManager.cpp`](SourceManager.cpp),
  [`llvm/Support/raw_ostream.h`](../../../llvm/include/llvm/Support/raw_ostream.h)。
- 关键类/函数: `class SourceLocation`, `class SourceRange`,
  `class FullSourceLoc`, `class PrettyStackTraceLoc`。

[`SourceMgrAdapter.cpp`](SourceMgrAdapter.cpp) — 把
`llvm::SourceMgr` / `SMDiagnostic` 适配到 Clang 的 `SourceManager` +
`DiagnosticsEngine`。
- 上游: [`CodeGenAction.cpp`](../CodeGen/CodeGenAction.cpp),
  [`CompilerInvocation.cpp`](../Frontend/CompilerInvocation.cpp)。
- 下游: [`SourceMgrAdapter.h`](../../include/clang/Basic/SourceMgrAdapter.h),
  [`Diagnostic.cpp`](Diagnostic.cpp)。
- 关键类/函数: `class SourceMgrAdapter`, `mapLocation`, `mapRange`,
  `handleDiag`。

[`AtomicLineLogger.cpp`](AtomicLineLogger.cpp) — 依赖扫描阶段用的
线程安全原子行日志 (依赖扫描构建 `.d` 文件时记录)。
- 上游: [`CompilerInstance.cpp`](../Frontend/CompilerInstance.cpp),
  [`clang/lib/DependencyScan/`](../DependencyScan/)。
- 下游: [`AtomicLineLogger.h`](../../include/clang/Basic/AtomicLineLogger.h),
  [`llvm/Support/FileSystem.h`](../../../llvm/include/llvm/Support/FileSystem.h)。
- 关键类/函数: `class AtomicLineLogger`, `class LogLine`,
  `initialize`, `log`。

### 3.2 Diagnostics & warnings

[`Diagnostic.cpp`](Diagnostic.cpp) — 实现 `DiagnosticsEngine`, 诊断
发出/格式化/stack map (含 source/range/fix-it)。
- 上游: [`Preprocessor.cpp`](../Lex/Preprocessor.cpp),
  [`Parser.cpp`](../Parse/Parser.cpp),
  [`Sema.cpp`](../Sema/Sema.cpp)。
- 下游: [`Diagnostic.h`](../../include/clang/Basic/Diagnostic.h),
  [`DiagnosticIDs.cpp`](DiagnosticIDs.cpp),
  [`SourceManager.cpp`](SourceManager.cpp)。
- 关键类/函数: `class DiagnosticsEngine`,
  `class DiagnosticBuilder`, `class DiagStateMap`, `Report`,
  `setSeverityForKind`。

[`DiagnosticIDs.cpp`](DiagnosticIDs.cpp) — `DiagnosticIDs` 查表
(内置 diag 字符串/类别/stable-id), `-W`/`-Werror` 选项匹配。
- 上游: [`Diagnostic.cpp`](Diagnostic.cpp),
  [`Warnings.cpp`](Warnings.cpp),
  [`TextDiagnosticPrinter.cpp`](../Frontend/TextDiagnosticPrinter.cpp)。
- 下游: [`DiagnosticIDs.h`](../../include/clang/Basic/DiagnosticIDs.h),
  [`AllDiagnostics.h`](../../include/clang/Basic/AllDiagnostics.h),
  [`SourceManager.cpp`](SourceManager.cpp)。
- 关键类/函数: `class DiagnosticIDs`, `getNearestOption`,
  `isWarningOrExtension`, `class StaticDiagInfo`。

[`DiagnosticOptions.cpp`](DiagnosticOptions.cpp) —
`DiagnosticLevelMask` 流输出 (`raw_ostream` 重载)。
- 上游: [`CompilerInvocation.cpp`](../Frontend/CompilerInvocation.cpp),
  [`Diagnostic.cpp`](Diagnostic.cpp)。
- 下游: [`DiagnosticOptions.h`](../../include/clang/Basic/DiagnosticOptions.h),
  [`llvm/Support/raw_ostream.h`](../../../llvm/include/llvm/Support/raw_ostream.h)。
- 关键类/函数: `class DiagnosticLevelMask`, `operator<<`。

[`Warnings.cpp`](Warnings.cpp) — `ProcessWarningOptions`: 解析
`-W`/`-Wno-`/`-Werror` 等命令行, 设置 `DiagnosticsEngine`。
- 上游: [`CompilerInvocation.cpp`](../Frontend/CompilerInvocation.cpp),
  [`CompilerInstance.cpp`](../Frontend/CompilerInstance.cpp)。
- 下游: [`AllDiagnostics.h`](../../include/clang/Basic/AllDiagnostics.h),
  [`Diagnostic.cpp`](Diagnostic.cpp),
  [`DiagnosticIDs.cpp`](DiagnosticIDs.cpp)。
- 关键类/函数: `ProcessWarningOptions`,
  `EmitUnknownDiagWarning`, `diag::Flavor`。

[`CLWarnings.cpp`](CLWarnings.cpp) — MSVC `/W4` 警告 ID
(4005/4018/4100/4910/4996) → Clang `diag::Group` 映射。
- 上游: [`MSVC.cpp`](../Driver/ToolChains/MSVC.cpp)。
- 下游: [`CLWarnings.h`](../../include/clang/Basic/CLWarnings.h),
  [`DiagnosticCategories.h`](../../include/clang/Basic/DiagnosticCategories.h)。
- 关键类/函数: `diagGroupFromCLWarningID`, `diag::Group`。

[`Sarif.cpp`](Sarif.cpp) — SARIF (静态分析结果交换格式) JSON 文档写入
器, 包含 artifact/rule/result 构造。
- 上游: [`AnalysisConsumer.cpp`](../StaticAnalyzer/Frontend/AnalysisConsumer.cpp)。
- 下游: [`Sarif.h`](../../include/clang/Basic/Sarif.h),
  [`SourceManager.cpp`](SourceManager.cpp),
  [`llvm/Support/JSON.h`](../../../llvm/include/llvm/Support/JSON.h)。
- 关键类/函数: `class SarifDocumentWriter`, `class SarifResult`,
  `class SarifRule`, `class SarifArtifact`,
  `percentEncodeURICharacter`。

### 3.3 Language & identifiers

[`IdentifierTable.cpp`](IdentifierTable.cpp) — 实现
`IdentifierTable` / `IdentifierInfo`, 关键字添加、token-kind 标记、
ObjC keyword 缓存。
- 上游: [`Preprocessor.cpp`](../Lex/Preprocessor.cpp),
  [`Parser.cpp`](../Parse/Parser.cpp)。
- 下游: [`IdentifierTable.h`](../../include/clang/Basic/IdentifierTable.h),
  [`TokenKinds.h`](../../include/clang/Basic/TokenKinds.h),
  [`CharInfo.cpp`](CharInfo.cpp)。
- 关键类/函数: `class IdentifierTable`, `class IdentifierInfo`,
  `AddKeywords`, `ObjCOrBuiltinID`。

[`LangOptions.cpp`](LangOptions.cpp) — `LangOptions` 构造,
`resetNonModularOptions` (PCM 兼容模式), OpenCL/HIP/Module 派生标志。
- 上游: [`CompilerInvocation.cpp`](../Frontend/CompilerInvocation.cpp),
  [`ASTWriter.cpp`](../Serialization/ASTWriter.cpp)。
- 下游: [`LangOptions.h`](../../include/clang/Basic/LangOptions.h),
  [`LangStandard.h`](../../include/clang/Basic/LangStandard.h)。
- 关键类/函数: `class LangOptions`, `resetNonModularOptions`,
  `isNoBuiltinFunc`, `getOpenCLVersionTuple`。

[`LangStandards.cpp`](LangStandards.cpp) — `LangStandard` 表
(c99/c11/c++17/gnu17/hlsl202x 等), 名称 ↔ Kind 互查, HLSL 子集。
- 上游: [`CompilerInvocation.cpp`](../Frontend/CompilerInvocation.cpp),
  [`InitPreprocessor.cpp`](../Frontend/InitPreprocessor.cpp)。
- 下游: [`LangStandard.h`](../../include/clang/Basic/LangStandard.h),
  [`clang/Config/config.h`](../../include/clang/Config/config.h)。
- 关键类/函数: `class LangStandard`, `languageToString`,
  `getLangKind`, `getDefaultLanguageStandard`。

[`OperatorPrecedence.cpp`](OperatorPrecedence.cpp) —
`getBinOpPrecedence`: 由 `tok::TokenKind` 返回 `prec::Level` (含 C++
模板右尖括号特例)。
- 上游: [`ParseExpr.cpp`](../Parse/ParseExpr.cpp),
  [`ParseTemplate.cpp`](../Parse/ParseTemplate.cpp)。
- 下游: [`OperatorPrecedence.h`](../../include/clang/Basic/OperatorPrecedence.h)。
- 关键类/函数: `getBinOpPrecedence`, `prec::Level`。

[`TokenKinds.cpp`](TokenKinds.cpp) — 实现
`tok::getTokenName` / `getPunctuatorSpelling` /
`getKeywordSpelling` 等查表函数。
- 上游: [`Preprocessor.cpp`](../Lex/Preprocessor.cpp),
  [`Parser.cpp`](../Parse/Parser.cpp)。
- 下游: [`TokenKinds.h`](../../include/clang/Basic/TokenKinds.h)。
- 关键类/函数: `getTokenName`, `getPunctuatorSpelling`,
  `getKeywordSpelling`, `isAnnotation`。

[`CharInfo.cpp`](CharInfo.cpp) — 256-entry `InfoTable`, ASCII 字符分类
(`CHAR_DIGIT`/`CHAR_PUNCT`/`CHAR_UPPER` 等)。
- 上游: [`IdentifierTable.cpp`](IdentifierTable.cpp),
  [`Preprocessor.cpp`](../Lex/Preprocessor.cpp)。
- 下游: [`CharInfo.h`](../../include/clang/Basic/CharInfo.h)。
- 关键类/函数: `charinfo::InfoTable`, `isIdentifierBody`,
  `isHorizontalWhitespace`。

### 3.4 Module system

[`Module.cpp`](Module.cpp) — 实现 `Module` (子模块/可见性/配置宏/隐式
模块推断)。
- 上游: [`ModuleMap.cpp`](../Lex/ModuleMap.cpp),
  [`ASTWriter.cpp`](../Serialization/ASTWriter.cpp)。
- 下游: [`Module.h`](../../include/clang/Basic/Module.h),
  [`TargetInfo.cpp`](TargetInfo.cpp),
  [`SourceLocation.cpp`](SourceLocation.cpp)。
- 关键类/函数: `class Module`, `isPlatformEnvironment`,
  `enum Kind`, `class Requirement`,
  `class UnresolvedHeaderDirective`。

[`ASTSourceDescriptor.cpp`](ASTSourceDescriptor.cpp) —
`ASTSourceDescriptor` (抽象模块和 PCH 的源) 构造与 Name 解析。
- 上游: [`ASTUnit.cpp`](../Frontend/ASTUnit.cpp),
  [`ASTReader.cpp`](../Serialization/ASTReader.cpp)。
- 下游: [`ASTSourceDescriptor.h`](../../include/clang/Basic/ASTSourceDescriptor.h),
  [`Module.h`](../../include/clang/Basic/Module.h)。
- 关键类/函数: `class ASTSourceDescriptor`, `getModuleName`。

### 3.5 Attribute system

[`Attributes.cpp`](Attributes.cpp) — `AttributeCommonInfo` 规范化
(`__foo__` → `foo`), `hasAttribute` 查询与 `__has_attribute` 实现。
- 上游: [`SemaAttr.cpp`](../Sema/SemaAttr.cpp),
  [`ParseDecl.cpp`](../Parse/ParseDecl.cpp)。
- 下游:
  [`AttrHasAttributeImpl.inc`](../../include/clang/Basic/AttrHasAttributeImpl.inc)
  (TableGen 生成),
  [`ParsedAttrInfo.cpp`](ParsedAttrInfo.cpp),
  [`TargetInfo.cpp`](TargetInfo.cpp)。
- 关键类/函数: `class AttributeCommonInfo`, `hasAttribute`,
  `canonicalizeAttrName`。

[`ParsedAttrInfo.cpp`](ParsedAttrInfo.cpp) —
`ParsedAttrInfoRegistry` (LLVM 静态注册表) 初始化, 实例化插件贡献的
属性。
- 上游: [`SemaAttr.cpp`](../Sema/SemaAttr.cpp),
  [`Attributes.cpp`](Attributes.cpp)。
- 下游: [`ParsedAttrInfo.h`](../../include/clang/Basic/ParsedAttrInfo.h),
  [`llvm/Support/ManagedStatic.h`](../../../llvm/include/llvm/Support/ManagedStatic.h)。
- 关键类/函数: `class ParsedAttrInfoRegistry`,
  `getAttributePluginInstances`, `class ParsedAttrInfo`。

[`SimpleTypoCorrection.cpp`](SimpleTypoCorrection.cpp) +
[`SimpleTypoCorrection.h`](../../include/clang/Basic/SimpleTypoCorrection.h)
— 基于编辑距离的轻量级拼写纠错 (用于 unknown warning 提示)。
- 上游: [`Warnings.cpp`](Warnings.cpp)。
- 下游: [`IdentifierTable.cpp`](IdentifierTable.cpp)。
- 关键类/函数: `class SimpleTypoCorrection`, `add`, `getCorrection`,
  `hasCorrection`。

### 3.6 Target architecture core

[`TargetInfo.cpp`](TargetInfo.cpp) — `TargetInfo` 基类核心实现
(默认 ABI/intrinsic/cpu/feature/平台默认宏与地址空间映射)。
- 上游: [`CodeGenModule.cpp`](../CodeGen/CodeGenModule.cpp),
  [`Targets.cpp`](Targets.cpp),
  [`ASTContext.cpp`](../AST/ASTContext.cpp)。
- 下游: [`TargetInfo.h`](../../include/clang/Basic/TargetInfo.h),
  [`AddressSpaces.h`](../../include/clang/Basic/AddressSpaces.h),
  [`LangOptions.h`](../../include/clang/Basic/LangOptions.h)。
- 关键类/函数: `class TargetInfo`, `getTargetDefines`,
  `setMaxAtomicWidth`, `checkCPUKind`, `getBuiltinVaListKind`。

[`Targets.cpp`](Targets.cpp) — `AllocateTarget`: 按 `Triple` 分发到
`Targets/*.h` 中的具体 `TargetInfo` 子类工厂。
- 上游: [`CompilerInstance.cpp`](../Frontend/CompilerInstance.cpp),
  [`CodeGenModule.cpp`](../CodeGen/CodeGenModule.cpp)。
- 下游: `Targets/AArch64.h`、`Targets/X86.h`、
  `Targets/OSTargets.h`、所有 `Targets/*.h`。
- 关键类/函数: `AllocateTarget`, `DefineStd`, `getTargetMask`。

[`Targets.h`](Targets.h) — Targets 子目录公共聚合头, include
`TargetDefines.h` 与 `clang/Basic/TargetInfo.h`。
- 上游: [`Targets.cpp`](Targets.cpp)。
- 下游: [`TargetDefines.h`](TargetDefines.h),
  [`TargetInfo.h`](../../include/clang/Basic/TargetInfo.h)。
- 关键类/函数: (wrapper header)。

[`TargetID.cpp`](TargetID.cpp) — 解析 `--offload-arch=target-id`
(AMDGPU 多维 feature: xnack/sramecc), 规范化为 Processor+Feature。
- 上游: [`Driver.cpp`](../Driver/Driver.cpp),
  [`Targets.cpp`](Targets.cpp)。
- 下游: [`TargetID.h`](../../include/clang/Basic/TargetID.h),
  [`OffloadArch.cpp`](OffloadArch.cpp),
  [`llvm/TargetParser/AMDGPUTargetParser.h`](../../../llvm/include/llvm/TargetParser/AMDGPUTargetParser.h)。
- 关键类/函数: `getAllPossibleTargetIDFeatures`,
  `getCanonicalProcessorName`, `parseTargetID`。

[`TargetDefines.h`](TargetDefines.h) — `MacroBuilder` (`BuildDefine` +
链式 `push_back` 输出 `#define ...`) — 给所有 arch 共享用。
- 上游: `Targets/*.cpp`。
- 下游: (无外部)。
- 关键类/函数: `class MacroBuilder`, `defineMacro`, `undefineMacro`。

[`BuiltinTargetFeatures.h`](BuiltinTargetFeatures.h) —
`Builtin::TargetFeatures`: 解析 `,`(`and`) / `|`(`or`) / `(` `)` 表达式,
builtin 的目标 feature 谓词求值。
- 上游: [`Builtins.cpp`](Builtins.cpp)。
- 下游: [`llvm/ADT/StringMap.h`](../../../llvm/include/llvm/ADT/StringMap.h)。
- 关键类/函数: `Builtin::TargetFeatures`, `hasRequiredFeatures`。

### 3.7 Builtins & CodeGen

[`Builtins.cpp`](Builtins.cpp) — 实现 `Builtin::Context` (sharded
infos, aux builtin, target builtin), `BuiltinInfo` 名字/类型表。
- 上游: [`SemaDecl.cpp`](../Sema/SemaDecl.cpp),
  [`CGBuiltin.cpp`](../CodeGen/CGBuiltin.cpp)。
- 下游: [`Builtins.h`](../../include/clang/Basic/Builtins.h),
  [`BuiltinTargetFeatures.h`](BuiltinTargetFeatures.h),
  [`TargetInfo.cpp`](TargetInfo.cpp)。
- 关键类/函数: `class Builtin::Context`, `struct Builtin::Info`,
  `struct Builtin::InfosShard`, `getBuiltinName`。

[`BuiltinTraits.cpp`](BuiltinTraits.cpp) —
`__has_*`(`type`/`array`/`unary`/`expression` trait) 的
name/spelling/arity 查表 (`BuiltinTraits.inc`)。
- 上游: [`SemaExpr.cpp`](../Sema/SemaExpr.cpp)。
- 下游:
  [`BuiltinTraits.h`](../../include/clang/Basic/BuiltinTraits.h),
  [`BuiltinTraits.inc`](../../include/clang/Basic/BuiltinTraits.inc)
  (TableGen 生成)。
- 关键类/函数: `getTraitName`, `getTraitSpelling`,
  `getTypeTraitArity`。

[`CodeGenOptions.cpp`](CodeGenOptions.cpp) — `CodeGenOptions`
构造/`resetNonModularOptions` 与 `remapDebugPathPrefix` (debug 路径重映射)。
- 上游: [`CompilerInvocation.cpp`](../Frontend/CompilerInvocation.cpp)。
- 下游:
  [`CodeGenOptions.h`](../../include/clang/Basic/CodeGenOptions.h),
  [`llvm/Support/Path.h`](../../../llvm/include/llvm/Support/Path.h)。
- 关键类/函数: `class CodeGenOptions`, `resetNonModularOptions`,
  `remapDebugPathPrefix`。

### 3.8 Sanitizers & instrumentation lists

[`Sanitizers.cpp`](Sanitizers.cpp) — `SanitizerMask` /
`SanitizerMaskCutoffs`, `parseSanitizerValue` (字符串 → 掩码),
`AllSanitizers` 表。
- 上游: [`SanitizerArgs.cpp`](../Driver/SanitizerArgs.cpp),
  [`NoSanitizeList.cpp`](NoSanitizeList.cpp)。
- 下游: [`Sanitizers.h`](../../include/clang/Basic/Sanitizers.h),
  [`Sanitizers.def`](../../include/clang/Basic/Sanitizers.def)
  (TableGen 生成)。
- 关键类/函数: `parseSanitizerValue`, `SanitizerMaskCutoffs`,
  `SanitizerKind`。

[`SanitizerSpecialCaseList.cpp`](SanitizerSpecialCaseList.cpp) — 扩
展 `llvm::SpecialCaseList`, 以 `SanitizerMask` 查询各 section, 持有
`SanitizerSections`。
- 上游: [`NoSanitizeList.cpp`](NoSanitizeList.cpp)。
- 下游: [`SanitizerSpecialCaseList.h`](../../include/clang/Basic/SanitizerSpecialCaseList.h),
  [`Sanitizers.def`](../../include/clang/Basic/Sanitizers.def)。
- 关键类/函数: `class SanitizerSpecialCaseList`, `inSection`,
  `inSectionBlame`, `createSanitizerSections`。

[`NoSanitizeList.cpp`](NoSanitizeList.cpp) — `NoSanitizeList`: 查询
`fun`/`src`/`type`/`mainfile`/`global` 前缀, 决定不参与 instrument 的
实体。
- 上游: [`SanitizerMetadata.cpp`](../CodeGen/SanitizerMetadata.cpp)。
- 下游: [`SanitizerSpecialCaseList.cpp`](SanitizerSpecialCaseList.cpp),
  [`SourceManager.cpp`](SourceManager.cpp)。
- 关键类/函数: `class NoSanitizeList`, `containsFunction`,
  `containsFile`, `containsLocation`。

[`XRayInstr.cpp`](XRayInstr.cpp) — `parseXRayInstrValue` /
`serializeXRayInstrValue` (XRay 插桩掩码
`all`/`function`/`typed`/`custom`)。
- 上游: [`clang/lib/Driver/ToolChains/`](../Driver/ToolChains/)。
- 下游: [`XRayInstr.h`](../../include/clang/Basic/XRayInstr.h)。
- 关键类/函数: `parseXRayInstrValue`, `serializeXRayInstrValue`,
  `XRayInstrMask`。

[`XRayLists.cpp`](XRayLists.cpp) — `XRayFunctionFilter`: 三段
(`always`/`never`/`attr`) `SpecialCaseList`, 决定函数/文件 XRay
插桩。
- 上游: [`CodeGenFunction.cpp`](../CodeGen/CodeGenFunction.cpp)。
- 下游: [`SourceManager.cpp`](SourceManager.cpp),
  [`llvm/Support/SpecialCaseList.h`](../../../llvm/include/llvm/Support/SpecialCaseList.h)。
- 关键类/函数: `class XRayFunctionFilter`, `shouldImbueFunction`,
  `shouldImbueFunctionsInFile`。

[`ProfileList.cpp`](ProfileList.cpp) — `ProfileList`: 按
`llvm::driver::ProfileInstrKind` (`clang`/`llvm`/`csllvm`/
`sample-coldcov`) 段决定 `allow`/`skip`。
- 上游: [`CodeGenPGO.cpp`](../CodeGen/CodeGenPGO.cpp)。
- 下游: [`SourceManager.cpp`](SourceManager.cpp),
  [`llvm/Support/SpecialCaseList.h`](../../../llvm/include/llvm/Support/SpecialCaseList.h)。
- 关键类/函数: `class ProfileList`, `isFunctionExcluded`,
  `isFileExcluded`, `enum ExclusionType`。

### 3.9 Offload / GPU / CUDA

[`OffloadArch.cpp`](OffloadArch.cpp) — `OffloadArch` 类: 跨架构
(NVPTX/AMDGPU/AMDGCNSPIRV/Intel) 抽象的 offload arch, 字符串/虚拟
arch 互查。
- 上游: [`Driver.cpp`](../Driver/Driver.cpp),
  [`TargetID.cpp`](TargetID.cpp)。
- 下游: [`OffloadArch.h`](../../include/clang/Basic/OffloadArch.h),
  [`llvm/TargetParser/AMDGPUTargetParser.h`](../../../llvm/include/llvm/TargetParser/AMDGPUTargetParser.h),
  [`llvm/TargetParser/NVPTXTargetParser.h`](../../../llvm/include/llvm/TargetParser/NVPTXTargetParser.h)。
- 关键类/函数: `class OffloadArch`, `CudaDefault`, `HIPDefault`,
  `OffloadArchToString`, `OffloadArchToVirtualArchString`。

[`Cuda.cpp`](Cuda.cpp) — `CudaVersion` 解析/字符串化,
`Min`/`MaxVersionForOffloadArch`, `CudaFeatureEnabled` (≥ 9.2/10.1 判定)。
- 上游: [`OffloadArch.cpp`](OffloadArch.cpp),
  [`Targets.cpp`](Targets.cpp),
  [`Targets/NVPTX.cpp`](Targets/NVPTX.cpp)。
- 下游: [`Cuda.h`](../../include/clang/Basic/Cuda.h),
  [`llvm/TargetParser/NVPTXTargetParser.h`](../../../llvm/include/llvm/TargetParser/NVPTXTargetParser.h)。
- 关键类/函数: `class CudaVersion`, `CudaVersionToString`,
  `MinVersionForOffloadArch`, `CudaFeatureEnabled`。

### 3.10 OpenMP / OpenCL / ObjC

[`OpenMPKinds.cpp`](OpenMPKinds.cpp) — OpenMP
clause/schedule/modifier/distribute/depend/linear 等枚举的 name ↔ kind
互查。
- 上游: [`SemaOpenMP.cpp`](../Sema/SemaOpenMP.cpp),
  [`CGOpenMPRuntime.cpp`](../CodeGen/CGOpenMPRuntime.cpp)。
- 下游: [`OpenMPKinds.h`](../../include/clang/Basic/OpenMPKinds.h),
  [`OpenMPKinds.def`](../../include/clang/Basic/OpenMPKinds.def)
  (TableGen 生成),
  [`llvm/Frontend/OpenMP/OMPKinds.def`](../../../llvm/include/llvm/Frontend/OpenMP/OMPKinds.def)。
- 关键类/函数: `getOpenMPSimpleClauseType`,
  `getOpenMPDefaultVariableCategory`, `isOpenMPContextTrait`。

[`OpenCLOptions.cpp`](OpenCLOptions.cpp) — `OpenCLOptions`: 已知
extension/feature 表 (`cl_khr_fp64`/`__opencl_c_*`), 依赖关系 + 版本
可用性。
- 上游: [`SemaOpenCL.cpp`](../Sema/SemaOpenCL.cpp)。
- 下游: [`OpenCLOptions.h`](../../include/clang/Basic/OpenCLOptions.h),
  [`TargetInfo.cpp`](TargetInfo.cpp)。
- 关键类/函数: `class OpenCLOptions`, `isKnown`,
  `isAvailableOption`, `isSupported`, `isEnabled`。

[`ObjCRuntime.cpp`](ObjCRuntime.cpp) — `ObjCRuntime`:
macosx/ios/gnustep/objfw 等运行时 kind + version 解析, 流输出。
- 上游: [`CompilerInvocation.cpp`](../Frontend/CompilerInvocation.cpp),
  [`CodeGenModule.cpp`](../CodeGen/CodeGenModule.cpp)。
- 下游: [`ObjCRuntime.h`](../../include/clang/Basic/ObjCRuntime.h)。
- 关键类/函数: `class ObjCRuntime`, `tryParse`, `getAsString`,
  `operator<<`。

### 3.11 OS / SDK / Stack / misc

[`DarwinSDKInfo.cpp`](DarwinSDKInfo.cpp) — 解析 Darwin
`SDKSettings.json`, 维护 `RelatedTargetVersionMapping` (iOS/macOS 版本映射)。
- 上游: [`Darwin.cpp`](../Driver/ToolChains/Darwin.cpp),
  [`CompilerInstance.cpp`](../Frontend/CompilerInstance.cpp)。
- 下游: [`DarwinSDKInfo.h`](../../include/clang/Basic/DarwinSDKInfo.h),
  [`llvm/Support/JSON.h`](../../../llvm/include/llvm/Support/JSON.h),
  [`llvm/TargetParser/ARMTargetParser.h`](../../../llvm/include/llvm/TargetParser/ARMTargetParser.h)。
- 关键类/函数: `class DarwinSDKInfo`,
  `class RelatedTargetVersionMapping`, `parseJSON`。

[`Stack.cpp`](Stack.cpp) — 栈底记录 + `isStackNearlyExhausted`
(256 KiB 余量判定) + `runWithSufficientStackSpaceSlow`。
- 上游: [`StackExhaustionHandler.cpp`](StackExhaustionHandler.cpp),
  [`Parser.cpp`](../Parse/Parser.cpp)。
- 下游: [`Stack.h`](../../include/clang/Basic/Stack.h),
  [`llvm/Support/ProgramStack.h`](../../../llvm/include/llvm/Support/ProgramStack.h)。
- 关键类/函数: `noteBottomOfStack`, `isStackNearlyExhausted`,
  `runWithSufficientStackSpaceSlow`, `DesiredStackSize`。

[`StackExhaustionHandler.cpp`](StackExhaustionHandler.cpp) —
`StackExhaustionHandler`: `warnStackExhausted` 单次警告, 封装 `Stack.h`
的高级封装。
- 上游: [`Parser.cpp`](../Parse/Parser.cpp),
  [`Sema.cpp`](../Sema/Sema.cpp)。
- 下游: [`Stack.cpp`](Stack.cpp),
  [`Diagnostic.h`](../../include/clang/Basic/Diagnostic.h)。
- 关键类/函数: `class StackExhaustionHandler`,
  `runWithSufficientStackSpace`, `warnStackExhausted`。

[`MakeSupport.cpp`](MakeSupport.cpp) — `printMakeDependencyFile` +
`quoteMakeTarget`: 生成 Make/NMake 兼容的 `.d` 依赖文件。
- 上游: [`DependencyFile.cpp`](../Frontend/DependencyFile.cpp)。
- 下游: [`MakeSupport.h`](../../include/clang/Basic/MakeSupport.h),
  [`llvm/Support/Path.h`](../../../llvm/include/llvm/Support/Path.h)。
- 关键类/函数: `printMakeDependencyFile`, `quoteMakeTarget`,
  `DependencyOutputFormat`。

[`Version.cpp`](Version.cpp) — `getClangFullVersion` /
`getClangRevision` 等: 把 `VCSVersion.inc` 与配置宏组合成版本字符串。
- 上游: [`Driver.cpp`](../Driver/Driver.cpp),
  [`TextDiagnosticPrinter.cpp`](../Frontend/TextDiagnosticPrinter.cpp)。
- 下游: `VCSVersion.inc` (TableGen 生成),
  [`clang/Config/config.h`](../../include/clang/Config/config.h)。
- 关键类/函数: `getClangFullVersion`, `getClangFullCPPVersion`,
  `getClangRepositoryPath`。

### 3.12 Targets/ 子目录 — 27 arch

每个 arch 一个 `.cpp` + 一个 `.h`, 派生 `TargetInfo` 子类, 全部
dispatched by [`Targets.cpp::AllocateTarget`](Targets.cpp)。

| Arch | Path | 关键类 | 关键方法 |
|------|------|--------|----------|
| AArch64 | [`Targets/AArch64.cpp`](Targets/AArch64.cpp) + `.h` | `AArch64TargetInfo`, `AArch64le/beTargetInfo`, `WindowsARM64/AppleMachO/DarwinAArch64/...` | `setFeatureEnabled`, `hasFeature`, `getTargetDefinesARMV81A/82A/83A`, `validateTargetFeatures`, `setMaxAtomicWidth` |
| AMDGPU | [`Targets/AMDGPU.cpp`](Targets/AMDGPU.cpp) + `.h` | `AMDGPUTargetInfo` (RDNA/GCN/CDNA, 含 HIP/SPIR-V/OpenCL/AMDHSA) | `isAddressSpaceSupersetOf`, `getTargetDefines`, `setMaxAtomicWidth`, `hasFeature`, `checkCPUKind` |
| ARC | [`Targets/ARC.cpp`](Targets/ARC.cpp) + `.h` | `ARCTargetInfo` (Synopsys ARC, BE/LE) | `getTargetDefines`, `setMaxAtomicWidth` |
| ARM | [`Targets/ARM.cpp`](Targets/ARM.cpp) + `.h` | `ARMTargetInfo`, `ARMle/beTargetInfo`, `WindowsARM/MicrosoftARM/MinGWARM/CygwinARM/AppleMachOARM/DarwinARM/...` | `setFeatureEnabled`, `hasFeature`, `getTargetDefinesARMV8{A,82A,83A}`, `setMaxAtomicWidth` |
| AVR | [`Targets/AVR.cpp`](Targets/AVR.cpp) + `.h` | `AVRTargetInfo` (Atmel AVR 8-bit MCU) | `getTargetDefines`, `setMaxAtomicWidth`, `checkCPUKind` |
| BPF | [`Targets/BPF.cpp`](Targets/BPF.cpp) + `.h` | `BPFTargetInfo` (eBPF, BE/LE) | `setFeatureEnabled`, `getTargetDefines`, `setMaxAtomicWidth` |
| CSKY | [`Targets/CSKY.cpp`](Targets/CSKY.cpp) + `.h` | `CSKYTargetInfo` (中天微 C-Sky) | `getTargetDefines`, `setMaxAtomicWidth` |
| DirectX | [`Targets/DirectX.cpp`](Targets/DirectX.cpp) + `.h` | `DirectXTargetInfo` (DXIL/SM6) | `getTargetDefines`, `setMaxAtomicWidth` |
| Hexagon | [`Targets/Hexagon.cpp`](Targets/Hexagon.cpp) + `.h` | `HexagonTargetInfo` (QDSP6 V60-V75) | `getTargetDefines`, `setMaxAtomicWidth`, `hasFeature` |
| Lanai | [`Targets/Lanai.cpp`](Targets/Lanai.cpp) + `.h` | `LanaiTargetInfo` (LLVM 实验后端) | `getTargetDefines`, `setMaxAtomicWidth` |
| LoongArch | [`Targets/LoongArch.cpp`](Targets/LoongArch.cpp) + `.h` | `LoongArchTargetInfo`, `LoongArch32/64TargetInfo` (龙芯 LA32S/LA64) | `getTargetDefines`, `setMaxAtomicWidth`, `hasFeature` |
| M68k | [`Targets/M68k.cpp`](Targets/M68k.cpp) + `.h` | `M68kTargetInfo` (m68000-m68060, ColdFire) | `getTargetDefines`, `setMaxAtomicWidth` |
| MSP430 | [`Targets/MSP430.cpp`](Targets/MSP430.cpp) + `.h` | `MSP430TargetInfo` (TI MSP430) | `getTargetDefines`, `setMaxAtomicWidth` |
| Mips | [`Targets/Mips.cpp`](Targets/Mips.cpp) + `.h` | `MipsTargetInfo`, `WindowsMips/MicrosoftMips/MinGWMips/...` | `getTargetDefines`, `hasFeature`, `setMaxAtomicWidth` |
| NVPTX | [`Targets/NVPTX.cpp`](Targets/NVPTX.cpp) + `.h` | `NVPTXTargetInfo` (NVIDIA PTX, CUDA device-only) | `isAddressSpaceSupersetOf`, `getTargetDefines`, `setMaxAtomicWidth` |
| PPC | [`Targets/PPC.cpp`](Targets/PPC.cpp) + `.h` | `PPCTargetInfo`, `PPC32/64TargetInfo`, 含 AIX/Darwin/Linux/MinGW/Microsoft 变体 | `setFeatureEnabled`, `getTargetDefines`, `setMaxAtomicWidth`, `hasFeature` |
| RISCV | [`Targets/RISCV.cpp`](Targets/RISCV.cpp) + `.h` | `RISCVTargetInfo`, `RISCV32/64TargetInfo` (RV32/RV64 + M/A/F/D/V/B/K + XAndes/XMIPS/XCV) | `getTargetDefines`, `setMaxAtomicWidth`, `hasFeature` |
| SPIR | [`Targets/SPIR.cpp`](Targets/SPIR.cpp) + `.h` | `SPIRTargetInfo`, `SPIR32/64TargetInfo` (OpenCL/HIP/SYCL 中间表示) | `isAddressSpaceSupersetOf`, `getTargetDefines` |
| Sparc | [`Targets/Sparc.cpp`](Targets/Sparc.cpp) + `.h` | `SparcTargetInfo`, `SparcV8/V8el/V9TargetInfo` (SPARC V8/V9, BE/LE) | `getTargetDefines`, `setMaxAtomicWidth` |
| SystemZ | [`Targets/SystemZ.cpp`](Targets/SystemZ.cpp) + `.h` | `SystemZTargetInfo` (IBM z/Architecture s390x) | `getTargetDefines`, `setMaxAtomicWidth` |
| TCE | [`Targets/TCE.cpp`](Targets/TCE.cpp) + `.h` | `TCETargetInfo`, `TCELE/LE64TargetInfo` (TTA) | `getTargetDefines` |
| VE | [`Targets/VE.cpp`](Targets/VE.cpp) + `.h` | `VETargetInfo` (NEC SX-Aurora TSUBASA) | `getTargetDefines`, `setMaxAtomicWidth` |
| WebAssembly | [`Targets/WebAssembly.cpp`](Targets/WebAssembly.cpp) + `.h` | `WebAssemblyTargetInfo`, `WebAssembly32/64TargetInfo` (Emscripten/非Emscripten) | `setFeatureEnabled`, `hasFeature`, `getTargetDefines` |
| X86 | [`Targets/X86.cpp`](Targets/X86.cpp) + `.h` | `X86TargetInfo`, `X86_32/64TargetInfo`, 含 Apple/Darwin/Windows/MinGW/Cygwin/Linux/Android/OHOS/Haiku/RTEMS/MCU 多 OS 变体 | `validateOperandSize`, `setFeatureEnabled`, `setMaxAtomicWidth`, `hasFeature`, `checkCPUKind` |
| XCore | [`Targets/XCore.cpp`](Targets/XCore.cpp) + `.h` | `XCoreTargetInfo` (XMOS XCore XS1) | `getTargetDefines`, `setMaxAtomicWidth` |
| Xtensa | [`Targets/Xtensa.cpp`](Targets/Xtensa.cpp) + `.h` | `XtensaTargetInfo` (Tensilica Xtensa, ESP32/S1C33/NX/NXP/DLX/MAC/Windows) | `getTargetDefines`, `setMaxAtomicWidth` |
| OSTargets | [`Targets/OSTargets.cpp`](Targets/OSTargets.cpp) + `.h` | `OSTargetInfo<TgtInfo>`, `AppleMachOTargetInfo<Target>`, `DarwinTargetInfo<Target>`, `MinGWTargetInfo<Target>`, `MicrosoftTargetInfo<Target>`, `LinuxTargetInfo<Target>`, `NaClTargetInfo<Target>`, `HaikuTargetInfo<Target>` (cross-arch OS 模板) | `getOSDefines`, `getAppleMachODefines`, `getDarwinDefines` |

### 3.13 Build / misc

[`CMakeLists.txt`](CMakeLists.txt) — 列出 `clangBasic` library target
的所有 `.cpp` 文件; 引用各 TableGen 输入生成 `.inc`。

---

## §4. 关键调用链

### 4.1 顶层 Frontend 初始化链

```
clang::CompilerInstance::createDiagnostics / createFileManager /
  createSourceManager / createTarget / createPreprocessor
  [clang/lib/Frontend/CompilerInstance.cpp]
  │
  ├─► new FileManager (FileManager.cpp)
  ├─► new SourceManager (SourceManager.cpp)
  │      └─ 缓存 VFS + FileID 映射
  ├─► new DiagnosticsEngine (Diagnostic.cpp)
  │      ├─ new DiagnosticIDs (DiagnosticIDs.cpp)
  │      │      └─ 全局 StaticDiagInfo 表 (AllDiagnosticKinds.inc)
  │      ├─ DiagnosticOptions (DiagnosticOptions.cpp)
  │      └─ ProcessWarningOptions (Warnings.cpp)
  │            ├─ 解析 -W/-Wno-/-Werror/-Wno-everything
  │            └─ 调用 DiagnosticsEngine::setSeverityForKind
  └─► AllocateTarget (Targets.cpp) ← 关键分发
        ├─ 根据 llvm::Triple::ArchType 选 Targets/XxxTargetInfo
        ├─ Targets/X86.cpp (e.g.) 构造 + 调 setFeatureEnabled
        ├─ Targets/OSTargets.h::OSTargetInfo (e.g.) 套 OS 宏
        ├─ TargetInfo::getTargetDefines 写出 #define ...
        └─ TargetInfo::setMaxAtomicWidth / checkCPUKind
             └─ 后续 Preprocessor/ASTContext/CodeGenModule 用 TargetInfo
```

### 4.2 诊断发出链

```
Sema::Diag(...) [clang/lib/Sema/Sema.cpp]
  └─ DiagnosticsEngine::Report [Diagnostic.cpp]
       ├─ DiagnosticIDs::getDiagnosticInfo [DiagnosticIDs.cpp]
       │    └─ 在 StaticDiagInfo 表中查 ID → severity / category
       ├─ ProcessDiag (Severity + SourceLocation + Range + FixIt)
       ├─ 调 TextDiagnosticPrinter::HandleDiagnostic (Frontend)
       │    ├─ SourceManager::getExpansionLoc / getSpellingLoc
       │    └─ 格式化为人类可读 + colored output
       └─ 调 SARIF writer [Sarif.cpp] (如 -fdiagnostics-format=sarif)
```

### 4.3 OpenMP/OpenCL 解析链

```
SemaOpenMP::ActOnOpenMPExecutableDirective [clang/lib/Sema/SemaOpenMP.cpp]
  ├─ OpenMPKinds::getOpenMPSimpleClauseType / getScheduleKind / ...
  │    [OpenMPKinds.cpp]  ←─ OpenMPKinds.def 查表
  ├─ LangOptions::OpenMP (LangOptions.cpp) ← 运行时版本
  └─ emit 转换给 CodeGen (CGOpenMPRuntime.cpp)
       ├─ OpenMPKinds 同样被 CodeGen 用

SemaOpenCL::checkBuiltinKernelWorkGroupSize [clang/lib/Sema/SemaOpenCL.cpp]
  ├─ OpenCLOptions::isAvailableOption / isSupported [OpenCLOptions.cpp]
  └─ LangOptions::OpenCLVersion
       └─ TargetInfo (Targets/AMDGPU.cpp / NVPTX.cpp / ...) 检查 address space
```

### 4.4 Builtin 校验链

```
SemaChecking::CheckBuiltinCall [clang/lib/Sema/SemaChecking.cpp]
  ├─ Builtin::Context::isBuiltin (Builtins.cpp)
  │    └─ Builtin::Info 查表 (Builtins.inc ← Builtins.td)
  ├─ per-arch Sema<X>::Check<X>BuiltinFunctionCall
  │    └─ 用 TargetInfo::hasFeature 校验 (Targets/X86.cpp 等)
  ├─ BuiltinTargetFeatures::hasRequiredFeatures
  │    [BuiltinTargetFeatures.h]
  └─ CGBuiltin::EmitBuiltinExpr [clang/lib/CodeGen/CGBuiltin.cpp]
       └─ Builtin::Context::getBuiltinName 等
```

### 4.5 Target attribute 解析链

```
Driver (clang -march=skylake-avx512 ...)
  │
  ▼
CompilerInvocation::ParseArgs [clang/lib/Frontend/CompilerInvocation.cpp]
  ├─ 解析 -march / -m<cpu> / -m<feature>+- 等
  └─ 传给 TargetInfo::setFeatureEnabled (Targets/X86.cpp 等)
       ├─ TargetInfo::hasFeature 检查
       ├─ TargetInfo::validateTargetFeatures
       └─ 影响 AddressSpace / MaxAtomicWidth / ABI choice
              │
              ▼
       ASTContext 构造时:
         ASTContext::InitBuiltinTypes → TargetInfo::getBuiltinVaListKind
         ASTContext::setTargetInfo → TargetInfo::getTargetDefines
              │
              ▼
       CodeGenModule::Create 决定 ABI / CallingConv / DataLayout
```

### 4.6 Sanitizer / XRay 插桩链

```
Driver -fsanitize=address,undefined ...
  │
  ▼
SanitizerArgs::SanitizerArgs [clang/lib/Driver/SanitizerArgs.cpp]
  ├─ parseSanitizerValue [Sanitizers.cpp] ←─ Sanitizers.def 查表
  ├─ parseXRayInstrValue [XRayInstr.cpp]
  └─ 生成 SanitizerMask 传给 Frontend
       │
       ▼
CompilerInstance 把 SanitizerArgs 注入到 CodeGen
  │
  ▼
CGCodeGenModule 构造:
  ├─ SanitizerSpecialCaseList::createOrCreate [SanitizerSpecialCaseList.cpp]
  │    └─ 加载 .scl 文件
  ├─ NoSanitizeList [NoSanitizeList.cpp]
  │    ├─ containsFunction / containsFile / containsLocation
  │    └─ 在 CGFunctionInstantiationData 决定是否插桩
  ├─ XRayFunctionFilter [XRayLists.cpp]
  │    └─ shouldImbueFunction / shouldImbueFunctionsInFile
  └─ ProfileList [ProfileList.cpp]
       └─ isFunctionExcluded / isFileExcluded (PGO)
```

---

## §5. 推荐阅读顺序

### 阶段 0: 基底与配置 (30 分钟)
1. [`Targets.h`](Targets.h) + [`TargetDefines.h`](TargetDefines.h) —
   Targets 子目录聚合头, 了解 arch 接口契约。
2. [`CMakeLists.txt`](CMakeLists.txt) — 看 lib/Basic 的注册顺序。
3. [`Version.cpp`](Version.cpp) — 简单, 理解 `clang --version`。
4. [`CharInfo.cpp`](CharInfo.cpp) + [`TokenKinds.cpp`](TokenKinds.cpp) —
   字符分类与 token 表。

### 阶段 1: 编译/链接身份 (1 小时)
- [`LangOptions.cpp`](LangOptions.cpp) +
  [`LangStandards.cpp`](LangStandards.cpp) +
  [`CodeGenOptions.cpp`](CodeGenOptions.cpp) +
  [`DiagnosticOptions.cpp`](DiagnosticOptions.cpp)
- [`ObjCRuntime.cpp`](ObjCRuntime.cpp) +
  [`Cuda.cpp`](Cuda.cpp) +
  [`OffloadArch.cpp`](OffloadArch.cpp) +
  [`DarwinSDKInfo.cpp`](DarwinSDKInfo.cpp)
- [`OpenCLOptions.cpp`](OpenCLOptions.cpp) +
  [`OpenMPKinds.cpp`](OpenMPKinds.cpp) +
  [`OperatorPrecedence.cpp`](OperatorPrecedence.cpp)

### 阶段 2: 源文件 / 词法原料 (1.5 小时)
- [`FileEntry.cpp`](FileEntry.cpp) → [`FileManager.cpp`](FileManager.cpp) →
  [`SourceManager.cpp`](SourceManager.cpp) → [`SourceLocation.cpp`](SourceLocation.cpp) →
  [`SourceMgrAdapter.cpp`](SourceMgrAdapter.cpp) →
  [`IdentifierTable.cpp`](IdentifierTable.cpp)

### 阶段 3: 诊断 (1 小时)
- [`Diagnostic.cpp`](Diagnostic.cpp) +
  [`DiagnosticIDs.cpp`](DiagnosticIDs.cpp) +
  [`Warnings.cpp`](Warnings.cpp)
- [`CLWarnings.cpp`](CLWarnings.cpp) +
  [`SimpleTypoCorrection.cpp`](SimpleTypoCorrection.cpp) +
  [`Sarif.cpp`](Sarif.cpp)

### 阶段 4: 模块 / 属性 (30 分钟)
- [`Module.cpp`](Module.cpp) +
  [`ASTSourceDescriptor.cpp`](ASTSourceDescriptor.cpp)
- [`Attributes.cpp`](Attributes.cpp) +
  [`ParsedAttrInfo.cpp`](ParsedAttrInfo.cpp)

### 阶段 5: Target 架构核心 (1.5 小时)
1. [`TargetInfo.cpp`](TargetInfo.cpp) — 基类核心 (默认 ABI/cpu/feature)
2. [`Targets.cpp`](Targets.cpp) — 唯一分发点
3. [`Targets/OSTargets.cpp`](Targets/OSTargets.cpp) + `.h` — 跨 arch
   OS 模板
4. [`Targets/X86.cpp`](Targets/X86.cpp) — 最具代表性的 arch (含 20+
   OS 变体)
5. [`Targets/AArch64.cpp`](Targets/AArch64.cpp) — ARM64 + SVE/SME
6. [`Targets/AMDGPU.cpp`](Targets/AMDGPU.cpp) — GPU arch
7. [`Targets/RISCV.cpp`](Targets/RISCV.cpp) — RISC-V + 多 vendor 扩展
8. 其他 arch (按需)

### 阶段 6: Builtins / Trait (30 分钟)
- [`Builtins.cpp`](Builtins.cpp) +
  [`BuiltinTraits.cpp`](BuiltinTraits.cpp) +
  [`BuiltinTargetFeatures.h`](BuiltinTargetFeatures.h)

### 阶段 7: Sanitizer / XRay / PGO (30 分钟)
- [`Sanitizers.cpp`](Sanitizers.cpp) +
  [`SanitizerSpecialCaseList.cpp`](SanitizerSpecialCaseList.cpp) +
  [`NoSanitizeList.cpp`](NoSanitizeList.cpp)
- [`XRayInstr.cpp`](XRayInstr.cpp) +
  [`XRayLists.cpp`](XRayLists.cpp) +
  [`ProfileList.cpp`](ProfileList.cpp)

### 阶段 8: 依赖 / 栈 / 杂项 (15 分钟)
[`MakeSupport.cpp`](MakeSupport.cpp) +
[`AtomicLineLogger.cpp`](AtomicLineLogger.cpp) +
[`Stack.cpp`](Stack.cpp) +
[`StackExhaustionHandler.cpp`](StackExhaustionHandler.cpp)

---

## §6. 常用操作指南

### 6.1 添加新目标架构 (假设 `MyArch`)

1. **新建 `Targets/MyArch.cpp` + `Targets/MyArch.h`**:
   - 派生 `TargetInfo`, 实现 `getTargetDefines`、`setMaxAtomicWidth`、
     `checkCPUKind`、`hasFeature`、`setFeatureEnabled` 等虚函数。
   - 在头里声明工厂:
     `std::unique_ptr<TargetInfo> AllocateMyArchTarget(...)`.
2. **注册到 [`Targets.cpp`](Targets.cpp)**: 在 `AllocateTarget` 的
   switch 加 `case llvm::Triple::myarch:` 分支, 返回 `AllocateMyArchTarget`。
3. **加进 [`CMakeLists.txt`](CMakeLists.txt)**: 添加
   `Targets/MyArch.cpp` 到 `clangBasic` library source list。
4. **OS 模板复用**: 如果你的 arch 有 Apple/Darwin/Windows/Linux 变体,
   经 `Targets/OSTargets.h::OSTargetInfo<Target>` 模板自动获得。
5. **测试**: `clang/test/Preprocessor/init.c` (含 #define 校验),
   `clang/test/CodeGen/<arch>*.c`。

### 6.2 添加新 builtin (通用或 per-arch)

1. **通用 builtin**: 在
   [`BuiltinsBase.td`](../../include/clang/Basic/BuiltinsBase.td) 加
   `def X` 块 (类型 + 属性)。
2. **per-arch builtin**: 在
   `Builtins<Arch>.td` (e.g.
   [`BuiltinsX86.td`](../../include/clang/Basic/BuiltinsX86.td)) 加
   `def X` 块 (含 target feature 要求)。
3. **编译**: 触发 TableGen 重新生成 `Builtins.inc`。
4. **Sema 校验** (如需): 在
   [`clang/lib/Sema/SemaChecking.cpp`](../Sema/SemaChecking.cpp) 的
   `CheckBuiltinCall` switch 加新 case, 或经 per-arch
   [`Sema<Arch>.cpp`](../Sema/SemaX86.cpp) 校验。
5. **Codegen** (如需): 在
   [`clang/lib/CodeGen/CGBuiltin.cpp`](../CodeGen/CGBuiltin.cpp) 加
   codegen。
6. **测试**: `clang/test/Sema/builtin-<name>.c` +
   `clang/test/CodeGen/<arch>-builtin-<name>.c`。

### 6.3 添加新诊断

1. **找到对应 .td**:
   - 编译器/Driver 错: `Diagnostic.td` /
     `DiagnosticDriverKinds.td` /
     `DiagnosticFrontendKinds.td`
   - Lex/Parser 错: `DiagnosticLexKinds.td` /
     `DiagnosticParseKinds.td`
   - Sema 错: `DiagnosticSemaKinds.td`
   - CodeGen 错: `DiagnosticCodeGenKinds.td`
   - AST 错: `DiagnosticASTKinds.td`
   - Serialization 错: `DiagnosticSerializationKinds.td`
   - 等
2. **加 `def err_my_diag : Error<...>`** (或 `Warning`/`ExtWarn`/`Note`)
   到对应 .td, 含 message + select clause。
3. **编译**: TableGen 重新生成 `AllDiagnosticKinds.inc` /
   `DiagnosticGroups.inc`。
4. **触发诊断**: 在相应 .cpp 文件调 `Diag(loc, diag::err_my_diag) << args`。
5. **测试**: `clang/test/<path>/<my-diag>.c` + 看 `-W<group>`。

### 6.4 添加新警告组 (例如 `-Wmy-group`)

1. 在 [`DiagnosticGroups.td`](../../include/clang/Basic/DiagnosticGroups.td)
   加 `def MyGroup : DiagGroup<"my-group">`, 可用 `in_group` 链入
   `AllWarnings` / 其他 group。
2. 在具体诊断的 .td 加 `in_group = MyGroup` (如未指定)。
3. 编译 + 测试:
   `clang -Wmy-group -Wno-my-group ...` (用 `RUN` 行测试开启/关闭)。

### 6.5 添加新 sanitizer

1. **加 `enum` + `Name`** 到
   [`Sanitizers.def`](../../include/clang/Basic/Sanitizers.def):
   `SANITIZER(<kind>, <group>, <name>, <alias>)`.
2. **编译**: TableGen 重新生成 (本目录无需重新生成, 因为 `.def` 是
   macro 形式)。
3. **Driver 解析**: 在
   [`clang/lib/Driver/SanitizerArgs.cpp`](../Driver/SanitizerArgs.cpp)
   检查新 sanitizer 的 deps (如 `requires`)。
4. **Sema/Codegen 校验**: 在
   [`clang/lib/CodeGen/SanitizerMetadata.cpp`](../CodeGen/SanitizerMetadata.cpp)
   + 各 sanitizer 运行时 `CodeGen/<sanitizer>` 加 metadata 注入。
5. **测试**: `clang/test/Driver/fsanitize.c` (开关) +
   `clang/test/CodeGen/<sanitizer>*.c`。

### 6.6 调试 TargetInfo

1. 用 `clang -E -dM` 看实际 #define (检查
   [`Targets/Xxx.cpp`](Targets/X86.cpp) 的 `getTargetDefines` 是否漏宏)。
2. 用 `clang -target <triple> -print-effective-triple` 看 driver 决策。
3. 用 `clang -print-target-features` 看 `setFeatureEnabled` 结果。
4. 用 `clang -print-supported-cpus` 看 `checkCPUKind` 是否识别。
5. 看 [`TargetInfo.cpp`](TargetInfo.cpp) 的 `setMaxAtomicWidth` 与
   CodeGenModule 是否一致 (影响 atomic 内置 builtin)。

### 6.7 调试 Sanitizer / XRay 插入

1. 用 `clang -fsanitize=address -fno-sanitize-recover=... -S` 看
   codegen (asm 是否插入 sanitizer runtime call)。
2. 用 `clang -fxray-instrument -fxray-list` 看 XRay list 解析。
3. 用 `clang -fprofile-instr-generate -fprofile-list=...` 看 PGO
   list 解析 (ProfileList.cpp)。
4. 看 [`NoSanitizeList.cpp`](NoSanitizeList.cpp) 的
   `containsLocation` (e.g. 某个函数漏掉, 看 `src`/`fun` 段)。

### 6.8 调试 SourceManager

1. 用 `clang -Xclang -ast-dump -ast-dump-decls -ast-dump-filter=foo`
   看 SourceLocation。
2. 看 `SourceManager::getExpansionLoc` vs `getSpellingLoc` (宏展开 vs
   spelling)。
3. 临时 `SourceManager::dump()` (如果有) 看 FileID / LineTable。

---

## §7. NT 注释索引

当前 `clang/lib/Basic/` 下尚无 `// <NT>` 注释。已建立目录索引, 姊妹
overview:

- `clang/lib/AST/` — Clang AST 层 (待写)
- `clang/lib/CodeGen/` (待写)
- `clang/lib/Driver/` — Clang 命令行驱动 (待写)
- `clang/lib/Frontend/` — Clang 前端桥接 (待写)
- `clang/lib/Lex/` — Clang Lexer (待写)
- `clang/lib/Parse/` — Clang Parser (待写)
- [`clang/lib/Sema/0-overview.md`](../Sema/0-overview.md) — Clang
  语义分析层

按"少而精"原则, 加 NT 注释建议优先级:

1. [`TargetInfo.cpp`](TargetInfo.cpp) — 8-12 段 (跨 arch 基类, 所有
   TargetInfo 子类都依赖)
2. [`SourceManager.cpp`](SourceManager.cpp) — 6-10 段 (最复杂 file/line
   tracking)
3. [`Builtins.cpp`](Builtins.cpp) — 6-8 段 (Builtin::Context / Info 表)
4. [`Diagnostic.cpp`](Diagnostic.cpp) + [`DiagnosticIDs.cpp`](DiagnosticIDs.cpp)
   — 5-8 段 (诊断机制核心)
5. [`Targets.cpp`](Targets.cpp) — 4-6 段 (唯一 arch 分发点)
6. [`FileManager.cpp`](FileManager.cpp) — 4-6 段 (VFS + 缓存)
7. [`Warnings.cpp`](Warnings.cpp) — 4-6 段 (-W/-Werror 解析)
8. [`IdentifierTable.cpp`](IdentifierTable.cpp) — 3-5 段
9. 单 arch (Targets/X86.cpp / AArch64.cpp / RISCV.cpp / AMDGPU.cpp 等)
   — 各 3-5 段
10. [`LangOptions.cpp`](LangOptions.cpp) + [`LangStandards.cpp`](LangStandards.cpp)
    — 各 3-4 段
11. [`Sanitizers.cpp`](Sanitizers.cpp) +
    [`NoSanitizeList.cpp`](NoSanitizeList.cpp) — 各 3-4 段
12. [`OpenMPKinds.cpp`](OpenMPKinds.cpp) +
    [`OpenCLOptions.cpp`](OpenCLOptions.cpp) — 各 2-3 段

---

**姊妹文档**: 本目录对应 LLVM 流水线中的 **Clang 基础设置 + 共享数据**
层, 是 Lex / Parse / AST / Sema / CodeGen / Frontend 都依赖的"底座"。
它与 [`clang/lib/Frontend/`](../Frontend/) 紧密配合 (Frontend 在
初始化时构造 Basic 的所有对象), 与
[`clang/lib/Driver/`](../Driver/) 配合接收命令行决策。Clang 用户可见
的选项在 [`clang/include/clang/Driver/`](../../include/clang/Driver/)。