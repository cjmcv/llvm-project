//===-- RISCVDisassembler.cpp - Disassembler for RISC-V -------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the RISCVDisassembler class.
//
// <NT> 文件简介:
//   RISCVDisassembler.cpp 实现 RISC-V 反汇编器, 把二进制指令流反向解析成
//   MCInst + 助记符, 是 llvm-objdump -d / -D 等工具看到 RISC-V 反汇编
//   产物的来源. 上游由 TargetRegistry 在 LLVMInitializeRISCVDisassembler
//   注册 (4 个 target: riscv32/64/32be/64be), 下游把 MCInst 交给
//   RISCVInstPrinter 打印. 本文件处于 LLVM MC (Machine Code) 层.
//
// <NT> 关键函数串联 (单条指令反汇编的主流程):
//   getInstruction              顶层入口, 按指令长度 (2/4/6 字节) 分派
//     ├─ getInstruction32       32 位主路径, 走 DecoderList32 表
//     ├─ getInstruction16       16 位压缩指令 (RVC), 走 DecoderList16 表
//     └─ getInstruction48       48 位长指令 (如 RVV 配置), 走 DecoderList48 表
//          └─> decodeInstruction
//                └─> TableGen 生成的解码表逐项匹配, 每条指令格式
//                    会调用 DecoderMethod 指定的解码函数:
//                      ├─ DecodeSimpleRegisterClass   通用 GPR/FPR/VR
//                      ├─ DecodeGPRX1X5 / DecodeFilteredRegisterClass   子集
//                      ├─ DecodeVectorRegisterClass   RVV v0-v31 + 掩码
//                      ├─ decodeUImmOperand / decodeSImmOperand   立即数
//                      ├─ decodeUImmLog2XLenOperand   shamt (slli/srli 等)
//                      ├─ decodeSImmOperandAndLslN   立即数 + 左移 (栈调整)
//                      ├─ decodeCLUIImmOperand        C.LUI 立即数
//                      ├─ decodeImmZibiOperand        Zibi 编码 (clz/ctz)
//                      ├─ decodeVMaskReg              v0.t 掩码位
//                      └─ decodeFRMArg / decodeZcmpRlist / decodeYBNDSWImm
//                          浮点舍入模式 / 寄存器列表 / XTHead 边界
//
//   工厂与注册:
//     LLVMInitializeRISCVDisassembler
//       ├─ RegisterMCDisassembler (×4 targets)
//       └─ RegisterMCSymbolizer   (×4 targets)
//            └─ createRISCVDisassembler / createRISCVMCSymbolizer
//
// <NT> 总结:
//   本文件是 RISC-V 后端的反汇编侧. 三大职责:
//     1) 按指令长度 (2/4/6 字节) 分派解码路径, 走 TableGen 生成的
//        DecoderList 表 (DecoderList16/32/48).
//     2) 提供各寄存器类的解码器: 通用模板 DecodeSimpleRegisterClass +
//        子集特化 (X1/X5/SP/Pair/Vector/TRM 等) + 厂商扩展 (XTHead).
//     3) 提供各立即数 / 特殊域解码器: 含范围检查、特化编码
//        (UImm/SImm/Log2XLen/Plus1/CLUI/Zibi 等).
//   推荐阅读顺序: getInstruction -> getInstruction32 -> decodeInstruction
//     -> DecodeSimpleRegisterClass + decodeUImmOperand (主流程模板).
//   所有 NT 注释均以 "// <NT>" 开头, 方便搜索定位.
//
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/RISCVBaseInfo.h"
#include "MCTargetDesc/RISCVMCTargetDesc.h"
#include "TargetInfo/RISCVTargetInfo.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCDecoder.h"
#include "llvm/MC/MCDecoderOps.h"
#include "llvm/MC/MCDisassembler/MCDisassembler.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/Endian.h"

using namespace llvm;
using namespace llvm::MCD;

#define DEBUG_TYPE "riscv-disassembler"

typedef MCDisassembler::DecodeStatus DecodeStatus;

namespace {
class RISCVDisassembler : public MCDisassembler {
  std::unique_ptr<MCInstrInfo const> const MCII;

public:
  RISCVDisassembler(const MCSubtargetInfo &STI, MCContext &Ctx,
                    MCInstrInfo const *MCII)
      : MCDisassembler(STI, Ctx), MCII(MCII) {}

  DecodeStatus getInstruction(MCInst &Instr, uint64_t &Size,
                              ArrayRef<uint8_t> Bytes, uint64_t Address,
                              raw_ostream &CStream) const override;

private:
  DecodeStatus getInstruction48(MCInst &Instr, uint64_t &Size,
                                ArrayRef<uint8_t> Bytes, uint64_t Address,
                                raw_ostream &CStream) const;

  DecodeStatus getInstruction32(MCInst &Instr, uint64_t &Size,
                                ArrayRef<uint8_t> Bytes, uint64_t Address,
                                raw_ostream &CStream) const;
  DecodeStatus getInstruction16(MCInst &Instr, uint64_t &Size,
                                ArrayRef<uint8_t> Bytes, uint64_t Address,
                                raw_ostream &CStream) const;
};
} // end anonymous namespace

// <NT> 反汇编器工厂入口:
//   被 TargetRegistry::RegisterMCDisassembler 在 LLVMInitializeRISCVDisassembler
//   中注册, 给 riscv32/riscv64/riscv32be/riscv64be 四个 target 各注册一次.
//   调用方是 MCDisassembler::create<Target>(STI, Ctx) 工厂模式: 给定 Target
//   (含 createMCInstrInfo) + SubtargetInfo + MCContext, 在堆上 new 一个
//   RISCVDisassembler 实例并返回基类指针. 构造时把 MCInstrInfo 缓存为成员
//   (MCII), 反汇编时通过 MCII 查 opcode 的 MCInstrDesc. 创建失败/成功
//   仅由 new 决定, 此函数不抛错.
static MCDisassembler *createRISCVDisassembler(const Target &T,
                                               const MCSubtargetInfo &STI,
                                               MCContext &Ctx) {
  return new RISCVDisassembler(STI, Ctx, T.createMCInstrInfo());
}

