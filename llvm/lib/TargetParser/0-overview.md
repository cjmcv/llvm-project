<!-- <NT>overview:llvm/lib/TargetParser/ -->

# LLVM TargetParser 库导读 — `llvm/lib/TargetParser/`

> 本文档梳理 `llvm/lib/TargetParser/` 目录下全部源文件（18 个 `.cpp` + 2 个
> `.inc`，2 个子目录）的职责、上下游与推荐阅读顺序。目标读者：想理解 LLVM
> target 三元组（arch-vendor-os-env）解析、CPU/feature 字符串解析、host 探测、
> DataLayout 计算的开发者。
>
> TargetParser 是 LLVMTargetParser 库，被 `clang`、`llc`、`opt`、`llvm-mc`、
> `llvm-objdump` 等几乎所有 LLVM 工具静态链接。

---

## §0. TargetParser 在 LLVM 中的位置

TargetParser 是 LLVM **target 元信息层**——纯解析 / 查表层，**不含任何机器状态**。
它把外部字符串映射成 codegen 层 (`TargetSubtargetInfo` 等) 所需的数据结构:

- **输入**: 三元组字符串 (`aarch64-linux-gnu`)、CPU 名 (`skylake`/`gfx90a`/
  `cortex-a57`)、arch 名 (`armv8-a`/`rv64imafdc`)、ELF `EF_*` 标志、target
  feature 列表 (`+sve2,-neon`)、host 探测请求。
- **输出**: `Triple` (arch/vendor/OS/env/object-format 枚举包)、per-target feature
  位掩码 / 扩展集合、寄存器文件 / SGPR / VGPR / LDS / 向量长度等数值、ABI 名、
  DataLayout 字符串。

它在流水线中的位置:

```
clang driver (-target/-mcpu/-march/-mattr)
  ├─ TargetInfo / TargetLowering
  │    └─ Triple (本目录)
  ├─ TargetMachine
  │    └─ DataLayout (本目录的 TargetDataLayout.cpp)
  └─ TargetSubtargetInfo (lib/Target/<Arch>/)
       └─ SubtargetFeatures (本目录)
            └─ <Arch>TargetParser (本目录每个 target 的解析器)
                 └─ TableGen 生成 *.def / *.inc
```

**编译注意**: `LLVMTargetParser` 在 CMakeLists.txt 中设
`-Werror=global-constructors`——它被每个 LLVM 工具静态链接，任何 static
initializer 都会在 `clang`/`llc`/`opt` 等工具中重复, 所以本目录所有数据都
必须是 `constexpr` 或函数局部。

---

## §1. 编译流水线概览

```
┌────────────────────────────────────────┐
│ 用户输入字符串                            │
│   · -target aarch64-linux-gnu           │
│   · -mcpu=cortex-a57                    │
│   · -march=rv64imafdc                   │
│   · -mattr=+sve2,-neon                  │
└────────────────────────────────────────┘
                │
                ▼
┌──────────────────────────────────────────────────────────┐
│ TargetParser  (本目录, 纯解析/查表)                       │
│   · [`Triple.cpp`](Triple.cpp): 解析三元组 → 枚举                        │
│   · [`Host.cpp`](Host.cpp):    探测 host 平台                          │
│   · [`SubtargetFeature.cpp`](SubtargetFeature.cpp): 拆分 +/- feature 字符串         │
│   · [`TargetDataLayout.cpp`](TargetDataLayout.cpp): Triple+ABI → DataLayout 字符串 │
│   · <Arch>[`TargetParser.cpp`](TargetParser.cpp): per-target CPU/feature 查表  │
└──────────────────────────────────────────────────────────┘
                │
                ▼
┌────────────────────────────────────────────────────────────┐
│ lib/Target/<Arch>/  后端                                    │
│   · TargetMachine → TargetSubtargetInfo → ISel/RegAlloc   │
└────────────────────────────────────────────────────────────┘
```

辅助入口:

- **host 探测**: [`Host.cpp`](Host.cpp) 提供 `sys::getHostCPUName` /
  `sys::getDefaultTargetTriple`, 在用 `-march=native` 或默认编译时使用。
- **平台特定实现**: [`Unix/Host.inc`](Unix/Host.inc) 与 [`Windows/Host.inc`](Windows/Host.inc)
  是平台相关的 `getDefaultTargetTriple` 实现, 由 [`Host.cpp`](Host.cpp) 按
  `LLVM_ON_UNIX` / `_WIN32` 选择 include。

