<!-- <NT>overview:clang/lib/Serialization/ -->

# Clang Serialization 库导读 — `clang/lib/Serialization/`

> 本文档梳理 `clang/lib/Serialization/` 目录下所有源文件 (21 个
> .cpp/.h + 1 个 `CMakeLists.txt` = 22 个) 的职责、上下游与推荐
> 阅读顺序。
>
> 目标读者: 想理解 **Clang AST 持久化层** (`.pch` / `.pcm` /
> `.ast` bitstream 读写、模块加载链、容器包装、跨模块索引) 的
> 开发者, 以及要给 Clang 加新 PCH/Module 容器格式 / 加新
> `ModuleFileExtension` 块 / 调优 PCH/PCM 加载性能的人。
>
> 所有路径相对 `clang/lib/Serialization/`。同名公开头文件位于
> `clang/include/clang/Serialization/` (如 `ASTReader.h`、
> `ASTWriter.h`、`ModuleFile.h`、`ModuleManager.h`、
> `ASTBitCodes.h` 等)。本目录编译为 `clangSerialization` 库, 链接
> `clangAST/Basic/Lex/Sema` + LLVM `BitReader/BitstreamReader/Object`。

---

## §0. Clang Serialization 库在编译流水线中的位置

`clang/lib/Serialization` 是 Clang 的 **AST 持久化层**, 负责:

1. 把 `ASTContext` 中所有 Decl / Type / Stmt / Expr / Identifier /
   Selector / Macro / SourceLocation / Module / DiagnosticOptions
   **序列化** 到 LLVM bitstream 格式的 `.pch` / `.pcm` / `.ast`
   文件;
2. 反向把 bitstream 文件 **反序列化** 回 `ASTContext`, 实现
   `ExternalASTSource` 接口供 Sema / Parser 在 lazy deserialization
   期间按需加载;
3. 维护 `ModuleManager` / `GlobalModuleIndex` /
   `ModuleCache` 等模块/PCH 文件管理基础设施;
4. 通过 `PCHContainerOperations` 把 bitstream 直接写入 / 嵌入到
   Mach-O / ELF / COFF object 文件中 (使 PCH 既能编译又能 link)。

它在 Clang 内部的层次:

```
┌─────────────────────────────────────────────────────────┐
│ 产物: .pch (precompiled header) / .pcm (precompiled       │
│       module) / .ast (ASTUnit dump) / __module__/        │
│       modules.idx (cross-module index)                    │
└─────────────────────────────────────────────────────────┘
                ▲
                │ (写)
                │
┌─────────────────────────────────────────────────────────┐
│ clang/lib/Serialization  (本目录) ← 你在这里              │
│   · ASTWriter + ASTWriterDecl + ASTWriterStmt:           │
│     把 ASTContext → bitstream                            │
│   · ASTReader + ASTReaderDecl + ASTReaderStmt:           │
│     把 bitstream → ASTContext (实现 ExternalASTSource)    │
│   · ModuleFile: 单个文件 in-memory 表示                   │
│   · ModuleManager: 多 ModuleFile 依赖图 + 签名校验        │
│   · GlobalModuleIndex: 跨模块索引 (modules.idx)          │
│   · ModuleCache (cross-process) + InMemoryModuleCache:   │
│     PCM 文件缓存                                          │
│   · PCHContainerOperations + ObjectFilePCHContainerReader:│
│     PCH 容器包装 (raw / Mach-O / ELF / COFF)              │
│   · ModuleFileExtension: 自定义块插件                     │
│   · GeneratePCH / CXX20ModulesGenerator: SemaConsumer     │
│     驱动 ASTWriter                                        │
│   · TemplateArgumentHasher + MultiOnDiskHashTable:       │
│     跨 TU 稳定 hash + 多模块 hash 表合并                  │
└─────────────────────────────────────────────────────────┘
                │
                ▼
┌─────────────────────────────────────────────────────────┐
│ 上游 (写):                                                │
│   · Frontend/FrontendActions.cpp (GeneratePCHAction,     │
│     GenerateModuleAction)                                │
│   · Frontend/CompilerInstance.cpp (save .pch / load .pcm)│
│   · Frontend/ASTUnit.cpp (Save → .ast)                   │
│ 上游 (读):                                                │
│   · Frontend/ASTUnit.cpp (LoadFromASTFile)               │
│   · Frontend/CompilerInstance.cpp (loadModule)           │
│   · Frontend/PrecompiledPreamble.cpp (PCH 缓存复用)      │
│   · Sema (lazy lookup 触发 deserialization)              │
│   · Lex/Preprocessor (SourceLocation 还原触发 PPCallbacks)│
└─────────────────────────────────────────────────────────┘
                │
                ▼
┌─────────────────────────────────────────────────────────┐
│ 下游依赖:                                                │
│   · llvm/Bitstream/BitstreamReader.h + BitstreamWriter.h │
│   · llvm/Object/ (MachO/ELF/COFF 解析)                  │
│   · clang/lib/AST (Decl/Stmt/Type 节点创建)              │
│   · clang/lib/Basic (SourceManager / IdentifierTable /    │
│     LangOptions)                                         │
│   · clang/lib/Sema (ExternalASTSource 接口消费)          │
└─────────────────────────────────────────────────────────┘
```

### §0.1 公开接口 (`clang/include/clang/Serialization/`)

本目录 `.cpp` 文件依赖的 **公开头** 在
[`clang/include/clang/Serialization/`](../../include/clang/Serialization/),
最关键的:

- [`ASTReader.h`](../../include/clang/Serialization/ASTReader.h)
  — `class ASTReader` 顶层入口, 实现 `ExternalASTSource`。
- [`ASTWriter.h`](../../include/clang/Serialization/ASTWriter.h)
  — `class ASTWriter` 顶层入口。
- [`ASTBitCodes.h`](../../include/clang/Serialization/ASTBitCodes.h)
  — bitstream 格式的稳定枚举 (block ID / record code / abbrev)。
- [`ASTRecordReader.h`](../../include/clang/Serialization/ASTRecordReader.h)
  + [`ASTRecordWriter.h`](../../include/clang/Serialization/ASTRecordWriter.h)
  — 单个 bitcode record 的游标 / 构造器。
- [`ASTDeserializationListener.h`](../../include/clang/Serialization/ASTDeserializationListener.h)
  — 反序列化观察者回调。
