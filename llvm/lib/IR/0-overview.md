<!-- <NT>overview:llvm/lib/IR/ -->

# LLVM IR 库导读 — `llvm/lib/IR/`

> 本文档梳理 `llvm/lib/IR/` 目录下全部文件（90 个，无子目录）的职责、上下游
> 与推荐阅读顺序。目标读者：想理解 LLVM IR 数据结构、添加新指令 / 新 intrinsic /
> 新 attribute / 新 metadata 节点的开发者。
>
> 所有路径相对 `llvm/lib/IR/`。`llvm/include/llvm/IR/` 头文件不在本导读
> 范围，但被本目录几乎所有 .cpp 实现包含。

---

## §0. lib/IR 在 LLVM 编译流水线中的位置

`lib/IR/` 是 LLVM 的 **LLVM IR 中间表示层**，是整个编译器的核心数据模型。
它定义了 `Module` / `Function` / `BasicBlock` / `Instruction` / `Value` / `Type`
/ `Constant` / `Metadata` / `Attribute` 等核心数据结构，几乎被 LLVM 每一层
（前端 / 优化 Pass / CodeGen / 后端）引用。

- **上游层**：LLVM 前端（Clang / 其他）通过 `IRBuilder` 构造 LLVM IR；或者
  通过 `LLVMCore` C API（被外部语言绑定使用）。
- **下游层**：被 `lib/Transforms/`（IR 层优化 Pass）和 `lib/CodeGen/`
  （指令选择）以及所有 `lib/Target/` 后端大量引用。IR 合法性校验
  （`llvm-as` / `opt -verify`）由本目录的 `Verifier.cpp` 提供。

`lib/IR/` 还包含 LLVM 的 **Pass 基础设施**：旧式 `LegacyPassManager`、
新式 `PassManager` / `AnalysisManager`、Pass 注册与计时。所有 IR 层和
CodeGen 层 Pass 都依赖这套基础设施。

---

## §1. 编译流水线概览

```
┌──────────────────────────────────────┐
│ 源代码 (C / C++ / Rust / ...)         │
└──────────────────────────────────────┘
                │
                ▼
┌────────────────────────────────────────────────────────────┐
│ Frontend (Clang / Clang AST / 其他)                         │
│   · Parser / Sema / AST 构造                                │
│   · CodeGen: AST -> LLVM IR                                │
│   · 调用 IRBuilder 构造 Module/Function/Instruction        │
└────────────────────────────────────────────────────────────┘
                │
                ▼
┌────────────────────────────────────────────────────────────┐
│ LLVM IR (Module / Function / BasicBlock / Instruction)     │
│   · lib/IR 数据结构 (Module, Value, Use, Type, Constant)   │
│   · 序列化: lib/Bitcode/Reader/Writer + lib/AsmParser/     │
└────────────────────────────────────────────────────────────┘
                │
                ▼
┌────────────────────────────────────────────────────────────┐
│ IR 层优化 (lib/Transforms/)                                 │
│   · InstCombine / SimplifyCFG / GVN / LICM / LoopVectorize │
│   · Pass 调度: lib/IR/LegacyPassManager + PassManager       │
│   · 分析: Dominators / LoopInfo / AliasAnalysis            │
└────────────────────────────────────────────────────────────┘
                │
                ▼
┌────────────────────────────────────────────────────────────┐
│ 目标无关 CodeGen (lib/CodeGen/)                              │
│   · CodeGenPrepare / ISel (SelectionDAG / GlobalISel)       │
│   · RegAlloc / BranchFold / PrologEpilogInserter            │
└────────────────────────────────────────────────────────────┘
                │
                ▼
┌────────────────────────────────────────────────────────────┐
│ 目标专属 CodeGen (lib/Target/<Arch>/)                       │
└────────────────────────────────────────────────────────────┘
```

辅助入口:

- **C API** ([Core.cpp](Core.cpp))：供其它语言绑定构造 LLVM IR，
  也被 `opt` / `llvm-as` 工具内部使用。
