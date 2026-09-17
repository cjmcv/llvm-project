<!-- <NT>overview:llvm/lib/Target/RISCV/ -->

# LLVM RISC-V 后端导读 — `llvm/lib/Target/RISCV/`

> 本文档梳理 `llvm/lib/Target/RISCV/` 目录下所有文件的职责、上下游与推荐阅读顺序，
> 目标读者：想深入理解 RISC-V 后端 / 准备给 RISC-V 添加自定义扩展的 LLVM 开发者。
>
> 所有路径相对 `llvm/lib/Target/RISCV/`。`llvm/include/llvm/Target/RISCV/` 与
> `llvm/test/CodeGen/RISCV/` 不在本导读范围。

---

## §0. 后端在 LLVM 编译流水线中的位置

RISC-V 后端（`RISCVCodeGen` 组件）介入的是 **LLVM IR → 机器码** 阶段，
包含两套指令选择（SelectionDAG / GlobalISel）、若干 MachineFunctionPass
优化、以及最终落地为汇编/MC 指令的 AsmPrinter 层。其上游是通用的 IR 层
优化（InstCombine / LoopVectorize 等，由 `RISCVTargetTransformInfo` 提供
成本估算），下游是 LLVM MC 层（MCTargetDesc + AsmParser + Disassembler
+ 汇编输出），把 MCInst 编码为 ELF/Mach-O 字节流。

后端启动入口是 `RISCVTargetMachine::RISCVTargetMachine`，
由 LLVM 的 Target 注册机制（`LLVMInitializeRISCVTargetMC` 等）注入。
任何 RISC-V 工具（`llc`、`clang -cc1 --target=riscv64`、
`llvm-mc -triple=riscv64`、`llvm-objdump -d`）启动时都会调到本后端的
某个初始化函数，从而把分散在 9 个子目录下的实现串接起来。

---

## §1. 编译流水线概览

一个典型的 `clang -c -target riscv64 file.c → file.o` 调用，按时间顺序：

```
┌──────────────────────────┐     ┌──────────────────────────────┐
│ Frontend (clang)         │     │ LLVM IR (Module / Function)  │
└──────────────────────────┘     └──────────────────────────────┘
                                                │
              ┌─────────────────────────────────┘
              ▼
┌────────────────────────────────────────────────────────┐
│ RISCVTargetTransformInfo (IR 层优化成本估算)            │
│   · LoopVectorize / SLP / unroll 等根据 RVV/位宽决策    │
└────────────────────────────────────────────────────────┘
              │
              ▼
┌────────────────────────────────────────────────────────┐
│ RISCVCodeGenPrepare (IR → IR 的 RISC-V 特化改写)        │
│   · 地址计算 / shuffle / splat / 整型→浮点 调整          │
└────────────────────────────────────────────────────────┘
              │
              ▼
┌──────────────────────────────────────────────────────────┐
│ 指令选择 (二选一):                                        │
│   ┌─ SelectionDAG: RISCVISelLowering + RISCVISelDAGToDAG │
│   └─ GlobalISel:     GISel/* 8 个文件                    │
└──────────────────────────────────────────────────────────┘
              │
              ▼  MachineFunction
┌──────────────────────────────────────────────────────────┐
│ Pre-RA MachineFunctionPass (RISCVCodeGenPassBuilder 编排)│
│   RISCVLoadStoreOptimizer                                │
│   RISCVMachineScheduler  (RISCVVectorMaskDAGMutation)    │
│   RISCVInsertVSETVLI  (RVV: 插入 vsetvli)                │
│   RISCVInsertWriteVXRM (定点舍入模式)                     │
│   RISCVLateBranchOpt                                     │
│   RISCVPushPopOptimizer  (Zcmp/Zcmt)                     │
│   RISCVGatherScatterLowering  (IR→MF 阶段)               │
└──────────────────────────────────────────────────────────┘
              │
              ▼
┌────────────────────────────────────────────────────────┐
│ 寄存器分配 (LLVM 通用 RegAlloc)                          │
└────────────────────────────────────────────────────────┘
              │
              ▼
┌────────────────────────────────────────────────────────┐
│ Post-RA / PreEmit MachineFunctionPass                   │
│   RISCVExpandPseudo{PreRA,PostRA,PreEmit,Atomics}        │
│   RISCVMakeCompressible  (RVC 压缩)                     │
│   RISCVMergeBaseOffset / RISCVFoldMemOffset              │
│   RISCVMoveMerger / RISCVRedundantCopyElimination        │
│   RISCVDeadRegisterDefinitions                          │
│   RISCVVectorPeephole / RISCVVMV0Elimination             │
│   RISCVVLOptimizer / RISCVZacasABIFix / RISCVZilsdOptimizer│
│   RISCVIndirectBranchTracking / RISCVLandingPadSetup     │
│   RISCVQCRelaxMarking                                    │
└────────────────────────────────────────────────────────┘
              │
              ▼  MachineInstr + MCInst
┌────────────────────────────────────────────────────────┐
│ RISCVAsmPrinter  (MF → MCStreamer 上的 MC 指令)          │
│   · .option arch / .attribute / STACKMAP / CFI          │
│   · 调用 RISCVInstPrinter 打印助记符                      │
│   · 调用 RISCVTargetStreamer 维护 .riscv.attributes 段   │
└────────────────────────────────────────────────────────┘
              │
              ▼  MCStreamer
┌────────────────────────────────────────────────────────┐
│ MCTargetDesc 层 (汇编 / 编码 / 文件输出)                 │
│   RISCVMCCodeEmitter     MCInst → 字节                   │
│   RISCVAsmBackend        fixup + relax                   │
│   RISCVELFStreamer       汇编流层 (ELF)                  │
│   RISCVTargetELFStreamer .option / .attribute 状态机     │
│   RISCVELFObjectWriter   ELF header / e_flags / 重定位   │
└────────────────────────────────────────────────────────┘
              │
              ▼  ELF 字节流
┌────────────────────────────────────────────────────────┐
│ ld.lld / GNU ld 链接                                    │
└────────────────────────────────────────────────────────┘
```

汇编路径（`llvm-mc -triple=riscv64 -filetype=obj file.s → file.o`）跳过
整个 MachineFunctionPass 流程，直接走

```
RISCVAsmParser → RISCVELFStreamer → RISCVMCCodeEmitter → RISCVAsmBackend → RISCVELFObjectWriter
```

反汇编路径（`llvm-objdump -d file.o`）走

```
RISCVDisassembler → RISCVInstPrinter
```

---

## §2. 文件目录结构