// <NT> 符号化器工厂入口:
//   被 TargetRegistry::RegisterMCSymbolizer 注册, 在反汇编带 %hi/%lo
//   修饰符的指令时调用. 用途: 把 HI20/LO12 这种指令字段回填为带符号名的
//   表达式 (如 %hi(symbol)+4), 让反汇编输出更可读 (带名字, 不只是裸字节).
//   注意: RISC-V 仅用于 HI20/LO12 地址片段, 这些必须依赖重定位信息
//   (RelInfo), 不能脱离 RelInfo 解析为绝对地址, 因此把 SymbolLookup
//   留 null. GetOpInfo 是 LLVM 提供的回调, 用来问链接器某地址对应
//   哪个符号. 调用方是 llvm-objdump 等反汇编工具的内部流程.
static MCSymbolizer *
createRISCVMCSymbolizer(const Triple &TT, LLVMOpInfoCallback GetOpInfo,
                        LLVMSymbolLookupCallback /*SymbolLookUp*/,
                        void *DisInfo, MCContext *Ctx,
                        std::unique_ptr<MCRelocationInfo> &&RelInfo) {
  // RISC-V only asks MCSymbolizer to decode HI20/LO12 address fragments. They
  // require relocation information and cannot be looked up as absolute
  // addresses when GetOpInfo fails.
  return llvm::createMCSymbolizer(TT, GetOpInfo, /*SymbolLookUp=*/nullptr,
                                  DisInfo, Ctx, std::move(RelInfo));
}

// <NT> 反汇编器全局注册入口 (extern "C", 由 LLVMInitialize* 调度):
//   把 createRISCVDisassembler 和 createRISCVMCSymbolizer 注册到
//   TargetRegistry 的 4 个 RISC-V target 上: riscv32 / riscv64
//   (小端, RV32I/RV64I 默认) 和 riscv32be / riscv64be (大端).
//   extern "C" 是为了让 LLVM 插件机制 / dlsym 能找到这个符号
//   (LLVMInitialize<TargetName>Disassembler 是约定的初始化符号名).
//   被动态库加载器或 llvm-mc / llvm-objdump 等工具启动时调用, 是
//   整个 RISC-V 反汇编能力对外暴露的总闸口.
extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void
LLVMInitializeRISCVDisassembler() {
  // Register the disassembler for each target.
  TargetRegistry::RegisterMCDisassembler(getTheRISCV32Target(),
                                         createRISCVDisassembler);
  TargetRegistry::RegisterMCSymbolizer(getTheRISCV32Target(),
                                       createRISCVMCSymbolizer);
  TargetRegistry::RegisterMCDisassembler(getTheRISCV64Target(),
                                         createRISCVDisassembler);
  TargetRegistry::RegisterMCSymbolizer(getTheRISCV64Target(),
                                       createRISCVMCSymbolizer);
  TargetRegistry::RegisterMCDisassembler(getTheRISCV32beTarget(),
                                         createRISCVDisassembler);
  TargetRegistry::RegisterMCSymbolizer(getTheRISCV32beTarget(),
                                       createRISCVMCSymbolizer);
  TargetRegistry::RegisterMCDisassembler(getTheRISCV64beTarget(),
                                         createRISCVDisassembler);
  TargetRegistry::RegisterMCSymbolizer(getTheRISCV64beTarget(),
                                       createRISCVMCSymbolizer);
}

// <NT> 通用寄存器类解码模板 (所有 Decode*RegisterClass 的基底):
//   三个模板参数:
//     - FirstReg: 起始 MCRegister 枚举值 (如 RISCV::X0)
//     - NumRegsInClass: 类内寄存器总数, 用于 RegNo 范围检查
//     - RVELimit: 若非 0, 在 RV32E 模式下限制可用的最高 RegNo (默认 16)
//   关键机制: 把 5 位的 RegNo 直接加到 FirstReg 上得到 MCRegister, 然后
//   作为 MCOperand 追加到 Inst. 失败条件 = RegNo 越界 || RV32E 下越界.
//   实例化用法 (文件下方 constexpr): DecodeGPRRegisterClass =
//   DecodeSimpleRegisterClass<RISCV::X0, 32, 16> 等. 是 LLVM 后端反汇编
//   侧 "RegisterClass" 抽象的标准实现, 由 TableGen 在 DecoderEmitter
//   生成的代码里调用.
template <unsigned FirstReg, unsigned NumRegsInClass, unsigned RVELimit = 0>
static DecodeStatus DecodeSimpleRegisterClass(MCInst &Inst, uint32_t RegNo,
                                              uint64_t Address,
                                              const MCDisassembler *Decoder) {
  bool CheckRVE = RVELimit != 0 &&
                  Decoder->getSubtargetInfo().hasFeature(RISCV::FeatureStdExtE);

  if (RegNo >= NumRegsInClass || (CheckRVE && RegNo >= RVELimit))
    return MCDisassembler::Fail;

  MCRegister Reg = FirstReg + RegNo;
  Inst.addOperand(MCOperand::createReg(Reg));
  return MCDisassembler::Success;
}

constexpr auto DecodeGPRRegisterClass =
    DecodeSimpleRegisterClass<RISCV::X0, 32, /*RVELimit=*/16>;

static DecodeStatus DecodeGPRX1X5RegisterClass(MCInst &Inst, uint32_t RegNo,
                                               uint64_t Address,
                                               const MCDisassembler *Decoder) {
  MCRegister Reg = RISCV::X0 + RegNo;
  if (Reg != RISCV::X1 && Reg != RISCV::X5)
    return MCDisassembler::Fail;

  Inst.addOperand(MCOperand::createReg(Reg));
  return MCDisassembler::Success;
}

static DecodeStatus DecodeGPRX1RegisterClass(MCInst &Inst,
                                             const MCDisassembler *Decoder) {
  Inst.addOperand(MCOperand::createReg(RISCV::X1));
  return MCDisassembler::Success;
}

static DecodeStatus DecodeSPRegisterClass(MCInst &Inst,
                                          const MCDisassembler *Decoder) {
  Inst.addOperand(MCOperand::createReg(RISCV::X2));
  return MCDisassembler::Success;
}

static DecodeStatus DecodeSPRegisterClass(MCInst &Inst, uint64_t RegNo,
                                          uint32_t Address,
                                          const MCDisassembler *Decoder) {
  assert(RegNo == 2);
  Inst.addOperand(MCOperand::createReg(RISCV::X2));
  return MCDisassembler::Success;
}

