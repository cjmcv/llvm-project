<!-- <NT>overview:lib/CodeGen/ -->

# LLVM CodeGen 导读 — `llvm/lib/CodeGen/`

> 本文档梳理 `llvm/lib/CodeGen/` 目录下全部文件（~373 个，含 5 个子目录）的
> 职责、上下游与推荐阅读顺序。目标读者：想理解 LLVM 后端共用框架（与
> `lib/Target/X/` 配套）、给 LLVM 添加新 Pass / 新目标 hook 的开发者。
>
> 所有路径相对 `llvm/lib/CodeGen/`。`llvm/include/llvm/CodeGen/` 头文件
> 不在本导读范围，但被大量引用。

---

## §0. CodeGen 在 LLVM 编译流水线中的位置

`CodeGen/` 是 LLVM **目标无关的机器码生成框架**，介入 LLVM IR → 机器指令的
整段流水线。它由 `llvm/lib/CodeGen/` 下的 target-independent Pass / 数据结构 /
分析组成；具体后端（RISCV / X86 / AArch64…）位于 `llvm/lib/Target/<Arch>/`，
通过 `TargetMachine` / `TargetLowering` / `TargetInstrInfo` 等接口与本目录
提供的框架对接。

- **上游层**：LLVM IR 优化（`lib/Transforms/` + `lib/Analysis/`）— 经
  `TargetPassConfig` 编排后进入 CodeGen。
- **下游层**：目标专属的指令编码 / 汇编输出（`lib/Target/<Arch>/MCTargetDesc/`
  + `lib/MC/`）— 经 AsmPrinter 把 MIR 落地为汇编/MC 指令。

入口是 `CodeGenPassBuilder`（在 `llvm/lib/CodeGen/[TargetPassConfig.cpp](TargetPassConfig.cpp)` 中），
由 `LLVMTargetMachine::addPassesToEmitFile` 调用。它按"指令选择 →
寄存器分配 → 后端优化 → AsmPrinter"四阶段调度 `lib/CodeGen/` 下的所有 Pass。

---

## §1. 编译流水线概览

```
┌──────────────────────────────────────┐
│ LLVM IR (Module / Function)          │
└──────────────────────────────────────┘
                │
                ▼
┌────────────────────────────────────────────────────────────┐
│ 预处理 (IR 层, 跑在指令选择前,是架构无关的IR优化Passes)       │
│   · CodeGenPrepare             改写 IR 让 ISel 更顺畅       │
│   · AtomicExpandPass           拆分 atomic ops             │
│   · ExpandReductions / ExpandVectorPredication             │
│   · LowerEmuTLS / PreISelIntrinsicLowering                 │
└────────────────────────────────────────────────────────────┘
                │
                ▼
┌────────────────────────────────────────────────────────────┐
│ 指令选择 (二选一)     IR → MIR机器指令/伪指令                │
│   ┌─ SelectionDAG: SelectionDAGBuilder -> SelectionDAG ->  │
│  │    DAGCombiner -> Legalize -> SelectionDAGISel ->       │
│  │    InstrEmitter -> FastISel fallback                    │
│   └─ GlobalISel:   IRTranslator -> Legalizer ->            │
│        RegBankSelect -> InstructionSelect                  │
└────────────────────────────────────────────────────────────┘
                │
                ▼  MachineFunction (MIR)
┌────────────────────────────────────────────────────────────┐
│ Pre-RA Passes (SelectionDAG/ISel 后、寄存器分配-RA 前，属于MIR层面的硬件感知优化)   │
│   · PHIElimination               SSA PHI -> 寄存器拷贝      │ 
│   · TwoAddressInstructionPass    3-addr -> 2-addr           │
│   · RegisterCoalescer            同值合并                    │
│   · MachineLICM / MachineCSE     循环不变 / 公共子表达式      │
│   · MachineScheduler             指令调度                    │
│   · BranchFolding                分支折叠                    │
│   · MachineBlockPlacement        按 profile 重排基本块       │
│   · TailDuplication / IfConversion                          │
│   · LiveVariables / LiveIntervals 计算活跃区间               │
│   · StackSlotColoring / LocalStackSlotAllocation            │
│   · MachineCopyPropagation / DetectDeadLanes / DeadMIElim  │
└────────────────────────────────────────────────────────────┘
                │
                ▼
┌────────────────────────────────────────────────────────────┐
│ 寄存器分配 (RA: Register Allocation)                            │
│   · RegAllocBase (基类) -> RegAllocFast / Greedy / Basic /  │
│     PBQP                                                   │
│   · VirtRegMap / SplitKit / InlineSpiller                  │
│   · CalcSpillWeights / RegisterPressure / LiveRegMatrix    │
└────────────────────────────────────────────────────────────┘
                │
                ▼
┌────────────────────────────────────────────────────────────┐
│ Post-RA Passes                                              │
│   · PostRASchedulerList / MachineScheduler                 │
│   · BranchFolding (final)                                   │
│   · ExpandPostRAPseudos / GCRootLowering                   │
│   · PrologEpilogInserter / ShrinkWrap / StackProtector     │
│   · LiveDebugValues (VarLoc 或 InstrRef 实现)               │
│   · MachineOutliner                                         │
│   · CFIInstrInserter / CFIFixup / PatchableFunction         │
│   · FEntryInserter / XRayInstrumentation                    │
└────────────────────────────────────────────────────────────┘
                │
                ▼  MachineFunction + 物理寄存器
┌────────────────────────────────────────────────────────────┐
│ AsmPrinter (lib/CodeGen/AsmPrinter/)                       │
│   · DWARF / CodeView 调试信息                              │ 
│   · EH (Itanium CFI / ARM EHABI / Win SEH / Wasm EH /     │ 
│     AIX XCOFF)                                              │
│   · StackMap / FaultMap / PseudoProbe                       │
│   · GC root descriptors / Win CFG / Win EH Continuation    │
└────────────────────────────────────────────────────────────┘
                │
                ▼  汇编/MC 层
┌────────────────────────────────────────────────────────────┐
│ lib/Target/<Arch>/MCTargetDesc/ (目标专属)                  │
│   · MCCodeEmitter / AsmBackend / ELFObjectWriter           │
│   · AsmParser / Disassembler / TargetStreamer              │
└────────────────────────────────────────────────────────────┘
                │
                ▼  ELF/Mach-O 字节流
┌────────────────────────────────────────────────────────────┐
│ ld.lld / GNU ld                                            │
└────────────────────────────────────────────────────────────┘

名词解释：
* PHI指令 是 SSA 形式中的 Φ 函数，放在控制流汇合块；根据来自哪个前驱基本块选择对应数值。PHI 不是硬件可执行指令
* SSA：Static Single Assignment，静态单赋值。在 SSA 形式里，每一个变量只能被赋值一次。
* EH：Exception Handling
* Itanium CFI: Call Frame Information, Linux ELF 平台默认（x86_64/RISC-V），也就是 Itanium ABI 异常处理。
              - `.cfi_*` 汇编伪指令，生成 `.eh_frame` section（属于 DWARF 的一部分）
              - 记录栈帧布局、寄存器保存位置，用于**栈展开 unwind**（异常回溯、也用于 crash backtrace）
* DWARF（Debugging With Arbitrary Record Formats）是 ELF 二进制里存储源码调试信息的标准二进制格式，GDB/LLDB 靠它做源码级调试
* PseudoProbe: 编译期在 IR/MIR 插入 伪探针标记（不是真实机器指令，无运行时开销！），AsmPrinter 把探针元数据写入`.llvm.pseudoprobe`段。
               采样 profiler 拿到硬件 perf 采样后，通过 PseudoProbe 元数据，精准还原：基本块执行次数;内联栈信息; 直接 / 间接调用的执行计数
               > 核心亮点：几乎 0 运行时开销，不用插计数器，生产环境可以采样，用于 PGO 优化 (调分支权重、内联、循环优化)
* PGO: Profile-Guided Optimization，配置文件引导优化 / 剖面引导优化
       一句话：先跑一遍真实业务负载，采集程序运行时的热点、分支走向数据（profile 剖面）；再用这份数据重新编译，让编译器针对性做优化。
```

