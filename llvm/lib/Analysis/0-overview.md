<!-- <NT>overview:lib/Analysis/ -->

# LLVM Analysis 库导读 — `llvm/lib/Analysis/`

> 本文档梳理 `llvm/lib/Analysis/` 目录下全部源文件（125 个 `.cpp` + 1 个
> 子目录 `models/`）的职责、上下游与推荐阅读顺序。目标读者：想理解 LLVM IR
> 层分析（AliasAnalysis / ScalarEvolution / LoopInfo / MemorySSA / BFI 等）
> 的开发者，以及要为 transforms 写新分析的开发者。
>
> 所有路径相对 `llvm/lib/Analysis/`。头文件全部位于 `llvm/include/llvm/Analysis/`。

---

## §0. Analysis 库在 LLVM 中的位置

`lib/Analysis` 是 LLVM **IR 层分析库**——它提供 `lib/Transforms/` (以及部分
`lib/CodeGen/`/Target 后端) 用来决策"是否/如何改 IR"的**信息**。

包含的内容:

- **Alias Analysis 系列**: AAManager + Basic/Scoped/TBAA/SCEV/Globals/ObjC ARC AA。
- **支配/CFG/Region/Cycle 分析**: DominatorTree, PostDominatorTree, CycleInfo, RegionInfo。
- **调用图 + CGSCC PassManager**: CallGraph, LazyCallGraph, CGSCCPassManager。
- **循环栈**: LoopInfo, LoopAccessAnalysis, LoopVectorizationLegality, ScalarEvolution。
- **MemorySSA + MemoryDependenceAnalysis**。
- **Profile 分析**: BPI, BFI, PSI, LazyBlockFrequencyInfo, ProfileSummaryInfo。
- **IR 工具分析**: AssumptionCache, ValueTracking, InstructionSimplify, CaptureTracking, CostModel, InlineCost, DependenceAnalysis 等。
- **ML inliner advisor**: `MLInlineAdvisor` + TFLite / interactive / training-mode runners。

它在流水线中的位置:

```
源代码 -> Frontend (Clang)
            │
            ▼
       LLVM IR (lib/IR)
            │
            ├─→ lib/Analysis  (提供信息, 供下游决策)
            │     ├─ AliasAnalysis → 决定 IR 改写是否安全
            │     ├─ ScalarEvolution → 归纳变量 / trip count 推理
            │     ├─ LoopInfo / MemorySSA
            │     ├─ ProfileSummaryInfo / InlineCost → 决定 inlining
            │     └─ TargetTransformInfo → 各 target 的成本/合法性
            │
            ├─→ lib/Transforms  (消费 lib/Analysis, 改写 IR)
            │     ├─ InstCombine / SimplifyCFG
            │     ├─ LICM / LoopRotate / LoopUnroll / LoopVectorize
            │     ├─ Inliner / IPSCCP / GlobalDCE
            │     └─ GVN / DSE / MemCpyOpt
            │
            └─→ lib/CodeGen  (部分分析被 codegen 消费, 如 BFI / GlobalsAA / TTI)
```

所有本目录的 pass 通过 [`Analysis.cpp`](Analysis.cpp) 的 `initializeAnalysis()`
注册, 工具通过 `PassRegistry` 调用。

---

## §1. 编译流水线概览

```
┌──────────────────────────────────────────────────────────┐
│ LLVM IR (Module / Function / BasicBlock / Instruction)   │
└──────────────────────────────────────────────────────────┘
                │
                ▼
┌──────────────────────────────────────────────────────────┐
│ lib/Analysis  (本目录)                                     │
│   · AliasAnalysis: 指针别名                                │
│   · ScalarEvolution: 符号算术                              │
│   · LoopInfo / MemorySSA: 循环 + 内存 SSA                │
│   · Profile: BPI/BFI/PSI                                  │
│   · Target: TLI/TTI                                       │
│   · InlineCost/Advisor: 启发式 + ML                        │
│   · ValueTracking/InstructionSimplify: IR 属性查询        │
└──────────────────────────────────────────────────────────┘
                │
                ▼
┌──────────────────────────────────────────────────────────┐
│ lib/Transforms  (消费 Analysis, 改写 IR)                   │
│   · InstCombine / GVN / LICM / LoopVectorize / Inliner ... │
└──────────────────────────────────────────────────────────┘
                │
                ▼
┌──────────────────────────────────────────────────────────┐
│ lib/CodeGen  (SelectionDAG / GlobalISel / RegAlloc)       │
└──────────────────────────────────────────────────────────┘
```

辅助入口:

- **C API**: [`Analysis.cpp`](Analysis.cpp) 暴露 `LLVMVerifyModule` /
  `LLVMVerifyFunction` / `LLVMViewFunctionCFG` 等 C 接口。
- **ML 模型**: `models/` 提供 Python 工具, 用于生成 / 加载 / 与 TFLite 模型通信。

---

## §2. 文件目录结构

```
llvm/lib/Analysis/
├── CMakeLists.txt
├── 顶层 125 个 .cpp          ← 见 §3.1 - §3.15 分类
└── models/                  ← ML 模型的 Python 工具 (非 C++)
```

### 2.1 顶层文件按职责分类 (15 大类, 125 文件)

| # | 分类 | 文件数 | 核心标识符 |
|---|------|--------|-----------|
| 1 | Alias Analysis 系列 | ~10 | `AAResults`, `BasicAA`, `ScopedNoAliasAA`, `TBAA`, `SCEVAA`, `GlobalsAA` |
| 2 | Scalar Evolution | 3 | `ScalarEvolution`, `SCEVDivision`, `SCEVNormalization` |
| 3 | Dominators / CFG / Cycles / Regions | ~10 | `DominatorTree`, `PostDominatorTree`, `CycleInfo`, `RegionInfo` |
| 4 | Call Graph / CGSCC | 6 | `CallGraph`, `LazyCallGraph`, `CGSCCPassManager` |
| 5 | Loop Stack | ~10 | `LoopInfo`, `LoopAccessAnalysis`, `LoopNestAnalysis`, `IVUsers` |
| 6 | Memory Dependence / SSA | 8 | `MemorySSA`, `MemoryDependenceAnalysis`, `DependenceAnalysis`, `DDG` |
| 7 | Profile / Branch Probability | 8 | `BranchProbabilityInfo`, `BlockFrequencyInfo`, `ProfileSummaryInfo` |
| 8 | Scalar / Value Analyses | ~13 | `ValueTracking`, `AssumptionCache`, `LazyValueInfo`, `MustExecute` |
| 9 | Inline Advisor / Cost Models | ~15 | `InlineCost`, `MLInlineAdvisor`, `TFLiteUtils`, `TrainingLogger` |
| 10 | Memory Builtins / Libcalls | 3 | `MemoryBuiltins`, `RuntimeLibcallInfo`, `LibcallLoweringInfo` |
| 11 | Target-Library / Target-Transform Info | 2 | `TargetLibraryInfo`, `TargetTransformInfo` |
| 12 | Profile / Statistics / Printers | ~10 | `Lint`, `StructuralHash`, `OptimizationRemarkEmitter`, `HeatUtils` |
| 13 | 专项分析 (DXIL / CtxProf / IR2Vec / Hash) | 7 | `DXILMetadataAnalysis`, `CtxProfAnalysis`, `IR2Vec` |
| 14 | 通用 Pass 基础设施 | 1 | `Analysis.cpp` |
| 15 | Trace (单文件) | 1 | `Trace.cpp` |

---

## §3. 文件详解

### 3.1 Alias Analysis 系列

[`AliasAnalysis.cpp`](AliasAnalysis.cpp) — `AAResults` 链式调用 + 旧式
`AAResultsWrapperPass` 聚合器。
- 上游: 每个 IR pass 调用 `AA.alias()` / `AA.getModRefInfo()` (LoopVectorize,
  InstCombine, GVN, LICM, DSE, ...); `AAManager`。
- 下游: `BasicAA`, `ScopedNoAliasAA`, `TBAA`, `GlobalsAA`, `SCEVAA`, `ExternalAA`;
  `MemoryLocation`, `TargetLibraryInfo`, `CaptureTracking`。
- 关键类/函数: `AAResults`, `AAManager::run`, `AAResultsWrapperPass::runOnFunction`,
  `ExternalAAWrapperPass`, `getModRefInfo`, `isIdentifiedObject`, `isEscapeSource`。

[`BasicAliasAnalysis.cpp`](BasicAliasAnalysis.cpp) — 默认无状态 AA, 处理
明显 case (不同 globals, allocas, varargs, byval/noalias)。
- 上游: `AAResultsWrapperPass::runOnFunction` (注册顺序第一)。
- 下游: `MemoryLocation`, `ValueTracking::getUnderlyingObject`,
  `CaptureTracking`, `GlobalsModRef`。
- 关键类/函数: `BasicAAResult`, `BasicAAWrapperPass`, `BasicAA::alias`,
  `isOffsetCascadingPtr`.

[`AliasSetTracker.cpp`](AliasSetTracker.cpp) — 按指针追踪 alias set / mod-ref set,
饱和后回退到 "may-alias everything" 单一集合。
- 上游: 旧式 loop/BB-level pass (LICM old-PM, LoopRotate pre-NPM);
  `AliasSetsPrinterPass`。
- 下游: `BatchAAResults`, `MemoryLocation`, `GuardUtils`。
- 关键类/函数: `AliasSet`, `AliasSetTracker::getAliasSetFor`,
  `AliasSet::addMemoryLocation`, `mergeAllAliasSets` (饱和)。

