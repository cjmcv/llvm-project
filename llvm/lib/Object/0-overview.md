<!-- <NT>overview:llvm/lib/Object/ -->

# LLVM Object 库导读 — `llvm/lib/Object/`

> 本文档梳理 `llvm/lib/Object/` 目录下全部源文件（37 个 `.cpp` + 1 个
> 内部 `.h`，全为顶层文件无子目录）的职责、上下游与推荐阅读顺序。
> 目标读者：想理解 LLVM **二进制对象文件解析层**（ELF / COFF / Mach-O /
> Wasm / XCOFF / GOFF / DXContainer / Minidump / Tapi / Archive / Offload
> 容器）的开发者，以及要给工具（llvm-objdump / llvm-readobj / llvm-nm /
> llvm-objcopy / LLD 链接器）加新格式支持的开发者。
>
> 所有路径相对 `llvm/lib/Object/`。同名头文件在 `llvm/include/llvm/Object/`。

---

## §0. Object 库在 LLVM 中的位置

`lib/Object` 是 LLVM **二进制对象文件解析层**——把磁盘/内存中的二进制
对象/可执行/容器/归档格式解析为 LLVM 内部的迭代器接口 (`SymbolRef` /
`SectionRef` / `RelocationRef`)。

包含的内容（按职责）:

- **核心基类**: `Binary` / `SymbolicFile` / `ObjectFile` (抽象基类 + 工厂分发)。
- **格式 parser**: ELF(Executable and Linkable Format), COFF(Common Object File Format,window的elf), Mach-O, Wasm, XCOFF, GOFF, DXContainer, Minidump, Tapi。
- **归档/容器**: `Archive` (ar), `ArchiveWriter`, `OffloadBinary`, `OffloadBundle`。
- **Bitcode 适配**: `IRObjectFile`, `IRSymtab`, `ModuleSymbolTable`, `RecordStreamer`。
- **特殊 section 解析**: BBAddrMap, BuildID, SFrame, FaultMap, WindowsResource, COFFImportFile, COFFModuleDefinition。
- **公用工具**: Decompressor (zlib/zstd), Error, MachOUniversal(Writer), SymbolSize, RelocationResolver。

它在流水线中的位置:

```
磁盘 / 内存中的二进制 (.o / .so / .a / .exe / .dylib / .wasm / .dxbc / ...)
   │
   ▼
┌──────────────────────────────────────────────────────────┐
│ lib/Object  (本目录)                                       │
│   · createBinary() 分发 → Archive / MachOUniversal / Minidump / ...  │
│   · createObjectFile() 分发 → ELF / COFF / Mach-O / Wasm / ...       │
│   · createSymbolicFile() 分发 → ObjectFile / IRObjectFile / COFFImportFile │
│   · 各种 SectionRef / SymbolRef / RelocationRef iterator                │
└──────────────────────────────────────────────────────────┘
   │
   ├─→ LLD 链接器 (读 ELF/COFF/Mach-O/Wasm/XCOFF)
   ├─→ llvm-objdump / llvm-readobj / llvm-readelf
   ├─→ llvm-nm / llvm-strings
   ├─→ llvm-objcopy / llvm-strip / llvm-dwp
   ├─→ llvm-ar / llvm-ranlib
   ├─→ llvm-symbolizer / llvm-dwarfdump / dsymutil
   ├─→ LTO / ThinLTO (通过 IRObjectFile / IRSymtab)
   ├─→ Clang OpenMP/CUDA/HIP/SYCL offload (通过 OffloadBinary/Bundle)
   └─→ llvm-c/Object.h C API (Rust inkwell, llvmlite, ...)
```

---

## §1. 编译流水线概览

```
磁盘二进制 (任意格式)
   │
   ▼
┌──────────────────────────────────────────────────────────┐
│ lib/Object::createBinary() (Binary.cpp)                     │
│   · 读 magic bytes → file_magic                             │
│   · 分发:                                              │
│       - archive_magic       → Archive                       │
│       - macho_universal     → MachOUniversal                │
│       - minidump            → MinidumpFile                  │
│       - windows_resource    → WindowsResource               │
│       - tapi_universal      → TapiUniversal                 │
│       - object              → createObjectFile()            │
│           ├─ elf_*         → ELFObjectFile<ELFT>           │
│           ├─ coff          → COFFObjectFile                 │
│           ├─ macho         → MachOObjectFile                │
│           ├─ wasm          → WasmObjectFile                 │
│           ├─ xcoff_*       → XCOFFObjectFile                │
│           ├─ goff_*        → GOFFObjectFile                 │
│           └─ dxcontainer   → DXContainer                    │
└──────────────────────────────────────────────────────────┘
   │
   ▼
ObjectFile / SymbolicFile / Archive / IRObjectFile / ...
   │
   ▼
上层工具 / LLD 链接 / LTO / debug-info 处理
```

辅助入口:

- **C API**: [`Object.cpp`](Object.cpp) 暴露 `LLVMCreateBinary` /
  `LLVMCreateObjectFile` 等, 给语言绑定用。
- **批处理 / fat binary**: [`MachOUniversal.cpp`](MachOUniversal.cpp) +
  [`MachOUniversalWriter.cpp`](MachOUniversalWriter.cpp) 处理多 arch Mach-O。
- **TAPI (Apple TBD)**: [`TapiUniversal.cpp`](TapiUniversal.cpp) +
  [`TapiFile.cpp`](TapiFile.cpp) 解析 `.tbd` 文本 API。

---

## §2. 文件目录结构

```
llvm/lib/Object/   (FLAT — 全部 37 .cpp + 1 .h 在顶层, 无子目录)
├── 6 大类共 37 个文件
└── RecordStreamer.h    (唯一的内部头, 紧耦合 ModuleSymbolTable)
```

### 2.1 文件按职责分类 (6 类, 37 文件)

| # | 分类 | 文件数 | 核心标识符 |
|---|------|--------|-----------|
| 1 | Base / utility | 8 | `Binary`, `SymbolicFile`, `ObjectFile`, `Error`, `Decompressor` |
| 2 | 对象格式 parser | 9 | `ELFObjectFile`, `COFFObjectFile`, `MachOObjectFile`, `WasmObjectFile`, `XCOFFObjectFile`, `GOFFObjectFile`, `DXContainer`, `Minidump`, `ELF.cpp` |
| 3 | Archive / 容器 | 4 | `Archive`, `ArchiveWriter`, `OffloadBinary`, `OffloadBundle` |
| 4 | Symbol/section utilities | 5+1 | `SymbolSize`, `RelocationResolver`, `ModuleSymbolTable`, `RecordStreamer.cpp/.h` |
| 5 | IR / bitcode 层 | 2 | `IRObjectFile`, `IRSymtab` |
| 6 | 专项 section parser | 8 | `BBAddrMap`, `BuildID`, `FaultMapParser`, `SFrameParser`, `WindowsMachineFlag`, `WindowsResource`, `COFFImportFile`, `COFFModuleDefinition` |

---

## §3. 文件详解

### 3.1 Base / utility

[`Binary.cpp`](Binary.cpp) — `Binary` 抽象基类 + `createBinary` 工厂
(按 file magic 分发到对应 parser)。
- 上游: 所有打开二进制的 LLVM 工具 (llvm-objdump, llvm-readobj, llvm-nm,
  llvm-readelf, llvm-ar, llvm-objcopy, llvm-strip, llvm-strings, LLD)。
- 下游: [`llvm/BinaryFormat/Magic.h`](../../include/llvm/BinaryFormat/Magic.h)
  (`file_magic`), 各格式 header (`Archive.h`, `MachOUniversal.h`,
  [`Minidump.h`](../../include/llvm/Object/Minidump.h), `ObjectFile.h`,
  [`OffloadBinary.h`](../../include/llvm/Object/OffloadBinary.h),
  [`TapiUniversal.h`](../../include/llvm/Object/TapiUniversal.h),
  [`WindowsResource.h`](../../include/llvm/Object/WindowsResource.h)),
  [`llvm/Support/Error.h`](../../include/llvm/Support/Error.h)。
