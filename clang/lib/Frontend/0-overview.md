<!-- <NT>overview:clang/lib/Frontend/ -->

# Clang Frontend 库导读 — `clang/lib/Frontend/`

> 本文档梳理 `clang/lib/Frontend/` 目录下所有源文件 (35 个顶层
> .cpp/.h + 9 个 `Rewrite/` 子目录 .cpp/.h = 44 个) 的职责、上下游与
> 推荐阅读顺序。
>
> 目标读者: 想理解 **Clang 编译器前端** (编译流水线编排、命令行动作、
> AST 装配、预处理初始化、诊断输出、依赖/模块收集、源码改写) 的开发者,
> 以及要给 Clang 加新 FrontendAction / 新 DiagnosticConsumer / 新依赖
> 收集器 / 新改写操作的人。
>
> 所有路径相对 `clang/lib/Frontend/`。同名公开头文件位于
> `clang/include/clang/Frontend/` (如 `CompilerInstance.h`、
> `FrontendAction.h`、`ASTUnit.h`、`DiagnosticRenderer.h` 等)。
> 子目录 `Rewrite/` 编译为独立库 `clangRewriteFrontend`, 链接
> `clangAST/Basic/Edit/Frontend/Lex/Rewrite/Serialization`。

---

## §0. Clang Frontend 库在编译流水线中的位置

`clang/lib/Frontend` 是 Clang 的 **编译器前端协调层**, 负责:

1. 把 Driver 通过 `clang -cc1 ...` 传入的 argv 解析成 `CompilerInvocation`
   (含 LangOptions / TargetOptions / CodeGenOptions / PreprocessorOptions
   等所有 `*Options`);
2. 构造 `CompilerInstance`, 创建并装配 DiagnosticsEngine /
   SourceManager / Preprocessor / Sema / ASTContext / ASTConsumer 等
   子对象;
3. 通过 `FrontendAction::Execute` 流水线驱动 **ParseAST → Sema →
   CodeGen / ASTDump / PrintPreprocessed / RewriteObjC / ...** 任意一个
   目标操作;
4. 把 diagnostic 路由给可插拔的 `DiagnosticConsumer`
   (TextDiagnosticPrinter / SARIFDiagnosticPrinter /
   SerializedDiagnosticPrinter / VerifyDiagnosticConsumer / Chained);
5. 把预处理产物 / 依赖 / 模块 / 包含图等通过各种 OutputConsumer 输出
   到 raw_ostream / 文件 / make-style dependency file / GraphViz。

它在 Clang 内部的层次:

```
┌─────────────────────────────────────────────────────────┐
│ 用户: clang -cc1 -emit-obj -O2 foo.c                     │
│      clang -cc1 -ast-dump foo.c                          │
│      clang -cc1 -E foo.c                                 │
│      clang -cc1 -fixit-recompile foo.c                   │
└─────────────────────────────────────────────────────────┘
                │
                ▼
┌─────────────────────────────────────────────────────────┐
│ clang/lib/Frontend  (本目录) ← 你在这里                    │
│   · CompilerInstance + CompilerInvocation: 中央协调器   │
│     + 命令行解析 (覆盖所有 *Options 子对象)               │
│   · FrontendAction 框架: 生命周期 (BeginSourceFile →     │
│     Execute → EndSourceFile) + 标准 Action 集合          │
│   · ASTUnit / ASTMerge / MultiplexConsumer: 脱离驱动    │
│     进程的 AST 容器 + AST 合并 + 多个 consumer 扇出      │
│   · Diagnostic 链路: DiagnosticRenderer 基类 + 5 种     │
│     DiagnosticConsumer 实现 (Text/SARIF/Serialized/     │
│     Log/Verify/Chained)                                 │
│   · Preprocessor 初始化 + 输出: InitPreprocessor,        │
│     -E 输出, -H /showIncludes, -M/MM 依赖文件,         │
│     include graph (GraphViz), module deps, PCH preamble  │
│   · Rewrite/ 子库: clangRewriteFrontend (HTMLPrint,      │
│     FixIt, RewriteObjC / RewriteModernObjC,             │
│     RewriteMacros / Inclusion / RewriteTest)            │
└─────────────────────────────────────────────────────────┘
                │
                ▼
┌──────────────────────────────────────────────────────────────┐
│ 实际编译器工作:                                                │
│   · clang/lib/Basic  (诊断 + LangOptions + TargetInfo)        │
│   · clang/lib/Lex    (词法, Lexer + Preprocessor, 将字符流转为Token)             │
│   · clang/lib/Parse  (语法, Parser, 解析Token 创建 Stmt/Decl 节点，搭起 AST 骨架) │
│   · clang/lib/AST    (语法树, AST + ASTContext + Sema 上下文)  │
│   · clang/lib/Sema   (语义分析, 由 FrontendAction 触发, )      │
│                       遍历 Parser 生成的半成品 AST，做名字查找  │   
│                       类型检查、重载决议等，填充 AST 语义信息    │   
│   · clang/lib/CodeGen(LLVM IR 生成, EmitObj/EmitLLVM，        │
│                       将完整 AST 翻译成 LLVM IR)               │
│   · clang/lib/Serialization (PCH/Module 读写)                 │
│ Lex 将字符流转为Token; Parser语法解析Token 创建 Stmt/Decl 节点，搭起 AST 骨架
└──────────────────────────────────────────────────────────────┘
                │
                ▼
┌─────────────────────────────────────────────────────────┐
│ 下游产物:                                                │
│   · .o / .s / .ll / .bc / .ast / .pcm / .ifc / .html    │
│     / 改写后 C 源码 / .dia / .sarif / Make depfile       │
│   · 退出码 + 时间统计                                    │
└─────────────────────────────────────────────────────────┘
```

### §0.1 公开接口 (`clang/include/clang/Frontend/`)

本目录 `.cpp` 文件依赖的 **公开头** 在
[`clang/include/clang/Frontend/`](../../include/clang/Frontend/), 最关键
的:

- [`CompilerInstance.h`](../../include/clang/Frontend/CompilerInstance.h)
  — `class CompilerInstance` 中央协调器。
- [`CompilerInvocation.h`](../../include/clang/Frontend/CompilerInvocation.h)
  — `class CompilerInvocation` 不可变选项快照。
- [`FrontendAction.h`](../../include/clang/Frontend/FrontendAction.h)
  — `class FrontendAction` / `ASTFrontendAction` /
  `PluginASTAction` / `PreprocessorFrontendAction` /
  `WrapperFrontendAction` / `ASTMergeAction`。
- [`FrontendOptions.h`](../../include/clang/Frontend/FrontendOptions.h)
  — `FrontendOptions` / `InputKind` / `FrontendInputFile` /
  `ProgramAction`。
- [`ASTUnit.h`](../../include/clang/Frontend/ASTUnit.h) —
  `class ASTUnit` 脱离驱动的 AST 容器 (libclang 用)。
- [`ASTConsumers.h`](../../include/clang/Frontend/ASTConsumers.h) —
  标准 `ASTConsumer` 工厂: `CreateASTPrinter` /
  `CreateASTDumper` / `CreateASTDeclNodeLister` / `CreateASTViewer`。
- [`MultiplexConsumer.h`](../../include/clang/Frontend/MultiplexConsumer.h)
  — 多 consumer 扇出。
- [`Utils.h`](../../include/clang/Frontend/Utils.h) —
  `InitializePreprocessor` / `DoPrintPreprocessedInput` /
  `createChainedIncludesSource` / `createHeaderIncluderGen` 等工厂。
- [`DiagnosticRenderer.h`](../../include/clang/Frontend/DiagnosticRenderer.h)
  — `class DiagnosticRenderer` 渲染基类。
- [`TextDiagnostic.h`](../../include/clang/Frontend/TextDiagnostic.h) +
  [`TextDiagnosticPrinter.h`](../../include/clang/Frontend/TextDiagnosticPrinter.h)
  — 默认 human-readable 输出。
- [`SARIFDiagnostic.h`](../../include/clang/Frontend/SARIFDiagnostic.h)
  + [`SARIFDiagnosticPrinter.h`](../../include/clang/Frontend/SARIFDiagnosticPrinter.h)
  — SARIF JSON 输出。
- [`SerializedDiagnosticPrinter.h`](../../include/clang/Frontend/SerializedDiagnosticPrinter.h)
  + [`SerializedDiagnosticReader.h`](../../include/clang/Frontend/SerializedDiagnosticReader.h)
  — 跨进程的 `.dia` bitstream 诊断。
- [`LogDiagnosticPrinter.h`](../../include/clang/Frontend/LogDiagnosticPrinter.h)
  — plist 风格日志 (Xcode 消费)。
- [`VerifyDiagnosticConsumer.h`](../../include/clang/Frontend/VerifyDiagnosticConsumer.h)
  — `// expected-*` 验证。
- [`ChainedDiagnosticConsumer.h`](../../include/clang/Frontend/ChainedDiagnosticConsumer.h)
  — primary + secondary fan-out 适配器。
- [`StandaloneDiagnostic.h`](../../include/clang/Frontend/StandaloneDiagnostic.h)
  — SourceManager 销毁后回放诊断。
- [`TextDiagnosticBuffer.h`](../../include/clang/Frontend/TextDiagnosticBuffer.h)
  — diagnostic 暂存 buffer (Verify 用)。
- [`DependencyOutputOptions.h`](../../include/clang/Frontend/DependencyOutputOptions.h)
  + [`PreprocessorOutputOptions.h`](../../include/clang/Frontend/PreprocessorOutputOptions.h)
  — 各种输出选项。
- [`PrecompiledPreamble.h`](../../include/clang/Frontend/PrecompiledPreamble.h)
  — `class PrecompiledPreamble` 缓存 main file PCH。
- [`ChainedIncludesSource.h`](../../include/clang/Frontend/ChainedIncludesSource.h)
  — 把 include 链上的头做成内存 PCH。
- [`LayoutOverrideSource.h`](../../include/clang/Frontend/LayoutOverrideSource.h)
  — `-foverride-record-layout` 测试钩子。
- [`ModuleFileExtension.h`](../../include/clang/Frontend/ModuleFileExtension.h)
  — 自定义 PCM 块读写扩展点。
- [`PCHContainerOperations.h`](../../include/clang/Frontend/PCHContainerOperations.h)
  — `.pch` 容器包装/解包。

### §0.2 与 [`clang/lib/Driver/`](../Driver/) 的关系

Driver 通过 [`ToolChains/Clang.cpp`](../Driver/ToolChains/Clang.cpp) 把
所有 driver flag 渲染为 `clang -cc1 ...` argv, 然后
`Compilation::ExecuteJobs` 调 fork+exec 启动 cc1 子进程, 子进程内
`ExecuteCC1Tool.cpp` (实为
[`clang/tools/clang/driver.cpp`](../../tools/clang/driver.cpp) 调用
`clang::executeCC1Tool`) 解析 argv, 构造 `CompilerInvocation` + `CompilerInstance`,
按 `frontend::ActionKind` 选 `FrontendAction`, 调
`FrontendAction::Execute`。Driver 的 argv 字符串 **直接是本目录的输入**。

### §0.3 与 `clang/lib/Lex` / `clang/lib/Parse` / `clang/lib/AST` / `clang/lib/Sema` / `clang/lib/CodeGen` 的关系

本目录 **编排** 但 **不实现** 编译流水线本身。`CompilerInstance`
在 `ExecuteAction` 中按需创建并持有 Lexer/Preprocessor (来自
[`clang/lib/Lex/`](../Lex/))、Parser (来自
[`clang/lib/Parse/`](../Parse/))、Sema (来自
[`clang/lib/Sema/`](../Sema/))、ASTContext + ASTConsumer (来自
[`clang/lib/AST/`](../AST/))。对于 `EmitObj` / `EmitLLVM` / `EmitBC` /
`EmitAssembly`, 由 `CodeGenAction` (在
[`clang/lib/CodeGen/`](../CodeGen/) 中) 提供 `ASTConsumer` 子类;
对于 `PrintPreprocessed` / `InitOnly` / `ParseSyntaxOnly` /
`GeneratePCH` / `GenerateModule`, 由本目录的 `FrontendActions.cpp` 提供
对应 `ASTConsumer` 或 free function; 对于 `FixIt` / `HTMLPrint` /
`RewriteObjC`, 由 `Rewrite/` 子目录提供。