- **Verifier** ([Verifier.cpp](Verifier.cpp))：校验 Module / Function
  是否合法 IR。在 IR 层优化前后、CodeGen 前都会调用。

---

## §2. 文件目录结构

```
llvm/lib/IR/
├── CMakeLists.txt        ← LLVMCore 组件编译入口 (90 个文件全在此目录)
├── 顶层 90 个 .cpp/.h    ← IR 数据结构 + Pass 基础设施 + 诊断/校验
```

### 2.1 顶层文件按职责分类（11 大类，共 90 文件）

| # | 分类 | 文件数（约） | 核心类 |
|---|------|------------|--------|
| 1 | IR 核心数据结构 | ~12 | `Module`, `Function`, `BasicBlock`, `Value`, `Type`, `Use`, `User` |
| 2 | 指令 / 内建操作 | ~8 | `Instruction`, `IntrinsicInst`, `IRBuilder`, `InlineAsm` |
| 3 | 常量与常量折叠 | ~7 | `Constant`, `ConstantRange`, `ConstantFold` |
| 4 | 类型与属性 | ~4 | `Attribute`, `AttributeSet`, `DataLayout` |
| 5 | 模块/全局符号与材质化 | ~3 | `ModuleSummaryIndex`, `GVMaterializer`, `Mangler` |
| 6 | 调试信息 / Metadata | ~9 | `DIBuilder`, `MDNode`, `DILocation`, `DebugLoc`, `DbgRecord` |
| 7 | CFG / 支配 / 循环 | ~5 | `DominatorTree`, `CycleInfo`, `SSAContext`, `ConvergenceVerifier` |
| 8 | Pass 管理与插桩 | ~13 | `LegacyPassManager`, `PassManager`, `PassInstrumentation` |
| 9 | 诊断 / 提示 / 验证 | ~9 | `DiagnosticHandler`, `LLVMContext`, `Verifier`, `AutoUpgrade` |
| 10 | 实用工具与运行时辅助 | ~12 | `Statepoint`, `GCStrategy`, `RuntimeLibcalls`, `FPEnv` |
| 11 | 头文件 / 内部实现 | ~6 | `AttributeImpl.h`, `LLVMContextImpl.h`, `MetadataImpl.h` |

---

## §3. 文件详解

### 3.1 IR 核心数据结构（Module / Function / Value / Type / Use）

[`Module.cpp`](Module.cpp) — `Module` 类实现，全局容器、符号表、链接/分块管理
[`Function.cpp`](Function.cpp) — `Function` / `Argument` 类实现，参数 / 属性 / 调用约定
[`BasicBlock.cpp`](BasicBlock.cpp) — `BasicBlock` 类实现，指令链表 / 基本块操作
[`Globals.cpp`](Globals.cpp) — `GlobalValue` / `GlobalVariable` / `GlobalAlias` / `GlobalIFunc`
[`Value.cpp`](Value.cpp) — `Value` 基类实现，名称 / use-list / 类型 / RAUW
[`Use.cpp`](Use.cpp) — `Use` 类实现，def-use 链表节点
[`User.cpp`](User.cpp) — `User` 类实现，操作数分配 / 子类操作接口
[`Type.cpp`](Type.cpp) — `Type` / `IntegerType` / `PointerType` 等类型系统实现
[`TypedPointerType.cpp`](TypedPointerType.cpp) — 旧 `TypedPointerType` 支持（已弃用）
[`ValueSymbolTable.cpp`](ValueSymbolTable.cpp) — 全局 / 函数符号名查重
[`Comdat.cpp`](Comdat.cpp) — `Comdat`（comdat 选择组）实现
[`VectorTypeUtils.cpp`](VectorTypeUtils.cpp) — `VectorType` 工具函数

**关键类**：`Module`、`Function`、`BasicBlock`、`Value`、`Use`、`User`、`Type`、`IntegerType`、`PointerType`、`VectorType`、`GlobalValue`、`GlobalVariable`、`Argument`。

### 3.2 指令 / 内建操作