- 关键类/函数: `Binary`, `createBinary(MemoryBufferRef, LLVMContext*)`,
  `createBinary(StringRef Path)`, `OwningBinary<Binary>`,
  `ID_Archive` / `ID_ELF64L` 等, `getType()`。

[`Object.cpp`](Object.cpp) — `llvm-c/Object.h` C API wrapper
(`LLVMCreateBinary`, `LLVMCreateObjectFile`, iterator API)。
- 上游: 外部语言绑定 (Rust inkwell, CPython llvmlite, Ruby, ...)。
- 下游: [`llvm-c/Object.h`](../../include/llvm-c/Object.h),
  [`llvm/IR/LLVMContext.h`](../../include/llvm/IR/LLVMContext.h),
  [`llvm/Object/ObjectFile.h`](../../include/llvm/Object/ObjectFile.h),
  [`llvm/Object/MachOUniversal.h`](../../include/llvm/Object/MachOUniversal.h)。
- 关键类/函数: `LLVMCreateBinary`, `LLVMCreateObjectFile`,
  `LLVMBinaryGetType`, `LLVMObjectFileCopySectionIterator`,
  `LLVMGetSectionName`, `LLVMGetSymbols`, `BinaryTypeMapper`。

[`ObjectFile.cpp`](ObjectFile.cpp) — 格式无关 `ObjectFile` 抽象基类 +
`createObjectFile` 工厂 (按 magic 分发到 ELF/COFF/Mach-O/Wasm/XCOFF/GOFF/
DXContainer)。
- 上游: llvm-objdump, llvm-readobj, llvm-nm, llvm-readelf, llvm-objcopy,
  llvm-strip, llvm-ar, LLD, 任何 `ObjectFile` 用户。
- 下游: [`llvm/BinaryFormat/Magic.h`](../../include/llvm/BinaryFormat/Magic.h),
  [`llvm/Object/COFF.h`](../../include/llvm/Object/COFF.h),
  [`llvm/Object/ELF.h`](../../include/llvm/Object/ELF.h),
  [`llvm/Object/MachO.h`](../../include/llvm/Object/MachO.h),
  [`llvm/Object/Wasm.h`](../../include/llvm/Object/Wasm.h),
  [`LLVMObject/DXContainer.h`](../../include/llvm/Object/DXContainer.h),
  [`llvm/Object/GOFFObjectFile.h`](../../include/llvm/Object/GOFFObjectFile.h),
  [`llvm/Object/XCOFFObjectFile.h`](../../include/llvm/Object/XCOFFObjectFile.h),
  [`llvm/Support/Format.h`](../../include/llvm/Support/Format.h)。
- 关键类/函数: `ObjectFile`, `SectionRef::containsSymbol`, `getSymbolValue`,
  `makeTriple` (Arch→Triple dispatch), `createObjectFile`,
  `createELFObjectFile` / `createMachOObjectFile` /
  `createCOFFObjectFile` / `createWasmObjectFile` /
  `createXCOFFObjectFile` / `createGOFFObjectFile` /
  `createDXContainerObjectFile`。

[`SymbolicFile.cpp`](SymbolicFile.cpp) — `SymbolicFile` 超类, 处理只有
symbol 没有完整 section 语义的文件 (`IRObjectFile` / `COFFImportFile`)。
- 上游: `createSymbolicFile` 被 `Binary::createBinary` 调; 任何需要
  symbol-only 迭代的 (LTO symbol lookup)。
- 下游: [`llvm/BinaryFormat/Magic.h`](../../include/llvm/BinaryFormat/Magic.h),
  [`llvm/Object/IRObjectFile.h`](../../include/llvm/Object/IRObjectFile.h),
  [`llvm/Object/COFFImportFile.h`](../../include/llvm/Object/COFFImportFile.h),
  [`llvm/Object/ObjectFile.h`](../../include/llvm/Object/ObjectFile.h)。
- 关键类/函数: `SymbolicFile`, `createSymbolicFile`, `isSymbolicFile`,
  `BasicSymbolRef`。

[`Error.cpp`](Error.cpp) — `object_error` enum, `object_category()`,
`BinaryError` / `GenericBinaryError` 类型。
- 上游: `lib/Object` 其它所有文件; 透传到所有工具。
- 下游: [`llvm/Support/ErrorHandling.h`](../../include/llvm/Support/ErrorHandling.h),
  [`llvm/ADT/Twine.h`](../../include/llvm/ADT/Twine.h)。
- 关键类/函数: `object_error` (`arch_not_found`, `invalid_file_type`,
  `parse_failed`, `unexpected_eof`, `bitcode_section_not_found`,
  `invalid_symbol_index`, `section_stripped`, ...),
  `_object_error_category`, `GenericBinaryError`, `BinaryError`,
  `isNotObjectErrorInvalidFileType`。

[`Decompressor.cpp`](Decompressor.cpp) — 解压 ELFCOMPRESS_ZLIB/ZSTD ELF
section payload (典型场景: debug sections)。
- 上游: `ELFObjectFile` 读 `.zdebug_*` / SHF_COMPRESSED sections 时;
  llvm-dwarfdump, llvm-objdump --dwarf。
- 下游: [`llvm/BinaryFormat/ELF.h`](../../include/llvm/BinaryFormat/ELF.h)
  (`Elf32_Chdr` / `Elf64_Chdr`, `ELFCOMPRESS_ZLIB/ZSTD`),
  [`llvm/Support/Compression.h`](../../include/llvm/Support/Compression.h),
  [`llvm/Support/DataExtractor.h`](../../include/llvm/Support/DataExtractor.h)。
- 关键类/函数: `Decompressor::create`,
  `Decompressor::consumeCompressedHeader`, `Decompressor::decompress`。

[`MachOUniversal.cpp`](MachOUniversal.cpp) — 解析 Mach-O fat/universal
二进制 (`FAT_MAGIC` / `FAT_MAGIC_64`), 把 per-arch sub-object 暴露为
`MachOObjectFile` 或 `IRObjectFile`。
- 上游: llvm-lipo, llvm-objdump, llvm-nm, llvm-otool, dsymutil,
  LLD/Mach-O, Xcode tools。
- 下游: [`llvm/Object/Archive.h`](../../include/llvm/Object/Archive.h),
  [`llvm/Object/IRObjectFile.h`](../../include/llvm/Object/IRObjectFile.h),
  [`llvm/Object/MachO.h`](../../include/llvm/Object/MachO.h),
  [`llvm/Object/ObjectFile.h`](../../include/llvm/Object/ObjectFile.h),
  [`llvm/Support/SwapByteOrder.h`](../../include/llvm/Support/SwapByteOrder.h)。
- 关键类/函数: `MachOUniversalBinary`,
  `ObjectForArch::getAsObjectFile`,
  `ObjectForArch::getAsIRObject`,
  `getMachOObjectForArch(StringRef Arch)`, `objects()`。

[`MachOUniversalWriter.cpp`](MachOUniversalWriter.cpp) — 构建 Mach-O
universal ("fat") 二进制 (类 cctools `lipo`); 处理 per-arch alignment。
- 上游: llvm-lipo (主要); clang driver fat-binary 生成。
- 下游: [`llvm/Object/Archive.h`](../../include/llvm/Object/Archive.h),
  [`llvm/Object/IRObjectFile.h`](../../include/llvm/Object/IRObjectFile.h),
  [`llvm/Object/MachO.h`](../../include/llvm/Object/MachO.h),
  [`llvm/Object/MachOUniversal.h`](../../include/llvm/Object/MachOUniversal.h),
  [`llvm/TargetParser/Triple.h`](../../include/llvm/TargetParser/Triple.h)。
- 关键类/函数: `Slice`, `Slice::create(Archive&, LLVMContext*)`,
  `Slice::create(IRObjectFile&, uint32_t Align)`,
  `writeUniversalBinary`, `calculateAlignment`, `calculateFileAlignment`。

[`TapiUniversal.cpp`](TapiUniversal.cpp) — 解析 TAPI 文本文件 (`.tbd`,
Apple Text-based Dynamic Library Stub format)。
- 上游: Mach-O linker (LLD), llvm-nm for TBD inspection, TAPI tooling。
- 下游: [`llvm/Object/TapiFile.h`](../../include/llvm/Object/TapiFile.h),
  [`llvm/TextAPI/TextAPIReader.h`](../../include/llvm/TextAPI/TextAPIReader.h),
  [`llvm/TextAPI/InterfaceFile.h`](../../include/llvm/TextAPI/InterfaceFile.h)。