汇编路径（`llvm-mc`）跳过整个 MIR 流水线，直接从汇编源进入
`lib/MC/MCParser` + 目标专属的 `RISCVAsmParser.cpp` 等。

---

## §2. 文件目录结构

```
llvm/lib/CodeGen/
├── CMakeLists.txt                ← LLVMCodeGen 组件编译入口
│
├── 顶层 ~250 个 .cpp/.h          ← target-independent Pass / 数据结构 / 分析
│
├── AsmPrinter/        ~47 个文件   MF -> 汇编的发射层 (含 DWARF/EH/GC 全部子模块)
├── SelectionDAG/      ~33 个文件   DAG 指令选择与合法化 (传统路径)
├── GlobalISel/        ~30 个文件   GlobalISel 现代指令选择框架
├── LiveDebugValues/   ~5 个文件    dbg.value/dbg.declare 区间传播
└── MIRParser/         ~5 个文件    .mir 文本格式解析
```

### 2.1 子目录角色一览

| 子目录 | 核心职责 | 关键类/入口 |
|--------|----------|-------------|
| `AsmPrinter/` | MIR → 汇编 + 调试/EH 元数据发射 | `AsmPrinter`, `DwarfDebug`, `EHStreamer` |
| `SelectionDAG/` | DAG 指令选择与合法化（传统） | `SelectionDAG`, `SelectionDAGISel`, `DAGCombiner` |
| `GlobalISel/` | GlobalISel 框架（IR→Generic MIR→目标） | `IRTranslator`, `Legalizer`, `RegBankSelect`, `InstructionSelect` |
| `LiveDebugValues/` | 寄存器分配后 dbg.value 区间传播 | `LiveDebugValues`, `VarLocBasedImpl`, `InstrRefBasedImpl` |
| `MIRParser/` | `.mir` 文本解析回 `MachineFunction` | `MIRParser`, `MIParser`, `MILexer` |

### 2.2 顶层文件按职责分类（12 大类，约 250 个文件）

| # | 分类 | 文件数（约） | 代表 Pass / 类 |
|---|------|------------|----------------|
| 1 | 指令选择与 lowering 基础设施 | ~12 | `CodeGenPrepare`, `AtomicExpandPass`, `ExpandPostRAPseudos` |
| 2 | 寄存器分配与活跃区间 | ~37 | `RegAllocBase/Fast/Greedy/Basic/PBQP`, `LiveIntervals`, `VirtRegMap`, `RegisterCoalescer` |
| 3 | 栈帧 / 序言与尾声 | ~10 | `PrologEpilogInserter`, `TargetFrameLoweringImpl`, `StackProtector`, `ShrinkWrap` |
| 4 | 分支折叠 / CFG 优化 | ~14 | `BranchFolding`, `BranchRelaxation`, `MachineBlockPlacement`, `TailDuplication` |
| 5 | 调度与冒险检测 | ~16 | `ScheduleDAGInstrs`, `MachineScheduler`, `PostRASchedulerList`, `DFAPacketizer`, `MachinePipeliner` |
| 6 | 调用约定与 lowering 工具 | ~8 | `CallingConvLower`, `TargetLoweringBase`, `CommandFlags` |
| 7 | MIR 表示与基础数据结构 | ~37 | `MachineFunction`, `MachineInstr`, `MachineOperand`, `MIRPrinter`, `MachineFrameInfo` |
| 8 | 调试 / 异常 / CFI / 安全 | ~28 | `DwarfEHPrepare`, `WinEHPrepare`, `CFIInstrInserter`, `KCFI`, `StackMaps`, `XRayInstrumentation` |
| 9 | CFG / MIR 分析与图算法 | ~18 | `MachineDominators`, `MachineLoopInfo`, `MachineBlockFrequencyInfo`, `MachineVerifier` |
| 10 | 通用基础设施 / Target 抽象 | ~17 | `CodeGen`, `TargetPassConfig`, `TargetInstrInfo`, `TargetRegisterInfo`, `BasicTargetTransformInfo` |
| 11 | 性能微优化 / IR 整理 | ~36 | `MachineCSE`, `MachineLICM`, `PeepholeOptimizer`, `MachineCombiner`, `MachineOutliner`, `InterleavedAccessPass` |
| 12 | GC 与运行时支持 | ~5 | `GCMetadata`, `GCRootLowering`, `SwiftErrorValueTracking` |

---

## §3. 文件详解

### 3.1 顶层目录 — target-independent Pass / 数据结构

#### 类别 1：指令选择与 lowering 基础设施（IR → MIR）
- [`CodeGenPrepare.cpp`](CodeGenPrepare.cpp) — ISel 前的 IR 优化（地址模式改写 / 类型提升 / select→br）
- [`AtomicExpandPass.cpp`](AtomicExpandPass.cpp) — 把 atomic ops 扩展为 LL/SC 序列
- [`ComplexDeinterleavingPass.cpp`](ComplexDeinterleavingPass.cpp) — 复数交错访存拆成实/虚部独立操作
- [`ExpandReductions.cpp`](ExpandReductions.cpp) — 在 ISel 前把 reduction 规约到目标指令
- [`ExpandVectorPredication.cpp`](ExpandVectorPredication.cpp) — 谓词向量指令 lower 到目标形态
- [`PreISelIntrinsicLowering.cpp`](PreISelIntrinsicLowering.cpp) — ISel 前把剩余 intrinsic lower 掉
- [`LowerEmuTLS.cpp`](LowerEmuTLS.cpp) — 把 TLS 变量替换为 `__emutls_*` 模拟实现
- [`FinalizeISel.cpp`](FinalizeISel.cpp) — 指令选择结束后清理 flags / 垃圾指令
- [`ExpandPostRAPseudos.cpp`](ExpandPostRAPseudos.cpp) — 寄存器分配后再展开伪指令
- 关键类：`AtomicExpandImpl`、`AtomicExpandLegacy`、`ExpandPostRA`、`ExpandPostRALegacy`

#### 类别 2：寄存器分配与活跃区间
- **分配器**：
  - `[RegAllocBase.cpp](RegAllocBase.cpp)/.h` — 分配器公共基类
  - [`RegAllocFast.cpp`](RegAllocFast.cpp) — 快速本地分配（InstrPosIndexes）
  - `[RegAllocBasic.cpp](RegAllocBasic.cpp)/.h` — 朴素按槽分配
  - `[RegAllocGreedy.cpp](RegAllocGreedy.cpp)/.h` — 贪心权重驱动（默认分配器）
  - [`RegAllocPBQP.cpp`](RegAllocPBQP.cpp) — 基于 PBQP 求解
  - [`RegAllocEvictionAdvisor.cpp`](RegAllocEvictionAdvisor.cpp) + [`RegAllocPriorityAdvisor.cpp`](RegAllocPriorityAdvisor.cpp) — 驱逐/优先级建议器
  - `[MLRegAllocEvictAdvisor.cpp](MLRegAllocEvictAdvisor.cpp)/.h` + [`MLRegAllocPriorityAdvisor.cpp`](MLRegAllocPriorityAdvisor.cpp) — ML 增强建议器