static DecodeStatus DecodeGPRX5RegisterClass(MCInst &Inst,
                                             const MCDisassembler *Decoder) {
  Inst.addOperand(MCOperand::createReg(RISCV::X5));
  return MCDisassembler::Success;
}

template <auto DecodeFn, auto PredicateFn>
static DecodeStatus DecodeFilteredRegisterClass(MCInst &Inst, uint32_t RegNo,
                                                uint64_t Address,
                                                const MCDisassembler *Decoder) {
  if (!PredicateFn(RegNo))
    return MCDisassembler::Fail;
  return DecodeFn(Inst, RegNo, Address, Decoder);
}

constexpr bool PredNoX0(uint32_t RegNo) { return RegNo != 0; }
constexpr bool PredNoX2(uint32_t RegNo) { return RegNo != 2; }
constexpr bool PredNoX31(uint32_t RegNo) { return RegNo != 31; }

constexpr auto DecodeGPRNoX0RegisterClass =
    DecodeFilteredRegisterClass<DecodeGPRRegisterClass, PredNoX0>;
constexpr auto DecodeGPRNoX2RegisterClass =
    DecodeFilteredRegisterClass<DecodeGPRRegisterClass, PredNoX2>;
constexpr auto DecodeGPRNoX31RegisterClass =
    DecodeFilteredRegisterClass<DecodeGPRRegisterClass, PredNoX31>;

static DecodeStatus DecodeGPRPairRegisterClass(MCInst &Inst, uint32_t RegNo,
                                               uint64_t Address,
                                               const MCDisassembler *Decoder) {
  if (RegNo >= 32 || RegNo % 2)
    return MCDisassembler::Fail;

  const RISCVDisassembler *Dis =
      static_cast<const RISCVDisassembler *>(Decoder);
  const MCRegisterInfo *RI = Dis->getContext().getRegisterInfo();
  MCRegister Reg = RI->getMatchingSuperReg(
      RISCV::X0 + RegNo, RISCV::sub_gpr_even,
      &getRISCVMCRegisterClass(RISCV::GPRPairRegClassID));
  Inst.addOperand(MCOperand::createReg(Reg));
  return MCDisassembler::Success;
}

constexpr auto DecodeGPRPairNoX0RegisterClass =
    DecodeFilteredRegisterClass<DecodeGPRPairRegisterClass, PredNoX0>;

static DecodeStatus DecodeGPRPairCRegisterClass(MCInst &Inst, uint32_t RegNo,
                                                uint64_t Address,
                                                const MCDisassembler *Decoder) {
  if (RegNo >= 8 || RegNo % 2)
    return MCDisassembler::Fail;

  const RISCVDisassembler *Dis =
      static_cast<const RISCVDisassembler *>(Decoder);
  const MCRegisterInfo *RI = Dis->getContext().getRegisterInfo();
  MCRegister Reg = RI->getMatchingSuperReg(
      RISCV::X8 + RegNo, RISCV::sub_gpr_even,
      &getRISCVMCRegisterClass(RISCV::GPRPairCRegClassID));
  Inst.addOperand(MCOperand::createReg(Reg));
  return MCDisassembler::Success;
}

static DecodeStatus DecodeGPRS07RegisterClass(MCInst &Inst, uint32_t RegNo,
                                              uint64_t Address,
                                              const void *Decoder) {
  if (RegNo >= 8)
    return MCDisassembler::Fail;

  MCRegister Reg = (RegNo < 2) ? (RegNo + RISCV::X8) : (RegNo - 2 + RISCV::X18);
  Inst.addOperand(MCOperand::createReg(Reg));
  return MCDisassembler::Success;
}

template <unsigned RegisterClass, unsigned NumRegsInClass, unsigned LMul>
static DecodeStatus DecodeVectorRegisterClass(MCInst &Inst, uint32_t RegNo,
                                              uint64_t Address,
                                              const MCDisassembler *Decoder) {
  if (RegNo >= NumRegsInClass || RegNo % LMul)
    return MCDisassembler::Fail;

  const RISCVDisassembler *Dis =
      static_cast<const RISCVDisassembler *>(Decoder);
  const MCRegisterInfo *RI = Dis->getContext().getRegisterInfo();
  MCRegister Reg =
      RI->getMatchingSuperReg(RISCV::V0 + RegNo, RISCV::sub_vrm1_0,
                              &getRISCVMCRegisterClass(RegisterClass));

  Inst.addOperand(MCOperand::createReg(Reg));
  return MCDisassembler::Success;
}

static DecodeStatus DecodeTRM2RegisterClass(MCInst &Inst, uint32_t RegNo,
                                            uint64_t Address,
                                            const MCDisassembler *Decoder) {
  if (RegNo > 15 || RegNo % 2)
    return MCDisassembler::Fail;

  MCRegister Reg = RISCV::T0 + RegNo;
  Inst.addOperand(MCOperand::createReg(Reg));
  return MCDisassembler::Success;
}

static DecodeStatus DecodeTRM4RegisterClass(MCInst &Inst, uint32_t RegNo,
                                            uint64_t Address,
                                            const MCDisassembler *Decoder) {
  if (RegNo > 15 || RegNo % 4)
    return MCDisassembler::Fail;

  MCRegister Reg = RISCV::T0 + RegNo;
  Inst.addOperand(MCOperand::createReg(Reg));
  return MCDisassembler::Success;
}

static DecodeStatus DecodeYBNDSWImm(MCInst &Inst, uint64_t Imm, int64_t Address,
                                    const MCDisassembler *Decoder) {
  assert(isUInt<9>(Imm) && "Invalid immediate");
  uint64_t Result;
  if (Imm == 0) {
    // If imm[8:0] == 0, result is 4096.
    Result = 4096;
  } else if (Imm <= 255) {
    // If imm[8] == 0 and imm[7:0] != 0, result is imm[7:0].
    Result = Imm;
  } else {
    uint64_t Imm7To0 = Imm & 0xFF;
    if (Imm7To0 <= 31) {
      // If imm[8] == 1 and imm[7:5] == 0 (i.e. imm[7:0] <= 31), result is
      // `256 | (imm[3:0] << 4) | (imm[4] << 3)`.
      Result = 256 + ((Imm & 0xF) << 4) + (((Imm >> 4) & 1) << 3);
    } else {
      // Otherwise, result is imm[7:0] << 4.
      Result = Imm7To0 << 4;
    }
  }
  Inst.addOperand(MCOperand::createImm(Result));
  return MCDisassembler::Success;
}