- 关键类/函数: `TapiUniversal::create`,
  `TapiUniversal::ObjectForArch::getAsObjectFile`,
  `TapiUniversal::Libraries`, `ParsedFile`。

[`TapiFile.cpp`](TapiFile.cpp) — 把单 arch `InterfaceFile` 包成
`SymbolicFile` (与正常 object 并列枚举)。
- 上游: `TapiUniversal::ObjectForArch::getAsObjectFile`, LLD/Mach-O 消费
  TBD stubs。
- 下游: [`llvm/BinaryFormat/MachO.h`](../../include/llvm/BinaryFormat/MachO.h),
  [`llvm/TextAPI/InterfaceFile.h`](../../include/llvm/TextAPI/InterfaceFile.h),
  [`llvm/TextAPI/Symbol.h`](../../include/llvm/TextAPI/Symbol.h)。
- 关键类/函数: `TapiFile`, `getFlags(const Symbol*)`,
  `getType(const Symbol*)`, `ObjC1ClassNamePrefix` /
  `ObjC2ClassNamePrefix` / `ObjC2MetaClassNamePrefix` /
  `ObjC2EHTypePrefix` / `ObjC2IVarPrefix`。

### 3.2 对象文件格式 parsers

[`ELF.cpp`](ELF.cpp) — 实现 header-only `llvm/Object/ELF.h` API:
relocation-type-name 查询、dynamic-tag stringification、packed/Android
CREL relocation decoder、section-name table helper。
- 上游: llvm-readobj, llvm-readelf, llvm-objdump, LLD/ELF,
  llvm-dwarfdump (note parsing)。
- 下游: [`llvm/BinaryFormat/ELF.h`](../../include/llvm/BinaryFormat/ELF.h),
  `llvm/BinaryFormat/ELFRelocs/*.def` (per-arch relocation enums via
  `#include`), [`DynamicTags.def`](../../include/llvm/BinaryFormat/DynamicTags.def),
  [`llvm/Object/BBAddrMap.h`](../../include/llvm/Object/BBAddrMap.h),
  [`llvm/Object/Decompressor.h`](../../include/llvm/Object/Decompressor.h)。
- 关键类/函数: `getELFRelocationTypeName`, `getELFSectionTypeName`,
  `ELFFile<ELFT>::crels`, `ELFFile<ELFT>::android_relas`, `decodeCrel`,
  `getDynamicTagAsString`, `getSectionAddress`, `Elf_Note_Iter`。

[`ELFObjectFile.cpp`](ELFObjectFile.cpp) — `ELFObjectFile<ELFT>` 具体
类: 把原始 ELF 结构转为通用 `ObjectFile` iterator 接口; 从 build
attributes / MIPS / RISCV / Hexagon / ARM attribute sections 推断
target-feature。
- 上游: LLD/ELF (主要), llvm-objdump, llvm-readobj, llvm-readelf,
  llvm-nm, llvm-ar。
- 下游: [`llvm/BinaryFormat/ELF.h`](../../include/llvm/BinaryFormat/ELF.h),
  `llvm/MC/{MCInstrAnalysis,TargetRegistry}.h`,
  [`llvm/Object/ELF.h`](../../include/llvm/Object/ELF.h),
  [`llvm/Object/ELFTypes.h`](../../include/llvm/Object/ELFTypes.h),
  `llvm/Support/{ARMAttributeParser,ARMBuildAttributes,HexagonAttributeParser,RISCVAttributeParser,RISCVAttributes}.h`,
  `llvm/TargetParser/{RISCVISAInfo,SubtargetFeature,Triple}.h`。
- 关键类/函数: `ELFObjectFileBase`, `ELFObjectFile<ELFT>`,
  `createELFObjectFile`, `getMIPSFeatures`, `getRISCVFeatures` (via
  `RISCVAttributeParser`), `ELFSymbolRef`, `setARMSubArch`,
  `getElfSymbolTypes`。

[`COFFObjectFile.cpp`](COFFObjectFile.cpp) — 解析 PE/COFF object &
PE 可执行: section/symbol/relocation/import tables, base-64 string-table
解码, debug-directory walking。
- 上游: LLD/COFF (linker), llvm-objdump, llvm-readobj, llvm-nm,
  llvm-strings, llvm-pdbutil。
- 下游: [`llvm/BinaryFormat/COFF.h`](../../include/llvm/BinaryFormat/COFF.h),
  [`llvm/Object/COFF.h`](../../include/llvm/Object/COFF.h),
  [`llvm/Object/WindowsMachineFlag.h`](../../include/llvm/Object/WindowsMachineFlag.h),
  `llvm/Support/{BinaryStreamReader,Endian,MathExtras}.h`。
- 关键类/函数: `COFFObjectFile`, `createCOFFObjectFile`,
  `getNumberOfSections`, `getSection`, `getSymbol`, `getRelocations`,
  `getDebugString`, `decodeBase64StringEntry`, `getCOFFSection`。

[`MachOObjectFile.cpp`](MachOObjectFile.cpp) — 库中最大的文件: 完整
Mach-O parser (load commands, segments, sections, symtab, indirect
symtab, dysymtab, rebase/bind opcodes, chained fixups, exports trie,
CODE_SIGNATURE, LC_DYLD_INFO, Swift metadata)。
- 上游: LLD/Mach-O, llvm-objdump, llvm-readobj, llvm-nm, llvm-otool,
  dsymutil, llvm-codesign, ld64-lld。
- 下游: [`llvm/BinaryFormat/MachO.h`](../../include/llvm/BinaryFormat/MachO.h),
  [`llvm/BinaryFormat/Swift.h`](../../include/llvm/BinaryFormat/Swift.h),
  `llvm/Support/{DataExtractor,LEB128,SwapByteOrder}.h`,
  `llvm/TargetParser/{Host,Triple}.h`。
- 关键类/函数: `MachOObjectFile`,
  `getSegment{,64}LoadCommand`, `getSection{,64}`, `parseLoadCommands`,
  `Rebase/BindEntry`, `ChainedFixupTarget`, `ExportEntry`,
  `getStructOrErr`, `getSectionPtr`, `MachOElement` (LINKEDIT element
  parser)。

[`WasmObjectFile.cpp`](WasmObjectFile.cpp) — 解析 WebAssembly object:
sections, symbols (WASM_SYMBOL_* flags), relocations, init exprs, custom
sections, feature/architecture 推断。
- 上游: lld/wasm (linker), llvm-objdump (wasm), llvm-readobj (wasm),
  wasm-ld。
- 下游: [`llvm/BinaryFormat/Wasm.h`](../../include/llvm/BinaryFormat/Wasm.h),
  [`llvm/Object/Wasm.h`](../../include/llvm/Object/Wasm.h),
  `llvm/Support/{Endian,LEB128,ScopedPrinter}.h`,
  `llvm/TargetParser/{SubtargetFeature,Triple}.h`。
- 关键类/函数: `WasmObjectFile`, `WasmSymbol`, `createWasmObjectFile`,
  `WasmSectionOrderChecker`, `readUint8` / `readUint32` / `readULEB128` /
  `readSignedLEB128` / `readString` (LEB128 helpers),
  `WasmObjectFile::ReadContext`。

[`XCOFFObjectFile.cpp`](XCOFFObjectFile.cpp) — 解析 AIX XCOFF 32/64-bit
object: file header, section headers, symbol table with auxiliary entries
(csect, function, file, block, exception), loader section。
- 上游: LLD/XCOFF (AIX linker), llvm-objdump, llvm-readobj, llvm-nm,
  llvm-readelf for XCOFF。
- 下游: [`llvm/BinaryFormat/XCOFF.h`](../../include/llvm/BinaryFormat/XCOFF.h),
  `llvm/Support/{Compiler,DataExtractor}.h`,
  [`llvm/TargetParser/SubtargetFeature.h`](../../include/llvm/TargetParser/SubtargetFeature.h)。