---

## §2. 文件目录结构

```
llvm/lib/TargetParser/
├── CMakeLists.txt
├── 顶层 16 个 .cpp        ← 见 §3.1 / §3.2 分类
├── Unix/Host.inc         ← Unix 平台默认三元组
└── Windows/Host.inc      ← Windows 平台默认三元组
```

### 2.1 顶层文件按职责分类

| # | 分类 | 文件数 | 核心标识符 |
|---|------|--------|-----------|
| 1 | 公共核心组件 | 5 | `Triple`, `SubtargetFeatures`, `DataLayout`, `getCPUDefaultTargetFeatures` |
| 2 | ARM/AArch64 系列 | 3 | `AArch64::*`, `ARM::*`, `ARMTargetParserCommon` |
| 3 | RISC-V 系列 | 2 | `RISCVISAInfo`, `RISCVTargetParser` |
| 4 | x86 / x86-64 | 1 | `X86::*` |
| 5 | GPU (AMD/NVPTX) | 2 | `AMDGPU::*`, `NVPTX::*` |
| 6 | 其它架构 | 5 | AVR / CSKY / LoongArch / PPC / Xtensa |

---

## §3. 文件详解

### 3.1 公共核心组件

[`Triple.cpp`](Triple.cpp) — `Triple` 类实现, 把 `arch-vendor-os-env[-object-format]`
字符串解析为 `ArchType`/`VendorType`/`OSType`/`EnvironmentType` 枚举, 并提供
endianness / 64-bit / OS 类型查询接口.
- 上游: clang driver、`TargetMachine` 构造、`Module::getTargetTriple()`
  访问器、SubtargetFeature 解析.
- 下游: `StringSwitch`, `VersionTuple`; 反向调用 [`ARMTargetParser`](ARMTargetParser.cpp) /
  [`ARMTargetParserCommon`](ARMTargetParserCommon.cpp) /
  [`AMDGPUTargetParser`](AMDGPUTargetParser.cpp) / [`Host.cpp`](Host.cpp)。
- 关键类/函数: `class Triple`, `Triple::parseArch/parseVendor/parseOS/parseEnvironment`,
  `Triple::normalize`, `Triple::getArchName`, `Triple::isArch64Bit/isArch32Bit/isLittleEndian`,
  `Triple::isOSWindows/isOSDarwin/isAndroid`, `Triple::computeDataLayout`,
  `enum ArchType/SubArchType/VendorType/OSType/EnvironmentType/ObjectFormatType`.

[`Host.cpp`](Host.cpp) — host 平台探测 (`sys::getHostCPUName`,
`sys::getDefaultTargetTriple`, `sys::getProcessTriple`)。
- 上游: clang driver (`-mtune=native`, 默认三元组)、`TargetMachine` 构造
  (`-march=native` 时)、JIT 运行时查询。
- 下游: [`Unix/Host.inc`](Unix/Host.inc) 与 [`Windows/Host.inc`](Windows/Host.inc)
  (平台相关实现)、`<cpuid.h>`、`<sys/sysctl.h>` (Darwin)、`<sys/systemcfg.h>`
  (AIX)、`<kstat.h>` (Solaris); 调用
  [`RISCVTargetParser`](RISCVTargetParser.cpp) /
  [`X86TargetParser`](X86TargetParser.cpp) /
  [`Triple.cpp`](Triple.cpp)。
- 关键类/函数: `sys::getHostCPUName`, `sys::getDefaultTargetTriple`,
  `sys::getProcessTriple`, `sys::getOSVersion`, `sys::printDefaultTargetAndDetectedCPU`,
  `sys::detail::getHostCPUNameFor{ARM,PowerPC,S390x,RISCV,BPF,SPARC,x86}`,
  `RISCVHwProbe`.

[`SubtargetFeature.cpp`](SubtargetFeature.cpp) — `SubtargetFeatures` 容器,
处理逗号分隔的 feature 字符串 (`+sve2,-neon`) 与 Triple-keyed 默认值.
- 上游: 每个后端 `TargetSubtargetInfo::ParseSubtargetFeatures`
  (在 `lib/Target/<Arch>/` 下), clang 的 `-mattr` 处理。
- 下游: `llvm/ADT/StringExtras` (`join`/`lower_bound`), [`Triple.cpp`](Triple.cpp)
  的 `getVendor/getArch`。
