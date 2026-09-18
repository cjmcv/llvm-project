<!-- <NT>overview:clang/lib/Driver/ -->

# Clang Driver 库导读 — `clang/lib/Driver/`

> 本文档梳理 `clang/lib/Driver/` 目录下所有源文件 (19 个顶层 .cpp/.h
> + 97 个 `ToolChains/` 子目录 .cpp/.h + 28 个 `ToolChains/Arch/` 子
> 目录 .cpp/.h = 144 个) 的职责、上下游与推荐阅读顺序。
>
> 目标读者: 想理解 **Clang 编译器驱动** (命令行解析、目标平台分发、
> Action DAG 构造、JobList 调度) 的开发者, 以及要给 Clang 加新
> toolchain / 新 arch flag / 新 GPU offload 后端的人。
>
> 所有路径相对 `clang/lib/Driver/`。同名公开头文件位于
> `clang/include/clang/Driver/` (如 `Driver.h`、`ToolChain.h`、
> `Compilation.h` 等)。

---

## §0. Clang Driver 库在编译流水线中的位置

`clang/lib/Driver` 是 Clang 的 **命令行驱动**, 负责把 `clang -X -Y
...` 命令行解析成 **Action DAG**, 选 **ToolChain**, 生成 **JobList**,
最后 `ExecuteCompilation` 启动子进程 (cc1 / llvm-ld / ptxas 等)。
Driver 是 Clang 用户最先接触的入口, 但它本身 **不编译任何代码** ——
它只 **调度** 真正的编译子任务。

它在 Clang 内部的层次:

```
┌─────────────────────────────────────────────────────────┐
│ 用户: clang -target x86_64-linux-gnu -O2 -fopenmp ...   │
└─────────────────────────────────────────────────────────┘
                │
                ▼
┌─────────────────────────────────────────────────────────┐
│ clang/lib/Driver  (本目录) ← 你在这里                    │
│   · Driver.cpp: 顶层 driver, BuildCompilation /          │
│     ExecuteCompilation                                  │
│   · Compilation.cpp + Job.cpp + Action.cpp: Action DAG  │
│     + JobList 调度                                      │
│   · Phases.cpp + Types.cpp: 编译阶段 + 文件类型注册     │
│   · ToolChain.cpp + ToolChains/*.cpp: 平台抽象, 选      │
│     driver 实例 (gcc/clang/darwin/cuda/amd/...)         │
│   · ToolChains/Arch/*.cpp: per-arch -mcpu/-march/-mabi  │
│     解析                                                │
│   · SanitizerArgs.cpp / XRayArgs.cpp: -fsanitize=... /   │
│     -fxray-* 解析                                       │
│   · Multilib.cpp / MultilibBuilder.cpp: GCC-style 多    │
│     ABI 选择                                            │
│   · OffloadBundler.cpp: clang-offload-bundler library    │
│   · ModulesDriver.cpp: C++20 named-module 调度          │
│   · CreateInvocationFromArgs.cpp /                       │
│     CreateASTUnitFromArgs.cpp: library entry (libclang / │
│     clangd / tooling 用)                                │
└─────────────────────────────────────────────────────────┘
                │
                ▼
┌─────────────────────────────────────────────────────────┐
│ 调度的子进程:                                            │
│   · clang -cc1 (Preprocess / Compile / Backend)         │
│   · clang -cc1 -fopenmp -fopenmp-is-target-device (GPU) │
│   · llvm-ld / ld / link / lld / ar / ranlib              │
│   · ptxas / nvlink / hipcc / clang-offload-bundler /    │
│     spirv-link / llvm-spirv                             │
│   · dxc / dxil-spirv / ifsmerger (HLSL)                  │
└─────────────────────────────────────────────────────────┘
                │
                ▼
┌─────────────────────────────────────────────────────────┐
│ 子进程内: clang/lib/Frontend + clang/lib/Lex +           │
│ clang/lib/Parse + clang/lib/AST + clang/lib/Sema +      │
│ clang/lib/CodeGen + clang/lib/Basic                    │
└─────────────────────────────────────────────────────────┘
```

### §0.1 公开接口 (`clang/include/clang/Driver/`)

本目录 `.cpp` 文件依赖的 **公开头** 在
[`clang/include/clang/Driver/`](../../include/clang/Driver/), 最关键的:

- [`Driver.h`](../../include/clang/Driver/Driver.h) — `class Driver`
  顶层 API。
- [`Compilation.h`](../../include/clang/Driver/Compilation.h) —
  `class Compilation` 每调用上下文。
- [`ToolChain.h`](../../include/clang/Driver/ToolChain.h) — `class
  ToolChain` 平台抽象。
- [`Tool.h`](../../include/clang/Driver/Tool.h) — `class Tool` 工具子
  命令单元。
- [`Job.h`](../../include/clang/Driver/Job.h) — `class Command` /
  `class JobList`。
- [`Action.h`](../../include/clang/Driver/Action.h) — Action DAG 基类
  + 子类。
- [`Phases.h`](../../include/clang/Driver/Phases.h) —
  `enum phases::ID`。
- [`Types.h`](../../include/clang/Driver/Types.h) — `enum types::ID`。
- [`Multilib.h`](../../include/clang/Driver/Multilib.h) +
  [`MultilibBuilder.h`](../../include/clang/Driver/MultilibBuilder.h)
  — GCC multilib 选择。
- [`Distro.h`](../../include/clang/Driver/Distro.h) — Linux 发行版
  检测。
- [`SanitizerArgs.h`](../../include/clang/Driver/SanitizerArgs.h) +
  [`XRayArgs.h`](../../include/clang/Driver/XRayArgs.h) — 插桩选项
  解析。
- [`CudaInstallationDetector.h`](../../include/clang/Driver/CudaInstallationDetector.h)
  + [`RocmInstallationDetector.h`](../../include/clang/Driver/RocmInstallationDetector.h)
  + [`SyclInstallationDetector.h`](../../include/clang/Driver/SyclInstallationDetector.h)
  — GPU SDK 自动探测。
- [`CreateInvocationFromArgs.h`](../../include/clang/Driver/CreateInvocationFromArgs.h)
  — library 入口。

### §0.2 与 `clang/tools/clang/main.cpp` 的关系

`clang/tools/clang/main.cpp` (`clang_main` 函数) 是 Clang 可执行文件
入口, 它 `new Driver`, 调 `BuildCompilation` + `ExecuteCompilation`,
捕获错误并 `clang::executeCC1Tool` (走 -cc1 模式)。

### §0.3 与 `libclang` / `clangd` 的关系

[`CreateInvocationFromArgs.cpp`](CreateInvocationFromArgs.cpp) +
[`CreateASTUnitFromArgs.cpp`](CreateASTUnitFromArgs.cpp) 提供 **library
入口**, 让 libclang (`CIndex.cpp`) / `clangd` / `clang-tidy` / `clangd
indexer` 能直接从字符串数组构造 `CompilerInvocation` / `ASTUnit` 而无
需走真实 driver 命令行。

### §0.4 与 [`clang/lib/Frontend/`](../Frontend/) 的关系

Driver 调子进程 `clang -cc1 ...` 进入 Frontend (经
[`clang/tools/clang/driver.cpp`](../../tools/clang/driver.cpp) →
[`ExecuteCC1Tool.cpp`](../Frontend/ExecuteCC1Tool.cpp))。Driver 的
输出 argv[] 直接传给 Frontend。

### §0.5 与 [`clang/lib/Basic/`](../Basic/) 的关系

Driver 产生 [`TargetInfo`](../Basic/Targets/X86.cpp) + [`SanitizerArgs`](../Basic/Sanitizers.cpp) +
[`LangOptions`](../Basic/LangOptions.cpp) + [`Builtin::Context`](../Basic/Builtins.cpp) 等,
它们由 Frontend 用作子进程 `clang -cc1` 的一部分 argv。但 Driver 本身
**不直接调用** Basic 层的 `.cpp` —— 它只把它生成的参数传给子进程。

---

## §1. 编译流水线概览

```
                clang -O2 -target x86_64-linux-gnu -fopenmp foo.c
                              │
                              ▼
┌─────────────────────────────────────────────────────────┐
│ Driver.cpp::main (clang_main, in clang/tools)            │
│   └─ Driver::BuildCompilation (Driver.cpp)              │
│        ├─ parse args → InputArgList                     │
│        ├─ host Triple 检测                              │
│        ├─ select host ToolChain                          │
│        │    (Gnu / Darwin / MSVC / ...)                │
│        ├─ select offload toolchains (CUDA / HIP / SYCL) │
│        └─ BuildActions (Action DAG)                      │
│             ├─ InputAction                              │
│             ├─ PreprocessAction → BackendAction →        │
│             │  AssembleAction → LinkAction              │
│             ├─ BindArchAction (for multi-arch)           │
│             └─ OffloadAction (for GPU offload)           │
└─────────────────────────────────────────────────────────┘
                │
                ▼
┌─────────────────────────────────────────────────────────┐
│ Driver::BuildJobs (Driver.cpp)                          │
│   · for each Action → ToolChain::getTool                │
│     (e.g. Clang / gcc::Compiler / darwin::Linker)       │
│   · Tool::ConstructJob renders argv[]                  │
│     · Clang::ConstructJob → argv[] = {clang, -cc1, ...} │
│     · gnutools::Linker::ConstructJob →                  │
│       argv[] = {ld.lld, -pie, -o, foo, foo.o, ...}      │
│   · JobList 收集                                        │
└─────────────────────────────────────────────────────────┘
                │
                ▼
┌─────────────────────────────────────────────────────────┐
│ Compilation::ExecuteJobs (Compilation.cpp + Job.cpp)    │
│   · ThreadPool parallel Execute                         │
│   · Command::Execute → llvm::sys::ExecuteAndWait         │
│        ├─ spawn 子进程 (cc1 / ld / ptxas / ...)         │
│        ├─ 收集 stdout / stderr                          │
│        └─ 异常 → generateCompilationDiagnostics (Crash   │
│           report)                                       │
└─────────────────────────────────────────────────────────┘
                │
                ▼
┌─────────────────────────────────────────────────────────┐
│ Driver 退出码 + 编译统计                                │
└─────────────────────────────────────────────────────────┘

辅助入口:
- ToolChains/Clang.cpp: 把所有 driver flag → cc1 arg (-cc1 的"翻译器")
- ToolChains/Arch/*.cpp: -mcpu/-march/-mabi/-m<feature> 解析
- ToolChains/{Cuda,AMDGPU,HIPAMD,HIPSPV,SYCL,SPIRV}.cpp:
  GPU offload 子工具链
- SanitizerArgs / XRayArgs / Multilib / Distro / ModulesDriver:
  横向横切模块
- OffloadBundler.cpp: fat-binary 库
- CreateInvocationFromArgs / CreateASTUnitFromArgs: library 入口
```