- 关键类/函数: `XCOFFObjectFile`, `XCOFFFileHeader{32,64}`,
  `XCOFFSectionHeader{32,64}`, `XCOFFRelocation`, `ExceptionSectionEntry`,
  `createXCOFFObjectFile`, `isReservedSectionType`, `getRelocatedLength`,
  `generateXCOFFFixedNameStringRef`, `getLoaderSecSymNameInStrTbl`。

[`GOFFObjectFile.cpp`](GOFFObjectFile.cpp) — 解析 z/OS GOFF (General
Object File Format): physical-record / continuation-record walking,
扁平化到 logical records。
- 上游: LLVM s390x z/OS 工具链 (llvm-objdump, llvm-readobj)。
- 下游: [`llvm/BinaryFormat/GOFF.h`](../../include/llvm/BinaryFormat/GOFF.h),
  [`llvm/Object/GOFF.h`](../../include/llvm/Object/GOFF.h),
  `llvm/Support/{DataExtractor,Errc,raw_ostream}.h`。
- 关键类/函数: `GOFFObjectFile`, `GOFF::RecordType`, `getRecordType`,
  `isContinuation`, `isContinued`, `getContinuousData`,
  `createFlattenedData`。

[`DXContainer.cpp`](DXContainer.cpp) — 解析 DirectX container
(`.DXBC` / `.DXIL`) 用于 shader 调试: DXIL program part, debug info
part, hash part, version 校验。
- 上游: DirectX shader tooling, llvm-dwarfdump for shader DXIL, DXC
  integration。
- 下游:
  [`llvm/BinaryFormat/DXContainer.h`](../../include/llvm/BinaryFormat/DXContainer.h)
  (`dxbc::PartType`),
  `llvm/Support/{Compression,Endian,FormatVariadic}.h`,
  [`llvm/TargetParser/SubtargetFeature.h`](../../include/llvm/TargetParser/SubtargetFeature.h)。
- 关键类/函数: `DXContainer`, `parseHeader`, `parseDXILHeader`,
  `parseParts`, `DXILData` (DXIL / DebugDXIL), `readStruct` /
  `readInteger` / `readString` (helpers), `parseFailed`。

[`Minidump.cpp`](Minidump.cpp) — 解析 MINIDUMP_HEADER + directory entries,
stream 查找, UTF-16 string 解码, exception/memory-info stream 迭代。
- 上游: llvm-symbolizer, llvm-dwarfdump, crash-reporting tools, debugger
  集成, breakpad-style tooling。
- 下游: [`llvm/Support/ConvertUTF.h`](../../include/llvm/Support/ConvertUTF.h),
  [`llvm/BinaryFormat/Minidump.h`](../../include/llvm/BinaryFormat/Minidump.h),
  [`llvm/Support/DataExtractor.h`](../../include/llvm/Support/DataExtractor.h)。
- 关键类/函数: `MinidumpFile`, `MinidumpFile::create`,
  `getRawStream(StreamType)`, `getString`, `getExceptionStreams`,
  `getMemoryInfoList`, `StreamMap`, `ExceptionStreamsIterator`,
  `MemoryInfoIterator`。

### 3.3 Archive / 容器格式

[`Archive.cpp`](Archive.cpp) — 解析读 `ar`-style archive: Unix (thin/
SVR4/GNU), BSD big archive (K_BIG), z/OS extended archive; 处理 member
header, symbol table, thin archive, EBCDIC。
- 上游: llvm-ar (主要), llvm-ranlib, llvm-objcopy, llvm-dwp, LLD (读
  thin archive), gold plugin。
- 下游: [`llvm/Object/Archive.h`](../../include/llvm/Object/Archive.h),
  [`llvm/Object/Binary.h`](../../include/llvm/Object/Binary.h),
  `llvm/Support/{Chrono,ConvertEBCDIC,Endian,EndianStream,FileSystem,Path}.h`,
  [`llvm/TargetParser/Host.h`](../../include/llvm/TargetParser/Host.h)。
- 关键类/函数: `Archive`, `ArchiveMemberHeader`,
  `BigArchiveMemberHeader`, `ZOSArchiveMemberHeader`,
  `CommonArchiveMemberHeader<T>`, `Child` (Archive iterator),
  `Archive::Kind` (`K_GNU` / `K_DARWIN` / `K_COFF` / `K_AIXBIG` / `K_ZOS`
  / `K_BSD`), `SymbolicFile` interface impl, `getNumberOfMembers`,
  `isThin`。

[`ArchiveWriter.cpp`](ArchiveWriter.cpp) — 写 `ar` archive: 自动按
member 检测 kind, 生成 symbol table (GNU/COFF/XCOFF/zOS-style),
支持 thin & fat output。
- 上游: llvm-ar, llvm-ranlib, llvm-dwp, linker plugins。
- 下游:
  `llvm/Object/{Archive,ArchiveWriter,COFF,COFFImportFile,GOFFObjectFile,IRObjectFile,MachO,ObjectFile,SymbolicFile,XCOFFObjectFile}.h`,
  [`llvm/BinaryFormat/Magic.h`](../../include/llvm/BinaryFormat/Magic.h),
  `llvm/Support/{Alignment,EndianStream,MathExtras,Path,SmallVectorMemoryBuffer}.h`,
  [`llvm/TargetParser/Host.h`](../../include/llvm/TargetParser/Host.h)。
- 关键类/函数: `NewArchiveMember`,
  `NewArchiveMember::detectKindFromObject`, `writeArchive`, `SymMap`,
  `MemberData`, archive-kind auto-selection。

[`OffloadBinary.cpp`](OffloadBinary.cpp) — 读 `OffloadBinary` 容器
(host 二进制内嵌的 device image); 通过解析 `offload_binary` magic &
迭代 header 抽取。
- 上游: Clang OpenMP offload driver, libomptarget, llvm-objdump on
  offload sections, CUDA/HIP/SYCL fatbin tooling。
- 下游:
  `llvm/Object/{Archive,Binary,ELFObjectFile,IRObjectFile,ObjectFile}.h`,
  `llvm/IR/{Constants,Module}.h`,
  [`llvm/IRReader/IRReader.h`](../../include/llvm/IRReader/IRReader.h),
  [`llvm/MC/StringTableBuilder.h`](../../include/llvm/MC/StringTableBuilder.h),
  `llvm/Support/{Alignment,SourceMgr}.h`,
  [`llvm/TargetParser/AMDGPUTargetParser.h`](../../include/llvm/TargetParser/AMDGPUTargetParser.h)。
- 关键类/函数: `OffloadBinary`, `OffloadBinary::create`,
  `OffloadBinary::extractHeader`, `OffloadBinary::Header`, `OffloadFile`,
  `SharedMemoryBuffer`, `extractOffloadFiles`, `extractFromObject`。

[`OffloadBundle.cpp`](OffloadBundle.cpp) — 读/写 `__CLANG_OFFLOAD_BUNDLE__`
clang-trick bundle (Foundry 与 CFF / CCOB-compressed bundle); 迭代 ELF
/ Mach-O fat-binary sections 内的 bundled device image。
- 上游: Clang -fembed-offload-bundle / clang-linker-wrapper, llvm-objdump
  offload-bundle, libomptarget。
- 下游:
  `llvm/Object/{Archive,Binary,COFF,ELFObjectFile,IRObjectFile,ObjectFile}.h`,
  `llvm/IR/Module.h`,
  [`llvm/IRReader/IRReader.h`](../../include/llvm/IRReader/IRReader.h),
  [`llvm/MC/StringTableBuilder.h`](../../include/llvm/MC/StringTableBuilder.h),
  `llvm/Support/{BinaryStreamReader,EndianStream,SourceMgr,Timer}.h`。
- 关键类/函数: `OffloadBundleFatBin`, `OffloadBundleEntry`,
  `OffloadBundleURI`, `CompressedOffloadBundle`,
  `OffloadBundleFatBin::create`, `extractOffloadBundle`,
  `getCompressedBundleSize`, `OffloadBundlerTimerGroup`。

### 3.4 Symbol / section utilities