[`Instruction.cpp`](Instruction.cpp) — `Instruction` 基类实现，指令元数据 / 桶位 / 删除
[`Instructions.cpp`](Instructions.cpp) — 全部 LLVM 指令子类实现（`BinaryOperator` / `CastInst` / `CallInst` / `LoadInst` / `StoreInst` / ...）
[`IntrinsicInst.cpp`](IntrinsicInst.cpp) — `IntrinsicInst` 包装类与查询（`MemIntrinsic`、`CallBrInst`）
[`Intrinsics.cpp`](Intrinsics.cpp) — intrinsic ID 表与名字解析（`Intrinsic::ID`、`getDeclaration`）
[`InlineAsm.cpp`](InlineAsm.cpp) — `InlineAsm`（内联汇编）实现
[`Operator.cpp`](Operator.cpp) — `Operator` 二元属性查询（`hasNoSignedWrap` 等）
[`IRBuilder.cpp`](IRBuilder.cpp) — `IRBuilder` 指令构造 API（`Create*` 系列）
[`AbstractCallSite.cpp`](AbstractCallSite.cpp) — 抽象调用点（call/invoke/callbr 统一接口）

**关键类**：`Instruction`、`IRBuilder`、`InlineAsm`、`AbstractCallSite`、`Operator`、`IntrinsicInst`。

### 3.3 常量与常量折叠

[`Constants.cpp`](Constants.cpp) — 全部 `Constant` 子类（`ConstantInt` / `ConstantFP` / `ConstantArray` / `ConstantDataVector` / ...）
[`ConstantsContext.h`](ConstantsContext.h) — `ConstantsContext` 内部存储与去重
[`ConstantFold.cpp`](ConstantFold.cpp) — 常量折叠与常量表达式求值
[`ConstantRange.cpp`](ConstantRange.cpp) — 整数区间分析（ICmp 区间推理）
[`ConstantFPRange.cpp`](ConstantFPRange.cpp) — 浮点区间分析
[`ConstantRangeList.cpp`](ConstantRangeList.cpp) — 区间联合/交/差运算
[`ReplaceConstant.cpp`](ReplaceConstant.cpp) — 在模块中安全替换常量

**关键类**：`Constant`、`ConstantInt`、`ConstantFP`、`ConstantExpr`、`ConstantRange`、`ConstantFPRange`。

### 3.4 类型与属性

[`Attributes.cpp`](Attributes.cpp) — `Attribute` / `AttributeSet` / `AttributeList` 实现
[`AttributeImpl.h`](AttributeImpl.h) — `AttributeImpl` 内部数据结构
[`BundleAttributes.cpp`](BundleAttributes.cpp) — 属性束（operand bundles / attribute bundles）
[`DataLayout.cpp`](DataLayout.cpp) — `DataLayout` 数据布局（端序、对齐、ABI 类型大小）

**关键类**：`Attribute`、`AttributeSet`、`AttributeList`、`AttributeBundle`、`DataLayout`。

### 3.5 模块/全局符号与材质化

[`ModuleSummaryIndex.cpp`](ModuleSummaryIndex.cpp) — 模块摘要索引（thin-LTO / 跨模块分析）
[`GVMaterializer.cpp`](GVMaterializer.cpp) — `GlobalValue` 材质化接口（位码惰性加载）
[`Mangler.cpp`](Mangler.cpp) — C/ASM 符号名混淆（`Mangler::getNameWithPrefix`）

### 3.6 调试信息 / Metadata

[`DebugInfo.cpp`](DebugInfo.cpp) — `DebugInfo` 通用辅助（验证 / 枚举查询）
[`DebugInfoMetadata.cpp`](DebugInfoMetadata.cpp) — 所有 debug info metadata 子类（`DISubprogram` / `DILocalVariable` / `DIType` / ...）
[`DIBuilder.cpp`](DIBuilder.cpp) — `DIBuilder`（构造 debug info 节点的 API）
[`DIExpressionOptimizer.cpp`](DIExpressionOptimizer.cpp) — `DIExpression` 常量折叠
[`DebugLoc.cpp`](DebugLoc.cpp) — `DebugLoc` / `DILocation` 实现
[`DebugProgramInstruction.cpp`](DebugProgramInstruction.cpp) — `DbgRecord` / `DbgMarker`（新版调试记录）
[`Metadata.cpp`](Metadata.cpp) — `Metadata` / `MDNode` / `MDString` 通用 metadata
[`MetadataImpl.h`](MetadataImpl.h) — metadata 内部节点结构
[`MDBuilder.cpp`](MDBuilder.cpp) — `MDBuilder`（构造通用 metadata 节点）
[`DroppedVariableStats.cpp`](DroppedVariableStats.cpp) — 统计调试信息中丢弃的变量
[`DroppedVariableStatsIR.cpp`](DroppedVariableStatsIR.cpp) — `DroppedVariableStats` 在 IR 上的入口