- [`ModuleFile.h`](../../include/clang/Serialization/ModuleFile.h)
  — `class ModuleFile` 单个文件的内存表示。
- [`ModuleManager.h`](../../include/clang/Serialization/ModuleManager.h)
  — `class ModuleManager` 多文件依赖管理。
- [`ModuleFileExtension.h`](../../include/clang/Serialization/ModuleFileExtension.h)
  — 自定义 PCM 块插件。
- [`ModuleCache.h`](../../include/clang/Serialization/ModuleCache.h)
  — `class ModuleCache` / `CrossProcessModuleCache` 抽象 + 实现。
- [`InMemoryModuleCache.h`](../../include/clang/Serialization/InMemoryModuleCache.h)
  — `class InMemoryModuleCache` 内存缓存。
- [`GlobalModuleIndex.h`](../../include/clang/Serialization/GlobalModuleIndex.h)
  — `class GlobalModuleIndex` 跨模块标识符索引。
- [`PCHContainerOperations.h`](../../include/clang/Serialization/PCHContainerOperations.h)
  — `class PCHContainerOperations` 注册表 + `struct PCHBuffer`。
- [`ObjectFilePCHContainerReader.h`](../../include/clang/Serialization/ObjectFilePCHContainerReader.h)
  — 从 Mach-O/ELF/COFF 中读 PCH。
- [`ContinuousRangeMap.h`](../../include/clang/Serialization/ContinuousRangeMap.h)
  — 局部→全局 ID 映射的 sparse 区间 map。
- [`SourceLocationEncoding.h`](../../include/clang/Serialization/SourceLocationEncoding.h)
  — `SourceLocation` ↔ bitstream-stable 编码。
- [`SerializationDiagnostic.h`](../../include/clang/Serialization/SerializationDiagnostic.h)
  — Serialization 诊断 ID。
- [`TypeBitCodes.def`](../../include/clang/Serialization/TypeBitCodes.def)
  — `Type::TypeClass` ↔ 稳定 record code 的 X-macro 表。

### §0.2 与 [`clang/lib/Frontend/`](../Frontend/) 的关系

- [`Frontend/ASTUnit.cpp`](../Frontend/ASTUnit.cpp) 调
  `ASTReader::ReadAST` 加载 `.ast`; 调 `ASTWriter` 存 `.ast`。
- [`Frontend/CompilerInstance.cpp`](../Frontend/CompilerInstance.cpp)
  通过 `createPCHExternalASTSource` 创建 `ASTReader`;
  `loadModule` 走完整模块加载链 (ModuleManager → ModuleCache →
  PCHContainerOperations)。
- [`Frontend/FrontendActions.cpp`](../Frontend/FrontendActions.cpp)
  的 `GeneratePCHAction` / `GenerateModuleAction` / `MigrateSourceAction`
  触发 `GeneratePCH.cpp` 的 `PCHGenerator` /
  `CXX20ModulesGenerator`。
- [`Frontend/PrecompiledPreamble.cpp`](../Frontend/PrecompiledPreamble.cpp)
  把 PCH 缓存复用: `BuildPreamble` 走 `ASTWriter`, `AddImplicitPreamble`
  时调 `ASTReader` 注入。

### §0.3 与 [`clang/lib/Sema/`](../Sema/) 的关系

`ASTReader` 实现 `ExternalASTSource`: 当 Sema 在 lookup /
type-completion 期间发现某个 Decl / Type 来自外部 `ASTContext`
(即从 `.pch` / `.pcm` 加载), 调 `ASTReader::CompleteDecl` /
`CompleteType` / `GetExternalDecl` 触发按需反序列化, 完成后把
AST 节点挂回 `ASTContext`。

### §0.4 与 [`clang/lib/AST/`](../AST/) 的关系

`ASTWriter::WriteAST` 遍历 `ASTContext` 中的所有 Decl / Type /
Stmt 子类; `ASTWriterDecl.cpp` 的 `ASTDeclWriter::VisitXxx` 对应
`AST/Decl.h` 中每个 Decl 子类; `ASTWriterStmt.cpp` 的
`ASTStmtWriter::VisitXxx` 对应 `AST/Stmt.h` / `AST/Expr.h`。反方
向, `ASTReaderDecl.cpp` 的 `ASTDeclReader::VisitXxx` /
`ASTReaderStmt.cpp` 的 `ASTStmtReader::VisitXxx` 重建这些节点。

### §0.5 与 [`clang/lib/Lex/`](../Lex/) 的关系

`ASTWriter::WritePreprocessor` 把 `Preprocessor` 中的
`HeaderSearch` + `HeaderFileInfo` + `MacroInfo` +
`SourceManager` 的 line table + `PPCallbacks` 状态全部序列化;
`ASTReader::ReadPreprocessorBlock` 反向读回, 触发 PPCallbacks
让 Lex 重建完整的预处理环境。

### §0.6 与 [`clang/lib/Basic/`](../Basic/) 的关系

`SourceManager` / `IdentifierTable` / `LangOptions` /
`TargetInfo` / `FileManager` / `Module` 等基础对象都来自 Basic,
Serialization 仅序列化它们的内部状态。

---

## §1. 编译流水线概览