---

## §2. 文件目录结构

```
clang/lib/Driver/  (144 文件, 2 子目录 ToolChains/ + ToolChains/Arch/)
├── §3.1  Driver core / scheduler                     (8 文件)
├── §3.2  Argument / phase / type registry             (3 文件)
├── §3.3  Cross-cutting features                        (5 文件)
├── §3.4  Library entry points                         (2 文件)
├── §3.5  Multilib / distro                            (3 文件)
├── §3.6  ToolChains — core / generic helpers           (6 文件)
├── §3.7  ToolChains — GCC-family base + helpers        (3 文件)
├── §3.8  ToolChains — Linux / Unix-likes              (18 文件)
├── §3.9  ToolChains — Apple Mach-O                     (2 文件)
├── §3.10 ToolChains — Windows family                   (5 文件)
├── §3.11 ToolChains — GPU offload (CUDA/HIP/SYCL/SPIR-V) (8 文件)
├── §3.12 ToolChains — Bare-metal / embedded / vendor   (15 文件)
├── §3.13 ToolChains/Arch/ — per-arch feature (14 arch)  (28 文件)
└── §3.14 Build / misc                                  (1 文件 CMakeLists.txt)
```

### 2.1 文件数量统计

| 区域 | .cpp | .h | 合计 |
|------|------|-----|------|
| Driver core / scheduler | 8 | 0 | 8 |
| Argument / phase / type registry | 3 | 0 | 3 |
| Cross-cutting features (Sanitizer/XRay/OffloadBundler/ModulesDriver) | 4 | 0 | 4 |
| Library entry points | 2 | 0 | 2 |
| Multilib / distro | 3 | 0 | 3 |
| ToolChains — core / generic | 5 | 1 | 6 |
| ToolChains — GCC-family | 2 | 1 | 3 |
| ToolChains — Linux / Unix-likes | 16 | 0 | 16 |
| ToolChains — Apple | 1 | 1 | 2 |
| ToolChains — Windows | 4 | 1 | 5 |
| ToolChains — GPU offload | 7 | 1 | 8 |
| ToolChains — Bare-metal / embedded / vendor | 13 | 1 | 14 |
| ToolChains/Arch/ — 14 arch | 14 | 14 | 28 |
| Build / misc | 1 (`CMakeLists.txt`) | 0 | 1 |
| **总计** | **~82** | **~21** | **~144** + `CMakeLists.txt` |

---

## §3. 文件详解

### 3.1 Driver core / scheduler

[`Driver.cpp`](Driver.cpp) — 顶层 driver: 解析参数、选 host
ToolChain、建 Action DAG 和 JobList, 然后启 `ExecuteCompilation`。
- 上游: [`clang/tools/clang/main.cpp`](../../tools/clang/main.cpp)
  (经 Driver API)。
- 下游: [`Driver.h`](../../include/clang/Driver/Driver.h),
  [`ToolChains/Gnu.h`](ToolChains/Gnu.h),
  [`ToolChains/Darwin.h`](ToolChains/Darwin.h)。
- 关键类/函数: `class Driver`, `BuildCompilation`,
  `ExecuteCompilation`, `BuildActions`, `BuildJobs`,
  `generateCompilationDiagnostics`。

[`Compilation.cpp`](Compilation.cpp) — 拥有 per-invocation 状态:
InputArgList、TranslatedArgs、有序 offload toolchains 和 JobList;
经 ThreadPool 并行跑 jobs。
- 上游: [`Driver.cpp`](Driver.cpp) (BuildCompilation /
  ExecuteCompilation)。
- 下游: [`Compilation.h`](../../include/clang/Driver/Compilation.h),
  [`Job.h`](../../include/clang/Driver/Job.h),
  [`llvm/Support/ThreadPool.h`](../../../llvm/include/llvm/Support/ThreadPool.h)。
- 关键类/函数: `class Compilation`, `CleanupFileList`, `ExecuteJobs`,
  `addOffloadDeviceToolChain`, `getJobs`。

[`Job.cpp`](Job.cpp) — `Command` / `JobList` 运行时: 由 `Inputs` 建
argv、跳过 repro flag、经 `llvm::sys::ExecuteAndWait` 执行; 支持
IOSandbox 隔离与 crash-context。
- 上游: [`Compilation.cpp`](Compilation.cpp)。
- 下游: [`Job.h`](../../include/clang/Driver/Job.h),
  [`llvm/Support/Program.h`](../../../llvm/include/llvm/Support/Program.h),
  [`llvm/Support/CrashRecoveryContext.h`](../../../llvm/include/llvm/Support/CrashRecoveryContext.h)。
- 关键类/函数: `class Command`, `class JobList`, `Execute`, `Print`,
  `ResponseFileSupport`。

[`Action.cpp`](Action.cpp) — 抽象 `Action` class 体系
(`Input`/`BindArch`/`Offload`/`Preprocess`/`Compile`/`Assemble`/`Link`/...);
沿 DAG 传播 device offload 信息并序列化 kind。
- 上游: [`Driver.cpp`](Driver.cpp) (BuildActions),
  [`ToolChain.cpp`](ToolChain.cpp)。
- 下游: [`Action.h`](../../include/clang/Driver/Action.h)。
- 关键类/函数: `class Action`, `class InputAction`,
  `class BindArchAction`, `class OffloadAction`, `class JobAction`,
  `propagateDeviceOffloadInfo`, `getClassName`。

[`Tool.cpp`](Tool.cpp) — 小: `Tool` 构造函数/包装, 给每个 per-platform
`Tool` 子类 (`Clang`、`gcc::Compiler` 等) 用于 `ConstructJob` 分发。
- 上游: [`ToolChains/Clang.cpp`](ToolChains/Clang.cpp),
  [`ToolChains/Gnu.cpp`](ToolChains/Gnu.cpp),
  [`ToolChains/Darwin.cpp`](ToolChains/Darwin.cpp)。
- 下游: [`Tool.h`](../../include/clang/Driver/Tool.h)。
- 关键类/函数: `class Tool`, `ConstructJob`,
  `hasIntegratedAssembler`, `isLinkJob`。

[`Phases.cpp`](Phases.cpp) — 把 phase ID
(`Preprocess`/`Compile`/`Backend`/`Assemble`/`Link`/`IfsMerge`) 映射
到显示字符串; 逻辑少。
- 上游: [`Driver.cpp`](Driver.cpp)。
- 下游: [`Phases.h`](../../include/clang/Driver/Phases.h)。
- 关键类/函数: `phases::ID`, `getPhaseName`。

[`Types.cpp`](Types.cpp) — 文件类型注册表: 名称/后缀、preprocessor/
precompiled 派生类型; 驱动 input-type-to-action 查找。
- 上游: [`Driver.cpp`](Driver.cpp),
  [`ToolChains/Clang.cpp`](ToolChains/Clang.cpp)。
- 下游: [`Types.def`](../../include/clang/Driver/Types.def)
  (TableGen macro),
  [`Types.h`](../../include/clang/Driver/Types.h)。
- 关键类/函数: `types::ID`, `getTypeName`, `getPreprocessedType`,
  `lookupType`。

### 3.2 Argument / phase / type registry (顶层)

(Phases.cpp / Types.cpp / Tool.cpp 见 §3.1)

### 3.3 Cross-cutting features

[`SanitizerArgs.cpp`](SanitizerArgs.cpp) — 把
`-fsanitize=...`/`-fno-sanitize=...` 解析成 `SanitizerMask`; 强制
target/rT 支持与 per-sanitizer runtime/link 需求。
- 上游: [`Driver.cpp`](Driver.cpp),
  [`ToolChains/Darwin.cpp`](ToolChains/Darwin.cpp),
  [`ToolChains/Clang.cpp`](ToolChains/Clang.cpp)。
- 下游: [`Sanitizers.h`](../../include/clang/Basic/Sanitizers.h)
  (定义),
  [`llvm/Transforms/Instrumentation/AddressSanitizerOptions.h`](../../../llvm/include/llvm/Transforms/Instrumentation/AddressSanitizerOptions.h)。
- 关键类/函数: `class SanitizerArgs`, `SanitizerMask`, `needsPIE`,
  `requiresPIEWithASLR`, `addArgs`。

[`XRayArgs.cpp`](XRayArgs.cpp) — 解析 `-fxray-instrument` 及相关;
强制 per-arch 支持矩阵与选 xray-fdr vs xray-basic 模式。
- 上游: [`ToolChains/Clang.cpp`](ToolChains/Clang.cpp),
  [`ToolChains/Gnu.cpp`](ToolChains/Gnu.cpp)。
- 下游: [`XRayArgs.h`](../../include/clang/Driver/XRayArgs.h),
  [`llvm/Support/SpecialCaseList.h`](../../../llvm/include/llvm/Support/SpecialCaseList.h)。
- 关键类/函数: `class XRayArgs`, `XRayInstrument`, `XRayShared`,
  `XRaySupportedModes`。

[`OffloadBundler.cpp`](OffloadBundler.cpp) — 独立 bundler/unbundler
for fat offload images (`clang-offload-bundler` library), 用
`llvm::object::OffloadBundle` 格式。
- 上游: [`ToolChains/Clang.cpp`](ToolChains/Clang.cpp)
  (OffloadBundler tool), [`Driver.cpp`](Driver.cpp) (offload path)。
- 下游: [`llvm/Object/OffloadBundle.h`](../../../llvm/include/llvm/Object/OffloadBundle.h),
  [`llvm/Support/Compression.h`](../../../llvm/include/llvm/Support/Compression.h),
  [`OffloadArch.h`](../../include/clang/Basic/OffloadArch.h)。
- 关键类/函数: `class OffloadTargetInfo`,
  `class OffloadBundlerConfig`, `bundle`, `unbundle`,
  `OFFLOAD_BUNDLER_MAGIC_STR`。

[`ModulesDriver.cpp`](ModulesDriver.cpp) — Driver-managed C++20
named-module build 支持: dependency graph extraction、scheduler、
manifest JSON I/O、module-name collision 诊断。
- 上游: [`Driver.cpp`](Driver.cpp),
  [`Compilation.cpp`](Compilation.cpp)。
- 下游: [`clang/DependencyScanning/DependencyScanningUtils.h`](../../include/clang/DependencyScanning/DependencyScanningUtils.h),
  [`llvm/ADT/DirectedGraph.h`](../../../llvm/include/llvm/ADT/DirectedGraph.h)。
- 关键类/函数: `class ModulesDriver`, `class StdModuleManifest`,
  `compileModule`, `diagnoseModulesDriverArgs`, `class DepsScanner`。

### 3.4 Library entry points