**关键类**：`DIBuilder`、`MDNode`、`DILocation`、`DebugLoc`、`DbgRecord`、`DbgMarker`、`MDBuilder`。

### 3.7 CFG（Control Flow Graph） / 支配 / 循环分析

[`Dominators.cpp`](Dominators.cpp) — 支配树与支配前沿（`DominatorTree`、`DominanceFrontier`）
[`CycleInfo.cpp`](CycleInfo.cpp) — 通用 Cycle 树分析
[`SSAContext.cpp`](SSAContext.cpp) — SSA 形式辅助
[`ConvergenceVerifier.cpp`](ConvergenceVerifier.cpp) — convergence token 一致性校验
[`SafepointIRVerifier.cpp`](SafepointIRVerifier.cpp) — 安全点插入 IR 校验
[`PseudoProbe.cpp`](PseudoProbe.cpp) — 伪探针（sample PGO）指令与块探针

**关键类**：`DominatorTree`、`DominanceFrontier`、`CycleInfo`、`ConvergenceVerifier`。

### 3.8 Pass 管理与插桩

[`Pass.cpp`](Pass.cpp) — `Pass` 基类与 Module/Function/LoopPass 骨架
[`LegacyPassManager.cpp`](LegacyPassManager.cpp) — 旧式 pass 管理者（基于回调列表）
[`PassManager.cpp`](PassManager.cpp) — 新式 pass 管理基础设施（`PassManager` / `AnalysisManager`）
[`PassRegistry.cpp`](PassRegistry.cpp) — Pass 注册表（`initializePass` / `registerPass`）
[`PassInstrumentation.cpp`](PassInstrumentation.cpp) — Pass 插桩接口（`beforePass` / `afterPass` 回调）
[`PassTimingInfo.cpp`](PassTimingInfo.cpp) — Pass 计时与统计
[`PrintPasses.cpp`](PrintPasses.cpp) — `--print-passes` / `--print-pipeline-pass-names` 选项处理
[`OptBisect.cpp`](OptBisect.cpp) — `opt-bisect-limit`（按编号跳过 pass 调试）
[`IRPrintingPasses.cpp`](IRPrintingPasses.cpp) — `PrintModulePass` / `PrintFunctionPass`
[`ProfDataUtils.cpp`](ProfDataUtils.cpp) — ProfileData 工具函数
[`ProfileSummary.cpp`](ProfileSummary.cpp) — `ProfileSummary`（剖析数据摘要与热/冷阈值）
[`Assumptions.cpp`](Assumptions.cpp) — `@llvm.assume` 收集与查询辅助（`AssumptionCache`）
[`EHPersonalities.cpp`](EHPersonalities.cpp) — 异常处理 personality 函数名映射

**关键类**：`Pass`、`ModulePass`、`FunctionPass`、`LoopPass`、`LegacyPassManager`、`PassManager`、`AnalysisManager`、`PassInstrumentationCallbacks`、`AssumptionCache`、`ProfileSummary`。

### 3.9 诊断 / 提示 / 验证