[`AliasAnalysisEvaluator.cpp`](AliasAnalysisEvaluator.cpp) — 诊断 pass,
统计 AA 查询结果 (debug/启发式调优工具)。
- 上游: `opt -passes=print<aa-eval>`; 旧式 `AAEvaluator` pass.
- 下游: `AAManager`, `MemoryLocation`.
- 关键类/函数: `AAEvaluator::runInternal`, `AAEvaluator::~AAEvaluator`
  (打印 NoAlias/May/Must/Partial 统计).

[`ScopedNoAliasAA.cpp`](ScopedNoAliasAA.cpp) — 基于 `llvm.noalias` /
`llvm.noalias.scope.decl` metadata 的作用域 no-alias AA。
- 上游: `AAResultsWrapperPass` 链 (`!disable-scoped-noalias` 时注册)。
- 下游: `MDNode` (noalias scope IDs); `MemoryLocation`.
- 关键类/函数: `ScopedNoAliasAAResult`, `ScopedNoAliasAAWrapperPass`,
  `getNoAliasScope`.

[`TypeBasedAliasAnalysis.cpp`](TypeBasedAliasAnalysis.cpp) — 基于 `tbaa` metadata
的 AA (使用 IR 层类型 tag).
- 上游: `AAResultsWrapperPass` 链。
- 下游: `MDNode` TBAA tree; `MemoryLocation`。
- 关键类/函数: `TypeBasedAAResult`, `TypeBasedAAWrapperPass`, `TBAANode`.

[`ScalarEvolutionAliasAnalysis.cpp`](ScalarEvolutionAliasAnalysis.cpp) — 基于
SCEV 的 AA: 两个 SCEV 相等的指针返回 `MustAlias`。
- 上游: `AAResultsWrapperPass` 链 (最后注册)。
- 下游: `ScalarEvolution`, `SCEVAAWrapperPass`.
- 关键类/函数: `SCEVAAResult`, `SCEVAA::alias`.

[`GlobalsModRef.cpp`](GlobalsModRef.cpp) — 模块级 AA: 对地址不被取的
global 给出精确 NoModRef/Ref/Mod/ModRef。
- 上游: `AAResultsWrapperPass` 链; 读取函数属性 (`readnone`/`readonly`)。
- 下游: `Module`, `CallGraph` (函数 mod/ref 标志)。
- 关键类/函数: `GlobalsAAResult`, `GlobalsAAWrapperPass`, `FunctionsModRef`,
  `isNonEscapingGlobal`.

[`ObjCARCAliasAnalysis.cpp`](ObjCARCAliasAnalysis.cpp) — ARC-aware AA:
ObjC retain/release 对不能与用户指针别名。
- 上游: `AAResultsWrapperPass` 链 (Apple 前端)。
- 下游: `ObjCARCInstKind`, `ObjCARCAnalysisUtils`.
- 关键类/函数: `ObjCARCAA`, `ObjCARCAAWrapperPass`.

[`CaptureTracking.cpp`](CaptureTracking.cpp) — 判断指针是否逃逸, 用于
AA, ArgPromotion, ArgAttrs。
- 上游: `BasicAA`, `MemoryLocation::getForArgument`, `MergedLoadStoreMotion`.
- 下游: `DominatorTree` (可选, "captured-before" 查询);
  `ValueTracking::getUnderlyingObject`.
- 关键类/函数: `PointerMayBeCaptured`, `CaptureComponents`,
  `isPotentiallyCaptured`, `getCapturesBefore`.

[`MemoryLocation.cpp`](MemoryLocation.cpp) — 定义 AA 处处使用的 `MemoryLocation`
值类型。
- 上游: 所有 AA 查询 (`AA.alias(MemoryLocation, ...)`).
- 下游: `TargetLibraryInfo` (memcpy/memmove dest/src 提取); `DataLayout`.
- 关键类/函数: `MemoryLocation::get`, `MemoryLocation::getForArgument`,
  `MemoryLocation::getForDest`, `getBeforeOrAfter`.

### 3.2 Scalar Evolution

[`ScalarEvolution.cpp`](ScalarEvolution.cpp) — 核心: 归纳变量与 trip count 的
符号算术引擎。
- 上游: `LoopVectorize`, `LoopUnroll`, `IndVarSimplify`, `IVUsers`,
  `LoopAccessAnalysis`, `SCEVAA`, `Delinearization`.
- 下游: `LoopInfo`, `DominatorTree`, `TargetLibraryInfo`, `AssumptionCache`,
  `SCEVDivision`, `SCEVNormalization`.
- 关键类/函数: `ScalarEvolution`, `SCEV`, `SCEVAddRecExpr`, `SCEVConstant`,
  `getSCEV`, `getBackedgeTakenCount`, `getTripCountFromExitCount`, `forgetLoop`.

[`ScalarEvolutionDivision.cpp`](ScalarEvolutionDivision.cpp) — `SCEVDivision`:
SCEV 上的精确除法 (无余数)。
- 上游: `ScalarEvolution::getMulExpr`, `ScalarEvolution::getUDivExpr`.
- 下游: `ScalarEvolution` (内部).
- 关键类/函数: `SCEVDivision`, `visitCastExpr`, `visitAddRecExpr`.

[`ScalarEvolutionNormalization.cpp`](ScalarEvolutionNormalization.cpp) —
把 SCEV 归一化为 canonical "postinc" 形式以便匹配。
- 上游: `LoopVectorize` (IV 匹配); `IndVarSimplify`.
- 下游: `ScalarEvolution`, `SCEV`.
- 关键类/函数: `normalizeForPostIncUse`, `normalizeForBinaryUse`,
  `SCEVNormalize`.

### 3.3 Dominators / CFG / Cycles / Regions

[`DominanceFrontier.cpp`](DominanceFrontier.cpp) — 计算支配前沿, PHI 放置与
SSA 构造辅助所需。
- 上游: `PHIPlacement`, `RegAllocFast` debug; 旧式 pass.
- 下游: `DominatorTree`; `DominanceFrontierImpl` template.
- 关键类/函数: `DominanceFrontier`, `DominanceFrontier::addFrontier`,
  `DominanceFrontierWrapperPass`, `calculate`.

[`PostDominators.cpp`](PostDominators.cpp) — 后支配树 (旧式 + NPM)。
- 上游: `SimplifyCFG`, `JumpThreading`, `MemoryDependenceAnalysis`, `LICM`
  (post-dom 检查), `RegionInfo`.
- 下游: `DominatorTreeBase` template.
- 关键类/函数: `PostDominatorTree`, `PostDominatorTreeWrapperPass`,
  `PostDominatorTreeAnalysis`.

[`DomTreeUpdater.cpp`](DomTreeUpdater.cpp) — IR 改变时增量更新 (Post)DomTree,
统一 DT 和 PDT 更新。
- 上游: `SimplifyCFG`, `JumpThreading`, `LoopRotate`, `LoopUnswitch`,
  任何改 CFG 的 pass。
- 下游: `DominatorTreeBase`, `GenericDomTreeUpdater` (模板在 `include/`).
- 关键类/函数: `DomTreeUpdater`, `DomTreeUpdater::applyUpdates`,
  `recalculate`, `flush`.

[`DomPrinter.cpp`](DomPrinter.cpp) — `-dot-dom` / `-dot-postdom` / `-view-dom`
调试打印器。
- 上游: `opt -passes=dot-dom` 等.
- 下游: `DominatorTreeWrapperPass`, `DOTGraphTraitsPass`.
- 关键类/函数: `DominatorTreePrinter`, `PostDominatorTreePrinter`,
  `DomViewer`, `DomOnlyViewer`.

[`DomConditionCache.cpp`](DomConditionCache.cpp) — 按 dom-tree 节点缓存
`(Condition, TrueBB, FalseBB)`, 给 GuardUtils + LazyValueInfo 使用。
- 上游: `LazyValueInfo` 查询路径; 查询 "assumed-dominating condition" 的 pass.
- 下游: `DominatorTree`, `ValueTracking::isImpliedCondition`.
- 关键类/函数: `DomConditionCache::isImpliedByDomTree`.

[`CFG.cpp`](CFG.cpp) — CFG 上的辅助函数: 后继 / 前驱 / walk instructions /
`viewCFG` / `isCriticalEdge` 等。
- 上游: 每个改 CFG 的 transform pass.
- 下游: `CycleAnalysis` (部分查询); `BasicBlock`/`Instruction` iterators.
- 关键类/函数: `viewCFG`, `isCriticalEdge`, `MergeBlockIntoPredecessor`,
  `SplitCriticalEdge`, `GetSuccessorNumber`.

[`CFGPrinter.cpp`](CFGPrinter.cpp) — `-dot-cfg` / `-view-cfg` / `viewCFG()` 工具。
- 上游: `opt -passes=dot-cfg`; `Function::viewCFG()` 调用处.
- 下游: [`CFG.h`](../../include/llvm/Analysis/CFG.h) dot traits; `DOTGraphTraitsPass`.
- 关键类/函数: `CFGPrinterPass`, `CFGViewerPass`, `CFGOnlyPrinterPass`,
  `DOTGraphTraits<CFG>`.

[`CycleAnalysis.cpp`](CycleAnalysis.cpp) — 通用 cycles 分析 (UnifyFunctionExitNodes
语义但不修改 IR), 给 UniformityAnalysis 提供输入。
- 上游: `UniformityAnalysis`; 旧式 `CycleInfoWrapperPass`.
- 下游: `GenericCycleImpl.h` template.
- 关键类/函数: `CycleInfo`, `Cycle`, `CycleInfoAnalysis`, `CycleInfoWrapperPass`.
- 下游: [`GenericCycleImpl.h`](../../include/llvm/ADT/GenericCycleImpl.h) template.