[`CreateInvocationFromArgs.cpp`](CreateInvocationFromArgs.cpp) —
library 入口: 由 fake argv 建 `Driver`、`BuildCompilation`, 然后转发
到 `CompilerInvocation`; 支持 `-fsyntax-only` 嵌入。
- 上游: [`CompilerInstance.cpp`](../Frontend/CompilerInstance.cpp),
  [`Tooling.cpp`](../../tooling/Tooling.cpp)。
- 下游: [`Driver.h`](../../include/clang/Driver/Driver.h),
  [`Frontend/CompilerInstance.h`](../../include/clang/Frontend/CompilerInstance.h)。
- 关键类/函数: `createInvocation`, `class CreateInvocationOptions`,
  `EmbedSource`。

[`CreateASTUnitFromArgs.cpp`](CreateASTUnitFromArgs.cpp) — 高级
helper: 把 args 解析成 `CompilerInvocation`、parse source、返回完全形
成的 `ASTUnit` (给 libclang/clangd 用)。
- 上游: `libclang` (`CIndex.cpp`)、`clangd`、`Tooling`。
- 下游:
  [`CreateInvocationFromArgs.h`](../../include/clang/Driver/CreateInvocationFromArgs.h),
  [`Frontend/ASTUnit.h`](../../include/clang/Frontend/ASTUnit.h)。
- 关键类/函数: `CreateASTUnitFromCommandLine`,
  `CreateInvocationFromArgs`。

### 3.5 Multilib / distro

[`Multilib.cpp`](Multilib.cpp) — GCC-style multilib 选择: 持有
GCCSuffix/OSSuffix/IncludeSuffix + flag set + exclusive group; 支持
YAML 序列化和 Regex 选择。
- 上游: [`MultilibBuilder.cpp`](MultilibBuilder.cpp),
  [`ToolChains/Gnu.cpp`](ToolChains/Gnu.cpp)。
- 下游: [`Multilib.h`](../../include/clang/Driver/Multilib.h),
  [`llvm/Support/YAMLTraits.h`](../../../llvm/include/llvm/Support/YAMLTraits.h),
  [`llvm/Support/Regex.h`](../../../llvm/include/llvm/Support/Regex.h)。
- 关键类/函数: `class Multilib`, `class MultilibSet`, `select`,
  `flagList`, `print`。

[`MultilibBuilder.cpp`](MultilibBuilder.cpp) — `Multilib` 描述文件的
builder + YAML 解析器; 把文本配置转成 `Multilib` / `MultilibSet`。
- 上游: [`Multilib.cpp`](Multilib.cpp),
  [`ToolChains/Gnu.cpp`](ToolChains/Gnu.cpp)。
- 下游: [`MultilibBuilder.h`](../../include/clang/Driver/MultilibBuilder.h),
  [`llvm/Support/YAMLParser.h`](../../../llvm/include/llvm/Support/YAMLParser.h)。
- 关键类/函数: `class MultilibBuilder`, `class MultilibSetBuilder`,
  `parseYaml`, `makeMultilibSet`。

[`Distro.cpp`](Distro.cpp) — 检测 host Linux 发行版 (Debian/Ubuntu/
Fedora/RHEL/SUSE/Alpine/...) 经 `/etc/os-release` 等。
- 上游: [`ToolChains/Linux.cpp`](ToolChains/Linux.cpp),
  [`Driver.cpp`](Driver.cpp)。
- 下游: [`Distro.h`](../../include/clang/Driver/Distro.h),
  [`llvm/Support/VirtualFileSystem.h`](../../../llvm/include/llvm/Support/VirtualFileSystem.h)。
- 关键类/函数: `class Distro`, `DistroType`, `DetectDistro`,
  `DetectOsRelease`, `DetectLsbRelease`。

### 3.6 ToolChains — core / generic helpers

[`ToolChains/Clang.cpp`](ToolChains/Clang.cpp) — "`clang -cc1` invoker"
(~10k lines): 把所有 driver flag 渲染为 cc1 args、target options、
ObjC runtime、language standard、preprocessing。
- 上游: [`Driver.cpp`](Driver.cpp) (ConstructJob 分发),
  [`ToolChains/Darwin.cpp`](ToolChains/Darwin.cpp)。
- 下游: `ToolChains/Arch/*`,
  [`ObjCRuntime.h`](../../include/clang/Basic/ObjCRuntime.h),
  [`CodeGenOptions.h`](../../include/clang/Basic/CodeGenOptions.h)。
- 关键类/函数: `class Clang`, `class ClangAs`,
  `class OffloadBundler`, `class OffloadPackager`,
  `class LinkerWrapper`, `ConstructJob`。

[`ToolChains/Clang.h`](ToolChains/Clang.h) — 声明 `Clang` / `ClangAs`
/ `OffloadBundler` / `OffloadPackager` / `LinkerWrapper` `Tool` 子
类 + helper 如 `getCXX20NamedModuleOutputPath`。
- 上游: [`ToolChains/Clang.cpp`](ToolChains/Clang.cpp)。
- 下游: [`ToolChains/MSVC.h`](ToolChains/MSVC.h)。
- 关键类/函数: `class Clang`, `class ClangAs`,
  `class OffloadBundler`, `class OffloadPackager`,
  `class LinkerWrapper`。

[`ToolChains/CommonArgs.cpp`](ToolChains/CommonArgs.cpp) — 共享
`-m`/`-f`/`-X` 参数翻译, 给所有 `ToolChain` 用: PIC/PIE、split-stack、
ARC、ASan、profile、libcpp/libstdc++、runtime libs。
- 上游: [`ToolChains/Gnu.cpp`](ToolChains/Gnu.cpp),
  [`ToolChains/Darwin.cpp`](ToolChains/Darwin.cpp),
  [`ToolChains/Linux.cpp`](ToolChains/Linux.cpp)。
- 下游: `ToolChains/Arch/*`,
  [`SanitizerArgs.h`](../../include/clang/Driver/SanitizerArgs.h)。
- 关键类/函数: `addCommonArgs`, `ParsePICArgs`,
  `addArchSpecificLLVMPasses`, `renderCommonOptions`。

[`ToolChains/Flang.cpp`](ToolChains/Flang.cpp) — `flang-new` 前端
invoker: 把 Fortran driver flag 映射到 `flang -fc1`、dependency
rendering、target-specific features。
- 上游: [`Driver.cpp`](Driver.cpp) (driver-mode=flang 时),
  [`ToolChains/Cuda.cpp`](ToolChains/Cuda.cpp)。
- 下游: [`ToolChains/Arch/RISCV.h`](ToolChains/Arch/RISCV.h),
  [`CodeGenOptions.h`](../../include/clang/Basic/CodeGenOptions.h)。
- 关键类/函数: `class Flang`, `ConstructJob`,
  `renderDependencyGenerationOptions`, `addTargetOptions`。

[`ToolChains/InterfaceStubs.cpp`](ToolChains/InterfaceStubs.cpp) —
小: 产生 `ifsmerger` 调用来 merge TBD/IFS stub 文件为统一 ELF stub 库。
- 上游: [`ToolChains/Gnu.cpp`](ToolChains/Gnu.cpp),
  [`ToolChains/Darwin.cpp`](ToolChains/Darwin.cpp)。
- 下游: [`Tool.h`](../../include/clang/Driver/Tool.h)。
- 关键类/函数: `class Merger`, `ConstructJob`。

[`ToolChains/HIPUtility.cpp`](ToolChains/HIPUtility.cpp) —
HIP offload 共享 helper (AMDGPU/SPIRV): fatbin command 构造、HIP
runtime include 检测。
- 上游: [`ToolChains/HIPAMD.cpp`](ToolChains/HIPAMD.cpp),
  [`ToolChains/HIPSPV.cpp`](ToolChains/HIPSPV.cpp)。
- 下游: [`ToolChains/AMDGPU.h`](ToolChains/AMDGPU.h)。
- 关键类/函数: `HIP::constructHIPFatbinCommand`,
  `HIP::detectHIPRuntime`。

### 3.7 ToolChains — GCC-family base + helpers

[`ToolChains/Gnu.cpp`](ToolChains/Gnu.cpp) — 通用 GCC toolchain:
GCC install 检测 (multilibs、Debian multiarch、Gentoo)、`--gcc-toolchain`/
`-stdlib++`/`-unwindlib` 参数翻译。
- 上游: [`Driver.cpp`](Driver.cpp),
  [`ToolChains/Linux.cpp`](ToolChains/Linux.cpp),
  [`ToolChains/FreeBSD.cpp`](ToolChains/FreeBSD.cpp)。
- 下游: `ToolChains/Arch/*`,
  [`Distro.h`](../../include/clang/Driver/Distro.h),
  [`MultilibBuilder.h`](../../include/clang/Driver/MultilibBuilder.h)。
- 关键类/函数: `class Generic_GCC`, `class GCCInstallationDetector`,
  `addMultilibFlag`, `TranslateArgs`, `addLibCxxIncludePaths`。

[`ToolChains/Gnu.h`](ToolChains/Gnu.h) — 声明 `Generic_GCC` /
`Generic_ELF` 基类 + `gnutools::{Assembler,Linker,StaticLibTool}`、
`gcc::{Preprocessor,Compiler,Linker}`、与 `GCCVersion` struct。
- 上游: [`ToolChains/Gnu.cpp`](ToolChains/Gnu.cpp),
  [`ToolChains/Linux.cpp`](ToolChains/Linux.cpp),
  [`ToolChains/FreeBSD.cpp`](ToolChains/FreeBSD.cpp)。
- 下游: [`LazyDetector.h`](../../include/clang/Driver/LazyDetector.h),
  [`CudaInstallationDetector.h`](../../include/clang/Driver/CudaInstallationDetector.h)。
- 关键类/函数: `class Generic_GCC`, `class Generic_ELF`,
  `class GCCInstallationDetector`, `class GCCVersion`。

(`ToolChains/CommonArgs.h` 不存在, 所有 CommonArgs helper 都在 `.cpp`
内 namespace `tools::` 中)

### 3.8 ToolChains — Linux / Unix-likes

[`ToolChains/Linux.cpp`](ToolChains/Linux.cpp) — Linux Generic_ELF:
multiarch triple 映射、per-arch dynamic linker 选择、Debian/Fedora/
SUSE include 路径。
- 上游: [`Driver.cpp`](Driver.cpp),
  [`ToolChains/Gnu.cpp`](ToolChains/Gnu.cpp)。
- 下游: [`ToolChains/Arch/ARM.h`](ToolChains/Arch/ARM.h),
  [`ToolChains/Arch/RISCV.h`](ToolChains/Arch/RISCV.h),
  [`Distro.h`](../../include/clang/Driver/Distro.h)。
