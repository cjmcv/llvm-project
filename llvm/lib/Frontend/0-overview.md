<!-- <NT>overview:llvm/lib/Frontend/ -->

# LLVM Frontend 库导读 — `llvm/lib/Frontend/`

> 本文档梳理 `llvm/lib/Frontend/` 目录下全部源文件（20 个 `.cpp`，分布在 7 个子目录）的职责、上下游与推荐阅读顺序。目标读者：想理解 LLVM **前端桥接层**（HLSL / OpenMP / OpenACC / Atomic / Offloading / Driver 选项）的开发者。
>
> **重要区分**: 本目录**不是** Clang C/C++ 前端（Clang 在 `llvm/tools/clang/`）。`lib/Frontend` 提供的是 **"source frontend → LLVM IR"** 之间的桥接组件，给 Clang / Flang / DirectX 后端 / LTO / 链接器共享使用。
>
> 所有路径相对 `llvm/lib/Frontend/`。头文件全部位于 `llvm/include/llvm/Frontend/`。

---

## §0. Frontend 库在 LLVM 中的位置

`lib/Frontend` 是 LLVM **前端桥接层**——为各 source-language 前端（Clang C/C++/HLSL、Flang、HIP/CUDA/SYCL wrapper、Clang Driver）在降到 LLVM IR 时提供**通用 helper**。

包含的内容（按子目录）:

- **Atomic/**: 共享 atomic codegen 基类 `AtomicInfo`，给 Clang 的 `AtomicExpr`、OpenMP、C11/HIP/CUDA atomic 等共用。
- **Directive/**: 通用"指令拼写-版本"区间查询（`Spelling`），被 OpenMP / OpenACC 共享。
- **Driver/**: Clang Driver 风格选项 ↔ LLVM 内部 `TargetLibraryInfo` 的小桥。
- **HLSL/**: HLSL ↔ IR 的桥梁——root signature 解析/校验/打印、cbuffer 元数据、resource 绑定、shader 语义签名打包。给 DirectX 后端 (DXIL) 用。
- **Offloading/**: GPU offloading 工具：CUDA/OpenMP/HIP/SYCL fatbinary 包装、`__tgt_*_register_lib` 注册、ELF note + msgpack 元数据。
- **OpenACC/**: OpenACC 指令枚举的 tablegen 入口（`#include "ACC.inc"`，由 `ACC.td` build 时生成）。
- **OpenMP/**: OpenMP 指令/子句/descriptor 查询 + 完整的 `OMPIRBuilder`（用 IRBuilder 拼装 OpenMP region）。

它在流水线中的位置:

```
源代码 (.c/.cpp/.hlsl/.f90/.cu)
   │
   ▼
┌────────────────────────────────────────────────────────┐
│ Source Frontend (Clang / Flang / DXC / clang-hlsl)      │
│   · Sema / Parse / CodeGen                              │
│   · 这里会 #include llvm/Frontend/... 的头              │
└────────────────────────────────────────────────────────┘
   │
   ├─→ Clang C/C++ → 调 Atomic.cpp (C11 atomics)
   │                → 调 OMPIRBuilder.cpp (OpenMP)
   │                → 调 Offloading/ (HIP/CUDA fatbinary)
   │                → 调 Driver/CodeGenOptions.cpp
   │
   ├─→ Clang HLSL → 调 HLSL/* (root signature + cbuffer + semantic)
   │              → 调 HLSLBinding.cpp (binding allocation)
   │              → 调 SemanticSignatures.cpp (PSV0 元数据)
   │
   ├─→ Flang  → 调 OpenMP/* + OMPIRBuilder
   │
   └─→ DXIL 后端 → 调 HLSL/* 读取 module 命名 metadata
                  → 调 Offloading/Utility.cpp 解析 ELF note
   │
   ▼
┌────────────────────────────────────────────────────────┐
│ LLVM IR (Module / Function / ...)                        │
└────────────────────────────────────────────────────────┘
   │
   ▼
lib/Transforms / lib/CodeGen / lib/Target/...
```

---

## §1. 编译流水线概览

以 HLSL → DXIL 为例:

```
HLSL 源 (.hlsl)
   │
   ▼
┌────────────────────────────────────────────────────────┐
│ Clang-HLSL (libclang + Frontend/HLSL/ headers)         │
│   · Sema: 检查 root signature                           │
│   · CodeGen: 生成 LLVM IR                               │
│   · 用 HLSLBinding.cpp 分配 register/space             │
│   · 用 CBuffer.cpp 把 cbuffer 写成命名 metadata         │
│   · 用 SemanticSignatures.cpp 写 PSV0                   │
└────────────────────────────────────────────────────────┘
   │
   ▼
┌────────────────────────────────────────────────────────┐
│ LLVM IR Module (带 hlsl.cbs / dx.rootSignature / ... ) │
└────────────────────────────────────────────────────────┘
   │
   ▼
┌────────────────────────────────────────────────────────┐
│ DXIL Backend (lib/Target/DirectX/)                       │
│   · 读 RootSignatureMetadata.cpp::ParseRootSignature    │
│   · 调 RootSignatureValidations.cpp 校验                │
│   · 用 HLSLResource.cpp 做 type 映射                    │
│   · 用 SemanticSignaturePacking.cpp 分配 startrow/col  │
└────────────────────────────────────────────────────────┘
   │
   ▼
DXIL / DXBC 字节码
```

以 Clang C++ OpenMP 为例:

```
C/C++ 源 + #pragma omp parallel
   │
   ▼
Clang OpenMP Sema (ParseOpenMP / SemaOpenMP)
   ├─ 用 Directive/Spelling.cpp::FindName 找版本拼写
   ├─ 用 OMP.cpp::getLeafConstructs / isCompositeConstruct 分类
   └─ 用 OMPDescriptors.cpp 查 clause/modifier 兼容性
   │
   ▼
Clang OpenMP CodeGen
   └─ 调 OMPIRBuilder.cpp::CreateParallel / applyWorkshareLoop
      ├─ 拼装 entry/exit basicblock
      ├─ 调 __kmpc_fork_call 调用 libomp
      └─ 用 Offloading/Utility.cpp::emitOffloadingEntry 注册目标入口
   │
   ▼
LLVM IR Module (带 @__tgt_offload_entry 全局)
```

辅助入口:

- **Clang**: [`CGAtomic.cpp`](../../../clang/lib/CodeGen/CGAtomic.cpp) 等通过 include [`llvm/Frontend/Atomic/Atomic.h`](../../include/llvm/Frontend/Atomic/Atomic.h) 调 [`Atomic.cpp`](Atomic/Atomic.cpp)。
- **HLSL**: Clang HLSL 与 DirectX 后端通过 include `llvm/Frontend/HLSL/*.h` 调对应 .cpp。
- **OpenMP**: Clang / Flang / MLIR / LTO/OpenMPOpt 都通过 include `llvm/Frontend/OpenMP/*.h` 调对应 .cpp。
- **Driver**: Clang 的 `CodeGenOptions` 序列化器通过 include [`CodeGenOptions.h`](../../include/llvm/Frontend/Driver/CodeGenOptions.h) 调 [`Driver/CodeGenOptions.cpp`](Driver/CodeGenOptions.cpp)。

---

## §2. 文件目录结构

```
llvm/lib/Frontend/
├── CMakeLists.txt                ← 仅 add_subdirectory 7 个子目录
├── Atomic/      (1 cpp)          ← libLLVMFrontendAtomic
├── Directive/   (1 cpp)          ← libLLVMFrontendDirective
├── Driver/      (1 cpp)          ← libLLVMFrontendDriver
├── HLSL/        (8 cpp)          ← libLLVMFrontendHLSL
├── Offloading/  (3 cpp)          ← libLLVMFrontendOffloading
├── OpenACC/     (1 cpp)          ← libLLVMFrontendOpenACC
└── OpenMP/      (5 cpp)          ← libLLVMFrontendOpenMP (最重)
```

### 2.1 文件按职责分类

| 子目录 | 文件数 | 核心标识符 | 给谁用 |
|--------|--------|-----------|--------|
| `Atomic/` | 1 | `AtomicInfo`, `AtomicCABI` | Clang C/C++ atomic、OpenMP、C11/HIP/CUDA atomic |
| `Directive/` | 1 | `directive::FindName`, `VersionRange` | OpenMP/OpenACC/未来其它 directive-driven 前端 |
| `Driver/` | 1 | `driver::createTLII`, `convertDriverVectorLibraryToVectorLibrary` | Clang / Flang driver |
| `HLSL/` | 8 | `RootSignature`, `CBuffer`, `BindingInfo`, `SemanticSignatureElement` | Clang HLSL + DirectX 后端 (DXIL) |
| `Offloading/` | 3 | `wrapOpenMPBinaries`, `wrapCudaBinary`, `PropertySet`, `emitOffloadingEntry` | Clang OpenMP/CUDA/HIP/SYCL + LLD linker |
| `OpenACC/` | 1 | `acc::Directive`, `acc::getDirectiveName` | Clang OpenACC Sema |
| `OpenMP/` | 5 | `OMPIRBuilder`, `OMPContext`, `OMPDescriptors`, `DirectiveNameParser`, `OMP.cpp` | Clang/Flang/MLIR OpenMP, OpenMPOpt pass |

---

## §3. 文件详解

### 3.1 Atomic/

[`Atomic/Atomic.cpp`](Atomic/Atomic.cpp) — `AtomicInfo` 抽象基类的 IR 构造
实现 (非循环的 atomic load/store/cmpxchg, 含 intrinsic + libcall 两条路径)。
- 上游: Clang `Sema`/`CodeGen`
  ([`CGAtomic.cpp`](../../../clang/lib/CodeGen/CGAtomic.cpp)); OpenMP
  CodeGen; HIP/CUDA/HLSL atomic lowering。
- 下游: [`llvm/IR/IRBuilder`](../../include/llvm/IR/IRBuilder.h),
  [`llvm/IR/Attributes`](../../include/llvm/IR/Attributes.h),
  [`llvm/IR/Module`](../../include/llvm/IR/Module.h); 仅生成 IR, 不再调其它 lib。
- 关键类/函数: `AtomicInfo::EmitAtomicLoadOp`, `EmitAtomicStoreLibcall`,
  `EmitAtomicCompareExchange`, `EmitAtomicLibcall`, `shouldCastToInt`,
  `decorateWithTBAA`, 派生于 [`llvm/Frontend/Atomic/Atomic.h`](../../include/llvm/Frontend/Atomic/Atomic.h)
  的纯虚接口。

### 3.2 Directive/

[`Directive/Spelling.cpp`](Directive/Spelling.cpp) — 通用"指令拼写-版本"
区间查询辅助 (`FindName` / `VersionRange` / `Contains`)。
- 上游: OpenMP/OpenACC 的 `getOpenMPDirectiveName` 系列; 未来其它
  directive-driven 前端 (HIP, SYCL?)。
- 下游: [`llvm/ADT/StringRef`](../../include/llvm/ADT/StringRef.h),
  [`llvm/Support/MathExtras`](../../include/llvm/Support/MathExtras.h); 只依赖
  STL/Support。
- 关键类/函数: `llvm::directive::FindName`, `directive::VersionRange`,
  `directive::Contains`。所有符号都在 [`llvm/Frontend/Directive/Spelling.h`](../../include/llvm/Frontend/Directive/Spelling.h)
  中声明。

### 3.3 Driver/

[`Driver/CodeGenOptions.cpp`](Driver/CodeGenOptions.cpp) — Driver 风格选项
(`driver::VectorLibrary` 等) ↔ LLVM 内部 `TargetLibraryInfo` 的小桥。
- 上游: Clang 的 `CodeGenOptions` 加载器; Flang driver; 各 frontend driver
  shim。
- 下游: [`llvm/Analysis/TargetLibraryInfo`](../Analysis/TargetLibraryInfo.cpp),
  [`llvm/IR/SystemLibraries`](../../include/llvm/IR/SystemLibraries.h),
  [`llvm/ProfileData/InstrProfCorrelator`](../../include/llvm/ProfileData/InstrProfCorrelator.h),
  [`llvm/TargetParser/Triple`](../TargetParser/Triple.cpp),
  [`llvm/Support/CommandLine`](../../include/llvm/Support/CommandLine.h)。
- 关键类/函数: `convertDriverVectorLibraryToVectorLibrary`,
  `driver::createTLII`, `driver::getDefaultProfileGenName`,
  `llvm::driver::VectorLibrary` enum。

### 3.4 HLSL/

[`HLSL/CBuffer.cpp`](HLSL/CBuffer.cpp) — 解析 IR 命名 metadata `hlsl.cbs`,
把 cbuffer handle 与成员变量映射回 `CBufferMapping` 列表。
- 上游: DXIL 后端 passes (`HLSLResourceImpl` / `Inline` /
  `LowerResourceHandle`); Clang HLSL CodeGen。
- 下游: [`llvm/IR/DerivedTypes`](../../include/llvm/IR/DerivedTypes.h),
  [`llvm/IR/Metadata`](../../include/llvm/IR/Metadata.h),
  [`llvm/IR/Module`](../../include/llvm/IR/Module.h)。
- 关键类/函数: `CBufferMetadata::get`, `CBufferMetadata::eraseFromModule`,
  `getMemberOffsets` (static)。

[`HLSL/HLSLBinding.cpp`](HLSL/HLSLBinding.cpp) — register/space 分配与重叠
检测: 把 binding 排序并折叠为每个 `ResourceClass` 的 free-range 链表。
- 上游: Clang HLSL Sema (隐式 binding 分配); DXIL passes (binding 重叠
  警告 / conflict report)。
- 下游: [`llvm/ADT/STLExtras`](../../include/llvm/ADT/STLExtras.h),
  [`llvm/Support/DXILABI`](../../include/llvm/Support/DXILABI.h)
  (`dxil::ResourceClass`)。
- 关键类/函数: `BindingInfoBuilder::calculateBindingInfo`,
  `BindingInfo::BindingSpaces::getOrInsertSpace`,
  `BindingInfo::RegisterSpace::findAvailableBinding`,
  `BindingInfoBuilder::findOverlapping`, `Binding` (record)。

[`HLSL/HLSLResource.cpp`](HLSL/HLSLResource.cpp) — LLVM IR `Type` → DXIL
element-type 枚举 (I/U/F16/32/64) 的轻量映射。
- 上游: DXIL 后端; Clang HLSL 的 DXIL lowering。
- 下游: [`llvm/IR/DerivedTypes`](../../include/llvm/IR/DerivedTypes.h),
  [`llvm/Support/DXILABI`](../../include/llvm/Support/DXILABI.h)。
- 关键类/函数: `hlsl::getDXILElementType`。

[`HLSL/HLSLRootSignature.cpp`](HLSL/HLSLRootSignature.cpp) — root signature
in-memory 数据结构 (`RootFlags`, `RootDescriptor`, `DescriptorTableClause`
等) 的 `raw_ostream` 打印运算符。
- 上游: DXC / Clang HLSL root signature 解析器; DXIL 容器 dump 工具。
- 下游: [`llvm/Support/DXILABI`](../../include/llvm/Support/DXILABI.h),
  [`llvm/Support/InterleavedRange`](../../include/llvm/Support/InterleavedRange.h),
  [`llvm/Support/ScopedPrinter`](../../include/llvm/Support/ScopedPrinter.h)。
- 关键类/函数: `operator<<(RootElement)`, `dumpRootElements`,
  `operator<<(RootFlags/DescriptorTable/StaticSampler/RootDescriptor)`。

[`HLSL/RootSignatureMetadata.cpp`](HLSL/RootSignatureMetadata.cpp) —
in-memory root signature ↔ IR metadata (NamedMDNode) 双向转换 + 字段校验。
- 上游: DXC/Clang HLSL Sema (生成); DXIL/CodeGen passes (读取并校验)。
- 下游: [`llvm/IR/IRBuilder`](../../include/llvm/IR/IRBuilder.h),
  [`llvm/IR/Metadata`](../../include/llvm/IR/Metadata.h),
  [`llvm/Support/DXILABI`](../../include/llvm/Support/DXILABI.h),
  [`llvm/Support/ScopedPrinter`](../../include/llvm/Support/ScopedPrinter.h)。
- 关键类/函数: `MetadataBuilder::BuildRootSignature`,
  `MetadataParser::ParseRootSignature`,
  `parseRootFlags/parseRootConstants/parseRootDescriptors/parseDescriptorRange/parseDescriptorTable/parseStaticSampler`,
  `MetadataParser::validateRootSignature`。

[`HLSL/RootSignatureValidations.cpp`](HLSL/RootSignatureValidations.cpp) —
单独的 root signature 字段合法性验证 (register/space 范围、flag 互斥、
LOD/各向异性等 DXBC 规范检查)。
- 上游: [`HLSL/RootSignatureMetadata.cpp`](HLSL/RootSignatureMetadata.cpp)
  中的 `validateRootSignature`。
- 下游: [`llvm/Support/DXILABI`](../../include/llvm/Support/DXILABI.h)
  (`dxbc::*`, `dxil::ResourceClass`); `<cmath>`。
- 关键类/函数: `verifyRootFlag`, `verifyVersion`, `verifyRegisterValue`,
  `verifyRegisterSpace`, `verifyRootDescriptorFlag`,
  `verifyDescriptorRangeFlag`, `verifyStaticSamplerFlags`,
  `verifyNumDescriptors`, `verifyMipLODBias`, `verifyMaxAnisotropy`,
  `verifyLOD`, `verifyNoOverflowedOffset`, `computeRangeBound`。

[`HLSL/SemanticSignaturePacking.cpp`](HLSL/SemanticSignaturePacking.cpp) —
为 vertex shader input signature 分配 `(StartRow, StartCol)`; 只支持 stacked
布局。
- 上游: Clang HLSL CodeGen; `HLSLPatch`/signature 序列化 passes; DXIL 写入
  PSV0。
- 下游: [`llvm/ADT/STLExtras`](../../include/llvm/ADT/STLExtras.h)
  (`enumerate`), [`llvm/Support/Error`](../../include/llvm/Support/Error.h)
  (`make_error<SignaturePackingError>`)。
- 关键类/函数: `hlsl::packSignatureStacked`, `SignaturePackingError::log`,
  常量 `MaxSignatureRows` / `UnallocatedRow` / `UnallocatedCol`。

[`HLSL/SemanticSignatures.cpp`](HLSL/SemanticSignatures.cpp) — 在
`dxbc::PSV::SemanticKind` 与 shader stage / `IOType` 之间查表, 以及 metadata
↔ struct 的双向转换。
- 上游: Clang HLSL Sema (semantic → PSV 映射); DXIL 后端 (PSV0 序列化);
  HLSL signature dump 工具。
- 下游: [`llvm/ADT/Enum`](../../include/llvm/ADT/Enum.h),
  [`llvm/ADT/bit`](../../include/llvm/ADT/bit.h),
  [`llvm/IR/Constants`](../../include/llvm/IR/Constants.h),
  [`llvm/IR/Metadata`](../../include/llvm/IR/Metadata.h),
  [`llvm/Support/ErrorHandling`](../../include/llvm/Support/ErrorHandling.h)。
- 关键类/函数: `hlsl::getSemanticKind`, `hlsl::getAvailableStages`,
  `hlsl::getInterpretationKind`, `SemanticSignatureElement::fromMetadata`,
  `SemanticSignatureElement::toMetadata`, `SemanticStageInfo`。

### 3.5 Offloading/

[`Offloading/OffloadWrapper.cpp`](Offloading/OffloadWrapper.cpp) — 在 host
module 中嵌入 device image 并生成 fatbinary wrapper、
`__tgt_*_register_lib` 启动注册函数 (支持 OpenMP/CUDA/HIP/SYCL)。
- 上游: Clang `-fopenmp` / `-fcuda-include-gpubinary` / `-fsycl` 后端嵌入器;
  `clang-offload-bundler` 链路; LLD-linker 启动段。
- 下游: [`llvm/IR/IRBuilder`](../../include/llvm/IR/IRBuilder.h),
  [`llvm/IR/Module`](../../include/llvm/IR/Module.h),
  [`llvm/IR/Constants`](../../include/llvm/IR/Constants.h),
  [`llvm/Object/OffloadBinary`](../../include/llvm/Object/OffloadBinary.h),
  [`llvm/BinaryFormat/Magic`](../../include/llvm/BinaryFormat/Magic.h),
  [`llvm/Transforms/Utils/ModuleUtils`](../Transforms/Utils/ModuleUtils.cpp),
  [`llvm/Frontend/Offloading/Utility.h`](../../include/llvm/Frontend/Offloading/Utility.h)
  (同子目录内部)。
- 关键类/函数: `offloading::wrapOpenMPBinaries`, `wrapCudaBinary`,
  `wrapHIPBinary`, `wrapSYCLBinaries`, `createBinDesc`,
  `createFatbinDesc`, `createRegisterGlobalsFunction`,
  `createRegisterFatbinFunction`, `SYCLWrapper`。

[`Offloading/PropertySet.cpp`](Offloading/PropertySet.cpp) — offloading binary
属性集合的 JSON 序列化 (含 base64 编码的 byte array), 并支持反向解析。
- 上游: Offload binary 序列化器; `llvm-offload` 工具链; bundler/unbundler。
- 下游: [`llvm/Support/Base64`](../../include/llvm/Support/Base64.h),
  [`llvm/Support/JSON`](../../include/llvm/Support/JSON.h),
  [`llvm/Support/MemoryBufferRef`](../../include/llvm/Support/MemoryBufferRef.h)。
- 关键类/函数: `offloading::writePropertiesToJSON`,
  `offloading::readPropertiesFromJSON`, `readPropertyValueFromJSON` (static),
  `PropertySetRegistry`, `PropertyValue`。

[`Offloading/Utility.cpp`](Offloading/Utility.cpp) — 在 module 中创建
`__tgt_offload_entry` / `__tgt_device_image` / `__tgt_bin_desc` 全局与 ELF
section; 解析 AMDGPU ELF note msgpack。
- 上游: Clang OpenMP/HIP/CUDA codegen; Clang SYCL; Offload bundle/dump 工具。
- 下游: [`llvm/IR/Module`](../../include/llvm/IR/Module.h),
  [`llvm/IR/GlobalVariable`](../../include/llvm/IR/GlobalVariable.h),
  [`llvm/BinaryFormat/ELF`](../../include/llvm/BinaryFormat/ELF.h),
  [`llvm/BinaryFormat/MsgPackDocument`](../../include/llvm/BinaryFormat/MsgPackDocument.h),
  [`llvm/BinaryFormat/AMDGPUMetadataVerifier`](../../include/llvm/BinaryFormat/AMDGPUMetadataVerifier.h),
  [`llvm/Object/ELFObjectFile`](../../include/llvm/Object/ELFObjectFile.h),
  [`llvm/Object/OffloadBinary`](../../include/llvm/Object/OffloadBinary.h),
  [`llvm/ObjectYAML/yaml2obj`](../ObjectYAML/yaml2obj.cpp),
  [`llvm/Transforms/Utils/ModuleUtils`](../Transforms/Utils/ModuleUtils.cpp)。
- 关键类/函数: `offloading::getEntryTy`,
  `offloading::getOffloadingEntryInitializer`,
  `offloading::emitOffloadingEntry`, `offloading::getOffloadEntryArray`,
  `offloading::amdgpu::isImageCompatibleWithEnv`,
  `offloading::amdgpu::getAMDGPUMetaDataFromImage`,
  `offloading::containerizeImage`,
  `offloading::intel::containerizeOpenMPSPIRVImage`,
  `sycl::writeSymbolTable`, `KernelInfoReader` (内部)。

### 3.6 OpenACC/

[`OpenACC/ACC.cpp`](OpenACC/ACC.cpp) — 触发 `#include "llvm/Frontend/OpenACC/ACC.inc"`
的 `GEN_DIRECTIVES_IMPL`, 为 `acc::*` 枚举生成 `toString` / `getName` 等
访问器 (tablegen 生成的代码入口)。
- 上游: Clang OpenACC Sema/CodeGen
  ([`SemaOpenACC.cpp`](../../../clang/lib/Sema/SemaOpenACC.cpp),
  [`ParseOpenACC.cpp`](../../../clang/lib/Parse/ParseOpenACC.cpp))。
- 下游: [`llvm/ADT/StringSwitch`](../../include/llvm/ADT/StringSwitch.h)
  和生成的 `ACC.inc` / `ACC.h.inc` (来自
  [`ACC.td`](../../include/llvm/Frontend/OpenACC/ACC.td))。
- 关键类/函数: `acc::Directive` 枚举, `acc::getDirectiveName`,
  `acc::getDirectiveKind` (由 .inc 生成)。

### 3.7 OpenMP/

[`OpenMP/DirectiveNameParser.cpp`](OpenMP/DirectiveNameParser.cpp) — 把每条
OpenMP directive 的名字拆成 token, 建一个有限状态机用于 "ORDERED" /
"PARALLEL FOR SIMD" 等组合识别。
- 上游: Clang
  [`ParseOpenMP.cpp`](../../../clang/lib/Parse/ParseOpenMP.cpp)
  (预处理/预解析); 其它需要模糊匹配 OpenMP 拼写的工具。
- 下游: [`llvm/ADT/StringExtras`](../../include/llvm/ADT/StringExtras.h)
  (`SplitString`), [`llvm/Frontend/OpenMP/OMP.h`](../../include/llvm/Frontend/OpenMP/OMP.h)。
- 关键类/函数: `DirectiveNameParser::DirectiveNameParser(SourceLanguage)`,
  `consume`, `tokenize`, `insertName`, `insertTransition`, `State::next`。

[`OpenMP/OMP.cpp`](OpenMP/OMP.cpp) — OMP 指令/子句的高层语义查询:
leaf/composite/combined construct 识别、privatizing construct、kernel
name 反解。
- 上游: Clang OpenMP Sema/CodeGen
  ([`SemaOpenMP.cpp`](../../../clang/lib/Sema/SemaOpenMP.cpp),
  [`CGStmtOpenMP.cpp`](../../../clang/lib/CodeGen/CGStmtOpenMP.cpp));
  Flang OpenMP;
  [`OpenMPOpt`](../Transforms/IPO/OpenMPOpt.cpp) /
  `OpenMPOffload` pass; [`OpenMP/OMPIRBuilder.cpp`](OpenMP/OMPIRBuilder.cpp)。
- 下游: `llvm/ADT/{ArrayRef,SmallSet,SmallVector,StringRef}`,
  [`llvm/Demangle/Demangle`](../Demangle/Demangle.cpp),
  [`llvm/Support/ErrorHandling`](../../include/llvm/Support/ErrorHandling.h);
  由 [`OMP.td`](../../include/llvm/Frontend/OpenMP/OMP.td) 生成的 OMP 指令/子句
  表。
- 关键类/函数: `getLeafConstructs`, `getLeafConstructsOrSelf`,
  `getLeafOrCompositeConstructs`, `getCompoundConstruct`,
  `isLeafConstruct`, `isCompositeConstruct`, `isCombinedConstruct`,
  `isPrivatizingConstruct`, `getReservedLocatorNames`,
  `prettifyFunctionName`, `deconstructOpenMPKernelName`,
  `getOpenMPVersions`, `TargetRegionEntryInfo::KernelNamePrefix`。

[`OpenMP/OMPContext.cpp`](OpenMP/OMPContext.cpp) — `OMPContext`: 依据
target triple / device 推出活跃 trait 集合, 并完成 `declare variant` /
`metadirective` 的 trait 匹配 + 评分。
- 上游: Clang OpenMP Sema
  ([`SemaOpenMP.cpp`](../../../clang/lib/Sema/SemaOpenMP.cpp)) 的
  `BuildVariants` / `BuildDeclareVariant` 路径; MLIR OpenMP dialect
  lowering。
- 下游: `llvm/ADT/{StringRef,StringSwitch}`,
  [`llvm/Support/Debug`](../../include/llvm/Support/Debug.h),
  [`llvm/Support/raw_ostream`](../../include/llvm/Support/raw_ostream.h),
  [`llvm/TargetParser/Triple`](../TargetParser/Triple.cpp); 由
  [`OMPKinds.def`](../../include/llvm/Frontend/OpenMP/OMPKinds.def)
  X-macro 驱动。
- 关键类/函数: `OMPContext::OMPContext`, `isVariantApplicableInContext`,
  `getBestVariantMatchForContext`,
  `getOpenMPContextTraitSetKind/ForSelector/ForProperty`,
  `getOpenMPContextTraitSelectorKind/ForProperty`,
  `getOpenMPContextTraitPropertyKind/Name/FullName`,
  `isValidTraitSelectorForTraitSet`,
  `isValidTraitPropertyForTraitSetAndSelector`,
  `listOpenMPContextTraitSets/Selectors/Properties`。

[`OpenMP/OMPDescriptors.cpp`](OpenMP/OMPDescriptors.cpp) — 维护 `Clause` /
`Modifier` / `ModifierSet` 的"每版本属性 / 允许指令 / 允许子句 / 修饰符集
合"映射, 按 OpenMP 版本过滤。
- 上游: Clang OpenMP Sema/CodeGen (语义/特性查询); 将来 MLIR OpenMP
  dialect。
- 下游: 头文件
  [`OMPDescriptors.h`](../../include/llvm/Frontend/OpenMP/OMPDescriptors.h)
  (声明) 与其同名 `.inc` 表 (由 tablegen 生成于 build 时)。
- 关键类/函数: `getClauseMap`, `getModifierMap`, `getModifierSetMap`,
  `descriptor::Clause::getProperties/getDirectives/getModifiers/getModifierSets`,
  `descriptor::Modifier::*`, `descriptor::ModifierSet::*`,
  `getDescriptor(...)` (3 overloads), `getProperties(Clause, Version)`。

[`OpenMP/OMPIRBuilder.cpp`](OpenMP/OMPIRBuilder.cpp) — 完整实现
`OpenMPIRBuilder`: 用 IRBuilder 拼装所有 OpenMP region (parallel / teams /
distribute / for / simd / target / task / taskloop 等), runtime call、
outline、cancellation、task、atomic、reduction、allocations、interop。
- 上游: Clang OpenMP CodeGen
  ([`CGStmtOpenMP.cpp`](../../../clang/lib/CodeGen/CGStmtOpenMP.cpp))
  (在并行 lowering 时调 `OpenMPIRBuilder` API); Flang OpenMP; MLIR
  OpenMP→LLVM 转换; `OpenMPOpt` / `OpenMPOffload` / `OMPIRBuilderAttributor`
  配套 pass; `clang-tools-extra` 的 transformer 测试。
- 下游: `llvm/Analysis/{AssumptionCache,CodeMetrics,LoopInfo,OptimizationRemarkEmitter,PostDominators,ScalarEvolution,TargetLibraryInfo}`,
  [`llvm/Bitcode/BitcodeReader`](../Bitcode/Reader/BitcodeReader.cpp),
  [`llvm/Frontend/Offloading/Utility.h`](../../include/llvm/Frontend/Offloading/Utility.h)
  (跨子目录), [`llvm/Frontend/OpenMP/OMPGridValues.h`](../../include/llvm/Frontend/OpenMP/OMPGridValues.h),
  `llvm/IR/{IRBuilder,DIBuilder,MDBuilder,PassInstrumentation,PassManager,ReplaceConstant}`,
  [`llvm/MC/TargetRegistry`](../MC/TargetRegistry.cpp),
  [`llvm/Support/{CommandLine,Error,NVVMAttributes,VirtualFileSystem}`](../../include/llvm/Support/),
  `llvm/Target/{TargetMachine,TargetOptions}`,
  `llvm/Transforms/Utils/{BasicBlockUtils,Cloning,CodeExtractor,LoopPeel,UnrollLoop}`。
- 关键类/函数: `OpenMPIRBuilder::*` (大型 API, 200+ 函数):
  `CreateParallel`, `createOffloadEntry`, `emitTargetTask`, `emitTeams`,
  `emitDistribute`, `applyWorkshareLoop`, `emitTask`, `emitTaskwait`,
  `emitCancel`, `emitReduction`, `emitInterop`, `emitBarrier`,
  `OMPScheduleType`, `TargetRegionEntryInfo` 等。

---

## §4. 关键调用链

### 4.1 HLSL Root Signature 生成与校验

```
Clang HLSL Sema ([`SemaHLSL.cpp`](../../../clang/lib/Sema/SemaHLSL.cpp))
  ├─ 用户写 [RootSignature(...)] attributes
  │    └─ 解析为 in-memory RootElements 列表
  └─ CodeGen 把 RootElements 写到 module 命名 metadata
       └─ HLSL::RootSignatureMetadata::MetadataBuilder::BuildRootSignature
            ├─ 为每个 clause 生成 NamedMDNode
            └─ emit hlsl.* / dx.* metadata
   │
   ▼
DXIL Backend Pass
  └─ HLSL::RootSignatureMetadata::MetadataParser::ParseRootSignature
       ├─ 把 NamedMDNode 反解回 RootElements
       └─ MetadataParser::validateRootSignature
            └─ HLSL::RootSignatureValidations::verifyRootFlag
                 / verifyVersion / verifyRegisterValue / verifyRegisterSpace
                 / verifyRootDescriptorFlag / verifyDescriptorRangeFlag
                 / verifyStaticSamplerFlags / verifyNumDescriptors
                 / verifyMipLODBias / verifyMaxAnisotropy / verifyLOD
                 / verifyNoOverflowedOffset / computeRangeBound
                 → emit Warning / Error
```

### 4.2 OpenMP Region Codegen (parallel for)

```
Clang CodeGen ([`CGStmtOpenMP.cpp`](../../../clang/lib/CodeGen/CGStmtOpenMP.cpp))
  └─ Emitted code for #pragma omp parallel for
       └─ OpenMPIRBuilder::CreateParallel (OpenMP/OMPIRBuilder.cpp)
            ├─ 配置 callback: __kmpc_fork_call
            ├─ 拼装 entry / exit basicblock
            ├─ OpenMPIRBuilder::applyWorkshareLoop (loop body lowering)
            │    └─ OpenMPIRBuilder::applyStaticSchedule / Dynamic
            │         └─ emit __kmpc_for_static_init_4 etc.
            ├─ OpenMPIRBuilder::emitBarrier (if nowait 不在)
            └─ 用 CodeExtractor 把 outlined function 抽出
                 ├─ 调 Offloading::Utility::emitOffloadingEntry
                 │    (如果 target 构造则同时生成 __tgt_offload_entry)
                 └─ emit @__kmpc_fork_call (runtime lib 入口)
```

### 4.3 OpenMP declare variant trait 匹配

```
Clang OpenMP Sema::BuildDeclareVariant
  └─ 收集 construct / set / selector / property / score
       └─ OMPContext (OpenMP/OMPContext.cpp)
            ├─ OMPContext::OMPContext (从 target triple / device 推出活跃 trait)
            ├─ OMPContext::isVariantApplicableInContext (per variant)
            │    ├─ getOpenMPContextTraitSetKind
            │    ├─ getOpenMPContextTraitSelectorKind
            │    └─ getOpenMPContextTraitPropertyKind
            └─ OMPContext::getBestVariantMatchForContext (按 score 选最佳)
                 → Clang Sema 选中对应的 variant 函数
```

### 4.4 OpenMP/HIP/CUDA fatbinary 嵌入

```
Clang -fopenmp / -fcuda-include-gpubinary / -fsycl 后端
  └─ Frontend/Offloading/OffloadWrapper.cpp
       ├─ offloading::wrapOpenMPBinaries
       │    ├─ Offloading::Utility::containerizeImage
       │    └─ 写出 .img.bin global + __tgt_bin_desc 全局
       ├─ offloading::wrapCudaBinary
       │    ├─ createFatbinDesc
       │    └─ createRegisterFatbinFunction
       │         └─ emit __cudaRegisterFatbin 调用
       └─ offloading::wrapHIPBinary / wrapSYCLBinaries 类似

LLD 链接时
  └─ LLD/Cuda / LLD/HIP / LLD/Offload 段处理
       └─ 读 Offloading::Utility::getOffloadEntryArray
       └─ 把 device image 复制到可加载段
```

### 4.5 HLSL Binding 分配

```
Clang HLSL Sema::CheckBinding
  └─ HLSL::BindingInfoBuilder::calculateBindingInfo (HLSL/HLSLBinding.cpp)
       ├─ 收集所有声明 (register(bN), register(bN, spaceM))
       ├─ 按 ResourceClass 分组 → BindingSpaces
       ├─ 对每个 RegisterSpace: findAvailableBinding
       │    ├─ 维护已分配范围 (sorted vector of [lo, hi])
       │    └─ 找一个未被占的 register slot
       └─ 检测 findOverlapping → emit warning
```

### 4.6 HLSL CBuffer 解析 (DXIL 阶段)

```
DXIL Backend Pass (LowerResourceHandle / Inline)
  └─ HLSL::CBufferMetadata::get (HLSL/CBuffer.cpp)
       ├─ 读 module 命名 metadata "hlsl.cbs"
       ├─ 为每个 cbuffer 构造 CBufferMapping (handle → member offsets)
       └─ 把 cbuffer handle 替换成全局访问 + element ptr arithmetic
            └─ 用 getMemberOffsets 算每个字段的实际字节偏移
```

### 4.7 Atomic codegen (C11 atomics)

```
Clang CodeGen ([`CGAtomic.cpp`](../../../clang/lib/CodeGen/CGAtomic.cpp))
  └─ 决定 atomic ordering / size / 是否 volatile
       └─ 构造 AtomicInfo 子类 (Atomic/Atomic.cpp)
            ├─ AtomicInfo::EmitAtomicLoadOp
            │    ├─ 优先 atomic.load intrinsic
            │    └─ fallback AtomicInfo::EmitAtomicLibcall
            ├─ AtomicInfo::EmitAtomicCompareExchange
            │    ├─ cmpxchg intrinsic
            │    └─ fallback __atomic_compare_exchange libcall
            └─ AtomicInfo::decorateWithTBAA (打 tbaa metadata)
```

---

## §5. 推荐阅读顺序

### 阶段 1: 数据入口 (30 分钟)
1. [`Directive/Spelling.cpp`](Directive/Spelling.cpp) — 最简单的 1-文件组件, 看懂
   `FindName` / `VersionRange` 模式。
2. [`Driver/CodeGenOptions.cpp`](Driver/CodeGenOptions.cpp) — 看懂 driver → IR
   选项桥。
3. [`OpenACC/ACC.cpp`](OpenACC/ACC.cpp) — tablegen 入口的"Hello world"。

### 阶段 2: Atomic (1 小时)
1. [`Atomic/Atomic.cpp`](Atomic/Atomic.cpp) — `AtomicInfo` 抽象; 理解 intrinsic
   vs libcall 双路径。

### 阶段 3: Offloading (2-3 小时)
1. [`Offloading/Utility.cpp`](Offloading/Utility.cpp) — 全局 entry/device image
   创建 + ELF note 解析。
2. [`Offloading/OffloadWrapper.cpp`](Offloading/OffloadWrapper.cpp) —
   `wrapOpenMPBinaries` / `wrapCudaBinary` 入口, 用
   [`Offloading/Utility.cpp`](Offloading/Utility.cpp) 写 module。
3. [`Offloading/PropertySet.cpp`](Offloading/PropertySet.cpp) — 序列化
   PropertySet。

### 阶段 4: OpenMP 元数据层 (2-3 小时)
1. [`OpenMP/DirectiveNameParser.cpp`](OpenMP/DirectiveNameParser.cpp) — 状态机。
2. [`OpenMP/OMP.cpp`](OpenMP/OMP.cpp) — `isLeafConstruct` /
   `getLeafConstructs` 等查询; `deconstructOpenMPKernelName`。
3. [`OpenMP/OMPContext.cpp`](OpenMP/OMPContext.cpp) — `OMPContext` +
   `getBestVariantMatchForContext`。
4. [`OpenMP/OMPDescriptors.cpp`](OpenMP/OMPDescriptors.cpp) — clause/modifier
   版本矩阵。

### 阶段 5: OpenMP IRBuilder (4-6 小时, 最重)
1. [`OpenMP/OMPIRBuilder.cpp`](OpenMP/OMPIRBuilder.cpp) — `OpenMPIRBuilder`
   主类; 从 `CreateParallel` / `applyWorkshareLoop` 入手。
2. 看 `emitTargetTask` / `emitTeams` 等 target 类构造。
3. 看 reduction / cancellation / interop 这几个高阶 API。

### 阶段 6: HLSL (3-4 小时)
1. [`HLSL/HLSLResource.cpp`](HLSL/HLSLResource.cpp) — 最简单的 type 映射。
2. [`HLSL/SemanticSignatures.cpp`](HLSL/SemanticSignatures.cpp) — SemanticKind
   ↔ PSV0 映射。
3. [`HLSL/SemanticSignaturePacking.cpp`](HLSL/SemanticSignaturePacking.cpp) —
   `packSignatureStacked` 分配算法。
4. [`HLSL/CBuffer.cpp`](HLSL/CBuffer.cpp) — cbuffer metadata 解析。
5. [`HLSL/HLSLBinding.cpp`](HLSL/HLSLBinding.cpp) — binding 分配算法。
6. [`HLSL/HLSLRootSignature.cpp`](HLSL/HLSLRootSignature.cpp) — root
   signature 打印 (辅助)。
7. [`HLSL/RootSignatureMetadata.cpp`](HLSL/RootSignatureMetadata.cpp) —
   metadata 双向转换。
8. [`HLSL/RootSignatureValidations.cpp`](HLSL/RootSignatureValidations.cpp) —
   校验规则 (debug 用)。

---

## §6. 常用操作指南

### 6.1 添加新 OpenMP 指令

1. 在 [`OMP.td`](../../include/llvm/Frontend/OpenMP/OMP.td)
   加新条目 (枚举 + 拼写 + 版本范围)。
2. 重跑 `acc_gen` / `omp_gen` (CMake 的 `intrinsics_gen` / `omp_gen` 自
   动触发), 重新生成 `OMP.h.inc` / `OMP.inc`。
3. 如果是 combined/composite 构造, 更新
   [`OpenMP/OMP.cpp`](OpenMP/OMP.cpp) 的 `isCombinedConstruct` /
   `getLeafConstructsOrSelf`。
4. 如需新 clause, 在 `OMP.td` 加 `Clause` 条目;
   [`OpenMP/OMPDescriptors.cpp`](OpenMP/OMPDescriptors.cpp) 的
   `getProperties` 自动生成 (X-macro)。
5. 在 [`OpenMP/OMPIRBuilder.cpp`](OpenMP/OMPIRBuilder.cpp) 实现新的
   `emitXxx` API。
6. 在 Clang ([`SemaOpenMP.cpp`](../../../clang/lib/Sema/SemaOpenMP.cpp) /
   [`CGStmtOpenMP.cpp`](../../../clang/lib/CodeGen/CGStmtOpenMP.cpp))
   调用新 API。

### 6.2 添加新 HLSL 资源类型

1. 在 [`DXILABI.h`](../../include/llvm/Support/DXILABI.h)
   加新 `ResourceClass` (如果是新类别) + 配套 DXBC 校验常量。
2. 在 [`HLSL/HLSLResource.cpp`](HLSL/HLSLResource.cpp) 加
   `getDXILElementType` 分支。
3. 在 [`HLSL/HLSLBinding.cpp`](HLSL/HLSLBinding.cpp) 加 binding 范围常量。
4. 在 [`HLSL/RootSignatureMetadata.cpp`](HLSL/RootSignatureMetadata.cpp) 加
   新 clause 的 parse 函数 + metadata builder。
5. 在 [`HLSL/RootSignatureValidations.cpp`](HLSL/RootSignatureValidations.cpp)
   加对应 `verifyXxx`。
6. 单测:
   [`test/CodeGen/DirectX/`](../../test/CodeGen/DirectX/) +
   [`HLSLBindingTest.cpp`](../../unittests/Frontend/HLSLBindingTest.cpp)
   (及相关 HLSL*Test.cpp)。

### 6.3 添加新 OpenACC 指令

1. 在 [`ACC.td`](../../include/llvm/Frontend/OpenACC/ACC.td)
   加新条目 (枚举 + 拼写 + 子句)。
2. 重新生成 `ACC.h.inc` / `ACC.inc` (CMake 自触发)。
3. Clang Sema ([`SemaOpenACC.cpp`](../../../clang/lib/Sema/SemaOpenACC.cpp) /
   [`ParseOpenACC.cpp`](../../../clang/lib/Parse/ParseOpenACC.cpp)) 自动从
   .inc 拿到访问器。
4. 单测: `clang/test/SemaOpenACC/`。

### 6.4 让新 Clang atomic lowering 用 `AtomicInfo` 抽象

1. 在 [`Atomic.h`](../../include/llvm/Frontend/Atomic/Atomic.h)
   看 `AtomicInfo` 纯虚接口 (`EmitAtomicLoadOp` / `EmitAtomicStoreLibcall` /
   `EmitAtomicCompareExchange` 等)。
2. 在 Clang CodeGen
   ([`CGAtomic.cpp`](../../../clang/lib/CodeGen/CGAtomic.cpp)) 实现一个
   派生 `AtomicInfo` 的类, 覆盖必要的 hook (size / ordering / 内存模型)。
3. 在 [`Atomic/Atomic.cpp`](Atomic/Atomic.cpp) 实现共享算法 (cmpxchg loop
   展开 / TBAA decorate)。
4. 单测: `clang/test/CodeGen/atomic-*.c`。

### 6.5 让新 frontend 用 `OpenMPIRBuilder`

1. 链接 `LLVMFrontendOpenMP` (CMake `target_link_libraries`)。
2. `#include` [`OMPIRBuilder.h`](../../include/llvm/Frontend/OpenMP/OMPIRBuilder.h)。
3. 构造 `OpenMPIRBuilder` (传入 `Module` + `LLVMContext` + `TargetMachine`)。
4. 调用 `CreateParallel` / `applyWorkshareLoop` 等 API, 把 OpenMP 构造
   lowered 到 IR。
5. 单测:
   [`OpenMPIRBuilderTest.cpp`](../../unittests/Frontend/OpenMPIRBuilderTest.cpp)
   + 实际前端测试用例。

### 6.6 添加新 Offloading target

1. 在 [`OffloadBinary.h`](../../include/llvm/Object/OffloadBinary.h)
   加新 `OffloadKind`。
2. 在 [`Offloading/Utility.cpp`](Offloading/Utility.cpp) 实现对应
   `containerizeXxxImage` (类似 `amdgpu::isImageCompatibleWithEnv`)。
3. 在 [`Offloading/OffloadWrapper.cpp`](Offloading/OffloadWrapper.cpp) 实现
   `wrapXxxBinary` 入口。
4. 在 [`Offloading/PropertySet.cpp`](Offloading/PropertySet.cpp) 添加新
   property 序列化字段 (JSON 字段)。
5. 单测:
   [`test/Driver/offloading-*.c`](../../../clang/test/Driver/) +
   [`OffloadingTest.cpp`](../../unittests/Object/OffloadingTest.cpp)
   (位于 `Object/`, 不是 `Frontend/`)。

---

## §7. NT 注释索引

当前 `llvm/lib/Frontend/` 下尚无 `// <NT>` 注释。姊妹 overview:

- [`llvm/lib/IR/0-overview.md`](../IR/0-overview.md) — LLVM IR 层
- [`llvm/lib/CodeGen/0-overview.md`](../CodeGen/0-overview.md) — target-independent CodeGen
- [`llvm/lib/Target/RISCV/0-overview.md`](../Target/RISCV/0-overview.md) — RISCV 后端
- [`llvm/lib/TargetParser/0-overview.md`](../TargetParser/0-overview.md) — TargetParser
- [`llvm/lib/Analysis/0-overview.md`](../Analysis/0-overview.md) — IR 层分析

按"少而精"原则, 添加 NT 注释时建议优先级:

1. [`OpenMP/OMPIRBuilder.cpp`](OpenMP/OMPIRBuilder.cpp) — 5-8 段 (最大、最核心;
   CreateParallel / applyWorkshareLoop / emitTargetTask 三个主 API)
2. [`HLSL/RootSignatureMetadata.cpp`](HLSL/RootSignatureMetadata.cpp) — 3-5 段
   (双向转换主入口)
3. [`Offloading/OffloadWrapper.cpp`](Offloading/OffloadWrapper.cpp) — 3-5 段
   (wrapOpenMP / wrapCuda / wrapHIP / wrapSYCL 四个入口)
4. [`Atomic/Atomic.cpp`](Atomic/Atomic.cpp) — 3-5 段 (intrinsic vs libcall 双
   路径)
5. 单 utility 文件 ([`Directive/Spelling.cpp`](Directive/Spelling.cpp)、
   [`Driver/CodeGenOptions.cpp`](Driver/CodeGenOptions.cpp)、
   [`OpenACC/ACC.cpp`](OpenACC/ACC.cpp)) — 1-2 段

---

**姊妹文档**: 已建立完整目录索引, 如需查看 Clang 前端 (`clang/`) 或 MLIR
方言 (`mlir/lib/Dialect/OpenMP/`), 需要单独写。