[`SymbolSize.cpp`](SymbolSize.cpp) — 算每个 symbol 的 size (用于
`llvm-objdump --size-sort` 等); 按格式分发到 native size fields (ELF/
Wasm/XCOFF) 或 address-gap fallback。
- 上游: llvm-objdump --size-sort, llvm-nm --size-sort, llvm-readelf -s。
- 下游:
  [`llvm/Object/COFF.h`](../../include/llvm/Object/COFF.h),
  [`llvm/Object/ELFObjectFile.h`](../../include/llvm/Object/ELFObjectFile.h),
  [`llvm/Object/MachO.h`](../../include/llvm/Object/MachO.h),
  [`llvm/Object/Wasm.h`](../../include/llvm/Object/Wasm.h),
  [`llvm/Object/XCOFFObjectFile.h`](../../include/llvm/Object/XCOFFObjectFile.h)。
- 关键类/函数: `computeSymbolSizes`, `SymEntry`, `compareAddress`,
  `getSectionID`, `getSymbolSectionID`。

[`RelocationResolver.cpp`](RelocationResolver.cpp) — 纯 relocation
解析器 (有限子集), 支持 x86_64, AArch64, 32-bit ARM, MIPS,
COFF/IMAGE_REL_*, Mach-O, Wasm; 计算 S + A + offset。
- 上游: llvm-objdump --reloc (计算 effective value), llvm-readobj
  --reloc, fuzzing harness, 计算表达式的链接器。
- 下游:
  [`llvm/BinaryFormat/COFF.h`](../../include/llvm/BinaryFormat/COFF.h),
  [`llvm/BinaryFormat/ELF.h`](../../include/llvm/BinaryFormat/ELF.h),
  [`llvm/BinaryFormat/MachO.h`](../../include/llvm/BinaryFormat/MachO.h),
  [`llvm/BinaryFormat/Wasm.h`](../../include/llvm/BinaryFormat/Wasm.h),
  [`llvm/Object/ELFObjectFile.h`](../../include/llvm/Object/ELFObjectFile.h),
  [`llvm/Object/ObjectFile.h`](../../include/llvm/Object/ObjectFile.h),
  [`llvm/Object/SymbolicFile.h`](../../include/llvm/Object/SymbolicFile.h),
  `llvm/Support/{Casting,Error,ErrorHandling}.h`,
  [`llvm/TargetParser/Triple.h`](../../include/llvm/TargetParser/Triple.h)。
- 关键类/函数: `resolveRelocation`, `supportsRelocationType`,
  `resolveX86_64`, `resolveAArch64`, `resolve{,Mips,COFF,MachO,Wasm}`
  变体, `getELFAddend`。

[`ModuleSymbolTable.cpp`](ModuleSymbolTable.cpp) — 从内存中的 LLVM IR
(Modules + inline asm) 构建 `ModuleSymbolTable`; 用于 LTO 生成
bitcode symbol table 而无需经过 MC。
- 上游: LTO / ThinLTO symbol-table writer
  ([`LTO.cpp`](../LTO/LTO.cpp)),
  `WriteBitcodeToFile`, clang -flto。
- 下游: [`llvm/Object/ModuleSymbolTable.h`](../../include/llvm/Object/ModuleSymbolTable.h),
  [`RecordStreamer.h`](RecordStreamer.h) (内部, 同目录),
  `llvm/IR/{Function,GlobalAlias,GlobalValue,GlobalVariable,InlineAsm,Module}.h`,
  `llvm/MC/{MCAsmInfo,MCContext,MCInstrInfo,MCObjectFileInfo,MCSubtargetInfo,MCSymbol,MCTargetOptions,TargetRegistry}.h`,
  `llvm/MC/MCParser/{MCAsmParser,MCTargetAsmParser}.h`,
  [`llvm/TargetParser/Triple.h`](../../include/llvm/TargetParser/Triple.h)。
- 关键类/函数: `ModuleSymbolTable::addModule`,
  `ModuleSymbolTable::CollectAsmSymbols`,
  `ModuleSymbolTable::CollectAsmSymvers`,
  `ModuleSymbolTable::printSymbolName`, `initializeRecordStreamer`,
  `AsmSymbol`, `Symbol` (public struct), `SymTab`。

[`RecordStreamer.cpp`](RecordStreamer.cpp) + [`RecordStreamer.h`](RecordStreamer.h)
— `RecordStreamer : MCStreamer`, 非发射的 MCStreamer; 解析 IR module
的 inline asm 时只记录 asm-defined/used/.symver symbol。
- 上游: `ModuleSymbolTable::CollectAsmSymbols`,
  `ModuleSymbolTable::CollectAsmSymvers`; 最终 LTO symbol-table writer。
- 下游:
  `llvm/MC/{MCStreamer,MCDirectives}.h`,
  [`llvm/IR/Mangler.h`](../../include/llvm/IR/Mangler.h),
  [`llvm/IR/Module.h`](../../include/llvm/IR/Module.h),
  `llvm/MC/{MCContext,MCSymbol}.h`,
  `llvm/ADT/{DenseMap,MapVector,StringMap}.h`。
- 关键类/函数: `RecordStreamer`, `State {NeverSeen, Global, Defined,
  DefinedGlobal, DefinedWeak, Used, UndefinedWeak}`, `markDefined`,
  `markGlobal`, `markUsed`, `emitELFSymverDirective`,
  `flushSymverDirectives`, `symverAliases`, `visitUsedSymbol`。
- **注意**: 这是 `llvm/lib/Object/` 下唯一的内部头 (在 lib/ 而非
  include/), 因为与 `ModuleSymbolTable.cpp` 紧耦合。

### 3.5 IR / bitcode 对象层

[`IRObjectFile.cpp`](IRObjectFile.cpp) — `IRObjectFile : SymbolicFile`,
把 bitcode module (裸的或嵌入 ELF/COFF/Mach-O/Wasm `.llvm.lto` section
的) 包成通用 symbol iterator。
- 上游: LTO (llvm-lto2, libLTO), llvm-nm --bitcode, llvm-objdump on LTO
  object, gold-plugin / LLD 读 ThinLTO bitcode。
- 下游: [`llvm/BinaryFormat/Magic.h`](../../include/llvm/BinaryFormat/Magic.h),
  [`llvm/Bitcode/BitcodeReader.h`](../../include/llvm/Bitcode/BitcodeReader.h),
  [`llvm/IR/Module.h`](../../include/llvm/IR/Module.h),
  [`llvm/Object/ObjectFile.h`](../../include/llvm/Object/ObjectFile.h)。
- 关键类/函数: `IRObjectFile::create`, `findBitcodeInObject`,
  `findBitcodeInMemBuffer`, `getTargetTriple`, `readIRSymtab`,
  `IRObjectFile::Mods`。

[`IRSymtab.cpp`](IRSymtab.cpp) — `irsymtab` 符号表的读写器, 内嵌于
LLVM bitcode (LTO native format): 用紧凑 versioned layout 编码
module / comdat / symbol / strtab; 处理 bitcode version upgrade。
- 上游: LTO / ThinLTO bitcode 读+写, llvm-dis / llvm-as 验证, libLTO。
- 下游: [`llvm/Bitcode/BitcodeReader.h`](../../include/llvm/Bitcode/BitcodeReader.h),
  `llvm/IR/{Comdat,DataLayout,GlobalAlias,GlobalObject,Mangler,Metadata,Module}.h`,
  [`llvm/MC/StringTableBuilder.h`](../../include/llvm/MC/StringTableBuilder.h),
  [`llvm/Object/ModuleSymbolTable.h`](../../include/llvm/Object/ModuleSymbolTable.h),
  [`llvm/Object/SymbolicFile.h`](../../include/llvm/Object/SymbolicFile.h),
  `llvm/Support/{Allocator,Casting,CommandLine,StringSaver,VCSRevision,raw_ostream}.h`,
  [`llvm/TargetParser/Triple.h`](../../include/llvm/TargetParser/Triple.h)。
- 关键类/函数: `irsymtab::readBitcode`, `irsymtab::write`,
  `Builder` (内部), `storage::{Symbol,Comdat,Module,Str,Uncommon,Range}`,
  `kExpectedProducerName`, `DisableBitcodeVersionUpgrade` (cl::opt),
  `IRSymtabFile`。

### 3.6 专项 section parsers