- 关键类/函数: `class Linux`, `getMultiarchTriple`,
  `getDynamicLinker`, `AddClangSystemIncludeArgs`。

[`ToolChains/Hexagon.cpp`](ToolChains/Hexagon.cpp) — Qualcomm
Hexagon: HVX/SmallData 阈值、hexagon-as/hexagon-link 调用、Linux
sysroot 默认。
- 上游: [`ToolChains/Linux.cpp`](ToolChains/Linux.cpp),
  [`ToolChains/Clang.cpp`](ToolChains/Clang.cpp)。
- 下游: [`ToolChains/Hexagon.h`](ToolChains/Hexagon.h)。
- 关键类/函数: `class HexagonToolChain`, `isAutoHVXEnabled`,
  `GetHVXVersion`, `getCompilerRTPath`。

[`ToolChains/NetBSD.cpp`](ToolChains/NetBSD.cpp) — NetBSD
Generic_ELF: NetBSD include/lib 路径约定、exception model、libstdc++
路径。
- 上游: [`Driver.cpp`](Driver.cpp),
  [`ToolChains/Gnu.cpp`](ToolChains/Gnu.cpp)。
- 下游: [`ToolChains/NetBSD.h`](ToolChains/NetBSD.h)。
- 关键类/函数: `class NetBSD`, `GetExceptionModel`,
  `AddClangSystemIncludeArgs`。

[`ToolChains/OpenBSD.cpp`](ToolChains/OpenBSD.cpp) — OpenBSD
Generic_ELF: system include/lib 与 exception-model 默认; ld.lld 默认
linker。
- 上游: [`Driver.cpp`](Driver.cpp),
  [`ToolChains/Gnu.cpp`](ToolChains/Gnu.cpp)。
- 下游: [`ToolChains/OpenBSD.h`](ToolChains/OpenBSD.h)。
- 关键类/函数: `class OpenBSD`, `HasNativeLLVMSupport`,
  `getCompilerRT`。

[`ToolChains/FreeBSD.cpp`](ToolChains/FreeBSD.cpp) — FreeBSD
Generic_ELF: FreeBSD system include/lib/CUDA/HIP 路径、AMDGPU/CUDA
offloading 支持。
- 上游: [`Driver.cpp`](Driver.cpp),
  [`ToolChains/Gnu.cpp`](ToolChains/Gnu.cpp)。
- 下游: [`ToolChains/Cuda.h`](ToolChains/Cuda.h),
  [`ToolChains/FreeBSD.h`](ToolChains/FreeBSD.h)。
- 关键类/函数: `class FreeBSD`, `AddCudaIncludeArgs`,
  `AddHIPIncludeArgs`。

[`ToolChains/DragonFly.cpp`](ToolChains/DragonFly.cpp) —
DragonFlyBSD Generic_ELF: BSD include 路径、stdlib++ 处理。
- 上游: [`Driver.cpp`](Driver.cpp),
  [`ToolChains/Gnu.cpp`](ToolChains/Gnu.cpp)。
- 下游: [`ToolChains/DragonFly.h`](ToolChains/DragonFly.h)。
- 关键类/函数: `class DragonFly`, `AddClangSystemIncludeArgs`。

[`ToolChains/Haiku.cpp`](ToolChains/Haiku.cpp) — Haiku Generic_ELF:
libstdc++ include 路径、默认 linker ld.lld。
- 上游: [`Driver.cpp`](Driver.cpp),
  [`ToolChains/Gnu.cpp`](ToolChains/Gnu.cpp)。
- 下游: [`ToolChains/Haiku.h`](ToolChains/Haiku.h)。
- 关键类/函数: `class Haiku`, `AddClangSystemIncludeArgs`,
  `addLibCxxIncludePaths`。

[`ToolChains/Serenity.cpp`](ToolChains/Serenity.cpp) — SerenityOS
Generic_ELF: Serenity include/lib 约定。
- 上游: [`Driver.cpp`](Driver.cpp),
  [`ToolChains/Gnu.cpp`](ToolChains/Gnu.cpp)。
- 下游: [`ToolChains/Serenity.h`](ToolChains/Serenity.h)。
- 关键类/函数: `class Serenity`, `AddClangSystemIncludeArgs`。

[`ToolChains/Managarm.cpp`](ToolChains/Managarm.cpp) — Managarm
Generic_ELF: Managarm dynamic linker / sysroot 约定。
- 上游: [`Driver.cpp`](Driver.cpp),
  [`ToolChains/Gnu.cpp`](ToolChains/Gnu.cpp)。
- 下游: [`ToolChains/Managarm.h`](ToolChains/Managarm.h)。
- 关键类/函数: `class Managarm`, `getDynamicLinker`,
  `computeSysRoot`。

[`ToolChains/PPCFreeBSD.cpp`](ToolChains/PPCFreeBSD.cpp) — 小 stub:
PowerPC FreeBSD 子类 flag (多数逻辑继承 `FreeBSD`)。
- 上游: [`ToolChains/FreeBSD.cpp`](ToolChains/FreeBSD.cpp)。
- 下游: [`ToolChains/FreeBSD.h`](ToolChains/FreeBSD.h)。
- 关键类/函数: `class PPCFreeBSDToolChain`。

[`ToolChains/PPCLinux.cpp`](ToolChains/PPCLinux.cpp) — 小: PowerPC
Linux 子类 (大部分继承 `Generic_ELF` on Linux)。
- 上游: [`ToolChains/Linux.cpp`](ToolChains/Linux.cpp)。
- 下游: [`ToolChains/Linux.h`](ToolChains/Linux.h)。
- 关键类/函数: `class PPCLinuxToolChain`, `supportIBMLongDouble`。

[`ToolChains/MipsLinux.cpp`](ToolChains/MipsLinux.cpp) — MIPS Linux
子类: MIPS abi flag、libpath suffix 翻译。
- 上游: [`ToolChains/Linux.cpp`](ToolChains/Linux.cpp)。
- 下游: [`ToolChains/Arch/Mips.h`](ToolChains/Arch/Mips.h)。
- 关键类/函数: `class MipsLLVMToolChain`, `getCompilerRT`,
  `computeSysRoot`。

[`ToolChains/LFILinux.cpp`](ToolChains/LFILinux.cpp) — LFI 的 Linux:
同 `Generic_ELF` 但 default C++ stdlib 经 `AddCXXStdlibLibArgs` 覆盖。
- 上游: [`ToolChains/Linux.cpp`](ToolChains/Linux.cpp)。
- 下游: [`ToolChains/Linux.h`](ToolChains/Linux.h)。
- 关键类/函数: `class LFILinux`, `AddCXXStdlibLibArgs`。

[`ToolChains/OHOS.cpp`](ToolChains/OHOS.cpp) — OpenHarmony
Generic_ELF: OHOS dynamic linker、multiarch triple、OpenHarmony
include 约定。
- 上游: [`ToolChains/Gnu.cpp`](ToolChains/Gnu.cpp)。
- 下游: [`ToolChains/Gnu.h`](ToolChains/Gnu.h)。
- 关键类/函数: `class OHOS`, `getDynamicLinker`,
  `getMultiarchTriple`, `computeSysRoot`。

[`ToolChains/Hurd.cpp`](ToolChains/Hurd.cpp) — GNU/Hurd Generic_ELF:
Hurd dynamic linker / multiarch triple。
- 上游: [`ToolChains/Gnu.cpp`](ToolChains/Gnu.cpp)。
- 下游: [`ToolChains/Gnu.h`](ToolChains/Gnu.h)。
- 关键类/函数: `class Hurd`, `getDynamicLinker`,
  `getMultiarchTriple`。

[`ToolChains/Solaris.cpp`](ToolChains/Solaris.cpp) — Solaris
Generic_GCC: Solaris-specific linker/isystem 约定、gnu-ld 检测。
- 上游: [`ToolChains/Gnu.cpp`](ToolChains/Gnu.cpp)。
- 下游: [`ToolChains/CommonArgs.cpp`](ToolChains/CommonArgs.cpp)。
- 关键类/函数: `class Solaris`, `isLinkerGnuLd`,
  `AddClangSystemIncludeArgs`。

[`ToolChains/ZOS.cpp`](ToolChains/ZOS.cpp) — z/OS (IBM mainframe):
MetalC toolchain with z/OS USS include/lib 路径约定。
- 上游: [`Driver.cpp`](Driver.cpp)。
- 下游: [`ToolChains/ZOS.h`](ToolChains/ZOS.h)。
- 关键类/函数: `class ZOS`, `AddCXXStdlibLibArgs`,
  `TryAddIncludeFromPath`。

### 3.9 ToolChains — Apple Mach-O

[`ToolChains/Darwin.cpp`](ToolChains/Darwin.cpp) — Apple MachO
toolchain: SDK/lipo/dsymutil、多 arch (x86/arm64/arm64e)、
deployment-target/version 处理、ARC runtime。
- 上游: [`Driver.cpp`](Driver.cpp),
  [`ToolChains/Clang.cpp`](ToolChains/Clang.cpp)。
- 下游: [`ToolChains/Arch/ARM.h`](ToolChains/Arch/ARM.h),
  [`DarwinSDKInfo.h`](../../include/clang/Basic/DarwinSDKInfo.h),
  [`ObjCRuntime.h`](../../include/clang/Basic/ObjCRuntime.h)。
- 关键类/函数: `class Darwin`, `class DarwinClang`, `class MachO`,
  `AddDeploymentTarget`, `ComputeEffectiveClangTriple`,
  `addMinVersionArgs`。

[`ToolChains/Darwin.h`](ToolChains/Darwin.h) — Darwin 体系 (MachO →
AppleMachO → Darwin → DarwinClang) 头, 加 `darwin::*` `Tool` 子类
(`Assembler`、`Linker`、`Lipo`、`Dsymutil`、`VerifyDebug`)。
- 上游: [`ToolChains/Darwin.cpp`](ToolChains/Darwin.cpp)。
- 下游: [`DarwinSDKInfo.h`](../../include/clang/Basic/DarwinSDKInfo.h),
  [`XRayArgs.h`](../../include/clang/Driver/XRayArgs.h)。
- 关键类/函数: `class MachO`, `class AppleMachO`, `class Darwin`,
  `class DarwinClang`, `class MachOTool`。

### 3.10 ToolChains — Windows family

[`ToolChains/MSVC.cpp`](ToolChains/MSVC.cpp) — Microsoft Visual
Studio: vcvars 检测、link.exe 调用、WindowsSDK/UniversalCRT 路径发现、
ARM64X objcopy。
- 上游: [`Driver.cpp`](Driver.cpp),
  [`ToolChains/Cuda.cpp`](ToolChains/Cuda.cpp)。
- 下游: [`ToolChains/MSVC.h`](ToolChains/MSVC.h),
  [`ToolChains/Darwin.h`](ToolChains/Darwin.h)。
