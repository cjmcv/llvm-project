//===-- RISCVAsmPrinter.cpp - RISC-V LLVM assembly writer -----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains a printer that converts from our internal representation
// of machine-dependent LLVM code to the RISC-V assembly language.
//
//===----------------------------------------------------------------------===//

// <NT> 文件简介:
//   RISCVAsmPrinter.cpp 实现 AsmPrinter 子类, 把 MachineFunction + MachineInstr
//   降级为 MCStreamer 上的 MCInst / 汇编文本 / ELF 段元数据. 上游 LLVM 通用
//   AsmPrinter 框架 + LLVM 通用 RegAlloc 之后的 MachineFunction; 下游
//   MCTargetDesc 层 (RISCVMCInst / RISCVELFStreamer / RISCVAsmBackend /
//   RISCVELFObjectWriter / RISCVTargetStreamer). 是 CodeGen → MC 的桥接层,
//   同时是 RISC-V 专属 ELF 输出 (.riscv.attributes / .note.gnu.property /
//   .variant_cc / .option arch) 的唯一发射点.
//
// <NT> 关键函数串联 (单条指令 / 单个函数的 emit 主流程):
//   Module 级别:
//     emitStartOfAsmFile           模块开头: ABI / ISA / .attribute 初始化
//     ├─ emitAttributes          把 Subtarget 翻译成 ELF .RISCV.attributes
//     ├─ HWASAN 初始化           为 HWASAN 标志做准备
//     └─ 读 llvm.riscv-isa MDNode  更新 Subtarget 特性 (用于函数级 .option push)
//     runOnMachineFunction         每个 MF 入口 (per-function 驱动)
//     ├─ emitTargetFeaturePush   必要时 push .option arch
//     ├─ SetupMachineFunction    通用 AsmPrinter 框架
//     ├─ emitFunctionBody        遍历每条 MI 调 emitInstruction
//     ├─ emitXRayTable           XRay patchable 站点表
//     └─ emitTargetFeaturePop    与 push 对称地 pop
//     emitEndOfAsmFile             模块收尾: close attributes / emitNoteGnuProperty / HWASAN 符号
//   MI 级别 (核心循环):
//     emitInstruction             每条 MI 的总入口 (本文件最热的函数)
//     ├─ verifyInstructionPredicates  断言: 当前 Subtarget 是否允许此 opcode
//     ├─ emitNTLHint              若 MI 带 NTL mem 属性, 插入 HINT 编码
//     ├─ MI->MCInst 转换         lowerToMCInst / 伪指令 lowering
//     ├─ 特殊指令: STACKMAP / PATCHPOINT / STATEPOINT / KCFI / HWASAN /
//     │            PATCHABLE_* / CFI / LPAD-aligned call / returns_twice
//     └─ OutStreamer->emitInstruction  最终把 MCInst 推给 MCStreamer
//   其它 emit 入口:
//     emitFunctionEntryLabel       函数标号 + variant_cc 标记 (vector-call ABI)
//     emitMachineConstantPoolValue 长 PC 相对寻址的常量池条目
//     LowerSTACKMAP/PATCHPOINT/STATEPOINT   StackMap 落地
//     LowerHWASAN_CHECK_MEMACCESS / LowerKCFI_CHECK  内存安全 pseudo lowering
//     emitNTLHint                   非临时访存提示 (X2..X5 编码)
//     PrintAsmOperand / PrintAsmMemoryOperand  内联汇编操作数打印
//
// <NT> 总结:
//   本文件是 RISC-V 后端 CodeGen → MC 的总出口. 三大职责:
//     1) MI → MC 转换: 把 MachineInstr 通过 lowerToMCInst / 各种 Lower* 钩子
//        转成 MCInst (含伪指令展开 + RVV 快速路径), 推给 MCStreamer.
//     2) ELF 元数据: 发出 .riscv.attributes / .note.gnu.property /
//        .variant_cc 等 RISC-V 专属段, 与 GNU as 输出兼容.
//     3) 调试/安全: STACKMAP / PATCHPOINT / STATEPOINT / HWASAN / KCFI /
//        XRay / NTL hint 等特殊指令的落地, 满足 sanitizer / debugger /
//        linker relaxation 需求.
//   推荐阅读顺序: runOnMachineFunction -> emitInstruction -> emitStartOfAsmFile ->
//      emitEndOfAsmFile -> lowerToMCInst. 这条线走通后, 再去看
//      LowerSTACKMAP / emitNTLHint / PrintAsmOperand 等分支细节.
//   所有 NT 注释均以 "// <NT>" 开头, 方便搜索定位.
#include "RISCVAsmPrinter.h"
#include "MCTargetDesc/RISCVBaseInfo.h"
#include "MCTargetDesc/RISCVELFStreamer.h"
#include "MCTargetDesc/RISCVInstPrinter.h"
#include "MCTargetDesc/RISCVMCAsmInfo.h"
#include "MCTargetDesc/RISCVMatInt.h"
#include "MCTargetDesc/RISCVTargetStreamer.h"
#include "RISCV.h"
#include "RISCVConstantPoolValue.h"
#include "RISCVMachineFunctionInfo.h"
#include "RISCVRegisterInfo.h"
#include "TargetInfo/RISCVTargetInfo.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/CodeGen/AsmPrinter.h"
#include "llvm/CodeGen/AsmPrinterAnalysis.h"
#include "llvm/CodeGen/MachineConstantPool.h"
#include "llvm/CodeGen/MachineFunctionAnalysisManager.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/IR/Module.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCInstBuilder.h"
#include "llvm/MC/MCObjectFileInfo.h"
#include "llvm/MC/MCSectionELF.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/CHERICapabilityFormat.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/RISCVISAInfo.h"
#include "llvm/Transforms/Instrumentation/HWAddressSanitizer.h"

using namespace llvm;

#define DEBUG_TYPE "asm-printer"

STATISTIC(RISCVNumInstrsCompressed,
          "Number of RISC-V Compressed instructions emitted");

namespace {
class RISCVAsmPrinter : public AsmPrinter {
public:
  static char ID;

private:
  const RISCVSubtarget *STI;

public:
  explicit RISCVAsmPrinter(TargetMachine &TM,
                           std::unique_ptr<MCStreamer> Streamer)
      : AsmPrinter(TM, std::move(Streamer), ID) {}

  StringRef getPassName() const override { return "RISC-V Assembly Printer"; }

  RISCVTargetStreamer &getTargetStreamer() const {
    return static_cast<RISCVTargetStreamer &>(
        *OutStreamer->getTargetStreamer());
  }

  void LowerSTACKMAP(MCStreamer &OutStreamer, StackMaps &SM,
                     const MachineInstr &MI);

  void LowerPATCHPOINT(MCStreamer &OutStreamer, StackMaps &SM,
                       const MachineInstr &MI);

  void LowerSTATEPOINT(MCStreamer &OutStreamer, StackMaps &SM,
                       const MachineInstr &MI);

  bool runOnMachineFunction(MachineFunction &MF) override;

  void emitInstruction(const MachineInstr *MI) override;

  void emitMachineConstantPoolValue(MachineConstantPoolValue *MCPV) override;

  bool PrintAsmOperand(const MachineInstr *MI, unsigned OpNo,
                       const char *ExtraCode, raw_ostream &OS) override;
  bool PrintAsmMemoryOperand(const MachineInstr *MI, unsigned OpNo,
                             const char *ExtraCode, raw_ostream &OS) override;

  // Returns whether Inst is compressed.
  bool EmitToStreamer(MCStreamer &S, const MCInst &Inst,
                      const MCSubtargetInfo &SubtargetInfo);
  bool EmitToStreamer(MCStreamer &S, const MCInst &Inst) {
    return EmitToStreamer(S, Inst, *STI);
  }

  bool lowerPseudoInstExpansion(const MachineInstr *MI, MCInst &Inst);

  typedef std::tuple<unsigned, uint32_t> HwasanMemaccessTuple;
  std::map<HwasanMemaccessTuple, MCSymbol *> HwasanMemaccessSymbols;
  void LowerHWASAN_CHECK_MEMACCESS(const MachineInstr &MI);
  void LowerKCFI_CHECK(const MachineInstr &MI);
  void EmitHwasanMemaccessSymbols(Module &M);

  // Wrapper needed for tblgenned pseudo lowering.
  bool lowerOperand(const MachineOperand &MO, MCOperand &MCOp) const;

  void emitStartOfAsmFile(Module &M) override;
  void emitEndOfAsmFile(Module &M) override;

  void emitFunctionEntryLabel() override;
  bool emitTargetFeaturePush(const MCSubtargetInfo &STI) override;
  void emitTargetFeaturePop(const MCSubtargetInfo &STI, bool DidPush) override;

  void emitNoteGnuProperty(const Module &M);

private:
  void emitAttributes(const MCSubtargetInfo &SubtargetInfo);

  void emitNTLHint(const MachineInstr *MI);

  void emitLpadAlignedCall(const MachineInstr &MI);

  // XRay Support
  void LowerPATCHABLE_FUNCTION_ENTER(const MachineInstr *MI);
  void LowerPATCHABLE_FUNCTION_EXIT(const MachineInstr *MI);
  void LowerPATCHABLE_TAIL_CALL(const MachineInstr *MI);
  void emitSled(const MachineInstr *MI, SledKind Kind);

