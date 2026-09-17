<!-- <NT>overview:llvm/lib/ObjectYAML/ -->

# LLVM ObjectYAML 库导读 — `llvm/lib/ObjectYAML/`

> 本文档梳理 `llvm/lib/ObjectYAML/` 目录下全部源文件（31 个 `.cpp`，全
> 在顶层无子目录）的职责、上下游与推荐阅读顺序。目标读者：想理解 LLVM
> **基于 YAML 的对象文件构造库**（`llvm-yaml2obj` / `obj2yaml`）的开发者，
> 以及要给工具加新格式支持 / 写 fuzz 测试 seed 的人。
>
> 所有路径相对 `llvm/lib/ObjectYAML/`。同名头文件位于
> `llvm/include/llvm/ObjectYAML/`。

---

## §0. ObjectYAML 库在 LLVM 中的位置

`lib/ObjectYAML` 是 LLVM **YAML ↔ 二进制对象** 双向转换层。它支撑两个
核心工具:

- **`llvm-yaml2obj`** (tools/yaml2obj/): 读人类可读的 YAML → 写出真正的
  二进制对象 (.o / .so / .a / .elf / .wasm / ...) 。**Fuzz 测试**、
  **回归测试**、**手工构造测试用例**的黄金工具。
- **`obj2yaml`** (tools/obj2yaml/): 反向, 二进制对象 → YAML, 用于调试
  与 round-trip 验证。

它本身**没有**任何 IR/MC 层, 直接调用 `lib/Object` 的格式 enum 和
`lib/BinaryFormat` 的 packed struct 定义。

包含的内容（按职责）:

- **核心 dispatcher**: `ObjectYAML.cpp` (按 tag 分发) + `yaml2obj.cpp`
  (YAML→二进制 dispatch) + `YAML.cpp` (`BinaryRef`) +
  `ContiguousBlobAccumulator.cpp` (共享 blob 写入器)。
- **11 种对象格式**的 YAML 映射 + 二进制 emitter (ELF / COFF / Mach-O /
  Wasm / XCOFF / GOFF / DXContainer / Minidump / Archive / Offload /
  BBAddrMap)。
- **DWARF** (.debug_*) YAML ↔ 二进制 — 单独一对。
- **CodeView** (.debug$S/.debug$T/.debug$P/.debug$H) YAML ↔ 二进制 — 4
  个文件, 被 COFF emitter 调用。

它在流水线中的位置:

```
┌────────────────────────────────────────────────────────┐
│ 测试 / fuzz seed / 手工用例 (YAML 文本)                  │
│   !ELF / !COFF / !WASM / !mach-o / !dxcontainer / ...   │
└────────────────────────────────────────────────────────┘
                │
                ▼
┌────────────────────────────────────────────────────────┐
│ lib/ObjectYAML  (本目录)                                 │
│   · ObjectYAML.cpp: tag dispatcher                      │
│   · XxxYAML.cpp: YAML 文本 → in-memory 结构             │
│   · XxxEmitter.cpp: in-memory 结构 → 二进制             │
│   · ContiguousBlobAccumulator: 共享 blob 写入器          │
│   · DWARFYAML/DWARFEmitter: 独立 DWARF 段               │
│   · CodeViewYAML*: CV symbols/types/hashes YAML        │
└────────────────────────────────────────────────────────┘
                │
                ├─→ tools/yaml2obj: YAML → 二进制 (.o 文件)
                ├─→ tools/obj2yaml: 二进制 → YAML
                ├─→ llvm-ifs, llvm-profdata, llvm-pdbutil 等
                ├─→ unittests/ObjectYAML/*Test.cpp (round-trip 测试)
                └─→ lib/CGData, lib/DebugInfo/CodeView 等
```

### §0.1 与 [`lib/Object`](../Object/0-overview.md) 的姊妹关系

`lib/ObjectYAML` 与 [`lib/Object`](../Object/0-overview.md) 是 LLVM 中一对
"读 / 写镜子" — **同一份字节布局的两面**。理解这个关系是看懂本目录
所有 emitter 设计的钥匙。

```
                    写 (本目录)                读
   YAML 文本  ──────►  二进制 .o  ──────►  ObjectFile (内存)
              ▲                              │
              │                              │
              │        共享同一套类型         │
              └──────────────────────────────┘
                            │
                  ┌─────────┴──────────┐
                  ▼                    ▼
       llvm/include/llvm/    llvm/include/llvm/
         BinaryFormat/         Object/
       (packed struct +       (ELFObjectFile<ELFT>,
        enum + magic)          COFFObjectFile, ...
                                symbols_begin(), sections())
```

#### 角色对照

| 维度 | [`lib/Object`](../Object/0-overview.md) (解析) | `lib/ObjectYAML` (本目录, 构造) |
|------|----------------------------------------------|--------------------------------|
| **方向** | 二进制 → 内存 (`ObjectFile`) | YAML → 二进制 (.o 文件) |
| **入口** | `createBinary()` / `ObjectFile::createObjectFile` | `convertYAML()` → `yaml2elf/wasm/coff/...` |
| **输出** | `Expected<std::unique_ptr<ObjectFile>>` | `raw_fd_ostream` 写出文件 |
| **典型工具** | `llvm-readelf`, `llvm-objdump`, `llvm-nm`, [`obj2yaml`](../../tools/obj2yaml/) | [`llvm-yaml2obj`](../../tools/yaml2obj/) |
| **共同依赖** | [`llvm/BinaryFormat/`](../../include/llvm/BinaryFormat/) + [`llvm/Object/`](../../include/llvm/Object/) 头 | **完全相同** |

#### 三个共享层

1. **结构定义层** ([`llvm/include/llvm/BinaryFormat/`](../../include/llvm/BinaryFormat/))
   — ELF/COFF/Mach-O/Wasm/XCOFF/Minidump/DXContainer 的 packed struct、
   枚举、magic。`ObjectYAML` 的 emitter 和 `Object` 的 parser 都读这些,
   保证**字节布局 100% 一致**。
   - 例如: [`ELFYAML.cpp`](ELFYAML.cpp) 和 [`ELFObjectFile.cpp`](../Object/ELFObjectFile.cpp)
     都用 [`llvm/BinaryFormat/ELF.h`](../../include/llvm/BinaryFormat/ELF.h)
     的 `Elf64_Ehdr` / `Elf64_Shdr` 定义。
   - [`COFFEmitter.cpp`](COFFEmitter.cpp) 写 `COFF::section_header32/64`,
     [`COFFObjectFile.cpp`](../Object/COFFObjectFile.cpp) 读同一份。

2. **类型包装层** ([`llvm/include/llvm/Object/`](../../include/llvm/Object/))
   — `ELFObjectFile<ELFT>` / `COFFObjectFile` / `MachOObjectFile` 等高层
   包装, 含 `symbols_begin()` / `sections()` / `getRelocations()` 等通用
   接口。`ObjectYAML` 不直接用它们 (它写出的是 raw 字节, 不是
   `ObjectFile`), 但产出**兼容它们读取**的二进制。

3. **YAML ↔ 二进制双向桥** — 工具 [`obj2yaml`](../../tools/obj2yaml/) 内
   部**先**调 [`lib/Object`](../Object/0-overview.md) 解析二进制,
   **再**用 [`lib/ObjectYAML`](./) 的 `MappingTraits` 写出 YAML。
   [`llvm-yaml2obj`](../../tools/yaml2obj/) 反过来, YAML → 二进制。
   这一对工具是验证两个库字节兼容性的官方回归测试。

#### 典型协作场景

- **测试 / fuzz 闭环**: LLVM 开发者手写 YAML (含人工构造的 ELF header +
  .debug_info) → [`llvm-yaml2obj`](../../tools/yaml2obj/) → 二进制 .o →
  [`llvm-objdump`](../../tools/llvm-objdump/) /
  [`llvm-dwarfdump`](../../tools/llvm-dwarfdump/) ([`lib/Object`](../Object/0-overview.md)
  解析) → 验证输出。
- **真实编译器输出往返**:
  `clang -c foo.c -o foo.o` →
  [`obj2yaml`](../../tools/obj2yaml/) `foo.o > foo.yaml`
  (经 [`lib/Object`](../Object/0-overview.md) 解析 →
  [`lib/ObjectYAML`](./) 写出)。
- **CodeView 调试信息**:
  [`COFFEmitter.cpp`](COFFEmitter.cpp) 写 `.debug$S/.debug$T/.debug$H`
  (经 [`CodeViewYAMLSymbols.cpp`](CodeViewYAMLSymbols.cpp) +
  [`CodeViewYAMLTypes.cpp`](CodeViewYAMLTypes.cpp) +
  [`CodeViewYAMLDebugSections.cpp`](CodeViewYAMLDebugSections.cpp) +
  [`CodeViewYAMLTypeHashing.cpp`](CodeViewYAMLTypeHashing.cpp) YAML
  → CV 记录); 写出的字节随后被
  [`lib/DebugInfo/CodeView/`](../../include/llvm/DebugInfo/CodeView/)
  + [`lib/Object/COFFObjectFile.cpp`](../Object/COFFObjectFile.cpp)
  读取 — 这就是 [`llvm-pdbutil`](../../tools/llvm-pdbutil/) 能解析
  PDB 的根因。

#### 设计哲学一句话

`lib/ObjectYAML` 和 [`lib/Object`](../Object/0-overview.md) 共享同一个
[`llvm/BinaryFormat/`](../../include/llvm/BinaryFormat/) 头文件库 — 这是
LLVM 保证"yaml2obj 出的文件 obj2dump 能读"的唯一办法。任何对 packed
struct 的修改都必须**同时**更新两边。

---

## §1. 编译流水线概览