[`RegionInfo.cpp`](RegionInfo.cpp) — CFG 上的 SESE (single-entry single-exit)
region 树。
- 上游: region 级 pass; 旧式 `RegionInfoPass`.
- 下游: `PostDominatorTree`, `DominatorTree`.
- 关键类/函数: `RegionInfo`, `Region`, `RegionInfoPass`, `getMaxRegion`.

[`RegionPass.cpp`](RegionPass.cpp) — 旧式 region 级 pass 基类 + `RGPassManager`.
- 上游: (旧式 region pass — 大多已废弃).
- 下游: `RegionInfo`.
- 关键类/函数: `RegionPass`, `RGPassManager::runOnFunction`.

[`RegionPrinter.cpp`](RegionPrinter.cpp) — `RegionInfo` 的 DOT 打印器 (region 树)。
- 上游: `opt -passes=dot-regions`.
- 下游: `RegionInfo`, `DOTGraphTraitsPass`.
- 关键类/函数: `RegionViewer`, `RegionPrinter`, `RegionOnlyViewer`.

### 3.4 Call Graph / CGSCC

[`CallGraph.cpp`](CallGraph.cpp) — 构建模块级调用图 (whole-module IPA 与
devirtualization 使用)。
- 上游: `IPSCCP`, `GlobalDCE`, `Inliner` (CGSCC), `ArgumentPromotion`,
  `DeadArgElimination`, LTO/ThinLTO.
- 下游: `Module`, `Function`, `CallBase` iteration.
- 关键类/函数: `CallGraph`, `CallGraphNode`, `CallGraphWrapperPass`,
  `CallGraph::addToCallGraph`, `removeFunctionFromModule`.

[`LazyCallGraph.cpp`](LazyCallGraph.cpp) — Ref-edges-only, SCC 可分解的
调用图, 按需 materializes callees.
- 上游: 新式 `CGSCCPassManager` (`InlinerPass`, `FunctionAttrs`, `OpenMPOpt`).
- 下游: `Module`, `Function` (按需迭代).
- 关键类/函数: `LazyCallGraph`, `LazyCallGraph::Node`, `LazyCallGraph::SCC`,
  `getOrInsertFunction`, `buildRefSCCs`.

[`CGSCCPassManager.cpp`](CGSCCPassManager.cpp) — CGSCC pass 管理器:
沿 RefSCC 自底向上运行 pass (更新时保留嵌套)。
- 上游: `PassBuilder` (`PassPipelineRegistration`); 外层
  `ModuleToPostOrderCGSCCPassAdaptor`.
- 下游: `LazyCallGraph`, `AnalysisManager` (CGSCC), `PassInstrumentation`.
- 关键类/函数: `CGSCCAnalysisManager`, `ModuleToPostOrderCGSCCPassAdaptor`,
  `run()`, `addPass()`.

[`CallGraphSCCPass.cpp`](CallGraphSCCPass.cpp) — 旧式 `CallGraphSCCPass` 基类 +
旧 CGSCC PM (`CGPassManager`).
- 上游: 旧式 opt/old PM Inliner (pre-NPM).
- 下游: `CallGraph`.
- 关键类/函数: `CallGraphSCCPass`, `CGPassManager::runOnModule`,
  `getAnalysisUsage`.

[`CFGSCCPrinter.cpp`](CFGSCCPrinter.cpp) — CFG SCC 的 DOT 打印器
(`-dot-cfg-scc`, `-view-cfg-scc`)。
- 上游: `opt -passes=dot-cfg-scc`.
- 下游: `SCCIterator<Function*>`, `DOTGraphTraitsPass`.
- 关键类/函数: `CFGSCCPrinter`, `CFGSCCViewer`, `DOTGraphTraits<CFGSCC>`.

[`CallPrinter.cpp`](CallPrinter.cpp) — `-dot-callgraph` / `-view-callgraph` 打印器。
- 上游: `opt -passes=print-callgraph`; 仅 debug.
- 下游: `CallGraph`, `DOTGraphTraitsPass`.
- 关键类/函数: `CallGraphPrinter`, `CallGraphDOTPrinterPass`,
  `CallGraphViewerPass`.

### 3.5 Loop Stack

[`LoopInfo.cpp`](LoopInfo.cpp) — CFG 上的 natural-loop 森林: 循环头 / 块 / 深度 / 出口。
- 上游: 每个循环 transform (LICM, LoopRotate, LoopUnroll, LoopVectorize,
  IndVarSimplify, Delinearization, IVUsers)。
- 下游: `DominatorTree`, `Function`.
- 关键类/函数: `Loop`, `LoopInfo`, `LoopInfoWrapperPass`,
  `LoopAnalysisManagerFunctionProxy`, `getLoopFor`, `getLoopsInPreorder`.

[`LoopPass.cpp`](LoopPass.cpp) — 旧式 `LoopPass` 基类 + `LPPassManager`.
- 上游: 旧式 loop pass (大多已迁移到 NPM).
- 下游: `LoopInfo`, `DominatorTree`, `ScalarEvolution` (旧 PM 依赖).
- 关键类/函数: `LoopPass`, `LPPassManager::runOnFunction`,
  `cloneBasicBlockSimplePredecessor`.

[`LoopAnalysisManager.cpp`](LoopAnalysisManager.cpp) — NPM 的 LoopAnalysisManager:
按 loop 缓存结果, 内层循环改变时 invalidate.
- 上游: 新式 PM loop pass (IndVarSimplify, LICM, LoopVectorize).
- 下游: `LoopInfo`, `AssumptionCache`, `MemorySSA`, `ScalarEvolution`.
- 关键类/函数: `LoopAnalysisManagerFunctionProxy`, `getResult`, `invalidate`.

[`LoopAccessAnalysis.cpp`](LoopAccessAnalysis.cpp) — 循环向量化合法性:
检查依赖 / 对齐 / dependence distance; 给向量化 cost model 喂数据。
- 上游: `LoopVectorize`, 也被 `LoopDistribution` 查询.
- 下游: `ScalarEvolution`, `LoopInfo`, `TargetTransformInfo`,
  `AliasAnalysis`, `MemoryDependenceAnalysis`.
- 关键类/函数: `LoopAccessInfo`, `VectorizationFactor`, `checkDependency`,
  `canVectorizeMemory`, `isStridedPtr`.

[`LoopCacheAnalysis.cpp`](LoopCacheAnalysis.cpp) — 估计 cache-line 复用 /
AOT 成本 (trip count × stride)。
- 上游: `LoopVectorize` (向量化 cost 决策); `UnrollAndJam`.
- 下游: `ScalarEvolution`, `TargetTransformInfo`.
- 关键类/函数: `LoopCacheCost`, `CacheCost::getCacheCost`,
  `computeLoopTripCount`.

[`LoopNestAnalysis.cpp`](LoopNestAnalysis.cpp) — 识别完美嵌套循环结构, 报告
内外层关系。
- 上游: `LoopVectorize`, `LoopUnroll`, `LoopInterchange`.
- 下游: `LoopInfo`, `ScalarEvolution`.
- 关键类/函数: `LoopNest`, `LoopNestAnalysis`, `getInterveningInstructions`,
  `arePerfectlyNested`.

[`LoopUnrollAnalyzer.cpp`](LoopUnrollAnalyzer.cpp) — unroll 前的分析:
预测部分 unroll 会启用哪些简化 (常量传播等)。
- 上游: `LoopUnrollPass`.
- 下游: `ScalarEvolution`, `LoopInfo`, `DominatorTree`.
- 关键类/函数: `UnrolledInstAnalyzer::visit`, `simplifyAfterUnroll`.

[`IVDescriptors.cpp`](IVDescriptors.cpp) — 描述归纳变量 (kind, step, wrap 标志,
衍生 IV)。
- 上游: `IndVarSimplify`, `LoopVectorize`, `LoopStrengthReduce`, `IVUsers`.
- 下游: `ScalarEvolution`, `LoopInfo`.
- 关键类/函数: `InductionDescriptor`, `RecurrenceDescriptor`,
  `isInductionPHI`, `getInductionDescriptor`.

[`IVUsers.cpp`](IVUsers.cpp) — 追踪归纳 / 复发表达式的 user, 即"在循环外
使用"的 interesting users。
- 上游: `IndVarSimplify`, `LoopStrengthReduce`, `LoopVectorize` (有时).
- 下游: `ScalarEvolution`, `LoopInfo`, `DominatorTree`.
- 关键类/函数: `IVUsers`, `IVStrideUse`, `IVUsersWrapperPass`, `AddRecIVs`.

### 3.6 Memory Dependence / SSA

[`MemoryDependenceAnalysis.cpp`](MemoryDependenceAnalysis.cpp) — 给定 mem-op,
其依赖的前序 mem-op (缓存 + 懒)。
- 上游: `GVN`, `PRE`, `LoopLoadElim`, `MemCpyOpt`.
- 下游: `AliasAnalysis`, `DominatorTree`, `TargetLibraryInfo`.
- 关键类/函数: `MemoryDependenceResults`, `MemoryDependenceWrapperPass`,
  `getDependency`, `getNonLocalPointerDependency`.

[`MemorySSA.cpp`](MemorySSA.cpp) — Memory SSA 形式: 内存上稀疏、factor 的
use/def 链。
- 上游: `GVN`, `EarlyCSE`, `LICM`, `DSE`, `LoopVectorize`, `LoopLoadElim`.
- 下游: `AliasAnalysis`, `DominatorTree`, `AssumptionCache`,
  `TargetLibraryInfo`.
- 关键类/函数: `MemorySSA`, `MemoryAccess`, `MemoryUse`, `MemoryDef`,
  `MemoryPhi`, `getMemoryAccess`, `walkAccesses`.