- **辅助**：
  - [`VirtRegMap.cpp`](VirtRegMap.cpp) — 虚拟寄存器→物理寄存器映射及重写
  - [`CalcSpillWeights.cpp`](CalcSpillWeights.cpp) — 计算溢出权重
  - [`InlineSpiller.cpp`](InlineSpiller.cpp) — 内联溢出器
  - `[SplitKit.cpp](SplitKit.cpp)/.h` — LiveRange 区间拆分工具
  - [`SpillPlacement.cpp`](SpillPlacement.cpp) — 溢出位置选择
  - `[AllocationOrder.cpp](AllocationOrder.cpp)/.h` — 同类寄存器分配顺序启发式
  - [`Rematerializer.cpp`](Rematerializer.cpp) — 重物质化支持
- **活跃区间分析**：
  - `[LiveIntervals.cpp](LiveIntervals.cpp)/.h` — 活跃区间主入口
  - `[LiveInterval.cpp](LiveInterval.cpp)/.h` + [`LiveIntervalCalc.cpp`](LiveIntervalCalc.cpp) + [`LiveRangeCalc.cpp`](LiveRangeCalc.cpp) + [`LiveRangeEdit.cpp`](LiveRangeEdit.cpp) + [`LiveRangeShrink.cpp`](LiveRangeShrink.cpp) + [`LiveRangeUtils.h`](LiveRangeUtils.h) — 区间数据/工具
  - [`LiveIntervalUnion.cpp`](LiveIntervalUnion.cpp) — 跨块查询并集
  - [`LiveRegMatrix.cpp`](LiveRegMatrix.cpp) — 活跃虚区 × 物理寄存器占用矩阵
  - [`LiveRegUnits.cpp`](LiveRegUnits.cpp) + [`LivePhysRegs.cpp`](LivePhysRegs.cpp) + [`LiveStacks.cpp`](LiveStacks.cpp) — 不同粒度的活跃集合
  - [`LiveDebugVariables.cpp`](LiveDebugVariables.cpp) — 调试值区间扩展
  - [`LiveVariables.cpp`](LiveVariables.cpp) — 寄存器粒度活跃变量（已被 LiveIntervals 取代）
  - `[InterferenceCache.cpp](InterferenceCache.cpp)/.h` — 区间冲突缓存
- **其他**：
  - [`RegisterCoalescer.cpp`](RegisterCoalescer.cpp) — 同值合并（JoinVals）
  - [`RegisterPressure.cpp`](RegisterPressure.cpp) — 寄存器压力追踪

#### 类别 3：栈帧 / 序言与尾声
- [`PrologEpilogInserter.cpp`](PrologEpilogInserter.cpp) — 插入函数序言尾声、保存 callee-saved
- [`TargetFrameLoweringImpl.cpp`](TargetFrameLoweringImpl.cpp) — 目标无关的 TargetFrameLowering 基类
- [`LocalStackSlotAllocation.cpp`](LocalStackSlotAllocation.cpp) — 把 spill 合并到局部栈槽
- [`StackSlotColoring.cpp`](StackSlotColoring.cpp) — 复用栈槽消除冗余
- [`StackColoring.cpp`](StackColoring.cpp) — 栈对象间染色以共享
- [`SafeStack.cpp`](SafeStack.cpp) + `[SafeStackLayout.cpp](SafeStackLayout.cpp)/.h` — 安全栈保护
- [`ShrinkWrap.cpp`](ShrinkWrap.cpp) — 缩小 prologue/epilogue 覆盖范围
- [`StackFrameLayoutAnalysisPass.cpp`](StackFrameLayoutAnalysisPass.cpp) — 分析栈帧布局
- [`StackProtector.cpp`](StackProtector.cpp) — 插入 canary 栈保护
- [`FuncletLayout.cpp`](FuncletLayout.cpp) — Windows funclet 块布局
- 关键类：`PEIImpl`、`PEILegacy`、`SafeStackLegacyPass`、`ShrinkWrapImpl`

#### 类别 4：分支折叠 / CFG 优化
- `[BranchFolding.cpp](BranchFolding.cpp)/.h` — 合并相同尾/头块、消除 fall-through
- [`BranchRelaxation.cpp`](BranchRelaxation.cpp) — 把超出范围的跳转改用远跳转
- [`MachineBlockPlacement.cpp`](MachineBlockPlacement.cpp) — 依据 profile 重排基本块
- [`TailDuplication.cpp`](TailDuplication.cpp) + [`TailDuplicator.cpp`](TailDuplicator.cpp) — 尾部复制以消除分支
- [`IfConversion.cpp`](IfConversion.cpp) — 三角/菱形 CFG 转条件传送
- [`EarlyIfConversion.cpp`](EarlyIfConversion.cpp) — SSA 形态下的早期 IfConv
- [`UnreachableBlockElim.cpp`](UnreachableBlockElim.cpp) — 删除不可达基本块
- [`BasicBlockSections.cpp`](BasicBlockSections.cpp) + [`BasicBlockSectionsProfileReader.cpp`](BasicBlockSectionsProfileReader.cpp) — 按 section 切分基本块
- [`BasicBlockMatchingAndInference.cpp`](BasicBlockMatchingAndInference.cpp) — 推测/推断基础块对应
- [`BasicBlockPathCloning.cpp`](BasicBlockPathCloning.cpp) — 复制 CFG 路径以优化布局
- [`EdgeBundles.cpp`](EdgeBundles.cpp) — CFG 边打包便于 layout
- [`LoopTraversal.cpp`](LoopTraversal.cpp) — CFG 循环遍历辅助
- 关键类：`BranchFolderLegacy`、`TailDuplicateBaseLegacy`、`BasicBlockSections`、`StaleMatcher`

#### 类别 5：调度与冒险检测
- **调度框架**：
  - [`ScheduleDAG.cpp`](ScheduleDAG.cpp) + [`ScheduleDAGInstrs.cpp`](ScheduleDAGInstrs.cpp) + [`ScheduleDAGPrinter.cpp`](ScheduleDAGPrinter.cpp) — 通用调度基类
  - [`MachineScheduler.cpp`](MachineScheduler.cpp) + [`VLIWMachineScheduler.cpp`](VLIWMachineScheduler.cpp) — 通用/VLIW 调度器
  - [`PostRASchedulerList.cpp`](PostRASchedulerList.cpp) — PostRA 调度入口
  - [`PostRAHazardRecognizer.cpp`](PostRAHazardRecognizer.cpp) — PostRA 冒险识别
- **流水线 / VLIW**：
  - [`ModuloSchedule.cpp`](ModuloSchedule.cpp) + [`MachinePipeliner.cpp`](MachinePipeliner.cpp) — 软件流水线核心
  - [`DFAPacketizer.cpp`](DFAPacketizer.cpp) — DFA 驱动 VLIW 打包器
  - [`MacroFusion.cpp`](MacroFusion.cpp) — 指令宏融合