### §0.4 与 [`clang/lib/Basic/`](../Basic/) 的关系

所有 `DiagnosticConsumer` / `DiagnosticRenderer` / `DiagnosticOptions`
来自 [`clang/lib/Basic/`](../Basic/)。`LangOptions` /
`TargetOptions` / `CodeGenOptions` / `FileSystemOptions` /
`SourceManager` 的核心定义也在 Basic。本目录只在
`CompilerInstance::createDiagnostics` / `createFileManager` /
`createSourceManager` 等工厂方法中装配它们。

### §0.5 与 libclang / clangd / Tooling 的关系

[`ASTUnit.cpp`](ASTUnit.cpp) 是 libclang (在
[`clang/tools/libclang/`](../../tools/libclang/)) / `clangd` /
[`clang/lib/Tooling/`](../Tooling/) 用的核心 ——
`ASTUnit::LoadFromCompilerInvocation` / `LoadFromASTFile` 把
`FrontendAction` 跑完后的 AST/PCH 容器 **脱离** `CompilerInstance`
生命周期保留下来, 供 IDE 长期持有 + 增量解析。

### §0.6 与 `clang/include/clang/Frontend/CompilerInvocation.h` 的关系

所有 `FrontendAction` 在 `ExecuteAction` 第一步都依赖
`CompilerInstance::getInvocation()` 取不可变 argv 快照, 把其中
`LangOptions` / `TargetOptions` / `HeaderSearchOptions` 传给 Lexer /
Preprocessor / Sema 子对象。

### §0.7 与 [`clang/lib/FrontendTool/`](../FrontendTool/) 的关系

`clang/lib/FrontendTool/` 是从 `clang/lib/Frontend/` 拆出的独立子
库 (单文件, 编译为 `clangFrontendTool`), 仅含 `clang-cc1` 驱动入
口 `ExecuteCompilerInvocation.cpp`。拆分的目的是 **最小化 `clangFrontend`
的依赖**: `Frontend/` 不能依赖 `clangCodeGen` / `clangStaticAnalyzer`
/ `clangExtractAPI` / `clangScalableStaticAnalysis` / `clangCIR` 等具体
FrontendAction 实现, 而 `clangFrontendTool/` 把这些 FrontendAction 的
构造 (经 `CreateFrontendAction`) 集中到一处, 让 `clang/tools/clang/driver.cpp`
通过 `clang::ExecuteCompilerInvocation` 调用, 同时让 `clang-tidy` /
`clang-refactor` 等 libclang-style 工具可以 **不** 链 clangFrontendTool
就能跑 Frontend 内部组件。

它在 Clang 内部的层次:

```
┌─────────────────────────────────────────────────────────┐
│ clang/tools/clang/driver.cpp (clang_main → executeCC1Tool)│
│  · 收到 -cc1 argv                                          │
│  · new CompilerInvocation::CreateFromArgs                  │
│  · new CompilerInstance                                    │
│  · clang::ExecuteCompilerInvocation (经 clangFrontendTool) │
└─────────────────────────────────────────────────────────┘
                │
                ▼
┌─────────────────────────────────────────────────────────┐
│ clang/lib/FrontendTool/ (本目录的"驱动壳")                │
│   · ExecuteCompilerInvocation.cpp:                         │
│      ├─ CreateFrontendAction (frontend::ActionKind →       │
│      │    具体 FrontendAction 子类)                         │
│      │    ├─ EmitObj/EmitLLVMAction → clang/lib/CodeGen/    │
│      │    ├─ PrintPreprocessedAction → 本目录 FrontendActions│
│      │    ├─ FixItAction → 本目录 Rewrite/FrontendActions  │
│      │    ├─ ExtractAPIAction → clang/lib/ExtractAPI/      │
│      │    ├─ SSAF SourceTransformationFrontendAction →      │
│      │    │    clang/lib/ScalableStaticAnalysis/            │
│      │    └─ CIRGenAction → clang/lib/CIR/                 │
│      ├─ loadPlugins (FrontendPluginRegistry)               │
│      ├─ 装 ExecutePluginAction / AddWrapperActions /      │
│      │    AddPluginActions                                  │
│      └─ 调 CI.ExecuteAction()                              │
│      (注: CompilerInvocation / FrontendOptions / 基础        │
│       DiagnosticRenderer 等来自本目录 clang/lib/Frontend/) │
└─────────────────────────────────────────────────────────┘
                │
                ▼
本目录 (clang/lib/Frontend/) 的 全部 组件被 ExecuteCompilerInvocation
用, 但 ExecuteCompilerInvocation 本人不属于本目录。
```

### §0.8 与其他 lib 库的关系

| 上游消费者 | 通过什么接口调本目录 |
|------------|---------------------|
| [`clang/lib/FrontendTool/`](../FrontendTool/) | `ExecuteCompilerInvocation` → `CreateFrontendAction` → `CI.ExecuteAction` |
| [`clang/lib/Interpreter/`](../../lib/Interpreter/) | `IncrementalAction` (继承 `WrapperFrontendAction`) → `CI.ExecuteAction` |
| [`tools/clang-check/ClangCheck.cpp`](../../tools/clang-check/ClangCheck.cpp) | `ClangCheckAction` 派生自 `ASTFrontendAction` |
| `libclang` (`CIndex.cpp`) / `clangd` | `ASTUnit::LoadFromCompilerInvocation` (见 §0.5) |

---

## §1. 编译流水线概览

```
                            clang -cc1 -emit-obj foo.c
                                      │
                                      ▼
┌─────────────────────────────────────────────────────────┐
│ ExecuteCC1Tool (clang/tools/clang/driver.cpp)            │
│   · parse args → CompilerInvocation                     │
│   · 选 FrontendAction (frontend::ActionKind lookup)      │
│   · 构造 CompilerInstance                                │
└─────────────────────────────────────────────────────────┘
                │
                ▼
┌─────────────────────────────────────────────────────────┐
│ CompilerInstance::ExecuteAction (CompilerInstance.cpp)   │
│   ├─ createDiagnostics (DiagnosticOptions 解析)         │
│   ├─ createFileManager + createSourceManager             │
│   ├─ createTargetInfo + createAuxTarget                  │
│   ├─ createPreprocessor (调 InitializePreprocessor 填    │
│   │   全部默认 builtin 宏)                              │
│   ├─ createASTContext + createSema (CreateSema)         │
│   └─ createDefaultOutputFile (-o / -S / -E 输出)        │
└─────────────────────────────────────────────────────────┘
                │
                ▼
┌─────────────────────────────────────────────────────────┐
│ FrontendAction::Execute (FrontendAction.cpp)             │
│   ├─ BeginSourceFile (CompilerInstance 装配完成)        │
│   ├─ ExecuteAction (派生类)                              │
│   │    ├─ ParseAST (ParseAST.cpp in clang/lib/Parse)    │
│   │    │    └─ Sema::ActOnTranslationUnit                │
│   │    │         └─ ASTConsumer::HandleTranslationUnit    │
│   │    │              ├─ CodeGenAction → EmitLLVM/EmitObj │
│   │    │              ├─ ASTDumpAction → ASTDumper      │
│   │    │              ├─ ASTPrintAction → ASTPrinter    │
│   │    │              ├─ GeneratePCHAction → ASTWriter  │
│   │    │              ├─ HTMLPrintAction → HTMLPrinter  │
│   │    │              ├─ FixItAction → FixItRewriter    │
│   │    │              └─ PrintPreprocessedAction →       │
│   │    │                   DoPrintPreprocessedInput     │
│   │    └─ (或 PreprocessorFrontendAction 直接跑 PP)     │
│   └─ EndSourceFile (reset + 释放)                       │
└─────────────────────────────────────────────────────────┘
                │
                ▼
┌─────────────────────────────────────────────────────────┐
│ 旁路 (并行挂载):                                         │
│   · DiagnosticConsumers: TextDiagnosticPrinter 默认;    │
│     SARIF / Serialized / Verify / Log / Chained 可叠加   │
│   · DependencyFileGenerator: -M/-MD/-MMD depfile        │
│   · HeaderIncludeGen: -H / /showIncludes / JSON include │
│   · ModuleDependencyCollector: 构建 module 时收集 PCM 依 │
│   · DependencyGraphPrinter: GraphViz .dot include graph │
│   · PrecompiledPreamble: 缓存 main file PCH 给 IDE 复用 │
└─────────────────────────────────────────────────────────┘
                │
                ▼
┌─────────────────────────────────────────────────────────┐
│ 输出: .o / .bc / .ll / .s / .ast / .pcm / .ifc / .html    │
│       / 改写后 C 源码 / .dia / .sarif / Make depfile /    │
│       -E 输出 / -H include 列表                          │
└─────────────────────────────────────────────────────────┘
```

---

## §2. 文件目录结构

```
clang/lib/Frontend/  (44 文件, 1 子目录 Rewrite/)
├── §3.1  Compiler orchestration (Compiler/Invocation/Options)  (4 文件)
├── §3.2  FrontendAction framework + 标准 Action                 (3 文件)
├── §3.3  AST utilities (Unit/Merge/Consumers/Multiplex/...)     (8 文件)
├── §3.4  Diagnostic 链路 (Renderer + 5 种 Consumer)             (12 文件)
├── §3.5  Preprocessor 初始化 + 输出 + 依赖                      (5 文件)
├── §3.6  Module dependency + PCH preamble                       (2 文件)
├── §3.7  杂项 (TestModuleFileExtension + FrontendOptions)      (3 文件)
└── §3.8  Rewrite/ 子库 (clangRewriteFrontend, 8 文件 + 1 build)
```

注: 本目录不含 `clang-cc1` 驱动入口 ——
[`clang/lib/FrontendTool/ExecuteCompilerInvocation.cpp`](../FrontendTool/ExecuteCompilerInvocation.cpp)
作为独立子库 `clangFrontendTool` 提供, 详见 §0.7。

### 2.1 文件数量统计

| 区域 | .cpp | .h | 合计 |
|------|------|-----|------|
| Compiler orchestration | 4 | 0 | 4 |
| FrontendAction framework + 标准 Action | 3 | 0 | 3 |
| AST utilities | 8 | 0 | 8 |
| Diagnostic 链路 | 12 | 0 | 12 |
| Preprocessor init / output / dep | 5 | 0 | 5 |
| Module dependency + PCH | 2 | 0 | 2 |
| 杂项 | 1 | 1 | 2 + 1 CMakeLists |
| **顶层小计** | **~35** | **~1** | **~36** + `CMakeLists.txt` |
| Rewrite/ 子库 | 8 | 0 | 8 + 1 CMakeLists |
| **总计** | **~43** | **~1** | **~44** + 2 × `CMakeLists.txt` |

### 2.2 姊妹子库 [`clang/lib/FrontendTool/`](../FrontendTool/) 文件清单

仅 1 个 .cpp + 1 个公开头 + 1 个 `CMakeLists.txt`, 编译为独立
库 `clangFrontendTool`:

| 文件 | 作用 |
|------|------|
| [`FrontendTool/ExecuteCompilerInvocation.cpp`](../FrontendTool/ExecuteCompilerInvocation.cpp) | `clang-cc1` 驱动入口: `CreateFrontendAction` + `ExecuteCompilerInvocation` |
| [`FrontendTool/Utils.h`](../FrontendTool/Utils.h) | `CreateFrontendAction` / `ExecuteCompilerInvocation` 自由函数声明 |
| `FrontendTool/CMakeLists.txt` | `clangFrontendTool` 库定义 |

详见 §3.9 与 §4.1。

---

## §3. 文件详解

### 3.1 Compiler orchestration