- 关键类/函数: `class SubtargetFeatures`, `SubtargetFeatures::Split`,
  `SubtargetFeatures::AddFeature`, `SubtargetFeatures::addFeaturesVector`,
  `SubtargetFeatures::getString`, `SubtargetFeatures::print/dump`,
  `SubtargetFeatures::getDefaultSubtargetFeatures`.

[`TargetDataLayout.cpp`](TargetDataLayout.cpp) — `Triple::computeDataLayout` 实现,
根据 Triple + ABI 名生成 LLVM `DataLayout` 字符串。
- 上游: `Triple::computeDataLayout` 调用方 (`Module::setDataLayout`,
  `TargetMachine::createDataLayout`, clang `-m<triple>-data-layout`).
- 下游: [`Triple.cpp`](Triple.cpp) 的 endianness / 64-bit / Mach-O 标志查询;
  [`ARMTargetParser.cpp`](ARMTargetParser.cpp) 的 `ARM::computeTargetABI`.
- 关键类/函数: `Triple::computeDataLayout`, `getManglingComponent`,
  `computeARMDataLayout/computeAArch64DataLayout/computeBPFDataLayout/
  computeMipsDataLayout/computePowerDataLayout/computeSparcDataLayout/
  computeX86DataLayout/computeNVPTXDataLayout/computeLanaiDataLayout/
  computeRISCVDataLayout/computeWebAssemblyDataLayout/computeVEDataLayout`.

[`TargetParser.cpp`](TargetParser.cpp) — 通用 CPU 默认 feature 展开工具, 从
TableGen 风格的 `BasicSubtargetSubTypeKV` / `BasicSubtargetFeatureKV` 表计算
隐含 feature 位。
- 上游: PPC 后端, 以及任何想用位掩码隐含扩展的目标.
- 下游: `llvm/ADT/ArrayRef`, `llvm::lower_bound`, `StringMap`.
- 关键类/函数: `getCPUDefaultTargetFeatures`, `setImpliedBits`, `find`
  (在 `BasicSubtargetSubTypeKV` 上二分查找).

### 3.2 ARM / AArch64 系列

[`AArch64TargetParser.cpp`](AArch64TargetParser.cpp) — AArch64 CPU/arch/extension
解析, 处理 `-mcpu` / `-march` / `-mattr` 字符串。
- 上游: AArch64 后端 `AArch64Subtarget`, clang `AArch64TargetInfo::getArchFeatures`,
  `clang driver -print-supported-extensions`。
- 下游: `AArch64TargetParserDef.inc` (TableGen 生成 `CpuInfos/ArchInfos/Extensions/
  CpuAliases/FMVInfo/ExtensionDependencies`), [`ARMTargetParserCommon.cpp`](ARMTargetParserCommon.cpp),
  [`Triple.cpp`](Triple.cpp).
- 关键类/函数: `AArch64::parseArch/parseCpu/parseArchExtension/parseFMVExtension`,
  `AArch64::getArchForCpu`, `AArch64::resolveCPUAlias/getArchExtFeature`,
  `AArch64::getFMVPriority/getCpuSupportsMask`,
  `AArch64::fillValidCPUArchList`, `AArch64::isX18ReservedByDefault`,
  `AArch64::ExtensionSet::enable/disable/addCPUDefaults/addArchDefaults/parseModifier`,
  `AArch64::PrintSupportedExtensions`.

[`ARMTargetParser.cpp`](ARMTargetParser.cpp) — ARM CPU/FPU/Arch/extension/HWDiv
解析与 ABI 选择。
- 上游: ARM 后端 `ARMSubtarget`, clang `ARMTargetInfo`, 驱动层
  `-march=armv7-a` 类 flag.
- 下游: `ARMTargetParser.def`, [`ARMTargetParserCommon.cpp`](ARMTargetParserCommon.cpp),
  [`Triple.cpp`](Triple.cpp).
- 关键类/函数: `ARM::parseArch/parseArchVersion/parseArchProfile/parseArchExt/parseFPU/parseCPUArch/parseHWDiv`,
  `ARM::getFPUFeatures/getHWDivFeatures/getExtensionFeatures/appendArchExtFeatures`,
  `ARM::getDefaultCPU/getDefaultFPU/getDefaultExtensions/getARMCPUForArch`,
  `ARM::getArchName/getSubArch/getCPUAttr/getArchAttr/getArchExtName/getArchExtFeature`,
  `ARM::computeDefaultTargetABI/computeTargetABI`, `ARM::convertV9toV8`,
  `ARM::fillValidCPUArchList/PrintSupportedExtensions`.