```
llvm/lib/Target/RISCV/
├── CMakeLists.txt              ← td 生成器清单 + CodeGen 组件编译入口
├── [`RISCV.h`](RISCV.h) / [`RISCV.td`](RISCV.td)          ← 后端总入口 (Pass 声明 + 顶层 td include)
│
├── 顶层 .cpp/.h                ← 9 个核心类 (AsmPrinter / ISelLowering / Subtarget / ...)
├── 顶层 Pass .cpp              ← 约 30 个 MachineFunctionPass
├── 顶层 .td                    ← 约 60 个 TableGen 文件 (指令/寄存器/调度/特性)
│
├── AsmParser/      RISCVAsmParser.cpp        汇编解析
├── Disassembler/   RISCVDisassembler.cpp     反汇编
├── GISel/          8 个 .cpp/.h + 1 .td      GlobalISel 指令选择
├── MCA/            RISCVCustomBehaviour.cpp  llvm-mca RVV 模拟
├── MCTargetDesc/   13 个 .cpp + 9 个 .h      MC 层 (编码/输出/relax/factory)
└── TargetInfo/     RISCVTargetInfo.cpp       Target 注册入口
```

### 2.1 TableGen 生成器清单（来自 `CMakeLists.txt`）

`RISCV.td` 一份输入生成以下 15 个 `.inc` 文件，每个被对应 .cpp 包含：

| 生成文件                          | 用途                                  |
|----------------------------------|---------------------------------------|
| `RISCVGenAsmMatcher.inc`         | `RISCVAsmParser::MatchInstructionImpl`|
| `RISCVGenAsmWriter.inc`          | `RISCVInstPrinter::printInstruction`  |
| `RISCVGenCompressInstEmitter.inc` | `RISCVAsmBackend` 的 RVC 压缩展开     |
| `RISCVGenMacroFusion.inc`        | `RISCVInstrInfo::isMacroFusible`      |
| `RISCVGenDAGISel.inc`            | `RISCVISelDAGToDAG` 模式选择          |
| `RISCVGenDisassemblerTables.inc` | `RISCVDisassembler::getInstruction`   |
| `RISCVGenInstrInfo.inc`          | MCInstrInfo / 指令枚举                |
| `RISCVGenMCCodeEmitter.inc`      | `RISCVMCCodeEmitter::getMachineOpValue`|
| `RISCVGenMCPseudoLowering.inc`   | 伪指令 lowering 表                    |
| `RISCVGenRegisterBank.inc`       | GISel 的 RegBank 选择                 |
| `RISCVGenRegisterInfo.inc`       | `RISCVRegisterInfo`                   |
| `RISCVGenSearchableTables.inc`   | 调优信息表 + Specifier 反查           |
| `RISCVGenSubtargetInfo.inc`      | `RISCVSubtarget` 字段                 |
| `RISCVGenExegesis.inc`           | llvm-exegesis 的指令描述              |
| `RISCVGenSDNodeInfo.inc`         | SDNode 反查表                         |

[``RISCVGISel.td``](RISCVGISel.td) 额外生成 5 个 GISel 相关的 `.inc`，被 `GISel/*.cpp` 包含。

---

## §3. 文件详解

### 3.1 顶层核心类（9 个）

#### RISCV.h
- **作用**：后端总入口，声明所有 RISC-V Pass 类（RISCVCodeGenPreparePass / RISCVISelDAGToDAGPass / RISCVGatherScatterLoweringPass / RISCVVectorPeepholePass / RISCVOptWInstrsPass / RISCVFoldMemOffsetPass / RISCVExpandPseudo\*Pass / RISCVZacasABIFixPass / RISCVVLOptimizerPass 等）的旧式 `create*LegacyPass` 工厂 + `initialize*Pass` 注册函数。
- **上游**：由 `RISCVTargetMachine::createPassConfig` 与 `RISCVCodeGenPassBuilder` 调用；CMake 通过 `RISCVPassRegistry.def` 自动生成 `initializeRISCVTarget` 中的注册语句。
- **下游**：被本目录下所有 `RISCV*Pass.cpp` 实现。
- **关键函数**：`initializeRISCVTarget`（不在此文件，由 [``RISCVTargetMachine.cpp``](RISCVTargetMachine.cpp) 实现）。

#### [`RISCVAsmPrinter.cpp`](RISCVAsmPrinter.cpp) / .h
- **作用**：把 MachineFunction 降级到 MCStreamer 的 MC 指令。处理 `.option arch/.option pic`、`.riscv.attributes` 段、栈图/PatchPoint/StatePoint、CFI、`.note.gnu.property`、NTL hint、`lpADDI`、函数入口对齐等 RISC-V 专属输出。
- **上游**：`AsmPrinter` 框架（`llvm/lib/CodeGen/AsmPrinter/`）调用 `RISCVAsmPrinter::emitInstruction` 等回调。
- **下游**：调用 `RISCVTargetStreamer`（维护 .option 状态）、`RISCVMCExpr`（解析 `%lo`/`%hi` 等）、`RISCVInstPrinter`（打印助记符）。
- **关键类/函数**：`RISCVAsmPrinter::emitInstruction`、`emitStartOfAsmFile`、`emitEndOfAsmFile`、`PrintAsmOperand`、`PrintAsmMemoryOperand`、`LowerSTACKMAP/PATCHPOINT/STATEPOINT`、`emitLpadAlignedCall`、`emitNTLHint`、`emitFunctionEntryLabel`。

#### [`RISCVISelLowering.cpp`](RISCVISelLowering.cpp) / .h
- **作用**：最大最复杂的文件，包含 `RISCVTargetLowering`、合法化/降级规则、内建函数（intrinsic）映射、向量化分段/掩码 lowering、调用约定参数处理、SIMD/向量 split-fold-merge 优化、向量元素提取、MaskDAG Mutation。
- **上游**：`SelectionDAGBuilder`、`GlobalISel`（GISel 间接）调用；其他 RISC-V Pass 调用 `getRegisterType`、钩子函数。
- **下游**：`RISCVISelDAGToDAG`、GISel 的 `RISCVCallLowering` / `RISCVInstructionSelector`、`VectorPeephole`、`InsertVSETVLI`。
- **关键类/函数**：`RISCVTargetLowering`、`RISCVISD::*` 节点枚举、`RISCVVIntrinsicsTable`、`getRegisterType` / `getCopyToPartsVector`、`LowerFormalArguments`、`LowerCall`、`LowerReturn`、`ReplaceNodeResults`、`PerformDAGCombine`、`isLegalAddressingMode`。

#### [`RISCVSubtarget.cpp`](RISCVSubtarget.cpp) / .h
- **作用**：描述一个具体 RISC-V 子目标（CPU + 启用的扩展）。对 SubtargetFeature 解析、调度模型选择、调优参数（TuneInfo）、栈布局特性、VLEN/SLEN 推导等进行集中封装。
- **上游**：所有 RISC-V Pass 通过 `MF.getSubtarget<RISCVSubtarget>()` 访问。
- **下游**：被 RegisterInfo / InstrInfo / FrameLowering / InsertVSETVLI / VLOptimizer 等大量 Pass 引用。
- **关键类/函数**：`RISCVSubtarget::initializeProperties`、`initializeSubtargetDependencies`、`getFeatureInfo`、`getTargetLowering`、`getRegisterInfo`、`getInstrInfo`、`RISCVTuneInfo`、`is64Bit`、`hasStdExtV`、`enableMachineScheduler`。