- **冒险检测器**：
  - `[AggressiveAntiDepBreaker.cpp](AggressiveAntiDepBreaker.cpp)/.h` + `[CriticalAntiDepBreaker.cpp](CriticalAntiDepBreaker.cpp)/.h` — 打破寄存器反相关
  - [`ScoreboardHazardRecognizer.cpp`](ScoreboardHazardRecognizer.cpp) — 计分板式
  - [`MultiHazardRecognizer.cpp`](MultiHazardRecognizer.cpp) + [`WindowScheduler.cpp`](WindowScheduler.cpp) — 多窗口/窗口式
  - [`LatencyPriorityQueue.cpp`](LatencyPriorityQueue.cpp) — 延迟敏感的指令优先级队列
- **硬件循环**：[`HardwareLoops.cpp`](HardwareLoops.cpp) — 硬件循环指令降级

#### 类别 6：调用约定与 lowering 工具
- [`CallingConvLower.cpp`](CallingConvLower.cpp) — 实现 CCState 调用约定解析
- [`TargetLoweringBase.cpp`](TargetLoweringBase.cpp) — TargetLowering 抽象基类
- [`TargetLoweringObjectFileImpl.cpp`](TargetLoweringObjectFileImpl.cpp) — 目标对象文件 lowering
- [`TargetOptionsImpl.cpp`](TargetOptionsImpl.cpp) — TargetOptions 内部表示
- [`TargetSchedule.cpp`](TargetSchedule.cpp) — TargetSchedule 调度模型基类
- [`LibcallLoweringInfo.cpp`](LibcallLoweringInfo.cpp) — RuntimeLibcallInfo legacy 包装
- [`IntrinsicLowering.cpp`](IntrinsicLowering.cpp) — 默认 intrinsic lowering 实现
- [`CommandFlags.cpp`](CommandFlags.cpp) — codegen 共享命令行选项

#### 类别 7：MIR 表示与基础数据结构
- **MIR 序列化**：
  - [`MIRPrinter.cpp`](MIRPrinter.cpp) + [`MIRPrintingPass.cpp`](MIRPrintingPass.cpp) — 序列化 MIR 到文本
  - [`MIRNamerPass.cpp`](MIRNamerPass.cpp) + `[MIRVRegNamerUtils.cpp](MIRVRegNamerUtils.cpp)/.h` — vreg 稳定命名
  - [`MIRCanonicalizerPass.cpp`](MIRCanonicalizerPass.cpp) — 把 MIR 规范化为等价形式
  - [`MIRFSDiscriminator.cpp`](MIRFSDiscriminator.cpp) — FS 自动微分 discriminator
  - [`MIRSampleProfile.cpp`](MIRSampleProfile.cpp) — 基于 sample profile 的 MIR 注释
  - [`MIRYamlMapping.cpp`](MIRYamlMapping.cpp) — MIR ↔ YAML 映射
  - [`MIR2Vec.cpp`](MIR2Vec.cpp) — MIR 嵌入为向量表示
- **核心容器**：
  - [`MachineFunction.cpp`](MachineFunction.cpp) + [`MachineFunctionPass.cpp`](MachineFunctionPass.cpp) + [`MachineFunctionAnalysis.cpp`](MachineFunctionAnalysis.cpp) + [`MachineFunctionPrinterPass.cpp`](MachineFunctionPrinterPass.cpp) + [`MachineFunctionSplitter.cpp`](MachineFunctionSplitter.cpp)
  - [`MachineBasicBlock.cpp`](MachineBasicBlock.cpp) + [`MachineInstr.cpp`](MachineInstr.cpp) + [`MachineInstrBundle.cpp`](MachineInstrBundle.cpp) + [`MachineOperand.cpp`](MachineOperand.cpp)
  - [`MachineFrameInfo.cpp`](MachineFrameInfo.cpp) + [`MachineModuleInfo.cpp`](MachineModuleInfo.cpp) + [`MachineModuleInfoImpls.cpp`](MachineModuleInfoImpls.cpp) + [`MachineModuleSlotTracker.cpp`](MachineModuleSlotTracker.cpp)
  - [`MachineRegisterInfo.cpp`](MachineRegisterInfo.cpp) + [`MachineSSAUpdater.cpp`](MachineSSAUpdater.cpp) + [`MachineSSAContext.cpp`](MachineSSAContext.cpp) + [`MachineIDFSSAUpdater.cpp`](MachineIDFSSAUpdater.cpp)
- **辅助**：
  - [`MachineOptimizationRemarkEmitter.cpp`](MachineOptimizationRemarkEmitter.cpp) + [`MachineStableHash.cpp`](MachineStableHash.cpp) + [`LowLevelTypeUtils.cpp`](LowLevelTypeUtils.cpp) + [`PseudoSourceValue.cpp`](PseudoSourceValue.cpp)
  - [`ResetMachineFunctionPass.cpp`](ResetMachineFunctionPass.cpp) + [`NonRelocatableStringpool.cpp`](NonRelocatableStringpool.cpp)

#### 类别 8：调试信息 / 异常 / CFI / 安全元数据
- **EH 准备**：
  - [`DwarfEHPrepare.cpp`](DwarfEHPrepare.cpp) — DWARF CFI/eh 准备（IR 层）
  - [`WinEHPrepare.cpp`](WinEHPrepare.cpp) — Windows EH 准备
  - [`WasmEHPrepare.cpp`](WasmEHPrepare.cpp) — WebAssembly EH 准备
  - [`SjLjEHPrepare.cpp`](SjLjEHPrepare.cpp) — setjmp/longjmp EH 准备
- **CFI / CFG**：
  - [`CFIInstrInserter.cpp`](CFIInstrInserter.cpp) — 插入 `.cfi_*` 指令
  - [`CFIFixup.cpp`](CFIFixup.cpp) — 修整 CFI 偏移
  - [`CFGuardLongjmp.cpp`](CFGuardLongjmp.cpp) — 插入 `__guard_dispatch_icall` / longjmp CFG 守卫
  - [`EHContGuardTargets.cpp`](EHContGuardTargets.cpp) — Windows EH continuation guard
  - [`KCFI.cpp`](KCFI.cpp) — KCFI 类型校验
- **StackMap / FaultMap**：
  - [`StackMaps.cpp`](StackMaps.cpp) — 收集/打印 StackMap 区段
  - [`StackMapLivenessAnalysis.cpp`](StackMapLivenessAnalysis.cpp) — 计算 StackMap 点的寄存器活跃
  - [`FaultMaps.cpp`](FaultMaps.cpp) — Faulting maps for landingpads
- **调试变量管理**：
  - [`MachineDebugify.cpp`](MachineDebugify.cpp) + [`MachineCheckDebugify.cpp`](MachineCheckDebugify.cpp) — debugify / check-debugify
  - [`MachineStripDebug.cpp`](MachineStripDebug.cpp) + [`RemoveRedundantDebugValues.cpp`](RemoveRedundantDebugValues.cpp)
  - [`AssignmentTrackingAnalysis.cpp`](AssignmentTrackingAnalysis.cpp) — 变量→存储位置的 assignment tracking
  - [`DroppedVariableStatsMIR.cpp`](DroppedVariableStatsMIR.cpp) — 统计调试变量丢失
  - [`LexicalScopes.cpp`](LexicalScopes.cpp) — 构建 lexical scope
  - [`JMCInstrumenter.cpp`](JMCInstrumenter.cpp) — 插入 JMC 探针