[`ARMTargetParserCommon.cpp`](ARMTargetParserCommon.cpp) — ARM/AArch64 共享 helper:
arch 名规范化、ISA/endianness 检测、branch-protection 解析。
- 上游: [`ARMTargetParser.cpp`](ARMTargetParser.cpp) 的 `parseArch/parseArchVersion`,
  [`AArch64TargetParser.cpp`](AArch64TargetParser.cpp) 的 `parseArch`, clang AArch64
  `-mbranch-protection`.
- 下游: `llvm/ADT/StringSwitch`.
- 关键类/函数: `ARM::getArchSynonym/getCanonicalArchName`,
  `ARM::parseArchISA/parseArchEndian/parseBranchProtection`,
  `enum ISAKind/EndianKind`, `struct ParsedBranchProtection`.

### 3.3 RISC-V 系列

[`RISCVISAInfo.cpp`](RISCVISAInfo.cpp) — 解析与校验 RISC-V arch 字符串
(`rv64imafdc_zicsr...`), 处理扩展存在性、版本、隐含图 (`G = i+m+a+f+d`)。
- 上游: RISC-V 后端 `RISCVSubtarget`, clang RISC-V 驱动
  (`-march=rv64imafdc`), [`RISCVTargetParser.cpp`](RISCVTargetParser.cpp) 的
  `parseArchString`。
- 下游: `RISCVTargetParserDef.inc` (TableGen 生成 `SupportedExtensions/
  SupportedExperimentalExtensions/SupportedProfiles`), `llvm/ADT/StringExtras`,
  `llvm/Support/Error`.
- 关键类/函数: `class RISCVISAInfo`,
  `RISCVISAInfo::parseArchString/parseFeatures/parseNormalizedArchString/createFromExtMap`,
  `RISCVISAInfo::isSupportedExtension/isSupportedExtensionFeature/isSupportedExtensionWithVersion/hasExtension`,
  `RISCVISAInfo::toFeatures/toString/getTargetFeatureForExtension/computeDefaultABI`,
  `RISCVISAInfo::printSupportedExtensions/printEnabledExtensions`,
  `RISCVISAInfo::postProcessAndChecking/checkDependency/updateImplication/updateCombination/updateImpliedLengths`.

[`RISCVTargetParser.cpp`](RISCVTargetParser.cpp) — RISC-V CPU/arch/feature 查表,
向量扩展的 V-type 编码。
- 上游: RISC-V 后端 `RISCVSubtarget`/`RISCVTargetInfo`, clang 驱动,
  [`Host.cpp`](Host.cpp) 的 `getHostCPUNameForRISCV`。
- 下游: `RISCVTargetParserDef.inc` (TableGen 生成 `PROC/TUNE_PROC`),
  [`RISCVISAInfo.cpp`](RISCVISAInfo.cpp), `llvm/ADT/StringTable`.
- 关键类/函数: `RISCV::parseCPU/parseTuneCPU`, `RISCV::getMArchFromMcpu/getFeaturesForCPU`,
  `RISCV::hasFastScalarUnalignedAccess/hasFastVectorUnalignedAccess/hasValidCPUModel/getCPUModel/getCPUNameFromCPUModel`,
  `RISCV::fillValidCPUArchList/fillValidTuneCPUArchList`,
  `RISCVTuneFeatureLookupTable::getAllTuneFeatures/getConfigurableFeatures`,
  `RISCVVType::encodeVTYPE/decodeVLMUL/printVType/getLambda/getLambdaEncoding/isAltFmtA/isAltFmtB`,
  `RISCV::getCPUConfigurableTuneFeatures`, `enum CPUKind`,
  `struct CPUInfo/CPUModel/ParserError/ParserWarning`.

### 3.4 x86 / x86-64

[`X86TargetParser.cpp`](X86TargetParser.cpp) — X86 CPU/feature/64-bit tier 解析
(`-march=x86-64-v3`), CPU dispatch 验证。
- 上游: X86 后端 `X86Subtarget`, clang X86 驱动, clang `-mcpu=registryspecific`
  / CPU-dispatch.
- 下游: `X86TargetParser.def` (TableGen `X86_FEATURE/X86_CPU`), `llvm/ADT/Bitset`,
  `llvm/ADT/Enum`.
