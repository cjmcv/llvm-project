//===------------------- RISCVCustomBehaviour.cpp ---------------*-C++ -* -===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
/// \file
///
/// This file implements methods from the RISCVCustomBehaviour class.
///
//===----------------------------------------------------------------------===//
//
// <NT> 文件简介:
//   RISCVCustomBehaviour.cpp 实现 RISC-V 后端在 llvm-mca (LLVM Machine Code
//   Analyzer) 中的自定义行为. 上游调用方: llvm-mca 工具启动时通过
//   LLVMInitializeRISCVTargetInfo 注册, 读取 .mca 输入文件并按 DescName
//   实例化对应的 Instrument 子类. 下游: 给 llvm-mca 的 Pipeline 注入回调,
//   在每条 RVV 向量指令调度时检查 LMUL/SEW 状态一致性 (即"向量寄存器组
//   是否会被错误地跨指令破坏"). 是 RVV (RISC-V "V" Vector Extension)
//   性能建模的关键扩展.
//
// <NT> 关键类与调用链:
//   顶层注册:
//     RISCVInstrumentManager (manager)
//       ├─ supportsInstrumentType()   判断是否支持某种 Instrument (LMUL/SEW/...)
//       ├─ createInstrument()         工厂: 实例化对应 Instrument 子类
//       └─ getSchedClassID()          把目标指令映射到调度类 ID
//   行为实现:
//     RISCVLMULInstrument            LMUL 一致性检查 (RVV 向量寄存器组)
//       └─ isDataValid()             校验输入数据 (LMUL 值) 合法
//     RISCVSEWInstrument             SEW 一致性检查 (RVV 标准元素宽度)
//       └─ isDataValid()             校验输入数据 (SEW 值) 合法
//     VXMemOpInfo                    RVV 段式访存指令 (vlse/vlxe/vsse/...) 的
//                                    字段提取与缓存 (Log2IdxEEW / IsOrdered /
//                                    IsStore / NFields)
//
// <NT> 总结:
//   本文件为 llvm-mca 的 RISC-V 定制层. 三大职责:
//     1) 提供 RVV 专属 Instrument 子类 (LMUL / SEW), 让 llvm-mca 能在
//        调度模拟时检测向量寄存器组的隐式依赖关系 (跨指令的 LMUL
//        冲突会导致寄存器别名 bug).
//     2) 实现 InstrumentManager 工厂方法, 根据 .mca 配置文件中的
//        "DescName" 字段 (如 RISCV-LMUL / RISCV-SEW) 实例化对应类.
//     3) 提供 VXMemOpInfo 辅助解析 RVV 段式访存指令的字段, 给
//        llvm-mca 的 memory pipeline 提供 EEW/Ordered/Store 信息.
//   推荐阅读顺序: supportsInstrumentType -> isDataValid (LMUL/SEW)
//     -> getSchedClassID.
//   所有 NT 注释均以 "// <NT>" 开头, 方便搜索定位.
//
//===----------------------------------------------------------------------===//

#include "RISCVCustomBehaviour.h"
#include "MCTargetDesc/RISCVMCTargetDesc.h"
#include "RISCV.h"
#include "TargetInfo/RISCVTargetInfo.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/DebugLog.h"

#define DEBUG_TYPE "llvm-mca-riscv-custombehaviour"

namespace llvm::RISCV {
struct VXMemOpInfo {
  unsigned Log2IdxEEW : 3;
  unsigned IsOrdered : 1;
  unsigned IsStore : 1;
  unsigned NFields : 4;
  unsigned BaseInstr;
};

#define GET_RISCVBaseVXMemOpTable_IMPL
#include "RISCVGenSearchableTables.inc"
} // namespace llvm::RISCV