- **运行时插桩**：
  - [`PatchableFunction.cpp`](PatchableFunction.cpp) — 函数 entry 改成可热修补
  - [`XRayInstrumentation.cpp`](XRayInstrumentation.cpp) — XRay 函数 instrumentation
  - [`WindowsSecureHotPatching.cpp`](WindowsSecureHotPatching.cpp) — Windows 安全热补丁支持
  - [`FEntryInserter.cpp`](FEntryInserter.cpp) — 函数入口加 `.fentry` 标记
  - [`ShadowStackGCLowering.cpp`](ShadowStackGCLowering.cpp) — GC 影子栈 lowering
  - [`SwiftErrorValueTracking.cpp`](SwiftErrorValueTracking.cpp) — 跟踪 swift error 寄存器

#### 类别 9：CFG / MIR 分析与图算法
- [`MachineDominators.cpp`](MachineDominators.cpp) + [`MachinePostDominators.cpp`](MachinePostDominators.cpp) + [`MachineDomTreeUpdater.cpp`](MachineDomTreeUpdater.cpp) + [`MachineDominanceFrontier.cpp`](MachineDominanceFrontier.cpp)
- [`MachineLoopInfo.cpp`](MachineLoopInfo.cpp) + [`MachineLoopUtils.cpp`](MachineLoopUtils.cpp) — 循环识别/工具
- [`MachineBlockFrequencyInfo.cpp`](MachineBlockFrequencyInfo.cpp) + [`LazyMachineBlockFrequencyInfo.cpp`](LazyMachineBlockFrequencyInfo.cpp) + [`MBFIWrapper.cpp`](MBFIWrapper.cpp)
- [`MachineBranchProbabilityInfo.cpp`](MachineBranchProbabilityInfo.cpp) + [`MachineRegionInfo.cpp`](MachineRegionInfo.cpp)
- [`MachineUniformityAnalysis.cpp`](MachineUniformityAnalysis.cpp) + [`MachineCycleAnalysis.cpp`](MachineCycleAnalysis.cpp) + [`MachineConvergenceVerifier.cpp`](MachineConvergenceVerifier.cpp)
- [`MachineSizeOpts.cpp`](MachineSizeOpts.cpp) + [`MachineCFGPrinter.cpp`](MachineCFGPrinter.cpp) + [`MachineTraceMetrics.cpp`](MachineTraceMetrics.cpp) + [`MachineBlockHashInfo.cpp`](MachineBlockHashInfo.cpp)
- [`MachineVerifier.cpp`](MachineVerifier.cpp) + [`ReachingDefAnalysis.cpp`](ReachingDefAnalysis.cpp) + [`SwitchLoweringUtils.cpp`](SwitchLoweringUtils.cpp)

#### 类别 10：通用基础设施 / Target 抽象
- [`CodeGen.cpp`](CodeGen.cpp) — CodeGen 命令行选项与公共初始化
- [`CodeGenCommonISel.cpp`](CodeGenCommonISel.cpp) — GlobalISel 与 SelectionDAG 共享的 lowering 工具
- [`CodeGenTargetMachineImpl.cpp`](CodeGenTargetMachineImpl.cpp) — TargetMachine 基类实现
- [`Analysis.cpp`](Analysis.cpp) — CodeGen Analysis 注册中心
- `TargetPassConfig.cpp` — 目标相关 pass pipeline 配置（**CodeGen pipeline 总控**）
- [`TargetInstrInfo.cpp`](TargetInstrInfo.cpp) — TargetInstrInfo 基类
- [`TargetRegisterInfo.cpp`](TargetRegisterInfo.cpp) — TargetRegisterInfo 基类
- [`TargetSubtargetInfo.cpp`](TargetSubtargetInfo.cpp) — TargetSubtargetInfo 基类
- [`BasicTargetTransformInfo.cpp`](BasicTargetTransformInfo.cpp) — 不含目标 hook 的 TTI 抽象
- [`MachinePassManager.cpp`](MachinePassManager.cpp) — 机器函数 pass manager 支持
- [`RegisterBank.cpp`](RegisterBank.cpp) + [`RegisterBankInfo.cpp`](RegisterBankInfo.cpp) + [`RegisterClassInfo.cpp`](RegisterClassInfo.cpp)
- [`RegisterScavenging.cpp`](RegisterScavenging.cpp) + [`RegisterUsageInfo.cpp`](RegisterUsageInfo.cpp) + [`RegUsageInfoCollector.cpp`](RegUsageInfoCollector.cpp) + [`RegUsageInfoPropagate.cpp`](RegUsageInfoPropagate.cpp)

#### 类别 11：性能微优化 / IR 整理
- **指令级优化**：
  - [`DeadMachineInstructionElim.cpp`](DeadMachineInstructionElim.cpp) + [`DetectDeadLanes.cpp`](DetectDeadLanes.cpp) + [`InitUndef.cpp`](InitUndef.cpp)
  - [`RenameIndependentSubregs.cpp`](RenameIndependentSubregs.cpp) + [`ProcessImplicitDefs.cpp`](ProcessImplicitDefs.cpp)
  - [`MachineCopyPropagation.cpp`](MachineCopyPropagation.cpp) + [`MachineCSE.cpp`](MachineCSE.cpp) + [`MachineLICM.cpp`](MachineLICM.cpp) + [`MachineSink.cpp`](MachineSink.cpp)
  - [`MachineLateInstrsCleanup.cpp`](MachineLateInstrsCleanup.cpp) + [`PeepholeOptimizer.cpp`](PeepholeOptimizer.cpp) + [`MachineCombiner.cpp`](MachineCombiner.cpp)
  - [`TwoAddressInstructionPass.cpp`](TwoAddressInstructionPass.cpp) + [`BreakFalseDeps.cpp`](BreakFalseDeps.cpp) + [`ExecutionDomainFix.cpp`](ExecutionDomainFix.cpp)
- **IR / 向量层**：
  - [`TypePromotion.cpp`](TypePromotion.cpp) — 窄整型提升到合法宽度
  - [`InsertCodePrefetch.cpp`](InsertCodePrefetch.cpp) + [`InterleavedAccessPass.cpp`](InterleavedAccessPass.cpp) + [`InterleavedLoadCombinePass.cpp`](InterleavedLoadCombinePass.cpp)
  - [`ReplaceWithVeclib.cpp`](ReplaceWithVeclib.cpp) + [`ExpandIRInsts.cpp`](ExpandIRInsts.cpp)
- **状态点 / 特殊 Pass**：
  - [`FixupStatepointCallerSaved.cpp`](FixupStatepointCallerSaved.cpp) + [`ImplicitNullChecks.cpp`](ImplicitNullChecks.cpp) + [`IndirectBrExpandPass.cpp`](IndirectBrExpandPass.cpp) + [`InlineAsmPrepare.cpp`](InlineAsmPrepare.cpp)
- **PHI / Select**：
  - [`PHIElimination.cpp`](PHIElimination.cpp) + `[PHIEliminationUtils.cpp](PHIEliminationUtils.cpp)/.h` + [`OptimizePHIs.cpp`](OptimizePHIs.cpp) + [`SelectOptimize.cpp`](SelectOptimize.cpp)
- **跨函数 / 模块级**：
  - [`MachineOutliner.cpp`](MachineOutliner.cpp) — 跨函数抽取重复指令序列
  - [`GlobalMerge.cpp`](GlobalMerge.cpp) + [`GlobalMergeFunctions.cpp`](GlobalMergeFunctions.cpp) — 短全局/函数合并
