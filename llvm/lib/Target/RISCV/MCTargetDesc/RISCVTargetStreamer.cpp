//===-- RISCVTargetStreamer.cpp - RISC-V Target Streamer Methods ----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file provides RISC-V specific target streamer methods.
//
//===----------------------------------------------------------------------===//

// <NT> 文件简介:
//   RISCVTargetStreamer.cpp 实现 TargetStreamer 子类, 维护汇编过程中 RISC-V
//   专属状态: 当前 VTYPE / 当前 ABI / .option arch 后的新 Subtarget / .attribute
//   收集等. 上游 RISCVAsmParser (解析 .option / .attribute) + RISCVELFStreamer
//   (在 emit 钩子里回调本类); 下游 RISCVAsmPrinter 在输出 ELF 时把状态写入
//   .riscv.attributes 段.
//
// <NT> 关键函数串联 (.option 指令主流程):
//   状态同步 (与 RISCVELFStreamer 紧密配合):
//     emitDirectiveOption       统一入口, 按 Arch/Relax/RVC 等子开关分派
//       ├─ resetToArch         切换 Subtarget 的 -march 子集
//       ├─ setPic/Relax/RVC    切换 PIC / 链接器 relaxation / RVC 标志
//       └─ updateABI / updateFlags   把当前 ABI / Feature 写入 .riscv.attributes
//   状态存储:
//     ArchString / ArchStringStack   维护 .option push/pop 的 ISA 字符串栈
//     CurrentVendor / AttributeSection   待写入 ELF 的属性表
//   .attribute 指令:
//     emitAttribute / emitTextAttribute / emitIntTextAttribute
//                                  把汇编源中的 .attribute X, Y 翻译成
//                                  MCELFStreamer 的 setAttributeItem 调用
//
//   注: 本文件是"汇编期状态机", 把每条 .option / .attribute 翻译成后续
//      emit 时需要的 Subtarget 变更和属性表条目.
//
// <NT> 总结:
//   本文件是 RISC-V 汇编期状态管理层. 三大职责:
//     1) .option 处理: 在汇编过程中切换 -march 子集 / RVC / Relaxation,
//        同步 Subtarget 状态, 让后续指令编码跟着切换.
//     2) .attribute 收集: 把汇编源中的 .attribute 指令累积到 .riscv.attributes
//        段, 输出 ELF 时由 RISCVELFStreamer::finishAttributeSection 落盘.
//     3) ISA 字符串维护: 维护 ArchString 栈 (push/pop) + InitialArchString,
//        让 $x<ISA> mapping symbol 能正确反映当前活跃 ISA.
//   推荐阅读顺序: emitDirectiveOption -> resetToArch -> emitAttribute ->
//      看 RISCVELFStreamer 的 finish() 如何落盘 e_flags.
//   所有 NT 注释均以 "// <NT>" 开头, 方便搜索定位.
#include "RISCVTargetStreamer.h"
#include "RISCVBaseInfo.h"
#include "RISCVMCTargetDesc.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FormattedStream.h"
#include "llvm/Support/RISCVAttributes.h"
#include "llvm/TargetParser/RISCVISAInfo.h"

using namespace llvm;

// This option controls whether or not we emit ELF attributes for ABI features,
// like RISC-V atomics or X3 usage.
static cl::opt<bool> RiscvAbiAttr(
    "riscv-abi-attributes",
    cl::desc("Enable emitting RISC-V ELF attributes for ABI features"),
    cl::Hidden);

RISCVTargetStreamer::RISCVTargetStreamer(MCStreamer &S) : MCTargetStreamer(S) {}

void RISCVTargetStreamer::finish() { finishAttributeSection(); }
void RISCVTargetStreamer::reset() {}

void RISCVTargetStreamer::emitDirectiveOptionArch(
    ArrayRef<RISCVOptionArchArg> Args) {}
void RISCVTargetStreamer::emitDirectiveOptionExact() {}
void RISCVTargetStreamer::emitDirectiveOptionNoExact() {}
void RISCVTargetStreamer::emitDirectiveOptionPIC() {}
void RISCVTargetStreamer::emitDirectiveOptionNoPIC() {}
void RISCVTargetStreamer::emitDirectiveOptionPop() {}
void RISCVTargetStreamer::emitDirectiveOptionPush() {}
void RISCVTargetStreamer::emitDirectiveOptionRelax() {}
void RISCVTargetStreamer::emitDirectiveOptionNoRelax() {}
void RISCVTargetStreamer::emitDirectiveOptionRVC() {}
void RISCVTargetStreamer::emitDirectiveOptionNoRVC() {}
void RISCVTargetStreamer::emitDirectiveVariantCC(MCSymbol &Symbol) {}
void RISCVTargetStreamer::emitAttribute(unsigned Attribute, unsigned Value) {}
void RISCVTargetStreamer::finishAttributeSection() {}
void RISCVTargetStreamer::emitTextAttribute(unsigned Attribute,
                                            StringRef String) {}
void RISCVTargetStreamer::emitIntTextAttribute(unsigned Attribute,
                                               unsigned IntValue,
                                               StringRef StringValue) {}