  void lowerToMCInst(const MachineInstr *MI, MCInst &OutMI);

  MaybeAlign
  getRequiredGlobalAlignmentGranule(const GlobalVariable &GV) override;
};
} // namespace

void RISCVAsmPrinter::LowerSTACKMAP(MCStreamer &OutStreamer, StackMaps &SM,
                                    const MachineInstr &MI) {
  unsigned NOPBytes = STI->hasStdExtZca() ? 2 : 4;
  unsigned NumNOPBytes = StackMapOpers(&MI).getNumPatchBytes();

  auto &Ctx = OutStreamer.getContext();
  MCSymbol *MILabel = Ctx.createTempSymbol();
  OutStreamer.emitLabel(MILabel);

  SM.recordStackMap(*MILabel, MI);
  assert(NumNOPBytes % NOPBytes == 0 &&
         "Invalid number of NOP bytes requested!");

  // Scan ahead to trim the shadow.
  const MachineBasicBlock &MBB = *MI.getParent();
  MachineBasicBlock::const_iterator MII(MI);
  ++MII;
  while (NumNOPBytes > 0) {
    if (MII == MBB.end() || MII->isCall() ||
        MII->getOpcode() == RISCV::DBG_VALUE ||
        MII->getOpcode() == TargetOpcode::PATCHPOINT ||
        MII->getOpcode() == TargetOpcode::STACKMAP)
      break;
    ++MII;
    NumNOPBytes -= NOPBytes;
  }

  // Emit nops.
  emitNops(NumNOPBytes / NOPBytes);
}

// Lower a patchpoint of the form:
// [<def>], <id>, <numBytes>, <target>, <numArgs>
void RISCVAsmPrinter::LowerPATCHPOINT(MCStreamer &OutStreamer, StackMaps &SM,
                                      const MachineInstr &MI) {
  unsigned NOPBytes = STI->hasStdExtZca() ? 2 : 4;

  auto &Ctx = OutStreamer.getContext();
  MCSymbol *MILabel = Ctx.createTempSymbol();
  OutStreamer.emitLabel(MILabel);
  SM.recordPatchPoint(*MILabel, MI);

  PatchPointOpers Opers(&MI);

  const MachineOperand &CalleeMO = Opers.getCallTarget();
  unsigned EncodedBytes = 0;

  if (CalleeMO.isImm()) {
    uint64_t CallTarget = CalleeMO.getImm();
    if (CallTarget) {
      assert((CallTarget & 0xFFFF'FFFF'FFFF) == CallTarget &&
             "High 16 bits of call target should be zero.");
      // Materialize the jump address:
      SmallVector<MCInst, 8> Seq;
      RISCVMatInt::generateMCInstSeq(CallTarget, *STI, RISCV::X1, Seq);
      for (MCInst &Inst : Seq) {
        bool Compressed = EmitToStreamer(OutStreamer, Inst);
        EncodedBytes += Compressed ? 2 : 4;
      }
      bool Compressed = EmitToStreamer(OutStreamer, MCInstBuilder(RISCV::JALR)
                                                        .addReg(RISCV::X1)
                                                        .addReg(RISCV::X1)
                                                        .addImm(0));
      EncodedBytes += Compressed ? 2 : 4;
    }
  } else if (CalleeMO.isGlobal()) {
    MCOperand CallTargetMCOp;
    lowerOperand(CalleeMO, CallTargetMCOp);
    EmitToStreamer(OutStreamer,
                   MCInstBuilder(RISCV::PseudoCALL).addOperand(CallTargetMCOp));
    EncodedBytes += 8;
  }

  // Emit padding.
  unsigned NumBytes = Opers.getNumPatchBytes();
  assert(NumBytes >= EncodedBytes &&
         "Patchpoint can't request size less than the length of a call.");
  assert((NumBytes - EncodedBytes) % NOPBytes == 0 &&
         "Invalid number of NOP bytes requested!");
  emitNops((NumBytes - EncodedBytes) / NOPBytes);
}

void RISCVAsmPrinter::LowerSTATEPOINT(MCStreamer &OutStreamer, StackMaps &SM,
                                      const MachineInstr &MI) {
  unsigned NOPBytes = STI->hasStdExtZca() ? 2 : 4;

  StatepointOpers SOpers(&MI);
  if (unsigned PatchBytes = SOpers.getNumPatchBytes()) {
    assert(PatchBytes % NOPBytes == 0 &&
           "Invalid number of NOP bytes requested!");
    emitNops(PatchBytes / NOPBytes);
  } else {
    // Lower call target and choose correct opcode
    const MachineOperand &CallTarget = SOpers.getCallTarget();
    MCOperand CallTargetMCOp;
    switch (CallTarget.getType()) {
    case MachineOperand::MO_GlobalAddress:
    case MachineOperand::MO_ExternalSymbol:
      lowerOperand(CallTarget, CallTargetMCOp);
      EmitToStreamer(
          OutStreamer,
          MCInstBuilder(RISCV::PseudoCALL).addOperand(CallTargetMCOp));
      break;
    case MachineOperand::MO_Immediate:
      CallTargetMCOp = MCOperand::createImm(CallTarget.getImm());
      EmitToStreamer(OutStreamer, MCInstBuilder(RISCV::JAL)
                                      .addReg(RISCV::X1)
                                      .addOperand(CallTargetMCOp));
      break;
    case MachineOperand::MO_Register:
      CallTargetMCOp = MCOperand::createReg(CallTarget.getReg());
      EmitToStreamer(OutStreamer, MCInstBuilder(RISCV::JALR)
                                      .addReg(RISCV::X1)
                                      .addOperand(CallTargetMCOp)
                                      .addImm(0));
      break;
    default:
      llvm_unreachable("Unsupported operand type in statepoint call target");
      break;
    }
  }

  auto &Ctx = OutStreamer.getContext();
  MCSymbol *MILabel = Ctx.createTempSymbol();
  OutStreamer.emitLabel(MILabel);
  SM.recordStatepoint(*MILabel, MI);
}

bool RISCVAsmPrinter::EmitToStreamer(MCStreamer &S, const MCInst &Inst,
                                     const MCSubtargetInfo &SubtargetInfo) {
  MCInst CInst;
  bool Res = RISCVRVC::compress(CInst, Inst, SubtargetInfo);
  if (Res)
    ++RISCVNumInstrsCompressed;
  S.emitInstruction(Res ? CInst : Inst, SubtargetInfo);
  return Res;
}

// Simple pseudo-instructions have their lowering (with expansion to real
// instructions) auto-generated.
#include "RISCVGenMCPseudoLowering.inc"

// Emit a call to a returns_twice function with LPAD.
// When Zca is enabled, emit .p2align 2 before the call to ensure the
// following LPAD is 4-byte aligned. For assembly output, wrap with
// .option push/exact/pop to prevent relaxation. For object output,
// emit the pseudo directly so MCCodeEmitter handles it without R_RISCV_RELAX.

// <NT> Zicfilp 对齐 call 发射:
//   emitLpadAlignedCall 处理 returns_twice 调用 (如 setjmp / vfork / Lua
//   yield 等). Zicfilp (landing pad) 要求 LPAD 必须 4 字节对齐; 当启用 Zca
//   (RVC 压缩) 时函数入口可能 2 字节对齐, 需要在 call 之前插 .p2align 2.
//   关键机制: 汇编输出走 .option push/exact/pop 关闭 relaxation, 让汇编器
//   不要把 call 后的 LPAD 跨边界合并; 对象输出走 pseudo 形式, 由
//   RISCVMCCodeEmitter 写 R_RISCV_RELAX 让 AsmBackend 在 fixup 阶段处理.
//   上游: emitInstruction 在识别到 returns_twice call 时调用; 下游:
//   OutStreamer + RISCVMCCodeEmitter. 失败模式: Zca 关闭时不需要 align, 直接
//   走 emitCall 即可; 没正确 emit .p2align 会导致运行时 LPAD misaligned trap.
void RISCVAsmPrinter::emitLpadAlignedCall(const MachineInstr &MI) {
  const MCSubtargetInfo &MCSTI = getSubtargetInfo();
  const bool IsIndirect = MI.getOpcode() == RISCV::PseudoCALLIndirectLpadAlign,
             HasZca = MCSTI.hasFeature(RISCV::FeatureStdExtZca),
             HasRelax = MCSTI.hasFeature(RISCV::FeatureRelax);

  if (HasZca)
    OutStreamer->emitCodeAlignment(Align(4), MCSTI);

  if (OutStreamer->hasRawTextSupport()) {
    // Assembly path: wrap call with .option push/exact/pop and emit LPAD
    // separately so the output is human-readable.
    RISCVTargetStreamer &RTS = getTargetStreamer();
    if (HasZca && HasRelax) {
      RTS.emitDirectiveOptionPush();
      RTS.emitDirectiveOptionExact();
    }

    MCInst CallInst;
    if (!IsIndirect) {
      MCOperand MCOp;
      lowerOperand(MI.getOperand(0), MCOp);
      CallInst = MCInstBuilder(RISCV::PseudoCALL).addOperand(MCOp);
    } else {
      CallInst = MCInstBuilder(RISCV::JALR)
                     .addReg(RISCV::X1)
                     .addReg(MI.getOperand(0).getReg())
                     .addImm(0);
    }

    if (HasZca && HasRelax) {
      MCSubtargetInfo NoRelaxSTI(MCSTI);
      NoRelaxSTI.ToggleFeature(RISCV::FeatureRelax);
      EmitToStreamer(*OutStreamer, CallInst, NoRelaxSTI);
      RTS.emitDirectiveOptionPop();
    } else {
      EmitToStreamer(*OutStreamer, CallInst, MCSTI);
    }

    // LPAD is encoded as AUIPC X0, label.
    MCInst LpadInst = MCInstBuilder(RISCV::AUIPC)
                          .addReg(RISCV::X0)
                          .addImm(MI.getOperand(1).getImm());
    EmitToStreamer(*OutStreamer, LpadInst, MCSTI);
  } else {
    // Object path: emit PseudoCALL(Indirect)LpadAlign directly.
    // MCCodeEmitter::expandFunctionCallLpad expands to AUIPC+JALR+LPAD
    // without emitting R_RISCV_RELAX on the call fixup.
    MCInst TmpInst;
    TmpInst.setOpcode(MI.getOpcode());
    if (!IsIndirect) {
      MCOperand MCOp;
      lowerOperand(MI.getOperand(0), MCOp);
      TmpInst.addOperand(MCOp);
    } else {
      TmpInst.addOperand(MCOperand::createReg(MI.getOperand(0).getReg()));
    }
    TmpInst.addOperand(MCOperand::createImm(MI.getOperand(1).getImm()));
    EmitToStreamer(*OutStreamer, TmpInst, MCSTI);
  }
}