static DecodeStatus decodeVMaskReg(MCInst &Inst, uint32_t RegNo,
                                   uint64_t Address,
                                   const MCDisassembler *Decoder) {
  if (RegNo >= 2)
    return MCDisassembler::Fail;

  MCRegister Reg = (RegNo == 0) ? RISCV::V0 : RISCV::NoRegister;

  Inst.addOperand(MCOperand::createReg(Reg));
  return MCDisassembler::Success;
}

static DecodeStatus decodeImmThreeOperand(MCInst &Inst,
                                          const MCDisassembler *Decoder) {
  Inst.addOperand(MCOperand::createImm(3));
  return MCDisassembler::Success;
}

static DecodeStatus decodeImmFourOperand(MCInst &Inst,
                                         const MCDisassembler *Decoder) {
  Inst.addOperand(MCOperand::createImm(4));
  return MCDisassembler::Success;
}

// <NT> 无符号立即数通用解码器:
//   RISC-V 反汇编侧最常用的立即数解码函数. 调用链:
//     getInstruction32 -> decodeInstruction -> [TableGen 生成的 DecoderTable]
//       -> decodeUImmOperand(Inst, Imm, Address, Decoder).
//   把 TableGen DecoderEmitter 从指令字段里提取出的 Imm (uint64_t)
//   直接作为 MCOperand::createImm 写入 Inst. 不做范围检查 / 符号扩展
//   / 缩放, 仅做 64 位字面量透传. 范围检查由 td 端的 EncoderMethod
//   配合约束 (AssemblerPredicate / Predicates) 保证, 反汇编侧只负责
//   把已解码的位段写出来. 对应的 AsmParser 端是 MatchInstructionImpl
//   调 generateImmOutOfRangeError 报错.
template <unsigned N>
static DecodeStatus decodeUImmOperand(MCInst &Inst, uint32_t Imm,
                                      int64_t Address,
                                      const MCDisassembler *Decoder) {
  assert(isUInt<N>(Imm) && "Invalid immediate");
  Inst.addOperand(MCOperand::createImm(Imm));
  return MCDisassembler::Success;
}

template <unsigned Width, unsigned LowerBound>
static DecodeStatus decodeUImmOperandGE(MCInst &Inst, uint32_t Imm,
                                        int64_t Address,
                                        const MCDisassembler *Decoder) {
  assert(isUInt<Width>(Imm) && "Invalid immediate");

  if (Imm < LowerBound)
    return MCDisassembler::Fail;

  Inst.addOperand(MCOperand::createImm(Imm));
  return MCDisassembler::Success;
}

template <unsigned Width, unsigned LowerBound>
static DecodeStatus decodeUImmPlus1OperandGE(MCInst &Inst, uint32_t Imm,
                                             int64_t Address,
                                             const MCDisassembler *Decoder) {
  assert(isUInt<Width>(Imm) && "Invalid immediate");

  if ((Imm + 1) < LowerBound)
    return MCDisassembler::Fail;

  Inst.addOperand(MCOperand::createImm(Imm + 1));
  return MCDisassembler::Success;
}

static DecodeStatus decodeUImmSlistOperand(MCInst &Inst, uint32_t Imm,
                                           int64_t Address,
                                           const MCDisassembler *Decoder) {
  assert(isUInt<3>(Imm) && "Invalid Slist immediate");
  const uint8_t Slist[] = {0, 1, 2, 4, 8, 16, 15, 31};
  Inst.addOperand(MCOperand::createImm(Slist[Imm]));
  return MCDisassembler::Success;
}

// <NT> shamt 立即数解码器 (SLLI/SRLI/SRAI 等移位指令):
//   RISC-V 移位指令的 shamt 字段是 log2(XLEN) 位编码: RV32 上 5 位
//   (代表 0-31), RV64 上 6 位 (代表 0-63). 该函数对 RV64 上的
//   6 位编码做隐式 zero-extend 到 64 位; RV32 上等价于透传.
//   调用方: getInstruction32 -> decodeInstruction -> [DecoderTable]
//     -> decodeUImmLog2XLenOperand (由 td 端 DecoderMethod="decodeUImmLog2XLenOperand" 指定).
//   与 decodeUImmOperand 的区别: 本函数会强制把 Imm 解释为合法的 shamt
//   范围 (无符号整数 0..XLEN-1), 写入 MCInst 后 InstPrinter 端就能
//   直接打印为十进制数. 注意: 不做 "Imm + 1" / "Imm - 1" 之类的特化,
//   那是 decodeUImmPlus1Operand / decodeUImm7EqXLenOperand 的活.
static DecodeStatus decodeUImmLog2XLenOperand(MCInst &Inst, uint32_t Imm,
                                              int64_t Address,
                                              const MCDisassembler *Decoder) {
  assert(isUInt<6>(Imm) && "Invalid immediate");

  if (!Decoder->getSubtargetInfo().hasFeature(RISCV::Feature64Bit) &&
      !isUInt<5>(Imm))
    return MCDisassembler::Fail;

  Inst.addOperand(MCOperand::createImm(Imm));
  return MCDisassembler::Success;
}

static DecodeStatus decodeUImm7EqXLenOperand(MCInst &Inst, uint32_t Imm,
                                             int64_t Address,
                                             const MCDisassembler *Decoder) {
  assert(isUInt<7>(Imm) && "Invalid immediate");

  uint32_t ExpectedValue =
      Decoder->getSubtargetInfo().hasFeature(RISCV::Feature64Bit) ? 64 : 32;
  if (Imm != ExpectedValue)
    return MCDisassembler::Fail;

  Inst.addOperand(MCOperand::createImm(Imm));
  return MCDisassembler::Success;
}

template <unsigned N>
static DecodeStatus decodeUImmNonZeroOperand(MCInst &Inst, uint32_t Imm,
                                             int64_t Address,
                                             const MCDisassembler *Decoder) {
  if (Imm == 0)
    return MCDisassembler::Fail;
  return decodeUImmOperand<N>(Inst, Imm, Address, Decoder);
}

static DecodeStatus
decodeUImmLog2XLenNonZeroOperand(MCInst &Inst, uint32_t Imm, int64_t Address,
                                 const MCDisassembler *Decoder) {
  if (Imm == 0)
    return MCDisassembler::Fail;
  return decodeUImmLog2XLenOperand(Inst, Imm, Address, Decoder);
}