[`BBAddrMap.cpp`](BBAddrMap.cpp) — 解码 BBAddrMap section
(`.llvm_bb_addr_map`), 用于 `--unique-internal-linkage-names` / sample
PGO: per-function BB address range, PGO analysis map (versions 2-5)。
- 上游: llvm-objdump --bb-addr-map, sample-profile loader,
  machine-function-splitter。
- 下游:
  [`llvm/BinaryFormat/BBAddrMap.h`](../../include/llvm/Object/BBAddrMap.h)
  (constants),
  `llvm/Support/{DataExtractor,MathExtras}.h`。
- 关键类/函数: `decodeBBAddrMapPayload`,
  `BBAddrMap::Features::decode`, `readULEB128As`, `PGOAnalysisMap`。

[`BuildID.cpp`](BuildID.cpp) — 抽取 ELF 的 GNU Build-ID note
(`NT_GNU_BUILD_ID`), 提供 `BuildIDFetcher` 定位
`.build-id/ab/cdef.debug` 下的匹配 split debug file。
- 上游: llvm-symbolizer, llvm-dwarfdump, gdb (via debuginfod), split-dwarf
  查找。
- 下游: [`llvm/Object/ELFObjectFile.h`](../../include/llvm/Object/ELFObjectFile.h),
  `llvm/Support/{Error,FileSystem,Path}.h`。
- 关键类/函数: `parseBuildID`, `getBuildID(const ObjectFile*)`,
  `BuildIDFetcher::fetch`, `BuildIDRef`, `BuildID`, `findBuildID`
  (内部)。

[`COFFImportFile.cpp`](COFFImportFile.cpp) — 解析 COFF short-import
file (`.lib` 由 `lib /DEF:` 生成) 和 `writeImportLibrary`: 合成 import
from DLL 的 COFF object。
- 上游: lld-link (import lib 生成), llvm-dlltool, llvm-lib。
- 下游: [`llvm/BinaryFormat/COFF.h`](../../include/llvm/BinaryFormat/COFF.h),
  `llvm/Object/{Archive,ArchiveWriter,COFF}.h`,
  `llvm/Support/{Allocator,Endian,Error,Path}.h`。
- 关键类/函数: `COFFImportFile`, `getFileFormatName`, `getExportName`,
  `writeImportLibrary`, `ImportNameType` (`IMPORT_NAME_*`),
  `applyNameType`。

[`COFFModuleDefinition.cpp`](COFFModuleDefinition.cpp) — 解析 MSVC
module-definition (`.def`) 文件: `LIBRARY` / `EXPORTS` / `HEAPSIZE` /
`VERSION` / `SECTIONS` 等; 用于驱动 import-lib 生成。
- 上游: lld-link /DEF:, llvm-dlltool, llvm-lib (def parsing)。
- 下游: [`llvm/Object/COFFImportFile.h`](../../include/llvm/Object/COFFImportFile.h),
  [`llvm/Object/Error.h`](../../include/llvm/Object/Error.h),
  `llvm/Support/{Error,Path}.h`。
- 关键类/函数: `Lexer`, `Parser`, `Token` (Kind enum), `isDecorated`,
  `parseCOFFModuleDefinition`, `Exports`, `ExcludedSymbols`。

[`SFrameParser.cpp`](SFrameParser.cpp) — 解析 SFrame stack-frame
unwinding section (`.sframe`) v2: 产生 FDE range 给 unwinder。
- 上游: llvm-objdump --sframe, stack-unwinder 集成 (ghs/cafe sframe
  consumer), debugger tooling。
- 下游: [`llvm/BinaryFormat/SFrame.h`](../../include/llvm/BinaryFormat/SFrame.h)
  (`Preamble`, `Header`, `FuncDescEntry`),
  [`llvm/Object/Error.h`](../../include/llvm/Object/Error.h),
  `llvm/Support/{FormatVariadic,MathExtras}.h`。
- 关键类/函数: `SFrameParser<E>::create`, `getAuxHeader`, `fdes`,
  `getAbsoluteStartAddress`, `getFDEBase`,
  `getDataSlice{,As{,AsArrayOf}}`。

[`WindowsMachineFlag.cpp`](WindowsMachineFlag.cpp) — 小工具: 把
`/machine:x64|arm|arm64|arm64ec|arm64x|…` 字符串映射到
`COFF::MachineTypes` (及反向), 与 MSVC `lib.exe` 平行。
- 上游: lld-link (--machine), llvm-lib, llvm-dlltool。
- 下游: [`llvm/BinaryFormat/COFF.h`](../../include/llvm/BinaryFormat/COFF.h),
  [`llvm/Support/ErrorHandling.h`](../../include/llvm/Support/ErrorHandling.h)。
- 关键类/函数: `getMachineType(StringRef)`,
  `machineToStr(COFF::MachineTypes)`。

[`WindowsResource.cpp`](WindowsResource.cpp) — 解析与序列化 Windows
`.res` 文件 (icon/string/manifest 资源), 写入 COFF 可执行
(`WindowsResourceCOFFWriter`)。
- 上游: lld-link (`.rc` 经 windres / llvm-windres 编译), llvm-windres。
- 下游: [`llvm/BinaryFormat/COFF.h`](../../include/llvm/BinaryFormat/COFF.h),
  [`llvm/Object/COFF.h`](../../include/llvm/Object/COFF.h),
  [`llvm/Object/WindowsMachineFlag.h`](../../include/llvm/Object/WindowsMachineFlag.h),
  `llvm/Support/{BinaryStreamReader,FormatVariadic,MathExtras,ScopedPrinter}.h`。
- 关键类/函数: `WindowsResource`, `ResourceEntryRef`,
  `ResourceSectionRef`, `WindowsResourceCOFFWriter`, `EmptyResError`,
  `WindowsResourceParser`, `getHeadEntry`,
  `writeWindowsResourceCOFF`。

[`FaultMapParser.cpp`](FaultMapParser.cpp) — 只读 stream-style
`FaultMapParser`, 处理 LLVM FaultMap section (`.llvm_faultmaps`),
由 `-fsanitize=fuzzer` / Fuchsia fault handling 发射; 只含打印
helper (parser 类是 header-only 在
[`llvm/include/llvm/Object/FaultMapParser.h`](../../include/llvm/Object/FaultMapParser.h))。
- 上游: llvm-objdump --fault-map, Fuchsia fault-injection 工具。
- 下游:
  `llvm/Support/{ErrorHandling,Format,raw_ostream}.h`。
- 关键类/函数: `printFaultType`,
  `operator<<(raw_ostream&, const FunctionFaultInfoAccessor&)`,
  `operator<<(raw_ostream&, const FunctionInfoAccessor&)`,
  `operator<<(raw_ostream&, const FaultMapParser&)`。

---

## §4. 关键调用链

### 4.1 通用 `createObjectFile` 入口 (LLD/ELF 读入文件)

```
LLD/ELF driver ([`Driver.cpp`](../../../lld/ELF/Driver.cpp))
  └─ createObjectFile(MemoryBufferRef) [ObjectFile.cpp]
       ├─ file_magic = identify_magic(MemoryBuffer) [BinaryFormat/Magic.h]
       └─ switch on file_magic:
            ├─ ELF64L → createELFObjectFile<ELF64>(buf, ...) [ELFObjectFile.cpp]
            │    └─ 构造 ELFObjectFile<ELFT>
            │         ├─ 读 ELF header / program headers / section headers
            │         ├─ ELF.cpp::getDynamicTagAsString (lazy lookup)
            │         ├─ 解析 target features (RISCV/MIPS/Hexagon/ARM attributes)
            │         └─ 构造 ELF symbol/section/relocation iterator
            ├─ COFF  → createCOFFObjectFile(buf) [COFFObjectFile.cpp]
            ├─ MachO → createMachOObjectFile(buf) [MachOObjectFile.cpp]
            ├─ WASM  → createWasmObjectFile(buf) [WasmObjectFile.cpp]
            ├─ XCOFF → createXCOFFObjectFile(buf) [XCOFFObjectFile.cpp]
            ├─ GOFF  → createGOFFObjectFile(buf) [GOFFObjectFile.cpp]
            └─ DXC   → createDXContainerObjectFile(buf) [DXContainer.cpp]
                 → 返回 std::unique_ptr<ObjectFile>
                    → LLD 用 SectionRef / SymbolRef / RelocationRef 迭代
```