- **采样 / 安全元数据**：
  - [`PseudoProbeInserter.cpp`](PseudoProbeInserter.cpp) + [`RemoveLoadsIntoFakeUses.cpp`](RemoveLoadsIntoFakeUses.cpp) + [`SanitizerBinaryMetadata.cpp`](SanitizerBinaryMetadata.cpp)
  - [`StaticDataAnnotator.cpp`](StaticDataAnnotator.cpp) + [`StaticDataSplitter.cpp`](StaticDataSplitter.cpp)

#### 类别 12：GC 与运行时支持
- [`GCMetadata.cpp`](GCMetadata.cpp) + [`GCMetadataPrinter.cpp`](GCMetadataPrinter.cpp) + [`GCRootLowering.cpp`](GCRootLowering.cpp)
- [`GCEmptyBasicBlocks.cpp`](GCEmptyBasicBlocks.cpp) — 移除空 GC 转换块
- `SwiftErrorValueTracking.cpp` — 跟踪 swift error 寄存器（也见 §8）

---

### 3.2 AsmPrinter/

#### Core 核心
- `AsmPrinter.cpp` — MachineFunction→汇编的核心 Pass（`AsmPrinter` / `SetupMachineFunction` / `emitFunctionBody` / `doFinalization`）
- `AsmPrinterInlineAsm.cpp` — MachineInstr 中的内联汇编处理
- `AsmPrinterDwarf.cpp` — 通用 DWARF/CFI 编码工具（leb128 / CFI 指令）
- `DebugHandlerBase.cpp` — 所有调试处理器的基类
- `ByteStreamer.h` — 字节流包装工具
- `CodeViewDebug.cpp/.h` — Microsoft CodeView 调试信息发射（Windows PDB）

#### DWARF 调试信息
- `DwarfDebug.cpp/.h` — DWARF 主入口，生成 `.debug_info` / `.debug_line` / `.debug_range`
- `DwarfUnit.cpp/.h` — DWARF 编译/类型单元抽象
- `DwarfCompileUnit.cpp/.h` — 编译单元专用逻辑（行号、变量位置）
- `DwarfFile.cpp/.h` — 单 .debug_info 文件视图
- `DwarfExpression.cpp/.h` — LLVM `DIExpression` 编码为 DWARF 位置表达式
- `DwarfStringPool.cpp/.h` — `.debug_str` / `.debug_str_offsets` 字符串去重池
- `DIE.cpp/.h` + `DIEHash.cpp/.h` + `DIEHashAttributes.def` — DWARF DIE 节点及 type unit 哈希
- `AccelTable.cpp/.h` — `.debug_pubnames` / `.debug_pubtypes` / DWARF5 `.debug_names`
- `AddressPool.cpp/.h` — `.debug_addr` 段地址池
- `DebugLocStream.cpp/.h` + `DebugLocEntry.h` — `.debug_loc` / `.debug_loclists` 字节流
- `DbgEntityHistoryCalculator.cpp` — 收集变量/标签在某指令点的历史区间

#### 异常处理（目标相关）
- `EHStreamer.cpp/.h` — 异常流发射器共同基类
- `AIXException.cpp` — AIX 平台 XCOFF LSDA
- `ARMException.cpp` — ARM EHABI（`.ARM.extab` / `.ARM.exidx`）
- `DwarfCFIException.cpp` — Itanium ABI（`.eh_frame` + LSDA）
- `WasmException.cpp/.h` — WebAssembly 异常
- `WinException.cpp/.h` — Windows SEH / C++ EH（`.pdata` / `.xdata`）
- `DwarfException.h` — DWARF exception 共享基类

#### GC / PseudoProbe / Windows 安全（杂项）
- `ErlangGCPrinter.cpp` — Erlang 运行时 GC 根描述符
- `OcamlGCPrinter.cpp` — OCaml 运行时 GC 根描述符
- `PseudoProbePrinter.cpp/.h` — AutoFDO / SamplePGO 伪探针发射
- `WinCFGuard.cpp/.h` — Windows CFG（控制流防护）`__guard_*` 表发射

---

### 3.3 SelectionDAG/

- `SelectionDAG.cpp` — `SelectionDAG` 主类，集中节点创建 / CSE / folding
- `SelectionDAGISel.cpp` — 把 DAG 节点匹配到目标指令的主入口
- `SelectionDAGBuilder.cpp/.h` — 把 LLVM IR 翻译为 SelectionDAG
- `DAGCombiner.cpp` — DAG 级模式合并与优化
- `LegalizeTypes.cpp/.h` + `LegalizeDAG.cpp` + `LegalizeFloatTypes.cpp` + `LegalizeIntegerTypes.cpp` + `LegalizeVectorOps.cpp` + `LegalizeVectorTypes.cpp` + `LegalizeTypesGeneric.cpp` — 把非法类型/操作降级
- `InstrEmitter.cpp/.h` — 把选中节点发射成 MachineInstr
- `ScheduleDAGSDNodes.cpp/.h` — 列表式机器指令调度（`ScheduleDAGSDNodes`）
- `ScheduleDAGFast.cpp` + `ScheduleDAGRRList.cpp` + `ScheduleDAGVLIW.cpp` — 快速/资源优先级/VLIW 调度
- `TargetLowering.cpp` — 目标相关 lowering 规则查询
- `FunctionLoweringInfo.cpp` — 函数级 IR 状态到 DAG 的桥梁
- `SelectionDAGAddressAnalysis.cpp` — DAG 寻址模式分析
- `SelectionDAGDumper.cpp` + `SelectionDAGPrinter.cpp` — DAG 可视化/打印
- `SelectionDAGTargetInfo.cpp` — 目标专属的 DAG 调整钩子
- `StatepointLowering.cpp/.h` — gc.statepoint 的 lowering
- `FastISel.cpp` — 快速指令选择 fallback（单指令级别）
- `SDNodeDbgValue.h` + `SDNodeInfo.cpp` — SDNode 调试/反查

---

### 3.4 GlobalISel/

- `GlobalISel.cpp` — pipeline 入口与模式注册
- `IRTranslator.cpp` — 把 LLVM IR 翻译到 Generic MIR
- `Legalizer.cpp` + `LegalizerHelper.cpp` + `LegalizerInfo.cpp` — 把非法 Generic MIR 操作合法化
- `LegalityPredicates.cpp` + `LegalizeMutations.cpp` — 合法化谓词与变换
- `RegBankSelect.cpp` — 为 vreg 选择 RegisterBank
- `InstructionSelect.cpp` + `InstructionSelector.cpp` — 把 Generic MIR 匹配到目标指令
- `MachineIRBuilder.cpp` — 构造/修改 MIR 的统一入口
- `CallLowering.cpp` + `InlineAsmLowering.cpp` — call/ret/inline asm 的 lowering
- `Combiner.cpp` + `CombinerHelper.cpp` + `CombinerHelperArtifacts.cpp` + `CombinerHelperCasts.cpp` + `CombinerHelperCompares.cpp` + `CombinerHelperVectorOps.cpp` — Generic MIR 上的组合优化
- `CSEInfo.cpp` + `CSEMIRBuilder.cpp` — MIR 上的 CSE
- `GIMatchTableExecutor.cpp` — 声明式指令选择匹配表执行器
- `GISelChangeObserver.cpp` + `GISelValueTracking.cpp` — MIR 变更观察与值追踪
- `Localizer.cpp` — 把跨块 vreg 局部化
- `LoadStoreOpt.cpp` — Generic MIR 的 ld/st 优化
- `LostDebugLocObserver.cpp` — 调试位置丢失检测
- `MachineFloatingPointPredicateUtils.cpp` — FP 谓词工具
- `Utils.cpp` — 共享工具函数

---