// If the instruction has a nontemporal MachineMemOperand, emit an NTL hint
// instruction before it. NTL hints are always safe to emit since they use
// HINT encodings that are guaranteed not to trap
// (riscv-non-isa/riscv-elf-psabi-doc#474).

// <NT> 非临时访存 HINT 编码:
//   emitNTLHint 处理 IR 层标记的 __builtin_nontemporal_* 访存. RISC-V 没有
//   专用 NTL 指令, 把 4 种 NTL 模式 (NONE / ALLOC / WRITE_THROUGH / WRITE_BACK)
//   编码成 5 位的 HINT 立即数塞进 C.ADD / ADD rd, rs, hint 的 funct3 字段.
//   关键机制: HINT 指令 (funct7=0, rs2=x0) 保证不 trap, 因此即使目标 CPU
//   不识别也只会被忽略, 完全安全. 上游: emitInstruction 在每条访存 MI 前
//   调用, 检查是否有 NonTemporal MachineMemOperand; 下游: MCStreamer 接收
//   HINT MCInst, 经 RISCVMCCodeEmitter 编码为 32 位字. 失败模式: 编译器错误
//   把 NTL 标在了写 / 读不一致的访问上, 会让数据缓存行为错误, 排查时检查
//   MI.getMemOperands() 的 NonTemporal 标记位.
void RISCVAsmPrinter::emitNTLHint(const MachineInstr *MI) {
  if (!STI->getInstrInfo()->requiresNTLHint(*MI))
    return;

  assert(!MI->memoperands_empty());

  MachineMemOperand *MMO = *(MI->memoperands_begin());

  assert(MMO->isNonTemporal());

  unsigned NontemporalMode = 0;
  if (MMO->getFlags() & MONontemporalBit0)
    NontemporalMode += 0b1;
  if (MMO->getFlags() & MONontemporalBit1)
    NontemporalMode += 0b10;

  MCInst Hint;
  if (STI->hasStdExtZca())
    Hint.setOpcode(RISCV::C_ADD);
  else
    Hint.setOpcode(RISCV::ADD);

  Hint.addOperand(MCOperand::createReg(RISCV::X0));
  Hint.addOperand(MCOperand::createReg(RISCV::X0));
  Hint.addOperand(MCOperand::createReg(RISCV::X2 + NontemporalMode));

  EmitToStreamer(*OutStreamer, Hint);
}

// <NT> MI emit 分派中心 (每条 MachineInstr 的总入口):
//   emitInstruction 是本文件最热的函数, LLVM 后端每条 MachineFunction 的每条
//   MI 都会过这里一次. 关键机制: 先用 td 生成的 verifyInstructionPredicates
//   断言 MI 的 opcode 与当前 Subtarget 兼容 (调试期; Release 不开), 再按需
//   插入 NTL HINT, 最后根据 MI 类型分派:
//     1) 伪指令 (Pseudo) -> 走 Lower* 钩子 (STACKMAP / PATCHPOINT /
//        STATEPOINT / HWASAN / KCFI / PATCHABLE_* / LPAD-aligned call) 或
//        OutStreamer->emitInstruction 默认路径 (lowerToMCInst 自动生成).
//     2) CFI 指令 -> 直接 emit.
//     3) 真实指令 -> emitToCompressedInst -> OutStreamer->emitInstruction.
//   上游: AsmPrinter::emitFunctionBody 的指令循环; 下游: OutStreamer ->
//   RISCVMCCodeEmitter -> RISCVAsmBackend. 失败模式: 添加新指令后忘记写
//   Predicates, Predicate 断言会先于此函数失败, 提示你补 td 的 Predicate 字段.
void RISCVAsmPrinter::emitInstruction(const MachineInstr *MI) {
  RISCV_MC::verifyInstructionPredicates(MI->getOpcode(), STI->getFeatureBits());

  emitNTLHint(MI);

  // Do any auto-generated pseudo lowerings.
  if (MCInst OutInst; lowerPseudoInstExpansion(MI, OutInst)) {
    EmitToStreamer(*OutStreamer, OutInst);
    return;
  }

  switch (MI->getOpcode()) {
  case RISCV::PseudoTAILX7: {
    // Lower to PseudoTAILReg with X7 as the register operand.
    MCOperand SymOp;
    lowerOperand(MI->getOperand(0), SymOp);
    MCInst TmpInst;
    TmpInst.setOpcode(RISCV::PseudoTAILReg);
    TmpInst.addOperand(SymOp);
    TmpInst.addOperand(MCOperand::createReg(RISCV::X7));
    EmitToStreamer(*OutStreamer, TmpInst);
    return;
  }
  case RISCV::HWASAN_CHECK_MEMACCESS_SHORTGRANULES:
    LowerHWASAN_CHECK_MEMACCESS(*MI);
    return;
  case RISCV::KCFI_CHECK:
    LowerKCFI_CHECK(*MI);
    return;
  case TargetOpcode::STACKMAP:
    return LowerSTACKMAP(*OutStreamer, SM, *MI);
  case TargetOpcode::PATCHPOINT:
    return LowerPATCHPOINT(*OutStreamer, SM, *MI);
  case TargetOpcode::STATEPOINT:
    return LowerSTATEPOINT(*OutStreamer, SM, *MI);
  case TargetOpcode::PATCHABLE_FUNCTION_ENTER: {
    const Function &F = MI->getParent()->getParent()->getFunction();
    if (F.hasFnAttribute("patchable-function-entry")) {
      unsigned Num =
          F.getFnAttributeAsParsedInteger("patchable-function-entry");
      emitNops(Num);
      return;
    }
    LowerPATCHABLE_FUNCTION_ENTER(MI);
    return;
  }
  case TargetOpcode::PATCHABLE_FUNCTION_EXIT:
    LowerPATCHABLE_FUNCTION_EXIT(MI);
    return;
  case TargetOpcode::PATCHABLE_TAIL_CALL:
    LowerPATCHABLE_TAIL_CALL(MI);
    return;
  case RISCV::PseudoCALLLpadAlign:
  case RISCV::PseudoCALLIndirectLpadAlign:
    emitLpadAlignedCall(*MI);
    return;
  }

  MCInst OutInst;
  lowerToMCInst(MI, OutInst);
  EmitToStreamer(*OutStreamer, OutInst);
}