#### RISCVTargetMachine.cpp / .h
- **作用**：创建并管理 `RISCVSubtarget`、Pass Builder、目标特定选项（ABI、relax、save-restore、vector-bits 等）。对外提供 LLVMTargetMachine 接口。
- **上游**：`TargetMachine` 框架、`llc`/`clang`/`lld` driver 调用。
- **下游**：返回所有 `RISCV*` 子对象、`RISCVCodeGenPassBuilder`。
- **关键类/函数**：`RISCVTargetMachine::RISCVTargetMachine`、`createPassConfig`、`getSubtargetImpl`、`getTargetTransformInfo`、`buildCodeGenPipeline`、`registerPassBuilderCallbacks`。

#### [`RISCVFrameLowering.cpp`](RISCVFrameLowering.cpp) / .h
- **作用**：实现 RISC-V 调用约定的栈帧布局（epilogue/prologue/save-restore、Zcmp push/pop）。
- **上游**：`PrologEpilogInserter`、`TargetFrameLowering` 框架。
- **下游**：调用 `RISCVRegisterInfo::eliminateFrameIndex`、MC Inst Printer。
- **关键类/函数**：`RISCVFrameLowering::emitPrologue / emitEpilogue`、`hasFP`、`getFrameIndexReference`、`assignCalleeSavedSpillSlots`、`spillCalleeSavedRegisters`、`restoreCalleeSavedRegisters`、PBO（Push/Pop）相关。

#### [`RISCVRegisterInfo.cpp`](RISCVRegisterInfo.cpp) / .h
- **作用**：描述寄存器集合，实现 ABI 的 caller/callee-saved 划分、`eliminateFrameIndex`、reserved register 列表、stack-slotable 跨类寄存器对。
- **上游**：被 `RISCVFrameLowering`、`RISCVInstrInfo`、`RISCVISelLowering`、`InsertVSETVLI` 等调用。
- **下游**：`MCRegisterInfo`、RegisterBankInfo。
- **关键类/函数**：`RISCVRegisterInfo`、`RISCVRI` 命名空间（寄存器类 enum）、`getCalleeSavedRegs`、`getReservedRegs`、`eliminateFrameIndex`、`needsFrameBaseReg`。

#### [`RISCVInstrInfo.cpp`](RISCVInstrInfo.cpp) / .h
- **作用**：实现指令级别的查询/转换：复制展开、load/store 折叠/合并、分析拆装、`analyzeBranch` / `insertBranch` / `removeBranch`、拷贝折叠、压缩指令、栈调整折叠。
- **上游**：CodeGen 各 Pass（MISched、RA、LiveVars、BranchFolding）。
- **下游**：`RISCVRegisterInfo`、LoadStoreOpt、LoadStoreOptimizer、MCCodeEmitter。
- **关键类/函数**：`RISCVInstrInfo`、`RISCVCC::CondCode`、`RISCVVPseudosTable`、`RISCVMaskedPseudoInfo`、`areLoadsFromSameBasePtr`、`mergePairedLoadStore`、`copyPhysReg`、`storeRegToStackSlot`、`loadRegFromStackSlot`、`analyzeBranch`、`isCompressible`、`getShortMCForm`、`getInstSizeInBytes`。

#### [`RISCVCallingConv.cpp`](RISCVCallingConv.cpp) / .h
- **作用**：RISC-V 调用约定（`.ll` 的 `ccc …`）参数/返回寄存器分配，含 ILP32/LP64、硬浮点、Zve*/X1* 变体；不依赖 SelectionDAG，作用于 Machine IR 之前。
- **上游**：`SelectionDAGISel` 阶段、`RISCVISelLowering::LowerCall/LowerFormalArguments`。
- **下游**：`RISCVSubtarget::getTargetLowering`、`RISCVRegisterInfo`。
- **关键类/函数**：`CC_RISCV`、`CC_RISCV_V` 等。

#### [`RISCVConstantPoolValue.cpp`](RISCVConstantPoolValue.cpp) / .h
- **作用**：把 BlockAddress / ExternalSymbol / 常量池值打包为 `MachineConstantPoolValue` 子类，以便 `auipc+addi` 等长 PC 相对寻址的常量池加载。
- **上游**：指令选择 / AsmPrinter 使用常量池时。
- **下游**：`RISCVMCExpr`、MCStreamer。

#### [`RISCVSelectionDAGInfo.cpp`](RISCVSelectionDAGInfo.cpp) / .h
- **作用**：为 SelectionDAG 提供 RISC-V 特有的 memcpy/expand 行为（memcpy/memset lowering、STRICT_* 节点扩展）。
- **上游**：`TargetLowering::getSelectionDAGInfo`。
- **下游**：DAGCombine、EmitNode。
- **关键类/函数**：`RISCVSelectionDAGInfo`、`EmitTargetSpecial`。

#### [`RISCVTargetObjectFile.cpp`](RISCVTargetObjectFile.cpp) / .h
- **作用**：ELF/Mach-O 下 RISC-V 段布局（`.text`、`.rodata`、`tdata/tbss`、TLS、绝对寻址相关段）。
- **上游**：`TargetLoweringObjectFile` 框架。
- **下游**：与 AsmPrinter / MCStreamer 协作。
- **关键类/函数**：`RISCVELFTargetObjectFile`、`RISCVMachOTargetObjectFile`、`getTTypeEncoding`、`Initialize` 重载。

#### [`RISCVMachineFunctionInfo.cpp`](RISCVMachineFunctionInfo.cpp) / .h
- **作用**：为每个 MachineFunction 缓存 RISC-V 特有的数据：是否使用 Zcmp push/pop、是否 RVC、是否用 VL 段、是否使用 NTL hint、island 标记等。
- **上游**：被各 RISC-V Pass 读写。
- **下游**：与 Subtarget 信息交互；可被 AsmPrinter 读取以决定 emit 行为。
- **关键类/函数**：`RISCVMachineFunctionInfo`、同名 `yaml::RISCVMachineFunctionInfo`（用于 `mir` 序列化）。

#### [`RISCVTargetTransformInfo.cpp`](RISCVTargetTransformInfo.cpp) / .h
- **作用**：RISC-V 特有的 TTI：循环向量化的成本估算、内建函数成本、interleave、peel/unroll 决策、SVE/RVV 特殊判断。
- **上游**：LLVM IR 层 LoopVectorize、IR 优化 Pass 调用。
- **下游**：与 `RISCVSubtarget`、`RISCVISelLowering` 信息共享。
- **关键类/函数**：`RISCVTTIImpl`、`getVectorInstrCost`、`getMemcpyCost`、`isHardwareLoopProfitable`、`preferVectorization`、`getInterleavedMemoryOpCost`。

---

### 3.2 顶层 TableGen 文件（约 60 个）