```
YAML 文本 (stdin 或文件)
   │
   ▼
┌────────────────────────────────────────────────────────┐
│ yaml2obj.cpp::convertYAML                              │
│   · 解析 YAML root                                      │
│   · ObjectYAML.cpp::MappingTraits<YamlObjectFile>      │
│   · 按顶层 !XXX tag 分发:                              │
│       !ELF       → ELFYAML.cpp (Object struct)          │
│       !COFF      → COFFYAML.cpp                         │
│       !mach-o    → MachOYAML.cpp                        │
│       !fat-mach-o→ MachOUniversalYAML (in MachO)       │
│       !WASM      → WasmYAML.cpp                         │
│       !XCOFF     → XCOFFYAML.cpp                        │
│       !GOFF      → GOFFYAML.cpp                         │
│       !dxcontainer → DXContainerYAML.cpp                │
│       !minidump  → MinidumpYAML.cpp                     │
│       !Arch      → ArchiveYAML.cpp                      │
│       !Offload   → OffloadYAML.cpp                      │
└────────────────────────────────────────────────────────┘
   │
   ▼
┌────────────────────────────────────────────────────────┐
│ yaml2xxx (per format, declared in yaml2obj.h)           │
│   · 调对应 XxxEmitter::writeXxx                          │
│   · 嵌入 DWARF 段时: DWARFEmitter::emitDebugSections    │
│   · 嵌入 BBAddrMap 段时: BBAddrMapYAML::encodePayload   │
│   · 嵌入 CodeView 时: CodeViewYAML::toDebugT/S/H        │
└────────────────────────────────────────────────────────┘
   │
   ▼
┌────────────────────────────────────────────────────────┐
│ 写出二进制 (二进制 .o / .so / .elf / ...)              │
│   · 通用 ContiguousBlobAccumulator 写入 section data    │
│   · 各 emitter 负责 header / section header / 符号表    │
└────────────────────────────────────────────────────────┘
```

辅助入口:

- **C API**: 无 (YAML 转换纯内部)
- **共用工具**: `ContiguousBlobAccumulator` (共享 blob 写入, 带 MaxSize
  保护)
- **共用 BinaryRef**: `BinaryRef` (hex 字符串 ↔ 二进制) 在所有
  `Section.SectionData` 字段用

---

## §2. 文件目录结构

```
llvm/lib/ObjectYAML/   (FLAT — 31 .cpp 全顶层, 无子目录)
├── 4 个 base / shared utilities
├── 11 对格式 (XxxYAML.cpp + XxxEmitter.cpp, 每对 2 个)
├── BBAddrMap 单独 1 个 (无 emitter 对, encodePayload 被 ELF 用)
├── DWARF 单独 1 对
└── CodeView 单独 4 个 (无 emitter 对, 调 COFFEmitter 写)
```

### 2.1 文件按职责分类 (14 类, 31 文件)

| # | 分类 | 文件数 | 关键标识符 |
|---|------|--------|-----------|
| 1 | Base / shared utilities | 4 | `YamlObjectFile`, `BinaryRef`, `convertYAML`, `ContiguousBlobAccumulator` |
| 2 | ELF | 2 | `ELFYAML::Object`, `ELFState<ELFT>`, `yaml2elf` |
| 3 | COFF | 2 | `COFFYAML::Object`, `COFFParser`, `yaml2coff` |
| 4 | Mach-O | 2 | `MachOYAML::Object`, `MachOWriter`, `UniversalWriter` |
| 5 | Wasm | 2 | `WasmYAML::Object`, `WasmWriter`, `yaml2wasm` |
| 6 | XCOFF | 2 | `XCOFFYAML::Object`, `XCOFFWriter`, `yaml2xcoff` |
| 7 | GOFF | 2 | `GOFFYAML::Object`, `GOFFState`, `yaml2goff` |
| 8 | DXContainer | 2 | `DXContainerYAML::Object`, `DXContainerWriter` |
| 9 | Minidump | 2 | `MinidumpYAML::Object`, `BlobAllocator`, `yaml2minidump` |
| 10 | Archive | 2 | `ArchYAML::Archive`, `yaml2archive` |
| 11 | Offload | 2 | `OffloadYAML::Binary`, `yaml2offload` |
| 12 | BBAddrMap (YAML only) | 1 | `BBAddrMapYAML::encodePayload` (共用) |
| 13 | DWARF | 2 | `DWARFYAML::Data`, `emitDebug*` 函数族 |
| 14 | CodeView (YAML only) | 4 | `SymbolRecord`, `LeafRecord`, `YAMLDebugSubsection`, `DebugHSection` |

---

## §3. 文件详解

### 3.1 Base / shared utilities

[`ObjectYAML.cpp`](ObjectYAML.cpp) — `YamlObjectFile` 顶层分发器: 根据
顶层 `!ELF` / `!COFF` / `!WASM` / `!mach-o` / `!fat-mach-o` / `!minidump`
/ `!Offload` / `!XCOFF` / `!GOFF` / `!dxcontainer` / `!Arch` tag 实例化
对应 YAML 结构。
- 上游: [`yaml2obj.cpp`](yaml2obj.cpp) 的 `convertYAML`;
  `obj2yaml` tool 链路;
  [`unittests/ObjectYAML/*Test.cpp`](../../unittests/ObjectYAML/)。
- 下游: 几乎所有 `<Format>YAML.h`
  (`ArchiveYAML.h` / `BBAddrMapYAML.h` / `COFFYAML.h` / `GOFFYAML.h` /
  `MachOYAML.h` / `MinidumpYAML.h` / `OffloadYAML.h` / `WasmYAML.h` /
  `XCOFFYAML.h` / `DXContainerYAML.h` + ELF);
  [`llvm/Support/YAMLTraits.h`](../../include/llvm/Support/YAMLTraits.h),
  [`YAMLParser.h`](../../include/llvm/Support/YAMLParser.h),
  [`Twine.h`](../../include/llvm/ADT/Twine.h)。
- 关键类/函数: `YamlObjectFile`,
  `MappingTraits<YamlObjectFile>::mapping`, `IO::mapTag` (各 tag)。

[`YAML.cpp`](YAML.cpp) — `BinaryRef` YAMLIO 特化: 十六进制字符串 ↔ 二进制
blob 互转。
- 上游: 几乎所有 ObjectYAML emitter 和各 `<Format>YAML.cpp`
  (`SectionData`、`Section.SectionData` 等字段都是 `BinaryRef`)。
- 下游: [`llvm/ObjectYAML/YAML.h`](../../include/llvm/ObjectYAML/YAML.h),
  [`llvm/ADT/StringExtras.h`](../../include/llvm/ADT/StringExtras.h),
  [`llvm/Support/raw_ostream.h`](../../include/llvm/Support/raw_ostream.h)。
- 关键类/函数: `ScalarTraits<BinaryRef>::output/input`,
  `BinaryRef::writeAsBinary`, `BinaryRef::writeAsHex`。

[`yaml2obj.cpp`](yaml2obj.cpp) — 顶层 YAML→二进制 dispatch; 调对应
`yaml2elf/yaml2coff/...` 写出文件, 也提供测试用 `yaml2ObjectFile`。
- 上游:
  [`tools/yaml2obj/yaml2obj.cpp`](../../tools/yaml2obj/yaml2obj.cpp),
  [`unittests/ObjectYAML/YAML2ObjTest.cpp`](../../unittests/ObjectYAML/YAML2ObjTest.cpp),
  任何把 ObjectYAML 当库的客户端 (`llvm-ifs` / `llvm-profdata` /
  `llvm-pdbutil`)。
- 下游:
  [`yaml2obj.h`](../../include/llvm/ObjectYAML/yaml2obj.h)
  (声明所有 `yaml2xxx` 函数),
  [`ObjectYAML.h`](../../include/llvm/ObjectYAML/ObjectYAML.h),
  [`llvm/Object/ObjectFile.h`](../../include/llvm/Object/ObjectFile.h),
  [`llvm/Support/WithColor.h`](../../include/llvm/Support/WithColor.h)。
- 关键类/函数: `convertYAML`, `yaml2ObjectFile`,
  `ErrorHandler` (= `function_ref<void(const Twine&)>`)。

[`ContiguousBlobAccumulator.cpp`](ContiguousBlobAccumulator.cpp) — 共享的
"带 MaxSize 上限的二进制 blob 累加器", 所有 Emitter 用它来安全写段。
- 上游: 各 `XxxEmitter.cpp` 在写 section data 时; ELFEmitter /
  COFFEmitter / XCOFFEmitter / MachOEmitter / DWARFEmitter 等。
- 下游:
  [`llvm/ObjectYAML/YAML.h`](../../include/llvm/ObjectYAML/YAML.h)
  (`BinaryRef`), [`llvm/Support/Errc.h`](../../include/llvm/Support/Errc.h),
  [`llvm/Support/LEB128.h`](../../include/llvm/Support/LEB128.h)。
- 关键类/函数: `ContiguousBlobAccumulator::checkLimit`,
  `padToAlignment`, `writeAsBinary`, `writeULEB128`, `writeSLEB128`,
  `updateDataAt`, `takeLimitError`, `writeBlobToStream`。

### 3.2 ELF

[`ELFYAML.cpp`](ELFYAML.cpp) — ELF 对象 / 段 / 符号 / 重定位 / notes /
dynamic / MIPS 等全部 YAML → 结构映射 (最大文件之一, 1948 行)。
- 上游:
  [`tools/obj2yaml/elf2yaml.cpp`](../../tools/obj2yaml/elf2yaml.cpp),
  [`ELFEmitter.cpp`](ELFEmitter.cpp) 的 `ELFState`,
  [`unittests/ObjectYAML/ELFYAMLTest.cpp`](../../unittests/ObjectYAML/ELFYAMLTest.cpp)。
- 下游:
  [`llvm/BinaryFormat/ELF.h`](../../include/llvm/BinaryFormat/ELF.h),
  [`llvm/Object/ELFTypes.h`](../../include/llvm/Object/ELFTypes.h),
  [`llvm/ObjectYAML/BBAddrMapYAML.h`](../../include/llvm/ObjectYAML/BBAddrMapYAML.h),
  [`llvm/ObjectYAML/DWARFYAML.h`](../../include/llvm/ObjectYAML/DWARFYAML.h),
  [`llvm/ObjectYAML/YAML.h`](../../include/llvm/ObjectYAML/YAML.h),
  [`llvm/Support/ARMEHABI.h`](../../include/llvm/Support/ARMEHABI.h),
  [`llvm/Support/MipsABIFlags.h`](../../include/llvm/Support/MipsABIFlags.h),
  [`llvm/ADT/MapVector.h`](../../include/llvm/ADT/MapVector.h)。