// <NT> inline asm 操作数打印:
//   PrintAsmOperand 是 inline asm 操作数的 RISC-V 专属打印入口. 关键机制:
//   在通用 AsmPrinter 框架之前先处理三个 RISC-V 专属修饰符:
//     z  -> 零寄存器 (x0)         (用于 cmp $0, $1 之类的内联汇编)
//     i  -> 立即数                  (Imm 强制, 不走寄存器)
//     N  -> 寄存器编码号 (1..31)    (用于 mfgid 之类的系统指令)
//   其它修饰符直接走父类 AsmPrinter::PrintAsmOperand.
//   上游: AsmPrinter 在处理 InlineAsm 的每个 MCInlineAsmOperand 时调用;
//   下游: raw_ostream 直接写到 .s 文件 (或 MCStreamer buffer). 失败模式:
//   内联汇编里写 "li $0, 1" 而不是 "li x0, 1" 会被打印成 "li r0, 1" 导致
//   GAS 报错; 此时检查是否需要新增修饰符.
bool RISCVAsmPrinter::PrintAsmOperand(const MachineInstr *MI, unsigned OpNo,
                                      const char *ExtraCode, raw_ostream &OS) {
  // First try the generic code, which knows about modifiers like 'c' and 'n'.
  if (!AsmPrinter::PrintAsmOperand(MI, OpNo, ExtraCode, OS))
    return false;

  const MachineOperand &MO = MI->getOperand(OpNo);
  if (ExtraCode && ExtraCode[0]) {
    if (ExtraCode[1] != 0)
      return true; // Unknown modifier.

    switch (ExtraCode[0]) {
    default:
      return true; // Unknown modifier.
    case 'z':      // Print zero register if zero, regular printing otherwise.
      if (MO.isImm() && MO.getImm() == 0) {
        OS << RISCVInstPrinter::getRegisterName(RISCV::X0);
        return false;
      }
      break;
    case 'i': // Literal 'i' if operand is not a register.
      if (!MO.isReg())
        OS << 'i';
      return false;
    case 'N': // Print the register encoding as an integer (0-31)
      if (!MO.isReg())
        return true;

      const RISCVRegisterInfo *TRI = STI->getRegisterInfo();
      OS << TRI->getEncodingValue(MO.getReg());
      return false;
    }
  }

  switch (MO.getType()) {
  case MachineOperand::MO_Immediate:
    OS << MO.getImm();
    return false;
  case MachineOperand::MO_Register:
    OS << RISCVInstPrinter::getRegisterName(MO.getReg());
    return false;
  case MachineOperand::MO_GlobalAddress:
    PrintSymbolOperand(MO, OS);
    return false;
  case MachineOperand::MO_BlockAddress: {
    MCSymbol *Sym = GetBlockAddressSymbol(MO.getBlockAddress());
    Sym->print(OS, MAI);
    return false;
  }
  default:
    break;
  }

  return true;
}

bool RISCVAsmPrinter::PrintAsmMemoryOperand(const MachineInstr *MI,
                                            unsigned OpNo,
                                            const char *ExtraCode,
                                            raw_ostream &OS) {
  if (ExtraCode)
    return AsmPrinter::PrintAsmMemoryOperand(MI, OpNo, ExtraCode, OS);

  const MachineOperand &AddrReg = MI->getOperand(OpNo);
  assert(MI->getNumOperands() > OpNo + 1 && "Expected additional operand");
  const MachineOperand &Offset = MI->getOperand(OpNo + 1);
  // All memory operands should have a register and an immediate operand (see
  // RISCVDAGToDAGISel::SelectInlineAsmMemoryOperand).
  if (!AddrReg.isReg())
    return true;
  if (!Offset.isImm() && !Offset.isGlobal() && !Offset.isBlockAddress() &&
      !Offset.isMCSymbol())
    return true;

  MCOperand MCO;
  if (!lowerOperand(Offset, MCO))
    return true;

  if (Offset.isImm())
    OS << MCO.getImm();
  else if (Offset.isGlobal() || Offset.isBlockAddress() || Offset.isMCSymbol())
    MAI.printExpr(OS, *MCO.getExpr());

  if (Offset.isMCSymbol())
    MMI->getContext().registerInlineAsmLabel(Offset.getMCSymbol());
  if (Offset.isBlockAddress()) {
    const BlockAddress *BA = Offset.getBlockAddress();
    MCSymbol *Sym = GetBlockAddressSymbol(BA);
    MMI->getContext().registerInlineAsmLabel(Sym);
  }

  OS << "(" << RISCVInstPrinter::getRegisterName(AddrReg.getReg()) << ")";
  return false;
}

bool RISCVAsmPrinter::emitTargetFeaturePush(const MCSubtargetInfo &STI) {
  RISCVTargetStreamer &RTS = getTargetStreamer();
  SmallVector<RISCVOptionArchArg> NeedEmitStdOptionArgs;
  const MCSubtargetInfo &MCSTI = TM.getMCSubtargetInfo();
  for (const auto &Feature : MCSTI.getAllProcessorFeatures()) {
    if (STI.hasFeature(Feature.Value) == MCSTI.hasFeature(Feature.Value))
      continue;

    if (!llvm::RISCVISAInfo::isSupportedExtensionFeature(Feature.key()))
      continue;

    auto Delta = STI.hasFeature(Feature.Value) ? RISCVOptionArchArgType::Plus
                                               : RISCVOptionArchArgType::Minus;
    StringRef ExtName = Feature.key();
    ExtName.consume_front("experimental-");
    NeedEmitStdOptionArgs.emplace_back(Delta, ExtName.str());
  }
  if (!NeedEmitStdOptionArgs.empty()) {
    RTS.emitDirectiveOptionPush();
    RTS.emitDirectiveOptionArch(NeedEmitStdOptionArgs);
    return true;
  }

  return false;
}

void RISCVAsmPrinter::emitTargetFeaturePop(const MCSubtargetInfo &STI,
                                           bool DidPush) {
  if (DidPush)
    getTargetStreamer().emitDirectiveOptionPop();
}

// <NT> 每个 MachineFunction 的入口驱动:
//   runOnMachineFunction 是每个 MachineFunction 必经的入口, 串起整个
//   per-function 打印流程. 关键机制:
//     1) STI 指向本 MF 的 Subtarget (per-function 特性, 由函数属性覆盖).
//     2) emitTargetFeaturePush 必要时写 .option push / .option arch, 让本函数
//        的指令编码按 Subtarget 跑; emitTargetFeaturePop 在结尾对称 pop.
//     3) SetupMachineFunction 是通用 AsmPrinter 框架 (frame info / CFI 等).
//     4) emitFunctionBody 是 LLVM 通用指令循环, 每条 MI -> emitInstruction.
//     5) emitXRayTable 写本 MF 的 XRay patchable 站点表 (供运行时插桩).
//   上游: AsmPrinter::runOnMachineFunction; 下游: emitInstruction / 通用框架.
//   失败模式: 没正确配 STI 会让函数级 .option 写到错误的 ISA 字符串; 检查
//   emitTargetFeaturePush 是否在 emitFunctionBody 之前调.
bool RISCVAsmPrinter::runOnMachineFunction(MachineFunction &MF) {
  STI = &MF.getSubtarget<RISCVSubtarget>();

  bool EmittedOptionArch = emitTargetFeaturePush(*STI);

  SetupMachineFunction(MF);
  emitFunctionBody();

  // Emit the XRay table
  emitXRayTable();

  emitTargetFeaturePop(*STI, EmittedOptionArch);
  return false;
}

void RISCVAsmPrinter::LowerPATCHABLE_FUNCTION_ENTER(const MachineInstr *MI) {
  emitSled(MI, SledKind::FUNCTION_ENTER);
}

void RISCVAsmPrinter::LowerPATCHABLE_FUNCTION_EXIT(const MachineInstr *MI) {
  emitSled(MI, SledKind::FUNCTION_EXIT);
}

void RISCVAsmPrinter::LowerPATCHABLE_TAIL_CALL(const MachineInstr *MI) {
  emitSled(MI, SledKind::TAIL_CALL);
}

void RISCVAsmPrinter::emitSled(const MachineInstr *MI, SledKind Kind) {
  // We want to emit the jump instruction and the nops constituting the sled.
  // The format is as follows:
  // .Lxray_sled_N
  //   ALIGN
  //   J .tmpN
  //   21 or 33 C.NOP instructions
  // .tmpN

  // The following variable holds the count of the number of NOPs to be patched
  // in for XRay instrumentation during compilation.
  // Note that RV64 and RV32 each has a sled of 68 and 44 bytes, respectively.
  // Assuming we're using JAL to jump to .tmpN, then we only need
  // (68 - 4)/2 = 32 NOPs for RV64 and (44 - 4)/2 = 20 for RV32. However, there
  // is a chance that we'll use C.JAL instead, so an additional NOP is needed.
  const uint8_t NoopsInSledCount = STI->is64Bit() ? 33 : 21;

  OutStreamer->emitCodeAlignment(Align(4), *STI);
  auto CurSled = OutContext.createTempSymbol("xray_sled_", true);
  OutStreamer->emitLabel(CurSled);
  auto Target = OutContext.createTempSymbol();

  const MCExpr *TargetExpr = MCSymbolRefExpr::create(Target, OutContext);

  // Emit "J bytes" instruction, which jumps over the nop sled to the actual
  // start of function.
  EmitToStreamer(
      *OutStreamer,
      MCInstBuilder(RISCV::JAL).addReg(RISCV::X0).addExpr(TargetExpr));

  // Emit NOP instructions
  for (int8_t I = 0; I < NoopsInSledCount; ++I)
    EmitToStreamer(*OutStreamer, MCInstBuilder(RISCV::ADDI)
                                     .addReg(RISCV::X0)
                                     .addReg(RISCV::X0)
                                     .addImm(0));

  OutStreamer->emitLabel(Target);
  recordSled(CurSled, *MI, Kind, 2);
}