- 关键类/函数: `X86::parseArchX86/parseTuneCPU`,
  `X86::fillValidCPUArchList/fillValidTuneCPUList`,
  `X86::getCPUDispatchMangling/validateCPUSpecificCPUDispatch`,
  `X86::getFeaturesForCPU`, `X86::updateImpliedFeatures`,
  `struct ProcInfo`, `enum CPUKind`,
  `constexpr` `Features*` 位掩码 (`FeaturesPentiumMMX`..`FeaturesGraniteRapids`).

### 3.5 GPU 解析器 (AMD/NVPTX)

[`AMDGPUTargetParser.cpp`](AMDGPUTargetParser.cpp) — AMDGPU/R600 GPU kind 解析,
寄存器文件 / SGPR / VGPR / LDS 计数, TargetID (xnack/sramecc)。
- 上游: AMDGPU 后端 `AMDGPUSubtarget`/`SIMachineFunctionInfo`, clang
  `AMDGPUTargetInfo`, HIP/OpenMP offload 编译。
- 下游: `AMDGPUTargetParserDef.inc`, `R600TargetParserDef.inc`
  (TableGen 生成), `Triple::SubArchType`, `llvm/ADT/StringTable`.
- 关键类/函数: `class AMDGPU::TargetID`, `enum class TargetIDSetting`,
  `parseArchAMDGCN/parseArchR600`, `getArchNameAMDGCN/getArchNameR600/getArchFamilyNameAMDGCN`,
  `getSubArch/getMajorSubArch/getSubArchName`,
  `fillAMDGPUFeatureMap/fillAMDGCNFeatureMap`,
  `getTotalNumSGPRs/getAddressableNumSGPRs/getSGPRAllocGranule/getVGPRAllocGranule/getTotalNumVGPRs/getMaxHWAddressableLocalMemorySize/getLDSBankCount/getMaxWavesPerEU/getIsaVersion`,
  `isCPUValidForSubArch/isSubArchCompatible/isPseudoTarget`,
  `TargetID::parse/parseTargetIDString/isEquivalent/providesFor/print`.

[`NVPTXTargetParser.cpp`](NVPTXTargetParser.cpp) — NVPTX GPU kind 解析,
`sm_xx` ↔ `GPUKind`。
- 上游: NVPTX 后端 `NVPTXSubtarget`, clang CUDA `-march=sm_xx`, `ptxas`
  等价 codegen.
- 下游: `NVPTXTargetParser.def` (TableGen 生成), `llvm/ADT/StringSwitch`.
- 关键类/函数: `NVPTX::parseArch`, `NVPTX::getArchName/getVirtualArch/getSmVersion/getArchSuffix`,
  `enum ArchSuffix`, `enum GPUKind`.

### 3.6 其它架构

[`AVRTargetParser.cpp`](AVRTargetParser.cpp) — 把 AVR ELF `EF_AVR_ARCH_*` 标志
映射回 feature 字符串。
- 上游: AVR 后端 `AVRSubtarget` (读取 ELF 标志), `llvm-objdump` AVR 模式。
- 下游: `llvm/BinaryFormat/ELF.h` (`EF_AVR_ARCH_*`), `llvm/ADT/DenseMap`.
- 关键类/函数: `AVR::getFeatureSetFromEFlag` (唯一公开函数).

[`CSKYTargetParser.cpp`](CSKYTargetParser.cpp) — C-SKY CPU/arch/FPU/extension
解析。
- 上游: CSKY 后端 `CSKYSubtarget`, clang CSKY 驱动。
- 下游: `CSKYTargetParser.def` (TableGen 生成), `llvm/ADT/StringSwitch`.
- 关键类/函数: `CSKY::parseArch/parseCPUArch/parseArchExt`,
  `CSKY::getFPUFeatures/getExtensionFeatures/getDefaultExtensions`,
  `CSKY::getArchName/getDefaultCPU/getFPUName/getFPUVersion/getArchExtName/getArchExtFeature`,
  `CSKY::fillValidCPUArchList`.

[`LoongArchTargetParser.cpp`](LoongArchTargetParser.cpp) — LoongArch arch/feature
名查表 (`la64v1.0`, `la32v1.0` 等)。
- 上游: LoongArch 后端 `LoongArchSubtarget`, clang LoongArch 驱动。
- 下游: `LoongArchTargetParser.def` (TableGen 生成 `AllFeatures/AllArchs`).
- 关键类/函数: `LoongArch::isValidArchName/isValidFeatureName/isValidCPUName`,
  `LoongArch::getArchFeatures`, `LoongArch::fillValidCPUList`,
  `LoongArch::getDefaultArch`, `struct FeatureInfo/ArchInfo`.