- 关键类/函数: `ELFYAML::Object / FileHeader / ProgramHeader / Symbol /
  Section / SectionOrType / Chunk`;
  `ScalarEnumerationTraits<ELFYAML::ELF_ET/ELF_PT/ELF_EM/ELF_ELFCLASS/ELF_SHT/ELF_RSS/ELF_NT/...>`,
  `ScalarBitSetTraits<ELF_EF/ELF_PF/ELF_SHF/MIPS_AFL_*>`,
  `MappingTraits<ELFYAML::Object>::mapping`, `NormalizedOther`,
  `dropUniqueSuffix`, `appendUniqueSuffix`, `getDefaultShEntSize`。

[`ELFEmitter.cpp`](ELFEmitter.cpp) — 把 `ELFYAML::Object` 拼成真正 ELF
二进制 (32/64 位 × LE/BE 四种 `ELFState<ELFT>`)。
- 上游: [`yaml2obj.cpp`](yaml2obj.cpp) 的 `convertYAML → yaml2elf`。
- 下游:
  [`llvm/BinaryFormat/ELF.h`](../../include/llvm/BinaryFormat/ELF.h),
  [`llvm/MC/StringTableBuilder.h`](../../include/llvm/MC/StringTableBuilder.h),
  [`llvm/Object/ELFTypes.h`](../../include/llvm/Object/ELFTypes.h),
  [`llvm/ObjectYAML/ContiguousBlobAccumulator.h`](../../include/llvm/ObjectYAML/ContiguousBlobAccumulator.h),
  [`llvm/ObjectYAML/DWARFEmitter.h`](../../include/llvm/ObjectYAML/DWARFEmitter.h),
  [`llvm/ObjectYAML/DWARFYAML.h`](../../include/llvm/ObjectYAML/DWARFYAML.h),
  [`llvm/ObjectYAML/ELFYAML.h`](../../include/llvm/ObjectYAML/ELFYAML.h),
  [`llvm/ObjectYAML/yaml2obj.h`](../../include/llvm/ObjectYAML/yaml2obj.h);
  链接时调 [`BBAddrMapYAML.cpp`](BBAddrMapYAML.cpp) 的
  `BBAddrMapYAML::encodePayload`。
- 关键类/函数: `ELFState<ELFT>` (`writeELF` / `initProgramHeaders` /
  `initSectionHeaders` / `initSymtabSectionHeader` /
  `initStrtabSectionHeader` / `initDWARFSectionHeader` / `writeELFHeader`
  / `toELFSymbols` / `finalizeStrings` / 多个 `writeSectionContent`
  重载 ~20 个); `NameToIdxMap`, `Fragment`, `writeELF`, `yaml2elf`,
  `shouldAllocateFileSpace`。

### 3.3 COFF

[`COFFYAML.cpp`](COFFYAML.cpp) — COFF 对象 / 段 / 符号 / 重定位 /
PE header / aux symbol / load config 的 YAML 映射 (735 行), 含一组枚举
/ 位集特化。
- 上游:
  [`tools/obj2yaml/coff2yaml.cpp`](../../tools/obj2yaml/coff2yaml.cpp),
  [`COFFEmitter.cpp`](COFFEmitter.cpp) 的 `COFFParser`,
  [`unittests/ObjectYAML/`](../../unittests/ObjectYAML/)。
- 下游:
  [`llvm/BinaryFormat/COFF.h`](../../include/llvm/BinaryFormat/COFF.h),
  [`llvm/Object/COFF.h`](../../include/llvm/Object/COFF.h),
  [`llvm/ObjectYAML/CodeViewYAMLDebugSections.h`](../../include/llvm/ObjectYAML/CodeViewYAMLDebugSections.h),
  [`CodeViewYAMLTypeHashing.h`](../../include/llvm/ObjectYAML/CodeViewYAMLTypeHashing.h),
  [`CodeViewYAMLTypes.h`](../../include/llvm/ObjectYAML/CodeViewYAMLTypes.h),
  [`YAML.h`](../../include/llvm/ObjectYAML/YAML.h)。
- 关键类/函数: `COFFYAML::Object / Section / Symbol / Relocation /
  SectionDataEntry / PEHeader`;
  `ScalarEnumerationTraits<COFF::MachineTypes / SymbolBaseType /
  SymbolStorageClass / ...>`,
  `ScalarBitSetTraits<COFF::Characteristics / SectionCharacteristics /
  DLLCharacteristics>`,
  `MappingTraits<COFFYAML::Object>::mapping`,
  `mapLoadConfigMember`,
  `MappingTraits<object::coff_load_configuration32/64>`。

[`COFFEmitter.cpp`](COFFEmitter.cpp) — COFF 对象 + PE 可选头 + Debug$S/
T/P/H 段写出; 封装 `COFFParser`。
- 上游: [`yaml2obj.cpp`](yaml2obj.cpp) 的 `convertYAML → yaml2coff`。
- 下游:
  [`llvm/DebugInfo/CodeView/StringsAndChecksums.h`](../../include/llvm/DebugInfo/CodeView/StringsAndChecksums.h),
  [`llvm/ObjectYAML/ContiguousBlobAccumulator.h`](../../include/llvm/ObjectYAML/ContiguousBlobAccumulator.h),
  [`llvm/ObjectYAML/CodeViewYAML*.h`](../../include/llvm/ObjectYAML/),
  [`ObjectYAML.h`](../../include/llvm/ObjectYAML/ObjectYAML.h),
  [`yaml2obj.h`](../../include/llvm/ObjectYAML/yaml2obj.h),
  [`llvm/Support/BinaryStreamWriter.h`](../../include/llvm/Support/BinaryStreamWriter.h),
  [`llvm/Support/Endian.h`](../../include/llvm/Support/Endian.h)。
- 关键类/函数: `COFFParser`, `writeSectionContent`,
  `initializeOptionalHeader`, `writeCOFF`, `writeLoadConfig`,
  `toDebugS`, `SectionDataEntry::writeAsBinary`, `yaml2coff`。

### 3.4 Mach-O

[`MachOYAML.cpp`](MachOYAML.cpp) — Mach-O 对象 / 通用段 / fat header /
load commands / NList / LinkEdit / UUID 的 YAML 映射 (648 行), 含 60+
load command 特化。
- 上游:
  [`tools/obj2yaml/macho2yaml.cpp`](../../tools/obj2yaml/macho2yaml.cpp),
  [`MachOEmitter.cpp`](MachOEmitter.cpp) 的 `MachOWriter / UniversalWriter`。
- 下游:
  [`llvm/BinaryFormat/MachO.h`](../../include/llvm/BinaryFormat/MachO.h)
  (含 `MachO.def`),
  [`llvm/ObjectYAML/DWARFYAML.h`](../../include/llvm/ObjectYAML/DWARFYAML.h),
  [`YAML.h`](../../include/llvm/ObjectYAML/YAML.h),
  [`llvm/TargetParser/Host.h`](../../include/llvm/TargetParser/Host.h),
  [`llvm/Support/raw_ostream.h`](../../include/llvm/Support/raw_ostream.h)。
- 关键类/函数: `MachOYAML::Object / FileHeader / LoadCommand / Section /
  LinkEditData / NListEntry / RebaseOpcode / BindOpcode / ExportEntry /
  DataInCodeEntry`;
  `UniversalBinary / FatHeader / FatArch`;
  `ScalarTraits<char_16>::output/input`,
  `ScalarTraits<uuid_t>::output/input`,
  `MappingTraits<MachO::segment_command / dylib_command / ...>`,
  `mapLoadCommandData`。

[`MachOEmitter.cpp`](MachOEmitter.cpp) — 写出 Mach-O 单文件 + Universal
(fat) 二进制; DWARF 段直接委托给 `DWARFEmitter`。
- 上游: [`yaml2obj.cpp`](yaml2obj.cpp) 的 `convertYAML → yaml2macho`。
- 下游:
  [`llvm/BinaryFormat/MachO.h`](../../include/llvm/BinaryFormat/MachO.h),
  [`llvm/ObjectYAML/DWARFEmitter.h`](../../include/llvm/ObjectYAML/DWARFEmitter.h),
  [`ObjectYAML.h`](../../include/llvm/ObjectYAML/ObjectYAML.h),
  [`yaml2obj.h`](../../include/llvm/ObjectYAML/yaml2obj.h),
  [`llvm/Support/LEB128.h`](../../include/llvm/Support/LEB128.h),
  [`FormatVariadic.h`](../../include/llvm/Support/FormatVariadic.h)。
- 关键类/函数: `MachOWriter` (`writeMachO` / `writeHeader` /
  `writeLoadCommands` / `writeSectionData` / `writeRelocations` /
  `writeLinkEditData` / `writeBindOpcodes` / `writeExportTrie` /
  `writeChainedFixups` / ...);
  `UniversalWriter` (`writeFatHeader` / `writeFatArchs` /
  `ZeroToOffset`),
  `ZeroFillBytes`, `Fill`, `writeFatArch`, `yaml2macho`。

### 3.5 Wasm

[`WasmYAML.cpp`](WasmYAML.cpp) — Wasm object / import / export / code /
data / linking / name / dylink / producers / target-features / reloc 段
的 YAML 映射 (669 行)。
- 上游:
  [`tools/obj2yaml/wasm2yaml.cpp`](../../tools/obj2yaml/wasm2yaml.cpp),
  [`WasmEmitter.cpp`](WasmEmitter.cpp) 的 `WasmWriter`。
- 下游:
  [`llvm/BinaryFormat/Wasm.h`](../../include/llvm/BinaryFormat/Wasm.h),
  [`llvm/ObjectYAML/YAML.h`](../../include/llvm/ObjectYAML/YAML.h),
  [`llvm/Support/Casting.h`](../../include/llvm/Support/Casting.h)。