[`MemorySSAUpdater.cpp`](MemorySSAUpdater.cpp) — IR 改变时增量更新
MemorySSA (插入/删除/移动 mem-op, phi 放置)。
- 上游: `GVN`, `DSE`, `MemCpyOpt`, `SimplifyCFG`, `LoopRotate` (旧式).
- 下游: `MemorySSA`, `DominatorTree`.
- 关键类/函数: `MemorySSAUpdater::insertDef`, `removeMemoryAccess`,
  `moveTo`, `createMemoryAccessBefore`.

[`DependenceAnalysis.cpp`](DependenceAnalysis.cpp) — 循环携带的内存依赖测试
(Goff–Kennedy–Tseng).
- 上游: `LoopVectorize` (旧路径), `DependenceAnalysis` 感知优化.
- 下游: `ScalarEvolution`, `AliasAnalysis`, `LoopInfo`.
- 关键类/函数: `Dependence`, `DependenceInfo`, `FullDependence`,
  `depends`, `isInput`, `isOutput`, `isFlow`.

[`DependenceGraphBuilder.cpp`](DependenceGraphBuilder.cpp) — 通用 DDG/PDG
builder, 数据依赖图构造共享步骤。
- 上游: [`DDG.cpp`](DDG.cpp); 想用 PDG 的用户.
- 下游: `LoopInfo`, `ScalarEvolution`.
- 关键类/函数: `DependenceGraphBuilder`, `DDGNode`, `DDGEdge`,
  `createDDGNode`, `createDefUseEdges`.

[`DDG.cpp`](DDG.cpp) — Data Dependence Graph: 每循环的 DAG, 指令带 raw/flow/output 边。
- 上游: [`DDGPrinter.cpp`](DDGPrinter.cpp); 未来 loop-fusion 类 pass.
- 下游: [`DependenceGraphBuilder.cpp`](DependenceGraphBuilder.cpp),
  [`LoopInfo.cpp`](LoopInfo.cpp), [`DependenceAnalysis.cpp`](DependenceAnalysis.cpp).
- 关键类/函数: `DataDependenceGraph`, `DDGNode`, `DDGEdge`, `getDependences`.

[`DDGPrinter.cpp`](DDGPrinter.cpp) — data-dependence graph 的 `-dot-ddg` 打印器。
- 上游: `opt -passes=dot-ddg`.
- 下游: `DataDependenceGraph`, `DOTGraphTraitsPass`.
- 关键类/函数: `DDGPrinterPass`, `DDGViewer`, `DOTGraphTraits<DDG>`.

[`PHITransAddr.cpp`](PHITransAddr.cpp) — 通过 PHI 节点翻译地址 (GEP 链),
在每个 incoming 块 materializes 等价的 `Addr`。
- 上游: `GVN` (PRE load-address), `MemCpyOpt`, `LICM`.
- 下游: `DominatorTree`, `AssumptionCache`.
- 关键类/函数: `PHITransAddr::PHITranslate`, `PHITranslateSubExpr`,
  `IsPotentiallyPHITranslatable`.

### 3.7 Profile / Branch Probability / Block Frequency

[`BranchProbabilityInfo.cpp`](BranchProbabilityInfo.cpp) — 每条边的 branch
probability (启发式 + metadata-aware)。
- 上游: [`BlockFrequencyInfo.cpp`](BlockFrequencyInfo.cpp),
  `LazyBranchProbabilityInfo`, `ProfileSummaryInfo` 消费者。
- 下游: `LoopInfo`, `TargetLibraryInfo`, `BranchProbability` (ADT).
- 关键类/函数: `BranchProbabilityInfo`, `BranchProbabilityInfoWrapperPass`,
  `BranchProbabilityInfoAnalysis`, `getEdgeProbability`.

[`BlockFrequencyInfo.cpp`](BlockFrequencyInfo.cpp) — 由 BPI + 循环结构导出
每块的执行频率。
- 上游: `BlockPlacement`, `MachineBlockPlacement`, `IndirectCallPromotion`,
  `PGOInline`.
- 下游: [`BranchProbabilityInfo.cpp`](BranchProbabilityInfo.cpp), `LoopInfo`,
  `TargetLibraryInfo`.
- 关键类/函数: `BlockFrequencyInfo`, `BlockFrequencyInfoWrapperPass`,
  `BlockFrequencyInfoAnalysis`, `getBlockFreq`.

[`BlockFrequencyInfoImpl.cpp`](BlockFrequencyInfoImpl.cpp) — BFI 的内核:
用 `BlockMass` / `Distribution` 在循环内分配 mass。
- 上游: [`BlockFrequencyInfo.cpp`](BlockFrequencyInfo.cpp) (header-only 模板).
- 下游: `LoopInfo`.
- 关键类/函数: `BlockFrequencyInfoImpl`, `DistributeMass`, `applyLoopMass`.

[`LazyBlockFrequencyInfo.cpp`](LazyBlockFrequencyInfo.cpp) — BFI 的 lazy
变体: 首次查询前延迟频率计算。
- 上游: `CodeGen`/`MachineBlockPlacement` (常见).
- 下游: [`BranchProbabilityInfo.cpp`](BranchProbabilityInfo.cpp), `LoopInfo`.
- 关键类/函数: `LazyBlockFrequencyInfo`, `LazyBlockFrequencyInfoPass`,
  `calculateIfNotReady`.

[`LazyBranchProbabilityInfo.cpp`](LazyBranchProbabilityInfo.cpp) — BPI 的
lazy 变体。
- 上游: [`BlockFrequencyInfo.cpp`](BlockFrequencyInfo.cpp) (旧式耦合).
- 下游: `LoopInfo`.
- 关键类/函数: `LazyBranchProbabilityInfoPass`.

[`ProfileSummaryInfo.cpp`](ProfileSummaryInfo.cpp) — 模块级 PGO summary
(hot/cold 阈值, 函数 hotness, 块/边计数)。
- 上游: `Inliner` (ML+classic), `PGOProfileUse`, `SamplePGO`, `CodeGen`.
- 下游: `InstrProfReader`/`Summary` (ProfileData); `Function`/`Module` metadata.
- 关键类/函数: `ProfileSummaryInfo`, `ProfileSummaryInfoWrapperPass`,
  `isHotFunction`, `isColdBlock`, `getProfileCount`.

[`StaticDataProfileInfo.cpp`](StaticDataProfileInfo.cpp) — 用 PGO 数据节
放置信息 (`!llvm.profile`) 注解全局变量。
- 上游: PGO/SamplePGO codegen prep; `CodeGenPrepare`.
- 下游: [`ProfileSummaryInfo.cpp`](ProfileSummaryInfo.cpp), `InstrProf` (ProfileData).
- 关键类/函数: `StaticDataProfileInfo`, `StaticDataProfileInfoWrapperPass`.

[`SyntheticCountsUtils.cpp`](SyntheticCountsUtils.cpp) — 在调用图上传播
合成的 entry count (无真实 PGO 数据时 IPO 启发式使用)。
- 上游: `Inliner` (synthetic-counts 模式), `SampleProfileLoader`.
- 下游: `CallGraph`, [`ProfileSummaryInfo.cpp`](ProfileSummaryInfo.cpp).
- 关键类/函数: `SyntheticCounts`, `propagate`, `computeSyntheticCounts`.

[`MemoryProfileInfo.cpp`](MemoryProfileInfo.cpp) — 内存访问 profiling 数据结构
(MemProf), 给 ThinLTO memprof propagation 使用。
- 上游: `MemProfUse`, `MemProfContextDisambiguation`; ThinLTO.
- 下游: `IndexedInstrProfReader` (MemProf records).
- 关键类/函数: `MemoryProfileInfo`, `getMemProfInfo`, `MIBInfo`,
  `CallsiteLatency`.

### 3.8 Scalar / Value Analyses

[`LazyValueInfo.cpp`](LazyValueInfo.cpp) — 按需的 lazy value-range 传播
(constant/not-equal/...)。
- 上游: `JumpThreading`, `CorrelatedValuePropagation`, `GVN`.
- 下游: `ValueTracking`, `DominatorTree`, `AssumptionCache`, `ConstantFolding`.
- 关键类/函数: `LazyValueInfo`, `LazyValueInfoWrapperPass`,
  `LazyValueInfoAnalysis`, `getConstant`, `getConstantOnEdge`.

[`ValueLattice.cpp`](ValueLattice.cpp) — `ValueLattice` lattice value
抽象 (`unknown/constant/range/undef`), 用于 constant-range 类分析。
- 上游: [`LazyValueInfo.cpp`](LazyValueInfo.cpp) (状态); `ConstantFolding` 类查询.
- 下游: `ConstantRange`, `APInt`.
- 关键类/函数: `ValueLattice`, `ValueLattice::getConstantRange`,
  `getFromRange`, `mergeIn`; `intersect`/`getValueLattice`/`isOverdefined`.

[`ValueTracking.cpp`](ValueTracking.cpp) — IR 属性库: `ComputeKnownBits`,
`isKnownNonZero`, `isKnownToBeAPowerOfTwo`, `willNotOverflow`,
`isDereferenceablePointer`, `FindInserted/UsedValues`, `GetUnderlyingObject`。
- 上游: 每个 transform pass (ConstantFolding, InstCombine, SimplifyCFG, GVN,
  LICM, Inliner, SROA, ...)。
- 下游: `AssumptionCache`, `DominatorTree` (可选), `DataLayout`,
  `TargetLibraryInfo`.