- 关键类/函数: `class MSVCToolChain`,
  `class visualstudio::Linker`, `AddMSVCStdlibIncludeArgs`,
  `ARM64XObjcopy`。

[`ToolChains/MSVC.h`](ToolChains/MSVC.h) — 声明 `MSVCToolChain` +
`visualstudio::{Assembler,Linker}`。
- 上游: [`ToolChains/MSVC.cpp`](ToolChains/MSVC.cpp)。
- 下游: [`ToolChains/Clang.h`](ToolChains/Clang.h)。
- 关键类/函数: `class MSVCToolChain`,
  `class visualstudio::Assembler`, `class visualstudio::Linker`。

[`ToolChains/MinGW.cpp`](ToolChains/MinGW.cpp) — MinGW (Windows
GCC): GCC linker 调用, 带 `-m32`/`-m64` 和 ld 默认。
- 上游: [`Driver.cpp`](Driver.cpp),
  [`ToolChains/Gnu.cpp`](ToolChains/Gnu.cpp)。
- 下游: [`ToolChains/MinGW.h`](ToolChains/MinGW.h)。
- 关键类/函数: `class MinGW`, `class tools::MinGW::Assembler`,
  `class tools::MinGW::Linker`。

[`ToolChains/CrossWindows.cpp`](ToolChains/CrossWindows.cpp) —
`CrossWindowsToolChain`: Windows-target GCC (mingw-w64) assembler/
linker 调用, 从 Unix host。
- 上游: [`Driver.cpp`](Driver.cpp),
  [`ToolChains/Gnu.cpp`](ToolChains/Gnu.cpp)。
- 下游: [`ToolChains/CrossWindows.h`](ToolChains/CrossWindows.h)。
- 关键类/函数: `class CrossWindowsToolChain`,
  `class CrossWindows::Assembler`, `class CrossWindows::Linker`。

[`ToolChains/Cygwin.cpp`](ToolChains/Cygwin.cpp) — Cygwin (Cygwin
GCC on Windows): exception model、Cygwin system include 约定。
- 上游: [`ToolChains/Gnu.cpp`](ToolChains/Gnu.cpp)。
- 下游: [`ToolChains/Cygwin.h`](ToolChains/Cygwin.h)。
- 关键类/函数: `class Cygwin`, `GetExceptionModel`,
  `addClangTargetOptions`。

### 3.11 ToolChains — GPU offload (CUDA / HIP / SYCL / SPIR-V)

[`ToolChains/Cuda.cpp`](ToolChains/Cuda.cpp) — NVPTX/CUDA: CUDA
install 检测 (version、arch)、ptxas/fatbinary/nvlink 调用、CUDA
arch/sanitizer 处理。
- 上游: [`Driver.cpp`](Driver.cpp),
  [`ToolChains/CommonArgs.cpp`](ToolChains/CommonArgs.cpp)。
- 下游: [`ToolChains/Cuda.h`](ToolChains/Cuda.h),
  [`CudaInstallationDetector.h`](../../include/clang/Driver/CudaInstallationDetector.h)。
- 关键类/函数: `class CudaToolChain`, `class NVPTXToolChain`,
  `class CudaInstallationDetector`, `getSystemGPUArchs`,
  `AddCudaIncludeArgs`。

[`ToolChains/AMDGPU.cpp`](ToolChains/AMDGPU.cpp) — ROCm/AMDGPU: HIP
install 检测、device library 扫描、AMDGCN linker、OpenCL device
build、target ID 解析。
- 上游: [`Driver.cpp`](Driver.cpp),
  [`ToolChains/Clang.cpp`](ToolChains/Clang.cpp)。
- 下游: [`ToolChains/AMDGPU.h`](ToolChains/AMDGPU.h),
  [`RocmInstallationDetector.h`](../../include/clang/Driver/RocmInstallationDetector.h),
  [`TargetID.h`](../../include/clang/Basic/TargetID.h)。
- 关键类/函数: `class AMDGPUToolChain`,
  `class RocmInstallationDetector`, `class amdgpu::Linker`,
  `getDeviceLibs`, `getParsedTargetID`。

[`ToolChains/HIPAMD.cpp`](ToolChains/HIPAMD.cpp) — AMD HIP-on-AMDGCN
子类 of AMDGPU: device link 流水线 (`llvm-link`/`opt`/`llc`/`lld` →
amdgcn-link)。
- 上游: [`ToolChains/AMDGPU.cpp`](ToolChains/AMDGPU.cpp),
  [`Driver.cpp`](Driver.cpp)。
- 下游: [`ToolChains/AMDGPU.h`](ToolChains/AMDGPU.h)。
- 关键类/函数: `class AMDGCN::Linker`, `constructLldCommand`,
  `constructLLVMLinkCommand`。

[`ToolChains/HIPSPV.cpp`](ToolChains/HIPSPV.cpp) — AMD
HIP-on-SPIR-V target (HIPSPV): device link 到 SPIR-V shared object,
经 hipspv-link / llvm-spirv 退路。
- 上游: [`ToolChains/SPIRV.cpp`](ToolChains/SPIRV.cpp),
  [`Driver.cpp`](Driver.cpp)。
- 下游: [`ToolChains/SPIRV.h`](ToolChains/SPIRV.h),
  [`ToolChains/HIPUtility.h`](ToolChains/HIPUtility.h)。
- 关键类/函数: `class HIPSPVToolChain`, `class HIPSPV::Linker`,
  `constructLinkAndEmitSpirvCommand`。

[`ToolChains/SYCL.cpp`](ToolChains/SYCL.cpp) — SYCL install 检测:
找 oneAPI/SYCL runtime 库, 加 include/lib/cc1 args for offloading。
- 上游: [`ToolChains/CommonArgs.cpp`](ToolChains/CommonArgs.cpp),
  [`ToolChains/Darwin.cpp`](ToolChains/Darwin.cpp)。
- 下游: [`SyclInstallationDetector.h`](../../include/clang/Driver/SyclInstallationDetector.h)。
- 关键类/函数: `class SYCLInstallationDetector`,
  `addSYCLIncludeArgs`, `getSYCLDeviceLibs`。

[`ToolChains/SPIRV.cpp`](ToolChains/SPIRV.cpp) — 通用 SPIR-V
toolchain: spirv-as/spirv-link/llvm-spirv translator; 退路 when
LLVM 没有 SPIR-V backend。
- 上游: [`Driver.cpp`](Driver.cpp),
  [`ToolChains/Clang.cpp`](ToolChains/Clang.cpp)。
- 下游: [`ToolChains/SPIRV.h`](ToolChains/SPIRV.h)。
- 关键类/函数: `class SPIRVToolChain`, `class SPIRV::Translator`,
  `class SPIRV::Assembler`, `class SPIRV::Linker`, `SelectTool`。

[`ToolChains/SPIRVOpenMP.cpp`](ToolChains/SPIRVOpenMP.cpp) — 小:
SPIR-V OpenMP target toolchain 覆盖 (`addClangTargetOptions` for
OpenMP device codegen)。
- 上游: [`ToolChains/SPIRV.cpp`](ToolChains/SPIRV.cpp)。
- 下游: [`ToolChains/SPIRV.h`](ToolChains/SPIRV.h)。
- 关键类/函数: `class SPIRVOpenMPToolChain`。

### 3.12 ToolChains — Bare-metal / embedded / vendor

[`ToolChains/BareMetal.cpp`](ToolChains/BareMetal.cpp) — 通用
bare-metal (无 OS) with multilib 选择、sysroot 发现、startup/end
files for AArch64/ARM/RISC-V。
- 上游: [`Driver.cpp`](Driver.cpp),
  [`ToolChains/Gnu.cpp`](ToolChains/Gnu.cpp)。
- 下游: [`ToolChains/Arch/AArch64.h`](ToolChains/Arch/AArch64.h),
  [`ToolChains/Arch/RISCV.h`](ToolChains/Arch/RISCV.h),
  [`MultilibBuilder.h`](../../include/clang/Driver/MultilibBuilder.h)。
- 关键类/函数: `class BareMetal`, `class StaticLibTool`,
  `class Linker`, `computeSysRoot`, `getMultilibs`。

[`ToolChains/AVR.cpp`](ToolChains/AVR.cpp) — AVR 微控制器: MCU-family
查找、avr-ld 调用、AVR libc / libdevice 路径。
- 上游: [`Driver.cpp`](Driver.cpp),
  [`ToolChains/Gnu.cpp`](ToolChains/Gnu.cpp)。
- 下游: [`ToolChains/MSP430.h`](ToolChains/MSP430.h) (sibling),
  [`ToolChains/AVR.h`](ToolChains/AVR.h)。
- 关键类/函数: `class AVRToolChain`, `class AVR::Linker`,
  `GetMCUFamilyName`, `findAVRLibcInstallation`。

[`ToolChains/MSP430.cpp`](ToolChains/MSP430.cpp) — TI MSP430: linker
调用 with crt0 / end-of-program libraries、target feature emit。
- 上游: [`Driver.cpp`](Driver.cpp),
  [`ToolChains/Gnu.cpp`](ToolChains/Gnu.cpp)。
- 下游: [`ToolChains/MSP430.h`](ToolChains/MSP430.h)。
- 关键类/函数: `class MSP430ToolChain`, `class msp430::Linker`,
  `AddStartFiles`, `getMSP430TargetFeatures`。

[`ToolChains/CSKYToolChain.cpp`](ToolChains/CSKYToolChain.cpp) —
C-SKY: csky-as / csky-ld 调用、sysroot 计算、libstdc++ 路径。
- 上游: [`Driver.cpp`](Driver.cpp),
  [`ToolChains/Gnu.cpp`](ToolChains/Gnu.cpp)。
- 下游: [`ToolChains/Arch/CSKY.h`](ToolChains/Arch/CSKY.h)。
- 关键类/函数: `class CSKYToolChain`, `class CSKY::Linker`,
  `addClangTargetOptions`。

[`ToolChains/TCE.cpp`](ToolChains/TCE.cpp) — TCE (TTA-based
Co-Design Environment) 极简 toolchain: tce-cc、pic=false 默认。
- 上游: [`Driver.cpp`](Driver.cpp)。
- 下游: [`ToolChain.h`](../../include/clang/Driver/ToolChain.h)。
- 关键类/函数: `class TCEToolChain`, `IsMathErrnoDefault`。

[`ToolChains/UEFI.cpp`](ToolChains/UEFI.cpp) — UEFI (TianoCore)
Generic_ELF: UEFI include 路径约定与 lld-link 调用。
- 上游: [`ToolChains/Gnu.cpp`](ToolChains/Gnu.cpp)。
- 下游: [`ToolChains/UEFI.h`](ToolChains/UEFI.h)。
- 关键类/函数: `class UEFI`, `class tools::uefi::Linker`,
  `AddClangSystemIncludeArgs`。