```
写 (.pch / .pcm / .ast):

Frontend (GeneratePCHAction / GenerateModuleAction)
  │
  ▼
PCHGenerator / CXX20ModulesGenerator (GeneratePCH.cpp)
  └─ new ASTWriter (ASTWriter.cpp)
       └─ WriteAST (顶层入口)
            ├─ WriteControlBlock (magic / version / 依赖列表)
            ├─ WriteSourceManagerBlock (SourceLocation + FileEntry)
            ├─ WritePreprocessor (Macros / HeaderSearch / Lexer state)
            ├─ WriteDeclsAndTypes
            │    ├─ WriteTypeTable (TypeIdx + Type::TypeClass)
            │    ├─ WriteDecls (每个 Decl 一条 record)
            │    │    └─ ASTWriterDecl.cpp ASTDeclWriter::VisitXxx
            │    ├─ WriteStmt (每个 Stmt 一条 record)
            │    │    └─ ASTWriterStmt.cpp ASTStmtWriter::VisitXxx
            │    └─ WriteIdentifiers + WriteSelectors
            ├─ WriteSubmodules (Module / Umbrella / Exports)
            ├─ WriteLookups (MultiOnDiskHashTable)
            └─ ModuleFileExtension 钩子 (custom block)


读 (.pch / .pcm / .ast):

Frontend (ASTUnit::LoadFromASTFile / CompilerInstance::loadModule)
  │
  ▼
ModuleCache::getPCMFile / LookupFileName
  │
  ▼
ModuleManager::addModule (ModuleManager.cpp)
  ├─ ModuleCache 校验 + 读 buffer
  ├─ PCHContainerOperations 解包 (raw / MachO / ELF / COFF)
  └─ 构造 ModuleFile (ModuleFile.cpp) + 解析 signature / 依赖
  │
  ▼
ASTReader::ReadAST (ASTReader.cpp)
  ├─ ReadControlBlock (校验 magic / version / 依赖匹配)
  ├─ ReadSourceManagerBlock (line table / FileEntry)
  ├─ ReadPreprocessorBlock (Macros / HeaderSearch)
  ├─ ReadDeclsAndTypes (lazy / on-demand)
  │    ├─ 类型懒加载 (按需通过 ASTReader::CompleteType)
  │    ├─ Decl 懒加载 (按需通过 ASTReader::CompleteDecl)
  │    │    └─ ASTReaderDecl.cpp ASTDeclReader::VisitXxx
  │    └─ Stmt 懒加载 (按需)
  │         └─ ASTReaderStmt.cpp ASTStmtReader::VisitXxx
  └─ GlobalModuleIndex 注入 (modules.idx)

旁路:
  · ModuleFileExtension: 自定义 PCM 块读写
  · GlobalModuleIndex: 跨模块标识符 → 模块集合索引
  · InMemoryModuleCache: 测试 / non-disk 场景
  · TemplateArgumentHasher: 跨 TU 模板参数稳定 hash
  · MultiOnDiskHashTable: 多模块 hash 表合并 (override semantics)
```

---

## §2. 文件目录结构

```
clang/lib/Serialization/  (22 文件, 0 子目录)
├── §3.1  ASTReader (顶层 + per-Decl/per-Stmt)              (4 文件)
├── §3.2  ASTWriter (顶层 + per-Decl/per-Stmt)              (3 文件)
├── §3.3  PCH 生成 / Frontend glue                          (1 文件)
├── §3.4  Module / PCH 文件表示与管理                       (4 文件)
├── §3.5  PCH Container / Object-file wrappers              (2 文件)
├── §3.6  Module Cache (on-disk + in-memory)                (2 文件)
└── §3.7  共享 helper / on-disk 结构 / type IDs             (5 文件 + CMakeLists)
```

### 2.1 文件数量统计

| 区域 | .cpp | .h | 合计 |
|------|------|-----|------|
| ASTReader | 3 | 1 | 4 |
| ASTWriter | 3 | 0 | 3 |
| PCH 生成 / Frontend glue | 1 | 0 | 1 |
| Module 文件管理 | 3 | 1 | 4 |
| PCH Container | 2 | 0 | 2 |
| Module Cache | 2 | 0 | 2 |
| 共享 helper | 3 | 3 | 6 |
| Build | 1 | 0 | 1 (`CMakeLists.txt`) |
| **总计** | **~18** | **~5** | **~23** + `CMakeLists.txt` |

(实际为 22 个文件, 因部分 .h 仅有声明无 .cpp 实现)

---

## §3. 文件详解

### 3.1 ASTReader (顶层 + per-Decl/per-Stmt)

[`ASTReader.cpp`](ASTReader.cpp) — 顶层 `ASTReader`: 读 `.pch` /
`.pcm` / `.ast` bitstream, 反序列化为 `ASTContext` (types /
decls / stmts / identifiers / source locations / macros /
modules); 实现 `ExternalASTSource` 让 Sema on-demand load。
- 上游:
  [`Frontend/ASTUnit.cpp`](../Frontend/ASTUnit.cpp)
  (`LoadFromASTFile`),
  [`Frontend/CompilerInstance.cpp`](../Frontend/CompilerInstance.cpp)
  (`loadModule`),
  [`Frontend/FrontendActions.cpp`](../Frontend/FrontendActions.cpp)
  (`GeneratePCHAction`)。
- 下游: [`ASTReaderDecl.cpp`](ASTReaderDecl.cpp),
  [`ASTReaderStmt.cpp`](ASTReaderStmt.cpp),
  [`ASTCommon.cpp`](ASTCommon.cpp),
  [`ModuleManager.cpp`](ModuleManager.cpp),
  [`GlobalModuleIndex.cpp`](GlobalModuleIndex.cpp)。
- 关键类/函数: `ASTReader`, `ReadASTCore`, `ReadControlBlock`,
  `ReadSourceManagerBlock`, `ReadPreprocessorBlock`, `ReadDecls`,
  `ReadDeclContextStorage`, `CompleteRedeclChain`, `Error`。

[`ASTReaderDecl.cpp`](ASTReaderDecl.cpp) — 每个 Decl 子类的反序列
化 (`ASTDeclReader::Visit*`): 从 record 重建 Decl 节点, 与
`ASTWriterDecl` 对称。
- 上游: [`ASTReader.cpp`](ASTReader.cpp)。
- 下游: [`ASTReaderStmt.cpp`](ASTReaderStmt.cpp),
  [`ASTCommon.cpp`](ASTCommon.cpp),
  [`TemplateArgumentHasher.cpp`](TemplateArgumentHasher.cpp)。
- 关键类/函数: `ASTDeclReader`, `ASTRecordReader`, `VisitDecl`,
  `VisitRecordDecl`, `VisitFunctionDecl`, `VisitVarDecl`,
  `VisitObjCInterfaceDecl`, `VisitCXXRecordDecl`。

[`ASTReaderStmt.cpp`](ASTReaderStmt.cpp) — 每个 Stmt / Expr 子类
的反序列化 (`ASTStmtReader::Visit*`): 重建表达式 / 语句子树。
- 上游: [`ASTReader.cpp`](ASTReader.cpp),
  [`ASTReaderDecl.cpp`](ASTReaderDecl.cpp)。
- 下游: [`ASTReaderDecl.cpp`](ASTReaderDecl.cpp)。
- 关键类/函数: `ASTStmtReader`, `ASTReader`, `VisitStmt`,
  `VisitExpr`, `VisitCallExpr`, `VisitDeclStmt`,
  `VisitCompoundStmt`, `ReadTemplateKWArg`。