- 关键类/函数: `ComputeKnownBits`, `isKnownNonZero`, `ComputeNumSignBits`,
  `isDereferenceablePointer`, `getUnderlyingObject`, `isKnownNonNull`,
  `willInstructionFail`.

[`AssumptionCache.cpp`](AssumptionCache.cpp) — 按函数缓存 `llvm.assume` 调用
以便快速查询。
- 上游: `ValueTracking`, `BasicAA`, `MemorySSA`, `LazyValueInfo`, `InlineCost`,
  `LoopAccessAnalysis`.
- 下游: `IntrinsicInst (assume)`.
- 关键类/函数: `AssumptionCache`, `AssumptionCacheTracker`, `AssumptionAnalysis`,
  `getAssumptionCache`, `affectsAttributes`.

[`EphemeralValuesCache.cpp`](EphemeralValuesCache.cpp) — 构建 ephemeral-value
集 (assume-like 值 + 其操作数 + `noalias` decls), 用于快速 assume-context 分类。
- 上游: `MemorySSA` (`isEphemeralValueIn`, `isGuaranteedLoopExecution`);
  `AssumptionCache` 用户。
- 下游: `AssumptionCache`, `CodeMetrics`, `MemorySSA`.
- 关键类/函数: `EphemeralValuesCache::isEphemeral`.

[`AssumeBundleQueries.cpp`](AssumeBundleQueries.cpp) — `@llvm.assume`
operand bundle 查询 helper (`ret_attr`/tags/equality/range)。
- 上游: `InlineCost`, `ValueTracking`, `AlignmentFromAssumptions`,
  `AssumeSimplifyPass`.
- 下游: `ValueTracking::isImpliedCondition`, `AssumptionCache`.
- 关键类/函数: `getAssumeBundle`, `findAllAssumeForValue`,
  `isAssumeWithEmptyBundle`, `getKnowledgeForValue`, `bundleHas`.

[`CmpInstAnalysis.cpp`](CmpInstAnalysis.cpp) — 分解 `icmp`/`fcmp` predicate
(`isTruePredicate`, `isFalsePredicate`, `swap`, `getInverse`,
`getNonStrictPredicate`).
- 上游: `GVN`, `CorrelatedValuePropagation`, `SimplifyCFG`, `InstCombine`.
- 下游: `CmpInst` (LLVM IR).
- 关键类/函数: `CmpInst::isTrueWhenEqual`, `DecomposePredicate`,
  `getCmpPredicateName`.

[`OverflowInstAnalysis.cpp`](OverflowInstAnalysis.cpp) — 识别 `with.overflow`
在静态情况下是 `false`/`true`.
- 上游: `InstCombine`, `SimplifyCFG`.
- 下游: `ValueTracking`, `IRBuilder`.
- 关键类/函数: `OverflowResult`, `computeOverflow`.

[`MustExecute.cpp`](MustExecute.cpp) — 判断某指令是否保证执行 (循环体, 在
`llvm.assume` 之后)。
- 上游: `LICM`, `MemorySSA`, `LoopVectorize` (predication), `ScalarEvolution`.
- 下游: `LoopInfo`, `DominatorTree`, `AssumptionCache`, `MemorySSA`.
- 关键类/函数: `isGuaranteedToExecute`, `wouldInstructionBeTriviallyDead`,
  `isMustExecuteInSuccessor`.

[`PhiValues.cpp`](PhiValues.cpp) — 收集 PHI 可能解析成的值 (跨嵌套 PHI 传递)。
- 上游: `JumpThreading`, `GVN`, `SCCP`; 旧式 pass.
- 下游: `DominatorTree`, `ValueTracking`.
- 关键类/函数: `PhiValues`, `PhiValuesWrapperPass`, `getValuesPhi`,
  `incomingValues`.

[`PtrUseVisitor.cpp`](PtrUseVisitor.cpp) — 指针的所有 user 访问 pattern,
`isAllocLikeFn`, `findAllocaForValue` 等的基础。
- 上游: `MemoryBuiltins`, `CaptureTracking` helpers, `BasicAA`.
- 下游: `Instruction` walk.
- 关键类/函数: `PtrUseVisitor`, `visitPtr`, `visitOffset`, `getHeadPtr`.

[`GuardUtils.cpp`](GuardUtils.cpp) — 识别/连接 `llvm.experimental.guard`
(deopt-style guards), true/false 分支与条件。
- 上游: `LoopVectorize` (predicate folding), `GVN`,
  `CorrelatedValuePropagation`.
- 下游: `ValueTracking::isImpliedCondition`, `BranchInst`, `SelectInst`.
- 关键类/函数: `isGuard`, `parseGuardCondition`, `widenGuardCondition`,
  `makeGuardCondition`.

[`FloatingPointPredicateUtils.cpp`](FloatingPointPredicateUtils.cpp) — 推理
NaN/inf, 有符号零, fast-math FP compare predicate。
- 上游: `GVN`, `CorrelatedValuePropagation`, `InstCombine`, `LoopVectorize`.
- 下游: `CmpInst`, `IRBuilder` (fast-math FMF), `ValueTracking`.
- 关键类/函数: `fcmpToPredicates`, `isFastPredicate`, `canIgnoreNaN`,
  `canIgnoreSignedZero`.

### 3.9 Inline Advisor / Cost Models

[`InlineAdvisor.cpp`](InlineAdvisor.cpp) — `InlineAdvisorAnalysis` +
`DefaultInlineAdvisor` (启发式 fallback)。
- 上游: `InlinerPass` (NPM), 旧式 `Inliner` (old PM).
- 下游: `InlineCost`, `CallGraph`, `ProfileSummaryInfo`, `AssumptionCache`.
- 关键类/函数: `InlineAdvisor`, `DefaultInlineAdvisor`, `getAdvice`,
  `InlineAdvisorAnalysis`, `MandatoryInlineAdvisor`.

[`InlineCost.cpp`](InlineCost.cpp) — 启发式 inliner cost 模型: InstCost,
threshold, callsite 分析, 向量化奖励。
- 上游: `InlineAdvisor`, `InlinerPass`.
- 下游: `TargetTransformInfo`, `AssumptionCache`, `ProfileSummaryInfo`,
  `LoopInfo`, `CodeMetrics`.
- 关键类/函数: `InlineCost`, `getInlineCost`, `InlineCostCallAnalyzer`,
  `isInlineViable`, `InlineParams`.

[`InlineOrder.cpp`](InlineOrder.cpp) — 决定考虑 callsite 的顺序 (优先级队列)。
- 上游: `InlinerPass` (ML/release 模式).
- 下游: `BlockFrequencyInfo`, `AssumptionCache`, `GlobalsModRef`,
  `ProfileSummaryInfo`.
- 关键类/函数: `InlineOrder`, `stdInlineOrder`, `mlInlineOrder`,
  `PriorityInlineOrder`.

[`MLInlineAdvisor.cpp`](MLInlineAdvisor.cpp) — 可插拔的 ML advisor, 训练 /
调用 TF 策略以决定 inline。
- 上游: `InlinerPass` (`enable-ml-inliner` 时).
- 下游: `MLModelRunner`, `TensorSpec`, `TrainingLogger`, `InlineFeatureMaps`,
  `InlineCost`.
- 关键类/函数: `MLInlineAdvisor`, `MLInlineAdvisorAnalysis`,
  `isInlineSuggestion`, `getAdvice`.

[`DevelopmentModeInlineAdvisor.cpp`](DevelopmentModeInlineAdvisor.cpp) —
编译期从文件加载 TFLite 模型并运行。
- 上游: [`MLInlineAdvisor.cpp`](MLInlineAdvisor.cpp) (development 模式构建).
- 下游: [`TFLiteUtils.cpp`](TFLiteUtils.cpp), `MLModelRunner`.
- 关键类/函数: `DevelopmentModeInlineAdvisor`, `loadModelfromFile`.

[`ModelUnderTrainingRunner.cpp`](ModelUnderTrainingRunner.cpp) — training 中的
模型运行器: 写 features + 接收 rewards, 策略训练时用。
- 上游: [`MLInlineAdvisor.cpp`](MLInlineAdvisor.cpp) (training 场景).
- 下游: [`InteractiveModelRunner.cpp`](InteractiveModelRunner.cpp)
  (写日志到文件), `TrainingLogger`.
- 关键类/函数: `ModelUnderTrainingRunner`, `evaluate`, `start`.

[`InteractiveModelRunner.cpp`](InteractiveModelRunner.cpp) — 基于 IPC 的模型
运行器, 通过 2 个文件描述符与外部策略服务器读写 features。
- 上游: [`MLInlineAdvisor.cpp`](MLInlineAdvisor.cpp) (interactive 模式).
- 下游: `MLModelRunner`, `TrainingLogger`.
- 关键类/函数: `InteractiveModelRunner`, `readFromFDs`,
  `configureRunServer`.

[`NoInferenceModelRunner.cpp`](NoInferenceModelRunner.cpp) — 无操作的模型
运行器: 抓 features 但不询问模型 (用于记默认策略数据)。
- 上游: [`MLInlineAdvisor.cpp`](MLInlineAdvisor.cpp) (默认策略日志).
- 下游: `TrainingLogger`.
- 关键类/函数: `NoInferenceModelRunner`.

[`ReplayInlineAdvisor.cpp`](ReplayInlineAdvisor.cpp) — 从 opt-remark 日志回放
历史 inline 决策, 用于固定 inlining 的 benchmark。
- 上游: `InlinerPass` (`-inline-replay` 时).
- 下游: `OptimizationRemark`, `InlineAdvisor`.
- 关键类/函数: `ReplayInlineAdvisor`, `ReplayInlineAdvisorAnalysis`,
  `getAdvice`.

