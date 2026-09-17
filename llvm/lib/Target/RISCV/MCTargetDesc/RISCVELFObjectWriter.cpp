//===-- RISCVELFObjectWriter.cpp - RISC-V ELF Writer ----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// <NT> 文件简介:
//   RISCVELFObjectWriter.cpp 实现 ELF 对象文件写入器 (RISCV 专属标记层).
//   MCELFObjectWriter 子类, 在 MCAssembler 输出 ELF 时被调用, 负责
//   设置 e_flags / e_ident 等 RISC-V 专属字段. 上游 MCAssembler, 下游
//   ld.lld / GNU ld 等 RISC-V ELF 链接器.
//
// <NT> 关键函数串联:
//   工厂入口:
//     createRISCVELFObjectWriter   被 TargetRegistry::RegisterELFObjectWriter 注册
//   标记生成:
//     GetOSFlags (RISCVELFObjectWriter override)
//       ├─ EF_RISCV_RVC               压缩指令已用 (c.ext / c.addi 等)
//       ├─ EF_RISCV_RVE               RV32E 嵌入式精简 ISA
//       ├─ EF_RISCV_TSO               Total Store Ordering (替代 RVWMO 可选)
//       └─ EF_RISCV_FLOAT_ABI_*       FPR 调用约定 (soft / single / double / quad)
//   重定位支持:
//     getRelocType (override MCELFObjectWriter)
//       └─ 把 RISCVFixupKinds 映射到 R_RISCV_* ELF 重定位枚举.
//
// <NT> 总结:
//   本文件是 RISC-V ELF 输出层. 三大职责:
//     1) e_flags 设置: 把 RISC-V 专属特性 (RVC / RVE / TSO / Float ABI 等)
//        编码进 ELF e_flags, 让链接器和运行时识别.
//     2) 重定位类型映射: 把内部的 RISCVFixupKinds 枚举翻译为 ELF 标准
//        R_RISCV_* 重定位条目.
//     3) 工厂注册: 把本文件实现通过 RegisterELFObjectWriter 注入
//        TargetRegistry, 让 TargetRegistry 在 riscv32/64 target 下使用.
//   推荐阅读顺序: createRISCVELFObjectWriter -> GetOSFlags -> getRelocType.
//   所有 NT 注释均以 "// <NT>" 开头, 方便搜索定位.
#include "MCTargetDesc/RISCVFixupKinds.h"
#include "MCTargetDesc/RISCVMCAsmInfo.h"
#include "MCTargetDesc/RISCVMCTargetDesc.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCELFObjectWriter.h"
#include "llvm/MC/MCFixup.h"
#include "llvm/MC/MCObjectWriter.h"
#include "llvm/MC/MCValue.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;

namespace {
class RISCVELFObjectWriter : public MCELFObjectTargetWriter {
public:
  RISCVELFObjectWriter(uint8_t OSABI, bool Is64Bit);

  ~RISCVELFObjectWriter() override;

  // Return true if the given relocation must be with a symbol rather than
  // section plus offset.
  bool needsRelocateWithSymbol(const MCValue &, unsigned Type) const override {
    // TODO: this is very conservative, update once RISC-V psABI requirements
    //       are clarified.
    return true;
  }

protected:
  unsigned getRelocType(const MCFixup &, const MCValue &,
                        bool IsPCRel) const override;
};
}

RISCVELFObjectWriter::RISCVELFObjectWriter(uint8_t OSABI, bool Is64Bit)
    : MCELFObjectTargetWriter(Is64Bit, OSABI, ELF::EM_RISCV,
                              /*HasRelocationAddend*/ true) {}

RISCVELFObjectWriter::~RISCVELFObjectWriter() = default;