- 关键类/函数: `WasmYAML::Object / FileHeader / Section(SectionType)`;
  各 `WasmYAML::*Section` 子类 (`DylinkSection / NameSection /
  LinkingSection / ProducersSection / TargetFeaturesSection /
  CustomSection / TypeSection / ImportSection / FunctionSection /
  TableSection / MemorySection / TagSection / GlobalSection /
  ExportSection / StartSection / ElemSection / CodeSection /
  DataSection / DataCountSection`);
  `MappingTraits<WasmYAML::Signature/Table/Function/Relocation/NameEntry/ProducerEntry/FeatureEntry/SegmentInfo/LocalDecl/Limits/ElemSegment/Import>`。

[`WasmEmitter.cpp`](WasmEmitter.cpp) — 把 `WasmYAML::Object` 按 wasm 二进制
布局写出 (含 init expr / reloc / subsection)。
- 上游: [`yaml2obj.cpp`](yaml2obj.cpp) 的 `convertYAML → yaml2wasm`。
- 下游:
  [`llvm/BinaryFormat/Wasm.h`](../../include/llvm/BinaryFormat/Wasm.h),
  [`llvm/ObjectYAML/ObjectYAML.h`](../../include/llvm/ObjectYAML/ObjectYAML.h),
  [`WasmYAML.h`](../../include/llvm/ObjectYAML/WasmYAML.h),
  [`yaml2obj.h`](../../include/llvm/ObjectYAML/yaml2obj.h),
  [`llvm/Support/EndianStream.h`](../../include/llvm/Support/EndianStream.h)。
- 关键类/函数: `WasmWriter` (`writeWasm` / `writeInitExpr` / 多个
  `writeSectionContent` 重载); `SubSectionWriter`; `writeUint64/32/8`,
  `writeStringRef`, `writeLimits`, `writeRelocSection`, `yaml2wasm`。

### 3.6 XCOFF

[`XCOFFYAML.cpp`](XCOFFYAML.cpp) — XCOFF 对象 / 辅助头 / 段 / 符号 / 重
定位 / Aux symbol (csect / function / file / exception / block / dwarf /
stat) 的 YAML 映射 (405 行)。
- 上游:
  [`tools/obj2yaml/xcoff2yaml.cpp`](../../tools/obj2yaml/xcoff2yaml.cpp),
  [`XCOFFEmitter.cpp`](XCOFFEmitter.cpp) 的 `XCOFFWriter`。
- 下游:
  [`llvm/BinaryFormat/XCOFF.h`](../../include/llvm/BinaryFormat/XCOFF.h),
  [`llvm/ObjectYAML/YAML.h`](../../include/llvm/ObjectYAML/YAML.h)。
- 关键类/函数: `XCOFFYAML::FileHeader / AuxiliaryHeader / Section /
  Relocation / Symbol / Object / StringTable`; `AuxSymbolEnt` (及其
  `CsectAuxEnt / FileAuxEnt / FunctionAuxEnt / ExcpetionAuxEnt /
  BlockAuxEnt / SectAuxEntForDWARF / SectAuxEntForStat` 派生类);
  `ScalarBitSetTraits<XCOFF::SectionTypeFlags>`,
  `ScalarEnumerationTraits<XCOFF::StorageClass / StorageMappingClass /
  SymbolType / DwarfSectionSubtypeFlags / CFileStringType>`,
  `MappingTraits<std::unique_ptr<XCOFFYAML::AuxSymbolEnt>>::mapping`,
  `MappingTraits<XCOFFYAML::Object>::mapping`。

[`XCOFFEmitter.cpp`](XCOFFEmitter.cpp) — 写出 32/64 位 big-endian XCOFF
(file header + aux header + section headers + symbols + relocations +
string table)。
- 上游: [`yaml2obj.cpp`](yaml2obj.cpp) 的 `convertYAML → yaml2xcoff`。
- 下游:
  [`llvm/BinaryFormat/XCOFF.h`](../../include/llvm/BinaryFormat/XCOFF.h),
  [`llvm/MC/StringTableBuilder.h`](../../include/llvm/MC/StringTableBuilder.h),
  [`llvm/Object/XCOFFObjectFile.h`](../../include/llvm/Object/XCOFFObjectFile.h),
  [`llvm/ObjectYAML/ObjectYAML.h`](../../include/llvm/ObjectYAML/ObjectYAML.h),
  [`yaml2obj.h`](../../include/llvm/ObjectYAML/yaml2obj.h),
  [`llvm/Support/EndianStream.h`](../../include/llvm/Support/EndianStream.h),
  [`MemoryBuffer.h`](../../include/llvm/Support/MemoryBuffer.h)。
- 关键类/函数: `XCOFFWriter` (`writeXCOFF` / `initFileHeader` /
  `initSectionHeaders` / `initRelocations` / `initStringTable` /
  `assignAddressesAndIndices` / `writeFileHeader` /
  `writeSectionHeaders` / `writeSectionData` / `writeRelocations` /
  `writeSymbols` / `writeStringTable`);
  多个 `writeAuxSymbol` 重载、`nameShouldBeInStringTable`, `yaml2xcoff`。

### 3.7 GOFF

[`GOFFYAML.cpp`](GOFFYAML.cpp) — GOFF 对象仅含 `FileHeader` 的 YAML 映射
(44 行, 最小文件)。
- 上游:
  [`tools/obj2yaml/goff2yaml.cpp`](../../tools/obj2yaml/goff2yaml.cpp),
  [`GOFFEmitter.cpp`](GOFFEmitter.cpp) 的 `GOFFState`。
- 下游:
  [`llvm/ObjectYAML/GOFFYAML.h`](../../include/llvm/ObjectYAML/GOFFYAML.h)
  (引入
  [`llvm/BinaryFormat/GOFF.h`](../../include/llvm/BinaryFormat/GOFF.h))。
- 关键类/函数: `GOFFYAML::Object::Object()`,
  `MappingTraits<GOFFYAML::FileHeader>::mapping`,
  `MappingTraits<GOFFYAML::Object>::mapping`。

[`GOFFEmitter.cpp`](GOFFEmitter.cpp) — 写出 GOFF 物理记录 (header + end
record), 处理 EBCDIC 转换与 fixed-size 物理块。
- 上游: [`yaml2obj.cpp`](yaml2obj.cpp) 的 `convertYAML → yaml2goff`。
- 下游:
  [`llvm/ObjectYAML/ObjectYAML.h`](../../include/llvm/ObjectYAML/ObjectYAML.h),
  [`yaml2obj.h`](../../include/llvm/ObjectYAML/yaml2obj.h),
  [`llvm/Support/ConvertEBCDIC.h`](../../include/llvm/Support/ConvertEBCDIC.h),
  [`Endian.h`](../../include/llvm/Support/Endian.h),
  [`raw_ostream.h`](../../include/llvm/Support/raw_ostream.h)。
- 关键类/函数: `GOFFOstream` (`raw_ostream` 子类, 分块写物理记录),
  `GOFFState` (`writeHeader` / `writeEnd` / `writeObject` / `writeGOFF`),
  `binaryBe`, `zeros`, `Rec_Continued`, `Rec_Continuation`, `yaml2goff`。

### 3.8 DXContainer

[`DXContainerYAML.cpp`](DXContainerYAML.cpp) — DirectX container 对象
/ parts (DXIL program / shader hash / PSV / signature / root signature
/ resource bind info / source info) 的 YAML 映射 (1186 行), 定义大量
dxbc/dxil 枚举/位集特化。
- 上游:
  [`tools/obj2yaml/dxcontainer2yaml.cpp`](../../tools/obj2yaml/dxcontainer2yaml.cpp),
  [`DXContainerEmitter.cpp`](DXContainerEmitter.cpp) 的
  `DXContainerWriter`、测试
  ([`DXContainerYAMLTest.cpp`](../../unittests/ObjectYAML/DXContainerYAMLTest.cpp))。
- 下游:
  [`llvm/BinaryFormat/DXContainer.h`](../../include/llvm/BinaryFormat/DXContainer.h)
  (含 `DXContainerConstants.def`),
  [`llvm/Object/DXContainer.h`](../../include/llvm/Object/DXContainer.h),
  [`llvm/ObjectYAML/YAML.h`](../../include/llvm/ObjectYAML/YAML.h),
  [`llvm/Support/YAMLTraits.h`](../../include/llvm/Support/YAMLTraits.h)。
- 关键类/函数: `DXContainerYAML::Object / FileHeader / Part /
  DXILProgram / ShaderFeatureFlags / ShaderHash / PSVInfo / Signature /
  RootSignatureYamlDesc / RootConstantsYaml / RootDescriptorYaml /
  DescriptorRangeYaml / DescriptorTableYaml / StaticSamplerYamlDesc /
  DebugName / StringTableEntry / SignatureElement / ResourceBindInfo`;
  `ScalarEnumerationTraits<dxbc::PSV::SemanticKind /
  dxil::ResourceClass / ...>`,
  `MappingTraits<DXContainerYAML::Object>::mapping`,
  `decodeShaderFeatureFlags`。

[`DXContainerEmitter.cpp`](DXContainerEmitter.cpp) — 写出 DXContainer 二
进制 (header + 各 part, PSV/root signature 委托给 MC 库)。
- 上游: [`yaml2obj.cpp`](yaml2obj.cpp) 的 `convertYAML → yaml2dxcontainer`。
- 下游:
  [`llvm/BinaryFormat/DXContainer.h`](../../include/llvm/BinaryFormat/DXContainer.h),
  [`llvm/MC/DXContainerInfo.h`](../../include/llvm/MC/DXContainerInfo.h),
  [`DXContainerPSVInfo.h`](../../include/llvm/MC/DXContainerPSVInfo.h),
  [`DXContainerRootSignature.h`](../../include/llvm/MC/DXContainerRootSignature.h),
  [`llvm/ObjectYAML/DXContainerYAML.h`](../../include/llvm/ObjectYAML/DXContainerYAML.h),
  [`yaml2obj.h`](../../include/llvm/ObjectYAML/yaml2obj.h),
  [`llvm/Support/Errc.h`](../../include/llvm/Support/Errc.h)。