[`TFLiteUtils.cpp`](TFLiteUtils.cpp) — TFLite 求值粘合 (加载 `.tflite`,
运行 `Invoke`, 读取输出)。
- 上游: [`DevelopmentModeInlineAdvisor.cpp`](DevelopmentModeInlineAdvisor.cpp);
  regalloc-ML advisor in `lib/CodeGen/`.
- 下游: `TF_Lite` C API.
- 关键类/函数: `TFModelEvaluator`, `loadModel`, `evaluate`.

[`TensorSpec.cpp`](TensorSpec.cpp) — Tensor-spec 抽象 + JSON I/O, 描述
TFLite 模型的 I/O。
- 上游: `MLModelRunner`, [`TFLiteUtils.cpp`](TFLiteUtils.cpp),
  `TrainingLogger`.
- 下游: JSON 解析 (LLVM Support), TF tensor shape utilities.
- 关键类/函数: `TensorSpec::create`, `isElementType`, `getSample`,
  `loadFromJSON`.

[`TrainingLogger.cpp`](TrainingLogger.cpp) — 训练期间用的 features / rewards 日志。
- 上游: [`MLInlineAdvisor.cpp`](MLInlineAdvisor.cpp) (development/training 模式),
  [`ModelUnderTrainingRunner.cpp`](ModelUnderTrainingRunner.cpp).
- 下游: `TensorSpec`, raw file output.
- 关键类/函数: `TrainingLogger`, `logReward`, `logFloatFeatureValue`,
  `logInt64FeatureValue`, `flush`.

[`ImportedFunctionsInliningStatistics.cpp`](ImportedFunctionsInliningStatistics.cpp)
— 统计 imported (ThinLTO) 函数的 inline 决策。
- 上游: `InlinerPass` (ThinLTO), `FunctionImportStats`.
- 下游: `OptimizationRemark`, raw counting.
- 关键类/函数: `ImportedFunctionsInliningStatistics`, `recordInline`.

[`CostModel.cpp`](CostModel.cpp) — 通过 `TargetTransformInfo` 的通用 IR cost
估计 (loop-vectorize 风格 cost 查询)。
- 上游: `LoopVectorize`, `CodeGenPrepare`, 启发式优化。
- 下游: `TargetTransformInfo`, `TargetLibraryInfo`.
- 关键类/函数: `CostModelAnalysis`, `CostModelPrinterPass`,
  `getInstructionCost`.

### 3.10 Memory Builtins / Libcalls

[`MemoryBuiltins.cpp`](MemoryBuiltins.cpp) — 识别 allocator 调用 (`malloc`,
`calloc`, `realloc`, `new`, `strdup` 等) 和 deallocator (`free`, `delete`)。
- 上游: `BasicAA`, `InlineCost`, `SROA`, `MemoryLocation`, `CaptureTracking`.
- 下游: `TargetLibraryInfo`, `ValueTracking`, `PtrUseVisitor`.
- 关键类/函数: `isMallocLikeFn`, `isAllocLikeFn`, `isFreeLikeFn`,
  `getAllocationSize`, `getAllocType`.

[`LibcallLoweringInfo.cpp`](LibcallLoweringInfo.cpp) — 把 `llvm.*` libcalls
(memcpy/memset/strlen 等) 映射到对应 lowering 形式的 helper。
- 上游: `SelectionDAG` (CodeGen), `RuntimeLibcallInfo`.
- 下游: [`RuntimeLibcallInfo.cpp`](RuntimeLibcallInfo.cpp), `TargetLibraryInfo`.
- 关键类/函数: `LibcallLoweringInfo`, `getLibcallName`, `getFunction`.

[`RuntimeLibcallInfo.cpp`](RuntimeLibcallInfo.cpp) — 每 runtime libcall
签名 + 在某 `Triple` 上的可用性。
- 上游: `CodeGen`/SelectionDAG; [`LibcallLoweringInfo.cpp`](LibcallLoweringInfo.cpp).
- 下游: `TargetLibraryInfo`, `Triple`.
- 关键类/函数: `RuntimeLibcallInfo`, `RuntimeLibcallInfoWrapperPass`,
  `getLibcallSignature`.

### 3.11 Target-Library / Target-Transform Info

[`TargetLibraryInfo.cpp`](TargetLibraryInfo.cpp) — 各 target 的知名 libcall
知识 (memcpy, sqrt, strlen 等): 可用性 / 签名 / 属性。
- 上游: `BasicAA`, `InstructionSimplify`, `ConstantFolding`, `LoopVectorize`,
  每个 codegen pass; `AAManager`.
- 下游: `Triple`, `TargetLibraryInfoWrapperPass` (旧式),
  `TargetLibraryAnalysis` (NPM).
- 关键类/函数: `TargetLibraryInfo`, `TargetLibraryInfoWrapperPass`,
  `TargetLibraryAnalysis`, `getLibFunc`, `has`.

[`TargetTransformInfo.cpp`](TargetTransformInfo.cpp) — 统一的 TTI CRTP 包装,
分发到各 target 的 hook 实现。
- 上游: 每个询问"cost 是多少?"的 IR transform (LoopVectorize, InlineCost,
  CodeGenPrepare, SROA, ...)。
- 下游: target 专属 `TargetTransformInfoImpl` (per `Target/*`);
  [`TargetLibraryInfo.cpp`](TargetLibraryInfo.cpp).
- 关键类/函数: `TargetTransformInfo`, `TargetTransformInfoWrapperPass`,
  `TargetIRAnalysis`, `getInstructionCost`, `isLegalAddImmediate`.

### 3.12 Profile / Statistics / Printers (诊断)

[`Lint.cpp`](Lint.cpp) — `-lint` IR 健全性检查 (类型不匹配, 缺 return 等)。
- 上游: `opt -passes=lint`.
- 下游: `AliasAnalysis`, `TargetLibraryInfo`, `DominatorTree`.
- 关键类/函数: `LintPass`, `lintFunction`, `visitInstruction`.

[`InstCount.cpp`](InstCount.cpp) — 统计 IR 指令总数 (简单诊断)。
- 上游: `opt -passes=print<inst-count>`.
- 下游: `Function`/`Module` iteration.
- 关键类/函数: `InstCountPass`, `visit`.

[`MemDerefPrinter.cpp`](MemDerefPrinter.cpp) — `-print-memderef` 打印
`isDereferenceablePointer` 结果。
- 上游: `opt -passes=print-memderef`.
- 下游: `ValueTracking`, `Loads`, `isDereferenceablePointer`.
- 关键类/函数: `MemDerefPrinter`, `visit`.

[`ModuleDebugInfoPrinter.cpp`](ModuleDebugInfoPrinter.cpp) — 以人类可读
形式 dump 模块级 debug info。
- 上游: `opt -passes=print<module-debuginfo>`.
- 下游: `DebugInfoFinder`, `Module`.
- 关键类/函数: `ModuleDebugInfoPrinterPass`, `printDebugInfo`.

[`StructuralHash.cpp`](StructuralHash.cpp) — 计算每个 function / module 的
结构 hash (用于 diff / 指纹)。
- 上游: `opt -passes=print<structural-hash>`; ThinLTO fingerprinting helpers.
- 下游: `Function`/`Module` walkers.
- 关键类/函数: `StructuralHash`, `StructuralHashPrinterPass`, `computeHash`.

[`HeatUtils.cpp`](HeatUtils.cpp) — 按 hot/cold heat 颜色给打印的 IR 上色。
- 上游: `opt -passes=print` (heat coloring);
  [`BlockFrequencyInfo.cpp`](BlockFrequencyInfo.cpp).
- 下游: `BFI`, `PSI`.
- 关键类/函数: `HeatUtils`, `colorBlockFreqName`, `getHeatColor`.

[`KernelInfo.cpp`](KernelInfo.cpp) — 发出 GPU kernel 函数 (block size 等) 的
opt-remarks。
- 上游: GPU codegen prep; `opt -passes=print<kernel-info>`.
- 下游: `TargetTransformInfo`, `Function`.
- 关键类/函数: `KernelInfoPrinter`, `printKernelInfo`.

[`OptimizationRemarkEmitter.cpp`](OptimizationRemarkEmitter.cpp) — 发出
`OptimizationRemark` / `Missed` / `Analysis` 诊断, 与 BFI hotness 关联。
- 上游: 几乎每个想要诊断的 IR transform (Inliner, LoopVectorize, GVN, SROA, ...)。
- 下游: [`BlockFrequencyInfo.cpp`](BlockFrequencyInfo.cpp),
  `LLVMContext::diagnose`.
- 关键类/函数: `OptimizationRemarkEmitter`, `OptimizationRemark`, `emit`,
  `allowRemark`.

[`IndirectCallPromotionAnalysis.cpp`](IndirectCallPromotionAnalysis.cpp) —
用 indirect-call value profile 选 profitable 的 indirect-call promotion 候选。
- 上游: `PGOIndirectCallPromotion`, `SamplePGO` codegen; `CodeGenPrepare`.
- 下游: `ProfileSummaryInfo`, `InstrProf` (ProfileData), `CallBase`.
- 关键类/函数: `IndirectCallPromotionAnalysis`, `isPromotionProfitable`,
  `getPromotedGUIDs`.

[`FunctionPropertiesAnalysis.cpp`](FunctionPropertiesAnalysis.cpp) — 计算
廉价函数级统计 (block/edge/instr count, 返回类型 info) 用于 tracing.
- 上游: ThinLTO cache, function-fingerprinting, sample-PGO 决策。
- 下游: `Function`, `LoopInfo`.
- 关键类/函数: `FunctionPropertiesInfo`, `FunctionPropertiesAnalysis`,
  `getFunctionPropertiesInfo`.