[`PPCTargetParser.cpp`](PPCTargetParser.cpp) — PPC CPU 归一化、默认 feature
查表、AIX 默认 CPU 选择。
- 上游: PPC 后端 `PPCSubtarget`, clang PowerPC 驱动,
  [`Host.cpp`](Host.cpp) 的 `getHostCPUName`.
- 下游: `PPCTargetParser.def`, `PPCGenTargetFeatures.inc`
  (TableGen 生成 `BasicPPCSubTypeKV`/`BasicPPCFeatureKV`),
  [`TargetParser.cpp`](TargetParser.cpp) 的 `getCPUDefaultTargetFeatures`,
  `Host.h`.
- 关键类/函数: `PPC::normalizeCPUName/getNormalizedPPCTargetCPU/getNormalizedPPCTuneCPU`,
  `PPC::fillValidCPUList/fillValidTuneCPUList`, `PPC::isValidCPU/isValidFeatureName`,
  `PPC::getPPCDefaultTargetFeatures`, `struct CPUInfo`.

[`XtensaTargetParser.cpp`](XtensaTargetParser.cpp) — Xtensa CPU/feature 名
→ `CPUKind` + feature 位, 含别名映射。
- 上游: Xtensa 后端 `XtensaSubtarget`, clang Xtensa 驱动。
- 下游: `XtensaTargetParser.def` (TableGen `XTENSA_FEATURE/XTENSA_CPU/XTENSA_CPU_ALIAS`),
  `llvm/ADT/Enum`, `llvm/ADT/StringSwitch`.
- 关键类/函数: `Xtensa::parseCPUKind`, `Xtensa::getBaseName/getAliasName/getCPUFeatures`,
  `Xtensa::fillValidCPUList`, `struct CPUInfo`.

### 3.7 平台特定实现

[`Unix/Host.inc`](Unix/Host.inc) — Unix 平台 `sys::getDefaultTargetTriple` 实现,
基于 `uname()` 加上 Darwin/macOS/AIX/PASE 的 OS version 标记。

[`Windows/Host.inc`](Windows/Host.inc) — Win32 平台 `sys::getDefaultTargetTriple`
存根, 直接返回 `LLVM_DEFAULT_TARGET_TRIPLE`, 支持环境变量 override.

---

## §4. 关键调用链

### 4.1 三元组解析 → DataLayout 构造

```
clang driver 解析 -target aarch64-linux-gnu
  └─ Triple::Triple("aarch64-linux-gnu")
      ├─ Triple::parseArch → ArchType = aarch64
      ├─ Triple::parseVendor → VendorType = unknown (linux-gnu 不带 vendor)
      ├─ Triple::parseOS → OSType = linux
      ├─ Triple::parseEnvironment → EnvironmentType = gnu
      └─ Triple::normalize → Triple 标准化 (vendor 推断等)

Triple::computeDataLayout
  ├─ getManglingComponent
  └─ computeAArch64DataLayout (在 TargetDataLayout.cpp)
       └─ ARM::computeTargetABI (在 ARMTargetParser.cpp)
            → DataLayout 字符串

Module::setDataLayout
  └─ 设置 IR 的 dlstr, 后续 IR 层使用
```

### 4.2 host 探测 (-march=native)

```
clang -march=native -mtune=native
  └─ TargetMachine 构造 (-march=native 触发)
       └─ sys::getHostCPUName
            ├─ Unix/Host.inc: getDefaultTargetTriple (基于 uname)
            └─ sys::detail::getHostCPUNameFor<Arch>:
                 ├─ X86: cpuid
                 ├─ ARM: HWCAP / /proc/cpuinfo
                 ├─ RISCV: RISCVHwProbe / /proc/cpuinfo
                 ├─ PPC: /proc/cpuinfo
                 ├─ SPARC: sysinfo
                 ├─ S390x: store-system-information
                 └─ BPF: 固定值
       → CPU 名字符串 ("cortex-a57", "skylake" 等)
            └─ <Arch>TargetParser::parseCpu
                 → CPUKind + 默认 feature
                      └─ TargetSubtargetInfo 构造
```

### 4.3 Subtarget feature 解析 (-mattr)