- 关键类/函数: `DXContainerWriter` (`write` / `validateSize` /
  `validatePartOffsets` / `computePartOffsets` / `writeHeader` /
  `writeParts`), `assign_if`, `yaml2dxcontainer`。

### 3.9 Minidump

[`MinidumpYAML.cpp`](MinidumpYAML.cpp) — Minidump 对象 / header / 各
stream 类型 (Memory / Thread / Module / SystemInfo / Exception /
Memory64List / ...) 的 YAML 映射 (608 行), 含 30+ stream mapping +
endian/hex 工具。
- 上游:
  [`tools/obj2yaml/minidump2yaml.cpp`](../../tools/obj2yaml/minidump2yaml.cpp),
  [`MinidumpEmitter.cpp`](MinidumpEmitter.cpp) 的 `BlobAllocator`、
  测试
  ([`MinidumpYAMLTest.cpp`](../../unittests/ObjectYAML/MinidumpYAMLTest.cpp))。
- 下游:
  [`llvm/BinaryFormat/Minidump.h`](../../include/llvm/BinaryFormat/Minidump.h),
  [`llvm/Object/Minidump.h`](../../include/llvm/Object/Minidump.h),
  [`llvm/ObjectYAML/YAML.h`](../../include/llvm/ObjectYAML/YAML.h),
  [`llvm/Support/YAMLTraits.h`](../../include/llvm/Support/YAMLTraits.h)。
- 关键类/函数: `MinidumpYAML::Object / Stream (StreamKind) /
  RawContentStream / MemoryListStream / MemoryInfoListStream /
  Memory64ListStream / ThreadListStream / ModuleListStream /
  SystemInfoStream / TextContentStream / ExceptionStream / CPUInfo
  (ArmInfo/X86Info/OtherInfo) / VSFixedFileInfo / MemoryDescriptor /
  MemoryInfo`;
  `MappingTraits<Object>::mapping`,
  `MappingTraits<std::unique_ptr<Stream>>::mapping`,
  `streamMapping/streamValidate` 一组,
  `mapRequiredAs/mapOptionalAs/mapRequiredHex/mapOptionalHex`,
  `ScalarBitSetTraits<MemoryProtection/MemoryState/MemoryType>`,
  `ScalarEnumerationTraits<ProcessorArchitecture/OSPlatform/StreamType>`,
  `MappingContextTraits<MemoryDescriptor/MemoryDescriptor_64,
  BinaryRef>::mapping`。

[`MinidumpEmitter.cpp`](MinidumpEmitter.cpp) — 把 Minidump YAML 写到文件,
按 directory + RVA 布局分配各 stream。
- 上游: [`yaml2obj.cpp`](yaml2obj.cpp) 的 `convertYAML → yaml2minidump`。
- 下游:
  [`llvm/BinaryFormat/Minidump.h`](../../include/llvm/BinaryFormat/Minidump.h),
  [`llvm/ObjectYAML/MinidumpYAML.h`](../../include/llvm/ObjectYAML/MinidumpYAML.h),
  [`ObjectYAML.h`](../../include/llvm/ObjectYAML/ObjectYAML.h),
  [`yaml2obj.h`](../../include/llvm/ObjectYAML/yaml2obj.h)。
- 关键类/函数: `BlobAllocator` (`writeTo` / `allocateObject` /
  `allocateArray`), `layout()` 重载 (含 `LocationDescriptor` /
  `ExceptionStream` / `Memory64ListStream` /
  `MemoryListStream::entry_type` / `ModuleListStream::entry_type` /
  `ThreadListStream::entry_type`),
  `Directory`, `yaml2minidump`。

### 3.10 Archive

[`ArchiveYAML.cpp`](ArchiveYAML.cpp) — `!Arch` 标签的 archive YAML 映射
(58 行), 含 `Child` + `Field` + magic + 内容。
- 上游:
  [`tools/obj2yaml/archive2yaml.cpp`](../../tools/obj2yaml/archive2yaml.cpp),
  [`ArchiveEmitter.cpp`](ArchiveEmitter.cpp) 的 `yaml2archive`。
- 下游:
  [`llvm/ObjectYAML/ArchiveYAML.h`](../../include/llvm/ObjectYAML/ArchiveYAML.h)
  (仅引入 [`YAML.h`](../../include/llvm/ObjectYAML/YAML.h))。
- 关键类/函数:
  `MappingTraits<ArchYAML::Archive>::mapping`,
  `MappingTraits<ArchYAML::Archive::Child>::mapping`,
  `MappingTraits<ArchYAML::Archive>::validate`,
  `MappingTraits<ArchYAML::Archive::Child>::validate`。

[`ArchiveEmitter.cpp`](ArchiveEmitter.cpp) — 直接写出 archive 字节流
(magic + 每个 child 的字段 + 内容 + 可选 padding) (50 行, 最小 emitter)。
- 上游: [`yaml2obj.cpp`](yaml2obj.cpp) 的 `convertYAML → yaml2archive`。
- 下游:
  [`llvm/ObjectYAML/ArchiveYAML.h`](../../include/llvm/ObjectYAML/ArchiveYAML.h),
  [`yaml2obj.h`](../../include/llvm/ObjectYAML/yaml2obj.h),
  [`llvm/Support/raw_ostream.h`](../../include/llvm/Support/raw_ostream.h)。
- 关键类/函数: `yaml2archive`, `WriteField` (内部 lambda)。

### 3.11 Offload

[`OffloadYAML.cpp`](OffloadYAML.cpp) — `!Offload` 标签的 offload binary
YAML 映射 (79 行): `Binary` / `Member` / `StringEntry` + `ImageKind` /
`OffloadKind` 枚举。
- 上游:
  [`tools/obj2yaml/offload2yaml.cpp`](../../tools/obj2yaml/offload2yaml.cpp),
  [`OffloadEmitter.cpp`](OffloadEmitter.cpp) 的 `yaml2offload`、测试
  ([`OffloadingBundleTest.cpp`](../../unittests/Object/OffloadingBundleTest.cpp))。
- 下游:
  [`llvm/ObjectYAML/OffloadYAML.h`](../../include/llvm/ObjectYAML/OffloadYAML.h)
  (引入
  [`llvm/Object/OffloadBinary.h`](../../include/llvm/Object/OffloadBinary.h),
  [`YAML.h`](../../include/llvm/ObjectYAML/YAML.h))。
- 关键类/函数:
  `MappingTraits<OffloadYAML::Binary>::mapping`,
  `MappingTraits<OffloadYAML::Binary::Member>::mapping`,
  `MappingTraits<OffloadYAML::Binary::StringEntry>::mapping`,
  `ScalarEnumerationTraits<object::ImageKind>::enumeration`,
  `ScalarEnumerationTraits<object::OffloadKind>::enumeration`。

[`OffloadEmitter.cpp`](OffloadEmitter.cpp) — 写出 offload binary; 通过
`object::OffloadBinary::write` 生成 base buffer, 再 patch header 字段
(62 行, 最小 emitter)。
- 上游: [`yaml2obj.cpp`](yaml2obj.cpp) 的 `convertYAML → yaml2offload`。
- 下游:
  [`llvm/Object/OffloadBinary.h`](../../include/llvm/Object/OffloadBinary.h),
  [`llvm/ObjectYAML/OffloadYAML.h`](../../include/llvm/ObjectYAML/OffloadYAML.h),
  [`yaml2obj.h`](../../include/llvm/ObjectYAML/yaml2obj.h),
  [`llvm/Support/raw_ostream.h`](../../include/llvm/Support/raw_ostream.h)。
- 关键类/函数: `yaml2offload`。

### 3.12 BBAddrMap (单文件, YAML + 共用 encode)

[`BBAddrMapYAML.cpp`](BBAddrMapYAML.cpp) — BB address map YAML 映射 +
**通用二进制编码** (4/8 字节地址 / 版本 / feature / PGO 频率 / BB hash)。
- 上游:
  [`ELFEmitter.cpp`](ELFEmitter.cpp) 调用
  `BBAddrMapYAML::encodePayload` (在
  `ELFState<ELFT>::writeSectionContent(...BBAddrMapSection...)` 内)。
- 下游:
  [`llvm/ObjectYAML/BBAddrMapYAML.h`](../../include/llvm/ObjectYAML/BBAddrMapYAML.h),
  [`llvm/Object/BBAddrMap.h`](../../include/llvm/Object/BBAddrMap.h),
  [`llvm/ObjectYAML/ContiguousBlobAccumulator.h`](../../include/llvm/ObjectYAML/ContiguousBlobAccumulator.h),
  [`llvm/Support/WithColor.h`](../../include/llvm/Support/WithColor.h)。
- 关键类/函数:
  `MappingTraits<BBAddrMapYAML::BBAddrMapEntry>::mapping`,
  `MappingTraits<BBAddrMapYAML::BBAddrMapEntry::BBRangeEntry>::mapping`,
  `MappingTraits<BBAddrMapYAML::BBAddrMapEntry::BBEntry>::mapping`,
  `MappingTraits<BBAddrMapYAML::PGOAnalysisMapEntry>::mapping`,
  `MappingTraits<BBAddrMapYAML::PGOAnalysisMapEntry::PGOBBEntry>::mapping`,
  `MappingTraits<BBAddrMapYAML::PGOAnalysisMapEntry::PGOBBEntry::SuccessorEntry>::mapping`,
  `BBAddrMapYAML::encodePayload`。
- **无独立 Emitter 文件**; 序列化函数 `encodePayload` 是格式无关的,
  直接被 ELF emitter 复用。

### 3.13 DWARF