[`CompilerInstance.cpp`](CompilerInstance.cpp) — 实现
`CompilerInstance`: 持有 Invocation / Diagnostics / VFS / Target /
AuxTarget / SourceManager / Preprocessor / Sema / ASTContext /
ASTConsumer / PCHContainer 等所有子对象, 提供 `create*` 工厂方法,
负责它们创建/释放/初始化; 同时实现 `ModuleLoader`, 提供
`createDefaultOutputFile` 等便捷入口。
- 上游:
  [`clang/lib/FrontendTool/ExecuteCompilerInvocation.cpp`](../FrontendTool/ExecuteCompilerInvocation.cpp),
  [`tools/clang-check/ClangCheck.cpp`](../../tools/clang-check/ClangCheck.cpp),
  [`clang/lib/Interpreter/`](../../lib/Interpreter/)。
- 下游: [`CompilerInstance.h`](../../include/clang/Frontend/CompilerInstance.h),
  [`FrontendAction.h`](../../include/clang/Frontend/FrontendAction.h),
  [`Serialization/ASTReader.h`](../../include/clang/Serialization/ASTReader.h)。
- 关键类/函数: `class CompilerInstance`, `createDiagnostics`,
  `createFileManager`, `createSourceManager`, `createPreprocessor`,
  `createSema`, `createASTContext`, `ExecuteAction`,
  `createDefaultOutputFile`。

[`CompilerInvocation.cpp`](CompilerInvocation.cpp) — 解析 `-cc1` 命令
行并填好 LangOptions / TargetOptions / CodeGenOptions /
PreprocessorOptions / HeaderSearchOptions / DependencyOutputOptions /
FrontendOptions 等所有 `*Options` 子对象; 提供 `RoundTrip` 把
`CompilerInvocation` 反向渲染回 argv (调试用)。
- 上游:
  [`clang/lib/FrontendTool/ExecuteCompilerInvocation.cpp`](../FrontendTool/ExecuteCompilerInvocation.cpp),
  [`clang/lib/Driver/Job.cpp`](../Driver/Job.cpp)。
- 下游: [`CompilerInvocation.h`](../../include/clang/Frontend/CompilerInvocation.h),
  [`Options/Options.h`](../../include/clang/Options/Options.h),
  [`Serialization/ASTBitCodes.h`](../../include/clang/Serialization/ASTBitCodes.h)。
- 关键类/函数: `class CompilerInvocation`, `CreateFromArgs`,
  `GenerateCC1CommandLine`, `ParseDiagnosticArgs`,
  `getOptimizationLevel`, `RoundTrip`。

[`FrontendOptions.cpp`](FrontendOptions.cpp) — `FrontendOptions` 补
充实现, 主要是 `getInputKindForExtension` 扩展名 → `InputKind` 映射
表 (`.c` → C, `.cpp` → CXX, `.m` → ObjC, `.mm` → ObjCXX, `.cppm`
→ CXXModule, ...)。
- 上游: [`CompilerInstance.cpp`](CompilerInstance.cpp),
  [`FrontendTool/`](../FrontendTool/)。
- 下游: [`FrontendOptions.h`](../../include/clang/Frontend/FrontendOptions.h),
  [`Basic/LangStandard.h`](../../include/clang/Basic/LangStandard.h)。
- 关键类/函数: `FrontendOptions::getInputKindForExtension`,
  `InputKind`。

[`CMakeLists.txt`](CMakeLists.txt) — 声明 `clangFrontend` 库源文件
列表 + 链接 `clangAST/Basic/Lex/Parse/Sema/Serialization/Edit/APINotes`。

### 3.2 FrontendAction framework + 标准 Action

[`FrontendAction.cpp`](FrontendAction.cpp) — 实现
`FrontendAction` / `ASTFrontendAction` / `PluginASTAction` /
`PreprocessorFrontendAction` / `WrapperFrontendAction` 五个基类的生
命周期回调, 串起 `BeginSourceFile` → `Execute` → `EndSourceFile` 流
水线; 处理 plugin 加载、ChainedIncludes 注入、ASTMerge 适配、
LayoutOverride 装配。
- 上游: [`clang/lib/CodeGen/CodeGenAction.cpp`](../CodeGen/CodeGenAction.cpp),
  [`clang/lib/FrontendTool/ExecuteCompilerInvocation.cpp`](../FrontendTool/ExecuteCompilerInvocation.cpp),
  [`tools/clang-check`](../../tools/clang-check/)。
- 下游:
  [`CompilerInstance.h`](../../include/clang/Frontend/CompilerInstance.h),
  [`MultiplexConsumer.h`](../../include/clang/Frontend/MultiplexConsumer.h),
  [`LayoutOverrideSource.h`](../../include/clang/Frontend/LayoutOverrideSource.h),
  [`Frontend/Utils.h`](../../include/clang/Frontend/Utils.h)。
- 关键类/函数: `FrontendAction`, `ASTFrontendAction`,
  `PluginASTAction`, `PreprocessorFrontendAction`,
  `WrapperFrontendAction`, `ASTMergeAction`,
  `CreateWrappedASTConsumer`, `FrontendPluginRegistry`。

[`FrontendActions.cpp`](FrontendActions.cpp) — 标准 `-cc1` Action 实
现: `InitOnlyAction` / `ReadPCHAndPreprocessAction` / `ASTPrintAction`
/ `ASTDumpAction` / `ASTDeclListAction` / `ASTViewAction` /
`GeneratePCHAction` / `GenerateModuleAction` /
`ParseSyntaxOnlyAction` / `PrintPreambleAction` /
`VerifyPCHAction` / `GenerateInterfaceStubsAction` / `MigrateSourceAction`
等。每个 Action 提供自己的 `CreateASTConsumer` + 必要时覆盖
`ExecuteAction`。
- 上游:
  [`clang/lib/FrontendTool/ExecuteCompilerInvocation.cpp`](../FrontendTool/ExecuteCompilerInvocation.cpp),
  [`clang/lib/Interpreter/`](../../lib/Interpreter/)。
- 下游:
  [`CompilerInstance.h`](../../include/clang/Frontend/CompilerInstance.h),
  [`MultiplexConsumer.h`](../../include/clang/Frontend/MultiplexConsumer.h),
  [`ASTConsumers.h`](../../include/clang/Frontend/ASTConsumers.h),
  [`Serialization/ASTWriter.h`](../../include/clang/Serialization/ASTWriter.h)。
- 关键类/函数: `InitOnlyAction`, `ReadPCHAndPreprocessAction`,
  `ASTPrintAction`, `ASTDumpAction`, `ASTDeclListAction`,
  `ASTViewAction`, `GeneratePCHAction`, `GenerateModuleAction`,
  `ParseSyntaxOnlyAction`, `PrintPreambleAction`, `VerifyPCHAction`,
  `GenerateInterfaceStubsAction`。

[`ASTConsumers.cpp`](ASTConsumers.cpp) — 实现一组通用
`ASTConsumer`: `ASTPrinter` (再打印回 C/C++)、`ASTDumper`、
`ASTDeclNodeLister`、`ASTViewer`; 给 `ASTPrintAction` /
`ASTDumpAction` / `ASTDeclListAction` / `ASTViewAction` 用。
- 上游: [`FrontendActions.cpp`](FrontendActions.cpp),
  [`Rewrite/FrontendActions.cpp`](Rewrite/FrontendActions.cpp)。
- 下游: [`ASTConsumers.h`](../../include/clang/Frontend/ASTConsumers.h),
  [`RecursiveASTVisitor.h`](../../include/clang/AST/RecursiveASTVisitor.h),
  [`PrettyPrinter.h`](../../include/clang/AST/PrettyPrinter.h)。
- 关键类/函数: `CreateASTPrinter`, `CreateASTDumper`,
  `CreateASTDeclNodeLister`, `CreateASTViewer`, `ASTPrinter`。

### 3.3 AST utilities

[`ASTUnit.cpp`](ASTUnit.cpp) — 实现 `ASTUnit`: libclang / 交叉 TU /
CSA 用, `LoadFromASTFile` / `LoadFromCompilerInvocation` 把 AST 加载或
完整跑 `-cc1` 后取出 AST / PCH 容器; 内部保留 PreprocessingRecord +
StandaloneDiagnostic 用于脱离 SourceManager 生命周期。
- 上游: [`tools/libclang/`](../../tools/libclang/),
  [`lib/CrossTU/CrossTranslationUnit.cpp`](../CrossTU/CrossTranslationUnit.cpp),
  [`lib/Tooling/Tooling.cpp`](../Tooling/Tooling.cpp)。
- 下游:
  [`CompilerInvocation.h`](../../include/clang/Frontend/CompilerInvocation.h),
  [`PrecompiledPreamble.h`](../../include/clang/Frontend/PrecompiledPreamble.h),
  [`StandaloneDiagnostic.h`](../../include/clang/Frontend/StandaloneDiagnostic.h),
  [`Serialization/ASTReader.h`](../../include/clang/Serialization/ASTReader.h)。
- 关键类/函数: `ASTUnit`, `LoadFromASTFile`,
  `LoadFromCompilerInvocation`, `Save`, `isMainFileAST`,
  `CleanUpOnBorrowActions`, `StoredDiagnostics`。

[`ASTMerge.cpp`](ASTMerge.cpp) — `ASTMergeAction`: 通过 `ASTImporter`
把若干 `.ast` 文件并入当前翻译单元, 适配任意被包裹 action。
- 上游: [`FrontendAction.cpp`](FrontendAction.cpp),
  [`FrontendTool/ExecuteCompilerInvocation.cpp`](../FrontendTool/ExecuteCompilerInvocation.cpp)。
- 下游: [`ASTImporter.h`](../../include/clang/AST/ASTImporter.h),
  [`ASTImporterSharedState.h`](../../include/clang/AST/ASTImporterSharedState.h),
  [`FrontendActions.h`](../../include/clang/Frontend/FrontendActions.h)。
- 关键类/函数: `ASTMergeAction`, `AdaptedAction`,
  `ForwardingDiagnosticConsumer`。

[`MultiplexConsumer.cpp`](MultiplexConsumer.cpp) —
`MultiplexConsumer` / `MultiplexASTMutationListener` /
`MultiplexASTDeserializationListener` 实现 —— 把 `ASTConsumer` 调用
**扇出** 到多个子 consumer。
- 上游: [`clang/lib/CodeGen/CodeGenAction.cpp`](../CodeGen/CodeGenAction.cpp),
  [`clang/lib/Interpreter/IncrementalAction.cpp`](../../lib/Interpreter/IncrementalAction.cpp),
  [`clang/lib/ScalableStaticAnalysis/Frontend`](../ScalableStaticAnalysis/Frontend)。
- 下游:
  [`MultiplexConsumer.h`](../../include/clang/Frontend/MultiplexConsumer.h),
  [`ASTMutationListener.h`](../../include/clang/AST/ASTMutationListener.h)。
- 关键类/函数: `MultiplexConsumer`,
  `MultiplexASTMutationListener`,
  `MultiplexASTDeserializationListener`。

[`ChainedIncludesSource.cpp`](ChainedIncludesSource.cpp) —
`createChainedIncludesSource`: 把 `#include` 链上的头预编译成内存
PCH, 仅 `-chained-include` 测试用。
- 上游: [`FrontendAction.cpp`](FrontendAction.cpp),
  [`lib/Tooling`](../Tooling/) (测试路径)。
- 下游: [`CompilerInstance.h`](../../include/clang/Frontend/CompilerInstance.h),
  [`TextDiagnosticPrinter.h`](../../include/clang/Frontend/TextDiagnosticPrinter.h),
  [`Serialization/ASTWriter.h`](../../include/clang/Serialization/ASTWriter.h)。
- 关键类/函数: `ChainedIncludesSource`, `createChainedIncludesSource`。

[`InterfaceStubFunctionsConsumer.cpp`](InterfaceStubFunctionsConsumer.cpp)
— 为 `GenerateInterfaceStubsAction` 实现 `ASTConsumer`: 仅导出
default visibility 符号, 生成 ELF `.ifc` 桩文件。
- 上游: [`FrontendActions.cpp`](FrontendActions.cpp)
  (`GenerateInterfaceStubsAction`),
  [`FrontendTool/ExecuteCompilerInvocation.cpp`](../FrontendTool/ExecuteCompilerInvocation.cpp)。