#### 主框架 .td
- **RISCV.td**：顶层 td 入口，include 所有其他 .td；定义子目标公共基类 `ProcFamily`。
- **[`RISCVInstrFormats.td`](RISCVInstrFormats.td)**：所有基础指令的多类格式（`RISCVI`、`RISCVBaseHI/HSI…`）。
- **[`RISCVInstrFormatsC.td`](RISCVInstrFormatsC.td)**：压缩指令格式 (`RVInst16…`)。
- **[`RISCVInstrFormatsV.td`](RISCVInstrFormatsV.td)**：RVV（向量）指令格式 (`RISCVIUVX…`、`RISCVMV_*`)。
- **[`RISCVInstrFormatsXAIF.td`](RISCVInstrFormatsXAIF.td)**：XAIF 自定义扩展指令格式。
- **[`RISCVInstrFormatsSpacemitV.td`](RISCVInstrFormatsSpacemitV.td)**：Spacemit 自定义向量格式。
- **[`RISCVInstrPredicates.td`](RISCVInstrPredicates.td)**：指令级 Predicate（用于 `Predicate` 字段）。
- **[`RISCVCombine.td`](RISCVCombine.td)**：DAGCombine 模式片段（小文件）。
- **[`RISCVSystemOperands.td`](RISCVSystemOperands.td)**：M-mode 系统寄存器/CSR 与 scause/stval/priv 编码等。

#### 寄存器 / 调用约定
- **[`RISCVRegisterInfo.td`](RISCVRegisterInfo.td)**：寄存器类（`GPR`、`FPR*`、`VR`、`VSR*`）、`RegClass`、`Register`、alias、`DwarfRegNum` 等。
- **[`RISCVCallingConv.td`](RISCVCallingConv.td)**：RISC-V 调用约定的 DAG ISel 与 GISel 版 CC 描述。

#### 子目标 / CPU / 调度
- **[`RISCVFeatures.td`](RISCVFeatures.td)**：所有子目标扩展 Feature（`HasStdExtA/B/C…V/X…`）。
- **[`RISCVProcessors.td`](RISCVProcessors.td)**：CPU 定义（generic、rocket、sifive-u7/u74/p400/p500/p600/p800、syntacore、mips-p8700、spacemit-x60/x100、tt-ascalon-x、xiangshan-kunminghu/nanhu、andes45 等）。
- **[`RISCVProfiles.td`](RISCVProfiles.td)**：RISC-V 配置文件（RVA20/RVA22/RVA23 等）。

#### GISel
- **[`RISCVInstrGISel.td`](RISCVInstrGISel.td) / RISCVGISel.td**：把指令映射到 GISel 操作码的描述，包含 `RISCVGenericInstruction`、RegClass、Bank。

#### 调度模型
- **[`RISCVSchedule.td`](RISCVSchedule.td)**：调度模型入口，include `[`RISCVScheduleZb.td`](RISCVScheduleZb.td) / V.td / XSf.td / Zvk.td`。
- **[`RISCVScheduleV.td`](RISCVScheduleV.td)**：RVV 调度（buffer、pipeline delay、issue）。
- **RISCVScheduleZb.td / Zvk.td / XSf.td**：Zb / Zvk / XSf 调度。
- **[`RISCVSchedGenericOOO.td`](RISCVSchedGenericOOO.td) / [`RISCVSchedRocket.td`](RISCVSchedRocket.td) / [`RISCVSchedAndes45.td`](RISCVSchedAndes45.td) / RISCVSchedSiFive7/8/P400/P500/P600/P800.td / [`RISCVSchedMIPSP8700.td`](RISCVSchedMIPSP8700.td) / [`RISCVSchedSpacemitX60.td`](RISCVSchedSpacemitX60.td) / [`RISCVSchedSpacemitX100.td`](RISCVSchedSpacemitX100.td) / [`RISCVSchedSyntacoreSCR1.td`](RISCVSchedSyntacoreSCR1.td) / [`RISCVSchedSyntacoreSCR345.td`](RISCVSchedSyntacoreSCR345.td) / [`RISCVSchedSyntacoreSCR7.td`](RISCVSchedSyntacoreSCR7.td) / [`RISCVSchedTTAscalonX.td`](RISCVSchedTTAscalonX.td) / [`RISCVSchedXiangShanKunMingHu.td`](RISCVSchedXiangShanKunMingHu.td) / [`RISCVSchedXiangShanNanHu.td`](RISCVSchedXiangShanNanHu.td)**：各家处理器调度模型。

#### 杂项
- **[`RISCVMacroFusion.td`](RISCVMacroFusion.td) / [`RISCVMacroFusionXQCI.td`](RISCVMacroFusionXQCI.td)**：定义宏融合对（如 `addi+branch`）。
- **[`RISCVPfmCounters.td`](RISCVPfmCounters.td)**：perf 事件描述。
- **RISCVPassRegistry.def**：与 `RISCV.h` 配合，列出所有需要注册的 Pass 名。
- **[`RISCVInstrInfo.td`](RISCVInstrInfo.td)**：主入口，include 所有 ISA/扩展的 InstrInfo。
- **[`RISCVInstrInfoA.td`](RISCVInstrInfoA.td)**：A 扩展（原子）。
- **[`RISCVInstrInfoC.td`](RISCVInstrInfoC.td)**：C 扩展（压缩指令）。
- **[`RISCVInstrInfoD.td`](RISCVInstrInfoD.td)**：D 扩展（双精度 FP）。
- **[`RISCVInstrInfoF.td`](RISCVInstrInfoF.td)**：F 扩展（单精度 FP）。
- **[`RISCVInstrInfoM.td`](RISCVInstrInfoM.td)**：M 扩展（乘除）。
- **[`RISCVInstrInfoP.td`](RISCVInstrInfoP.td)**：DSP/Simd P 扩展。
- **[`RISCVInstrInfoQ.td`](RISCVInstrInfoQ.td)**：Q 扩展（四精度 FP）。
- **[`RISCVInstrInfoV.td`](RISCVInstrInfoV.td)**：V 扩展（RVV）整数/FP 向量指令。
- **[`RISCVInstrInfoVPseudos.td`](RISCVInstrInfoVPseudos.td)**：V 扩展伪指令。
- **[`RISCVInstrInfoVSDPatterns.td`](RISCVInstrInfoVSDPatterns.td) / [`RISCVInstrInfoVVLPatterns.td`](RISCVInstrInfoVVLPatterns.td)**：V 扩展 SDNode 模式（VL/无 VL 版本，最大文件之一）。
- **[`RISCVInstrInfoSFB.td`](RISCVInstrInfoSFB.td)**：SFB（SiFive Bitmanip）伪指令。
- **[`RISCVInstrInfoSmcsps.td`](RISCVInstrInfoSmcsps.td) / Smip.td**：Smcsps/Smip 子扩展。
- **[`RISCVInstrInfoXAIF.td`](RISCVInstrInfoXAIF.td)**：XAIF 自定义扩展指令集。
- **[`RISCVInstrInfoXAndes.td`](RISCVInstrInfoXAndes.td) / XCV.td / XMips.td / XSpacemiT.td / XTHead.td**：Andes45 / Core-V / MIPS / Spacemi / TH 厂商扩展。
- **[`RISCVInstrInfoXSf.td`](RISCVInstrInfoXSf.td)**：SiFive 厂商扩展。
- **[`RISCVInstrInfoXSfmm.td`](RISCVInstrInfoXSfmm.td)**：SiFive matrix multiply。
- **[`RISCVInstrInfoXqccmp.td`](RISCVInstrInfoXqccmp.td) / Xqccmt.td / Xqci.td / Xwch.td / Y.td**：QCC 各组 + WCH + Y。
- **[`RISCVInstrInfoZa.td`](RISCVInstrInfoZa.td) / Zalasr.td / Zb.td / Zc.td / Zclsd.td / Zcmop.td / Zfa.td / Zfbfmin.td / Zfh.td / Zibi.td / Zicbo.td / Zicfiss.td / Zicond.td / Zilsd.td / Zilx.td / Zimop.td / Zk.td**：Z* 各子扩展。
- **[`RISCVInstrInfoZvabd.td`](RISCVInstrInfoZvabd.td) / Zvbdota.td / Zvdot4a8i.td / Zvdota.td / Zvfbf.td / Zvfofp8min.td / Zvk.td / Zvvm.td / Zvzip.td**：向量 Zv* 子扩展。