// <NT> 模块开头钩子 (.s 文件级):
//   emitStartOfAsmFile 是模块级 .s 文件的开头钩子. 关键机制:
//     1) 校验 TargetStreamer 已注入 (RISCVTargetELFStreamer 或 Darwin 版),
//        否则 ABI / ISA 信息无法落地.
//     2) emitAttributes 转发给 RISCVTargetStreamer, 把 Subtarget 翻译为
//        ELF .RISCV.attributes 段条目 (ABI / 原子 / FP ABI 等).
//     3) 读 Module 的 llvm.riscv-isa MDNode (用于 LTO 等场景下让函数级
//        特性覆盖模块级 Subtarget), 调用 Subtarget 的 updateFeatureBits.
//   上游: AsmPrinter::doInitialization / AsmPrinter::OutStreamer 初始化;
//   下游: RISCVTargetStreamer / RISCVSubtarget. 失败模式: Module 含非法
//   ISA 字符串会抛 "Unsupported feature"; 检查 RISC-V Subtarget 是否在
//   RISCVFeatures::parseFeatureBits 中已加对应关键字.
void RISCVAsmPrinter::emitStartOfAsmFile(Module &M) {
  assert(OutStreamer->getTargetStreamer() &&
         "target streamer is uninitialized");
  RISCVTargetStreamer &RTS = getTargetStreamer();
  if (const MDString *ModuleTargetABI =
          dyn_cast_or_null<MDString>(M.getModuleFlag("target-abi")))
    RTS.setTargetABI(RISCVABI::getTargetABI(ModuleTargetABI->getString()));

  MCSubtargetInfo SubtargetInfo = TM.getMCSubtargetInfo();

  // Use module flag to update feature bits.
  if (auto *MD = dyn_cast_or_null<MDNode>(M.getModuleFlag("riscv-isa"))) {
    for (auto &ISA : MD->operands()) {
      if (auto *ISAString = dyn_cast_or_null<MDString>(ISA)) {
        auto ParseResult = llvm::RISCVISAInfo::parseArchString(
            ISAString->getString(), /*EnableExperimentalExtension=*/true,
            /*ExperimentalExtensionVersionCheck=*/true);
        if (!errorToBool(ParseResult.takeError())) {
          auto &ISAInfo = *ParseResult;
          for (const auto &Feature : SubtargetInfo.getAllProcessorFeatures()) {
            if (ISAInfo->hasExtension(Feature.key()) &&
                !SubtargetInfo.hasFeature(Feature.Value))
              SubtargetInfo.ToggleFeature(Feature.key());
          }
        }
      }
    }

    RTS.setFlagsFromFeatures(SubtargetInfo);
  }

  if (TM.getTargetTriple().isOSBinFormatELF())
    emitAttributes(SubtargetInfo);
}

// <NT> 模块收尾钩子 (.s 文件级):
//   emitEndOfAsmFile 是模块级 .s 文件的收尾钩子. 关键机制:
//     1) RTS.finishAttributeSection 把累积的 .riscv.attributes 写入 ELF 段
//        (在 ELF 模式下, Mach-O 跳过此步).
//     2) emitNoteGnuProperty 写 .note.gnu.property 段, 声明 CFI 方案
//        (Zicfiss shadow stack / Zicfilp LPAD), 让运行时和链接器能识别.
//     3) EmitHwasanMemaccessSymbols 写每个 HWASAN 函数的 __hwasan_check_*
//        影子符号, 让 sanitizer runtime 在 LTO 后仍能解析对应 helper.
//   上游: AsmPrinter::doFinalization; 下游: RISCVTargetStreamer /
//   MCStreamer (落盘 .riscv.attributes / .note.gnu.property / 符号表).
//   失败模式: ELF flag 错误 (没识别出 ELF triple) 会让 finishAttributeSection
//   走空, .riscv.attributes 段缺失, GCC 链接器不识别; 检查 TM.getTargetTriple().
void RISCVAsmPrinter::emitEndOfAsmFile(Module &M) {
  RISCVTargetStreamer &RTS = getTargetStreamer();

  if (TM.getTargetTriple().isOSBinFormatELF()) {
    RTS.finishAttributeSection();
    emitNoteGnuProperty(M);
  }
  EmitHwasanMemaccessSymbols(M);
}

void RISCVAsmPrinter::emitAttributes(const MCSubtargetInfo &SubtargetInfo) {
  RISCVTargetStreamer &RTS = getTargetStreamer();
  // Use MCSubtargetInfo from TargetMachine. Individual functions may have
  // attributes that differ from other functions in the module and we have no
  // way to know which function is correct.
  RTS.emitTargetAttributes(SubtargetInfo, /*EmitStackAlign*/ true);
}

void RISCVAsmPrinter::emitFunctionEntryLabel() {
  const auto *RMFI = MF->getInfo<RISCVMachineFunctionInfo>();
  if (RMFI->isVectorCall()) {
    RISCVTargetStreamer &RTS = getTargetStreamer();
    RTS.emitDirectiveVariantCC(*CurrentFnSym);
  }
  return AsmPrinter::emitFunctionEntryLabel();
}

// Force static initialization.
extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void
LLVMInitializeRISCVAsmPrinter() {
  RegisterAsmPrinter<RISCVAsmPrinter> X(getTheRISCV32Target());
  RegisterAsmPrinter<RISCVAsmPrinter> Y(getTheRISCV64Target());
  RegisterAsmPrinter<RISCVAsmPrinter> A(getTheRISCV32beTarget());
  RegisterAsmPrinter<RISCVAsmPrinter> B(getTheRISCV64beTarget());
}

void RISCVAsmPrinter::LowerHWASAN_CHECK_MEMACCESS(const MachineInstr &MI) {
  Register Reg = MI.getOperand(0).getReg();
  uint32_t AccessInfo = MI.getOperand(1).getImm();
  MCSymbol *&Sym =
      HwasanMemaccessSymbols[HwasanMemaccessTuple(Reg, AccessInfo)];
  if (!Sym) {
    // FIXME: Make this work on non-ELF.
    if (!TM.getTargetTriple().isOSBinFormatELF())
      report_fatal_error("llvm.hwasan.check.memaccess only supported on ELF");

    std::string SymName = "__hwasan_check_x" + utostr(Reg - RISCV::X0) + "_" +
                          utostr(AccessInfo) + "_short";
    Sym = OutContext.getOrCreateSymbol(SymName);
  }
  auto Res = MCSymbolRefExpr::create(Sym, OutContext);
  auto Expr = MCSpecifierExpr::create(Res, RISCV::S_CALL_PLT, OutContext);

  EmitToStreamer(*OutStreamer, MCInstBuilder(RISCV::PseudoCALL).addExpr(Expr));
}

void RISCVAsmPrinter::LowerKCFI_CHECK(const MachineInstr &MI) {
  Register AddrReg = MI.getOperand(0).getReg();
  assert(std::next(MI.getIterator())->isCall() &&
         "KCFI_CHECK not followed by a call instruction");
  assert(std::next(MI.getIterator())->getOperand(0).getReg() == AddrReg &&
         "KCFI_CHECK call target doesn't match call operand");

  // Temporary registers for comparing the hashes. If a register is used
  // for the call target, or reserved by the user, we can clobber another
  // temporary register as the check is immediately followed by the
  // call. The check defaults to X6/X7, but can fall back to X28-X31 if
  // needed.
  unsigned ScratchRegs[] = {RISCV::X6, RISCV::X7};
  unsigned NextReg = RISCV::X28;
  auto isRegAvailable = [&](unsigned Reg) {
    return Reg != AddrReg && !STI->isRegisterReservedByUser(Reg);
  };
  for (auto &Reg : ScratchRegs) {
    if (isRegAvailable(Reg))
      continue;
    while (!isRegAvailable(NextReg))
      ++NextReg;
    Reg = NextReg++;
    if (Reg > RISCV::X31)
      report_fatal_error("Unable to find scratch registers for KCFI_CHECK");
  }

  if (AddrReg == RISCV::X0) {
    // Checking X0 makes no sense. Instead of emitting a load, zero
    // ScratchRegs[0].
    EmitToStreamer(*OutStreamer, MCInstBuilder(RISCV::ADDI)
                                     .addReg(ScratchRegs[0])
                                     .addReg(RISCV::X0)
                                     .addImm(0));
  } else {
    // Adjust the offset for patchable-function-prefix. This assumes that
    // patchable-function-prefix is the same for all functions.
    int NopSize = STI->hasStdExtZca() ? 2 : 4;
    int64_t PrefixNops =
        MI.getMF()->getFunction().getFnAttributeAsParsedInteger(
            "patchable-function-prefix");

    // Load the target function type hash.
    EmitToStreamer(*OutStreamer, MCInstBuilder(RISCV::LW)
                                     .addReg(ScratchRegs[0])
                                     .addReg(AddrReg)
                                     .addImm(-(PrefixNops * NopSize + 4)));
  }

  // Load the expected 32-bit type hash.
  const int64_t Type = MI.getOperand(1).getImm();
  const int64_t Hi20 = ((Type + 0x800) >> 12) & 0xFFFFF;
  const int64_t Lo12 = SignExtend64<12>(Type);
  if (Hi20) {
    EmitToStreamer(
        *OutStreamer,
        MCInstBuilder(RISCV::LUI).addReg(ScratchRegs[1]).addImm(Hi20));
  }
  if (Lo12 || Hi20 == 0) {
    EmitToStreamer(*OutStreamer,
                   MCInstBuilder((STI->hasFeature(RISCV::Feature64Bit) && Hi20)
                                     ? RISCV::ADDIW
                                     : RISCV::ADDI)
                       .addReg(ScratchRegs[1])
                       .addReg(ScratchRegs[1])
                       .addImm(Lo12));
  }

  // Compare the hashes and trap if there's a mismatch.
  MCSymbol *Pass = OutContext.createTempSymbol();
  EmitToStreamer(*OutStreamer,
                 MCInstBuilder(RISCV::BEQ)
                     .addReg(ScratchRegs[0])
                     .addReg(ScratchRegs[1])
                     .addExpr(MCSymbolRefExpr::create(Pass, OutContext)));

  MCSymbol *Trap = OutContext.createTempSymbol();
  OutStreamer->emitLabel(Trap);
  EmitToStreamer(*OutStreamer, MCInstBuilder(RISCV::EBREAK));
  emitKCFITrapEntry(*MI.getMF(), Trap);
  OutStreamer->emitLabel(Pass);
}