[`ASTReaderInternals.h`](ASTReaderInternals.h) — 内部 helper (查找
trait 类), 给 ASTReader 用于 on-disk hash 表 (decls / identifiers
/ selectors / header info)。
- 上游: [`ASTReader.cpp`](ASTReader.cpp)。
- 下游: [`MultiOnDiskHashTable.h`](MultiOnDiskHashTable.h)。
- 关键类/函数: `ASTDeclContextNameLookupTrait`,
  `ModuleLocalNameLookupTrait`, `ASTIdentifierLookupTrait`,
  `ASTSelectorLookupTrait`, `HeaderFileInfoTrait`,
  `ASTIdentifierLookupTable`, `LazySpecializationInfoLookupTrait`。

### 3.2 ASTWriter (顶层 + per-Decl/per-Stmt)

[`ASTWriter.cpp`](ASTWriter.cpp) — 顶层 `ASTWriter`: 序列化
`ASTContext` 到 bitstream。
- 上游: [`GeneratePCH.cpp`](GeneratePCH.cpp)
  (`PCHGenerator::HandleTranslationUnit`),
  [`Frontend/CompilerInstance`](../Frontend/CompilerInstance.cpp)。
- 下游: [`ASTWriterDecl.cpp`](ASTWriterDecl.cpp),
  [`ASTWriterStmt.cpp`](ASTWriterStmt.cpp),
  [`ASTCommon.cpp`](ASTCommon.cpp),
  [`ModuleFile.cpp`](ModuleFile.cpp)。
- 关键类/函数: `ASTWriter`, `WriteAST`, `WriteControlBlock`,
  `WriteSourceManagerBlock`, `WritePreprocessor`,
  `WriteDeclsAndTypes`, `WriteSubmodules`, `createSignature`。

[`ASTWriterDecl.cpp`](ASTWriterDecl.cpp) — 每个 Decl 子类序列化
(`ASTDeclWriter::Visit*`): 通过 `AbstractBasicWriter` 为每个
Decl kind 发一条 record。
- 上游: [`ASTWriter.cpp`](ASTWriter.cpp)。
- 下游: [`ASTCommon.cpp`](ASTCommon.cpp),
  [`ASTWriterStmt.cpp`](ASTWriterStmt.cpp)。
- 关键类/函数: `ASTDeclWriter`, `ASTRecordWriter`, `VisitDecl`,
  `VisitRecordDecl`, `VisitFunctionDecl`, `VisitVarDecl`,
  `VisitObjCInterfaceDecl`, `VisitDeclContext`。

[`ASTWriterStmt.cpp`](ASTWriterStmt.cpp) — 每个 Stmt / Expr 子类
序列化 (`ASTStmtWriter::Visit*`): 为每种 statement/expression
类发 record。
- 上游: [`ASTWriter.cpp`](ASTWriter.cpp)。
- 下游: [`ASTWriterDecl.cpp`](ASTWriterDecl.cpp)。
- 关键类/函数: `ASTStmtWriter`, `VisitStmt`, `VisitExpr`,
  `VisitCallExpr`, `VisitDeclStmt`, `VisitCompoundStmt`,
  `VisitIfStmt`, `VisitCXXConstructExpr`。

### 3.3 PCH 生成 / Frontend glue

[`GeneratePCH.cpp`](GeneratePCH.cpp) — `PCHGenerator` /
`CXX20ModulesGenerator` / `ReducedBMIGenerator` `SemaConsumer`:
在 `HandleTranslationUnit` 驱动 `ASTWriter` 输出 `.pch` / `.pcm` /
BMI。
- 上游:
  [`Frontend/FrontendActions.cpp`](../Frontend/FrontendActions.cpp)
  (`GeneratePCHAction`),
  [`Frontend/CompilerInstance.cpp`](../Frontend/CompilerInstance.cpp)。
- 下游: [`ASTWriter.cpp`](ASTWriter.cpp),
  [`ASTCommon.cpp`](ASTCommon.cpp)。
- 关键类/函数: `PCHGenerator`, `CXX20ModulesGenerator`,
  `ReducedBMIGenerator`, `HandleTranslationUnit`,
  `InitializeSema`, `GetASTMutationListener`。

### 3.4 Module / PCH 文件表示与管理

[`ModuleFile.cpp`](ModuleFile.cpp) — `ModuleFile` 析构 + debug dump
(per-file 元数据、查找表、ID remap)。
- 上游: [`ModuleManager.cpp`](ModuleManager.cpp),
  [`ASTReader.cpp`](ASTReader.cpp)。
- 下游: [`ASTReaderInternals.h`](ASTReaderInternals.h)。
- 关键类/函数: `ModuleFile`, `dumpLocalRemap`, `~ModuleFile`。

[`ModuleManager.cpp`](ModuleManager.cpp) — `ModuleManager`: 跟踪
已加载 ModuleFile 集合, 依赖图, signature / mtime 校验, visit
顺序。
- 上游: [`ASTReader.cpp`](ASTReader.cpp)。
- 下游: [`ModuleFile.cpp`](ModuleFile.cpp),
  [`ModuleCache.cpp`](ModuleCache.cpp),
  [`GlobalModuleIndex.cpp`](GlobalModuleIndex.cpp),
  [`PCHContainerOperations.cpp`](PCHContainerOperations.cpp)。
- 关键类/函数: `ModuleManager`, `addModule`, `removeModules`,
  `lookupByModuleName`, `lookupByFileName`,
  `isModuleFileOutOfDate`, `checkSignature`, `visit`。

[`ModuleFileExtension.cpp`](ModuleFileExtension.cpp) —
`ModuleFileExtension` / `ModuleFileExtensionWriter` /
`ModuleFileExtensionReader` anchor + out-of-line 定义, 插件扩
展块的基类。
- 上游:
  [`Frontend/CompilerInstance.cpp`](../Frontend/CompilerInstance.cpp)
  (extension registration)。
- 关键类/函数: `ModuleFileExtension`,
  `ModuleFileExtensionWriter`, `ModuleFileExtensionReader`,
  `hashExtension`。

[`GlobalModuleIndex.cpp`](GlobalModuleIndex.cpp) —
`GlobalModuleIndex`: 跨模块标识符索引, 写到 `modules.idx` 给
header → module 快速查找。
- 上游: [`ModuleManager.cpp`](ModuleManager.cpp),
  [`ASTReader.cpp`](ASTReader.cpp)。
- 下游: [`ModuleFile.cpp`](ModuleFile.cpp),
  [`PCHContainerOperations.cpp`](PCHContainerOperations.cpp)。