[`DWARFYAML.cpp`](DWARFYAML.cpp) — DWARF in-memory 结构
(`.debug_info` / `.debug_abbrev` / `.debug_line` / `.debug_aranges` /
`.debug_ranges` / `.debug_str` / `.debug_str_offsets` /
`.debug_pubnames` / ... / `.debug_names` / `.debug_loclists` /
`.debug_rnglists`) 的 YAML 映射 (400 行)。
- 上游:
  [`tools/obj2yaml/dwarf2yaml.cpp`](../../tools/obj2yaml/dwarf2yaml.cpp),
  [`DWARFEmitter.cpp`](DWARFEmitter.cpp) 全部 `emitDebug*` 函数、
  [`unittests/DebugInfo/DWARF/*Test.cpp`](../../unittests/DebugInfo/DWARF/)、
  ELF / Mach-O / COFF emitter 通过 `ELFYAML.cpp` / `MachOYAML.cpp` /
  `COFFYAML.cpp` 间接用。
- 下游:
  [`llvm/BinaryFormat/Dwarf.h`](../../include/llvm/BinaryFormat/Dwarf.h),
  [`llvm/ObjectYAML/YAML.h`](../../include/llvm/ObjectYAML/YAML.h),
  [`llvm/Support/Errc.h`](../../include/llvm/Support/Errc.h)。
- 关键类/函数: `DWARFYAML::Data::isEmpty`,
  `Data::getNonEmptySectionNames`,
  `Data::getAbbrevTableContentByIndex`;
  大量 `MappingTraits<DWARFYAML::Data / AbbrevTable / Abbrev / IdxForm /
  DebugNameAbbreviation / DebugNameEntry / DebugNamesSection /
  AttributeAbbrev / ARangeDescriptor / ARange / RangeEntry / Ranges /
  PubEntry / PubSection / Unit / Entry / FormValue / File / LnctForm /
  LineTableOpcode / LineTable / SegAddrPair / AddrTableEntry /
  StringOffsetsTable / DWARFOperation / RnglistEntry / LoclistEntry /
  ListEntries / ListTable>`。

[`DWARFEmitter.cpp`](DWARFEmitter.cpp) — 把 `DWARFYAML::Data` 写成各
`.debug_*` 段; 提供测试友好的顶层 API `emitDebugSections(YAMLString,
...)` (1362 行, 最大文件之一)。
- 上游: [`ELFEmitter.cpp`](ELFEmitter.cpp) (经
  [`DWARFEmitter.h`](../../include/llvm/ObjectYAML/DWARFEmitter.h))、
  [`MachOEmitter.cpp`](MachOEmitter.cpp) (仅 DWARF 段)、
  [`unittests/ObjectYAML/DWARFYAMLTest.cpp`](../../unittests/ObjectYAML/DWARFYAMLTest.cpp)。
- 下游:
  [`llvm/BinaryFormat/Dwarf.h`](../../include/llvm/BinaryFormat/Dwarf.h),
  [`llvm/ObjectYAML/DWARFEmitter.h`](../../include/llvm/ObjectYAML/DWARFEmitter.h),
  [`DWARFYAML.h`](../../include/llvm/ObjectYAML/DWARFYAML.h),
  [`llvm/Support/Errc.h`](../../include/llvm/Support/Errc.h),
  [`LEB128.h`](../../include/llvm/Support/LEB128.h),
  [`MemoryBuffer.h`](../../include/llvm/Support/MemoryBuffer.h),
  [`SourceMgr.h`](../../include/llvm/Support/SourceMgr.h),
  [`SwapByteOrder.h`](../../include/llvm/Support/SwapByteOrder.h),
  [`YAMLTraits.h`](../../include/llvm/Support/YAMLTraits.h),
  [`raw_ostream.h`](../../include/llvm/Support/raw_ostream.h),
  [`llvm/TargetParser/Host.h`](../../include/llvm/TargetParser/Host.h)。
- 关键类/函数: `DWARFYAML::emitDebugStr`, `emitDebugAbbrev`,
  `emitDebugAranges`, `emitDebugRanges`, `emitDebugPubnames`,
  `emitDebugPubtypes`, `emitDebugGNUPubnames`, `emitDebugGNUPubtypes`,
  `emitDebugInfo`, `emitDebugLine`, `emitDebugAddr`,
  `emitDebugStrOffsets`, `emitDebugRnglists`, `emitDebugLoclists`,
  `emitDebugNames`, `getDWARFEmitterByName`, `emitDebugSections`;
  `writeInteger`, `ZeroFillBytes`, `writeInitialLength`,
  `writeDWARFOffset`, `writeDIE`, `writeFormValues`,
  `emitPubSection`, `writeV5EntryFormat/V5Entry`,
  `writeDWARFExpression`, `writeDWARFLists`, `writeListEntry`,
  `PoolOffsetsAndData`, `emitDebugNamesEntryPool`,
  `writeLineTableOpcode`。

### 3.14 CodeView (4 文件, YAML only, 被 COFF emitter 调用)

[`CodeViewYAMLSymbols.cpp`](CodeViewYAMLSymbols.cpp) — CodeView
`SymbolRecord` (所有 S_* 类型 + Alias) ↔ YAML (744 行); 提供
`toCodeViewSymbol` / `fromCodeViewSymbol`。
- 上游:
  [`tools/obj2yaml/coff2yaml.cpp`](../../tools/obj2yaml/coff2yaml.cpp),
  [`COFFEmitter.cpp`](COFFEmitter.cpp) (debug$S symbols subsection),
  间接 `llvm-pdbutil`、单测。
- 下游:
  [`llvm/DebugInfo/CodeView/CodeView.h`](../../include/llvm/DebugInfo/CodeView/CodeView.h),
  [`CodeViewError.h`](../../include/llvm/DebugInfo/CodeView/CodeViewError.h),
  [`EnumTables.h`](../../include/llvm/DebugInfo/CodeView/EnumTables.h),
  [`RecordSerialization.h`](../../include/llvm/DebugInfo/CodeView/RecordSerialization.h),
  [`SymbolDeserializer.h`](../../include/llvm/DebugInfo/CodeView/SymbolDeserializer.h),
  [`SymbolRecord.h`](../../include/llvm/DebugInfo/CodeView/SymbolRecord.h),
  [`SymbolSerializer.h`](../../include/llvm/DebugInfo/CodeView/SymbolSerializer.h),
  [`TypeIndex.h`](../../include/llvm/DebugInfo/CodeView/TypeIndex.h),
  [`llvm/ObjectYAML/YAML.h`](../../include/llvm/ObjectYAML/YAML.h);
  通过 [`CodeViewSymbols.def`](../../include/llvm/DebugInfo/CodeView/CodeViewSymbols.def)
  宏展开所有符号类型。
- 关键类/函数: `CodeViewYAML::SymbolRecord::toCodeViewSymbol`,
  `SymbolRecord::fromCodeViewSymbol`,
  `MappingTraits<CodeViewYAML::SymbolRecord>::mapping`,
  `mapSymbolRecordImpl`, `UnknownSymbolRecord`,
  `SymbolRecordBase`, `SymbolRecordImpl<...>` (用 `SYMBOL_RECORD` /
  `SYMBOL_RECORD_ALIAS` 宏表生成);
  多个 `ScalarEnumerationTraits<SymbolKind/CPUType/RegisterId/SourceLanguage/...>`
  和 `ScalarBitSetTraits<ProcSymFlags/CompileSym2Flags/...>`。

[`CodeViewYAMLDebugSections.cpp`](CodeViewYAMLDebugSections.cpp) —
`.debug$S` 各 subsection (Lines / InlineeLines / Checksums /
CrossModuleExports / Imports / FrameData / CoffSymbolRVAs / Symbols /
StringTable) ↔ YAML (953 行, 最大 CV 文件); 提供
`toCodeViewSubsectionList`、`fromDebugS`、
`initializeStringsAndChecksums`。
- 上游: [`COFFEmitter.cpp`](COFFEmitter.cpp) 的 `toDebugS`,
  [`tools/obj2yaml/coff2yaml.cpp`](../../tools/obj2yaml/coff2yaml.cpp)。
- 下游:
  [`llvm/BinaryFormat/COFF.h`](../../include/llvm/BinaryFormat/COFF.h),
  [`llvm/DebugInfo/CodeView/CodeView.h`](../../include/llvm/DebugInfo/CodeView/CodeView.h)
  + 所有 `Debug*Subsection.h`
  (`DebugChecksumsSubsection.h` / `DebugLinesSubsection.h` /
  `DebugInlineeLinesSubsection.h` / `DebugCrossExSubsection.h` /
  `DebugCrossImpSubsection.h` / `DebugFrameDataSubsection.h` /
  `DebugStringTableSubsection.h` / `DebugSymbolsSubsection.h` /
  `DebugSubsectionRecord.h`),
  [`StringsAndChecksums.h`](../../include/llvm/DebugInfo/CodeView/StringsAndChecksums.h),
  [`DebugSubsectionVisitor.h`](../../include/llvm/DebugInfo/CodeView/DebugSubsectionVisitor.h)。
- 关键类/函数: `YAMLDebugSubsection::fromCodeViewSubection`,
  `toCodeViewSubsectionList`, `fromDebugS`,
  `initializeStringsAndChecksums`;
  `MappingTraits<YAMLDebugSubsection>::mapping`; 各
  `YAMLChecksumsSubsection / YAMLLinesSubsection /
  YAMLInlineeLinesSubsection / YAMLCrossModuleExportsSubsection /
  YAMLCrossModuleImportsSubsection / YAMLSymbolsSubsection /
  YAMLStringTableSubsection / YAMLFrameDataSubsection /
  YAMLCoffSymbolRVASubsection::map`; `SubsectionConversionVisitor`
  (多个 `visit*` 方法); `YAMLFrameData`, `SourceLineEntry`,
  `SourceColumnEntry`, `SourceLineBlock`,
  `SourceFileChecksumEntry`, `YAMLCrossModuleImport`, `InlineeSite`。

[`CodeViewYAMLTypes.cpp`](CodeViewYAMLTypes.cpp) — CodeView `LeafRecord`
(所有 LF_* 类型 + Alias) + `MemberRecord` ↔ YAML (866 行); 提供
`fromDebugT`, `toDebugT`。
- 上游: [`COFFEmitter.cpp`](COFFEmitter.cpp) (写 `.debug$T` /
  `.debug$P`, 调 `CodeViewYAML::toDebugT`),
  [`tools/obj2yaml/coff2yaml.cpp`](../../tools/obj2yaml/coff2yaml.cpp)。