- 下游: [`Mangle.h`](../../include/clang/AST/Mangle.h),
  [`RecursiveASTVisitor.h`](../../include/clang/AST/RecursiveASTVisitor.h),
  [`llvm/BinaryFormat/ELF.h`](../../../llvm/include/llvm/BinaryFormat/ELF.h)。
- 关键类/函数: `InterfaceStubFunctionsConsumer`,
  `GenerateInterfaceStubsAction`, `MangledSymbol`。

[`LayoutOverrideSource.cpp`](LayoutOverrideSource.cpp) —
`LayoutOverrideSource` 实现 —— `ExternalASTSource`, 读
`-fdump-record-layouts` 输出并强制覆盖 record layout, 用于测试代
码生成布局。
- 上游: [`FrontendAction.cpp`](FrontendAction.cpp)
  (`OverrideRecordLayouts`)。
- 下游: [`LayoutOverrideSource.h`](../../include/clang/Frontend/LayoutOverrideSource.h),
  [`DeclCXX.h`](../../include/clang/AST/DeclCXX.h)。
- 关键类/函数: `LayoutOverrideSource`, `Layout`,
  `layoutRecordType`。

[`TestModuleFileExtension.h`](TestModuleFileExtension.h) —
`TestModuleFileExtension` 声明: 用于测试 `ModuleFileExtension` 自
定义块读写回路的最小 `ModuleFileExtension` 子类。
- 上游: [`CompilerInvocation.cpp`](CompilerInvocation.cpp) (内含
  引用),
  [`clang/unittests/Serialization/`](../../unittests/Serialization/)。
- 下游: [`ModuleFileExtension.h`](../../include/clang/Frontend/ModuleFileExtension.h),
  [`llvm/Bitstream/BitstreamReader.h`](../../../llvm/include/llvm/Bitstream/BitstreamReader.h)。
- 关键类/函数: `TestModuleFileExtension`,
  `TestModuleFileExtension::Writer`, `TestModuleFileExtension::Reader`,
  `getExtensionMetadata`, `createExtensionReader`。

[`TestModuleFileExtension.cpp`](TestModuleFileExtension.cpp) —
`TestModuleFileExtension` 实现: 写/读一个 'Hello from ...' 的扩
展块, 验证 `ModuleFileExtension` ABI。
- 上游: [`CompilerInvocation.cpp`](CompilerInvocation.cpp),
  [`clang/unittests/Serialization/`](../../unittests/Serialization/)。
- 下游: [`TestModuleFileExtension.h`](TestModuleFileExtension.h),
  [`Serialization/ASTReader.h`](../../include/clang/Serialization/ASTReader.h),
  [`llvm/Bitstream/BitstreamWriter.h`](../../../llvm/include/llvm/Bitstream/BitstreamWriter.h)。
- 关键类/函数: `TestModuleFileExtension::Writer::writeExtensionContents`,
  `TestModuleFileExtension::Reader::Reader`, `hashExtension`。

### 3.4 Diagnostic 链路

[`DiagnosticRenderer.cpp`](DiagnosticRenderer.cpp) —
`DiagnosticRenderer` 基类实现 —— 处理宏展开 / include stack /
import stack / 模板实例栈并把 caret / range / fixit 拼到 emitter 上。
- 上游: [`TextDiagnostic.cpp`](TextDiagnostic.cpp),
  [`SARIFDiagnostic.cpp`](SARIFDiagnostic.cpp),
  [`SerializedDiagnosticPrinter.cpp`](SerializedDiagnosticPrinter.cpp)。
- 下游: [`DiagnosticRenderer.h`](../../include/clang/Frontend/DiagnosticRenderer.h),
  [`EditedSource.h`](../../include/clang/Edit/EditedSource.h),
  [`Lexer.h`](../../include/clang/Lex/Lexer.h)。
- 关键类/函数: `DiagnosticRenderer`, `emitDiagnostic`,
  `emitIncludeStack`, `emitMacroExpansions`, `emitCaret`,
  `getExpansionRangeInFile`。

[`TextDiagnostic.cpp`](TextDiagnostic.cpp) — `TextDiagnostic` 实现
—— 把 `DiagnosticRenderer` 渲染成带 caret / 波浪线 / fixit 高亮 /
颜色的人可读输出。
- 上游: [`TextDiagnosticPrinter.cpp`](TextDiagnosticPrinter.cpp),
  [`tools/libclang`](../../tools/libclang/)。
- 下游: [`TextDiagnostic.h`](../../include/clang/Frontend/TextDiagnostic.h),
  [`DiagnosticRenderer.h`](../../include/clang/Frontend/DiagnosticRenderer.h)。
- 关键类/函数: `TextDiagnostic`, `printDiagnosticLevel`,
  `printDiagnosticMessage`, `printNote`, `printFixIt`。

[`TextDiagnosticPrinter.cpp`](TextDiagnosticPrinter.cpp) —
`TextDiagnosticPrinter` 实现 —— **默认** `DiagnosticConsumer`, 把
diagnostic 输出到 stderr (或任意 `raw_ostream`), 支持 `setPrefix`。
- 上游: [`lib/Tooling/Tooling.cpp`](../Tooling/Tooling.cpp),
  [`lib/Tooling/Refactoring.cpp`](../Tooling/Refactoring.cpp),
  [`lib/CrossTU/CrossTranslationUnit.cpp`](../CrossTU/CrossTranslationUnit.cpp),
  [`lib/Frontend/ChainedIncludesSource.cpp`](ChainedIncludesSource.cpp)。
- 下游: [`TextDiagnostic.h`](../../include/clang/Frontend/TextDiagnostic.h),
  [`TextDiagnosticPrinter.h`](../../include/clang/Frontend/TextDiagnosticPrinter.h)。
- 关键类/函数: `TextDiagnosticPrinter`, `HandleDiagnostic`,
  `setPrefix`, `TextDiagnostic`。

[`TextDiagnosticBuffer.cpp`](TextDiagnosticBuffer.cpp) —
`TextDiagnosticBuffer` 实现 —— 把所有 diagnostic 暂存, 提供
`FlushDiagnostics` 把它们再喂回 `DiagnosticsEngine`。
- 上游: [`VerifyDiagnosticConsumer.cpp`](VerifyDiagnosticConsumer.cpp)。
- 下游: [`TextDiagnosticBuffer.h`](../../include/clang/Frontend/TextDiagnosticBuffer.h)。
- 关键类/函数: `TextDiagnosticBuffer`, `FlushDiagnostics`, `Errors`,
  `Warnings`, `Notes`, `Remarks`。

[`ChainedDiagnosticConsumer.cpp`](ChainedDiagnosticConsumer.cpp) —
为 `ChainedDiagnosticConsumer` 提供 **RTTI anchor**
(`KeyFunction` 防止 vtable 被丢)。
- 上游: [`CompilerInstance.cpp`](CompilerInstance.cpp)。
- 下游: [`ChainedDiagnosticConsumer.h`](../../include/clang/Frontend/ChainedDiagnosticConsumer.h)。
- 关键类/函数: `ChainedDiagnosticConsumer::anchor`。

[`LogDiagnosticPrinter.cpp`](LogDiagnosticPrinter.cpp) —
`LogDiagnosticPrinter` 实现 —— 输出 `.dia` plist 风格日志, 供
Xcode / IDE 解析 diagnostic 流。
- 上游: [`CompilerInstance.cpp`](CompilerInstance.cpp)。
- 下游: [`LogDiagnosticPrinter.h`](../../include/clang/Frontend/LogDiagnosticPrinter.h),
  [`Basic/PlistSupport.h`](../../include/clang/Basic/PlistSupport.h)。
- 关键类/函数: `LogDiagnosticPrinter`, `EmitDiagEntry`,
  `DiagEntry`, `HandleDiagnostic`, `setDwarfDebugFlags`。

[`SARIFDiagnostic.cpp`](SARIFDiagnostic.cpp) — `SARIFDiagnostic` 实
现 —— `DiagnosticRenderer` 子类, 把 diagnostic 转成 SARIF object
(含位置 / fixit / related)。
- 上游: [`SARIFDiagnosticPrinter.cpp`](SARIFDiagnosticPrinter.cpp)。
- 下游: [`SARIFDiagnostic.h`](../../include/clang/Frontend/SARIFDiagnostic.h),
  [`Basic/Sarif.h`](../../include/clang/Basic/Sarif.h)。
- 关键类/函数: `SARIFDiagnostic`, `emitDiagnosticMessage`,
  `emitDiagnosticLoc`, `emitCodeContext`。

[`SARIFDiagnosticPrinter.cpp`](SARIFDiagnosticPrinter.cpp) —
`SARIFDiagnosticPrinter` 实现 —— 输出符合 SARIF 规范的 JSON
diagnostic 文档 (供 GitHub / CodeQL 等消费)。
- 上游: [`CompilerInstance.cpp`](CompilerInstance.cpp)。
- 下游: [`SARIFDiagnosticPrinter.h`](../../include/clang/Frontend/SARIFDiagnosticPrinter.h),
  [`SARIFDiagnostic.h`](../../include/clang/Frontend/SARIFDiagnostic.h)。
- 关键类/函数: `SARIFDiagnosticPrinter`, `hasSarifWriter`,
  `getSarifWriter`, `createDocument`, `SarifDocumentWriter`。

[`SerializedDiagnosticPrinter.cpp`](SerializedDiagnosticPrinter.cpp)
— `serialized_diags::create`: 把 diagnostic 写入 `.dia` 二进制
bitstream (clang 跨进程传递诊断的标准格式)。
- 上游: [`CompilerInstance.cpp`](CompilerInstance.cpp)。
- 下游: [`SerializedDiagnosticPrinter.h`](../../include/clang/Frontend/SerializedDiagnosticPrinter.h),
  [`SerializedDiagnosticReader.h`](../../include/clang/Frontend/SerializedDiagnosticReader.h),
  [`llvm/Bitstream/BitCodes.h`](../../../llvm/include/llvm/Bitstream/BitCodes.h)。
- 关键类/函数: `serialized_diags::create`,
  `SerializedDiagnosticPrinter`, `AbbreviationMap`, `EmitDiag`。

[`SerializedDiagnosticReader.cpp`](SerializedDiagnosticReader.cpp)
— `SerializedDiagnosticReader::readDiagnostics`: 解码 `.dia`
bitstream, 触发 `visit*` 回调 (供 clang-repl / IDE 端解析)。
- 上游: [`lib/Tooling`](../Tooling/),
  [`tools (clang -serialize-diags 之类)`](../../tools/)。
- 下游: [`SerializedDiagnosticReader.h`](../../include/clang/Frontend/SerializedDiagnosticReader.h),
  [`SerializedDiagnostics.h`](../../include/clang/Frontend/SerializedDiagnostics.h),
  [`llvm/Bitstream/BitstreamReader.h`](../../../llvm/include/llvm/Bitstream/BitstreamReader.h)。
- 关键类/函数: `SerializedDiagnosticReader`, `readDiagnostics`,
  `readMetaBlock`, `readDiagnosticBlock`, `SDError`,
  `visitDiagnosticRecord`。

[`StandaloneDiagnostic.cpp`](StandaloneDiagnostic.cpp) —
`StandaloneDiagnostic` 实现 —— 把 `StoredDiagnostic` 序列化到与
`SourceManager` 无关的形态, 供 `SourceManager` 销毁后回放 (例如
deferred diagnostics)。
- 上游: [`ASTUnit.cpp`](ASTUnit.cpp),
  [`tools/libclang`](../../tools/libclang/)。
- 下游: [`StandaloneDiagnostic.h`](../../include/clang/Frontend/StandaloneDiagnostic.h),
  [`Lexer.h`](../../include/clang/Lex/Lexer.h)。
- 关键类/函数: `StandaloneDiagnostic`, `SourceOffsetRange`,
  `StandaloneFixIt`, `translateStandaloneDiag`。