---

### 3.3 顶层 MachineFunctionPass（约 30 个，按执行阶段排序）

#### Pass Builder
- **[`RISCVCodeGenPassBuilder.cpp`](RISCVCodeGenPassBuilder.cpp)**：实现 `RISCVCodeGenPassBuilder`，把 RISC-V 专属 Pass 插入到 codegen pipeline，决定调度、pre-RA、post-RA 各阶段插入哪些 RISC-V 优化。
  - **关键类/函数**：`RISCVCodeGenPassBuilder::addInstSelector / addPreSched / addPreEmit / addPostRegAlloc`。

#### IR 层
- **[`RISCVCodeGenPrepare.cpp`](RISCVCodeGenPrepare.cpp)**：IR 层 prepare：改写地址计算、shuffle、整型→浮点、splat、recurrence 等，便于后续指令选择/向量化。
- **[`RISCVInsertReadWriteCSR.cpp`](RISCVInsertReadWriteCSR.cpp)**：在 IR 层把伪读/写 CSR 节点扩展为真 CSR 指令（用于 mret/sret/wfi 等）。
- **[`RISCVGatherScatterLowering.cpp`](RISCVGatherScatterLowering.cpp)**：IR Pass，把连续的 `getelementptr + load/store` 模式降级为 RVV `vluxei/vloxei/vsuxei/vsoxei` 索引访存。
- **[`RISCVPromoteConstant.cpp`](RISCVPromoteConstant.cpp)**：Module 级 IR Pass：把 `MaterializeConstant` / float 立即数 promote 到符号常量，便于 RVC/lui 复用。

#### Pre-RA
- **[`RISCVLoadStoreOptimizer.cpp`](RISCVLoadStoreOptimizer.cpp)**：核心 load/store 优化器：合并相邻相同寄存器的 ld/st、把 ld/lui+addi 折叠等。
  - **关键类/函数**：`RISCVLoadStoreOpt::runOnMachineFunction`、`findMatchingOffsetAndOpcode`、`foldAsLoad/store`。
- **[`RISCVMachineScheduler.cpp`](RISCVMachineScheduler.cpp) / .h**：Pre-RA 调度策略（`RISCVPreRAMachineSchedStrategy`），按 RVV / SiFive 等调优调度顺序。
- **[`RISCVVectorMaskDAGMutation.cpp`](RISCVVectorMaskDAGMutation.cpp)**：调度前的 `ScheduleDAGMutation`，在 MachineSched 阶段为 RVV mask 操作添加数据依赖以避免非法重排。
- **[`RISCVInsertVSETVLI.cpp`](RISCVInsertVSETVLI.cpp)**：在 RVV 代码中插入/优化 `vsetvli` 配置指令；维护块级 AVL/SEW/LMUL/TA 等状态。
  - **关键类/函数**：`RISCVInsertVSETVLI::runOnMachineFunction`、`BlockData`、`insertVSETVLI`。
  - **依赖**：`RISCVVSETVLIInfoAnalysis`。
- **[`RISCVInsertWriteVXRM.cpp`](RISCVInsertWriteVXRM.cpp)**：管理 FP 舍入模式寄存器 `vxrm`：插入 `csrwi` 来切换定点舍入模式，类似 VSETVLI。
- **[`RISCVLateBranchOpt.cpp`](RISCVLateBranchOpt.cpp)**：迟阶段 branch 优化，合并相近的跳转/调换次序。
- **[`RISCVPushPopOptimizer.cpp`](RISCVPushPopOptimizer.cpp)**：Zcmp/Zcmt：把 `sd/lw` 序列折叠为 `push/pop`。
- **[`RISCVInterleavedAccess.cpp`](RISCVInterleavedAccess.cpp)**：识别交错访存 `ld4/st4` 等内建函数，使用 RVV `vlseg/vsseg` 指令展开。
- **[`RISCVVLOptimizer.cpp`](RISCVVLOptimizer.cpp)**：分析并缩短 `vl`/`vset` 指令的 EMASK/AVL（基于 demand-driven），消除 SEW/LMUL 切换冗余。

#### Post-RA / PreEmit
- **[`RISCVExpandPseudoBase.cpp`](RISCVExpandPseudoBase.cpp) / .h**：公用基类 `RISCVExpandPseudoImplBase`。
- **[`RISCVExpandPseudoPreRA.cpp`](RISCVExpandPseudoPreRA.cpp)**：Pre-RA 阶段展开伪指令。
- **[`RISCVExpandPseudoPostRA.cpp`](RISCVExpandPseudoPostRA.cpp)**：RA 后/PreEmit 前再次展开剩余伪指令。
- **[`RISCVExpandPseudoPreEmit.cpp`](RISCVExpandPseudoPreEmit.cpp)**：PreEmit 阶段最后清理伪指令，处理 `la`、sp 调整、frame-relative 寻址等。
- **[`RISCVExpandPseudoAtomics.cpp`](RISCVExpandPseudoAtomics.cpp)**：把 codegen 产生的 atomic-load/atomic-store pseudo 展开为真指令序列。
- **[`RISCVMakeCompressible.cpp`](RISCVMakeCompressible.cpp)**：把 MI 转成 RVC（压缩）能表达的等价形式（pre-RA 后）。
- **[`RISCVMergeBaseOffset.cpp`](RISCVMergeBaseOffset.cpp)**：合并 `addi + ld/st with offset` 为 `ld/st with combined offset`。
- **[`RISCVFoldMemOffset.cpp`](RISCVFoldMemOffset.cpp)**：把 `ld/st offset; addi reg, reg, off` 折叠为 `ld/st off+off`，减少指令。
- **[`RISCVMoveMerger.cpp`](RISCVMoveMerger.cpp)**：合并相邻 `mv`（`addi rd, rs, 0`）序列、消除冗余 mv。
- **[`RISCVRedundantCopyElimination.cpp`](RISCVRedundantCopyElimination.cpp)**：消除冗余 `copy`/`addi 0` 等。
- **[`RISCVDeadRegisterDefinitions.cpp`](RISCVDeadRegisterDefinitions.cpp)**：消除 `def X — ; … — X = …` 死定义。
- **[`RISCVVectorPeephole.cpp`](RISCVVectorPeephole.cpp)**：Post-RA 阶段 RVV 窥孔优化。
- **[`RISCVVMV0Elimination.cpp`](RISCVVMV0Elimination.cpp)**：消除 `vmv.v.x v0, x0` 等零向量 move。
- **[`RISCVZacasABIFix.cpp`](RISCVZacasABIFix.cpp)**：Zacas/ZaCmte/ZaCmp 调用前后 ABI 修复，确保 `a0`/`a1` 状态正确。
- **[`RISCVZilsdOptimizer.cpp`](RISCVZilsdOptimizer.cpp)**：Zilsd 扩展前的优化 pass —— 把高位 `lui+ld/st` 合并为短立即形式。
- **[`RISCVIndirectBranchTracking.cpp`](RISCVIndirectBranchTracking.cpp)**：实现 BTI（间接跳转跟踪）：在所有间接跳转处插入 `lpad`/`BTI`。
- **[`RISCVLandingPadSetup.cpp`](RISCVLandingPadSetup.cpp)**：在调用前/对齐位置插入 `lpADDI`（landpad）指令（与 BTI/lpADDI 紧密相关）。
- **[`RISCVQCRelaxMarking.cpp`](RISCVQCRelaxMarking.cpp)**：汇编器/反汇编层 QC 扩展的 RVC relax 标记（`qc.c.mv` 等）。
- **[`RISCVOptWInstrs.cpp`](RISCVOptWInstrs.cpp)**：把 RV32 上的 32 位指令（`lw`/`addiw`/`flw`）融合/转换为 RV64 优化形式（`ld`/`addiw`/`fld`），当目标是 RV64 时避免无谓 sign-extend。