- 下游:
  [`llvm/BinaryFormat/COFF.h`](../../include/llvm/BinaryFormat/COFF.h),
  [`llvm/DebugInfo/CodeView/AppendingTypeTableBuilder.h`](../../include/llvm/DebugInfo/CodeView/AppendingTypeTableBuilder.h),
  [`CVTypeVisitor.h`](../../include/llvm/DebugInfo/CodeView/CVTypeVisitor.h),
  [`CodeView.h`](../../include/llvm/DebugInfo/CodeView/CodeView.h),
  [`ContinuationRecordBuilder.h`](../../include/llvm/DebugInfo/CodeView/ContinuationRecordBuilder.h),
  [`TypeDeserializer.h`](../../include/llvm/DebugInfo/CodeView/TypeDeserializer.h),
  [`TypeIndex.h`](../../include/llvm/DebugInfo/CodeView/TypeIndex.h),
  [`TypeRecord.h`](../../include/llvm/DebugInfo/CodeView/TypeRecord.h),
  [`TypeVisitorCallbacks.h`](../../include/llvm/DebugInfo/CodeView/TypeVisitorCallbacks.h);
  通过 [`CodeViewTypes.def`](../../include/llvm/DebugInfo/CodeView/CodeViewTypes.def)
  宏表生成所有类型。
- 关键类/函数: `LeafRecord::toCodeViewRecord`,
  `LeafRecord::fromCodeViewRecord`,
  `MappingTraits<LeafRecord>::mapping`,
  `MappingTraits<MemberRecord>::mapping`,
  `mapLeafRecordImpl`, `mapMemberRecordImpl`,
  `fromDebugT`, `toDebugT`;
  `UnknownLeafRecord`, `LeafRecordBase`, `LeafRecordImpl<...>`,
  `MemberRecordConversionVisitor`;
  `ScalarTraits<TypeName/GUID/TypeIndex/APSInt>::output`;
  多个 `ScalarEnumerationTraits<TypeLeafKind / PointerToMemberRepresentation
  / CallingConvention / VFTableSlotKind / ...>` 和
  `ScalarBitSetTraits<PointerOptions / FunctionOptions / ClassOptions /
  MethodOptions / ...>`。

[`CodeViewYAMLTypeHashing.cpp`](CodeViewYAMLTypeHashing.cpp) —
`.debug$H` (type hash / global hash) ↔ YAML (85 行); 提供 `fromDebugH`,
`toDebugH`。
- 上游: [`COFFEmitter.cpp`](COFFEmitter.cpp) (`.debug$H` 写出, 调
  `CodeViewYAML::toDebugH`),
  [`tools/obj2yaml/coff2yaml.cpp`](../../tools/obj2yaml/coff2yaml.cpp)。
- 下游:
  [`llvm/ObjectYAML/CodeViewYAMLTypeHashing.h`](../../include/llvm/ObjectYAML/CodeViewYAMLTypeHashing.h)
  (引入
  [`llvm/DebugInfo/CodeView/TypeHashing.h`](../../include/llvm/DebugInfo/CodeView/TypeHashing.h),
  [`YAML.h`](../../include/llvm/ObjectYAML/YAML.h),
  [`llvm/Support/BinaryStreamReader.h`](../../include/llvm/Support/BinaryStreamReader.h),
  [`BinaryStreamWriter.h`](../../include/llvm/Support/BinaryStreamWriter.h))。
- 关键类/函数: `CodeViewYAML::fromDebugH`, `CodeViewYAML::toDebugH`,
  `MappingTraits<DebugHSection>::mapping`,
  `ScalarTraits<GlobalHash>::output/input`。

---

## §4. 关键调用链

### 4.1 `llvm-yaml2obj` 主入口

```
tools/yaml2obj/yaml2obj.cpp::main
  └─ convertYAML(input, output, ...) [yaml2obj.cpp]
       ├─ yaml::Input::parse 顶层 YAML 文档
       ├─ MappingTraits<YamlObjectFile>::mapping [ObjectYAML.cpp]
       │    └─ IO::mapTag("!ELF") 识别后实例化 ELFYAML::Object
       └─ switch on YamlObjectFile.Kind:
            ├─ ELF      → yaml2elf(...) [ELFEmitter.cpp]
            │    └─ ELFState<ELFT>::writeELF
            │         ├─ writeELFHeader
            │         ├─ initProgramHeaders
            │         ├─ initSectionHeaders
            │         └─ 对每段 writeSectionContent(...)
            │              ├─ DWARF 段 → DWARFEmitter::emitDebug*
            │              ├─ BBAddrMap 段 → BBAddrMapYAML::encodePayload
            │              └─ 其它 → ELFState 内部 ContiguousBlobAccumulator
            ├─ COFF     → yaml2coff(...) [COFFEmitter.cpp]
            │    └─ COFFParser::writeCOFF
            │         ├─ initializeOptionalHeader
            │         ├─ 对每段 writeSectionContent
            │         │    ├─ Debug$S → CodeViewYAMLDebugSections::toCodeViewSubsectionList
            │         │    ├─ Debug$T → CodeViewYAMLTypes::toDebugT
            │         │    ├─ Debug$H → CodeViewYAMLTypeHashing::toDebugH
            │         │    └─ Debug$P → CodeViewYAMLTypes::toDebugT
            │         └─ writeLoadConfig (load config table)
            ├─ MachO    → yaml2macho(...) [MachOEmitter.cpp]
            │    ├─ 单 arch → MachOWriter::writeMachO
            │    │    ├─ writeHeader
            │    │    ├─ writeLoadCommands (60+ types)
            │    │    ├─ writeSectionData
            │    │    │    └─ DWARF 段 → DWARFEmitter::emitDebug*
            │    │    ├─ writeRelocations
            │    │    └─ writeLinkEditData (rebase/bind/opcodes/trie/...)
            │    └─ fat binary → UniversalWriter::writeFatHeader + writeFatArchs
            ├─ Wasm     → yaml2wasm(...) [WasmEmitter.cpp]
            ├─ XCOFF    → yaml2xcoff(...) [XCOFFEmitter.cpp]
            ├─ GOFF     → yaml2goff(...) [GOFFEmitter.cpp]
            ├─ DXContainer → yaml2dxcontainer(...) [DXContainerEmitter.cpp]
            ├─ Minidump → yaml2minidump(...) [MinidumpEmitter.cpp]
            ├─ Arch     → yaml2archive(...) [ArchiveEmitter.cpp]
            └─ Offload  → yaml2offload(...) [OffloadEmitter.cpp]
                 → 写出二进制到 output file
```

### 4.2 `obj2yaml` 反向

```
tools/obj2yaml/<format>2yaml.cpp (e.g., elf2yaml.cpp)
  └─ 用 llvm::object::createBinary 解析
       └─ switch on file_magic:
            ├─ ELF  → 构造 ELFObjectFile<ELFT>
            │    └─ 迭代 sections / symbols / relocations
            │         → 用 ELFYAML.cpp 的 MappingTraits 写出 YAML
            │              └─ 顶层 !ELF tag 触发 ObjectYAML.cpp 分发
            ├─ COFF → 类似走 COFFYAML.cpp
            ├─ Mach-O → 类似走 MachOYAML.cpp (含 UniversalYAML)
            ├─ Wasm / XCOFF / GOFF / DXContainer / Minidump 类似
            └─ Archive → tools/obj2yaml/archive2yaml.cpp
```

### 4.3 CodeView Debug$S 写出 (COFF + CodeView YAML 协作)

```
COFFEmitter.cpp::writeCOFF
  └─ 对每段 writeSectionContent
       └─ 段名 ".debug$S"
            └─ toDebugS(S, ...) [CodeViewYAMLDebugSections.cpp]
                 ├─ initializeStringsAndChecksums
                 └─ toCodeViewSubsectionList
                      ├─ YAMLChecksumsSubsection::toCodeView (经 SubsectionConversionVisitor)
                      ├─ YAMLLinesSubsection::toCodeView
                      ├─ YAMLSymbolsSubsection::toCodeView
                      │    └─ 对每 symbol: CodeViewYAMLSymbols::SymbolRecord::toCodeViewSymbol
                      │         └─ 用 SYMBOL_RECORD_ALIAS 宏表查映射
                      └─ ...
                           → 写出 CV 字节流
```

### 4.4 BBAddrMap encodePayload 复用

```
ELFEmitter.cpp::ELFState<ELFT>::writeSectionContent(... BBAddrMapSection ...)
  └─ BBAddrMapYAML::encodePayload(CBA, ...) [BBAddrMapYAML.cpp]
       ├─ 写 version / features / BB ranges (4/8 字节地址)
       └─ 写 PGO frequency + BB hash
            └─ CBA.writeAsBinary(...) [ContiguousBlobAccumulator]
                 → 直接复用二进制编码, 不需要单独 emitter 文件
```

---

## §5. 推荐阅读顺序

### 阶段 1: 框架 (1-2 小时)
1. [`ObjectYAML.cpp`](ObjectYAML.cpp) — 顶层 tag dispatcher, 看懂
   `YamlObjectFile` 与 `IO::mapTag`。
2. [`yaml2obj.cpp`](yaml2obj.cpp) — 看懂 `convertYAML` 入口与
   `yaml2ObjectFile` (测试用) 的区别。
3. [`YAML.cpp`](YAML.cpp) — `BinaryRef` YAMLIO 特化, 所有段数据的格式。
4. [`ContiguousBlobAccumulator.cpp`](ContiguousBlobAccumulator.cpp) — 共享
   blob 写入器, 看懂 `padToAlignment` / `takeLimitError` 等。

### 阶段 2: 单格式深入 (任选一种, 2-3 小时)
- **ELF** (主线): [`ELFYAML.cpp`](ELFYAML.cpp) →
  [`ELFEmitter.cpp`](ELFEmitter.cpp) (最重, ~4000 行)。