```
clang -mattr=+sve2,-neon,+sve2-aes
  └─ TargetOptions::Features (vector<string>)
       └─ SubtargetFeatures(features)
            ├─ SubtargetFeatures::Split → ("+sve2", "-neon", "+sve2-aes")
            ├─ SubtargetFeatures::AddFeature:
            │    ├─ '+': 设置 feature 位
            │    ├─ '-': 清除 feature 位
            │    └─ 无前缀: 同 '+'
            └─ SubtargetFeatures::getString → " +sve2,-neon,+sve2-aes"

TargetSubtargetInfo::ParseSubtargetFeatures
  ├─ <Arch>TargetParser::parseArchExtension (per-target)
  ├─ <Arch>TargetParser::parseFMVExtension (AArch64 FMV)
  └─ updateImpliedFeatures (X86) / updateImplication (RISCV)
       → FeatureBits
```

### 4.4 RISC-V ISA 字符串解析 (-march=rv64imafdc_zicsr)

```
clang -march=rv64imafdc_zicsr
  └─ RISCVISAInfo::parseArchString("rv64imafdc_zicsr")
       ├─ 解析前缀 rv64 → 64-bit base
       ├─ 按 _ 拆分扩展: imafdc + zicsr
       ├─ checkDependency: 验证依赖 (G = i+m+a+f+d)
       ├─ updateImplication: 处理隐含 (D 隐含 F, F 隐含 Zicsr 等)
       ├─ updateCombination: 处理组合 (V 隐含 Zve32x 等)
       ├─ updateImpliedLengths: 计算 LMUL/SEW/ELEN 等长度参数
       └─ RISCVISAInfo → Features + Extensions + ABI

RISCVTargetParser::parseArchString
  └─ 创建 RISCVISAInfo 并 setFeatures
       └─ 传给 RISCVSubtarget
```

---

## §5. 推荐阅读顺序

### 阶段 1: 基础数据模型 (1-2 小时)
1. [`Triple.cpp`](Triple.cpp) — 三元组解析的基础。看 `parseArch/normalize` 与
   `computeDataLayout`。
2. [`TargetDataLayout.cpp`](TargetDataLayout.cpp) — Triple → DataLayout 的转换。

### 阶段 2: Feature 处理 (1 小时)
1. [`SubtargetFeature.cpp`](SubtargetFeature.cpp) — `+/-` 字符串处理。
2. [`TargetParser.cpp`](TargetParser.cpp) — CPU 默认 feature 展开 (位掩码隐含)。

### 阶段 3: host 探测 (1 小时)
1. [`Host.cpp`](Host.cpp) — 入口与跨平台分发。
2. [`Unix/Host.inc`](Unix/Host.inc) 与 [`Windows/Host.inc`](Windows/Host.inc) — 平台实现。

### 阶段 4: 一个 target 的完整解析 (以 AArch64 为例, 1-2 小时)
1. [`AArch64TargetParser.cpp`](AArch64TargetParser.cpp) — CPU/arch/extension 解析主表。
2. [`ARMTargetParserCommon.cpp`](ARMTargetParserCommon.cpp) — ARM/AArch64 共享。
3. [`ARMTargetParser.cpp`](ARMTargetParser.cpp) — ARM 端。

### 阶段 5: RISC-V (1-2 小时, 与阶段 4 可选)
1. [`RISCVTargetParser.cpp`](RISCVTargetParser.cpp) — CPU/feature 查表。
2. [`RISCVISAInfo.cpp`](RISCVISAInfo.cpp) — ISA 字符串校验 + 隐含图。

### 阶段 6: 其它架构 (按需)
1. [`X86TargetParser.cpp`](X86TargetParser.cpp) — 位掩码隐含 (`updateImpliedFeatures`)。
2. [`AMDGPUTargetParser.cpp`](AMDGPUTargetParser.cpp) — GPU 资源计数 (SGPR/VGPR/LDS)。
3. [`NVPTXTargetParser.cpp`](NVPTXTargetParser.cpp) — GPU kind 解析。
4. [`PPCTargetParser.cpp`](PPCTargetParser.cpp) — AIX 默认 CPU + CPU 归一化。
5. [`CSKYTargetParser.cpp`](CSKYTargetParser.cpp) / [`LoongArchTargetParser.cpp`](LoongArchTargetParser.cpp) /
   [`XtensaTargetParser.cpp`](XtensaTargetParser.cpp) / [`AVRTargetParser.cpp`](AVRTargetParser.cpp) — 较小, 各 1 小时。

---

## §6. 常用操作指南

### 6.1 添加新 CPU 到现有架构 (以 RISC-V 为例)

1. 在 `llvm/include/llvm/TargetParser/RISCVTargetParser.td` 加新 CPU 定义
   (在 `let Proc = ...` 块中)。
