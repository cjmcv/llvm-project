//===-- RISCVMCExpr.cpp - RISC-V specific MC expression classes -----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains the implementation of the assembly expression modifiers
// accepted by the RISC-V architecture (e.g. ":lo12:", ":gottprel_g1:", ...).
//
//===----------------------------------------------------------------------===//

// <NT> 文件简介:
//   RISCVMCExpr.cpp 实现 RISCV 专属 MCExpr 子类 (RISCVMCExpr), 承载汇编
//   修饰符 %lo/%hi/%pcrel_hi/%tprel_lo 等的语义与 VariantKind 枚举. 上游
//   RISCVAsmParser (解析 %lo(%sym) 等表达式) 和 RISCVMCAsmInfo (printSpecifierExpr);
//   下游 RISCVMCCodeEmitter (读 VariantKind 决定生成哪个 R_RISCV_* Fixup).
//   文件极短 (≈80 行), 是表达式修饰符层与二进制编码层之间的"翻译表".
//
// <NT> 关键函数串联:
//   工厂入口:
//     parseSpecifierName     把字符串 (lo/hi/pcrel_lo/...) 翻译成 Specifier 枚举
//   关键方法:
//     fixupForSpecifier      反向映射: Specifier -> RISCVFixupKinds (给 CodeEmitter 用)
//     evaluateAsRelocatable  把 RISCVMCExpr 折叠成最终地址值, 同时决定 Fixup 类型
//     print                   (override MCExpr::print) 打印 %lo(%sym) 文本给汇编输出
//
//   注: 本文件无主入口, 核心是 Specifier 枚举的"双向字典":
//      字符串<->枚举<->FixupKind, 三者一一对应.
//
// <NT> 总结:
//   本文件是 RISC-V 表达式修饰符层. 三大职责:
//     1) 修饰符解析: 把汇编源中 %lo / %hi / %pcrel_hi 等字符串翻译成
//        强类型 Specifier 枚举, 防字符串错拼.
//     2) Fixup 映射: 在 MCCodeEmitter 阶段把 Specifier 翻成 RISCVFixupKinds
//        枚举, 让 AsmBackend 生成正确的 R_RISCV_* 重定位.
//     3) 折叠计算: 在链接前把表达式折叠 (如 %hi(sym) = sym - sym_lo12) 并
//        把值交给 CodeEmitter 写入指令字段.
//   推荐阅读顺序: parseSpecifierName -> fixupForSpecifier -> evaluateAsRelocatable.
//   所有 NT 注释均以 "// <NT>" 开头, 方便搜索定位.
#include "MCTargetDesc/RISCVAsmBackend.h"
#include "MCTargetDesc/RISCVMCAsmInfo.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;

#define DEBUG_TYPE "riscvmcexpr"

RISCV::Specifier RISCV::parseSpecifierName(StringRef name) {
  return StringSwitch<RISCV::Specifier>(name)
      .Case("lo", RISCV::S_LO)
      .Case("hi", ELF::R_RISCV_HI20)
      .Case("pcrel_lo", RISCV::S_PCREL_LO)
      .Case("pcrel_hi", RISCV::S_PCREL_HI)
      .Case("got_pcrel_hi", RISCV::S_GOT_HI)
      .Case("tprel_lo", RISCV::S_TPREL_LO)
      .Case("tprel_hi", ELF::R_RISCV_TPREL_HI20)
      .Case("tprel_add", ELF::R_RISCV_TPREL_ADD)
      .Case("tls_ie_pcrel_hi", ELF::R_RISCV_TLS_GOT_HI20)
      .Case("tls_gd_pcrel_hi", ELF::R_RISCV_TLS_GD_HI20)
      .Case("tlsdesc_hi", ELF::R_RISCV_TLSDESC_HI20)
      .Case("tlsdesc_load_lo", ELF::R_RISCV_TLSDESC_LOAD_LO12)
      .Case("tlsdesc_add_lo", ELF::R_RISCV_TLSDESC_ADD_LO12)
      .Case("tlsdesc_call", ELF::R_RISCV_TLSDESC_CALL)
      .Case("qc.abs20", RISCV::S_QC_ABS20)
      .Case("qc.access", RISCV::S_QC_ACCESS)
      // Used in data directives
      .Case("pltpcrel", ELF::R_RISCV_PLT32)
      .Case("gotpcrel", ELF::R_RISCV_GOT32_PCREL)
      .Default(0);
}

StringRef RISCV::getSpecifierName(Specifier S) {
  switch (S) {
  case RISCV::S_None:
    llvm_unreachable("not used as %specifier()");
  case RISCV::S_LO:
    return "lo";
  case ELF::R_RISCV_HI20:
    return "hi";
  case RISCV::S_PCREL_LO:
    return "pcrel_lo";
  case RISCV::S_PCREL_HI:
    return "pcrel_hi";
  case RISCV::S_GOT_HI:
    return "got_pcrel_hi";
  case RISCV::S_TPREL_LO:
    return "tprel_lo";
  case ELF::R_RISCV_TPREL_HI20:
    return "tprel_hi";
  case ELF::R_RISCV_TPREL_ADD:
    return "tprel_add";
  case ELF::R_RISCV_TLS_GOT_HI20:
    return "tls_ie_pcrel_hi";
  case ELF::R_RISCV_TLSDESC_HI20:
    return "tlsdesc_hi";
  case ELF::R_RISCV_TLSDESC_LOAD_LO12:
    return "tlsdesc_load_lo";
  case ELF::R_RISCV_TLSDESC_ADD_LO12:
    return "tlsdesc_add_lo";
  case ELF::R_RISCV_TLSDESC_CALL:
    return "tlsdesc_call";
  case ELF::R_RISCV_TLS_GD_HI20:
    return "tls_gd_pcrel_hi";
  case RISCV::S_CALL_PLT:
    return "call_plt";
  case ELF::R_RISCV_32_PCREL:
    return "32_pcrel";
  case ELF::R_RISCV_GOT32_PCREL:
    return "gotpcrel";
  case ELF::R_RISCV_PLT32:
    return "pltpcrel";
  case RISCV::S_QC_ABS20:
    return "qc.abs20";
  case RISCV::S_QC_ACCESS:
    return "qc.access";
  }
  llvm_unreachable("Invalid ELF symbol kind");
}