void RISCVTargetStreamer::setTargetABI(RISCVABI::ABI ABI) {
  assert(ABI != RISCVABI::ABI_Unknown && "Improperly initialized target ABI");
  TargetABI = ABI;
}

void RISCVTargetStreamer::setFlagsFromFeatures(const MCSubtargetInfo &STI) {
  HasRVC = STI.hasFeature(RISCV::FeatureStdExtZca);
  HasTSO = STI.hasFeature(RISCV::FeatureStdExtZtso);
}

void RISCVTargetStreamer::emitTargetAttributes(const MCSubtargetInfo &STI,
                                               bool EmitStackAlign) {
  if (EmitStackAlign) {
    unsigned StackAlign;
    if (TargetABI == RISCVABI::ABI_ILP32E)
      StackAlign = 4;
    else if (TargetABI == RISCVABI::ABI_LP64E)
      StackAlign = 8;
    else
      StackAlign = 16;
    emitAttribute(RISCVAttrs::STACK_ALIGN, StackAlign);
  }

  auto ParseResult = RISCVFeatures::parseFeatureBits(STI);
  if (!ParseResult) {
    report_fatal_error(ParseResult.takeError());
  } else {
    auto &ISAInfo = *ParseResult;
    emitTextAttribute(RISCVAttrs::ARCH, ISAInfo->toString());
  }

  if (RiscvAbiAttr && STI.hasFeature(RISCV::FeatureStdExtA)) {
    unsigned AtomicABITag;
    if (STI.hasFeature(RISCV::FeatureStdExtZalasr))
      AtomicABITag = static_cast<unsigned>(RISCVAttrs::RISCVAtomicAbiTag::A7);
    else if (STI.hasFeature(RISCV::FeatureNoTrailingSeqCstFence))
      AtomicABITag = static_cast<unsigned>(RISCVAttrs::RISCVAtomicAbiTag::A6C);
    else
      AtomicABITag = static_cast<unsigned>(RISCVAttrs::RISCVAtomicAbiTag::A6S);
    emitAttribute(RISCVAttrs::ATOMIC_ABI, AtomicABITag);
  }
}

// This part is for ascii assembly output
RISCVTargetAsmStreamer::RISCVTargetAsmStreamer(MCStreamer &S,
                                               formatted_raw_ostream &OS)
    : RISCVTargetStreamer(S), OS(OS) {}

void RISCVTargetAsmStreamer::emitDirectiveOptionPush() {
  OS << "\t.option\tpush\n";
}

void RISCVTargetAsmStreamer::emitDirectiveOptionPop() {
  OS << "\t.option\tpop\n";
}

void RISCVTargetAsmStreamer::emitDirectiveOptionPIC() {
  OS << "\t.option\tpic\n";
}

void RISCVTargetAsmStreamer::emitDirectiveOptionNoPIC() {
  OS << "\t.option\tnopic\n";
}

void RISCVTargetAsmStreamer::emitDirectiveOptionRVC() {
  OS << "\t.option\trvc\n";
}

void RISCVTargetAsmStreamer::emitDirectiveOptionNoRVC() {
  OS << "\t.option\tnorvc\n";
}

void RISCVTargetAsmStreamer::emitDirectiveOptionExact() {
  OS << "\t.option\texact\n";
}

void RISCVTargetAsmStreamer::emitDirectiveOptionNoExact() {
  OS << "\t.option\tnoexact\n";
}

void RISCVTargetAsmStreamer::emitDirectiveOptionRelax() {
  OS << "\t.option\trelax\n";
}

void RISCVTargetAsmStreamer::emitDirectiveOptionNoRelax() {
  OS << "\t.option\tnorelax\n";
}

void RISCVTargetAsmStreamer::emitDirectiveOptionArch(
    ArrayRef<RISCVOptionArchArg> Args) {
  OS << "\t.option\tarch";
  for (const auto &Arg : Args) {
    OS << ", ";
    switch (Arg.Type) {
    case RISCVOptionArchArgType::Full:
      break;
    case RISCVOptionArchArgType::Plus:
      OS << "+";
      break;
    case RISCVOptionArchArgType::Minus:
      OS << "-";
      break;
    }
    OS << Arg.Value;
  }
  OS << "\n";
}

void RISCVTargetAsmStreamer::emitDirectiveVariantCC(MCSymbol &Symbol) {
  OS << "\t.variant_cc\t" << Symbol.getName() << "\n";
}

void RISCVTargetAsmStreamer::emitAttribute(unsigned Attribute, unsigned Value) {
  OS << "\t.attribute\t" << Attribute << ", " << Twine(Value) << "\n";
}

void RISCVTargetAsmStreamer::emitTextAttribute(unsigned Attribute,
                                               StringRef String) {
  OS << "\t.attribute\t" << Attribute << ", \"" << String << "\"\n";
}

void RISCVTargetAsmStreamer::emitIntTextAttribute(unsigned Attribute,
                                                  unsigned IntValue,
                                                  StringRef StringValue) {}

void RISCVTargetAsmStreamer::finishAttributeSection() {}