- **Mach-O** (Apple): [`MachOYAML.cpp`](MachOYAML.cpp) →
  [`MachOEmitter.cpp`](MachOEmitter.cpp)。
- **COFF** (Windows): [`COFFYAML.cpp`](COFFYAML.cpp) →
  [`COFFEmitter.cpp`](COFFEmitter.cpp)。
- **Wasm** / **XCOFF** / **GOFF** / **DXContainer** / **Minidump**: 各自
  一对, 文件相对短。

### 阶段 3: 小格式 (1 小时)
- [`ArchiveYAML.cpp`](ArchiveYAML.cpp) +
  [`ArchiveEmitter.cpp`](ArchiveEmitter.cpp) (最小一对, 100 行内)。
- [`OffloadYAML.cpp`](OffloadYAML.cpp) +
  [`OffloadEmitter.cpp`](OffloadEmitter.cpp) (类似小)。
- [`GOFFYAML.cpp`](GOFFYAML.cpp) +
  [`GOFFEmitter.cpp`](GOFFEmitter.cpp) (44 + 280 行)。

### 阶段 4: 共用 helpers (按需)
- [`BBAddrMapYAML.cpp`](BBAddrMapYAML.cpp) — 格式无关的
  `encodePayload`, 看懂怎么直接被 ELF emitter 复用。
- [`DWARFYAML.cpp`](DWARFYAML.cpp) +
  [`DWARFEmitter.cpp`](DWARFEmitter.cpp) — DWARF 段通用, 多个 emitter
  间接调。

### 阶段 5: CodeView (1-2 小时, 也按需)
1. [`CodeViewYAMLSymbols.cpp`](CodeViewYAMLSymbols.cpp) — symbols, 看懂
   `SYMBOL_RECORD_ALIAS` 宏表展开。
2. [`CodeViewYAMLTypes.cpp`](CodeViewYAMLTypes.cpp) — types, 同样宏表。
3. [`CodeViewYAMLDebugSections.cpp`](CodeViewYAMLDebugSections.cpp) —
   9 个 subsection mapping。
4. [`CodeViewYAMLTypeHashing.cpp`](CodeViewYAMLTypeHashing.cpp) — 简单。

---

## §6. 常用操作指南

### 6.1 添加新对象格式 (假设 `MyFormat`)

1. 在 `llvm/include/llvm/ObjectYAML/MyFormatYAML.h` 定义:
   - `MyFormatYAML::Object` (顶层结构)
   - 用 `MappingTraits<Object>::mapping` 注册 YAML 映射
2. 在 `MyFormatYAML.cpp` (放在本目录) 实现 `MappingTraits` /
   `ScalarTraits` / `ScalarEnumerationTraits` / `ScalarBitSetTraits`
   特化。
3. 创建 `MyFormatEmitter.cpp` (放在本目录), 实现 `yaml2myformat`
   函数 (签名见
   [`yaml2obj.h`](../../include/llvm/ObjectYAML/yaml2obj.h))。
4. 在 [`yaml2obj.cpp`](yaml2obj.cpp) 的 `convertYAML` switch 加新 case;
   在
   [`ObjectYAML.cpp`](ObjectYAML.cpp) 的
   `MappingTraits<YamlObjectFile>::mapping` 加新 `IO::mapTag`。
5. 配套 `tools/obj2yaml/myformat2yaml.cpp` (反向)。
6. 单测: `llvm/test/tools/yaml2obj/MyFormat/` +
   [`unittests/ObjectYAML/MyFormatYAMLTest.cpp`](../../unittests/ObjectYAML/)。

### 6.2 添加新 YAML 字段到现有格式

1. 在对应 `XxxYAML.h` 加新字段 (struct)。
2. 在 [`XxxYAML.cpp`](ELFYAML.cpp) 的 `MappingTraits<Object>::mapping`
   加 `IO.mapOptional("NewField", Obj.NewField)`, 标 default 值。
3. 如果新字段影响二进制输出, 同步在
   [`XxxEmitter.cpp`](ELFEmitter.cpp) 的 `writeXxx` 处理它。
4. 单测: 加 round-trip 测试到
   [`unittests/ObjectYAML/YAML2ObjTest.cpp`](../../unittests/ObjectYAML/YAML2ObjTest.cpp)。

### 6.3 给 DWARF 添加新段支持

1. 在 [`DWARFYAML.h`](../../include/llvm/ObjectYAML/DWARFYAML.h) 加新
   section 结构 (e.g., `MyNewDebugSection`)。
2. 在 [`DWARFYAML.cpp`](DWARFYAML.cpp) 加 `MappingTraits` 特化 +
   注册到 `DWARFYAML::Data::getNonEmptySectionNames`。
3. 在 [`DWARFEmitter.cpp`](DWARFEmitter.cpp) 实现 `emitMyNewDebug`,
   入口加到 [`DWARFEmitter.h`](../../include/llvm/ObjectYAML/DWARFEmitter.h)。
4. 单测:
   [`unittests/ObjectYAML/DWARFYAMLTest.cpp`](../../unittests/ObjectYAML/DWARFYAMLTest.cpp)。

### 6.4 给 CodeView 添加新 symbol/type kind

1. 在
   [`llvm/include/llvm/DebugInfo/CodeView/CodeViewSymbols.def`](../../include/llvm/DebugInfo/CodeView/CodeViewSymbols.def)
   加新 `SYMBOL_RECORD_ALIAS(name, value)` 或 `SYMBOL_RECORD(name, value)`。
2. (类似) 在
   [`llvm/include/llvm/DebugInfo/CodeView/CodeViewTypes.def`](../../include/llvm/DebugInfo/CodeView/CodeViewTypes.def)
   加 type。
3. 重编; `SymbolRecordImpl<...>` / `LeafRecordImpl<...>` 自动通过宏表
   展开, [`CodeViewYAMLSymbols.cpp`](CodeViewYAMLSymbols.cpp) /
   [`CodeViewYAMLTypes.cpp`](CodeViewYAMLTypes.cpp) 自动支持。
4. 测试: round-trip 测试一个新 kind。

### 6.5 调试 YAML 解析失败

1. 看 `tools/yaml2obj/yaml2obj.cpp` 调
   [`yaml2obj.cpp::convertYAML`](yaml2obj.cpp) 的
   `ErrorHandler` 报错 (`WithColor::error()` 输出)。
2. 常见原因:
   - 字段名拼错 (check `MappingTraits::mapping`)
   - 枚举值不在
     `ScalarEnumerationTraits<EnumType>::enumeration` 列表
   - `BinaryRef` 长度对不上 (e.g., "010203" 必须长度偶数)
   - `ContiguousBlobAccumulator::checkLimit` 触发 (write 超 MaxSize)
3. 加 `-DLY_ENABLE_DUMP=1` 或在 `yaml2obj.cpp` 加
   `llvm::yaml::dumpNode` 输出中间结构。

### 6.6 用 ObjectYAML 作为库 (写测试 fixture)

1. 链接 `LLVMObjectYAML`。
2. `#include "llvm/ObjectYAML/yaml2obj.h"` +
   `#include "llvm/ObjectYAML/ObjectYAML.h"`。
3. 构造 `YamlObjectFile` 对象 (直接 in-memory 不用 YAML 字符串)。
4. 调 `yaml2ObjectFile(YamlObjectFile, ...)`, 得到 `Expected<std::unique_ptr<ObjectFile>>`。
5. 单测样例:
   [`unittests/ObjectYAML/YAML2ObjTest.cpp`](../../unittests/ObjectYAML/YAML2ObjTest.cpp)。

---

## §7. NT 注释索引

当前 `llvm/lib/ObjectYAML/` 下尚无 `// <NT>` 注释。姊妹 overview:

- [`llvm/lib/IR/0-overview.md`](../IR/0-overview.md) — LLVM IR 层
- [`llvm/lib/CodeGen/0-overview.md`](../CodeGen/0-overview.md) — target-independent CodeGen
- [`llvm/lib/Target/RISCV/0-overview.md`](../Target/RISCV/0-overview.md) — RISCV 后端
- [`llvm/lib/TargetParser/0-overview.md`](../TargetParser/0-overview.md) — TargetParser
- [`llvm/lib/Analysis/0-overview.md`](../Analysis/0-overview.md) — IR 层分析
- [`llvm/lib/Frontend/0-overview.md`](../Frontend/0-overview.md) — 前端桥接层
- [`llvm/lib/Object/0-overview.md`](../Object/0-overview.md) — 二进制对象解析层 (本目录
  的姊妹)

按"少而精"原则, 添加 NT 注释时建议优先级:

1. [`ELFYAML.cpp`](ELFYAML.cpp) + [`ELFEmitter.cpp`](ELFEmitter.cpp) — 各 5-8 段
   (库最大, 关键路径)
2. [`MachOYAML.cpp`](MachOYAML.cpp) + [`MachOEmitter.cpp`](MachOEmitter.cpp) —
   各 3-5 段 (60+ load command 是核心)
3. [`DWARFEmitter.cpp`](DWARFEmitter.cpp) — 5-8 段 (与 ELF/Mach-O 协作的关键)
4. [`ObjectYAML.cpp`](ObjectYAML.cpp) — 3-5 段 (顶层 tag dispatch 模式)
5. 单 utility 文件 ([`ArchiveYAML.cpp`](ArchiveYAML.cpp)、
   [`GOFFYAML.cpp`](GOFFYAML.cpp)、[`YAML.cpp`](YAML.cpp)) — 1-2 段

---

**姊妹文档**: 已建立完整目录索引。`llvm-yaml2obj` 用户可见的用法在
[`llvm/tools/yaml2obj/`](../../tools/yaml2obj/), `obj2yaml` 用法在
[`llvm/tools/obj2yaml/`](../../tools/obj2yaml/)。LLVM 二进制对象解析
(`lib/Object`) 与 YAML 构造 (`lib/ObjectYAML`) 是双向关系 — 前者读已有
对象, 后者写新对象。