#### 指令选择（SelectionDAG 路径）
- **[`RISCVISelDAGToDAG.cpp`](RISCVISelDAGToDAG.cpp) / .h**：DAG-to-DAG 指令选择：RV32/RV64/向量/厂商扩展指令的 pattern 与特化选择。
  - **关键类/函数**：`RISCVDAGToDAGISel::Select`、`RISCVDAGToDAGISelLegacy`、`SelectAddrMode`、`SelectAddrFrameIndex`、`Select` 重载。

---

### 3.4 AsmParser/

#### RISCVAsmParser.cpp
- **作用**：唯一文件。实现 `RISCVAsmParser : MCTargetAsmParser`：解析 RISC-V 汇编（指令、寄存器、CSR、VMask、VType/SMtvType、`%hi/%lo/%pcrel_hi/%got` 等 MCExpr、浮点立即数、`push/pop` 寄存器列表、`cm.jt/cm.jalt` table、压缩指令隐式展开）。
- **上游**：`MCTargetAsmParser` 框架、driver（`llvm-mc` / clang 集成汇编）。
- **下游**：`MCTargetStreamer`、`RISCVMCExpr`、`RISCVInstPrinter`、常量表（来自 .td）。
- **关键类/函数**：`RISCVAsmParser`、`ParserOptionsSet`、`NearMissMessage`、`RegOp/ExprOp/FPImmOp/SysRegOp/VTypeOp/SMTVTypeOp/FRMOp/FenceOp/RegListOp/StackAdjOp/RegRegOp`、`MatchInstructionImpl`、`MatchImm`、`MatchFPReg`、NearMiss 建议表。

---

### 3.5 Disassembler/

#### RISCVDisassembler.cpp
- **作用**：唯一文件。实现 `RISCVDisassembler : MCDisassembler`：从字节解码为 MCInst，按 TableGen 生成的 decoder 表处理 RV32/RV64/RVC/RVV/X* 各子集。
- **上游**：`llvm-objdump`、`gdb`、`llvm-mc -d`。
- **下游**：`RISCVSubtarget`、`RISCVInstPrinter`、`RISCVBaseInfo`。
- **关键类/函数**：`RISCVDisassembler::getInstruction`、`decodeInstruction`、`DecoderListEntry`、`decodeToMCInst`、按 opcode 查 `DecoderTable` 的 `getFeatureBits`、`Decode64<...>` 系列。

---

### 3.6 GISel/

- **RISCVCallLowering.cpp / .h**：GISel 调用约定 lowering（基于 `CallLowering`），处理参数/返回寄存器/栈参数分配。
  - **关键类**：`RISCVCallLowering`、`RISCVOutgoingValueHandler`、`RISCVIncomingValueHandler`、`RISCVFormalArgHandler`、`RISCVCallReturnHandler`、`lowerReturn / lowerCall / lowerFormalArguments`。

- **RISCVInlineAsmLowering.cpp / .h**：GISel 对 inline asm 的降级，`RISCVInlineAsmLowering : InlineAsmLowering`。

- **RISCVInstructionSelector.cpp**：GISel MI → MCInstr 选择器，`RISCVInstructionSelector : InstructionSelector`，包含 `ConstAddrPlan` 等模式匹配表。

- **RISCVLegalizerInfo.cpp / .h**：定义哪些操作可被合法化、如何 lower/expand。`RISCVLegalizerInfo : LegalizerInfo`，含 RVV/LMUL-aware 自定义规则。

- **RISCVO0PreLegalizerCombiner.cpp**：O0 优化级 GISel 的 pre-legalize 合并，体积小、保守。

- **RISCVPostLegalizerCombiner.cpp**：合法化后 MI combine（去冗余、强匹配）。

- **RISCVPreLegalizerCombiner.cpp**：合法化前 MI combine（G_PTR_ADD/COPY 折叠等）。

- **RISCVRegisterBankInfo.cpp / .h**：将 MI 操作数映射到 Register Bank（`GPRB/FPRB/VRB/VSRB`）。`RISCVRegisterBankInfo : RISCVGenRegisterBankInfo`。

- **RISCVRegisterBanks.td**：RegisterBank 描述（`GPRB/FPRB/VRB/VSRB`）。

---

### 3.7 MCA/

#### RISCVCustomBehaviour.cpp / .h
- **作用**：`llvm-mca` 的 RISC-V 自定义行为：模拟 RVV 向量指令（LMUL/SEW 资源消耗、vxrm/seg 等）。
- **关键类/函数**：`RISCVLMULInstrument`、`RISCVSEWInstrument`、`RISCVInstrumentManager`、`VXMemOpInfo`、`getResourceUsage`。

---

### 3.8 MCTargetDesc/

#### RISCVAsmBackend.cpp / .h
- **作用**：`RISCVAsmBackend : MCAsmBackend`，负责 RISC-V 汇编器 relax（`auipc+addi → c.li/c.lui`、`addi → c.addi` 等）、fixup 调整、边界对齐；包含 `DarwinRISCVAsmBackend`（macOS 平台）。
- **关键类/函数**：`RISCVAsmBackend::relaxInstruction`、`mayNeedRelaxation`、`fixupOne`、`getFixupKindInfo`、`writeNopData`。