- 关键类/函数: `GlobalModuleIndex`, `GlobalModuleIndexBuilder`,
  `readIndex`, `writeIndex`, `lookupIdentifier`,
  `getModuleDependencies`, `loadedModuleFile`,
  `createIdentifierIterator`。

### 3.5 PCH Container / Object-file wrappers

[`PCHContainerOperations.cpp`](PCHContainerOperations.cpp) —
`PCHContainerOperations` 注册表 + raw (未包装) `.pch`/`.pcm`
container writer/reader 实现。
- 上游:
  [`Frontend/CompilerInstance`](../Frontend/CompilerInstance.cpp)
  (Construction)。
- 下游: [`ObjectFilePCHContainerReader.cpp`](ObjectFilePCHContainerReader.cpp)。
- 关键类/函数: `PCHContainerOperations`,
  `RawPCHContainerWriter`, `RawPCHContainerReader`,
  `RawPCHContainerGenerator`, `registerWriter`, `registerReader`。

[`ObjectFilePCHContainerReader.cpp`](ObjectFilePCHContainerReader.cpp)
— 通过 `llvm::object` 解析器从 Mach-O / ELF / COFF object 中抽
出嵌入的 PCH。
- 上游: [`PCHContainerOperations.cpp`](PCHContainerOperations.cpp)。
- 关键类/函数: `ObjectFilePCHContainerReader`, `getFormats`,
  `ExtractPCH`。

### 3.6 Module Cache (on-disk + in-memory)

[`ModuleCache.cpp`](ModuleCache.cpp) — `CrossProcessModuleCache`:
带 advisory lock / timestamp / prune / atomic write-read 的磁盘
缓存。
- 上游:
  [`Frontend/CompilerInstance`](../Frontend/CompilerInstance.cpp)
  (construction)。
- 下游: [`InMemoryModuleCache.cpp`](InMemoryModuleCache.cpp),
  [`ModuleFile.cpp`](ModuleFile.cpp)。
- 关键类/函数: `ModuleCache`, `CrossProcessModuleCache`,
  `createCrossProcessModuleCache`, `maybePruneImpl`, `writeImpl`,
  `readImpl`, `getLock`。

[`InMemoryModuleCache.cpp`](InMemoryModuleCache.cpp) —
`InMemoryModuleCache`: 内存版缓存, 测试 / build 中不落盘场景。
- 上游: [`ModuleCache.cpp`](ModuleCache.cpp),
  [`ModuleManager.cpp`](ModuleManager.cpp)。
- 关键类/函数: `InMemoryModuleCache`, `addPCM`, `addBuiltPCM`,
  `lookupPCM`, `getPCMState`, `finalizePCM`, `tryToDropPCM`。

### 3.7 共享 helper / on-disk 结构 / type IDs

[`ASTCommon.h`](ASTCommon.h) — Reader / Writer 共享头: `DeclUpdateKind`
枚举, `TypeIdxFromBuiltin`, `getDefinitiveDeclContext`, anonymous
decl 编号。
- 上游: [`ASTReader.cpp`](ASTReader.cpp),
  [`ASTWriter.cpp`](ASTWriter.cpp)。
- 关键类/函数: `DeclUpdateKind`, `TypeIdxFromBuiltin`,
  `ComputeHash`, `getDefinitiveDeclContext`,
  `isRedeclarableDeclKind`, `needsAnonymousDeclarationNumber`,
  `isPartOfPerModuleInitializer`。

[`ASTCommon.cpp`](ASTCommon.cpp) — `ASTCommon.h` 的实现: builtin
TypeIdx 表, Selector hash, definitive-DeclContext 逻辑, listener
vtable ODR anchor。
- 上游: [`ASTReader.cpp`](ASTReader.cpp),
  [`ASTWriter.cpp`](ASTWriter.cpp)。
- 下游: `ASTDeserializationListener`。
- 关键类/函数: `TypeIdxFromBuiltin`, `ComputeHash`,
  `getDefinitiveDeclContext`, `isRedeclarableDeclKind`,
  `needsAnonymousDeclarationNumber`。

[`MultiOnDiskHashTable.h`](MultiOnDiskHashTable.h) —
`MultiOnDiskHashTable`: 多模块磁盘 hash 表, 跨模块合并条目 +
override semantics。
- 上游: [`ASTReader.cpp`](ASTReader.cpp),
  [`ASTWriter.cpp`](ASTWriter.cpp)。
- 下游: [`ASTReaderInternals.h`](ASTReaderInternals.h)。
- 关键类/函数: `MultiOnDiskHashTable`,
  `MultiOnDiskHashTableGenerator`, `add`, `find`, `findAll`,
  `condense`, `removeOverriddenTables`。

[`TemplateArgumentHasher.h`](TemplateArgumentHasher.h) — 声明
`StableHashForTemplateArguments`, 跨 TU 模板实参稳定 hash
(PCH 兼容性校验)。
- 上游: [`ASTReader.cpp`](ASTReader.cpp),
  [`ASTWriter.cpp`](ASTWriter.cpp)。
- 关键内容: `StableHashForTemplateArguments`。

[`TemplateArgumentHasher.cpp`](TemplateArgumentHasher.cpp) — 实
现稳定跨进程 hash, 含类型 / 名字 / Decl 引用。
- 上游: [`ASTReader.cpp`](ASTReader.cpp),
  [`ASTWriter.cpp`](ASTWriter.cpp)。
- 关键类/函数: `TemplateArgumentHasher`,
  `StableHashForTemplateArguments`, `AddTemplateArgument`,
  `AddStructuralValue`, `TypeVisitorHelper`,
  `AddTemplateName`。

[`CMakeLists.txt`](CMakeLists.txt) — `clangSerialization` 库定义:
列出 21 个 .cpp + 链接 `clangAST/Basic/Lex/Sema` + LLVM
`BitReader/BitstreamReader/Object`。

---

## §4. 关键调用链

### 4.1 PCH 生成链

