//===-- RISCVMCObjectFileInfo.cpp - RISC-V object file properties ---------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains the declarations of the RISCVMCObjectFileInfo properties.
//
//===----------------------------------------------------------------------===//

// <NT> 文件简介:
//   RISCVMCObjectFileInfo.cpp 实现 MCObjectFileInfo 子类, 调整 RISC-V ELF
//   对象文件的段布局 (对齐 / 段名) 等元数据. 文件极短 (≈30 行), 只 override
//   几个静态成员常量. 上游 RISCVMCTargetDesc / MCContext, 下游 RISCVAsmBackend
//   在 emit 段时引用. 是 LLVM 对象文件层对 RISC-V ABI 的最小适配.
//
// <NT> 关键函数串联:
//   工厂入口:
//     (由 RISCVMCTargetDesc::createRISCVMCObjectFileInfo 创建)
//   关键 override:
//     getTextSectionAlignment   .text 段对齐字节数
//       ├─ 含 Zca 子集        对齐 2 字节 (RVC 友好, 函数入口 2 字节对齐即可)
//       └─ 其它情况             对齐 4 字节 (与 GCC/binutils 一致)
//   注: 本文件基本没有主入口, 几乎所有"工作"都集中在构造体 + 这一个 override.
//
// <NT> 总结:
//   本文件是 RISC-V 对象文件元数据层. 三大职责:
//     1) 段对齐策略: 根据是否启用 Zca 决定 .text 段对齐 (2 vs 4 字节),
//        让 RVC 压缩指令的函数入口能落到 2 字节对齐地址.
//     2) ABI 适配: 把 RISC-V ELF 对象文件与 GCC/binutils 工具链对齐,
//        保证 LLVM 编译产物能被现有 GNU 链接器处理.
//     3) 工厂接入: 把自己创建的 ObjectFileInfo 实例注册到 MCContext, 让
//        Streamer / AsmBackend 后续能正确发出段元数据.
//   推荐阅读顺序: 构造体 -> getTextSectionAlignment -> 看 RISCVMCTargetDesc 注册入口.
//   所有 NT 注释均以 "// <NT>" 开头, 方便搜索定位.
#include "RISCVMCObjectFileInfo.h"
#include "RISCVMCTargetDesc.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCSubtargetInfo.h"

using namespace llvm;

unsigned
RISCVMCObjectFileInfo::getTextSectionAlignment(const MCSubtargetInfo &STI) {
  return STI.hasFeature(RISCV::FeatureStdExtZca) ? 2 : 4;
}

unsigned RISCVMCObjectFileInfo::getTextSectionAlignment() const {
  return getTextSectionAlignment(*getContext().getSubtargetInfo());
}