#### RISCVBaseInfo.cpp / .h
- **作用**：定义指令/寄存器/SysReg 等的命名空间常量集合：`RISCVOp`、`RISCVII`（指令 IMM/format 标志）、`RISCVFenceField`、`RISCVFPRndMode`、`XSMTVTypeMode`、`RISCVVXRndMode`、`RISCVExceptFlags`、`RISCVLoadFPImm`、`RISCVSysReg`、`RISCVInsnOpcode`、`RISCVABI`、`RISCVFeatures`、`RISCVRVC`、`RISCVZC`、`RISCVVInversePseudosTable`、`VLSEGPseudo / VLXSEGPseudo / VSSEGPseudo / VSXSEGPseudo / VLEPseudo / VSEPseudo / VLX_VSXPseudo / NDSVLNPseudo`。
- **作用**：被 .td include 提供底层查表。

#### RISCVELFObjectWriter.cpp
- **作用**：把 MC 写入 ELF 文件，提供 RISC-V 专属 relocation。`RISCVELFObjectWriter : MCELFObjectTargetWriter`，负责设置 e_flags / e_ident 等 RISC-V 专属字段。`getRelocType` 把内部 Fixup 翻译为 R_RISCV_* ELF 重定位。

#### RISCVELFStreamer.cpp / .h
- **作用**：`RISCVELFStreamer : MCELFStreamer`、`RISCVTargetELFStreamer : RISCVTargetStreamer`：处理 ELF `.option arch/.option …`、`.attribute`、relax 标记。是汇编流层 (Streamer) 的 RISC-V 适配器。

#### RISCVFixupKinds.h
- **作用**：枚举所有 RISC-V 的 `Fixup` kind（HI/LO/PCREL_HI 等）。

#### RISCVInstPrinter.cpp / .h
- **作用**：`RISCVInstPrinter : MCInstPrinter`，把 MCInst 打印为人类可读汇编（包含 RVC、CSR、VType 等的特殊输出）。

#### RISCVMCAsmInfo.cpp / .h
- **作用**：`RISCVMCAsmInfo : MCAsmInfoELF`、`RISCVMCAsmInfoDarwin : MCAsmInfoDarwin`：汇编器方言（注释符、section 标签、`.text` 默认 flags、`.comm`/`zero string` 等）。

#### RISCVMCCodeEmitter.cpp
- **作用**：`RISCVMCCodeEmitter : MCCodeEmitter`，把 MCInst 编码为指令字节；按 `RISCV::Encoding_*` 查表 + 短立即/长立即分支 fixup。

#### RISCVMCExpr.cpp / .h
- **作用**：自定义 MCExpr：`%hi/%lo/%pcrel_hi/%got_pcrel_hi/%ie/%le/%tls_ie/%tls_le/%tls_gd/%tpcrel_hi` 等常量池/重定位表达式。

#### RISCVMCObjectFileInfo.cpp / .h
- **作用**：`RISCVMCObjectFileInfo : MCObjectFileInfo`：MC 层目标文件信息（TLS、段顺序、EH info），含 `getTextSectionAlignment`（Zca 时 2 字节对齐）。

#### RISCVMCTargetDesc.cpp / .h
- **作用**：`initializeRISCVTargetMC`、`createRISCVMCAsmInfo`、`createRISCVMCInstrInfo` 等工厂，初始化 Target MC 描述。包含 `RISCVMCInstrAnalysis : MCInstrAnalysis`。**整个 RISC-V 后端对外暴露的总闸口。**

#### RISCVMachObjectWriter.cpp
- **作用**：Mach-O RISC-V 输出 writer，`RISCVMachObjectWriter : MCMachObjectTargetWriter`（macOS / Apple 平台）。

#### RISCVMatInt.cpp / .h
- **作用**：`RISCVMatInt::Inst` + 实现：将 64 位整数最佳拆分为 `lui/addiw/sh…`（RV64 上 RV32 常量加载的 li 伪指令展开）。被 ISel/AsmPrinter 使用。

#### RISCVTargetStreamer.cpp / .h
- **作用**：`RISCVTargetStreamer : MCTargetStreamer`、`RISCVTargetAsmStreamer`（print）、`RISCVOptionArchArg`（`.option arch` 状态）：在 `.s` 文件里打印/记录 `.option arch/.option relax/.attribute` 等；维护 ArchString 栈。

---

### 3.9 TargetInfo/

#### RISCVTargetInfo.cpp / .h
- **作用**：`RISCVTargetInfo`、`initializeRISCVTargetInfo` 的 hook，挂接 `Target` 注册；为 IR 层提供 target-name → 全局 enable 的入口。

---

## §4. 关键调用链

### 4.1 编译（clang → .o）

```
clang (Frontend)
  → LLVM IR
  → RISCVTargetTransformInfo (IR 层优化成本估算)
  → RISCVCodeGenPrepare (IR → IR 的 RISC-V 特化改写)
  → 指令选择（SelectionDAG: RISCVISelLowering + RISCVISelDAGToDAG
              | GlobalISel: GISel/{CallLowering,InstructionSelector,...}）
  → RISCVCodeGenPassBuilder 编排的 MachineFunctionPass 序列
  → RISCVAsmPrinter (MF → MCStreamer 上的 MC 指令)
  → RISCVMCCodeEmitter (MCInst → 字节)
  → RISCVAsmBackend (fixup + relax)
  → RISCVELFStreamer → RISCVTargetELFStreamer (维护 .option / .attribute)
  → RISCVELFObjectWriter (e_flags / 重定位)
  → ELF 字节流
```

### 4.2 汇编（llvm-mc → .o）

```
RISCVAsmParser (解析指令、寄存器、%lo/%hi 等 MCExpr)
  → RISCVELFStreamer (汇编流层)
  → RISCVMCCodeEmitter (编码)
  → RISCVAsmBackend (fixup + relax)
  → RISCVELFObjectWriter (输出 ELF)
```

### 4.3 反汇编（llvm-objdump -d）

```
RISCVDisassembler (字节 → MCInst，按 DecoderTable 分派)
  → RISCVInstPrinter (MCInst → 助记符文本)
```

### 4.4 RVV 配置链路

```
RISCVInsertVSETVLI (MI 级)
  ↔ RISCVVSETVLIInfoAnalysis (分析每个 block 的 AVL/SEW/LMUL/TA 状态)
RISCVVLOptimizer (消除冗余 vset 切换)
RISCVVectorPeephole (Post-RA 优化)
RISCVVectorMaskDAGMutation (调度阶段)
```

---

## §5. 推荐阅读顺序

按"读懂一个 LLVM 后端"的目标，建议按以下顺序：