### 4.2 `createBinary` 顶层分发 (llvm-objdump / llvm-readobj 等)

```
llvm-objdump main
  └─ createBinary(file, ctx) [Binary.cpp]
       └─ file_magic → switch:
            ├─ archive_magic     → Archive [Archive.cpp]
            │    └─ iter Archive::Child() → recursively createObjectFile
            ├─ macho_universal_* → MachOUniversalBinary [MachOUniversal.cpp]
            │    └─ for each ObjectForArch:
            │         └─ ObjectForArch::getAsObjectFile → MachOObjectFile
            ├─ minidump          → MinidumpFile [Minidump.cpp]
            ├─ windows_resource  → WindowsResource [WindowsResource.cpp]
            ├─ tapi_universal_*  → TapiUniversal [TapiUniversal.cpp]
            └─ 对象 (其它)         → createObjectFile → 见 §4.1
                 → llvm-objdump dump sections / symbols / relocations
```

### 4.3 LLVM IR → bitcode symbol table (LTO 写出)

```
LTO 写出 bitcode (llvm/lib/LTO/LTO.cpp)
  └─ WriteBitcodeToFile(M, ...) [lib/Bitcode/Writer]
       └─ ModuleSymbolTable::addModule(M) [ModuleSymbolTable.cpp]
            ├─ ModuleSymbolTable::CollectAsmSymbols
            │    └─ initializeRecordStreamer [RecordStreamer.h]
            │         └─ MCStreamer 接口; 解析 asm 时:
            │              ├─ markDefined / markGlobal / markUsed
            │              └─ emitELFSymverDirective
            ├─ ModuleSymbolTable::CollectAsmSymvers
            │    └─ flushSymverDirectives [RecordStreamer.cpp]
            └─ ModuleSymbolTable::printSymbolName
                 → 输出到 SymTab
   │
   ▼
irsymtab::write [IRSymtab.cpp] → 嵌入 bitcode
```

### 4.4 Mach-O fat-binary 重建 (llvm-lipo)

```
llvm-lipo main
  └─ 创建 MachOUniversalBinary (输入) [MachOUniversal.cpp]
  └─ add input slices:
       ├─ Slice::create(Archive&, ...) [MachOUniversalWriter.cpp]
       └─ Slice::create(IRObjectFile&, Align) [MachOUniversalWriter.cpp]
            ├─ calculateAlignment [MachOUniversalWriter.cpp]
            └─ calculateFileAlignment
  └─ writeUniversalBinary(out_path)
       └─ 写 FAT_MAGIC_64 header
       └─ 对每个 slice 写对齐后的 slice data
```

### 4.5 抽取 GPU 设备镜像 (Clang OpenMP offload)

```
Clang -fopenmp -fembed-offload-binary
  └─ 编译 host → 编译 device (NVPTX/AMDGPU)
  └─ offloading::wrapOpenMPBinaries [lib/Frontend/Offloading/OffloadWrapper.cpp]
       └─ Offloading::Utility::containerizeImage
            └─ 写出 .img.bin global + __tgt_bin_desc 全局

运行时 (libomptarget)
  └─ 读 host module 中的 __tgt_offload_entry
       └─ OffloadBinary::extractHeader [OffloadBinary.cpp]
            └─ 解析 offload_binary magic
            └─ extractOffloadFiles
                 └─ 抽取 ELF/CUDA/HIP 镜像 + metadata
                      → 传给 plugin 加载
```

### 4.6 BBAddrMap 解码 (sample PGO / func splitting)

```
llvm-objdump --bb-addr-map < input.o
  └─ ELFObjectFile::getSectionByName(".llvm_bb_addr_map")
       └─ 取 raw data → BBAddrMap::decode [BBAddrMap.cpp]
            ├─ readULEB128As (utility)
            ├─ 解 version 2-5 头
            ├─ 对每个 function: 解 PGOAnalysisMap
            └─ 输出每个 BB 的 (offset, size, succ indices)
```

### 4.7 ELF debug section 解压 (llvm-dwarfdump)

```
llvm-dwarfdump < input.o
  └─ ELFObjectFile::getSection(".debug_info")
       └─ Decompressor::create(section data) [Decompressor.cpp]
            ├─ Decompressor::consumeCompressedHeader (读 Elf32_Chdr)
            │    ├─ ELFCOMPRESS_ZLIB → zlib 解压
            │    └─ ELFCOMPRESS_ZSTD → zstd 解压
            └─ Decompressor::decompress → 输出 .debug_info raw data
                 → DWARFContext::parse (lib/DebugInfo/DWARF)
```

### 4.8 COFF import lib 合成 (lld-link / llvm-dlltool)

```
lld-link /DEF:foo.def /DLL /OUT:foo.dll
  └─ 解析 foo.def
       └─ parseCOFFModuleDefinition [COFFModuleDefinition.cpp]
            ├─ Lexer::Lex → tokens (LIBRARY, EXPORTS, ...)
            └─ Parser::parseExports → Exports list
  └─ writeImportLibrary("foo.lib", exports) [COFFImportFile.cpp]
       ├─ 为每个 export 合成 COFFImportFile
       ├─ 写 short-import object (IMPORT_NAME_*, dll_name, ...)
       └─ 写 archive (.lib) via writeArchive [ArchiveWriter.cpp]
```

---

## §5. 推荐阅读顺序

### 阶段 1: 数据模型 (1-2 小时)
1. [`ObjectFile.cpp`](ObjectFile.cpp) + 头 [`ObjectFile.h`](../../include/llvm/Object/ObjectFile.h) — 核心 iterator 接口
   (`SymbolRef` / `SectionRef` / `RelocationRef`)。
2. [`Binary.cpp`](Binary.cpp) — `Binary` 基类 + `createBinary` 工厂。
3. [`SymbolicFile.cpp`](SymbolicFile.cpp) — symbol-only 超类。
4. [`Error.cpp`](Error.cpp) — `object_error` enum + `BinaryError`。

### 阶段 2: 选一个具体格式深入 (3-4 小时)
任选一种深入:
- **ELF** (主流): [`ELFObjectFile.cpp`](ELFObjectFile.cpp) →
  [`ELF.cpp`](ELF.cpp) (配 [`BinaryFormat/ELF.h`](../../include/llvm/BinaryFormat/ELF.h))。
- **COFF** (Windows): [`COFFObjectFile.cpp`](COFFObjectFile.cpp) →
  [`COFFImportFile.cpp`](COFFImportFile.cpp) →
  [`COFFModuleDefinition.cpp`](COFFModuleDefinition.cpp)。
- **Mach-O** (Apple): [`MachOObjectFile.cpp`](MachOObjectFile.cpp) →
  [`MachOUniversal.cpp`](MachOUniversal.cpp) →
  [`MachOUniversalWriter.cpp`](MachOUniversalWriter.cpp) →
  [`TapiFile.cpp`](TapiFile.cpp) / [`TapiUniversal.cpp`](TapiUniversal.cpp)。
- **Wasm**: [`WasmObjectFile.cpp`](WasmObjectFile.cpp)。
- **XCOFF** (AIX): [`XCOFFObjectFile.cpp`](XCOFFObjectFile.cpp)。
- **GOFF** (z/OS): [`GOFFObjectFile.cpp`](GOFFObjectFile.cpp)。
- **DXContainer**: [`DXContainer.cpp`](DXContainer.cpp)。
- **Minidump**: [`Minidump.cpp`](Minidump.cpp)。

### 阶段 3: Archive / Offload 容器 (2 小时)
1. [`Archive.cpp`](Archive.cpp) — `ar` 解析。
2. [`ArchiveWriter.cpp`](ArchiveWriter.cpp) — `ar` 生成。
3. [`OffloadBinary.cpp`](OffloadBinary.cpp) + [`OffloadBundle.cpp`](OffloadBundle.cpp) — offload 容器。