template <unsigned N>
static DecodeStatus decodeUImmPlus1Operand(MCInst &Inst, uint32_t Imm,
                                           int64_t Address,
                                           const MCDisassembler *Decoder) {
  assert(isUInt<N>(Imm) && "Invalid immediate");
  Inst.addOperand(MCOperand::createImm(Imm + 1));
  return MCDisassembler::Success;
}

// <NT> Zibi 编码立即数解码器 (clz/ctz 指令专用):
//   调用方: getInstruction32 -> decodeInstruction -> [DecoderTable]
//     -> decodeImmZibiOperand (用于 CLZ/CLZW/CTZ/CTZW 等 "找首位/末位 1"
//     指令的立即数操作数).
//   关键机制: 5 位编码, 值域 1..31 (0 在硬件里非法). 解码语义:
//     编码值 0 表示 64 (即 XLEN), 其他直接透传. 因为 Zibi 字段为 0
//     在指令里是非法值 (实际硬件实现返回 XLEN), 所以这里把 0 解释
//     为 -1LL (LLVM IR 里表示 "未知 / 全部位"), 让 InstPrinter 输出
//     与 GCC binutils 行为一致. 范围检查由 assert(isUInt<5>(Imm)) 保证.
static DecodeStatus decodeImmZibiOperand(MCInst &Inst, uint32_t Imm,
                                         int64_t Address,
                                         const MCDisassembler *Decoder) {
  assert(isUInt<5>(Imm) && "Invalid immediate");
  Inst.addOperand(MCOperand::createImm(Imm ? Imm : -1LL));
  return MCDisassembler::Success;
}

template <unsigned N>
static DecodeStatus decodeSImmOperand(MCInst &Inst, uint32_t Imm,
                                      int64_t Address,
                                      const MCDisassembler *Decoder) {
  assert(isUInt<N>(Imm) && "Invalid immediate");
  // Sign-extend the number in the bottom N bits of Imm
  Inst.addOperand(MCOperand::createImm(SignExtend64<N>(Imm)));
  return MCDisassembler::Success;
}

static DecodeStatus decodeSImm12LoOperand(MCInst &Inst, uint32_t Imm,
                                          int64_t Address,
                                          const MCDisassembler *Decoder) {
  assert(isUInt<12>(Imm) && "Invalid immediate");
  const int64_t Value = SignExtend64<12>(Imm);
  if (!Decoder->tryAddingSymbolicOperand(Inst, Value, Address,
                                         /*IsBranch=*/false,
                                         /*Offset=*/0, /*OpSize=*/4,
                                         /*InstSize=*/4))
    Inst.addOperand(MCOperand::createImm(Value));
  return MCDisassembler::Success;
}

static DecodeStatus decodeUImm20Operand(MCInst &Inst, uint32_t Imm,
                                        int64_t Address,
                                        const MCDisassembler *Decoder) {
  assert(isUInt<20>(Imm) && "Invalid immediate");
  const int64_t Value = SignExtend64<32>(Imm << 12);
  if (!Decoder->tryAddingSymbolicOperand(Inst, Value, Address,
                                         /*IsBranch=*/false,
                                         /*Offset=*/0, /*OpSize=*/4,
                                         /*InstSize=*/4))
    Inst.addOperand(MCOperand::createImm(Imm));
  return MCDisassembler::Success;
}

template <unsigned N>
static DecodeStatus decodeSImmNonZeroOperand(MCInst &Inst, uint32_t Imm,
                                             int64_t Address,
                                             const MCDisassembler *Decoder) {
  if (Imm == 0)
    return MCDisassembler::Fail;
  return decodeSImmOperand<N>(Inst, Imm, Address, Decoder);
}

// <NT> 立即数 + 左移 N 位解码器 (栈调整类指令专用):
//   调用方: getInstruction32 -> decodeInstruction -> [DecoderTable]
//     -> decodeSImmOperandAndLslN (用于 CM.ADJSP / ADDI 等"立即数 +
//     左移 N 位"格式的栈调整指令).
//   关键机制: 模板参数 T 是总位宽, N 是末位 0 的个数. 把 Imm 按
//   T-N+1 位无符号数解释 (LSB N 位总是 0), 然后左移 N 位得到最终
//   立即数. 例如 CM.ADJSP 用 T=10, N=4: 表示 "6 位有符号立即数
//   左移 4 位" (即栈调整粒度 16 字节).
//   注意: 和 decodeUImmOperand 不同, 这里必须做符号扩展 + 左移,
//   因为栈调整可能为负 (释放栈帧). 不同 (T, N) 组合对应不同的
//   DecoderMethod 后缀 (td 端声明).
template <unsigned T, unsigned N>
static DecodeStatus decodeSImmOperandAndLslN(MCInst &Inst, uint32_t Imm,
                                             int64_t Address,
                                             const MCDisassembler *Decoder) {
  assert(isUInt<T - N + 1>(Imm) && "Invalid immediate");
  // Sign-extend the number in the bottom T bits of Imm after accounting for
  // the fact that the T bit immediate is stored in T-N bits (the LSB is
  // always zero)
  Inst.addOperand(MCOperand::createImm(SignExtend64<T>(Imm << N)));
  return MCDisassembler::Success;
}

// <NT> C.LUI 立即数解码器 (压缩指令 LUI 专用):
//   调用方: getInstruction16 -> decodeInstruction -> [DecoderTable]
//     -> decodeCLUIImmOperand (仅用于 C.LUI 压缩指令).
//   关键机制: C.LUI 的立即数字段是 nzimm[17:12] + nzimm[5] 拼接成的
//   6 位有符号数, 值域 [-32, 31], 且不允许 0 (因为 0 由 C.ADDI 占
//   用). 解码流程: 把 6 位字段 sign-extend 到 64 位, 然后左移 12 位
//   得到最终 18 位立即数的高位 (高 6 位有效, 低 12 位是 LUI 指令
//   隐式为零). 注意: 即便输入值 0, 这里也要按 -1 输出 (与
//   decodeImmZibiOperand 类似的 "非法值兜底" 行为), 因为 0 表示
//   "用 C.ADDI 而不是 C.LUI".
static DecodeStatus decodeCLUIImmOperand(MCInst &Inst, uint32_t Imm,
                                         int64_t Address,
                                         const MCDisassembler *Decoder) {
  assert(isUInt<6>(Imm) && "Invalid immediate");
  if (Imm == 0)
    return MCDisassembler::Fail;
  Imm = SignExtend64<6>(Imm) & 0xfffff;
  Inst.addOperand(MCOperand::createImm(Imm));
  return MCDisassembler::Success;
}

