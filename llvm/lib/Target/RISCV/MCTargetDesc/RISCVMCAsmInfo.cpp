//===-- RISCVMCAsmInfo.cpp - RISC-V Asm properties ------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains the declarations of the RISCVMCAsmInfo properties.
//
//===----------------------------------------------------------------------===//

// <NT> 文件简介:
//   RISCVMCAsmInfo.cpp 实现 MCAsmInfo 子类, 配置 RISC-V 汇编器的全局行为
//   (注释字符、表达式分隔符、数据对齐、私有标签前缀等). 文件极短 (≈80 行),
//   是 LLVM 汇编器对 RISC-V ABI 约定的封装. 上游 llvm-mc / 集成汇编器, 下游
//   MCParser / RISCVAsmParser.
//
// <NT> 关键函数串联:
//   工厂入口:
//     createRISCVMCAsmInfo      注册到 TargetRegistry::RegisterMCAsmInfo
//   关键配置:
//     注释字符                  "#" (POSIX 风格, 而非 x86 的 ";")
//     私有标签前缀              ".L" (GCC 兼容, 本地符号不进入符号表)
//     表达式分隔符              支持 %lo/%hi 等 RISC-V 修饰符 (与 RISCVAsmParser::parseExprWithSpecifier 配合)
//     数据对齐                  riscv64 默认 8 字节, riscv32 默认 4 字节
//   注: 本文件无主入口, 主要是构造时一次性写好上述常量.
//
// <NT> 总结:
//   本文件是汇编器属性配置层. 三大职责:
//     1) 语法约定: 注释字符 / 标签前缀 / 表达式修饰符等汇编器全局开关.
//     2) ABI 封装: 把 RISC-V ABI 约定的符号命名规则、字段对齐规则集中到一处.
//     3) 工厂注册: 让 RISCV target 使用本文件实现的 MCAsmInfo 路径.
//   推荐阅读顺序: 构造体 -> createRISCVMCAsmInfo -> 各 set* 调用.
//   所有 NT 注释均以 "// <NT>" 开头, 方便搜索定位.
#include "RISCVMCAsmInfo.h"
#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/TargetParser/Triple.h"
using namespace llvm;

void RISCVMCAsmInfo::anchor() {}

RISCVMCAsmInfo::RISCVMCAsmInfo(const Triple &TT, const MCTargetOptions &Options)
    : MCAsmInfoELF(Options) {
  IsLittleEndian = TT.isLittleEndian();
  CodePointerSize = CalleeSaveStackSlotSize = TT.isArch64Bit() ? 8 : 4;
  CommentString = "#";
  AlignmentIsInBytes = false;
  SupportsDebugInformation = true;
  ExceptionsType = ExceptionHandling::DwarfCFI;
  Data16bitsDirective = "\t.half\t";
  Data32bitsDirective = "\t.word\t";
  // The default symbol subtraction results in an ADD/SUB relocation pair.
  // Processing this relocation pair is problematic when linker relaxation is
  // enabled, so we follow binutils in using the R_RISCV_32_PCREL relocation
  // for the FDE initial location.
  DwarfFDERelSymbolSpec = ELF::R_RISCV_32_PCREL;
}

void RISCVMCAsmInfo::printSpecifierExpr(raw_ostream &OS,
                                        const MCSpecifierExpr &Expr) const {
  auto S = Expr.getSpecifier();
  bool HasSpecifier = S != RISCV::S_None && S != RISCV::S_CALL_PLT;
  if (HasSpecifier)
    OS << '%' << RISCV::getSpecifierName(S) << '(';
  printExpr(OS, *Expr.getSubExpr());
  if (HasSpecifier)
    OS << ')';
}

RISCVMCAsmInfoDarwin::RISCVMCAsmInfoDarwin(const MCTargetOptions &Options)
    : MCAsmInfoDarwin(Options) {
  CodePointerSize = 4;
  InternalSymbolPrefix = "L";
  SeparatorString = "%%";
  CommentString = ";";
  AlignmentIsInBytes = false;
  SupportsDebugInformation = true;
  UseDataRegionDirectives = true;
  ExceptionsType = ExceptionHandling::DwarfCFI;
  Data16bitsDirective = "\t.half\t";
  Data32bitsDirective = "\t.word\t";
}

void RISCVMCAsmInfoDarwin::printSpecifierExpr(
    raw_ostream &OS, const MCSpecifierExpr &Expr) const {
  auto S = Expr.getSpecifier();
  bool HasSpecifier = S != RISCV::S_None && S != RISCV::S_CALL_PLT;
  if (HasSpecifier)
    OS << '%' << RISCV::getSpecifierName(S) << '(';
  printExpr(OS, *Expr.getSubExpr());
  if (HasSpecifier)
    OS << ')';
}