### 阶段 1：编译流水线全景（1-2 小时）
1. [`RISCV.h`](RISCV.h) — 后端入口声明。
2. [`RISCV.td`](RISCV.td) — 顶层 td include。
3. [`RISCVTargetMachine.cpp/h`](RISCVTargetMachine.h) — TargetMachine 创建与 PassConfig。
4. [`RISCVSubtarget.cpp/h`](RISCVSubtarget.h) — 子目标特性解析。
5. [`CMakeLists.txt`](CMakeLists.txt) — 看 td 生成器清单，了解 LLVM 把 `.td` 转成 `.inc` 的流程。
6. [`RISCVCodeGenPassBuilder.cpp`](RISCVCodeGenPassBuilder.cpp) — 看 RISC-V 把哪些 Pass 插入 LLVM codegen pipeline。

### 阶段 2：指令定义与编码（2-3 小时）
1. [`RISCVInstrInfo.cpp/h`](RISCVInstrInfo.cpp) — 指令查询/转换基类。
2. [`RISCVInstrFormats.td`](RISCVInstrFormats.td) → [`RISCVInstrInfo.td`](RISCVInstrInfo.td) → 各 `RISCVInstrInfo*.td` — 指令定义格式。
3. [[``MCTargetDesc/RISCVMCCodeEmitter.cpp``](MCTargetDesc/RISCVMCCodeEmitter.cpp)](MCTargetDesc/RISCVMCCodeEmitter.cpp) — 编码主流程。
4. [[``MCTargetDesc/RISCVInstPrinter.cpp``](MCTargetDesc/RISCVInstPrinter.cpp)](MCTargetDesc/RISCVInstPrinter.cpp) — 助记符打印。
5. [[``MCTargetDesc/RISCVAsmBackend.cpp``](MCTargetDesc/RISCVAsmBackend.cpp)](MCTargetDesc/RISCVAsmBackend.cpp) — fixup + relax。

### 阶段 3：汇编 / 反汇编（1 小时）
1. [[``AsmParser/RISCVAsmParser.cpp``](AsmParser/RISCVAsmParser.cpp)](AsmParser/RISCVAsmParser.cpp) — 解析主流程。
2. [[``Disassembler/RISCVDisassembler.cpp``](Disassembler/RISCVDisassembler.cpp)](Disassembler/RISCVDisassembler.cpp) — 解码主流程。
3. [[``MCTargetDesc/RISCVMCExpr.cpp``](MCTargetDesc/RISCVMCExpr.cpp)](MCTargetDesc/RISCVMCExpr.cpp) — `%lo/%hi` 等修饰符。

### 阶段 4：指令选择与 Lowering（2-3 小时）
1. [`RISCVISelLowering.cpp/h`](RISCVISelLowering.cpp) — DAG Lowering 入口。
2. [`RISCVISelDAGToDAG.cpp/h`](RISCVISelDAGToDAG.cpp) — DAG-to-DAG 模式选择。
3. [`GISel/`](GISel/) — GlobalISel 各文件。
4. [`RISCVInstrGISel.td`](RISCVInstrGISel.td) / [`RISCVGISel.td`](RISCVGISel.td) — GISel 指令映射。

### 阶段 5：代码生成优化（按需）
- **RVV 流水线**：`RISCVInsertVSETVLI.cpp` → [``RISCVVSETVLIInfoAnalysis.cpp``](RISCVVSETVLIInfoAnalysis.cpp) → `RISCVVLOptimizer.cpp` → `RISCVVectorPeephole.cpp` → `RISCVVectorMaskDAGMutation.cpp`。
- **栈帧与调用约定**：`RISCVFrameLowering.cpp` → `RISCVCallingConv.cpp` → `RISCVRegisterInfo.cpp` → `RISCVInstrInfo.cpp`。
- **Load/Store 优化**：`RISCVLoadStoreOptimizer.cpp` → `RISCVMergeBaseOffset.cpp` → `RISCVFoldMemOffset.cpp`。
- **RVC 压缩**：`RISCVMakeCompressible.cpp` → `MCTargetDesc/RISCVAsmBackend.cpp`（relax）→ `MCTargetDesc/RISCVMCCodeEmitter.cpp`（CompressInstEmitter）。

### 阶段 6：自定义扩展指令
1. [`docs/riscv-custom-extension.md`](../../docs/riscv-custom-extension.md) — 完整实战教程（`XTESTADD` 示例）。
2. `MCTargetDesc/` 下 13 个文件的文件级 NT 注释（已加）— 各模块职责速览。
3. [``MCA/RISCVCustomBehaviour.cpp``](MCA/RISCVCustomBehaviour.cpp) — 如果你的扩展带 RVV 风格行为，需要在 llvm-mca 中建模。

---

## §6. 自定义扩展指南（简要版）

完整流程参见 [`docs/riscv-custom-extension.md`](../../docs/riscv-custom-extension.md)。简版 7 步：

1. **`RISCVFeatures.td`** — 添加新 Feature（如 `HasStdExtXtest`）。
2. **新建 `RISCVInstrInfoXtest.td`** — 用 `RISCVInstrFormats.td` 的格式定义指令。
3. **`RISCVInstrInfo.td`** — include 新 td。
4. **`RISCVProcessors.td`** — 给目标 CPU 添加 `-mattr=+xtest`。
5. **`[`MCTargetDesc/RISCVBaseInfo.cpp`](MCTargetDesc/RISCVBaseInfo.cpp)/h`** — 如有特殊 Specifier / SysReg，加进 RISCVABI / RISCVSysReg 等枚举。
6. **[``MCTargetDesc/RISCVMCTargetDesc.cpp``](MCTargetDesc/RISCVMCTargetDesc.cpp)** — 如果新加 SubtargetFeature 字段，需要在 `RISCVGenSubtargetInfo.inc` 重新生成。
7. **`MCTargetDesc/RISCVAsmParser.cpp` + `RISCVDisassembler.cpp` + `RISCVInstPrinter.cpp`** — 解析 / 解码 / 打印。如指令有复杂编码，可能还需要改 `RISCVMCCodeEmitter.cpp`。

测试建议：

- `llvm-lit -v llvm/test/MC/RISCV/` 跑汇编/反汇编测试。
- `llvm-lit -v llvm/test/CodeGen/RISCV/` 跑 CodeGen 测试。
- 自己写 `clang -c --target=riscv64 -mattr=+xtest` 走完整编译流水线验证。

---

## §7. 文件级 NT 注释索引

`llvm/lib/Target/RISCV/` 下所有 `.cpp` 文件均已添加文件级 NT 注释（搜索 `// <NT>` 可定位），
结构为「文件简介 + 关键函数串联 + 总结」三段式：

- 顶层 30+ 个 Pass / 核心类
- `AsmParser/RISCVAsmParser.cpp` — 13 段 NT 注释（含 3 段文件级 + 10 段函数级）
- `Disassembler/RISCVDisassembler.cpp` — 13 段
- `GISel/*.cpp` — 各 1-3 段
- `MCA/RISCVCustomBehaviour.cpp` — 8 段
- `MCTargetDesc/*.cpp` — 13 个文件均含三段式文件级 NT 注释
- [``TargetInfo/RISCVTargetInfo.cpp``](TargetInfo/RISCVTargetInfo.cpp) — 1 段

加 NT 注释的 skill 见 `.claude/skills/nt-comments/SKILL.md`。