### 阶段 4: IR / LTO 集成 (2-3 小时)
1. [`IRObjectFile.cpp`](IRObjectFile.cpp) — bitcode 适配。
2. [`IRSymtab.cpp`](IRSymtab.cpp) — irsymtab 读写。
3. [`ModuleSymbolTable.cpp`](ModuleSymbolTable.cpp) — module → symbol table。
4. [`RecordStreamer.cpp`](RecordStreamer.cpp) + [`RecordStreamer.h`](RecordStreamer.h) —
   解析 inline asm 时记录 symbol。

### 阶段 5: 工具 / 通用 (1-2 小时)
1. [`Decompressor.cpp`](Decompressor.cpp) — zlib/zstd 解压。
2. [`SymbolSize.cpp`](SymbolSize.cpp) — symbol size 计算。
3. [`RelocationResolver.cpp`](RelocationResolver.cpp) — 纯 relocation 解析。

### 阶段 6: 专项 section (按需)
- [`BuildID.cpp`](BuildID.cpp) (split-dwarf / debuginfod)
- [`BBAddrMap.cpp`](BBAddrMap.cpp) (sample PGO)
- [`SFrameParser.cpp`](SFrameParser.cpp) (stack unwinding)
- [`FaultMapParser.cpp`](FaultMapParser.cpp) (fuzzer fault map)
- [`WindowsResource.cpp`](WindowsResource.cpp) (Windows .res)
- [`WindowsMachineFlag.cpp`](WindowsMachineFlag.cpp) (small util)
- [`Object.cpp`](Object.cpp) (C API)

---

## §6. 常用操作指南

### 6.1 添加新对象格式 (假设 `MyFormat`)

1. 在 [`llvm/include/llvm/BinaryFormat/`](../../include/llvm/BinaryFormat/)
   加 `MyFormat.h` (enums, packed structs), 注册 file_magic 到
   [`Magic.h`](../../include/llvm/BinaryFormat/Magic.h)。
2. 在 [`llvm/include/llvm/Object/`](../../include/llvm/Object/) 加
   `MyFormat.h`, 定义:
   - `class MyFormatObjectFile : public ObjectFile`
   - 内部结构 `MyFormat{Header,Section,Symbol,Relocation}`
3. 在 `MyFormatObjectFile.cpp`
   实现: `createMyFormatObjectFile(MemoryBufferRef)` + override 全部
   `ObjectFile` 虚函数 (sections_begin, symbols_begin, ...)。
4. 在 [`llvm/lib/Object/ObjectFile.cpp`](ObjectFile.cpp) 的
   `createObjectFile` switch 加分支, 在
   `ObjectFile.cpp::makeTriple` 加 `ArchType::MyArch` dispatch。
5. 单测: `llvm/unittests/Object/MyFormatTest.cpp` +
   `llvm/test/tools/llvm-readobj/MyFormat/`。
6. (可选) 加 `MyFormatRelocs.def` 到
   [`llvm/include/llvm/BinaryFormat/`](../../include/llvm/BinaryFormat/),
   与 [`RelocationResolver.cpp`](RelocationResolver.cpp) 加 `resolveMyFormat`。

### 6.2 添加新 Archive kind

1. 在 [`llvm/include/llvm/Object/Archive.h`](../../include/llvm/Object/Archive.h)
   加新 enum `Archive::Kind` (e.g., `K_MYARCH`)。
2. 在 [`Archive.cpp`](Archive.cpp) 加对应的 `Child` subclass 与识别
   逻辑 (从 magic / first member header 判 kind)。
3. 在 [`ArchiveWriter.cpp`](ArchiveWriter.cpp) 加 `writeArchive` 分支
   + `computeMemberFileContent` 中该 kind 的 symbol table 生成。
4. 单测: `llvm/test/Object/archive-myarch.test` +
   `llvm/test/tools/llvm-ar/myarch.test`。

### 6.3 添加新 OffloadBundle kind

1. 在 [`llvm/include/llvm/Object/OffloadBundle.h`](../../include/llvm/Object/OffloadBundle.h)
   加新 `OffloadBundleEntry::CodeObjectTarget` / magic。
2. 在 [`OffloadBundle.cpp`](OffloadBundle.cpp) 的
   `OffloadBundleFatBin::create` + `extractOffloadBundle` 加新
   magic 的解析/打包分支。
3. 配套 [`lib/Frontend/Offloading/OffloadWrapper.cpp`](../Frontend/Offloading/OffloadWrapper.cpp)
   的 `wrapXxxBinary` 加新 target 的注册入口。
4. 单测: `llvm/test/Object/OffloadBundle/`。

### 6.4 给 LTO / ThinLTO 添加新 symbol 信息

1. 在 [`llvm/include/llvm/Object/ModuleSymbolTable.h`](../../include/llvm/Object/ModuleSymbolTable.h)
   加新字段。
2. 在 [`ModuleSymbolTable.cpp`](ModuleSymbolTable.cpp) 实现
   `CollectAsmSymbols` / `CollectAsmSymvers` 的新采集。
3. 在 [`RecordStreamer.cpp`](RecordStreamer.cpp) 给 `RecordStreamer` 加
   对应的 `markXxx` 接口 (如需记录 asm 中新形式的引用)。
4. 同步 [`IRSymtab.cpp`](IRSymtab.cpp) 的 `storage::Symbol` /
   `write` / `readBitcode`。
5. 单测: `llvm/test/Object/IRSymtab/` + LTO 测试用例。

### 6.5 添加新错误码

1. 在 [`llvm/include/llvm/Object/Error.h`](../../include/llvm/Object/Error.h)
   的 `object_error` enum 加新值 (注意排到末尾, 不改旧值的编号)。
2. 在 [`Error.cpp`](Error.cpp) 的 `object_category::message()` 加对应
   文案。
3. 在相关 parser 用 `createStringError(object_error::xxx, msg)` 抛出。
4. 单测: `llvm/unittests/Object/ErrorTest.cpp`。

---

## §7. NT 注释索引

当前 `llvm/lib/Object/` 下尚无 `// <NT>` 注释。姊妹 overview:

- [`llvm/lib/IR/0-overview.md`](../IR/0-overview.md) — LLVM IR 层
- [`llvm/lib/CodeGen/0-overview.md`](../CodeGen/0-overview.md) — target-independent CodeGen
- [`llvm/lib/Target/RISCV/0-overview.md`](../Target/RISCV/0-overview.md) — RISCV 后端
- [`llvm/lib/TargetParser/0-overview.md`](../TargetParser/0-overview.md) — TargetParser
- [`llvm/lib/Analysis/0-overview.md`](../Analysis/0-overview.md) — IR 层分析
- [`llvm/lib/Frontend/0-overview.md`](../Frontend/0-overview.md) — 前端桥接层

按"少而精"原则, 添加 NT 注释时建议优先级:

1. [`ELFObjectFile.cpp`](ELFObjectFile.cpp) — 5-8 段 (核心 ELF iterator; entry 类
   + target-feature 推断)
2. [`MachOObjectFile.cpp`](MachOObjectFile.cpp) — 5-8 段 (库中最大; load
   command / chained fixup / exports trie 各一段)
3. [`OMPIRBuilder.cpp`](../Frontend/OpenMP/OMPIRBuilder.cpp) — (在
   `lib/Frontend/`, 不是本目录) OpenMPIRBuilder 主类
4. [`Archive.cpp`](Archive.cpp) — 3-5 段 (多 kind archive 识别 + thin 处理)
5. [`OffloadBinary.cpp`](OffloadBinary.cpp) + [`OffloadBundle.cpp`](OffloadBundle.cpp) —
   各 3-5 段 (offload 容器)
6. [`IRSymtab.cpp`](IRSymtab.cpp) + [`ModuleSymbolTable.cpp`](ModuleSymbolTable.cpp)
   — 各 3-5 段 (LTO 集成)
7. 单 utility 文件 ([`Decompressor.cpp`](Decompressor.cpp)、
   [`SymbolSize.cpp`](SymbolSize.cpp)、
   [`BuildID.cpp`](BuildID.cpp) 等) — 1-2 段

---

**姊妹文档**: 已建立完整目录索引。`llvm-c/Object.h` C API 用户可结合
[`llvm/tools/llvm-c-test`](../../tools/llvm-c-test/) 看用法示例。
LLVM debug-info 处理 (DWARF / PDB) 不在本目录, 在 `llvm/lib/DebugInfo/`。