[`VerifyDiagnosticConsumer.cpp`](VerifyDiagnosticConsumer.cpp) —
`VerifyDiagnosticConsumer` 实现 —— 解析源码中
`// expected-*` / `// expected-no-diagnostics` 注释并比对实际
diagnostic, 实现 clang test-suite 的 verify 机制。
- 上游: [`CompilerInstance.cpp`](CompilerInstance.cpp),
  [`lib/Tooling`](../Tooling/) (verify 模式)。
- 下游: [`VerifyDiagnosticConsumer.h`](../../include/clang/Frontend/VerifyDiagnosticConsumer.h),
  [`TextDiagnosticBuffer.h`](../../include/clang/Frontend/TextDiagnosticBuffer.h),
  [`PPCallbacks.h`](../../include/clang/Lex/PPCallbacks.h)。
- 关键类/函数: `VerifyDiagnosticConsumer`, `Directive`,
  `ExpectedData`, `CheckDiagnostics`, `HandleComment`,
  `UpdateParsedFileStatus`。

### 3.5 Preprocessor 初始化 + 输出 + 依赖

[`InitPreprocessor.cpp`](InitPreprocessor.cpp) —
`InitializePreprocessor` 实现 —— 填默认 builtin 宏 (语言标准、目
标、平台、Attr、`__has_attribute` 等)、预定义 buffer、初始 `#pragma`
状态。
- 上游: [`CompilerInstance.cpp`](CompilerInstance.cpp)
  (`createPreprocessor`)。
- 下游: [`FrontendOptions.h`](../../include/clang/Frontend/FrontendOptions.h),
  [`Basic/MacroBuilder.h`](../../include/clang/Basic/MacroBuilder.h),
  [`HeaderSearch.h`](../../include/clang/Lex/HeaderSearch.h)。
- 关键类/函数: `InitializePreprocessor`, `DefineBuiltinMacro`,
  `AddImplicitIncludePTH`, `InitializePreprocessorInitList`。

[`PrintPreprocessedOutput.cpp`](PrintPreprocessedOutput.cpp) —
`DoPrintPreprocessedInput` 实现 —— 实现 `-E` / `-M` 模式, 把预处
理结果 / 依赖写到 `raw_ostream` (含 line markers、宏展开、空白策
略)。
- 上游:
  [`FrontendTool/ExecuteCompilerInvocation.cpp`](../FrontendTool/ExecuteCompilerInvocation.cpp),
  [`FrontendActions.cpp`](FrontendActions.cpp) (`PrintPreprocessedAction`)。
- 下游:
  [`PreprocessorOutputOptions.h`](../../include/clang/Frontend/PreprocessorOutputOptions.h),
  [`Preprocessor.h`](../../include/clang/Lex/Preprocessor.h),
  [`TokenConcatenation.h`](../../include/clang/Lex/TokenConcatenation.h)。
- 关键类/函数: `DoPrintPreprocessedInput`, `PrintMacroDefinition`,
  `PrintPreprocessedTokens`, `OutputMacroHandler`。

[`HeaderIncludeGen.cpp`](HeaderIncludeGen.cpp) —
`createHeaderIncluderGen` + `HeaderIncludesCallback` —— 实现
`-H`、`/showIncludes`、cl.exe 风格的 header include 列表 / JSON
输出。
- 上游: [`CompilerInstance.cpp`](CompilerInstance.cpp),
  [`FrontendTool/ExecuteCompilerInvocation.cpp`](../FrontendTool/ExecuteCompilerInvocation.cpp)。
- 下游: [`DependencyOutputOptions.h`](../../include/clang/Frontend/DependencyOutputOptions.h),
  [`Utils.h`](../../include/clang/Frontend/Utils.h),
  [`Preprocessor.h`](../../include/clang/Lex/Preprocessor.h)。
- 关键类/函数: `createHeaderIncluderGen`, `HeaderIncludesCallback`,
  `FileChanged`, `ShouldShowHeader`。

[`DependencyFile.cpp`](DependencyFile.cpp) —
`DependencyFileGenerator` 实现 —— `-M` / `-MD` / `-MMD` 类依赖文
件 (Makefile / CMake / Fixdep / 多种格式), 含 system header 过
滤。
- 上游: [`CompilerInstance.cpp`](CompilerInstance.cpp),
  [`lib/Tooling/DependencyScanningTool.cpp`](../Tooling/DependencyScanningTool.cpp)。
- 下游: [`DependencyOutputOptions.h`](../../include/clang/Frontend/DependencyOutputOptions.h),
  [`Basic/MakeSupport.h`](../../include/clang/Basic/MakeSupport.h),
  [`Serialization/ASTReader.h`](../../include/clang/Serialization/ASTReader.h)。
- 关键类/函数: `DependencyFileGenerator`,
  `DependencyCollector`, `AddDependency`, `outputDependencyFile`。

[`DependencyGraph.cpp`](DependencyGraph.cpp) —
`DependencyGraphPrinter` 实现 —— 输出 GraphViz / DOT 格式的头依
赖图 (`--dump-include-graph`)。
- 上游:
  [`FrontendTool/ExecuteCompilerInvocation.cpp`](../FrontendTool/ExecuteCompilerInvocation.cpp)
  (DPrint 依赖图)。
- 下游: [`Utils.h`](../../include/clang/Frontend/Utils.h),
  [`PPCallbacks.h`](../../include/clang/Lex/PPCallbacks.h),
  [`llvm/Support/GraphWriter.h`](../../../llvm/include/llvm/Support/GraphWriter.h)。
- 关键类/函数: `DependencyGraphPrinter`,
  `DependencyGraphCallback`, `writeNodeReference`。

### 3.6 Module dependency + PCH preamble

[`ModuleDependencyCollector.cpp`](ModuleDependencyCollector.cpp) —
`ModuleDependencyCollector` 实现 —— 收集模块构建过程中实际访问的
所有 PCM / 头文件并写入 file-mapping, 供分布构建复制 module 依
赖。
- 上游: [`CompilerInstance.cpp`](CompilerInstance.cpp)
  (`createModuleDependencyCollector`)。
- 下游: [`Utils.h`](../../include/clang/Frontend/Utils.h),
  [`Serialization/ASTReader.h`](../../include/clang/Serialization/ASTReader.h),
  [`llvm/Support/IOSandbox.h`](../../../llvm/include/llvm/Support/IOSandbox.h)。
- 关键类/函数: `ModuleDependencyCollector`,
  `ModuleDependencyListener`, `ModuleDependencyPPCallbacks`,
  `writeFileMap`, `addFile`。

[`PrecompiledPreamble.cpp`](PrecompiledPreamble.cpp) —
`PrecompiledPreamble` 实现 —— `BuildPreamble` /
`CanReusePreamble` / `AddImplicitPreamble`, 缓存 main file preamble
的 PCH, 加速 IDE / 编辑场景重新解析。
- 上游: [`ASTUnit.cpp`](ASTUnit.cpp),
  [`lib/Tooling/Tooling.cpp`](../Tooling/Tooling.cpp),
  [`lib/Interpreter`](../../lib/Interpreter/)。
- 下游: [`CompilerInstance.h`](../../include/clang/Frontend/CompilerInstance.h),
  [`Preprocessor.h`](../../include/clang/Lex/Preprocessor.h),
  [`Serialization/ASTWriter.h`](../../include/clang/Serialization/ASTWriter.h)。
- 关键类/函数: `PrecompiledPreamble`, `BuildPreamble`,
  `CanReusePreamble`, `AddImplicitPreamble`, `PreambleCallbacks`,
  `ComputePreambleBounds`。

### 3.7 Rewrite/ 子库 (clangRewriteFrontend)

[`Rewrite/CMakeLists.txt`](Rewrite/CMakeLists.txt) —
`clangRewriteFrontend` 库定义 —— 链接
`clangAST/Basic/Edit/Frontend/Lex/Rewrite/Serialization`。
- 关键内容: `clangRewriteFrontend` 目标, `omp_gen`。

[`Rewrite/FrontendActions.cpp`](Rewrite/FrontendActions.cpp) — 实
现 Rewrite 子目录的 Action: `HTMLPrintAction` / `FixItAction` /
`FixItRecompile` / `RewriteObjCAction` / `RewriteModernObjCAction`
等的 `ExecuteAction` / `CreateASTConsumer`。
- 上游:
  [`FrontendTool/ExecuteCompilerInvocation.cpp`](../FrontendTool/ExecuteCompilerInvocation.cpp)。
- 下游:
  [`Rewrite/Frontend/FixItRewriter.h`](../../include/clang/Rewrite/Frontend/FixItRewriter.h),
  [`Rewrite/Frontend/ASTConsumers.h`](../../include/clang/Rewrite/Frontend/ASTConsumers.h),
  [`Rewrite/Frontend/Rewriters.h`](../../include/clang/Rewrite/Frontend/Rewriters.h)。
- 关键类/函数: `HTMLPrintAction::CreateASTConsumer`,
  `FixItAction::CreateASTConsumer`, `FixItRewriteInPlace`,
  `FixItActionSuffixInserter`。

[`Rewrite/FixItRewriter.cpp`](Rewrite/FixItRewriter.cpp) —
`FixItRewriter` 实现 —— `DiagnosticConsumer` 适配器, 把 diagnostic
的 `FixItHint` 应用到源码并把改写结果写回, 委托原 client 仍输出诊
断。
- 上游: [`Rewrite/FrontendActions.cpp`](Rewrite/FrontendActions.cpp)
  (`FixItAction` / `FixItRecompile`)。
- 下游:
  [`Commit.h`](../../include/clang/Edit/Commit.h),
  [`EditsReceiver.h`](../../include/clang/Edit/EditsReceiver.h),
  [`Rewrite/Core/Rewriter.h`](../../include/clang/Rewrite/Core/Rewriter.h)。
- 关键类/函数: `FixItRewriter`, `WriteFixedFile`,
  `HandleDiagnostic`, `FixItOptions`。

[`Rewrite/HTMLPrint.cpp`](Rewrite/HTMLPrint.cpp) — `HTMLPrinter`
`ASTConsumer`: 把源码 HTML 化 (语法高亮 + 宏高亮), 生成可阅读的
`.html` 源码视图。
- 上游: [`Rewrite/FrontendActions.cpp`](Rewrite/FrontendActions.cpp)
  (`HTMLPrintAction`)。
- 下游:
  [`Rewrite/Core/HTMLRewrite.h`](../../include/clang/Rewrite/Core/HTMLRewrite.h),
  [`Rewrite/Core/Rewriter.h`](../../include/clang/Rewrite/Core/Rewriter.h),
  [`Rewrite/Frontend/ASTConsumers.h`](../../include/clang/Rewrite/Frontend/ASTConsumers.h)。
- 关键类/函数: `CreateHTMLPrinter`, `HTMLPrinter`,
  `HandleTranslationUnit`。

[`Rewrite/InclusionRewriter.cpp`](Rewrite/InclusionRewriter.cpp) —
`DoRewriteInclude` 实现 —— 把 `#include` 替换为对应头的内容, 生成
"自包含"或 "preprocessed-includes" 风格的单一文件。
- 上游:
  [`FrontendTool/ExecuteCompilerInvocation.cpp`](../FrontendTool/ExecuteCompilerInvocation.cpp)。
- 下游:
  [`PreprocessorOutputOptions.h`](../../include/clang/Frontend/PreprocessorOutputOptions.h),
  [`Rewrite/Frontend/Rewriters.h`](../../include/clang/Rewrite/Frontend/Rewriters.h),
  [`Preprocessor.h`](../../include/clang/Lex/Preprocessor.h)。
- 关键类/函数: `DoRewriteInclude`, `InclusionRewriter`,
  `IncludedFile`。

[`Rewrite/RewriteMacros.cpp`](Rewrite/RewriteMacros.cpp) —
`DoRewriteMacros` 实现 —— 在 tokens 层把宏调用展开为其字面结果,
保留 `#include` 和注释 (`-rewrite-macros`)。
- 上游:
  [`FrontendTool/ExecuteCompilerInvocation.cpp`](../FrontendTool/ExecuteCompilerInvocation.cpp)
  (`RewriteMacros`)。