void RISCVAsmPrinter::EmitHwasanMemaccessSymbols(Module &M) {
  if (HwasanMemaccessSymbols.empty())
    return;

  assert(TM.getTargetTriple().isOSBinFormatELF());
  // Use MCSubtargetInfo from TargetMachine. Individual functions may have
  // attributes that differ from other functions in the module and we have no
  // way to know which function is correct.
  const MCSubtargetInfo &MCSTI = TM.getMCSubtargetInfo();

  MCSymbol *HwasanTagMismatchV2Sym =
      OutContext.getOrCreateSymbol("__hwasan_tag_mismatch_v2");
  // Annotate symbol as one having incompatible calling convention, so
  // run-time linkers can instead eagerly bind this function.
  RISCVTargetStreamer &RTS = getTargetStreamer();
  RTS.emitDirectiveVariantCC(*HwasanTagMismatchV2Sym);

  const MCSymbolRefExpr *HwasanTagMismatchV2Ref =
      MCSymbolRefExpr::create(HwasanTagMismatchV2Sym, OutContext);
  auto Expr = MCSpecifierExpr::create(HwasanTagMismatchV2Ref, RISCV::S_CALL_PLT,
                                      OutContext);

  for (auto &P : HwasanMemaccessSymbols) {
    unsigned Reg = std::get<0>(P.first);
    uint32_t AccessInfo = std::get<1>(P.first);
    MCSymbol *Sym = P.second;

    unsigned Size =
        1 << ((AccessInfo >> HWASanAccessInfo::AccessSizeShift) & 0xf);
    OutStreamer->switchSection(OutContext.getELFSection(
        ".text.hot", ELF::SHT_PROGBITS,
        ELF::SHF_EXECINSTR | ELF::SHF_ALLOC | ELF::SHF_GROUP, 0, Sym->getName(),
        /*IsComdat=*/true));

    OutStreamer->emitSymbolAttribute(Sym, MCSA_ELF_TypeFunction);
    OutStreamer->emitSymbolAttribute(Sym, MCSA_Weak);
    OutStreamer->emitSymbolAttribute(Sym, MCSA_Hidden);
    OutStreamer->emitLabel(Sym);

    // Extract shadow offset from ptr
    EmitToStreamer(
        *OutStreamer,
        MCInstBuilder(RISCV::SLLI).addReg(RISCV::X6).addReg(Reg).addImm(8),
        MCSTI);
    EmitToStreamer(*OutStreamer,
                   MCInstBuilder(RISCV::SRLI)
                       .addReg(RISCV::X6)
                       .addReg(RISCV::X6)
                       .addImm(12),
                   MCSTI);
    // load shadow tag in X6, X5 contains shadow base
    EmitToStreamer(*OutStreamer,
                   MCInstBuilder(RISCV::ADD)
                       .addReg(RISCV::X6)
                       .addReg(RISCV::X5)
                       .addReg(RISCV::X6),
                   MCSTI);
    EmitToStreamer(
        *OutStreamer,
        MCInstBuilder(RISCV::LBU).addReg(RISCV::X6).addReg(RISCV::X6).addImm(0),
        MCSTI);
    // Extract tag from pointer and compare it with loaded tag from shadow
    EmitToStreamer(
        *OutStreamer,
        MCInstBuilder(RISCV::SRLI).addReg(RISCV::X7).addReg(Reg).addImm(56),
        MCSTI);
    MCSymbol *HandleMismatchOrPartialSym = OutContext.createTempSymbol();
    // X7 contains tag from the pointer, while X6 contains tag from memory
    EmitToStreamer(*OutStreamer,
                   MCInstBuilder(RISCV::BNE)
                       .addReg(RISCV::X7)
                       .addReg(RISCV::X6)
                       .addExpr(MCSymbolRefExpr::create(
                           HandleMismatchOrPartialSym, OutContext)),
                   MCSTI);
    MCSymbol *ReturnSym = OutContext.createTempSymbol();
    OutStreamer->emitLabel(ReturnSym);
    EmitToStreamer(*OutStreamer,
                   MCInstBuilder(RISCV::JALR)
                       .addReg(RISCV::X0)
                       .addReg(RISCV::X1)
                       .addImm(0),
                   MCSTI);
    OutStreamer->emitLabel(HandleMismatchOrPartialSym);

    EmitToStreamer(*OutStreamer,
                   MCInstBuilder(RISCV::ADDI)
                       .addReg(RISCV::X28)
                       .addReg(RISCV::X0)
                       .addImm(16),
                   MCSTI);
    MCSymbol *HandleMismatchSym = OutContext.createTempSymbol();
    EmitToStreamer(
        *OutStreamer,
        MCInstBuilder(RISCV::BGEU)
            .addReg(RISCV::X6)
            .addReg(RISCV::X28)
            .addExpr(MCSymbolRefExpr::create(HandleMismatchSym, OutContext)),
        MCSTI);

    EmitToStreamer(
        *OutStreamer,
        MCInstBuilder(RISCV::ANDI).addReg(RISCV::X28).addReg(Reg).addImm(0xF),
        MCSTI);

    if (Size != 1)
      EmitToStreamer(*OutStreamer,
                     MCInstBuilder(RISCV::ADDI)
                         .addReg(RISCV::X28)
                         .addReg(RISCV::X28)
                         .addImm(Size - 1),
                     MCSTI);
    EmitToStreamer(
        *OutStreamer,
        MCInstBuilder(RISCV::BGE)
            .addReg(RISCV::X28)
            .addReg(RISCV::X6)
            .addExpr(MCSymbolRefExpr::create(HandleMismatchSym, OutContext)),
        MCSTI);

    EmitToStreamer(
        *OutStreamer,
        MCInstBuilder(RISCV::ORI).addReg(RISCV::X6).addReg(Reg).addImm(0xF),
        MCSTI);
    EmitToStreamer(
        *OutStreamer,
        MCInstBuilder(RISCV::LBU).addReg(RISCV::X6).addReg(RISCV::X6).addImm(0),
        MCSTI);
    EmitToStreamer(*OutStreamer,
                   MCInstBuilder(RISCV::BEQ)
                       .addReg(RISCV::X6)
                       .addReg(RISCV::X7)
                       .addExpr(MCSymbolRefExpr::create(ReturnSym, OutContext)),
                   MCSTI);

    OutStreamer->emitLabel(HandleMismatchSym);

    // | Previous stack frames...        |
    // +=================================+ <-- [SP + 256]
    // |              ...                |
    // |                                 |
    // | Stack frame space for x12 - x31.|
    // |                                 |
    // |              ...                |
    // +---------------------------------+ <-- [SP + 96]
    // | Saved x11(arg1), as             |
    // | __hwasan_check_* clobbers it.   |
    // +---------------------------------+ <-- [SP + 88]
    // | Saved x10(arg0), as             |
    // | __hwasan_check_* clobbers it.   |
    // +---------------------------------+ <-- [SP + 80]
    // |                                 |
    // | Stack frame space for x9.       |
    // +---------------------------------+ <-- [SP + 72]
    // |                                 |
    // | Saved x8(fp), as                |
    // | __hwasan_check_* clobbers it.   |
    // +---------------------------------+ <-- [SP + 64]
    // |              ...                |
    // |                                 |
    // | Stack frame space for x2 - x7.  |
    // |                                 |
    // |              ...                |
    // +---------------------------------+ <-- [SP + 16]
    // | Return address (x1) for caller  |
    // | of __hwasan_check_*.            |
    // +---------------------------------+ <-- [SP + 8]
    // | Reserved place for x0, possibly |
    // | junk, since we don't save it.   |
    // +---------------------------------+ <-- [x2 / SP]

    // Adjust sp
    EmitToStreamer(*OutStreamer,
                   MCInstBuilder(RISCV::ADDI)
                       .addReg(RISCV::X2)
                       .addReg(RISCV::X2)
                       .addImm(-256),
                   MCSTI);

    // store x10(arg0) by new sp
    EmitToStreamer(*OutStreamer,
                   MCInstBuilder(RISCV::SD)
                       .addReg(RISCV::X10)
                       .addReg(RISCV::X2)
                       .addImm(8 * 10),
                   MCSTI);
    // store x11(arg1) by new sp
    EmitToStreamer(*OutStreamer,
                   MCInstBuilder(RISCV::SD)
                       .addReg(RISCV::X11)
                       .addReg(RISCV::X2)
                       .addImm(8 * 11),
                   MCSTI);

    // store x8(fp) by new sp
    EmitToStreamer(
        *OutStreamer,
        MCInstBuilder(RISCV::SD).addReg(RISCV::X8).addReg(RISCV::X2).addImm(8 *
                                                                            8),
        MCSTI);
    // store x1(ra) by new sp
    EmitToStreamer(
        *OutStreamer,
        MCInstBuilder(RISCV::SD).addReg(RISCV::X1).addReg(RISCV::X2).addImm(1 *
                                                                            8),
        MCSTI);
    if (Reg != RISCV::X10)
      EmitToStreamer(
          *OutStreamer,
          MCInstBuilder(RISCV::ADDI).addReg(RISCV::X10).addReg(Reg).addImm(0),
          MCSTI);
    EmitToStreamer(*OutStreamer,
                   MCInstBuilder(RISCV::ADDI)
                       .addReg(RISCV::X11)
                       .addReg(RISCV::X0)
                       .addImm(AccessInfo & HWASanAccessInfo::RuntimeMask),
                   MCSTI);

    EmitToStreamer(*OutStreamer, MCInstBuilder(RISCV::PseudoCALL).addExpr(Expr),
                   MCSTI);
  }
}