static DecodeStatus decodeFRMArg(MCInst &Inst, uint32_t Imm, int64_t Address,
                                 const MCDisassembler *Decoder) {
  assert(isUInt<3>(Imm) && "Invalid immediate");
  if (!llvm::RISCVFPRndMode::isValidRoundingMode(Imm))
    return MCDisassembler::Fail;

  Inst.addOperand(MCOperand::createImm(Imm));
  return MCDisassembler::Success;
}

static DecodeStatus decodeZcmpRlist(MCInst &Inst, uint32_t Imm,
                                    uint64_t Address,
                                    const MCDisassembler *Decoder) {
  bool IsRVE = Decoder->getSubtargetInfo().hasFeature(RISCV::FeatureStdExtE);
  if (Imm < RISCVZC::RA || (IsRVE && Imm >= RISCVZC::RA_S0_S2))
    return MCDisassembler::Fail;
  Inst.addOperand(MCOperand::createImm(Imm));
  return MCDisassembler::Success;
}

static DecodeStatus decodeXqccmpRlistS0(MCInst &Inst, uint32_t Imm,
                                        uint64_t Address,
                                        const MCDisassembler *Decoder) {
  if (Imm < RISCVZC::RA_S0)
    return MCDisassembler::Fail;
  return decodeZcmpRlist(Inst, Imm, Address, Decoder);
}

#include "RISCVGenDisassemblerTables.inc"

namespace {

struct DecoderListEntry {
  const uint8_t *Table;
  FeatureBitset ContainedFeatures;
  const char *Desc;

  bool haveContainedFeatures(const FeatureBitset &ActiveFeatures) const {
    return ContainedFeatures.none() ||
           (ContainedFeatures & ActiveFeatures).any();
  }
};

} // end anonymous namespace

static constexpr FeatureBitset XCVFeatureGroup = {
    RISCV::FeatureVendorXCVbitmanip, RISCV::FeatureVendorXCVelw,
    RISCV::FeatureVendorXCVmac,      RISCV::FeatureVendorXCVmem,
    RISCV::FeatureVendorXCValu,      RISCV::FeatureVendorXCVsimd,
    RISCV::FeatureVendorXCVbi};

static constexpr FeatureBitset XqciFeatureGroup = {
    RISCV::FeatureVendorXqcia,   RISCV::FeatureVendorXqciac,
    RISCV::FeatureVendorXqcibi,  RISCV::FeatureVendorXqcibm,
    RISCV::FeatureVendorXqcicli, RISCV::FeatureVendorXqcicm,
    RISCV::FeatureVendorXqcics,  RISCV::FeatureVendorXqcicsr,
    RISCV::FeatureVendorXqciint, RISCV::FeatureVendorXqciio,
    RISCV::FeatureVendorXqcilb,  RISCV::FeatureVendorXqcili,
    RISCV::FeatureVendorXqcilia, RISCV::FeatureVendorXqcilo,
    RISCV::FeatureVendorXqcilsm, RISCV::FeatureVendorXqcisim,
    RISCV::FeatureVendorXqcisls, RISCV::FeatureVendorXqcisync,
};

static constexpr FeatureBitset XSfVectorGroup = {
    RISCV::FeatureVendorXSfvcp,          RISCV::FeatureVendorXSfvqmaccdod,
    RISCV::FeatureVendorXSfvqmaccqoq,    RISCV::FeatureVendorXSfvfwmaccqqq,
    RISCV::FeatureVendorXSfvfnrclipxfqf, RISCV::FeatureVendorXSfmmbase,
    RISCV::FeatureVendorXSfvfexpa,       RISCV::FeatureVendorXSfvfexpa64e,
    RISCV::FeatureVendorXSfvfbfexp16e,   RISCV::FeatureVendorXSfvfexp16e,
    RISCV::FeatureVendorXSfvfexp32e};
static constexpr FeatureBitset XSfSystemGroup = {
    RISCV::FeatureVendorXSiFivecdiscarddlone,
    RISCV::FeatureVendorXSiFivecflushdlone,
};

static constexpr FeatureBitset XMIPSGroup = {
    RISCV::FeatureVendorXMIPSLSP,
    RISCV::FeatureVendorXMIPSCMov,
    RISCV::FeatureVendorXMIPSCBOP,
    RISCV::FeatureVendorXMIPSEXECTL,
};

static constexpr FeatureBitset XTHeadGroup = {
    RISCV::FeatureVendorXTHeadBa,      RISCV::FeatureVendorXTHeadBb,
    RISCV::FeatureVendorXTHeadBs,      RISCV::FeatureVendorXTHeadCondMov,
    RISCV::FeatureVendorXTHeadCmo,     RISCV::FeatureVendorXTHeadFMemIdx,
    RISCV::FeatureVendorXTHeadMac,     RISCV::FeatureVendorXTHeadMemIdx,
    RISCV::FeatureVendorXTHeadMemPair, RISCV::FeatureVendorXTHeadSync,
    RISCV::FeatureVendorXTHeadVdot};

static constexpr FeatureBitset XAndesGroup = {
    RISCV::FeatureVendorXAndesPerf,      RISCV::FeatureVendorXAndesBFHCvt,
    RISCV::FeatureVendorXAndesVBFHCvt,   RISCV::FeatureVendorXAndesVSIntH,
    RISCV::FeatureVendorXAndesVSIntLoad, RISCV::FeatureVendorXAndesVPackFPH,
    RISCV::FeatureVendorXAndesVDot};

static constexpr FeatureBitset XSMTGroup = {RISCV::FeatureVendorXSMTVDot,
                                            RISCV::FeatureVendorXSMTVDotII};

static constexpr FeatureBitset XAIFGroup = {RISCV::FeatureVendorXAIFET};