- 下游: [`Rewrite/Core/Rewriter.h`](../../include/clang/Rewrite/Core/Rewriter.h),
  [`Rewrite/Frontend/Rewriters.h`](../../include/clang/Rewrite/Frontend/Rewriters.h),
  [`Preprocessor.h`](../../include/clang/Lex/Preprocessor.h)。
- 关键类/函数: `DoRewriteMacros`, `RewriteMacros`, `isSameToken`,
  `GetNextRawTok`。

[`Rewrite/RewriteModernObjC.cpp`](Rewrite/RewriteModernObjC.cpp) —
`DoRewriteModernObjC`: 把 ObjC 源码改写为 ObjC++ / C++ 形式, 生成
ARC / Modern ObjC 迁移产物 (现代 ObjC 转写器, 常用于迁移到 ARC)。
- 上游:
  [`FrontendTool/ExecuteCompilerInvocation.cpp`](../FrontendTool/ExecuteCompilerInvocation.cpp)
  (`RewriteObjC` 现代分支)。
- 下游: [`Rewrite/Core/Rewriter.h`](../../include/clang/Rewrite/Core/Rewriter.h),
  [`Sema.h`](../../include/clang/Sema/Sema.h),
  [`RecursiveASTVisitor.h`](../../include/clang/AST/RecursiveASTVisitor.h)。
- 关键类/函数: `DoRewriteModernObjC`, `RewriteModernObjC`,
  `RewriteModernObjCImpl`。

[`Rewrite/RewriteObjC.cpp`](Rewrite/RewriteObjC.cpp) —
`DoRewriteObjC`: 经典 ObjC → C 改写器, 把 retain / release /
autorelease / properties / synthesize / protocals 全部翻译为纯 C
调用 (ARC 之前的迁移工具)。
- 上游:
  [`FrontendTool/ExecuteCompilerInvocation.cpp`](../FrontendTool/ExecuteCompilerInvocation.cpp)
  (`RewriteObjC`)。
- 下游: [`Rewrite/Core/Rewriter.h`](../../include/clang/Rewrite/Core/Rewriter.h),
  [`Sema.h`](../../include/clang/Sema/Sema.h),
  [`RecursiveASTVisitor.h`](../../include/clang/AST/RecursiveASTVisitor.h)。
- 关键类/函数: `DoRewriteObjC`, `RewriteObjC`,
  `RewriteObjCImpl`。

[`Rewrite/RewriteTest.cpp`](Rewrite/RewriteTest.cpp) —
`DoRewriteTest` 实现 —— `TokenRewriter` 试验场, 演示如何在 tokens
上做插入 / 删除 (注释加 `<i></i>` 标签), 作为前端 rewriter API
的最小 example。
- 上游:
  [`FrontendTool/ExecuteCompilerInvocation.cpp`](../FrontendTool/ExecuteCompilerInvocation.cpp)
  (`RewriteTest`)。
- 下游:
  [`Rewrite/Core/TokenRewriter.h`](../../include/clang/Rewrite/Core/TokenRewriter.h),
  [`Rewrite/Frontend/Rewriters.h`](../../include/clang/Rewrite/Frontend/Rewriters.h)。
- 关键类/函数: `DoRewriteTest`, `TokenRewriter`。

---

### 3.9 FrontendTool (驱动壳, 独立子库)

注: FrontendTool 是从本目录拆出的独立子库 (单文件, 编译为
`clangFrontendTool`), 它 **依赖** 本目录的 `clangFrontend` + `clangCodeGen`
+ `clangStaticAnalyzer` + `clangExtractAPI` + `clangScalableStaticAnalysis`
+ `clangRewriteFrontend` 等多个子库, 把所有具体 FrontendAction 的构造
集中到一处供 `clang-cc1` 主程序调用。

[`FrontendTool/ExecuteCompilerInvocation.cpp`](../FrontendTool/ExecuteCompilerInvocation.cpp)
— `clang-cc1` 驱动入口: 实现 `CreateFrontendAction`
(`frontend::ActionKind` → 具体 `FrontendAction` 子类) +
`ExecuteCompilerInvocation` (经 `loadPlugins` / 装
`ExecutePluginAction` / `AddWrapperActions` / `AddPluginActions` 后调
`CI.ExecuteAction()`)。拆出本文件的目的是最小化 `clangFrontend` 的
依赖: `clangFrontend` 不需要再包含 `clangCodeGen` /
`clangStaticAnalyzer` / `clangExtractAPI` / `clangScalableStaticAnalysis`
/ `clangCIR` 等具体 Action 实现的头文件。
- 上游:
  [`clang/tools/clang/driver.cpp`](../../tools/clang/driver.cpp)
  (经 `clang::ExecuteCompilerInvocation`)。
- 下游: [`CompilerInstance.h`](../../include/clang/Frontend/CompilerInstance.h),
  [`CompilerInvocation.h`](../../include/clang/Frontend/CompilerInvocation.h),
  [`FrontendPluginRegistry.h`](../../include/clang/Frontend/FrontendPluginRegistry.h),
  [`CodeGen/CodeGenAction.h`](../../include/clang/CodeGen/CodeGenAction.h),
  [`Rewrite/Frontend/FrontendActions.h`](../../include/clang/Rewrite/Frontend/FrontendActions.h),
  [`StaticAnalyzer/Frontend/FrontendActions.h`](../../include/clang/StaticAnalyzer/Frontend/FrontendActions.h),
  [`ExtractAPI/FrontendActions.h`](../../include/clang/ExtractAPI/FrontendActions.h),
  [`ScalableStaticAnalysis/Frontend/SourceTransformationFrontendAction.h`](../../include/clang/ScalableStaticAnalysis/Frontend/SourceTransformationFrontendAction.h),
  [`CIR/FrontendAction/CIRGenAction.h`](../../include/clang/CIR/FrontendAction/CIRGenAction.h)。
- 关键类/函数: `CreateFrontendBaseAction`,
  `CreateFrontendAction`, `ExecuteCompilerInvocation`, `loadPlugins`,
  `RunOptionsAnalysis`, `EmitAssembly` / `EmitBC` / `EmitObj` /
  `EmitLLVM` / `EmitLLVMIr` / `EmitCIR` 等 switch case。

[`FrontendTool/Utils.h`](../FrontendTool/Utils.h) — FrontendTool
公开头: `CreateFrontendAction(CompilerInstance&)` /
`ExecuteCompilerInvocation(CompilerInstance*)` 自由函数声明。

`FrontendTool/CMakeLists.txt` — `clangFrontendTool` 库定义: 仅包
含 1 个 .cpp, 链接 `clangFrontend` + `clangCodeGen` +
`clangStaticAnalyzer` + `clangExtractAPI` +
`clangScalableStaticAnalysis` + `clangCIR` + `clangRewriteFrontend`
+ `clangDriver` (经 llvm::opt::OptTable)。

---

### 4.1 cc1 入口到 FrontendAction::Execute

```
clang -cc1 -emit-obj foo.c
  │
  ▼
clang_main [clang/tools/clang/main.cpp]
  └─ executeCC1Tool [clang/tools/clang/driver.cpp]
       └─ clang::ExecuteCompilerInvocation [clang/lib/FrontendTool/]
            ├─ CompilerInvocation::CreateFromArgs (CompilerInvocation.cpp)
            │    ├─ ParseDiagnosticArgs
            │    ├─ 解析所有 *Options (LangOptions/TargetOptions/...)
            │    └─ RoundTrip 缓存 argv 用于调试
            ├─ 选 FrontendAction (frontend::ActionKind lookup)
            │    ├─ EmitObj → CodeGenAction (clang/lib/CodeGen/)
            │    ├─ EmitLLVM → CodeGenAction
            │    ├─ -E → PrintPreprocessedAction
            │    ├─ -ast-dump → ASTDumpAction (FrontendActions.cpp)
            │    ├─ -emit-pch → GeneratePCHAction
            │    ├─ -emit-module → GenerateModuleAction
            │    └─ -fixit → FixItAction (Rewrite/FrontendActions.cpp)
            ├─ new CompilerInstance
            ├─ CompilerInstance::setInvocation
            └─ CompilerInstance::ExecuteAction
                 ├─ createDiagnostics
                 │    └─ DiagnosticOptions 解析 → 装 DiagnosticConsumer
                 │         ├─ default: TextDiagnosticPrinter
                 │         ├─ -verify: VerifyDiagnosticConsumer
                 │         │    └─ 链 TextDiagnosticBuffer
                 │         ├─ -sarif: SARIFDiagnosticPrinter
                 │         └─ -serialize-diagnostics: SerializedDiagnosticPrinter
                 ├─ createFileManager + createSourceManager
                 ├─ createTargetInfo + createAuxTarget
                 ├─ createPreprocessor
                 │    └─ InitializePreprocessor (InitPreprocessor.cpp)
                 │         └─ 填默认 builtin 宏 + #pragma 状态
                 ├─ createASTContext + createSema
                 ├─ createASTConsumer (派生 Action 提供)
                 └─ createDefaultOutputFile
                      │
                      ▼
                 FrontendAction::Execute (FrontendAction.cpp)
                   ├─ BeginSourceFile (装配 plugin / chained-include /
                   │                   LayoutOverride / ASTMerge)
                   ├─ Action::ExecuteAction (派生类)
                   │    └─ Parser::ParseAST (clang/lib/Parse/)
                   │         └─ Sema::ActOnTranslationUnit
                   │              └─ ASTConsumer::HandleTranslationUnit
                   │                   ├─ CodeGenAction → IR/对象输出
                   │                   ├─ ASTDumpAction → ASTDumper
                   │                   ├─ ASTPrintAction → ASTPrinter
                   │                   ├─ GeneratePCHAction → ASTWriter
                   │                   ├─ HTMLPrintAction → HTMLPrinter
                   │                   ├─ FixItAction → FixItRewriter
                   │                   └─ PrintPreprocessedAction →
                   │                        DoPrintPreprocessedInput
                   └─ EndSourceFile
```

### 4.2 ASTUnit 装入与脱离

```
libclang / clangd / tooling
  │
  ▼
ASTUnit::LoadFromCompilerInvocation (ASTUnit.cpp)
  ├─ 创建 CompilerInstance (跳过 Driver)
  ├─ 复用 FrontendAction 跑一遍
  ├─ 把 ASTContext / PreprocessingRecord / Sema 保留
  ├─ 保留 StandaloneDiagnostic (供 SourceManager 销毁后回放)
  └─ 析构 CompilerInstance 但保留 AST
       └─ 此时用户拿到 ASTUnit, 可长期持有 + 增量解析
            ├─ Reparse
            │    └─ PrecompiledPreamble::CanReusePreamble
            │         ├─ BuildPreamble (主文件 PCH 缓存)
            │         ├─ AddImplicitPreamble (回收 PCH 内容)
            │         └─ Continue without re-lexing main file
            ├─ Save
            │    └─ ASTWriter (写 .ast 文件)
            └─ LoadFromASTFile
                 └─ ASTReader (从 .ast 还原 ASTUnit)

ASTUnit 跨 TU 场景:
  ├─ ASTUnit::LoadFromASTFile → CrossTranslationUnit
  │    └─ ASTUnit::getASTContext → 多个 ctx 共享 AST
  └─ ASTMergeAction (ASTMerge.cpp) + ASTImporter
       └─ 把多个 .ast 并入当前 ctx (供 clang-repl JIT 用)
```

### 4.3 Diagnostic 路由链