2. 跑 `ninja` 触发 `RISCVTargetParserDef.inc` 重新生成。
3. 在 [`RISCVTargetParser.cpp`](RISCVTargetParser.cpp) 中如需新字段, 在
   `struct CPUInfo` 加成员。
4. 如需 tune 字段, 同步更新 `RISCVTuneFeatureLookupTable`。
5. 单测: `llvm/test/tools/llvm-mc/riscv/` 与 `llvm/unittests/TargetParser/`.

### 6.2 添加新 ISA 扩展 (RISC-V)

1. 在 `RISCVTargetParser.td` 的 `SupportedExtensions` 列表加新条目。
2. 跑 `ninja` 重生 `RISCVTargetParserDef.inc`.
3. 在 [`RISCVISAInfo.cpp`](RISCVISAInfo.cpp) 的 `checkDependency` /
   `updateImplication` / `updateCombination` 中更新隐含关系 (如新扩展依赖
   已有扩展, 需在此声明)。
4. 单测: `llvm/test/MC/RISCV/` 与 `llvm/unittests/TargetParser/RISCVISAInfoTest.cpp`.

### 6.3 添加新 target (新架构)

1. 在 `llvm/include/llvm/TargetParser/` 创建 `NewArchTargetParser.td` 与 `.h`.
2. 在 `llvm/lib/TargetParser/` 创建 `NewArchTargetParser.cpp`, 实现:
   - `parseCpu/parseTuneCPU` (CPU 字符串 → 枚举)
   - `fillValidCPUArchList` (列举所有支持的 CPU)
   - 配套的 feature map
3. 在 `llvm/lib/TargetParser/CMakeLists.txt` 加新源文件。
4. 在 `llvm/include/llvm/TargetParser/CMakeLists.txt` 加新头文件。
5. 后端 `llvm/lib/Target/NewArch/` 在 `TargetSubtargetInfo` 中调用本目录 API.
6. 单测: `llvm/unittests/TargetParser/NewArchTargetParserTest.cpp`.

### 6.4 添加新 host 平台

1. 在 [`Unix/Host.inc`](Unix/Host.inc) 或 [`Windows/Host.inc`](Windows/Host.inc)
   中加新 OS 的 `uname()` 分支处理 (Unix) 或 registry 查询 (Windows)。
2. 如需新 CPU 探测方法, 在 [`Host.cpp`](Host.cpp) 加新 `getHostCPUNameForXXX` 函数。
3. 在 `llvm/CMakeLists.txt` 加新平台的 `LLVM_HAS_*` 配置 (如适用).
4. 单测: `llvm/test/Support/Host.cpp` 与平台特定单元测试。

### 6.5 添加新 triple 组件 (新 OS/Environment)

1. 在 [`Triple.cpp`](Triple.cpp) 的对应枚举 (`OSType` / `EnvironmentType`) 加新值。
2. 更新 `parseOS` / `parseEnvironment` 的 StringSwitch。
3. 更新 `getOSName` / `getEnvironmentName` 的反向映射。
4. 在 [`TargetDataLayout.cpp`](TargetDataLayout.cpp) 如需新 DataLayout, 加
   `computeNewArchDataLayout`.
5. 单测: `llvm/unittests/ADT/TripleTest.cpp` 与 `llvm/test/CodeGen/`.

---

## §7. NT 注释索引

当前 `llvm/lib/TargetParser/` 下尚无 `// <NT>` 注释。可结合以下 skill 使用:

- `nt-comments` — 给单文件加 NT 中文注释。
- 姊妹 overview:
  - [`llvm/lib/Target/RISCV/0-overview.md`](../Target/RISCV/0-overview.md) — RISCV 后端
  - [`llvm/lib/CodeGen/0-overview.md`](../CodeGen/0-overview.md) — target-independent CodeGen
  - [`llvm/lib/IR/0-overview.md`](../IR/0-overview.md) — LLVM IR 层

按"少而精"原则, 添加 NT 注释时建议:
- [`Triple.cpp`](Triple.cpp) — 加 5-8 段 (核心数据结构, 多个解析入口)
- [`RISCVISAInfo.cpp`](RISCVISAInfo.cpp) — 加 5-8 段 (复杂的隐含图)
- [`Host.cpp`](Host.cpp) — 加 3-5 段
- 单 target parser 文件 — 加 2-3 段 (parseCpu + fillValidCPUArchList)
- `.inc` 文件 — 不加注释 (TableGen 生成)