```
Frontend 看到 GeneratePCHAction
  │
  ▼
generatePCHAction::CreateASTConsumer
  └─ return new PCHGenerator (GeneratePCH.cpp)
       │
       ▼
PCHGenerator::InitializeSema (GeneratePCH.cpp)
  ├─ 校验 HeaderSearchOptions / LangOpts 等
  └─ 准备 output file (raw / MachO-wrapped)
       │
       ▼
Sema 跑完整个 TU, 调 HandleTranslationUnit
  │
  ▼
PCHGenerator::HandleTranslationUnit (GeneratePCH.cpp)
  ├─ new ASTWriter (ASTWriter.cpp)
  └─ ASTWriter::WriteAST
       ├─ WriteControlBlock
       │    ├─ PCH_MAGIC / version / LangOptions hash
       │    └─ 依赖列表 (从 CompilerInstance 取 imports)
       ├─ WriteSourceManagerBlock
       │    ├─ line table (SourceManager 全部 line entries)
       │    └─ FileEntry hash table
       ├─ WritePreprocessor
       │    ├─ Macros (每个 MacroInfo 一条 record)
       │    ├─ HeaderSearch (每个 HeaderFileInfo)
       │    └─ PPCallbacks 状态
       ├─ WriteDeclsAndTypes
       │    ├─ WriteTypeTable (TypeIdx + Type::TypeClass)
       │    │    └─ 每个 Type 一条 record
       │    ├─ WriteDecls
       │    │    └─ ASTWriterDecl.cpp ASTDeclWriter::VisitXxx
       │    │         (每个 Decl kind 一条 record, ASTRecordWriter)
       │    ├─ WriteStmt
       │    │    └─ ASTWriterStmt.cpp ASTStmtWriter::VisitXxx
       │    └─ WriteIdentifiers + WriteSelectors (PP::IdentifierInfo)
       ├─ WriteSubmodules
       │    └─ Module hierarchy + umbrella + exports
       ├─ WriteLookups
       │    └─ MultiOnDiskHashTable (decl names / identifiers / selectors)
       └─ ModuleFileExtension 钩子 (custom blocks)
            │
            ▼
PCHContainerOperations::writePCH (PCHContainerOperations.cpp)
  ├─ RawPCHContainerWriter: 直接写 bitstream 到 .pch
  └─ 或 ObjectFilePCHContainerWriter: 把 bitstream 嵌到
       Mach-O/ELF/COFF object
            │
            ▼
ModuleCache::addPCMFile (ModuleCache.cpp)
  └─ 把 .pcm 写入 cross-process cache (with file lock)
```

### 4.2 PCH / PCM 读取链

```
Frontend 看到 ASTUnit::LoadFromASTFile 或
         CompilerInstance::loadModule
  │
  ▼
ModuleCache::getPCMFile (ModuleCache.cpp)
  ├─ 校验 signature / mtime
  ├─ 拿 advisory file lock
  └─ read buffer
       │
       ▼
ModuleManager::addModule (ModuleManager.cpp)
  ├─ 解析 PCHContainer
  │    ├─ RawPCHContainerReader (PCHContainerOperations.cpp)
  │    └─ 或 ObjectFilePCHContainerReader::ExtractPCH
  │         (ObjectFilePCHContainerReader.cpp, 走 llvm::object)
  ├─ 构造 ModuleFile (ModuleFile.cpp)
  │    ├─ BitstreamCursor
  │    ├─ 局部→全局 ID remap (ContinuousRangeMap)
  │    └─ 依赖列表 (Imports/ImportedBy)
  └─ 加入 ModuleManager 集合
       │
       ▼
ASTReader::ReadAST (ASTReader.cpp)
  ├─ ReadControlBlock
  │    ├─ 校验 PCH_MAGIC / version
  │    ├─ 校验 LangOptions hash 匹配
  │    └─ 校验依赖文件存在 + signature 匹配
  ├─ ReadSourceManagerBlock
  │    ├─ line table
  │    └─ FileEntry hash
  ├─ ReadPreprocessorBlock
  │    ├─ Macros (回到 Preprocessor 状态)
  │    └─ HeaderSearch 重建
  ├─ ReadDeclsAndTypes (lazy / on-demand)
  │    ├─ 类型懒加载 (按 ASTReader::CompleteType 触发)
  │    ├─ Decl 懒加载 (按 ASTReader::CompleteDecl 触发)
  │    │    └─ ASTReaderDecl.cpp ASTDeclReader::VisitXxx
  │    ├─ Stmt 懒加载 (按 ASTReader::CompleteStmt 触发)
  │    │    └─ ASTReaderStmt.cpp ASTStmtReader::VisitXxx
  │    └─ Identifier / Selector 懒加载
  ├─ ReadSubmodules + GlobalModuleIndex 注入
  └─ ModuleFileExtension 钩子 (custom blocks 还原)

后续 Sema 触发 lazy load:
  Sema::LookupName / QualType::getAs / ...
    └─ ExternalASTSource::CompleteDecl / CompleteType
         └─ ASTReader::CompleteDecl / CompleteType
              └─ 走对应 ASTDeclReader::VisitXxx / ASTStmtReader::VisitXxx
                   └─ 重建节点挂回 ASTContext
```

### 4.3 Module 加载链 (C++20 modules / clang modules)

```
Frontend 看到 @import Foo / import Foo / #include <foo>
  │
  ▼
ModuleLoader::loadModule (Lexer/ModuleLoader.h)
  └─ CompilerInstance::loadModule (Frontend)
       │
       ▼
ModuleManager::addModule (ModuleManager.cpp)
  ├─ 查 GlobalModuleIndex (GlobalModuleIndex.cpp)
  │    └─ modules.idx 中找 Foo 的模块路径
  ├─ ModuleCache::getPCMFile
  │    └─ 读 .pcm 缓存 (如果存在)
  ├─ 若缓存 miss:
  │    └─ spawn GenerateModuleAction 编译 foo.modulemap
  │         └─ PCHGenerator → ASTWriter → 写新 .pcm
  └─ PCHContainerOperations 解包 (raw / MachO)
       │
       ▼
ASTReader::ReadAST (ASTReader.cpp)
  └─ ... (同 §4.2 PCH 读取链)
       │
       ▼
Module 装入 ASTContext
  ├─ Foo module 的 decls 可被 lookup
  ├─ umbrellas / exports 按 ModuleMap 暴露
  └─ 触发 PPCallbacks (Lex/PP 重发 ModuleImport 事件)
```

### 4.4 GlobalModuleIndex 写入与查询链