```
clang::DiagnosticsEngine::Report (clang/lib/Basic/Diagnostic.cpp)
  │
  ▼
DiagnosticConsumer::HandleDiagnostic (从 DiagnosticsEngine 取出)
  │
  ├─ TextDiagnosticPrinter (TextDiagnosticPrinter.cpp) 默认
  │    └─ TextDiagnostic (TextDiagnostic.cpp)
  │         └─ DiagnosticRenderer::emitDiagnostic (DiagnosticRenderer.cpp)
  │              ├─ emitIncludeStack
  │              ├─ emitMacroExpansions
  │              └─ emitCaret (拼 caret/source-snippet/fixit/color)
  │                   └─ 写到 raw_ostream (默认 stderr)
  │
  ├─ SARIFDiagnosticPrinter (SARIFDiagnosticPrinter.cpp)
  │    └─ SARIFDiagnostic (SARIFDiagnostic.cpp)
  │         └─ DiagnosticRenderer::emitDiagnostic
  │              └─ 输出 SARIF object 给 JSON writer
  │
  ├─ SerializedDiagnosticPrinter (SerializedDiagnosticPrinter.cpp)
  │    └─ 写 .dia bitstream (LLVM BitstreamWriter)
  │     配套 SerializedDiagnosticReader
  │         └─ readDiagnostics → visit* 回调
  │
  ├─ LogDiagnosticPrinter (LogDiagnosticPrinter.cpp)
  │    └─ 输出 plist (.dia 但文本化) 给 Xcode
  │
  ├─ VerifyDiagnosticConsumer (VerifyDiagnosticConsumer.cpp)
  │    └─ HandleComment 收 // expected-*
  │         └─ TextDiagnosticBuffer (TextDiagnosticBuffer.cpp) 暂存
  │              └─ EndSourceFile → CheckDiagnostics 比对
  │                   └─ 不匹配 → 重新投给 primary consumer
  │
  └─ ChainedDiagnosticConsumer (ChainedDiagnosticConsumer.cpp)
       └─ 同时投给 Primary + Secondary (多通道)
```

### 4.4 依赖 / Include trace 输出链

```
CompilerInstance::ExecuteAction (CompilerInstance.cpp)
  │
  ▼
按 DiagnosticOptions / DependencyOutputOptions 装配:
  │
  ├─ DependencyFileGenerator (DependencyFile.cpp)
  │    └─ 挂 PPCallbacks → 每次 FileChanged/EndOfMainFile
  │         ├─ AddDependency (去重 + system header 过滤)
  │         └─ outputDependencyFile → 写 -M/-MD/-MMD depfile
  │              ├─ Make / CMake / Fixdep / MSVC 多格式
  │              └─ 通过 DependencyOutputOptions 选
  │
  ├─ HeaderIncludesCallback (HeaderIncludeGen.cpp)
  │    └─ 挂 PPCallbacks → FileChanged
  │         ├─ -H → ASCII 树形打印到 stderr
  │         ├─ /showIncludes → MSVC 风格 "[Note: ...]"
  │         └─ -header-include-format=json → 输出 JSON 列表
  │
  ├─ DependencyGraphPrinter (DependencyGraph.cpp)
  │    └─ 挂 PPCallbacks → 累计 NodeRef 边
  │         └─ 写 .dot GraphViz 文件 (--dump-include-graph)
  │
  ├─ ModuleDependencyCollector (ModuleDependencyCollector.cpp)
  │    └─ 挂 ASTReaderListener + PPCallbacks
  │         └─ writeFileMap
  │              └─ 供分布构建复制 module 依赖
  │
  └─ PrecompiledPreamble (PrecompiledPreamble.cpp)
       └─ ASTUnit 复用:
            ├─ BuildPreamble → 缓存 main file PCH (在内存)
            ├─ CanReusePreamble → 比较 PCH 边界 / 检查依赖
            └─ AddImplicitPreamble → 把 PCH 内容喂给新 Preprocessor
                 └─ 跳过 main file 重新 lex (IDE 重解析提速)
```

### 4.5 改写前端流水线

```
clang -cc1 -fixit foo.c
  │
  ▼
ExecuteCompilerInvocation
  ├─ CompilerInvocation::CreateFromArgs
  ├─ 选 FixItAction (Rewrite/FrontendActions.cpp)
  └─ CompilerInstance::ExecuteAction
       │
       ▼
  FixItAction::CreateASTConsumer
    └─ return MultiplexConsumer
         ├─ ASTSerializer (主翻译单元序列化)
         ├─ FixItActionSuffixInserter
         └─ ASTCommentInjection
              │
              ▼
  FrontendAction::Execute → ParseAST → Sema
    └─ ASTConsumer::HandleTranslationUnit → 收集 FixItHints
         │
         ▼
  FixItAction::ExecuteAction
    ├─ 二次 setSourceManager + 新 Preprocessor (无主翻译单元)
    ├─ 挂 FixItRewriter (FixItRewriter.cpp)
    │    └─ HandleDiagnostic 拦截 → 应用 FixItHint 到 source
    │         └─ WriteFixedFile → 改写后源码写到 .cpp
    └─ 委托原 client 仍输出诊断

clang -cc1 -rewrite-objc foo.m
  │
  ▼
  RewriteObjCAction::CreateASTConsumer
    └─ MultiplexConsumer
         ├─ ASTSerializer
         └─ RewriteObjC (RewriteObjC.cpp)
              └─ RewriteObjCImpl
                   └─ HandleTranslationUnit
                        ├─ 遍历 ASTDecl → 生成纯 C 输出
                        └─ Rewriter::getEditBuffer → 写 .cpp

clang -cc1 -HTML foo.c
  │
  ▼
  HTMLPrintAction::CreateASTConsumer
    └─ HTMLPrinter (Rewrite/HTMLPrint.cpp)
         └─ HandleTranslationUnit
              └─ HTMLRewrite::AddLineNumbers + AddStyleLinks
                   └─ 写 .html
```

### 4.6 初始化 Preprocessor 链

```
CompilerInstance::createPreprocessor (CompilerInstance.cpp)
  │
  ├─ 构造 Preprocessor (Preprocessor.cpp in clang/lib/Lex/)
  ├─ HeaderSearch::ApplySearchOptions (HeaderSearch.cpp)
  │    └─ 处理 -I/-isystem/-iquote/-include/-iframework
  ├─ InitializePreprocessor (InitPreprocessor.cpp)
  │    ├─ DefineBuiltinMacro (填默认 builtin 宏):
  │    │    ├─ __clang_major__/__clang_minor__/__clang_patchlevel__
  │    │    ├─ __VERSION__ / __GNUC__ / __GNUC_MINOR__
  │    │    ├─ __STDC__ / __STDC_VERSION__ / __cplusplus
  │    │    ├─ 语言标准相关宏 (__cpp_* / __has_*)
  │    │    ├─ 目标相关 (__LITTLE_ENDIAN__ / __LP64__ / ...)
  │    │    ├─ 平台相关 (__linux__ / __APPLE__ / _WIN32 / ...)
  │    │    └─ Attr 相关 (__has_attribute / __has_builtin / ...)
  │    ├─ 初始 #pragma 状态 (poison / system_header / ...)
  │    └─ AddImplicitIncludePTH (PTH 加速)
  └─ 返回完整可用 Preprocessor
       │
       ▼
  CompilerInstance 把 Preprocessor 给 Parser / Sema 消费
```

---

## §5. 推荐阅读顺序

### 阶段 1: 数据模型头 (30 分钟)
[`FrontendOptions.h`](../../include/clang/Frontend/FrontendOptions.h) +
[`FrontendAction.h`](../../include/clang/Frontend/FrontendAction.h) +
[`CompilerInstance.h`](../../include/clang/Frontend/CompilerInstance.h)
+ [`CompilerInvocation.h`](../../include/clang/Frontend/CompilerInvocation.h)
+ [`ASTUnit.h`](../../include/clang/Frontend/ASTUnit.h)

### 阶段 1.5: FrontendTool 驱动壳 (15 分钟)
[`FrontendTool/ExecuteCompilerInvocation.cpp`](../FrontendTool/ExecuteCompilerInvocation.cpp)
— `clang-cc1` 主程序的唯一入口 (`ExecuteCompilerInvocation` +
`CreateFrontendAction`)。本文件虽然不在本目录, 但它消费本目录全部
公开头, 且本目录的所有 `ActionKind` 都在它的 `switch` 中被映射到具
体 `FrontendAction` 子类。

### 阶段 2: 顶层 Compiler orchestration (2 小时)
- [`CompilerInstance.cpp`](CompilerInstance.cpp) — 中央协调器
- [`CompilerInvocation.cpp`](CompilerInvocation.cpp) — `-cc1` argv 解
  析
- [`FrontendOptions.cpp`](FrontendOptions.cpp) — InputKind 映射表
- [`FrontendAction.cpp`](FrontendAction.cpp) — Action 生命周期框架
- [`FrontendActions.cpp`](FrontendActions.cpp) — 标准 Action 集
- [`ASTConsumers.cpp`](ASTConsumers.cpp) — 标准 ASTConsumer 工厂

### 阶段 3: AST 装配 (1.5 小时)
- [`ASTUnit.cpp`](ASTUnit.cpp) — AST 容器 (libclang/clangd 用)
- [`ASTMerge.cpp`](ASTMerge.cpp) — 多 .ast 合并
- [`MultiplexConsumer.cpp`](MultiplexConsumer.cpp) — 多 consumer 扇出
- [`ChainedIncludesSource.cpp`](ChainedIncludesSource.cpp) — 内存 PCH
- [`InterfaceStubFunctionsConsumer.cpp`](InterfaceStubFunctionsConsumer.cpp)
  — `.ifc` 桩
- [`LayoutOverrideSource.cpp`](LayoutOverrideSource.cpp) —
  `-foverride-record-layout` 测试钩子

### 阶段 4: Preprocessor 初始化与输出 (1.5 小时)
- [`InitPreprocessor.cpp`](InitPreprocessor.cpp) — 默认 builtin 宏
- [`PrintPreprocessedOutput.cpp`](PrintPreprocessedOutput.cpp) — `-E`
  输出
- [`HeaderIncludeGen.cpp`](HeaderIncludeGen.cpp) — `-H` / `/showIncludes`
- [`DependencyFile.cpp`](DependencyFile.cpp) — `-M` / `-MD` 依赖
- [`DependencyGraph.cpp`](DependencyGraph.cpp) — include graph `.dot`
- [`ModuleDependencyCollector.cpp`](ModuleDependencyCollector.cpp) —
  module PCM 依赖
- [`PrecompiledPreamble.cpp`](PrecompiledPreamble.cpp) — main file
  PCH 缓存

### 阶段 5: Diagnostic 链路 (2 小时)
- [`DiagnosticRenderer.cpp`](DiagnosticRenderer.cpp) — 渲染基类
- [`TextDiagnostic.cpp`](TextDiagnostic.cpp) + [`TextDiagnosticPrinter.cpp`](TextDiagnosticPrinter.cpp)
  — 默认 human-readable
- [`TextDiagnosticBuffer.cpp`](TextDiagnosticBuffer.cpp) — 暂存
- [`ChainedDiagnosticConsumer.cpp`](ChainedDiagnosticConsumer.cpp) —
  fan-out
- [`LogDiagnosticPrinter.cpp`](LogDiagnosticPrinter.cpp) — plist 日志
- [`SARIFDiagnostic.cpp`](SARIFDiagnostic.cpp) +
  [`SARIFDiagnosticPrinter.cpp`](SARIFDiagnosticPrinter.cpp) — SARIF
- [`SerializedDiagnosticPrinter.cpp`](SerializedDiagnosticPrinter.cpp)
  + [`SerializedDiagnosticReader.cpp`](SerializedDiagnosticReader.cpp)
  — `.dia`
- [`VerifyDiagnosticConsumer.cpp`](VerifyDiagnosticConsumer.cpp) —
  test verify
- [`StandaloneDiagnostic.cpp`](StandaloneDiagnostic.cpp) —
  SourceManager 销毁后回放

### 阶段 6: 改写前端 (`clangRewriteFrontend`) (1.5 小时)
- [`Rewrite/FrontendActions.cpp`](Rewrite/FrontendActions.cpp) — 改写
  Action 集