### 3.13 专项分析 (DXIL / CtxProf / IR2Vec / Hash)

[`DXILMetadataAnalysis.cpp`](DXILMetadataAnalysis.cpp) — 汇总 DXIL 模块
metadata (shader-stage, entry-points, resource binding info)。
- 上游: DirectX 后端 (`lib/Target/DirectX/`).
- 下游: [`DXILResource.cpp`](DXILResource.cpp), `Module` metadata (`!llvm.*`).
- 关键类/函数: `DXILMetadataAnalysisWrapperPass`, `DXILMetadata`, `DXILEntry`.

[`DXILResource.cpp`](DXILResource.cpp) — 建模 DXIL 资源 (buffers, textures,
samplers, UAVs) 及其从模块 metadata 来的 binding / type 信息。
- 上游: `DirectX` 后端; [`DXILMetadataAnalysis.cpp`](DXILMetadataAnalysis.cpp).
- 下游: `Frontend/HLSL/HLSLResource`; `Module` MD.
- 关键类/函数: `DXILResource`, `DXILResourceMap`, `DXILResourceWrapperPass`,
  `DXILResourceTypeWrapperPass`, `DXILResourceBindingWrapperPass`.

[`CtxProfAnalysis.cpp`](CtxProfAnalysis.cpp) — Contextual-profile 分析: 在
IPO 中保留 per-callsite context ID, 让 PGCMix 能重新应用。
- 上游: `PGOInstrumentation`, ThinLTO + `PGOCtxProf`.
- 下游: `Module`, `Function`, `PGOFuncName`.
- 关键类/函数: `CtxProfAnalysis`, `PGOContext`, `getContextID`, `setContextID`.

[`IR2Vec.cpp`](IR2Vec.cpp) — IR2Vec: IR 的 embedding 用于 ML 应用
(相似度, hint features 等)。
- 上游: (research / ML 应用).
- 下游: `Module` / `Function` walks, vocabulary (种子 embedding JSON 在 `models/`).
- 关键类/函数: `IR2Vec`, `IR2Vec::Embeddings`, `computeEmbeddings`, `getVec`.

[`HashRecognize.cpp`](HashRecognize.cpp) — 识别未优化的 GF(2^n) 多项式
hash, 建议查表替换。
- 上游: (research / 启发式).
- 下游: `LoopInfo`, `ScalarEvolution`, `MathExtras`.
- 关键类/函数: `HashRecognize`, `HashRecognize::recognize`, `isCRC32Pattern`.

[`LastRunTrackingAnalysis.cpp`](LastRunTrackingAnalysis.cpp) — 记录跑了哪些
analyses / passes (及 fingerprint), 在 IR 没变时跳过重跑。
- 上游: `PassManager` plumbing, opt `-enable-last-run-tracking`.
- 下游: `PreservedAnalyses`, `AnalysisManager`.
- 关键类/函数: `LastRunTrackingAnalysis`, `shouldRun`, `update`.

### 3.14 通用 Pass 基础设施

[`Analysis.cpp`](Analysis.cpp) — 注册所有 Analysis 库的旧式 pass, 暴露
`LLVMVerifyModule` / `LLVMVerifyFunction` / `LLVMViewFunctionCFG` C API。
- 上游: `PassRegistry`, `llvm::initializeAnalysis` (被每个链接此 lib 的工具调用)。
- 下游: 所有 `initializeXxxPass`; `llvm-c/Analysis.h`.
- 关键类/函数: `initializeAnalysis`, `LLVMVerifyModule`,
  `LLVMVerifyFunction`, `LLVMViewFunctionCFG`, `LLVMViewFunctionCFGOnly`.

### 3.15 Trace (单文件)

[`Trace.cpp`](Trace.cpp) — `Trace` 类: 表示一条 basic-block trace
(single-entry multi-exit hot region)。
- 上游: trace-based 优化 (旧式/research)。
- 下游: `BasicBlock`, `Function`.
- 关键类/函数: `Trace`, `Trace::getBlockList`, `isTrace`, `getTrace`.

### 3.16 models/ 子目录

[`models/`](models/) 不是 C++ 代码, 是 dev / test 时的离线 Python 工具:

- [`models/gen-inline-oz-test-model.py`](models/gen-inline-oz-test-model.py)
  — 合成一个最小 TFLite 模型 (inliner 期望的 I/O tensor spec), 用于 `-O2` /
  test 跑。
- [`models/gen-regalloc-eviction-test-model.py`](models/gen-regalloc-eviction-test-model.py)
  — 合成 regalloc eviction advisor 的 TFLite 模型。
- [`models/gen-regalloc-priority-test-model.py`](models/gen-regalloc-priority-test-model.py)
  — 合成 regalloc priority advisor 的 TFLite 模型。
- [`models/saved-model-to-tflite.py`](models/saved-model-to-tflite.py)
  — TF saved_model 转 `.tflite` flatbuffer (给 inliner)。
- [`models/interactive_host.py`](models/interactive_host.py)
  — 启动 Python 策略服务器, 通过 2 个 fd 与 [`InteractiveModelRunner.cpp`](InteractiveModelRunner.cpp) 通信。
- [`models/log_reader.py`](models/log_reader.py) — 解析
  [`TrainingLogger.cpp`](TrainingLogger.cpp) 产生的二进制 feature/reward 日志。
- [`models/seedEmbeddingVocab75D.json`](models/seedEmbeddingVocab75D.json) /
  [`models/x86SeedEmbeddingVocab100D.json`](models/x86SeedEmbeddingVocab100D.json)
  — 预计算 IR2Vec 词表 (75-D / x86-aware 100-D), [`IR2Vec.cpp`](IR2Vec.cpp) 消费。

---

## §4. 关键调用链

### 4.1 IR 层优化 pass 调用分析的典型流程 (GVN 为例)

```
opt -passes=gvn < input.ll
  └─ PassBuilder::buildModuleOptimizationPipeline
       └─ createGVNPass
            └─ GVN::run ([`GVN.cpp`](../Transforms/Scalar/GVN.cpp))
                 ├─ AM.getResult<DominatorTreeAnalysis>(F)
                 ├─ AM.getResult<AssumptionAnalysis>(F)
                 ├─ AM.getResult<MemorySSAAnalysis>(F)
                 │    └─ MemorySSA::run
                 │         ├─ AA.alias (AliasAnalysis.cpp)
                 │         ├─ DT.domTree ([`Dominators.h`](../../include/llvm/IR/Dominators.h))
                 │         └─ AC.assumptions (AssumptionCache.cpp)
                 ├─ AM.getResult<TargetLibraryAnalysis>(F)
                 └─ AM.getResult<AliasAnalysis>(F)
                 └─ AM.getResult<AliasAnalysis>(F)
                      └─ AAManager::run
                           ├─ BasicAA::alias
                           ├─ TBAA::alias
                           └─ SCEVAA::alias (依赖 ScalarEvolution)
                 → GVN 决定重写 IR (使用上述分析)
```

### 4.2 Inliner 调用链 (ML 模式)

```
opt -passes='default<O2>' -enable-ml-inliner
  └─ PassBuilder::buildModuleOptimizationPipeline
       └─ InlinerPass ([`Inliner.cpp`](../Transforms/IPO/Inliner.cpp))
            ├─ AM.getResult<ProfileSummaryAnalysis>(M) → PSI
            ├─ AM.getResult<InlineAdvisorAnalysis>(M) → InlineAdvisor
            │    └─ MLInlineAdvisor (or DefaultInlineAdvisor)
            │         ├─ loadModelfromFile (DevelopmentMode)
            │         ├─ InteractiveModelRunner / ModelUnderTrainingRunner
            │         └─ InlineFeatureMaps
            ├─ for each callsite:
            │    ├─ InlineOrder (priority)
            │    ├─ getAdvice (returns InlineCost)
            │    │    └─ InlineCostCallAnalyzer
            │    │         ├─ TTI.getInlineCallCosts (TargetTransformInfo.cpp)
            │    │         ├─ AA.alias
            │    │         └─ CodeMetrics
            │    └─ if profitable: doInlining
            └─ Update CallGraph / LazyCallGraph (CGSCCPassManager)
```

### 4.3 循环向量化调用链

```
LoopVectorizePass ([`LoopVectorize.cpp`](../Transforms/Vectorize/LoopVectorize.cpp))
  ├─ AM.getResult<LoopAnalysisManagerFunctionProxy>(F)
  │    └─ getLoop(...)
  ├─ AM.getResult<ScalarEvolutionAnalysis>(F)
  │    └─ ScalarEvolution::getSCEV, getTripCountFromExitCount
  ├─ AM.getResult<LoopAccessAnalysis>(F, L)
  │    ├─ LoopAccessInfo
  │    │    ├─ DependenceAnalysis (Goff–Kennedy–Tseng)
  │    │    ├─ AA.alias
  │    │    └─ TTI.getMemOpsCost
  │    └─ VectorizationFactor
  ├─ AM.getResult<LoopCacheAnalysis>(F, L)
  │    └─ CacheCost::getCacheCost
  └─ for each candidate VF:
       └─ CostModel (TTI cost)
            → 选择最优 VF 并向量化
```

### 4.4 Profile-guided inliner 调用链

```
InlinerPass (PGO 模式)
  ├─ ProfileSummaryInfo::isHotFunction (hot threshold)
  ├─ ProfileSummaryInfo::isColdFunction (cold threshold)
  ├─ ProfileSummaryInfo::getProfileCount (edge counts)
  └─ InlineCost:
       ├─ Hot callsite bonus
       ├─ Cold callsite penalty
       └─ CodeMetrics (指令数, 调用深度)
```