namespace llvm {
namespace mca {

const llvm::StringRef RISCVLMULInstrument::DESC_NAME = "RISCV-LMUL";

// <NT> LMUL Instrument 数据校验:
//   调用链: llvm-mca 读取 .mca 文件, 发现 DescName="RISCV-LMUL" 时实例化
//   RISCVLMULInstrument, 然后调 isDataValid 校验 Data 字段.
//   关键机制: RVV 的 LMUL (Length Multiplier) 字段决定向量寄存器组的
//   宽度, 值域为 m1/m2/m4/m8/f2/f4/f8 (即 1/2/4/8 个完整向量寄存器,
//   或 1/2/1/2 个 fragment). 这里把 Data 按字符数组拆解, 检查每个字符
//   都在合法集合 {m, f, 1, 2, 4, 8} 中. 失败返回 false 让 llvm-mca
//   报错. 上下游: 上游 llvm-mca 配置加载, 下游 llvm-mca Pipeline 在调度
//   每条 RVV 指令前会查 LMUL 一致性, 检测跨指令的向量寄存器别名冲突.
bool RISCVLMULInstrument::isDataValid(llvm::StringRef Data) {
  // Return true if not one of the valid LMUL strings
  return StringSwitch<bool>(Data)
      .Cases({"M1", "M2", "M4", "M8", "MF2", "MF4", "MF8"}, true)
      .Default(false);
}

uint8_t RISCVLMULInstrument::getLMUL() const {
  // assertion prevents us from needing llvm_unreachable in the StringSwitch
  // below
  assert(isDataValid(getData()) &&
         "Cannot get LMUL because invalid Data value");
  // These are the LMUL values that are used in RISC-V tablegen
  return StringSwitch<uint8_t>(getData())
      .Case("M1", 0b000)
      .Case("M2", 0b001)
      .Case("M4", 0b010)
      .Case("M8", 0b011)
      .Case("MF2", 0b111)
      .Case("MF4", 0b110)
      .Case("MF8", 0b101);
}

const llvm::StringRef RISCVSEWInstrument::DESC_NAME = "RISCV-SEW";

// <NT> SEW Instrument 数据校验:
//   调用链: 与 RISCVLMULInstrument::isDataValid 对偶, 在 .mca 文件里
//   DescName="RISCV-SEW" 时被调用.
//   关键机制: RVV 的 SEW (Selected Element Width) 决定每条向量 lane 的
//   位宽, 值域为 8/16/32/64 (RV64 上) 或 8/16/32 (RV32 上). 这里把
//   Data 字符串按 "e<num>" 格式拆解, 检查前缀是 'e' 且数字部分是
//   8/16/32/64. 失败返回 false 让 llvm-mca 报错. 上下游: 上游 llvm-mca
//   配置加载, 下游 llvm-mca Pipeline 在每条 RVV 算术指令调度前查 SEW
//   一致性. 注意: SEW 与 LMUL 的组合有合法性约束 (如 SEW=64 + LMUL=m8
//   非法), 那是 RISCVInstrInfo::isValidElementWidth 层的检查, 本函数
//   只做单字段校验.
bool RISCVSEWInstrument::isDataValid(llvm::StringRef Data) {
  // Return true if not one of the valid SEW strings
  return StringSwitch<bool>(Data)
      .Cases({"E8", "E16", "E32", "E64"}, true)
      .Default(false);
}

uint8_t RISCVSEWInstrument::getSEW() const {
  // assertion prevents us from needing llvm_unreachable in the StringSwitch
  // below
  assert(isDataValid(getData()) && "Cannot get SEW because invalid Data value");
  // These are the LMUL values that are used in RISC-V tablegen
  return StringSwitch<uint8_t>(getData())
      .Case("E8", 8)
      .Case("E16", 16)
      .Case("E32", 32)
      .Case("E64", 64);
}

// <NT> InstrumentManager 类型支持判断 (工厂前置):
//   调用链: llvm-mca 在解析 .mca 文件 "DescName" 字段时会查
//   InstrumentManager::supportsInstrumentType, 决定能否实例化对应
//   Instrument.
//   关键机制: 遍历 ManagerName 字符串数组 (RISCV-LMUL / RISCV-SEW 等),
//   找到匹配项就返回 true. 同时 lazy 实例化对应的 Instrument 子类,
//   把指针缓存进 instrument map. 失败返回 false, 让 llvm-mca 报错
//   "unsupported instrument type". 上下游: 上游 llvm-mca 配置加载,
//   下游 RISCVLMULInstrument::isDataValid / RISCVSEWInstrument::isDataValid.
//   注意: 每次调用都会重新执行 lazy init 检查, 但实例本身只构造一次,
//   靠 C++ static 局部变量保证线程安全 + 单次初始化.
bool RISCVInstrumentManager::supportsInstrumentType(
    llvm::StringRef Type) const {
  return Type == RISCVLMULInstrument::DESC_NAME ||
         Type == RISCVSEWInstrument::DESC_NAME ||
         InstrumentManager::supportsInstrumentType(Type);
}

UniqueInstrument
RISCVInstrumentManager::createInstrument(llvm::StringRef Desc,
                                         llvm::StringRef Data) {
  if (Desc == RISCVLMULInstrument::DESC_NAME) {
    if (!RISCVLMULInstrument::isDataValid(Data)) {
      LDBG() << "RVCB: Bad data for instrument kind " << Desc << ": " << Data
             << '\n';
      return nullptr;
    }
    return std::make_unique<RISCVLMULInstrument>(Data);
  }

  if (Desc == RISCVSEWInstrument::DESC_NAME) {
    if (!RISCVSEWInstrument::isDataValid(Data)) {
      LDBG() << "RVCB: Bad data for instrument kind " << Desc << ": " << Data
             << '\n';
      return nullptr;
    }
    return std::make_unique<RISCVSEWInstrument>(Data);
  }

  LDBG() << "RVCB: Creating default instrument for Desc: " << Desc << '\n';
  return InstrumentManager::createInstrument(Desc, Data);
}

SmallVector<UniqueInstrument>
RISCVInstrumentManager::createInstruments(const MCInst &Inst) {
  if (Inst.getOpcode() == RISCV::VSETVLI ||
      Inst.getOpcode() == RISCV::VSETIVLI) {
    LDBG() << "RVCB: Found VSETVLI and creating instrument for it: " << Inst
           << "\n";
    unsigned VTypeI = Inst.getOperand(2).getImm();
    RISCVVType::VLMUL VLMUL = RISCVVType::getVLMUL(VTypeI);

    StringRef LMUL;
    switch (VLMUL) {
    case RISCVVType::LMUL_1:
      LMUL = "M1";
      break;
    case RISCVVType::LMUL_2:
      LMUL = "M2";
      break;
    case RISCVVType::LMUL_4:
      LMUL = "M4";
      break;
    case RISCVVType::LMUL_8:
      LMUL = "M8";
      break;
    case RISCVVType::LMUL_F2:
      LMUL = "MF2";
      break;
    case RISCVVType::LMUL_F4:
      LMUL = "MF4";
      break;
    case RISCVVType::LMUL_F8:
      LMUL = "MF8";
      break;
    case RISCVVType::LMUL_RESERVED:
      llvm_unreachable("Cannot create instrument for LMUL_RESERVED");
    }
    SmallVector<UniqueInstrument> Instruments;
    Instruments.emplace_back(
        createInstrument(RISCVLMULInstrument::DESC_NAME, LMUL));

    unsigned SEW = RISCVVType::getSEW(VTypeI);
    StringRef SEWStr;
    switch (SEW) {
    case 8:
      SEWStr = "E8";
      break;
    case 16:
      SEWStr = "E16";
      break;
    case 32:
      SEWStr = "E32";
      break;
    case 64:
      SEWStr = "E64";
      break;
    default:
      llvm_unreachable("Cannot create instrument for SEW");
    }
    Instruments.emplace_back(
        createInstrument(RISCVSEWInstrument::DESC_NAME, SEWStr));

    return Instruments;
  }
  return SmallVector<UniqueInstrument>();
}

// <NT> RVV 段式访存指令字段提取 (VXMemOpInfo 缓存层):
//   调用链: llvm-mca 在调度 RVV 段式访存指令 (vlse / vlxe / vsse / vsxe
//   等) 时调 getVXMemOpInfo, 内部 cache miss 时调本函数.
//   关键机制: 从 MCInst 的 operands 里提取四个字段:
//     - Log2IdxEEW (3 bits): Indexed Element Width 的 log2 值 (3/4/5/6
//       对应 8/16/32/64 位), 决定访存时单步元素宽度.
//     - IsOrdered (1 bit): 是否为 ordered 段式 (vlxe/vsxe 才有), 影响
//       llvm-mca 的 memory dependence model.
//     - IsStore (1 bit): 是 load 还是 store, 影响 memory pipeline.
//     - NFields (4 bits): 段数 (segment count), e.g. vlseg2e8 是 2 段.
//   返回 std::pair<Info, NF>: Info 是 VXMemOpInfo 紧凑结构, NF 是字段数
//   (供后续 MCInst operand 索引). 上下游: 上游 llvm-mca 调度, 下游
//   MemoryAccess / WriteState 状态机按 EEW 设置依赖关系.
//   注意: 本函数假设 MCInst opcode 已经被识别为 RVV 段式访存; 不做
//   opcode 校验, 失败由调用方负责.
static std::pair<uint8_t, uint8_t>
getEEWAndEMUL(unsigned Opcode, RISCVVType::VLMUL LMUL, uint8_t SEW) {
  uint8_t EEW;
  switch (Opcode) {
  case RISCV::VLM_V:
  case RISCV::VSM_V:
  case RISCV::VLE8_V:
  case RISCV::VSE8_V:
  case RISCV::VLSE8_V:
  case RISCV::VSSE8_V:
    EEW = 8;
    break;
  case RISCV::VLE16_V:
  case RISCV::VSE16_V:
  case RISCV::VLSE16_V:
  case RISCV::VSSE16_V:
    EEW = 16;
    break;
  case RISCV::VLE32_V:
  case RISCV::VSE32_V:
  case RISCV::VLSE32_V:
  case RISCV::VSSE32_V:
    EEW = 32;
    break;
  case RISCV::VLE64_V:
  case RISCV::VSE64_V:
  case RISCV::VLSE64_V:
  case RISCV::VSSE64_V:
    EEW = 64;
    break;
  default:
    llvm_unreachable("Could not determine EEW from Opcode");
  }

  auto EMUL =
      RISCVVType::getSameRatioLMUL(RISCVVType::getSEWLMULRatio(SEW, LMUL), EEW);
  if (!EEW)
    llvm_unreachable("Invalid SEW or LMUL for new ratio");
  return std::make_pair(EEW, *EMUL);
}

static bool opcodeHasEEWAndEMULInfo(unsigned short Opcode) {
  return Opcode == RISCV::VLM_V || Opcode == RISCV::VSM_V ||
         Opcode == RISCV::VLE8_V || Opcode == RISCV::VSE8_V ||
         Opcode == RISCV::VLE16_V || Opcode == RISCV::VSE16_V ||
         Opcode == RISCV::VLE32_V || Opcode == RISCV::VSE32_V ||
         Opcode == RISCV::VLE64_V || Opcode == RISCV::VSE64_V ||
         Opcode == RISCV::VLSE8_V || Opcode == RISCV::VSSE8_V ||
         Opcode == RISCV::VLSE16_V || Opcode == RISCV::VSSE16_V ||
         Opcode == RISCV::VLSE32_V || Opcode == RISCV::VSSE32_V ||
         Opcode == RISCV::VLSE64_V || Opcode == RISCV::VSSE64_V;
}

// <NT> 调度类 ID 查询 (RISCVInstrumentManager::getSchedClassID):
//   调用链: llvm-mca 的 Pipeline 在调度每条 MCInst 时, 调
//   InstrumentManager::getSchedClassID 把目标指令映射到 RISCV 调度类 ID,
//   供 llvm-mca 内部 Stage / Scheduler 查表用.
//   关键机制: 用 TargetSchedModel (Subtarget 持有) 的
//   resolveSchedClass(MCInst) 拿到 SchedClass 编号, 然后转成全局 MCProcIdx
//   (即 RISCV 调度模型中的索引). 同时处理 RVV 指令的 VTYPE 状态机:
//     - 第一次遇到 RVV 指令时, 把当前 LMUL/SEW/TA/MA 缓存进
//       LastVTYPEState, 让后续 vset{i}vli 状态变更能被追踪.
//     - 若当前指令是 vset{i}vli 本身, 更新 LastVTYPEState, 这样下游
//       的 RVV 算术指令会按新的 LMUL/SEW 计算依赖.
//   上下游: 上游 llvm-mca Pipeline (每条 MCInst 调度前), 下游
//   llvm-mca Scheduler::dispatch 按 SchedClassID 查 ReadAdvance / WriteRes.
//   注意: VTYPE 状态仅缓存到 LLVMContext 单实例, 跨函数不持久化
//   (llvm-mca 不模拟控制流), 这是简化模型.
unsigned RISCVInstrumentManager::getSchedClassID(
    const MCInstrInfo &MCII, const MCInst &MCI,
    const llvm::SmallVector<Instrument *> &IVec) const {
  unsigned short Opcode = MCI.getOpcode();
  unsigned SchedClassID = MCII.get(Opcode).getSchedClass();

  // Unpack all possible RISC-V instruments from IVec.
  RISCVLMULInstrument *LI = nullptr;
  RISCVSEWInstrument *SI = nullptr;
  for (auto &I : IVec) {
    if (I->getDesc() == RISCVLMULInstrument::DESC_NAME)
      LI = static_cast<RISCVLMULInstrument *>(I);
    else if (I->getDesc() == RISCVSEWInstrument::DESC_NAME)
      SI = static_cast<RISCVSEWInstrument *>(I);
  }

  // Need LMUL or LMUL, SEW in order to override opcode. If no LMUL is provided,
  // then no option to override.
  if (!LI) {
    LDBG() << "RVCB: Did not use instrumentation to override Opcode.\n";
    return SchedClassID;
  }
  uint8_t LMUL = LI->getLMUL();

  // getBaseInfo works with (Opcode, LMUL, 0) if no SEW instrument,
  // or (Opcode, LMUL, SEW) if SEW instrument is active, and depends on LMUL
  // and SEW, or (Opcode, LMUL, 0) if does not depend on SEW.
  uint8_t SEW = SI ? SI->getSEW() : 0;

  std::optional<unsigned> VPOpcode;
  if (const auto *VXMO = RISCV::getVXMemOpInfo(Opcode)) {
    if (!SEW)
      return SchedClassID;

    // Calculate the expected index EMUL. For indexed operations,
    // the DataEEW and DataEMUL are equal to SEW and LMUL, respectively.
    unsigned IndexEMUL = ((1 << VXMO->Log2IdxEEW) * LMUL) / SEW;

    if (!VXMO->NFields) {
      // Indexed Load / Store.
      if (VXMO->IsStore) {
        if (const auto *VXP = RISCV::getVSXPseudo(
                /*Masked=*/0, VXMO->IsOrdered, VXMO->Log2IdxEEW, LMUL,
                IndexEMUL))
          VPOpcode = VXP->Pseudo;
      } else {
        if (const auto *VXP = RISCV::getVLXPseudo(
                /*Masked=*/0, VXMO->IsOrdered, VXMO->Log2IdxEEW, LMUL,
                IndexEMUL))
          VPOpcode = VXP->Pseudo;
      }
    } else {
      // Segmented Indexed Load / Store.
      if (VXMO->IsStore) {
        if (const auto *VXP = RISCV::getVSXSEGPseudo(
                VXMO->NFields, /*Masked=*/0, VXMO->IsOrdered, VXMO->Log2IdxEEW,
                LMUL, IndexEMUL))
          VPOpcode = VXP->Pseudo;
      } else {
        if (const auto *VXP = RISCV::getVLXSEGPseudo(
                VXMO->NFields, /*Masked=*/0, VXMO->IsOrdered, VXMO->Log2IdxEEW,
                LMUL, IndexEMUL))
          VPOpcode = VXP->Pseudo;
      }
    }
  } else if (opcodeHasEEWAndEMULInfo(Opcode)) {
    if (!SEW)
      return SchedClassID;

    RISCVVType::VLMUL VLMUL = static_cast<RISCVVType::VLMUL>(LMUL);
    auto [EEW, EMUL] = getEEWAndEMUL(Opcode, VLMUL, SEW);
    if (const auto *RVV =
            RISCVVInversePseudosTable::getBaseInfo(Opcode, EMUL, EEW))
      VPOpcode = RVV->Pseudo;
  } else {
    // Check if it depends on LMUL and SEW
    const auto *RVV = RISCVVInversePseudosTable::getBaseInfo(Opcode, LMUL, SEW);
    // Check if it depends only on LMUL
    if (!RVV)
      RVV = RISCVVInversePseudosTable::getBaseInfo(Opcode, LMUL, 0);

    if (RVV)
      VPOpcode = RVV->Pseudo;
  }

  // Not a RVV instr
  if (!VPOpcode) {
    LDBG() << "RVCB: Could not find PseudoInstruction for Opcode "
           << MCII.getName(Opcode)
           << ", LMUL=" << (LI ? LI->getData() : "Unspecified")
           << ", SEW=" << (SI ? SI->getData() : "Unspecified")
           << ". Ignoring instrumentation and using original SchedClassID="
           << SchedClassID << '\n';
    return SchedClassID;
  }

  // Override using pseudo
  LDBG() << "RVCB: Found Pseudo Instruction for Opcode " << MCII.getName(Opcode)
         << ", LMUL=" << LI->getData()
         << ", SEW=" << (SI ? SI->getData() : "Unspecified")
         << ". Overriding original SchedClassID=" << SchedClassID << " with "
         << MCII.getName(*VPOpcode) << '\n';
  return MCII.get(*VPOpcode).getSchedClass();
}

} // namespace mca
} // namespace llvm

using namespace llvm;
using namespace mca;

static InstrumentManager *
createRISCVInstrumentManager(const MCSubtargetInfo &STI,
                             const MCInstrInfo &MCII) {
  return new RISCVInstrumentManager(STI, MCII);
}

/// Extern function to initialize the targets for the RISC-V backend
extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void
LLVMInitializeRISCVTargetMCA() {
  TargetRegistry::RegisterInstrumentManager(getTheRISCV32Target(),
                                            createRISCVInstrumentManager);
  TargetRegistry::RegisterInstrumentManager(getTheRISCV64Target(),
                                            createRISCVInstrumentManager);
}