### 3.5 LiveDebugValues/

- `LiveDebugValues.cpp/.h` — Pass 入口与共享基类（`LiveDebugValues`、`LiveDebugValuesLegacy`、`LDVImpl`）
- `VarLocBasedImpl.cpp` — 经典基于 VarLoc 的实现（`VarLocBasedLDV`、`transferRegisterDef`）
- `InstrRefBasedImpl.cpp/.h` — 基于指令序号的改进实现（`InstrRefBasedLDV`、`TransferTracker`、`MLocTracker`）

---

### 3.6 MIRParser/

- `MIRParser.cpp` — 顶层解析入口与 MachineFunction 装配（`MIRParser`、`parseMachineFunctions`）
- `MIParser.cpp` — 实际递归下降解析每条 MachineInstr / 寄存器 / 立即数
- `MILexer.cpp/.h` — 把 `.mir` 文本切成 token
- `CMakeLists.txt` — 构建脚本（无类）

---

## §4. 关键调用链

### 4.1 完整编译（IR → .o）

```
LLVMTargetMachine::addPassesToEmitFile
  └─ CodeGenPassBuilder / TargetPassConfig        ← §3.1 类别 10
     ├─ IR 预处理                                  ← §3.1 类别 1
     │   └─ CodeGenPrepare → AtomicExpand → ...
     ├─ 指令选择 (二选一)
     │   ├─ SelectionDAG:
     │   │     SelectionDAGBuilder → SelectionDAG → DAGCombiner
     │   │       → LegalizeTypes / LegalizeDAG → SelectionDAGISel
     │   │         → InstrEmitter (生成 MIR)        ← §3.3
     │   └─ GlobalISel:
     │         IRTranslator → Legalizer → RegBankSelect
     │           → InstructionSelect (生成 MIR)    ← §3.4
     ├─ Pre-RA Passes                              ← §3.1 类别 2~4, 11
     │   PHIElimination → TwoAddressInstructionPass
     │     → RegisterCoalescer → MachineCSE → MachineLICM
     │     → MachineScheduler → BranchFolding → ...
     ├─ 寄存器分配                                 ← §3.1 类别 2
     │   LiveIntervals 计算 → RegAllocGreedy/Fast/Basic/PBQP
     │     → VirtRegMap 重写
     ├─ Post-RA Passes                              ← §3.1 类别 2, 3, 5, 8, 11
     │   PrologEpilogInserter → StackProtector → LiveDebugValues
     │     → CFIInstrInserter → ... → MachineOutliner
     └─ AsmPrinter                                 ← §3.2
        DwarfDebug → DWARF 调试信息
        EHStreamer 子类 (Itanium/ARM/Win/Wasm/AIX) → 异常表
        → emitFunctionBody → target RISCVAsmPrinter → MC 层
```

### 4.2 寄存器分配主路径

```
RegAllocBase::run
  ├─ LiveIntervals::computeVirtRegs           ← 活跃区间
  ├─ RegAllocGreedy (默认):
  │   ├─ RegAllocEvictionAdvisor 选择驱逐 vreg
  │   ├─ CalcSpillWeights 计算权重
  │   ├─ SplitKit 拆分区间
  │   ├─ InlineSpiller 溢出/重物质化
  │   └─ 反复迭代直到收敛
  ├─ VirtRegMap::rewrite                       ← 把 vreg 重写为 preg
  └─ LiveStacks / LiveDebugVariables 处理
```

### 4.3 AsmPrinter 主路径

```
AsmPrinter::runOnMachineFunction
  ├─ SetupMachineFunction
  │   ├─ DwarfDebug::beginFunction           ← DWARF 调试入口
  │   ├─ EHStreamer 子类::beginFunction      ← ItaniumCFI / ARM / Win / Wasm / AIX
  │   └─ StackMap / FaultMap 准备
  ├─ emitFunctionBody
  │   ├─ 遍历每个基本块 emitInstruction
  │   └─ 目标专属 AsmPrinter (RISCVAsmPrinter) 处理 MCInst
  └─ emitEndOfAsmFile
      ├─ DwarfDebug::endFunction
      ├─ EHStreamer 子类::endFunction
      └─ doFinalization → 落盘所有段
```

---

## §5. 推荐阅读顺序

按"读懂 LLVM CodeGen 框架"的目标，建议按以下顺序：

### 阶段 1：编译流水线全景（1-2 小时）
1. [`CMakeLists.txt`](CMakeLists.txt) — LLVMCodeGen 组件入口。
2. [`TargetPassConfig.cpp`](TargetPassConfig.cpp) — CodeGen pipeline 总控，决定各阶段插入哪些 Pass。
3. [`CodeGen.cpp`](CodeGen.cpp) — 命令行选项与公共初始化。
4. [`CodeGenTargetMachineImpl.cpp`](CodeGenTargetMachineImpl.cpp) — TargetMachine 基类与 CodeGen 入口的关系。
5. [`CodeGenPrepare.cpp`](CodeGenPrepare.cpp) — 看 IR 层如何改写以利于 ISel。

### 阶段 2：MIR 数据结构（2 小时）
1. [`MachineFunction.cpp`](MachineFunction.cpp) + [`MachineFunctionPass.cpp`](MachineFunctionPass.cpp) — MIR 容器基类。
2. [`MachineBasicBlock.cpp`](MachineBasicBlock.cpp) + [`MachineInstr.cpp`](MachineInstr.cpp) + [`MachineOperand.cpp`](MachineOperand.cpp) — MIR 三件套。
3. [`MachineRegisterInfo.cpp`](MachineRegisterInfo.cpp) + [`MachineFrameInfo.cpp`](MachineFrameInfo.cpp) + [`MachineModuleInfo.cpp`](MachineModuleInfo.cpp) — MIR 元数据。
4. [`MIRPrinter.cpp`](MIRPrinter.cpp) — 看 MIR 如何被序列化为 `.mir` 文本。
5. 读 `MIRParser/` 子目录 — 看 `.mir` 如何被解析回 MIR。

### 阶段 3：指令选择（3-4 小时）
- **SelectionDAG 路径**：
  1. [`SelectionDAG.cpp`](SelectionDAG/SelectionDAG.cpp) — `SelectionDAG` 主类。
  2. [`SelectionDAGBuilder.cpp`](SelectionDAG/SelectionDAGBuilder.cpp) — IR→DAG。
  3. [`DAGCombiner.cpp`](SelectionDAG/DAGCombiner.cpp) — DAG 模式合并。
  4. [`LegalizeTypes.cpp`](SelectionDAG/LegalizeTypes.cpp) — 合法化。
  5. [`SelectionDAGISel.cpp`](SelectionDAG/SelectionDAGISel.cpp) — 模式匹配与指令发射。
  6. [`InstrEmitter.cpp`](SelectionDAG/InstrEmitter.cpp) — DAG→MIR。
  7. [`ScheduleDAGSDNodes.cpp`](SelectionDAG/ScheduleDAGSDNodes.cpp) — 指令调度。
- **GlobalISel 路径**：
  1. [`GlobalISel.cpp`](GlobalISel/GlobalISel.cpp) — pipeline 入口。
  2. [`IRTranslator.cpp`](GlobalISel/IRTranslator.cpp) — IR→Generic MIR。
  3. [`Legalizer.cpp`](GlobalISel/Legalizer.cpp) + [`LegalizerInfo.cpp`](GlobalISel/LegalizerInfo.cpp) — 合法化。
  4. [`RegBankSelect.cpp`](GlobalISel/RegBankSelect.cpp) — RegisterBank 分配。
  5. [`InstructionSelect.cpp`](GlobalISel/InstructionSelect.cpp) — 指令选择。