### 4.5 MemorySSA 更新 (GVN 改 IR 后)

```
GVN::run (修改 IR: 删除冗余 load, 替换为 phi 等)
  └─ MemorySSAUpdater (MemorySSAUpdater.cpp)
       ├─ removeMemoryAccess (删除旧 MemoryAccess)
       ├─ createMemoryAccessBefore / insertDef (插入新)
       └─ moveTo (移动 Phi)
            └─ update Phis (新增 / 删除 incoming edges)
```

---

## §5. 推荐阅读顺序

### 阶段 1: 数据模型 (1-2 小时)
1. [`MemoryLocation.cpp`](MemoryLocation.cpp) — AA 的"地址"抽象。
2. [`AliasAnalysis.cpp`](AliasAnalysis.cpp) — `AAResults` 接口与 chain。
3. [`BasicAliasAnalysis.cpp`](BasicAliasAnalysis.cpp) — 默认 AA 的 case 分类。

### 阶段 2: CFG / 支配 / 循环 (2-3 小时)
1. [`CFG.cpp`](CFG.cpp) — CFG 上的 helper。
2. [`PostDominators.cpp`](PostDominators.cpp) — Post-dom tree。
3. [`DomTreeUpdater.cpp`](DomTreeUpdater.cpp) — IR 改变时增量更新。
4. [`LoopInfo.cpp`](LoopInfo.cpp) — natural loop 森林。
5. [`CycleAnalysis.cpp`](CycleAnalysis.cpp) — 通用 cycle 树。

### 阶段 3: ScalarEvolution (2-3 小时, 最难部分)
1. [`ScalarEvolution.cpp`](ScalarEvolution.cpp) — 主入口。`getSCEV` / `getTripCountFromExitCount`。
2. [`ScalarEvolutionDivision.cpp`](ScalarEvolutionDivision.cpp) — SCEV 除法。
3. [`ScalarEvolutionNormalization.cpp`](ScalarEvolutionNormalization.cpp) — 归一化。

### 阶段 4: 内存 (2-3 小时)
1. [`MemoryDependenceAnalysis.cpp`](MemoryDependenceAnalysis.cpp) — 传统 mem-dep。
2. [`MemorySSA.cpp`](MemorySSA.cpp) — Memory SSA 形式。
3. [`MemorySSAUpdater.cpp`](MemorySSAUpdater.cpp) — 增量更新。
4. [`DependenceAnalysis.cpp`](DependenceAnalysis.cpp) — 循环携带依赖。

### 阶段 5: 调用图 / CGSCC (1 小时)
1. [`CallGraph.cpp`](CallGraph.cpp) — 旧式 CG。
2. [`LazyCallGraph.cpp`](LazyCallGraph.cpp) — 新式 lazy CG。
3. [`CGSCCPassManager.cpp`](CGSCCPassManager.cpp) — CGSCC PM。

### 阶段 6: 值分析 (1-2 小时)
1. [`ValueTracking.cpp`](ValueTracking.cpp) — IR 属性库。
2. [`LazyValueInfo.cpp`](LazyValueInfo.cpp) + [`ValueLattice.cpp`](ValueLattice.cpp) — 范围传播。
3. [`MustExecute.cpp`](MustExecute.cpp) — 必须执行判断。
4. [`AssumeBundleQueries.cpp`](AssumeBundleQueries.cpp) + [`AssumptionCache.cpp`](AssumptionCache.cpp) — `@llvm.assume`。

### 阶段 7: Profile (1 小时)
1. [`BranchProbabilityInfo.cpp`](BranchProbabilityInfo.cpp) + [`BlockFrequencyInfo.cpp`](BlockFrequencyInfo.cpp) — BPI / BFI。
2. [`ProfileSummaryInfo.cpp`](ProfileSummaryInfo.cpp) — PSI。

### 阶段 8: Inline / Advisor (1-2 小时, 按需)
1. [`InlineCost.cpp`](InlineCost.cpp) — 启发式 cost。
2. [`InlineAdvisor.cpp`](InlineAdvisor.cpp) — advisor 框架。
3. [`MLInlineAdvisor.cpp`](MLInlineAdvisor.cpp) — ML 部分。

### 阶段 9: Target 信息 (按需)
1. [`TargetLibraryInfo.cpp`](TargetLibraryInfo.cpp) — libcall 信息。
2. [`TargetTransformInfo.cpp`](TargetTransformInfo.cpp) — TTI 分发。

---

## §6. 常用操作指南

### 6.1 添加新 IR 分析

1. 在 `llvm/include/llvm/Analysis/` 创建新头文件 `NewAnalysis.h`。
2. 在 `llvm/lib/Analysis/` 创建 `NewAnalysis.cpp`, 实现:
   - 旧式: 派生 `ImmutablePass` / `FunctionPass` / `ModulePass`
   - 新式: 派生 `AnalysisInfoMixin<NewAnalysis>` (NPM)
3. 在 [`Analysis.cpp`](Analysis.cpp) 注册 (旧式 `initializeNewAnalysisPass`).
4. 在 `llvm/lib/Passes/PassRegistry.def` 注册 (NPM).
5. 在 [`Passes.h`](../../include/llvm/Analysis/Passes.h) 加 user-facing wrapper (如需).
6. 在 [`InitializePasses.h`](../../include/llvm/InitializePasses.h) 加 `initializeXxxPass` 声明.
7. 单测: `llvm/unittests/Analysis/NewAnalysisTest.cpp` 或 `llvm/test/Analysis/`.

### 6.2 添加新 Alias Analysis

1. 在 `llvm/include/llvm/Analysis/` 创建 `NewAA.h`, 定义 `NewAAResult` (继承
   `AAResultBase<NewAAResult>`).
2. 创建 `NewAAWrapperPass : public ImmutablePass` (旧式) 或
   `NewAAAnalysis : public AnalysisInfoMixin<NewAA>` (新式).
3. 在 `llvm/lib/Analysis/CMakeLists.txt` 加新源。
4. 在 [`AliasAnalysis.cpp`](AliasAnalysis.cpp) 的 `AAManager::run` 中按合适
   顺序 `addAAResult<NewAAResult>`.
5. 在 [`Passes.h`](../../include/llvm/Analysis/Passes.h) 加 `createNewAAPass`.
6. 单测: [`AliasAnalysisTest.cpp`](../../unittests/Analysis/AliasAnalysisTest.cpp).

### 6.3 给 InlineAdvisor 加新策略

1. 在 `llvm/include/llvm/Analysis/` 加 `NewAdvisor.h`.
2. 创建 `NewAdvisor.cpp`, 继承 `InlineAdvisor`:
   - 实现 `getAdvice(CallBase&)` 返回 `InlineCost`
   - 实现 `onPassEntry` / `onPassExit` (可选)
3. 在 `llvm/lib/Analysis/InlineAdvisor.cpp` 注册 (`createNewAdvisor`).
4. 在 [`InlineAdvisor.cpp`](InlineAdvisor.cpp) 的 `getInlineAdvisor` 分发中加选项.
5. 单测: `llvm/test/Transforms/Inline/new-advisor.ll`.

### 6.4 添加新打印 pass / 诊断 pass

1. 创建 `NewPrinter.cpp`, 派生 `PassInfoMixin<NewPrinterPass>`.
2. 实现 `printPipeline/rawString` / `run` / `preservedAnalyses`.
3. 在 `llvm/lib/Passes/PassRegistry.def` 注册:
   ```
   FUNCTION_PASS("print-new-info", NewPrinterPass)
   ```
4. 单测: `opt -passes=print-new-info input.ll`.

### 6.5 使用 IR 分析的常见模式

- **新式 PM (NPM)**: `AM.getResult<NewAnalysis>(F)`. NPM 帮你缓存, IR 改变时自动 invalidate.
- **旧式 PM**: `getAnalysis<NewAnalysis>().getSomething()`.
- **跨 pass 共享数据**: 使用 `AnalysisManager::getResult` 缓存, pass 配合 `PreservedAnalyses` 声明保活。

---

## §7. NT 注释索引

当前 `llvm/lib/Analysis/` 下尚无 `// <NT>` 注释。可结合以下 skill 使用:

- `nt-comments` — 给单文件加 NT 中文注释。
- 姊妹 overview:
  - [`llvm/lib/Target/RISCV/0-overview.md`](../Target/RISCV/0-overview.md) — RISCV 后端
  - [`llvm/lib/CodeGen/0-overview.md`](../CodeGen/0-overview.md) — target-independent CodeGen
  - [`llvm/lib/IR/0-overview.md`](../IR/0-overview.md) — LLVM IR 层
  - [`llvm/lib/TargetParser/0-overview.md`](../TargetParser/0-overview.md) — TargetParser

按"少而精"原则, 添加 NT 注释时建议:
- [`ScalarEvolution.cpp`](ScalarEvolution.cpp) — 5-8 段 (核心数据结构 + getSCEV)
- [`AliasAnalysis.cpp`](AliasAnalysis.cpp) + [`BasicAliasAnalysis.cpp`](BasicAliasAnalysis.cpp) — 各 3-5 段
- [`MemorySSA.cpp`](MemorySSA.cpp) + [`MemorySSAUpdater.cpp`](MemorySSAUpdater.cpp) — 各 3-5 段
- [`InlineCost.cpp`](InlineCost.cpp) + [`MLInlineAdvisor.cpp`](MLInlineAdvisor.cpp) — 各 3-5 段
- 单 utility 文件 (`Trace.cpp`, `CmpInstAnalysis.cpp` 等) — 1-2 段
- `models/` Python 文件 — 不加注释 (Python 而非 C++)