[`DiagnosticHandler.cpp`](DiagnosticHandler.cpp) — `DiagnosticHandler` 抽象基类
[`DiagnosticInfo.cpp`](DiagnosticInfo.cpp) — `DiagnosticInfo` 子类与诊断输出
[`DiagnosticPrinter.cpp`](DiagnosticPrinter.cpp) — `DiagnosticPrinter` 格式化器
[`LLVMContext.cpp`](LLVMContext.cpp) — `LLVMContext`（线程局部 IR 全局状态）实现
[`LLVMContextImpl.cpp`](LLVMContextImpl.cpp) — `LLVMContextImpl`（Context 后端存储）
[`LLVMContextImpl.h`](LLVMContextImpl.h) — `LLVMContextImpl` 数据结构定义
[`Verifier.cpp`](Verifier.cpp) — Module/Function IR 合法性校验
[`VerifierInternal.h`](VerifierInternal.h) — `Verifier` 内部辅助
[`VerifierAMDGPU.cpp`](VerifierAMDGPU.cpp) — AMDGPU 后端特定的 IR 校验
[`LLVMRemarkStreamer.cpp`](LLVMRemarkStreamer.cpp) — Remark 流序列化（YAML 输出优化提示）
[`AutoUpgrade.cpp`](AutoUpgrade.cpp) — 旧 bitcode/IR 自动升级到新 intrinsic 与参数类型

**关键类**：`LLVMContext`、`LLVMContextImpl`、`DiagnosticHandler`、`Verifier`、`LLVMRemarkStreamer`。

### 3.10 实用工具与运行时辅助

[`Core.cpp`](Core.cpp) — LLVM C API（`LLVMModuleRef` / `LLVMValueRef` 等 C 包装）
[`BuiltinGCs.cpp`](BuiltinGCs.cpp) — 内置 GC 策略（ocaml / shadow-stack / statepoint）
[`GCStrategy.cpp`](GCStrategy.cpp) — `GCStrategy` 基类与 GC 描述
[`Statepoint.cpp`](Statepoint.cpp) — `statepoint` intrinsic 的栈映射/relocation 处理
[`VFABIDemangler.cpp`](VFABIDemangler.cpp) — Vector Function ABI 名称反混淆
[`NVVMIntrinsicUtils.cpp`](NVVMIntrinsicUtils.cpp) — NVVM intrinsic 辅助查找
[`Mangler.cpp`](Mangler.cpp) — 符号名 mangling（已在 §3.5 列出）
[`RuntimeLibcalls.cpp`](RuntimeLibcalls.cpp) — 运行时库调用名枚举与查询
[`FPEnv.cpp`](FPEnv.cpp) — 浮点环境（constrained intrinsics）约束标志
[`ReplaceConstant.cpp`](ReplaceConstant.cpp) — 在模块中安全替换常量（已在 §3.3 列出）
[`StructuralHash.cpp`](StructuralHash.cpp) — IR 结构哈希（structural equivalence）
[`TypeFinder.cpp`](TypeFinder.cpp) — 类型查找器（遍历 Module 收集所有 Used types）

**关键类**：`Statepoint`、`GCStrategy`、`RuntimeLibcalls`、`FPEnv`、`VFABI`、`Mangler`、`TypeFinder`。

### 3.11 头文件 / 内部实现

- [`AttributeImpl.h`](AttributeImpl.h) — `AttributeSetImpl` / `AttributeImpl` 内部数据结构
- [`ConstantsContext.h`](ConstantsContext.h) — `ConstantsContext` 与 `ConstantExpr` 节点定义
- [`LLVMContextImpl.h`](LLVMContextImpl.h) — `LLVMContextImpl` 全局存储后端
- [`MetadataImpl.h`](MetadataImpl.h) — `MDNodeImpl` / `MDStringImpl` 等 metadata 节点实现
- [`SymbolTableListTraitsImpl.h`](SymbolTableListTraitsImpl.h) — ilist 特化（`SymbolTableListTraits` 内部实现）
- [`VerifierInternal.h`](VerifierInternal.h) — `Verifier` 内部辅助

---

## §4. 关键调用链

### 4.1 Clang 前端构造 LLVM IR