### 阶段 4：寄存器分配（2-3 小时）
1. [`LiveIntervals.cpp`](LiveIntervals.cpp) + [`LiveRangeCalc.cpp`](LiveRangeCalc.cpp) — 活跃区间分析。
2. [`RegisterCoalescer.cpp`](RegisterCoalescer.cpp) — 同值合并。
3. [`RegAllocBase.cpp`](RegAllocBase.cpp) + [`RegAllocGreedy.cpp`](RegAllocGreedy.cpp) — 默认分配器。
4. [`CalcSpillWeights.cpp`](CalcSpillWeights.cpp) + [`InlineSpiller.cpp`](InlineSpiller.cpp) + [`SplitKit.cpp`](SplitKit.cpp) — 溢出/拆分。

### 阶段 5：栈帧与 Prologue/Epilogue（1 小时）
1. [`PrologEpilogInserter.cpp`](PrologEpilogInserter.cpp) — 主入口。
2. [`TargetFrameLoweringImpl.cpp`](TargetFrameLoweringImpl.cpp) — 目标无关基类。

### 阶段 6：后端优化与调度（2-3 小时）
- **Pre-RA**：[`BranchFolding.cpp`](BranchFolding.cpp)、[`MachineScheduler.cpp`](MachineScheduler.cpp)、[`MachineBlockPlacement.cpp`](MachineBlockPlacement.cpp)、[`TailDuplication.cpp`](TailDuplication.cpp)、[`IfConversion.cpp`](IfConversion.cpp)、[`MachineCSE.cpp`](MachineCSE.cpp)、[`MachineLICM.cpp`](MachineLICM.cpp)。
- **Post-RA**：[`PostRASchedulerList.cpp`](PostRASchedulerList.cpp)、[`LiveDebugValues.cpp`](LiveDebugValues/LiveDebugValues.cpp)（含 [`VarLocBasedImpl.cpp`](LiveDebugValues/VarLocBasedImpl.cpp) / [`InstrRefBasedImpl.cpp`](LiveDebugValues/InstrRefBasedImpl.cpp)）、[`MachineOutliner.cpp`](MachineOutliner.cpp)。

### 阶段 7：AsmPrinter 与调试/EH 发射（2-3 小时）
1. [`AsmPrinter/AsmPrinter.cpp`](AsmPrinter/AsmPrinter.cpp) — 主框架。
2. [`AsmPrinter/DwarfDebug.cpp`](AsmPrinter/DwarfDebug.cpp) + [`DwarfCompileUnit.cpp`](AsmPrinter/DwarfCompileUnit.cpp) + [`DIE.cpp`](AsmPrinter/DIE.cpp) — DWARF 核心。
3. [`AsmPrinter/EHStreamer.cpp`](AsmPrinter/EHStreamer.cpp) + [`DwarfCFIException.cpp`](AsmPrinter/DwarfCFIException.cpp) — Itanium CFI 异常表。
4. [`AsmPrinter/CodeViewDebug.cpp`](AsmPrinter/CodeViewDebug.cpp) — Windows CodeView 调试。
5. [`AsmPrinter/StackMaps.cpp`](StackMaps.cpp) + [`FaultMaps.cpp`](FaultMaps.cpp) — StackMap/FaultMap 发射。

### 阶段 8：CFG 与分析（按需）
1. [`MachineDominators.cpp`](MachineDominators.cpp) + [`MachineLoopInfo.cpp`](MachineLoopInfo.cpp)。
2. [`MachineBlockFrequencyInfo.cpp`](MachineBlockFrequencyInfo.cpp)。
3. [`MachineVerifier.cpp`](MachineVerifier.cpp) — 校验 MIR 合法性。

---

## §6. 常用操作指南

### 6.1 添加新的 target-independent Pass

1. 新建 `llvm/lib/CodeGen/MyPass.cpp`（参考 `MachineCSE.cpp` / `MachineLICM.cpp` 写法）。
2. 在 [`CMakeLists.txt`](CMakeLists.txt) 的 `LLVMCodeGen` 列表里加上 `MyPass.cpp`。
3. 实现 `class MyPass : public MachineFunctionPass` + `runOnMachineFunction`。
4. 用 `INITIALIZE_PASS(MyPass, "my-pass", "...", false, false)` 注册。
5. 在 [`TargetPassConfig.cpp`](TargetPassConfig.cpp) 的合适阶段（Pre-RA / Post-RA）插入：
   ```cpp
   addPass(&MyPassID);
   ```
6. 加 `llvm/test/CodeGen/X86/my-pass.ll` 单测，跑 `ninja check-llvm-codegen`。

### 6.2 添加新的 GlobalISel combine rule

1. 编辑 `llvm/lib/CodeGen/GlobalISel/CombinerHelper.cpp` 加新方法（参考 `matchCombine` 已有模式）。
2. 在 [`GIMatchTableExecutor.cpp`](GlobalISel/GIMatchTableExecutor.cpp) 对应的 `.td`（在 `llvm/include/llvm/Target/GlobalISel/`）里加 combine 规则。
3. 重新跑 `llvm-tblgen` 生成新的 `*GenGlobalCombiner.inc`。
4. 单测放在 `llvm/test/CodeGen/AArch86/GlobalISel/combine-*`。

### 6.3 添加新的 DWARF 调试信息字段

1. 编辑 `llvm/lib/AsmPrinter/DIE.cpp/.h` 或 `DwarfUnit.cpp/.h` 加新属性 API。
2. 在 [`DwarfDebug.cpp`](AsmPrinter/DwarfDebug.cpp) 调用新 API 把 IR/MIR 信息翻译为 DIE。
3. 单测放在 `llvm/test/CodeGen/X86/dwarf-*` / `llvm/test/DebugInfo/X86/`。

### 6.4 添加新的 MachineFunction 分析

1. 在 `llvm/include/llvm/CodeGen/...` 加 `MachineFunctionAnalysis` 头文件。
2. 在 `llvm/lib/CodeGen/` 加对应实现（参考 `MachineBlockFrequencyInfo.cpp`）。
3. 在 [`TargetPassConfig.cpp`](TargetPassConfig.cpp) 注入 `getAnalysis<>` 调用。

### 6.5 调试 MIR 问题

- `llc -mtriple=riscv64 -print-after-all` 在每个 Pass 后打印 MIR。
- `llc -mtriple=riscv64 -debug-only=machine-cse` 只打印 MachineCSE 调试信息。
- `llc -mtriple=riscv64 -verify-machineinstrs` 启用 MIR 校验。
- 写 `.mir` 测试用 `llc -run-pass=my-pass -input=mir %s`。

---

## §7. NT 注释索引

当前 `llvm/lib/CodeGen/` 下尚无 `// <NT>` 注释。可结合以下 skill 使用：

- `nt-comments` — 给单文件加 NT 中文注释。
- 本 overview 文档的姊妹篇：[`llvm/lib/Target/RISCV/0-overview.md`](../Target/RISCV/0-overview.md) 演示了
  对一个 target 后端的完整导读写法（含 NT 注释落地）。

添加 NT 注释时按"少而精"原则：每文件 5-10 段，对应功能复杂度。例如
`SelectionDAGISel.cpp` 这类大文件可加 8 段；`MachineLICM.cpp` 这类典型
Pass 加 3-5 段。