```
写入 (build 阶段, 后台任务):

Builder:
  GlobalModuleIndexBuilder (GlobalModuleIndex.cpp)
    ├─ 遍历 module cache 目录
    ├─ 对每个 .pcm:
    │    ├─ ASTReader 浅读 (仅 metadata, 不读全部)
    │    │    └─ 读 identifier → module map
    │    └─ 累积到 in-memory 索引
    └─ writeIndex (写 modules.idx)
         └─ MultiOnDiskHashTable (按 identifier 分桶)

查询 (load 阶段):

Frontend::loadModule (Frontend/CompilerInstance.cpp)
  └─ ModuleManager 查 GlobalModuleIndex
       ├─ readIndex (GlobalModuleIndex.cpp)
       ├─ lookupIdentifier (identifier → 候选 module 列表)
       ├─ getModuleDependencies (候选 module 的依赖)
       └─ 选择最佳 module 加载
            └─ 进入 §4.3 Module 加载链
```

### 4.5 PCH 容器链 (raw vs Mach-O / ELF)

```
写:
  ASTWriter::WriteAST 完毕
    └─ Buffer 写到 output file (由 PCHContainerOperations::writePCH)
         ├─ RawPCHContainerWriter:
         │    └─ buffer → file (header + body)
         └─ ObjectFilePCHContainerWriter:
              └─ buffer → object section
                   ├─ Mach-O: __LLVM,__ast 段
                   ├─ ELF: .clang_ast 段
                   └─ COFF: .ast 段 (clang-cl)

读:
  ModuleManager::addModule
    └─ PCHContainerOperations::readPCHFile
         ├─ RawPCHContainerReader:
         │    └─ 整个文件 = bitstream
         └─ ObjectFilePCHContainerReader:
              └─ llvm::ObjectFile::createObjectFile
                   └─ getSection("__LLVM,__ast" / ".clang_ast" / ".ast")
                        └─ ExtractPCH 拿 buffer
```

### 4.6 ModuleFileExtension 钩子链

```
Front 端:
  Frontend::CompilerInstance 注册 extension
    └─ CompilerInvocation::ModuleFileExtensions.push_back(MyExt)

写:
  ASTWriter::WriteAST
    └─ 对每个 registered extension:
         ├─ createExtensionWriter
         │    └─ 在新 block 中写 extension 内容
         │         └─ hashExtension 算 signature
         └─ Writer 写完后 block 关闭

读:
  ASTReader::ReadAST
    └─ 遇到 extension block:
         ├─ getExtensionMetadata (读 signature)
         ├─ createExtensionReader
         └─ extension::Reader 读自定义内容

例子:
  clang/test/Serialization/module-file-extension.c
  + Frontend/TestModuleFileExtension.cpp (用于单测)
```

---

## §5. 推荐阅读顺序

### 阶段 1: bitstream 容器与扩展钩子 (1 小时)
- [`PCHContainerOperations.cpp`](PCHContainerOperations.cpp) —
  PCH container 注册表
- [`ObjectFilePCHContainerReader.cpp`](ObjectFilePCHContainerReader.cpp)
  — 从 Mach-O/ELF/COFF 抽 PCH
- [`ModuleFileExtension.cpp`](ModuleFileExtension.cpp) — 自定义块
  钩子

### 阶段 2: Module cache (1 小时)
- [`ModuleCache.cpp`](ModuleCache.cpp) — 磁盘缓存
- [`InMemoryModuleCache.cpp`](InMemoryModuleCache.cpp) — 内存缓存

### 阶段 3: Module file 数据模型 (2 小时)
- [`ModuleFile.cpp`](ModuleFile.cpp) — 单文件 in-memory 表示
- [`ModuleManager.cpp`](ModuleManager.cpp) — 依赖图 + 签名校验
- [`GlobalModuleIndex.cpp`](GlobalModuleIndex.cpp) — 跨模块索引

### 阶段 4: 共享序列化原语 (1.5 小时)
- [`ASTCommon.h`](ASTCommon.h) + [`ASTCommon.cpp`](ASTCommon.cpp)
  — 共享 helper
- [`MultiOnDiskHashTable.h`](MultiOnDiskHashTable.h) — 多模块 hash 表
- [`TemplateArgumentHasher.h`](TemplateArgumentHasher.h) +
  [`TemplateArgumentHasher.cpp`](TemplateArgumentHasher.cpp) —
  模板参数稳定 hash

### 阶段 5: ASTReader 反序列化 (3 小时)
- [`ASTReader.cpp`](ASTReader.cpp) — 顶层入口 + ExternalASTSource
- [`ASTReaderInternals.h`](ASTReaderInternals.h) — 查找 trait 类

### 阶段 6: per-node 反序列化 (2 小时)
- [`ASTReaderDecl.cpp`](ASTReaderDecl.cpp) — Decl 节点还原
- [`ASTReaderStmt.cpp`](ASTReaderStmt.cpp) — Stmt 节点还原

### 阶段 7: ASTWriter 序列化 (2 小时)
- [`ASTWriter.cpp`](ASTWriter.cpp) — 顶层入口

### 阶段 8: per-node 序列化 + driver (2 小时)
- [`ASTWriterDecl.cpp`](ASTWriterDecl.cpp) — Decl 节点序列化
- [`ASTWriterStmt.cpp`](ASTWriterStmt.cpp) — Stmt 节点序列化
- [`GeneratePCH.cpp`](GeneratePCH.cpp) — `SemaConsumer` driver

---

## §6. 自定义扩展指南

### 6.1 添加新 PCH 容器格式

1. 派生 `PCHContainerWriter` / `PCHContainerReader` (来自
   [`PCHContainerOperations.h`](../../include/clang/Serialization/PCHContainerOperations.h))。
2. 在
   [`PCHContainerOperations.cpp`](PCHContainerOperations.cpp)
   的 `PCHContainerOperations` 构造里 `registerWriter` /
   `registerReader`。
3. 在
   [`Frontend/CompilerInstance.cpp`](../Frontend/CompilerInstance.cpp)
   装入新容器 (经 `setPCHContainerOperations`)。
4. 测试: `clang/test/PCH/xxx-container.c`。

### 6.2 添加新 `ModuleFileExtension`

1. 派生
   [`ModuleFileExtension`](../../include/clang/Serialization/ModuleFileExtension.h)
   (或直接继承默认实现), 实现 `createExtensionReader` /
   `createExtensionWriter` / `getExtensionMetadata` /
   `hashExtension`。
2. 在
   [`Frontend/CompilerInstance.cpp`](../Frontend/CompilerInstance.cpp)
   或用户代码中:
   ```cpp
   CompilerInstance::getFrontendOpts().ModuleFileExtensions.push_back(
       new MyExtension());
   ```