```
clang -cc1 (Frontend)
  ├─ Parse: 词法/语法分析 -> AST
  ├─ Sema:   类型检查/语义分析
  └─ CodeGen (clang/lib/CodeGen/):
      └─ IRBuilderBase::Create* 系列
          ├─ Module / Function / BasicBlock 容器 (Module.cpp, Function.cpp, BasicBlock.cpp)
          ├─ Value / User / Use (Value.cpp, User.cpp, Use.cpp)
          ├─ Constant (Constants.cpp)
          ├─ Type (Type.cpp)
          ├─ Attribute (Attributes.cpp)
          ├─ Metadata / DebugInfo (Metadata.cpp, DebugInfoMetadata.cpp, DIBuilder.cpp)
          └─ Intrinsic (Intrinsics.cpp, IntrinsicInst.cpp)
```

### 4.2 IR 层优化 Pass 调度

```
opt / clang -O2
  └─ PassBuilder::buildModuleOptimizationPipeline
      ├─ 旧式: LegacyPassManager::run
      │     ├─ PassRegistry::getPassInfo
      │     ├─ PassInstrumentationCallbacks::beforePass
      │     ├─ Pass::run (用户 Pass)
      │     ├─ PassInstrumentationCallbacks::afterPass
      │     └─ Assumptions / ProfileSummary 跨 Pass 数据
      └─ 新式: PassManager<IRUnitT>::run
            ├─ AnalysisManager::getResult (Dominators, LoopInfo, ...)
            ├─ 调度 (按 Pipeline 顺序)
            └─ IRPrintingPasses::printBetween / printAfter
```

### 4.3 IR 合法性校验

```
opt -verify / llvm-as 中间件
  └─ Verifier::verify (Verifier.cpp)
      ├─ 遍历 Module 全局 (Globals.cpp)
      ├─ 遍历每个 Function (Function.cpp)
      │   ├─ 检查参数 / 返回值类型 / 属性
      │   ├─ 检查每条 Instruction 的操作数
      │   ├─ 检查 SSA / dominance (Dominators.cpp 辅助)
      │   ├─ 检查 intrinsic 参数 (Intrinsics.cpp)
      │   └─ 检查 metadata (Metadata.cpp, DebugInfoMetadata.cpp)
      └─ 输出 DiagnosticInfo + DiagnosticPrinter
```

### 4.4 调试信息流（构造 → 优化 → CodeGen → 汇编）

```
Clang Frontend (生成):
  DIBuilder::createCompileUnit / createFunction / createLocalVariable
    -> DISubprogram / DILocalVariable / DIType 等 (DebugInfoMetadata.cpp)
      附到 Instruction 的 Metadata (Instruction.cpp setMetadata)
        → IR 层 Pass (Assumptions.cpp / DroppedVariableStats.cpp 等跟踪)
          → CodeGen (AsmPrinter/DwarfDebug.cpp)
            → lib/CodeGen/AsmPrinter/DIE.cpp (生成 DWARF DIE)
              → 汇编输出 (.debug_info section)
```

### 4.5 Verifier / DominatorTree 协作

```
Function::verify (Verifier.cpp)
  ├─ 构造 DominatorTree (Dominators.cpp) -> 检查 PHI 节点 def 来自支配的前驱
  ├─ 构造 SSAContext (SSAContext.cpp) -> 验证 SSA 形式
  ├─ 检查每条 Instruction 的 metadata 引用合法 (Metadata.cpp)
  └─ 检查 convergent token 一致 (ConvergenceVerifier.cpp)
```

---

## §5. 推荐阅读顺序

按"读懂 LLVM IR 层"的目标，建议按以下顺序：

### 阶段 1：核心数据模型（2-3 小时）
1. [`Value.cpp`](Value.cpp) + [`Use.cpp`](Use.cpp) + [`User.cpp`](User.cpp) — IR 的 def-use 链表基础。
2. [`Type.cpp`](Type.cpp) — 类型系统。
3. [`Module.cpp`](Module.cpp) + [`Function.cpp`](Function.cpp) + [`BasicBlock.cpp`](BasicBlock.cpp) — 三层容器。
4. [`Globals.cpp`](Globals.cpp) — 全局符号。