static constexpr DecoderListEntry DecoderList32[]{
    // Vendor Extensions
    {DecoderTableXCV32, XCVFeatureGroup, "CORE-V extensions"},
    {DecoderTableXqci32, XqciFeatureGroup, "Qualcomm uC Extensions"},
    {DecoderTableXTHead32, XTHeadGroup, "T-Head extensions"},
    {DecoderTableXSfvector32, XSfVectorGroup, "SiFive vector extensions"},
    {DecoderTableXSfsystem32, XSfSystemGroup, "SiFive system extensions"},
    {DecoderTableXSfcease32, {RISCV::FeatureVendorXSfcease}, "SiFive sf.cease"},
    {DecoderTableXMIPS32, XMIPSGroup, "Mips extensions"},
    {DecoderTableXAndes32, XAndesGroup, "Andes extensions"},
    {DecoderTableXSMT32, XSMTGroup, "SpacemiT extensions"},
    {DecoderTableXAIF32, XAIFGroup, "AI Foundry extensions"},
    // Standard Extensions
    {DecoderTable32, {}, "standard 32-bit instructions"},
    {DecoderTableRV32Only32, {}, "RV32-only standard 32-bit instructions"},
    {DecoderTableZfinx32, {}, "Zfinx (Float in Integer)"},
    {DecoderTableZdinxRV32Only32, {}, "RV32-only Zdinx (Double in Integer)"},
};

namespace {
// Define bitwidths for various types used to instantiate the decoder.
template <> constexpr uint32_t InsnBitWidth<uint16_t> = 16;
template <> constexpr uint32_t InsnBitWidth<uint32_t> = 32;
// Use uint64_t to represent 48 bit instructions.
template <> constexpr uint32_t InsnBitWidth<uint64_t> = 48;
} // namespace

// <NT> 32 位指令反汇编主路径 (RV32I/RV64I/CB/CJ 等):
//   调用方: getInstruction (顶层入口) 在判断 Bytes.size() >= 4 且低 2
//   位不全为 11 时调用. 是 RISC-V 反汇编最高频的入口, 处理所有
//   标准 32 位指令 (含 RV64I/RV32I 的 CB/CJ 形式, 因为它们虽然操作
//   数不同但指令字长仍是 4 字节).
//   关键机制:
//     1) 小端读出 32 位 Insn (support::endian::read32le)
//     2) 遍历 DecoderList32 表, 每个 Entry 是 {FeatureBitset, DecDesc*, "name"}
//     3) 用 Entry.haveContainedFeatures(STI.getFeatureBits()) 按当前
//        Subtarget 过滤候选 (例如没开 RVV 就跳过 RVV 指令的解码表)
//     4) 调 decodeInstruction(Table, MI, Insn, Address, this, STI),
//        这是 LLVM MCD 通用解码器, 内部走 TableGen 生成的解码表
//     5) 返回 Success/SoftFail/Fail; SoftFail 表示解码成功但格式不
//        合法 (供 llvm-objdump 报告)
DecodeStatus RISCVDisassembler::getInstruction32(MCInst &MI, uint64_t &Size,
                                                 ArrayRef<uint8_t> Bytes,
                                                 uint64_t Address,
                                                 raw_ostream &CS) const {
  if (Bytes.size() < 4) {
    Size = 0;
    return MCDisassembler::Fail;
  }
  Size = 4;

  uint32_t Insn = support::endian::read32le(Bytes.data());

  for (const DecoderListEntry &Entry : DecoderList32) {
    if (!Entry.haveContainedFeatures(STI.getFeatureBits()))
      continue;

    LLVM_DEBUG(dbgs() << "Trying " << Entry.Desc << " table:\n");
    DecodeStatus Result =
        decodeInstruction(Entry.Table, MI, Insn, Address, this, STI);
    if (Result == MCDisassembler::Fail)
      continue;

    return Result;
  }

  return MCDisassembler::Fail;
}

static constexpr DecoderListEntry DecoderList16[]{
    // Vendor Extensions
    {DecoderTableXqci16, XqciFeatureGroup, "Qualcomm uC 16-bit"},
    {DecoderTableXqccmp16,
     {RISCV::FeatureVendorXqccmp},
     "Xqccmp (Qualcomm 16-bit Push/Pop & Double Move Instructions)"},
    {DecoderTableXqccmt16,
     {RISCV::FeatureVendorXqccmt},
     "Xqccmt (Qualcomm 16-bit Table Jump Instructions)"},
    {DecoderTableXwchc16, {RISCV::FeatureVendorXwchc}, "WCH QingKe XW"},
    // Standard Extensions
    // DecoderTableZicfiss16 must be checked before DecoderTable16.
    {DecoderTableZicfiss16, {}, "Zicfiss (Shadow Stack 16-bit)"},
    {DecoderTable16, {}, "standard 16-bit instructions"},
    {DecoderTableRV32Only16, {}, "RV32-only 16-bit instructions"},
    // Zc* instructions incompatible with Zcf or Zcd
    {DecoderTableZcOverlap16,
     {},
     "ZcOverlap (16-bit Instructions overlapping with Zcf/Zcd)"},
};

// <NT> 16 位压缩指令反汇编主路径 (RVC):
//   调用方: getInstruction (顶层入口) 在判断低 2 位不全为 11 (即
//   不是 32 位指令) 时调用. 处理 RISC-V 压缩扩展 (RVC) 的所有指令:
//   C.ADD/C.SUB/C.MV/C.AND 等, 占总指令密度的 30-50%.
//   关键机制:
//     1) 小端读出 16 位 Insn (support::endian::read16le)
//     2) 检查 RVC 扩展是否启用 (hasFeature(FeatureExtC)); 若未启用
//        则报 Fail (按 RISC-V 规范, 没开 C 扩展时不能有低 2 位 != 11
//        的指令)
//     3) 遍历 DecoderList16 表 (16 位解码表), 调 decodeInstruction
//        走 TableGen 生成的状态机, 匹配成功即返回. Size 写 2 表示
//        消耗了 2 字节, 让 llvm-objdump 知道下一条指令的偏移.
//   与 getInstruction32 的区别: 仅处理 16 位指令, RVC Feature 是
//   必备前提, 解码表条目少得多 (~50 条 vs 32 位表的 ~500 条).
DecodeStatus RISCVDisassembler::getInstruction16(MCInst &MI, uint64_t &Size,
                                                 ArrayRef<uint8_t> Bytes,
                                                 uint64_t Address,
                                                 raw_ostream &CS) const {
  if (Bytes.size() < 2) {
    Size = 0;
    return MCDisassembler::Fail;
  }
  Size = 2;

  uint16_t Insn = support::endian::read16le(Bytes.data());

  for (const DecoderListEntry &Entry : DecoderList16) {
    if (!Entry.haveContainedFeatures(STI.getFeatureBits()))
      continue;

    LLVM_DEBUG(dbgs() << "Trying " << Entry.Desc << " table:\n");
    DecodeStatus Result =
        decodeInstruction(Entry.Table, MI, Insn, Address, this, STI);
    if (Result != MCDisassembler::Fail)
      return Result;
  }

  return MCDisassembler::Fail;
}