- [`Rewrite/FixItRewriter.cpp`](Rewrite/FixItRewriter.cpp) — `-fixit`
- [`Rewrite/HTMLPrint.cpp`](Rewrite/HTMLPrint.cpp) — `-HTML`
- [`Rewrite/InclusionRewriter.cpp`](Rewrite/InclusionRewriter.cpp) —
  `-rewrite-includes`
- [`Rewrite/RewriteMacros.cpp`](Rewrite/RewriteMacros.cpp) —
  `-rewrite-macros`
- [`Rewrite/RewriteModernObjC.cpp`](Rewrite/RewriteModernObjC.cpp) —
  现代 ObjC 迁移
- [`Rewrite/RewriteObjC.cpp`](Rewrite/RewriteObjC.cpp) — 经典 ObjC → C
- [`Rewrite/RewriteTest.cpp`](Rewrite/RewriteTest.cpp) —
  `TokenRewriter` 试验场

### 阶段 7: 杂项 (按需)
[`TestModuleFileExtension.h`](TestModuleFileExtension.h) +
[`TestModuleFileExtension.cpp`](TestModuleFileExtension.cpp) —
`ModuleFileExtension` 自定义 PCM 块的最小样例。

---

## §6. 自定义扩展指南

### 6.1 添加新 `FrontendAction`

1. 在 [`clang/include/clang/Frontend/FrontendActions.h`](../../include/clang/Frontend/FrontendActions.h)
   声明 `class MyAction : public ASTFrontendAction` (或
   `PreprocessorFrontendAction`)。
2. 在 [`FrontendActions.cpp`](FrontendActions.cpp) 添加
   `CreateASTConsumer`: 返回你要的 `ASTConsumer`。
3. 在
   [`clang/lib/FrontendTool/ExecuteCompilerInvocation.cpp`](../FrontendTool/ExecuteCompilerInvocation.cpp)
   给 `frontend::ActionKind` 加 case, 调
   `CreateFrontendAction(MyAction)`。
4. 更新
   [`clang/include/clang/Options/Options.td`](../../include/clang/Options/Options.td)
   加 `-my-mode` 选项。
5. 测试: `clang/test/Frontend/my-action.cpp`。

### 6.2 添加新 `DiagnosticConsumer`

1. 派生自
   [`DiagnosticConsumer`](../../include/clang/Basic/Diagnostic.h)
   (或 `ChainedDiagnosticConsumer` 适配器)。
2. 实现 `HandleDiagnostic(DiagnosticsEngine::Level, const Diagnostic &)`。
3. 如需 caret / range / fixit 渲染: 派生自
   [`DiagnosticRenderer`](../../include/clang/Frontend/DiagnosticRenderer.h),
   覆写 `emitDiagnostic*`。
4. 在
   [`CompilerInvocation.cpp`](CompilerInvocation.cpp) 的
   `ParseDiagnosticArgs` 加识别新 flag。
5. 在 [`CompilerInstance.cpp`](CompilerInstance.cpp) 的
   `createDiagnostics` 装配它, 或经
   `ChainedDiagnosticConsumer` 与默认 `TextDiagnosticPrinter` 并联。
6. 测试: `clang/test/Frontend/my-diagnostic.c`。

### 6.3 添加新依赖 / Include 输出

1. 派生自
   [`DependencyCollector`](../../include/clang/Frontend/Utils.h)
   或 `PPCallbacks`。
2. 实现 `FileChanged` / `EndOfMainFile` / `InclusionDirective` 钩子。
3. 在 [`CompilerInstance.cpp`](CompilerInstance.cpp) 的
   `createPreprocessor` / `createModuleDependencyCollector` 装配到
   Preprocessor 的 `PPCallbacks` 链。
4. 用 `DependencyOutputOptions` 加新选项。
5. 测试: `clang/test/Frontend/my-dep.c`。

### 6.4 添加新改写 Action

1. 在 [`Rewrite/FrontendActions.cpp`](Rewrite/FrontendActions.cpp) 加
   `class MyRewriteAction : public ASTFrontendAction` + 实现
   `CreateASTConsumer`。
2. 用 `Rewriter` /
   [`TokenRewriter`](../../include/clang/Rewrite/Core/TokenRewriter.h)
   做改写。
3. 在
   [`FrontendTool/ExecuteCompilerInvocation.cpp`](../FrontendTool/ExecuteCompilerInvocation.cpp)
   加 `ActionKind` case。
4. 更新 [`Options.td`](../../include/clang/Options/Options.td)。
5. 测试: `clang/test/Rewrite/my-rewrite.m` (或 `.cpp`)。

### 6.5 添加新 `ModuleFileExtension`

1. 派生自
   [`ModuleFileExtension`](../../include/clang/Frontend/ModuleFileExtension.h),
   实现 `createExtensionReader` / `createExtensionWriter` /
   `getExtensionMetadata`。
2. 在
   [`CompilerInvocation.cpp`](CompilerInvocation.cpp) 的
   `ParseFileSystemArgs` 装入 FrontendOptions 的
   `ModuleFileExtensions`。
3. 在生成 PCM 的
   [`GeneratePCHAction`](FrontendActions.cpp) /
   [`GenerateModuleAction`](FrontendActions.cpp) 自动被
   `ASTWriter` 调用。
4. 测试: 参考
   [`TestModuleFileExtension.cpp`](TestModuleFileExtension.cpp) +
   [`clang/unittests/Serialization/`](../../unittests/Serialization/)。

### 6.6 自定义 ASTUnit 行为

1. 派生自 [`ASTUnit`](../../include/clang/Frontend/ASTUnit.h) 或直接
   复用。
2. 在 IDE / tooling 入口 (e.g. `libclang` /
   `clangd`) 调
   `ASTUnit::LoadFromCompilerInvocation`。
3. 用 `Reparse` + `PrecompiledPreamble::CanReusePreamble` 加速重解析。
4. 用 `StoredDiagnostics` + `StandaloneDiagnostic` 处理延迟诊断。
5. 测试: `clang/test/Index/my-astunit.m` (或 `clang/test/Tooling/`)。

### 6.7 添加新 Frontend tool / 选项

1. 加 `-my-flag` 到
   [`Options.td`](../../include/clang/Options/Options.td)。
2. 在 [`CompilerInvocation.cpp`](CompilerInvocation.cpp) 的
   `ParseArgs` / `RoundTrip` 加解析 / 反渲染。
3. 用 `LangOptions` / `TargetOptions` / `CodeGenOptions` / 自己的
   `*Options` 存。
4. 在相关 FrontendAction / CodeGenAction 读
   `CompilerInstance::getInvocation().getLangOpts()`。
5. 测试: `clang/test/Driver/my-flag.c` (driver 翻译)
   + `clang/test/Frontend/my-flag.c` (cc1 行为)。

### 6.8 调试 cc1 行为

1. 用 `clang -cc1 -v` (verbose) 看实际 Target / Includes / 警告。
2. 用 `clang -cc1 -### -my-flag` 间接 `RoundTrip` 验证 argv 解
   析。
3. 用 `clang -cc1 -ast-dump foo.c` 验证 AST 形状。
4. 用 `clang -cc1 -ast-dump-all` 看完整 cross-TU AST。
5. 用 `clang -cc1 -E foo.c -o foo.i` 验证 preprocessor。
6. 临时在 `CompilerInstance::ExecuteAction` / `InitializePreprocessor`
   加 `llvm::errs() << ...` trace。

---

## §7. NT 注释索引

当前 `clang/lib/Frontend/` 下尚无 `// <NT>` 注释。已建立目录索引,
姊妹 overview:

- [`clang/lib/AST/0-overview.md`](../AST/0-overview.md) — Clang AST
  层
- [`clang/lib/Basic/0-overview.md`](../Basic/0-overview.md) — Clang 基
  础设置 + 共享数据
- [`clang/lib/CodeGen/0-overview.md`](../CodeGen/0-overview.md) —
  Clang AST → LLVM IR
- [`clang/lib/Driver/0-overview.md`](../Driver/0-overview.md) — Clang
  编译驱动
- `clang/lib/Lex/` — Clang Lexer (待写)
- `clang/lib/Parse/` — Clang Parser (待写)
- [`clang/lib/Sema/0-overview.md`](../Sema/0-overview.md) — Clang 语义
  分析

按 "少而精" 原则, 加 NT 注释建议优先级:

1. [`CompilerInstance.cpp`](CompilerInstance.cpp) — 8-12 段 (中央协
   调器, 整个 frontend 入口)
2. [`CompilerInvocation.cpp`](CompilerInvocation.cpp) — 5-8 段
   (`-cc1` argv 解析 + 所有 *Options 子对象填充)
3. [`FrontendAction.cpp`](FrontendAction.cpp) — 6-10 段 (生命周期框
   架)
4. [`FrontendActions.cpp`](FrontendActions.cpp) — 5-8 段 (标准 Action
   集合)
5. [`ASTUnit.cpp`](ASTUnit.cpp) — 6-10 段 (libclang / clangd 入口)
6. [`InitPreprocessor.cpp`](InitPreprocessor.cpp) — 4-6 段 (默认
   builtin 宏填法)
7. [`DiagnosticRenderer.cpp`](DiagnosticRenderer.cpp) — 4-6 段 (渲染
   基类)
8. [`VerifyDiagnosticConsumer.cpp`](VerifyDiagnosticConsumer.cpp) —
   4-6 段 (verify 机制)
9. [`PrecompiledPreamble.cpp`](PrecompiledPreamble.cpp) — 4-6 段
   (PCH 缓存)
10. [`DependencyFile.cpp`](DependencyFile.cpp) — 3-5 段 (`-M` 依赖)
11. [`Rewrite/FixItRewriter.cpp`](Rewrite/FixItRewriter.cpp) — 3-5 段
    (`-fixit`)
12. [`Rewrite/RewriteObjC.cpp`](Rewrite/RewriteObjC.cpp) +
    [`Rewrite/RewriteModernObjC.cpp`](Rewrite/RewriteModernObjC.cpp)
    — 各 3-5 段

外部 (FrontendTool) 加 NT 注释建议 (作为姊妹库):

13. [`FrontendTool/ExecuteCompilerInvocation.cpp`](../FrontendTool/ExecuteCompilerInvocation.cpp)
    — 4-6 段 (`clang-cc1` 驱动入口 + ActionKind 分派 + 插件加载)

---

**姊妹文档**: 本目录对应 LLVM 流水线中的 **Clang 编译前端协调层**,
是 Driver 通过 `clang -cc1` 进入 Clang 后第一个被调用的库。它与
[`clang/lib/Basic/`](../Basic/0-overview.md) 配合定义所有 `*Options`
+ DiagnosticEngine, 与 [`clang/lib/AST/`](../AST/0-overview.md) 配合
装配 ASTContext + ASTConsumer, 与 [`clang/lib/Sema/`](../Sema/0-overview.md)
配合触发语义分析, 与 [`clang/lib/CodeGen/`](../CodeGen/0-overview.md)
配合 codegen, 与 [`clang/lib/Driver/`](../Driver/0-overview.md) 之间
通过 `clang -cc1` argv 字符串通信。Frontend 自身不实现 Lexer /
Parser / Sema / CodeGen, 仅 **编排** 它们的执行流水线。

**FrontendTool** ([`clang/lib/FrontendTool/`](../FrontendTool/))
是 `clang-cc1` 的 **驱动壳** 子库 (`clangFrontendTool`), 单文件
实现 `ExecuteCompilerInvocation` + `CreateFrontendAction` —— 把本
目录定义的 `frontend::ActionKind` 派发到具体 `FrontendAction` 子
类 (含 `clang/lib/CodeGen/` / `clang/lib/Rewrite/Frontend/` /
`clang/lib/StaticAnalyzer/Frontend/` /
`clang/lib/ExtractAPI/` / `clang/lib/ScalableStaticAnalysis/Frontend/`
/ `clang/lib/CIR/FrontendAction/` 等)。它从本目录拆出是为了最小化
`clangFrontend` 的依赖 (本目录不需要包含所有 Action 实现的头), 让
`clang-tidy` / `clang-refactor` 等 libclang-style 工具可以不链
`clangFrontendTool` 就使用 `clangFrontend` 的核心组件。