### 阶段 2：指令与 IRBuilder（1-2 小时）
1. [`Instruction.cpp`](Instruction.cpp) + [`Instructions.cpp`](Instructions.cpp) — 指令基类与子类。
2. [`IRBuilder.cpp`](IRBuilder.cpp) — 看前端如何用 `Create*` 系列构造 IR。
3. [`Operator.cpp`](Operator.cpp) + [`Intrinsics.cpp`](Intrinsics.cpp) + [`IntrinsicInst.cpp`](IntrinsicInst.cpp) — 操作符属性与 intrinsic。
4. [`InlineAsm.cpp`](InlineAsm.cpp) + [`AbstractCallSite.cpp`](AbstractCallSite.cpp) — 内联汇编与调用点抽象。

### 阶段 3：常量与属性（1 小时）
1. [`Constants.cpp`](Constants.cpp) + [`ConstantFold.cpp`](ConstantFold.cpp)。
2. [`Attributes.cpp`](Attributes.cpp) + [`BundleAttributes.cpp`](BundleAttributes.cpp)。
3. [`DataLayout.cpp`](DataLayout.cpp)。

### 阶段 4：Pass 基础设施（2-3 小时）
1. [`Pass.cpp`](Pass.cpp) — Pass 基类骨架。
2. [`LegacyPassManager.cpp`](LegacyPassManager.cpp) — 旧式调度。
3. [`PassManager.cpp`](PassManager.cpp) + [`PassRegistry.cpp`](PassRegistry.cpp) — 新式调度 + 注册。
4. [`PassInstrumentation.cpp`](PassInstrumentation.cpp) + [`PassTimingInfo.cpp`](PassTimingInfo.cpp) + [`IRPrintingPasses.cpp`](IRPrintingPasses.cpp) — 插桩与计时。
5. [`Assumptions.cpp`](Assumptions.cpp) + [`ProfileSummary.cpp`](ProfileSummary.cpp) — 跨 Pass 共享数据。

### 阶段 5：调试信息与 Metadata（2 小时）
1. [`Metadata.cpp`](Metadata.cpp) + [`MDBuilder.cpp`](MDBuilder.cpp) — metadata 基础。
2. [`DebugInfoMetadata.cpp`](DebugInfoMetadata.cpp) + [`DIBuilder.cpp`](DIBuilder.cpp) — debug info 节点。
3. [`DebugLoc.cpp`](DebugLoc.cpp) + [`DebugProgramInstruction.cpp`](DebugProgramInstruction.cpp) — 源码位置与新版 dbg record。
4. [`DroppedVariableStats.cpp`](DroppedVariableStats.cpp) — 调试变量统计。

### 阶段 6：CFG / 分析（1-2 小时）
1. [`Dominators.cpp`](Dominators.cpp) — 支配树。
2. [`CycleInfo.cpp`](CycleInfo.cpp) + [`SSAContext.cpp`](SSAContext.cpp) — 通用图算法。
3. [`ConvergenceVerifier.cpp`](ConvergenceVerifier.cpp) + [`SafepointIRVerifier.cpp`](SafepointIRVerifier.cpp) — 校验工具。

### 阶段 7：校验 / 诊断 / C API（1 小时）
1. [`Verifier.cpp`](Verifier.cpp) — IR 合法性校验。
2. [`LLVMContext.cpp`](LLVMContext.cpp) + [`LLVMContextImpl.cpp`](LLVMContextImpl.cpp) — 全局状态。
3. [`DiagnosticHandler.cpp`](DiagnosticHandler.cpp) + [`DiagnosticInfo.cpp`](DiagnosticInfo.cpp) — 诊断。
4. [`AutoUpgrade.cpp`](AutoUpgrade.cpp) — 旧 IR 升级。
5. [`Core.cpp`](Core.cpp) — C API。

---

## §6. 常用操作指南

### 6.1 添加新的 LLVM IR 指令

1. **更新 td 文件**：
   - 在 `llvm/include/llvm/IR/Instructions.td` 加新指令的 `Instruction` 描述。
   - 如需分支 / 内存操作，更新 `llvm/include/llvm/IR/Instruction.def`。