3. 测试: 参考
   [`Frontend/TestModuleFileExtension.cpp`](../Frontend/TestModuleFileExtension.cpp)
   (用于单测) +
   `clang/unittests/Serialization/ModuleFileExtensionTest.cpp`。

### 6.3 添加新 Module cache 后端

1. 派生 `ModuleCache` (来自
   [`ModuleCache.h`](../../include/clang/Serialization/ModuleCache.h))。
2. 实现 `maybePruneImpl` / `writeImpl` / `readImpl` / `getLock`。
3. 在 [`Frontend/CompilerInstance.cpp`](../Frontend/CompilerInstance.cpp)
   装入。
4. 测试: `clang/test/Modules/xxx-cache.c`。

### 6.4 添加新 on-disk hash 表 trait

1. 在 [`ASTReaderInternals.h`](ASTReaderInternals.h) 仿照
   `ASTIdentifierLookupTrait` 写新 trait。
2. 在 [`ASTReader.cpp`](ASTReader.cpp) 的 lookup 表构造处用新
   trait。
3. 测试: 在 [`ASTReader.cpp`](ASTReader.cpp) 加 unit test。

### 6.5 调试 PCH / PCM 行为

1. 用 `clang -Xclang -ast-dump -Xclang -ast-dump-filter=foo foo.c`
   + `-include foo.h` 看 PCH 反序列化后的 AST。
2. 用 `clang -Xclang -print-stats` 看 Serialization 统计。
3. 用 `clang -Xclang -module-file-info foo.pcm` 看 PCM metadata。
4. 用 `llvm-bcanalyzer -dump foo.pch` 看 bitstream 结构。
5. 用 `clang -Xclang -read-loaded-modules` (调试模式下) 看当前
   加载了哪些 module。
6. 临时在 [`ASTReader.cpp`](ASTReader.cpp) 的 `ReadASTCore` 加
   `llvm::errs() << ...` trace 加载流程。

### 6.6 调试模块缓存

1. 看 module cache 目录: `~/.cache/clang/ModuleCache/`
2. 用 `clang -fmodules-cache-path=...` 指定 cache 位置。
3. 用 `clang -fdisable-module-hash` 关 signature 校验。
4. 用 `clang -fmodules-validate-system-headers` 启用 system header
   校验。
5. 用 `clang -Xclang -debug-pch` 看 PCH 校验日志。
6. 删除整个 cache 目录强制 rebuild。

---

## §7. NT 注释索引

当前 `clang/lib/Serialization/` 下尚无 `// <NT>` 注释。已建立目
录索引, 姊妹 overview:

- [`clang/lib/AST/0-overview.md`](../AST/0-overview.md) — Clang AST
  层
- [`clang/lib/Basic/0-overview.md`](../Basic/0-overview.md) — Clang 基
  础设置 + 共享数据
- [`clang/lib/CodeGen/0-overview.md`](../CodeGen/0-overview.md) —
  Clang AST → LLVM IR
- [`clang/lib/Driver/0-overview.md`](../Driver/0-overview.md) — Clang
  编译驱动
- [`clang/lib/Frontend/0-overview.md`](../Frontend/0-overview.md) —
  Clang 前端桥接
- [`clang/lib/Lex/0-overview.md`](../Lex/0-overview.md) — Clang 词
  法 + 预处理器
- [`clang/lib/Parse/0-overview.md`](../Parse/0-overview.md) — Clang
  语法分析
- [`clang/lib/Sema/0-overview.md`](../Sema/0-overview.md) — Clang 语义
  分析

按 "少而精" 原则, 加 NT 注释建议优先级:

1. [`ASTWriter.cpp`](ASTWriter.cpp) — 8-12 段 (顶层 AST 序列化
   入口)
2. [`ASTReader.cpp`](ASTReader.cpp) — 8-12 段 (顶层反序列化 + 
   ExternalASTSource)
3. [`ASTWriterDecl.cpp`](ASTWriterDecl.cpp) — 5-8 段 (Decl 节点
   序列化样板)
4. [`ASTReaderDecl.cpp`](ASTReaderDecl.cpp) — 5-8 段 (Decl 节点
   反序列化样板)
5. [`ModuleManager.cpp`](ModuleManager.cpp) — 5-8 段 (依赖图 +
   签名校验)
6. [`GlobalModuleIndex.cpp`](GlobalModuleIndex.cpp) — 4-6 段
   (跨模块索引)
7. [`ModuleCache.cpp`](ModuleCache.cpp) — 4-6 段 (磁盘缓存 +
   lock)
8. [`GeneratePCH.cpp`](GeneratePCH.cpp) — 4-6 段 (`SemaConsumer`
   driver)
9. [`ASTWriterStmt.cpp`](ASTWriterStmt.cpp) + [`ASTReaderStmt.cpp`](ASTReaderStmt.cpp)
   — 各 3-5 段
10. [`ASTCommon.cpp`](ASTCommon.cpp) — 3-5 段 (共享 helper)
11. [`PCHContainerOperations.cpp`](PCHContainerOperations.cpp) +
    [`ObjectFilePCHContainerReader.cpp`](ObjectFilePCHContainerReader.cpp)
    — 各 3-4 段
12. [`TemplateArgumentHasher.cpp`](TemplateArgumentHasher.cpp) —
    3-4 段
13. [`ModuleFile.cpp`](ModuleFile.cpp) +
    [`ModuleFileExtension.cpp`](ModuleFileExtension.cpp) +
    [`InMemoryModuleCache.cpp`](InMemoryModuleCache.cpp) — 各
    2-3 段

---

**姊妹文档**: 本目录对应 LLVM 流水线中的 **Clang AST 持久化层**,
是 `.pch` / `.pcm` / `.ast` 的产生与消费方。它与
[`clang/lib/AST/`](../AST/0-overview.md) 配合序列化 Decl / Type /
Stmt 节点, 与 [`clang/lib/Lex/`](../Lex/0-overview.md) 配合序列化
Preprocessor 状态, 与 [`clang/lib/Sema/`](../Sema/0-overview.md)
配合通过 `ExternalASTSource` 提供按需加载, 与
[`clang/lib/Frontend/`](../Frontend/0-overview.md) 配合被
`GeneratePCHAction` / `GenerateModuleAction` 驱动, 与
[`clang/lib/Basic/`](../Basic/0-overview.md) 配合序列化
SourceManager / IdentifierTable 等基础对象。Serialization 自身不
实现编译流水线, 仅作为 AST 持久化与跨进程共享层。