void RISCVAsmPrinter::emitNoteGnuProperty(const Module &M) {
  assert(TM.getTargetTriple().isOSBinFormatELF() && "invalid binary format");
  uint32_t GnuProps = 0;
  if (const Metadata *const Flag = M.getModuleFlag("cf-protection-return");
      Flag && !mdconst::extract<ConstantInt>(Flag)->isZero())
    GnuProps |= ELF::GNU_PROPERTY_RISCV_FEATURE_1_CFI_SS;

  if (const Metadata *const Flag = M.getModuleFlag("cf-protection-branch");
      Flag && !mdconst::extract<ConstantInt>(Flag)->isZero()) {
    using namespace llvm::RISCVISAUtils;
    const Metadata *const CFBranchLabelSchemeFlag =
        M.getModuleFlag("cf-branch-label-scheme");
    assert(CFBranchLabelSchemeFlag &&
           "cf-protection=branch should come with cf-branch-label-scheme=... "
           "on RISC-V targets");
    const StringRef CFBranchLabelScheme =
        cast<MDString>(CFBranchLabelSchemeFlag)->getString();
    switch (llvm::RISCVCFI::getZicfilpLabelScheme(CFBranchLabelScheme)) {
    case llvm::RISCVCFI::ZicfilpLabelSchemeKind::Invalid:
      reportFatalInternalError("invalid RISC-V Zicfilp label scheme");
    case llvm::RISCVCFI::ZicfilpLabelSchemeKind::Unlabeled:
      GnuProps |= ELF::GNU_PROPERTY_RISCV_FEATURE_1_CFI_LP_UNLABELED;
      break;
    case llvm::RISCVCFI::ZicfilpLabelSchemeKind::FuncSig:
      // TODO: Emit the func-sig bit after the feature is implemented
      reportFatalUsageError("the complete func-sig label scheme feature is not "
                            "implemented yet");
      break;
    }
  }

  if (!GnuProps)
    return;

  auto &RTS = static_cast<RISCVTargetELFStreamer &>(getTargetStreamer());
  RTS.emitNoteGnuPropertySection(GnuProps);
}

static MCOperand lowerSymbolOperand(const MachineOperand &MO, MCSymbol *Sym,
                                    const AsmPrinter &AP) {
  MCContext &Ctx = AP.OutContext;
  RISCV::Specifier Kind;

  switch (MO.getTargetFlags()) {
  default:
    llvm_unreachable("Unknown target flag on GV operand");
  case RISCVII::MO_None:
    Kind = RISCV::S_None;
    break;
  case RISCVII::MO_CALL:
    Kind = RISCV::S_CALL_PLT;
    break;
  case RISCVII::MO_LO:
    Kind = RISCV::S_LO;
    break;
  case RISCVII::MO_HI:
    Kind = ELF::R_RISCV_HI20;
    break;
  case RISCVII::MO_PCREL_LO:
    Kind = RISCV::S_PCREL_LO;
    break;
  case RISCVII::MO_PCREL_HI:
    Kind = RISCV::S_PCREL_HI;
    break;
  case RISCVII::MO_GOT_HI:
    Kind = RISCV::S_GOT_HI;
    break;
  case RISCVII::MO_TPREL_LO:
    Kind = RISCV::S_TPREL_LO;
    break;
  case RISCVII::MO_TPREL_HI:
    Kind = ELF::R_RISCV_TPREL_HI20;
    break;
  case RISCVII::MO_TPREL_ADD:
    Kind = ELF::R_RISCV_TPREL_ADD;
    break;
  case RISCVII::MO_TLS_GOT_HI:
    Kind = ELF::R_RISCV_TLS_GOT_HI20;
    break;
  case RISCVII::MO_TLS_GD_HI:
    Kind = ELF::R_RISCV_TLS_GD_HI20;
    break;
  case RISCVII::MO_TLSDESC_HI:
    Kind = ELF::R_RISCV_TLSDESC_HI20;
    break;
  case RISCVII::MO_TLSDESC_LOAD_LO:
    Kind = ELF::R_RISCV_TLSDESC_LOAD_LO12;
    break;
  case RISCVII::MO_TLSDESC_ADD_LO:
    Kind = ELF::R_RISCV_TLSDESC_ADD_LO12;
    break;
  case RISCVII::MO_TLSDESC_CALL:
    Kind = ELF::R_RISCV_TLSDESC_CALL;
    break;
  case RISCVII::MO_QC_ACCESS:
    Kind = RISCV::S_QC_ACCESS;
    break;
  }

  const MCExpr *ME = MCSymbolRefExpr::create(Sym, Ctx);

  if (!MO.isJTI() && !MO.isMBB() && MO.getOffset())
    ME = MCBinaryExpr::createAdd(
        ME, MCConstantExpr::create(MO.getOffset(), Ctx), Ctx);

  if (Kind != RISCV::S_None)
    ME = MCSpecifierExpr::create(ME, Kind, Ctx);
  return MCOperand::createExpr(ME);
}

bool RISCVAsmPrinter::lowerOperand(const MachineOperand &MO,
                                   MCOperand &MCOp) const {
  switch (MO.getType()) {
  default:
    report_fatal_error("lowerOperand: unknown operand type");
  case MachineOperand::MO_Register:
    // Ignore all implicit register operands.
    if (MO.isImplicit())
      return false;
    MCOp = MCOperand::createReg(MO.getReg());
    break;
  case MachineOperand::MO_RegisterMask:
    // Regmasks are like implicit defs.
    return false;
  case MachineOperand::MO_Immediate:
    MCOp = MCOperand::createImm(MO.getImm());
    break;
  case MachineOperand::MO_MachineBasicBlock:
    MCOp = lowerSymbolOperand(MO, MO.getMBB()->getSymbol(), *this);
    break;
  case MachineOperand::MO_GlobalAddress:
    MCOp = lowerSymbolOperand(MO, getSymbolPreferLocal(*MO.getGlobal()), *this);
    break;
  case MachineOperand::MO_BlockAddress:
    MCOp = lowerSymbolOperand(MO, GetBlockAddressSymbol(MO.getBlockAddress()),
                              *this);
    break;
  case MachineOperand::MO_ExternalSymbol:
    MCOp = lowerSymbolOperand(MO, GetExternalSymbolSymbol(MO.getSymbolName()),
                              *this);
    break;
  case MachineOperand::MO_ConstantPoolIndex:
    MCOp = lowerSymbolOperand(MO, GetCPISymbol(MO.getIndex()), *this);
    break;
  case MachineOperand::MO_JumpTableIndex:
    MCOp = lowerSymbolOperand(MO, GetJTISymbol(MO.getIndex()), *this);
    break;
  case MachineOperand::MO_MCSymbol:
    MCOp = lowerSymbolOperand(MO, MO.getMCSymbol(), *this);
    break;
  }
  return true;
}