2. **生成代码**：跑 `ninja` 触发 `tblgen` 自动生成 `Instructions.inc`。
3. **手写实现**（如有必要）：在 [`Instructions.cpp`](Instructions.cpp) 加新指令的
   `Create*` / `cloneImpl` / `getOpcodeName` 等方法。
4. **更新 Verifier**（如需校验新属性）：编辑 [`Verifier.cpp`](Verifier.cpp)
   的 `visit*` 钩子。
5. **单测**：在 `llvm/test/Assembler/` 与 `llvm/test/Verifier/` 加测试。

### 6.2 添加新的 Intrinsic

1. 在 `llvm/include/llvm/IR/Intrinsics.td` 加新 intrinsic 声明。
2. 跑 `tblgen` 生成 `Intrinsics.inc`。
3. 在 [`Intrinsics.cpp`](Intrinsics.cpp) 或 [`IntrinsicInst.cpp`](IntrinsicInst.cpp)
   加对应查询 / 包装（如需特殊属性）。
4. 单测：`llvm/test/Assembler/intrinsic-*`。

### 6.3 添加新的 Attribute

1. 在 `llvm/include/llvm/IR/Attributes.td` 加新 attribute 声明。
2. 跑 `tblgen` 生成 `Attributes.inc`。
3. 在 [`Attributes.cpp`](Attributes.cpp) 更新枚举处理（如需）。
4. 单测：`llvm/test/Assembler/attribute-*`。

### 6.4 添加新的 Debug Info 节点

1. 在 `llvm/include/llvm/IR/DebugInfoMetadata.td` / `DebugInfoMetadata.h` 加新节点。
2. 跑 `tblgen` 生成 `DebugInfoMetadata.inc`。
3. 在 [`DebugInfoMetadata.cpp`](DebugInfoMetadata.cpp) 实现新节点的 create / get 工厂。
4. 在 [`DIBuilder.cpp`](DIBuilder.cpp) 加 builder API。
5. 在 [`Verifier.cpp`](Verifier.cpp) 加新节点的合法性校验。
6. 在 [`DebugInfo.cpp`](DebugInfo.cpp) 加新节点的工具函数（如枚举）。

### 6.5 添加新的 IR 分析 Pass

1. 在 [`Pass.cpp`](Pass.cpp) 派生 `AnalysisInfoMixin` / `PassInfoMixin`。
2. 在 [`PassRegistry.cpp`](PassRegistry.cpp) 注册。
3. 实现 `run` 方法（IR 层）或 `runOnFunction`（MIR 层）。
4. 在 `lib/Transforms/` 或 `lib/Analysis/` 加 Pass 实现文件。
5. 单测：`llvm/test/Transforms/<pass-name>/`。

### 6.6 调试 IR 相关问题

- `opt -verify-each` 在每个 Pass 后校验 IR。
- `opt -print-after-all` 在每个 Pass 后打印 IR。
- `opt -debug-pass-manager` 打印新式 PassManager 调度日志。
- `opt -pass-remarks=<name>` 看特定 Pass 的 remark。
- 用 [`Verifier.cpp`](Verifier.cpp) 打断点查 IR 不合法的具体原因。

---

## §7. NT 注释索引

当前 `llvm/lib/IR/` 下尚无 `// <NT>` 注释。可结合以下 skill 使用：

- `nt-comments` — 给单文件加 NT 中文注释。
- 本 overview 文档的姊妹篇：
  - [`llvm/lib/Target/RISCV/0-overview.md`](../Target/RISCV/0-overview.md) — target 后端导读
  - [`llvm/lib/CodeGen/0-overview.md`](../CodeGen/0-overview.md) — target-independent CodeGen 导读

添加 NT 注释时按"少而精"原则：
- 核心类文件（`Module.cpp`、`Function.cpp`、`Instruction.cpp`、`Verifier.cpp`）可加 5-8 段。
- 中等大小文件（`Dominators.cpp`、`Attributes.cpp`、`DIBuilder.cpp`）加 3-5 段。
- 小工具文件（`ReplaceConstant.cpp`、`TypeFinder.cpp`）加 1-2 段即可。