[`ToolChains/XCore.cpp`](ToolChains/XCore.cpp) — XMOS XCore: xcc
toolchain、no PIC 默认、no profiler。
- 上游: [`Driver.cpp`](Driver.cpp)。
- 下游: [`ToolChains/XCore.h`](ToolChains/XCore.h)。
- 关键类/函数: `class XCoreToolChain`, `SupportsProfiling`。

[`ToolChains/AIX.cpp`](ToolChains/AIX.cpp) — IBM AIX: XCOFF assembler/
linker 调用、OpenMP include 处理、profile runtime libs。
- 上游: [`Driver.cpp`](Driver.cpp)。
- 下游: [`ToolChains/AIX.h`](ToolChains/AIX.h)。
- 关键类/函数: `class AIX`, `class aix::Assembler`,
  `class aix::Linker`, `AddFilePathLibArgs`, `AddOpenMPIncludeArgs`。

[`ToolChains/Fuchsia.cpp`](ToolChains/Fuchsia.cpp) — Google Fuchsia
Generic_ELF: fuchsia link/StaticLib 调用、sanitizer 默认、libcpp
profile。
- 上游: [`Driver.cpp`](Driver.cpp),
  [`ToolChains/Gnu.cpp`](ToolChains/Gnu.cpp)。
- 下游: [`ToolChains/Gnu.h`](ToolChains/Gnu.h)。
- 关键类/函数: `class Fuchsia`, `class fuchsia::Linker`,
  `class fuchsia::StaticLibTool`, `getDefaultSanitizers`。

[`ToolChains/HLSL.cpp`](ToolChains/HLSL.cpp) — HLSL/DirectX shader
toolchain: dxil-spirv/dxc/llvm-objcopy 调用、shader model 解析、
SPIR-V 扩展验证。
- 上游: [`Driver.cpp`](Driver.cpp)。
- 下游: [`ToolChains/HLSL.h`](ToolChains/HLSL.h)。
- 关键类/函数: `class HLSLToolChain`, `class Validator`,
  `class MetalConverter`, `class LLVMObjcopy`, `parseTargetProfile`。

[`ToolChains/PS4CPU.cpp`](ToolChains/PS4CPU.cpp) — Sony PlayStation
4/5: PS4PS5Base toolchain (profiling、stdlib++ 路径、no blocks-runtime)。
- 上游: [`Driver.cpp`](Driver.cpp),
  [`ToolChains/Gnu.cpp`](ToolChains/Gnu.cpp)。
- 下游: [`ToolChains/Gnu.h`](ToolChains/Gnu.h)。
- 关键类/函数: `class PS4PS5Base`, `class PS4CPU`, `class PS5CPU`。

[`ToolChains/VEToolchain.cpp`](ToolChains/VEToolchain.cpp) — NEC
SX-Aurora TSUBASA VE: Generic_ELF 变体 with VE-specific multiarch
triple 和 feature emit。
- 上游: [`Driver.cpp`](Driver.cpp),
  [`ToolChains/Gnu.cpp`](ToolChains/Gnu.cpp)。
- 下游: [`ToolChains/Arch/VE.h`](ToolChains/Arch/VE.h)。
- 关键类/函数: `class VEToolChain`, `AddCXXStdlibLibArgs`,
  `SupportsProfiling`。

[`ToolChains/WebAssembly.cpp`](ToolChains/WebAssembly.cpp) —
Emscripten/wasi WebAssembly: wasm-ld 调用、no PIC 默认、wasm
sysroot/include 处理。
- 上游: [`Driver.cpp`](Driver.cpp)。
- 下游: [`ToolChains/WebAssembly.h`](ToolChains/WebAssembly.h)。
- 关键类/函数: `class WebAssembly`, `class wasm::Linker`,
  `HasNativeLLVMSupport`, `addClangTargetOptions`。

[`ToolChains/Lanai.h`](ToolChains/Lanai.h) — 仅 header: Lanai
toolchain 声明 (`LanaiToolChain` source 缺失/未启用)。
- 上游: [`Driver.cpp`](Driver.cpp)。
- 下游: [`ToolChain.h`](../../include/clang/Driver/ToolChain.h)。
- 关键类/函数: `class LanaiToolChain`。

### 3.13 ToolChains/Arch/ — 14 个 per-arch feature (28 文件)

每个 arch 一个 `.cpp` + 一个 `.h`, 专门处理 `-mcpu`/`-march`/
`-mabi`/`-m<feature>` 解析。

| Arch | Path | 关键函数 |
|------|------|---------|
| AArch64 | [`ToolChains/Arch/AArch64.cpp`](ToolChains/Arch/AArch64.cpp) + `.h` | `getAArch64TargetCPU`, `getAArch64TargetFeatures`, `getAArch64TargetTuneCPU`, `isAArch64BareMetal` |
| AMDGPU | [`ToolChains/Arch/AMDGPU.cpp`](ToolChains/Arch/AMDGPU.cpp) + `.h` | `AMDGPU::getAMDGPUArchCPUFromArgs`, `setArchNameInTriple` (Tiny: 把 `-mcpu=...` 解析成 `AMDGPU::GPUKind` 与 Triple 规范化) |
| ARM | [`ToolChains/Arch/ARM.cpp`](ToolChains/Arch/ARM.cpp) + `.h` | `getARMArchCPUFromArgs`, `getARMTargetFeatures`, `isARMMProfile`, `isARMBigEndian`, `appendBE8LinkFlag` |
| CSKY | [`ToolChains/Arch/CSKY.cpp`](ToolChains/Arch/CSKY.cpp) + `.h` | `getCSKYTargetFeatures`, `setArchNameInTriple` |
| LoongArch | [`ToolChains/Arch/LoongArch.cpp`](ToolChains/Arch/LoongArch.cpp) + `.h` | `getLoongArchTargetCPU`, `postProcessTargetCPUString`, `getLoongArchTargetFeatures` |
| M68k | [`ToolChains/Arch/M68k.cpp`](ToolChains/Arch/M68k.cpp) + `.h` | `getM68kTargetCPU`, `getM68kTargetFeatures` |
| Mips | [`ToolChains/Arch/Mips.cpp`](ToolChains/Arch/Mips.cpp) + `.h` | `getMipsCPUAndABI`, `getMIPSTargetFeatures`, `getMipsABILibSuffix`, `shouldUseFPXX`, `hasCompactBranches` |
| PPC | [`ToolChains/Arch/PPC.cpp`](ToolChains/Arch/PPC.cpp) + `.h` | `getPPCTargetFeatures`, `hasPPCAbiArg` |
| RISCV | [`ToolChains/Arch/RISCV.cpp`](ToolChains/Arch/RISCV.cpp) + `.h` | `getRISCVTargetCPU`, `getRISCVTargetFeatures`, `getRISCVArch` |
| Sparc | [`ToolChains/Arch/Sparc.cpp`](ToolChains/Arch/Sparc.cpp) + `.h` | `getSparcTargetCPU`, `getSparcTargetFeatures` |
| SystemZ | [`ToolChains/Arch/SystemZ.cpp`](ToolChains/Arch/SystemZ.cpp) + `.h` | `getSystemZTargetCPU`, `getSystemZTargetFeatures` |
| VE | [`ToolChains/Arch/VE.cpp`](ToolChains/Arch/VE.cpp) + `.h` | `getVETargetFeatures` (Tiny) |
| X86 | [`ToolChains/Arch/X86.cpp`](ToolChains/Arch/X86.cpp) + `.h` | `getX86TargetCPU`, `getX86TargetFeatures` |

### 3.14 Build / misc

[`CMakeLists.txt`](CMakeLists.txt) — 列出 `clangDriver` library target
的所有 `.cpp` 文件。

---

## §4. 关键调用链

### 4.1 顶层 driver 启动链

```
clang_main [clang/tools/clang/main.cpp]
  └─ new Driver
       └─ Driver::BuildCompilation (Driver.cpp)
            ├─ 解析 argv → InputArgList (OptTable + ArgList)
            ├─ 检测 host Triple
            ├─ 选 host ToolChain (Linux / Darwin / MSVC / ...)
            │    ├─ Driver::getToolChain(llvm::Triple)
            │    ├─ createToolChain (e.g. new Gnu())
            │    └─ ToolChain::init(env, Args, Host)
            │         └─ ToolChain::ComputeSysRoot
            ├─ 选 offload toolchain (CUDA / HIP / SYCL / SPIRV)
            │    └─ addOffloadDeviceToolChain (Compilation.cpp)
            └─ BuildActions (Action DAG)
                 └─ BuildJobs
                      └─ Compile (Compilation.cpp)
                           └─ ExecuteCompilation
                                └─ Compilation::ExecuteJobs (Job.cpp)
                                     └─ JobList::Execute
                                          └─ Command::Execute → fork+exec
                                               ├─ 调子进程 (cc1 / ld / ...)
                                               └─ 收集 stdout/stderr
                                                    └─ generateCompilationDiagnostics
                                                         (crash report)
```

### 4.2 Tool::ConstructJob 渲染链

```
Driver::BuildJobs (Driver.cpp)
  ├─ 对每个 Action:
  │    ├─ ToolChain::getTool(ActionClass)
  │    │    └─ switch (ActionClass):
  │    │         ├─ Preprocess → Preprocessor Tool (Clang)
  │    │         ├─ Compile → Compiler Tool (Clang)
  │    │         ├─ Assemble → Assembler Tool (gnutools::Assembler)
  │    │         └─ Link → Linker Tool (gnutools::Linker)
  │    └─ Tool::ConstructJob (per Tool subclass)
  │         ├─ Clang::ConstructJob (ToolChains/Clang.cpp)
  │         │    ├─ translate all driver flags → cc1 args
  │         │    ├─ render TargetOptions / LangOptions / CodeGenOptions
  │         │    ├─ addCommonArgs (ToolChains/CommonArgs.cpp)
  │         │    │    ├─ PIC/PIE/unwind/sanitize/profile/...
  │         │    ├─ addArchSpecificLLVMPasses (per arch)
  │         │    │    ├─ ToolChains/Arch/X86.cpp
  │         │    │    ├─ ToolChains/Arch/AArch64.cpp
  │         │    │    ├─ ToolChains/Arch/ARM.cpp
  │         │    │    └─ ...
  │         │    └─ 输出 argv[] = {clang, -cc1, -triple, ...}
  │         ├─ gnutools::Linker::ConstructJob (ToolChains/Gnu.cpp)
  │         │    └─ 输出 argv[] = {ld.lld, -pie, -o, foo, foo.o, ...}
  │         ├─ darwin::Linker::ConstructJob (ToolChains/Darwin.cpp)
  │         ├─ MSVC::Linker::ConstructJob (ToolChains/MSVC.cpp)
  │         ├─ Cuda::Linker / NVPTX::Linker (ToolChains/Cuda.cpp)
  │         ├─ amdgpu::Linker / AMDGCN::Linker (ToolChains/AMDGPU.cpp / HIPAMD.cpp)
  │         └─ ...
  └─ JobList 收集所有 Command
```