static bool lowerRISCVVMachineInstrToMCInst(const MachineInstr *MI,
                                            MCInst &OutMI,
                                            const RISCVSubtarget *STI) {
  const RISCVVPseudosTable::PseudoInfo *RVV =
      RISCVVPseudosTable::getPseudoInfo(MI->getOpcode());
  if (!RVV)
    return false;

  OutMI.setOpcode(RVV->BaseInstr);

  const TargetInstrInfo *TII = STI->getInstrInfo();
  const TargetRegisterInfo *TRI = STI->getRegisterInfo();
  assert(TRI && "TargetRegisterInfo expected");

  const MCInstrDesc &MCID = MI->getDesc();
  uint64_t TSFlags = MCID.TSFlags;
  unsigned NumOps = MI->getNumExplicitOperands();

  // Skip policy, SEW, VL, VXRM/FRM operands which are the last operands if
  // present.
  if (RISCVII::hasVecPolicyOp(TSFlags))
    --NumOps;
  if (RISCVII::hasSEWOp(TSFlags))
    --NumOps;
  if (RISCVII::hasVLOp(TSFlags))
    --NumOps;
  if (RISCVII::hasRoundModeOp(TSFlags))
    --NumOps;
  if (RISCVII::hasTWidenOp(TSFlags))
    --NumOps;
  if (RISCVII::hasTMOp(TSFlags))
    --NumOps;
  if (RISCVII::hasTKOp(TSFlags))
    --NumOps;

  bool hasVLOutput = RISCVInstrInfo::isFaultOnlyFirstLoad(*MI);
  for (unsigned OpNo = 0; OpNo != NumOps; ++OpNo) {
    const MachineOperand &MO = MI->getOperand(OpNo);
    // Skip vl output. It should be the second output.
    if (hasVLOutput && OpNo == 1)
      continue;

    // Skip passthru op. It should be the first operand after the defs.
    if (OpNo == MI->getNumExplicitDefs() && MO.isReg() && MO.isTied()) {
      assert(MCID.getOperandConstraint(OpNo, MCOI::TIED_TO) == 0 &&
             "Expected tied to first def.");
      const MCInstrDesc &OutMCID = TII->get(OutMI.getOpcode());
      // Skip if the next operand in OutMI is not supposed to be tied. Unless it
      // is a _TIED instruction.
      if (OutMCID.getOperandConstraint(OutMI.getNumOperands(), MCOI::TIED_TO) <
              0 &&
          !RISCVII::isTiedPseudo(TSFlags))
        continue;
    }

    MCOperand MCOp;
    switch (MO.getType()) {
    default:
      llvm_unreachable("Unknown operand type");
    case MachineOperand::MO_Register: {
      Register Reg = MO.getReg();

      if (RISCV::VRM2RegClass.contains(Reg) ||
          RISCV::VRM4RegClass.contains(Reg) ||
          RISCV::VRM8RegClass.contains(Reg)) {
        Reg = TRI->getSubReg(Reg, RISCV::sub_vrm1_0);
        assert(Reg && "Subregister does not exist");
      } else if (RISCV::FPR16RegClass.contains(Reg)) {
        Reg =
            TRI->getMatchingSuperReg(Reg, RISCV::sub_16, &RISCV::FPR32RegClass);
        assert(Reg && "Subregister does not exist");
      } else if (RISCV::FPR64RegClass.contains(Reg)) {
        Reg = TRI->getSubReg(Reg, RISCV::sub_32);
        assert(Reg && "Superregister does not exist");
      } else if (RISCV::VRN2M1RegClass.contains(Reg) ||
                 RISCV::VRN2M2RegClass.contains(Reg) ||
                 RISCV::VRN2M4RegClass.contains(Reg) ||
                 RISCV::VRN3M1RegClass.contains(Reg) ||
                 RISCV::VRN3M2RegClass.contains(Reg) ||
                 RISCV::VRN4M1RegClass.contains(Reg) ||
                 RISCV::VRN4M2RegClass.contains(Reg) ||
                 RISCV::VRN5M1RegClass.contains(Reg) ||
                 RISCV::VRN6M1RegClass.contains(Reg) ||
                 RISCV::VRN7M1RegClass.contains(Reg) ||
                 RISCV::VRN8M1RegClass.contains(Reg)) {
        Reg = TRI->getSubReg(Reg, RISCV::sub_vrm1_0);
        assert(Reg && "Subregister does not exist");
      }

      MCOp = MCOperand::createReg(Reg);
      break;
    }
    case MachineOperand::MO_Immediate:
      MCOp = MCOperand::createImm(MO.getImm());
      break;
    }
    OutMI.addOperand(MCOp);
  }

  // Unmasked pseudo instructions need to append dummy mask operand to
  // V instructions. All V instructions are modeled as the masked version.
  const MCInstrDesc &OutMCID = TII->get(OutMI.getOpcode());
  if (OutMI.getNumOperands() < OutMCID.getNumOperands()) {
    assert(OutMCID.operands()[OutMI.getNumOperands()].OperandType ==
               RISCVOp::OPERAND_VMASK &&
           "Expected only mask operand to be missing");
    OutMI.addOperand(MCOperand::createReg(RISCV::NoRegister));
  }

  assert(OutMI.getNumOperands() == OutMCID.getNumOperands());
  return true;
}

// <NT> MI → MCInst 默认 lowering 路径:
//   lowerToMCInst 是 MI -> MCInst 的"普通路径". 关键机制:
//     1) 先尝试 RVV 向量快速路径: lowerRISCVVMachineInstrToMCInst, 把 RVV
//        pseudos (带 AVL/SEW/LMUL/TA/MA) 折叠到 32/48 位真实指令, 避免
//        在普通路径里逐 MO 转换.
//     2) 快速路径不命中则 setOpcode + 逐 MO 调用 lowerOperand: 把
//        MachineOperand (Reg / Imm / Global / Block / JumpTable / CPI) 转成
//        MCOperand. 符号类操作数走 lowerSymbolOperand, 根据 modifier (如
//        CALL_PLT / GOT) 选 RISCV::Specifier, 让 AsmBackend 生成正确的
//        R_RISCV_* 重定位.
//   上游: AsmPrinter::emitInstruction 的默认分支 / OutStreamer->emitInstruction;
//   下游: RISCVMCCodeEmitter 接收 OutMI. 失败模式: 新加的 GlobalAddress
//   modifier 没在 lowerSymbolOperand 注册, 会退化成普通符号引用, 让
//   链接阶段无法解析.
void RISCVAsmPrinter::lowerToMCInst(const MachineInstr *MI, MCInst &OutMI) {
  if (lowerRISCVVMachineInstrToMCInst(MI, OutMI, STI))
    return;

  OutMI.setOpcode(MI->getOpcode());

  for (const MachineOperand &MO : MI->operands()) {
    MCOperand MCOp;
    if (lowerOperand(MO, MCOp))
      OutMI.addOperand(MCOp);
  }
}

void RISCVAsmPrinter::emitMachineConstantPoolValue(
    MachineConstantPoolValue *MCPV) {
  auto *RCPV = static_cast<RISCVConstantPoolValue *>(MCPV);
  MCSymbol *MCSym;

  if (RCPV->isGlobalValue()) {
    auto *GV = RCPV->getGlobalValue();
    MCSym = getSymbol(GV);
  } else {
    assert(RCPV->isExtSymbol() && "unrecognized constant pool type");
    auto Sym = RCPV->getSymbol();
    MCSym = GetExternalSymbolSymbol(Sym);
  }

  const MCExpr *Expr = MCSymbolRefExpr::create(MCSym, OutContext);
  uint64_t Size = getDataLayout().getTypeAllocSize(RCPV->getType());
  OutStreamer->emitValue(Expr, Size);
}

MaybeAlign
RISCVAsmPrinter::getRequiredGlobalAlignmentGranule(const GlobalVariable &GV) {
  const MCSubtargetInfo &MCSTI = TM.getMCSubtargetInfo();
  if (!GV.getValueType()->isSized())
    return std::nullopt;

  uint64_t Size = GV.getGlobalSize(getDataLayout());
  if (MCSTI.hasFeature(RISCV::FeatureVendorXCheriot))
    return CHERIoTCapabilityFormat::getRequiredAlignment(Size);

  if (MCSTI.hasFeature(RISCV::FeatureStdExtY)) {
    if (MCSTI.hasFeature(RISCV::Feature64Bit))
      return RV64YCapabilityFormat::getRequiredAlignment(Size);
    else
      return RV32YCapabilityFormat::getRequiredAlignment(Size);
  }

  return std::nullopt;
}

char RISCVAsmPrinter::ID = 0;

INITIALIZE_PASS(RISCVAsmPrinter, "riscv-asm-printer", "RISC-V Assembly Printer",
                false, false)

PreservedAnalyses RISCVAsmPrinterBeginPass::run(Module &M,
                                                ModuleAnalysisManager &MAM) {
  RISCVAsmPrinter &AsmPrinter = static_cast<RISCVAsmPrinter &>(
      MAM.getResult<AsmPrinterAnalysis>(M).getPrinter());
  setupModuleAsmPrinter(M, MAM, AsmPrinter);
  AsmPrinter.doInitialization(M);
  return PreservedAnalyses::all();
}

PreservedAnalyses
RISCVAsmPrinterPass::run(MachineFunction &MF,
                         MachineFunctionAnalysisManager &MFAM) {
  RISCVAsmPrinter &AsmPrinter = static_cast<RISCVAsmPrinter &>(
      MFAM.getResult<ModuleAnalysisManagerMachineFunctionProxy>(MF)
          .getCachedResult<AsmPrinterAnalysis>(*MF.getFunction().getParent())
          ->getPrinter());
  setupMachineFunctionAsmPrinter(MFAM, MF, AsmPrinter);
  AsmPrinter.runOnMachineFunction(MF);
  return PreservedAnalyses::all();
}

PreservedAnalyses RISCVAsmPrinterEndPass::run(Module &M,
                                              ModuleAnalysisManager &MAM) {
  RISCVAsmPrinter &AsmPrinter = static_cast<RISCVAsmPrinter &>(
      MAM.getResult<AsmPrinterAnalysis>(M).getPrinter());
  setupModuleAsmPrinter(M, MAM, AsmPrinter);
  AsmPrinter.doFinalization(M);
  return PreservedAnalyses::all();
}