unsigned RISCVELFObjectWriter::getRelocType(const MCFixup &Fixup,
                                            const MCValue &Target,
                                            bool IsPCRel) const {
  auto Kind = Fixup.getKind();
  auto Spec = Target.getSpecifier();
  switch (Spec) {
  case ELF::R_RISCV_TPREL_HI20:
  case ELF::R_RISCV_TLS_GOT_HI20:
  case ELF::R_RISCV_TLS_GD_HI20:
  case ELF::R_RISCV_TLSDESC_HI20:
    if (auto *SA = const_cast<MCSymbol *>(Target.getAddSym()))
      static_cast<MCSymbolELF *>(SA)->setType(ELF::STT_TLS);
    break;
  case ELF::R_RISCV_PLT32:
  case ELF::R_RISCV_GOT32_PCREL:
    if (Kind == FK_Data_4)
      break;
    reportError(Fixup.getLoc(), "%" + RISCV::getSpecifierName(Spec) +
                                    " can only be used in a .word directive");
    return ELF::R_RISCV_NONE;
  default:
    break;
  }

  // Extract the relocation type from the fixup kind, after applying STT_TLS as
  // needed.
  if (mc::isRelocation(Fixup.getKind()))
    return Kind;

  if (IsPCRel) {
    switch (Kind) {
    default:
      reportError(Fixup.getLoc(), "unsupported relocation type");
      return ELF::R_RISCV_NONE;
    case FK_Data_4:
      return ELF::R_RISCV_32_PCREL;
    case RISCV::fixup_riscv_pcrel_hi20:
      return ELF::R_RISCV_PCREL_HI20;
    case RISCV::fixup_riscv_pcrel_lo12_i:
      return ELF::R_RISCV_PCREL_LO12_I;
    case RISCV::fixup_riscv_pcrel_lo12_s:
      return ELF::R_RISCV_PCREL_LO12_S;
    case RISCV::fixup_riscv_jal:
      return ELF::R_RISCV_JAL;
    case RISCV::fixup_riscv_branch:
      return ELF::R_RISCV_BRANCH;
    case RISCV::fixup_riscv_rvc_jump:
      return ELF::R_RISCV_RVC_JUMP;
    case RISCV::fixup_riscv_rvc_branch:
      return ELF::R_RISCV_RVC_BRANCH;
    case RISCV::fixup_riscv_call:
      return ELF::R_RISCV_CALL_PLT;
    case RISCV::fixup_riscv_call_plt:
      return ELF::R_RISCV_CALL_PLT;
    case RISCV::fixup_riscv_qc_e_branch:
      return ELF::R_RISCV_QC_E_BRANCH;
    case RISCV::fixup_riscv_qc_e_call_plt:
      return ELF::R_RISCV_QC_E_CALL_PLT;
    case RISCV::fixup_riscv_nds_branch_10:
      return ELF::R_RISCV_NDS_BRANCH_10;
    }
  }

  switch (Kind) {
  default:
    reportError(Fixup.getLoc(), "unsupported relocation type");
    return ELF::R_RISCV_NONE;

  case FK_Data_1:
    reportError(Fixup.getLoc(), "1-byte data relocations not supported");
    return ELF::R_RISCV_NONE;
  case FK_Data_2:
    reportError(Fixup.getLoc(), "2-byte data relocations not supported");
    return ELF::R_RISCV_NONE;
  case FK_Data_4:
    switch (Spec) {
    case ELF::R_RISCV_32_PCREL:
    case ELF::R_RISCV_GOT32_PCREL:
    case ELF::R_RISCV_PLT32:
      return Spec;
    }
    return ELF::R_RISCV_32;
  case FK_Data_8:
    return ELF::R_RISCV_64;
  case RISCV::fixup_riscv_hi20:
    return ELF::R_RISCV_HI20;
  case RISCV::fixup_riscv_lo12_i:
    return ELF::R_RISCV_LO12_I;
  case RISCV::fixup_riscv_lo12_s:
    return ELF::R_RISCV_LO12_S;
  case RISCV::fixup_riscv_rvc_imm:
    reportError(Fixup.getLoc(), "No relocation for CI-type instructions");
    return ELF::R_RISCV_NONE;
  case RISCV::fixup_riscv_qc_e_32:
    return ELF::R_RISCV_QC_E_32;
  case RISCV::fixup_riscv_qc_abs20_u:
    return ELF::R_RISCV_QC_ABS20_U;
  case RISCV::fixup_qc_access_16:
    return ELF::R_RISCV_QC_ACCESS_16;
  case RISCV::fixup_qc_access_32:
    return ELF::R_RISCV_QC_ACCESS_32;
  }
}

std::unique_ptr<MCObjectTargetWriter>
llvm::createRISCVELFObjectWriter(uint8_t OSABI, bool Is64Bit) {
  return std::make_unique<RISCVELFObjectWriter>(OSABI, Is64Bit);
}