static constexpr DecoderListEntry DecoderList48[]{
    {DecoderTableXqci48, XqciFeatureGroup, "Qualcomm uC 48bit"},
};

// <NT> 48 位长指令反汇编主路径 (RVV 配置指令专用):
//   调用方: getInstruction (顶层入口) 在判断 Bytes.size() >= 6 且
//   满足 RVV "6 字节长指令" 特征位时调用. 处理 RVV (Vector) 扩展
//   的 vsetvli / vsetivli / vsetvl 配置类指令, 它们是 48 位长以
//   容纳 Zimm[9:0] / Zimm[9:0] + rs1 等额外字段.
//   关键机制:
//     1) 小端读出 48 位 Insn (read32le + 高位 16 位)
//     2) 遍历 DecoderList48 表; 与 32 位路径不同, 48 位表几乎只
//        包含 vsetvli 系列 (因为 RVV 大量算术指令仍保持 32 位)
//     3) 调 decodeInstruction; 失败回退到 32 位尝试 (即把 48 位
//        当 32 位解码), 这是兜底策略, 但语义上不严谨, 实际工具
//        不会触发 (因 48 位指令的特征位互斥).
//   Size 写 6. RVV 是 1.0 起新增的扩展, 所以该入口存在时间较
//   getInstruction32/16 短.
DecodeStatus RISCVDisassembler::getInstruction48(MCInst &MI, uint64_t &Size,
                                                 ArrayRef<uint8_t> Bytes,
                                                 uint64_t Address,
                                                 raw_ostream &CS) const {
  if (Bytes.size() < 6) {
    Size = 0;
    return MCDisassembler::Fail;
  }
  Size = 6;

  uint64_t Insn = 0;
  for (size_t i = Size; i-- != 0;)
    Insn += (static_cast<uint64_t>(Bytes[i]) << 8 * i);

  for (const DecoderListEntry &Entry : DecoderList48) {
    if (!Entry.haveContainedFeatures(STI.getFeatureBits()))
      continue;

    LLVM_DEBUG(dbgs() << "Trying " << Entry.Desc << " table:\n");
    DecodeStatus Result =
        decodeInstruction(Entry.Table, MI, Insn, Address, this, STI);
    if (Result == MCDisassembler::Fail)
      continue;

    return Result;
  }

  return MCDisassembler::Fail;
}

// <NT> 反汇编器顶层入口 (MCDisassembler 唯一 override 接口):
//   调用方: llvm-objdump / llvm-mc -disassemble 等工具; LLVM 框架
//   通过虚函数 MCDisassembler::getInstruction 调用本实现. 给定一段
//   字节流 (Bytes) 和起始地址 (Address), 反汇编出"一条"指令到 MI,
//   并把消耗的字节数写到 Size. 不负责多指令循环, 调用方需要循环
//   调用本函数直到 Size==0 或 EOF.
//   关键机制: 按字节数 + 指令特征位分派:
//     1) Bytes.size() < 2  -> Fail (不合法输入)
//     2) Bytes[0] 低 2 位不全为 11 (即 (Insn16 & 0x3) != 0x3) ->
//        getInstruction16 (RVC 压缩指令, 16 位)
//     3) 否则走 getInstruction32 (32 位), 它会先尝试匹配,
//        失败再走 getInstruction48 兜底 (仅在 RVV 启用时)
//   上游: MCDisassembler::getInstruction 接口, 继承自 MCDisassembler
//   基类, override 关键字保证签名匹配. 下游: 工具拿到 MI 后调
//   RISCVInstPrinter::printInstruction 打印.
DecodeStatus RISCVDisassembler::getInstruction(MCInst &MI, uint64_t &Size,
                                               ArrayRef<uint8_t> Bytes,
                                               uint64_t Address,
                                               raw_ostream &CS) const {
  CommentStream = &CS;
  // It's a 16 bit instruction if bit 0 and 1 are not 0b11.
  if ((Bytes[0] & 0b11) != 0b11)
    return getInstruction16(MI, Size, Bytes, Address, CS);

  // Try to decode as a 32-bit instruction first.
  DecodeStatus Result = getInstruction32(MI, Size, Bytes, Address, CS);
  if (Result != MCDisassembler::Fail)
    return Result;

  // If bits [4:2] are 0b111 this might be a 48-bit or larger instruction,
  // otherwise assume it's an unknown 32-bit instruction.
  if ((Bytes[0] & 0b1'1100) != 0b1'1100) {
    Size = Bytes.size() >= 4 ? 4 : 0;
    return MCDisassembler::Fail;
  }

  // 48-bit instructions are encoded as 0bxx011111.
  if ((Bytes[0] & 0b11'1111) == 0b01'1111)
    return getInstruction48(MI, Size, Bytes, Address, CS);

  // 64-bit instructions are encoded as 0x0111111.
  if ((Bytes[0] & 0b111'1111) == 0b011'1111) {
    Size = Bytes.size() >= 8 ? 8 : 0;
    return MCDisassembler::Fail;
  }

  // Remaining cases need to check a second byte.
  if (Bytes.size() < 2) {
    Size = 0;
    return MCDisassembler::Fail;
  }

  // 80-bit through 176-bit instructions are encoded as 0bxnnnxxxx_x1111111.
  // Where the number of bits is (80 + (nnn * 16)) for nnn != 0b111.
  unsigned nnn = (Bytes[1] >> 4) & 0b111;
  if (nnn != 0b111) {
    Size = 10 + (nnn * 2);
    if (Bytes.size() < Size)
      Size = 0;
    return MCDisassembler::Fail;
  }

  // Remaining encodings are reserved for > 176-bit instructions.
  Size = 0;
  return MCDisassembler::Fail;
}