### 4.3 GPU offload 处理链

```
Driver 检测 -fopenmp / -fcuda / -fhip / -fsycl / --offload-arch=...
  │
  ▼
AddOffloadDeviceToolChain (Compilation.cpp)
  ├─ 主 host ToolChain (e.g. Linux x86_64)
  └─ 创建 device ToolChain:
       ├─ -fopenmp → OpenMP-offload ToolChain
       ├─ -fcuda → CudaToolChain (CUDA host + NVPTXToolChain device)
       ├─ -fhip → AMDGPUToolChain (HIPAMD)
       ├─ -fsycl → SYCLToolChain (SYCL + SPIR-V device)
       └─ -fsycl-targets=amd_gpu → HIPSPVToolChain (HIP on SPIR-V)
            │
            ▼
BuildActions (Driver.cpp)
  └─ 添加 OffloadAction 包裹 CompileAction
       └─ 调度:
            ├─ Host CompileAction → host ToolChain::Clang::ConstructJob
            ├─ Device CompileAction → device ToolChain::Clang::ConstructJob
            │    (cl -cc1 -triple nvptx64-nvidia-cuda --offload ...)
            └─ OffloadBundler::ConstructJob
                 └─ merge host + device IRs into fat object
                      │
                      ▼
                 OffloadPackager / LinkerWrapper (host link)
```

### 4.4 Sanitizer / XRay 处理链

```
Driver 解析 -fsanitize=address,undefined ...
  │
  ▼
SanitizerArgs::SanitizerArgs (SanitizerArgs.cpp)
  ├─ parseSanitizerValue (-fsanitize=address,undefined 字符串)
  ├─ 检查 target 支持 (e.g. MSan 仅 x86_64)
  ├─ 检查 runtime support (e.g. sanitizer-runtime 库路径)
  ├─ 决定 needsPIE / requiresPIEWithASLR
  └─ 给 Compilation::Args 注入 cc1 + link args
       ├─ cc1: -fsanitize=address → Sema/CG 启用 ASan 注入
       └─ link: -fsanitize=address → 链 libclang_rt.asan

类似 XRayArgs::XRayArgs (XRayArgs.cpp)
  ├─ parseXRayInstrValue
  ├─ 检查 per-arch 支持 (e.g. X86 + AArch64 + ...)
  ├─ 选 mode (fdr / basic)
  └─ 给 Args 加 -fxray-instrument + -fxray-shared
       └─ cc1: → CGCodeGenFunction 插入 xray 桩
```

### 4.5 Multilib 选择链

```
ToolChain::init 调 Gnu::GCCInstallationDetector (ToolChains/Gnu.cpp)
  ├─ 找 GCC 安装 (e.g. /usr/lib/gcc/x86_64-linux-gnu/13/)
  ├─ 加载 multilib.yaml (MultilibBuilder.cpp 解析)
  │    └─ MultilibSetBuilder::parseYaml
  │         └─ 生成 MultilibSet (GCCSuffix + Flag + ExclusiveGroup)
  ├─ 根据 -march/-mabi 选 multilib:
  │    └─ MultilibSet::select (Multilib.cpp)
  │         ├─ flagList() 用 regex 匹配当前 Triple + flags
  │         └─ 返回最佳 Multilib
  └─ 添加 GCCSuffix + IncludeSuffix 到 sysroot
       e.g. /usr/lib/gcc/x86_64-linux-gnu/13/../../../../x86_64-linux-gnu/
```

### 4.6 Apple Mach-O 处理链

```
Driver 解析 -arch arm64 -arch x86_64 -isysroot ... -mmacosx-version-min=...
  │
  ▼
Darwin::MachO::ComputeEffectiveClangTriple (ToolChains/Darwin.cpp)
  ├─ AddDeploymentTarget (m32 / version-min / ...)
  ├─ 合并 multi-arch → multi-arg list
  ├─ Load DarwinSDKInfo (从 SDKSettings.json)
  │    └─ DarwinSDKInfo::parseJSON (Basic/DarwinSDKInfo.cpp)
  ├─ Lipo (universal binary)
  └─ ConstructJob:
       ├─ 每个 arch 一次 Clang::ConstructJob
       └─ lipo / dsymutil / verify-debug 调用
```

### 4.7 C++20 named-module 调度链

```
Driver 解析 -fmodules-ts / --module-name=foo / --std-modules-cache-path=...
  │
  ▼
ModulesDriver::compileModule (ModulesDriver.cpp)
  ├─ 解析 module manifest JSON (StdModuleManifest)
  │    └─ DepsScanner 找 module 依赖图
  ├─ 调度每个 module 编译:
  │    ├─ Action::Preprocess + Action::Compile + Action::Assemble
  │    └─ 经 ToolChain::Clang::ConstructJob
  ├─ module-name collision diagnostics
  └─ 输出 .pcm / .o 给 LinkAction
```

---

## §5. 推荐阅读顺序

### 阶段 1: 数据模型头 (30 分钟)
[`Phases.h`](../../include/clang/Driver/Phases.h) +
[`Types.h`](../../include/clang/Driver/Types.h) +
[`Action.h`](../../include/clang/Driver/Action.h) +
[`Job.h`](../../include/clang/Driver/Job.h) +
[`Tool.h`](../../include/clang/Driver/Tool.h) +
[`ToolChain.h`](../../include/clang/Driver/ToolChain.h) +
[`Driver.h`](../../include/clang/Driver/Driver.h) +
[`Compilation.h`](../../include/clang/Driver/Compilation.h) +
[`Multilib.h`](../../include/clang/Driver/Multilib.h) +
[`Distro.h`](../../include/clang/Driver/Distro.h) +
[`SanitizerArgs.h`](../../include/clang/Driver/SanitizerArgs.h) +
[`XRayArgs.h`](../../include/clang/Driver/XRayArgs.h)

### 阶段 2: 顶层 driver 核心 (2 小时)
- [`Driver.cpp`](Driver.cpp) — 理解 BuildCompilation +
  ExecuteCompilation
- [`Compilation.cpp`](Compilation.cpp) — per-invocation 容器
- [`Job.cpp`](Job.cpp) — Command 执行
- [`Action.cpp`](Action.cpp) — Action DAG
- [`Tool.cpp`](Tool.cpp) — Tool 基类
- [`Phases.cpp`](Phases.cpp) + [`Types.cpp`](Types.cpp) — phase + type
  注册表

### 阶段 3: 共享 arg-render helper (1.5 小时)
- [`ToolChains/CommonArgs.cpp`](ToolChains/CommonArgs.cpp) — 全平台
  通用参数
- [`ToolChains/Clang.cpp`](ToolChains/Clang.cpp) — `-cc1` invoker
  (~10k lines)
- [`ToolChains/Flang.cpp`](ToolChains/Flang.cpp) — `flang-new` invoker
- [`ToolChains/InterfaceStubs.cpp`](ToolChains/InterfaceStubs.cpp) —
  ifsmerger
- [`ToolChains/HIPUtility.cpp`](ToolChains/HIPUtility.cpp) — HIP 共享

### 阶段 4: GCC 基底 + multilib (1 小时)
- [`ToolChains/Gnu.cpp`](ToolChains/Gnu.cpp) +
  [`ToolChains/Gnu.h`](ToolChains/Gnu.h) — GCC family base
- [`Multilib.cpp`](Multilib.cpp) +
  [`MultilibBuilder.cpp`](MultilibBuilder.cpp) — multilib 选择
- [`Distro.cpp`](Distro.cpp) — Linux 发行版检测

### 阶段 5: per-arch feature (1.5 小时)
[`ToolChains/Arch/X86.cpp`](ToolChains/Arch/X86.cpp) +
[`ToolChains/Arch/ARM.cpp`](ToolChains/Arch/ARM.cpp) +
[`ToolChains/Arch/AArch64.cpp`](ToolChains/Arch/AArch64.cpp) +
[`ToolChains/Arch/RISCV.cpp`](ToolChains/Arch/RISCV.cpp) +
[`ToolChains/Arch/Mips.cpp`](ToolChains/Arch/Mips.cpp) +
[`ToolChains/Arch/PPC.cpp`](ToolChains/Arch/PPC.cpp) +
[`ToolChains/Arch/LoongArch.cpp`](ToolChains/Arch/LoongArch.cpp)

### 阶段 6: Linux / Unix-likes (1.5 小时)
[`ToolChains/Linux.cpp`](ToolChains/Linux.cpp) +
[`ToolChains/Hexagon.cpp`](ToolChains/Hexagon.cpp) +
[`ToolChains/FreeBSD.cpp`](ToolChains/FreeBSD.cpp) +
[`ToolChains/NetBSD.cpp`](ToolChains/NetBSD.cpp) +
[`ToolChains/OpenBSD.cpp`](ToolChains/OpenBSD.cpp) +
[`ToolChains/DragonFly.cpp`](ToolChains/DragonFly.cpp) +
[`ToolChains/Haiku.cpp`](ToolChains/Haiku.cpp) +
[`ToolChains/Solaris.cpp`](ToolChains/Solaris.cpp) +
[`ToolChains/OHOS.cpp`](ToolChains/OHOS.cpp) +
[`ToolChains/MipsLinux.cpp`](ToolChains/MipsLinux.cpp)

### 阶段 7: Apple Mach-O (1 小时)
[`ToolChains/Darwin.cpp`](ToolChains/Darwin.cpp) +
[`ToolChains/Darwin.h`](ToolChains/Darwin.h)

### 阶段 8: Windows family (30 分钟)
[`ToolChains/MSVC.cpp`](ToolChains/MSVC.cpp) +
[`ToolChains/MinGW.cpp`](ToolChains/MinGW.cpp) +
[`ToolChains/CrossWindows.cpp`](ToolChains/CrossWindows.cpp) +
[`ToolChains/Cygwin.cpp`](ToolChains/Cygwin.cpp)

### 阶段 9: GPU offload (2 小时)
[`ToolChains/Cuda.cpp`](ToolChains/Cuda.cpp) +
[`ToolChains/AMDGPU.cpp`](ToolChains/AMDGPU.cpp) +
[`ToolChains/HIPAMD.cpp`](ToolChains/HIPAMD.cpp) +
[`ToolChains/HIPSPV.cpp`](ToolChains/HIPSPV.cpp) +
[`ToolChains/SPIRV.cpp`](ToolChains/SPIRV.cpp) +
[`ToolChains/SYCL.cpp`](ToolChains/SYCL.cpp) +
[`OffloadBundler.cpp`](OffloadBundler.cpp)

### 阶段 10: Bare-metal / embedded / vendor (按需)
[`ToolChains/BareMetal.cpp`](ToolChains/BareMetal.cpp) +
[`ToolChains/AVR.cpp`](ToolChains/AVR.cpp) +
[`ToolChains/MSP430.cpp`](ToolChains/MSP430.cpp) +
[`ToolChains/CSKYToolChain.cpp`](ToolChains/CSKYToolChain.cpp) +
[`ToolChains/UEFI.cpp`](ToolChains/UEFI.cpp) +
[`ToolChains/TCE.cpp`](ToolChains/TCE.cpp) +
[`ToolChains/XCore.cpp`](ToolChains/XCore.cpp) +
[`ToolChains/AIX.cpp`](ToolChains/AIX.cpp) +
[`ToolChains/Fuchsia.cpp`](ToolChains/Fuchsia.cpp) +
[`ToolChains/HLSL.cpp`](ToolChains/HLSL.cpp) +
[`ToolChains/PS4CPU.cpp`](ToolChains/PS4CPU.cpp) +
[`ToolChains/VEToolchain.cpp`](ToolChains/VEToolchain.cpp) +
[`ToolChains/WebAssembly.cpp`](ToolChains/WebAssembly.cpp) +
[`ToolChains/ZOS.cpp`](ToolChains/ZOS.cpp)

### 阶段 11: 横切模块 + library 入口 (30 分钟)
[`SanitizerArgs.cpp`](SanitizerArgs.cpp) +
[`XRayArgs.cpp`](XRayArgs.cpp) +
[`ModulesDriver.cpp`](ModulesDriver.cpp) +
[`CreateInvocationFromArgs.cpp`](CreateInvocationFromArgs.cpp) +
[`CreateASTUnitFromArgs.cpp`](CreateASTUnitFromArgs.cpp)

---

## §6. 常用操作指南

### 6.1 添加新平台 (假设 `MyOS`)

1. **新建 `ToolChains/MyOS.cpp` + `ToolChains/MyOS.h`**: 派生自
   `Generic_ELF` (经 `ToolChains/Gnu.h`)。
2. **实现关键虚函数**: `AddClangSystemIncludeArgs`、
   `getDynamicLinker`、`computeSysRoot`、`AddCXXStdlibLibArgs`、
   `GetRuntimeLibType`、`GetCXXStdlibType` 等。
3. **注册到 [`Driver.cpp`](Driver.cpp)**: 在 `Driver::getToolChain`
   switch 加新 OS case, 调 `createToolChain<MyOS>`。
4. **更新 [`clang/include/clang/Driver/Options.td`](../../include/clang/Driver/Options.td)**:
   如需新 `-stdlib=` / `-static-libstdc++` 选项。
5. **测试**: `clang/test/Driver/myos.c` (basic compile + run),
   `clang/test/Driver/myos-header-search.c`。

### 6.2 给现有 arch 加新 -mcpu / -march

1. **找到对应 `ToolChains/Arch/<Arch>.cpp`** (e.g. `X86.cpp` /
   `AArch64.cpp` / `RISCV.cpp`)。
2. **在 `get<Arch>TargetCPU` 加新 case**: 调用
   `llvm::TargetParser::parseArch` 等。
3. **在 `get<Arch>TargetFeatures` 加新 feature** (如 `+newfeat`)。
4. **测试**: `clang/test/Driver/<arch>-features.c` 或
   `clang/test/Preprocessor/<arch>-newcpu.c` (验证 #define 变化)。

### 6.3 添加新 sanitizer

1. **加 enum**: [`Sanitizers.def`](../../include/clang/Basic/Sanitizers.def)
   加 `SANITIZER(my_san, my_group, "my-san", "my-san")`。
2. **解析 (Driver)**: [`SanitizerArgs.cpp`](SanitizerArgs.cpp) 在
   `parseSanitizerValue` 加新 case, 设默认 runtime。
3. **传递 (cc1)**: 经 `addArgs` 注入 `-fsanitize=my-san`。
4. **Backend**: 在 [`clang/lib/CodeGen/`](../CodeGen/) 加 IR 注入
   pass。
5. **测试**: `clang/test/Driver/fsanitize.c`,
   `clang/test/Sema/sanitize-my-san.c`。

### 6.4 添加新 GPU offload 后端

1. **建 `ToolChains/MyGPUToolChain.cpp`**: 派生自 `Generic_GCC` 或
   `ToolChain`。
2. **实现 `MyGPUToolChain::buildLinker`**: 调用 device linker (e.g.
   `clang-offload-bundler`)。
3. **注册**: 在 [`Driver.cpp`](Driver.cpp) 加
   `-f<my-gpu>=...` 选项 → 选 `MyGPUToolChain`。
4. **更新 [`OffloadBundler.cpp`](OffloadBundler.cpp)**: 如新 magic 格式
   , 更新 bundler。
5. **测试**: `clang/test/Driver/<my-gpu>*.c`。

### 6.5 调试 driver 调度

1. 用 `clang -v` (verbose) 看实际 argv (driver 输出)。
2. 用 `clang -###` (只打印, 不执行) 看 JobList。
3. 用 `clang -ccc-print-phases` 看 Action DAG 阶段。
4. 用 `clang -E -v` 看预处理阶段 + 实际 include 路径。
5. 用 `clang -print-target-features` / `-print-supported-cpus` /
   `-print-effective-triple` / `-print-prog-name=<tool>`。
6. 临时在 [`Driver.cpp`](Driver.cpp) 的 `BuildCompilation` 加
   `llvm::errs() << ...` trace argv。

### 6.6 调试 ToolChain 选择

1. 用 `clang -print-target-triple` 看实际 Triple。
2. 用 `clang -v -E` 看 include 路径 + selected sysroot。
3. 看 [`ToolChains/Gnu.cpp`](ToolChains/Gnu.cpp) 的
   `GCCInstallationDetector::ScanLibDirForGCCTriple` 是否找到 GCC。
4. 看 [`Multilib.cpp`](Multilib.cpp) 的 `select` 是否返回期望
   `Multilib`。
5. 看 [`Driver.cpp`](Driver.cpp) 的 `getToolChain` 哪个 branch 命中。

### 6.7 添加新 linker 选项 (e.g. `-static`)

1. 更新
   [`clang/include/clang/Driver/Options.td`](../../include/clang/Driver/Options.td):
   加 `def static : Flag<["-"], "static">;`
2. 在对应 ToolChain (e.g.
   [`ToolChains/Gnu.cpp`](ToolChains/Gnu.cpp) /
   [`ToolChains/Darwin.cpp`](ToolChains/Darwin.cpp)) 的
   `ConstructJob` 加选项解析。
3. 测试: `clang/test/Driver/<platform>-<flag>.c` + 用 `-###` 看实际
   argv。

### 6.8 调试 C++20 named-module 调度

1. 用 `clang -c -std=c++20 -fmodules-ts --module-name=foo` 看 driver
   行为。
2. 看 [`ModulesDriver.cpp`](ModulesDriver.cpp) 的
   `compileModule` 是否进入。
3. 用 `clang -v` 看实际 module 编译 argv。
4. 用 `--module-file-info` 看 manifest。

---

## §7. NT 注释索引

当前 `clang/lib/Driver/` 下尚无 `// <NT>` 注释。已建立目录索引, 姊妹
overview:

- `clang/lib/AST/` — Clang AST 层 (待写)
- `clang/lib/CodeGen/` — Clang AST → LLVM IR (待写)
- [`clang/lib/Basic/0-overview.md`](../Basic/0-overview.md) — Clang
  基础设置 + 共享数据
- `clang/lib/Frontend/` — Clang 前端桥接 (待写)
- `clang/lib/Lex/` — Clang Lexer (待写)
- `clang/lib/Parse/` — Clang Parser (待写)
- [`clang/lib/Sema/0-overview.md`](../Sema/0-overview.md) — Clang 语义
  分析

按"少而精"原则, 加 NT 注释建议优先级:

1. [`Driver.cpp`](Driver.cpp) — 8-12 段 (顶层调度, 整个 driver 入口)
2. [`ToolChains/Clang.cpp`](ToolChains/Clang.cpp) — 8-15 段 (~10k
   lines, `-cc1` invoker, 最大单文件)
3. [`ToolChains/Darwin.cpp`](ToolChains/Darwin.cpp) — 6-10 段 (Apple
   MachO, 多 arch 复杂)
4. [`ToolChains/Gnu.cpp`](ToolChains/Gnu.cpp) — 5-8 段 (GCC family base)
5. [`ToolChains/Linux.cpp`](ToolChains/Linux.cpp) — 4-6 段
6. [`Compilation.cpp`](Compilation.cpp) + [`Job.cpp`](Job.cpp) +
   [`Action.cpp`](Action.cpp) — 各 3-5 段 (JobList 调度)
7. [`ToolChains/Cuda.cpp`](ToolChains/Cuda.cpp) +
   [`ToolChains/AMDGPU.cpp`](ToolChains/AMDGPU.cpp) — 各 4-6 段 (GPU
   offload 核心)
8. [`ToolChains/CommonArgs.cpp`](ToolChains/CommonArgs.cpp) — 4-6 段
   (跨平台 shared args)
9. [`SanitizerArgs.cpp`](SanitizerArgs.cpp) + [`XRayArgs.cpp`](XRayArgs.cpp)
   — 各 3-4 段
10. [`Multilib.cpp`](Multilib.cpp) +
    [`MultilibBuilder.cpp`](MultilibBuilder.cpp) — 各 3-4 段
11. per-arch [`ToolChains/Arch/<X>.cpp`](ToolChains/Arch/X86.cpp) —
    各 2-3 段
12. [`OffloadBundler.cpp`](OffloadBundler.cpp) +
    [`ModulesDriver.cpp`](ModulesDriver.cpp) — 各 3-4 段

---

**姊妹文档**: 本目录对应 LLVM 流水线中的 **Clang 编译驱动**层, 是
Clang 用户最先接触的入口。它与
[`clang/lib/Basic/`](../Basic/0-overview.md) 配合生成 cc1 参数, 与
[`clang/lib/Sema/`](../Sema/0-overview.md) 配合处理语义, 与
[`clang/lib/CodeGen/`](../CodeGen/0-overview.md) 配合 codegen, 与
[`clang/lib/Frontend/`](../Frontend/) (待写) 配合 cc1 子进程。Driver
自身不直接调用 Basic 层 `.cpp`, 只通过 cc1 argv 把决定传给子进程。