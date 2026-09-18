<!-- <NT>overview:clang/lib/AST/ -->

# Clang AST 库导读 — `clang/lib/AST/`

> 本文档梳理 `clang/lib/AST/` 目录下所有源文件 (94 个顶层
> .cpp/.h/.txt + 64 个 `ByteCode/` 子目录 .cpp/.h/.td = 158 个) 的职
> 责、上下游与推荐阅读顺序。
>
> 目标读者: 想理解 **Clang 完全类型化 AST 层** (Sema 之后、CodeGen 之
> 前, 含 constexpr 字节码解释器、name mangling、record layout、ODR
> 检查) 的开发者, 以及要给 Clang 加新 AST 节点 / 新 C++ 特性 / 新
> constexpr builtin 的人。
>
> 所有路径相对 `clang/lib/AST/`。同名公开头文件位于
> `clang/include/clang/AST/` (如 `ASTContext.h`、`Decl.h`、`Expr.h`、
> `Type.h`、`Stmt.h`、`Attr.h` 等); TableGen 源文件全部在
> `clang/include/clang/AST/*.td`, 由 `clang-tblgen` 生成 `.inc` 后被
> 本目录 `.cpp` 文件 `#include`。`ByteCode/Opcodes.td` 是本目录中唯一
> 嵌入的 TableGen 源。

---

## §0. Clang AST 库在编译流水线中的位置

`clang/lib/AST` 是 Clang 的 **完全类型化 AST 层**, 位于 Sema 之后、
CodeGen 之前。它消费 Sema 已类型检查的 AST, 提供:
- AST 节点生命周期管理 (`ASTContext`)；
- Layout 计算 (record layout / vtable / VTT)；
- C++ ABI 与 name mangling (Itanium / Microsoft)；
- ODR 检查与跨 TU AST 导入 (clangd/clang-tidy 用)；
- **constexpr 字节码解释器** (`ByteCode/` 子目录, 完整栈式 VM)；
- 旧版 AST-walker 常量求值器 (`ExprConstant.cpp`, 逐步被 ByteCode
  替换)；
- 注释/属性/HLSL/格式串分析等。

它在 Clang 内部的层次:

```
┌─────────────────────────────────────────────────────────┐
│ clang/lib/Sema  (语义分析, 输出完整类型化 AST)           │
└─────────────────────────────────────────────────────────┘
                │
                ▼
┌─────────────────────────────────────────────────────────┐
│ clang/lib/AST  (本目录) ← 你在这里                       │
│   · ASTContext.cpp: 类型/Decl 唯一化 + 内存管理          │
│   · Decl*.cpp / Expr*.cpp / Stmt*.cpp: AST 节点实现      │
│   · Type.cpp / TypeLoc.cpp / TypePrinter.cpp /           │
│     NestedNameSpecifier.cpp / TemplateBase/Name.cpp:    │
│     类型系统                                              │
│   · Mangle.cpp / ItaniumMangle.cpp /                    │
│     MicrosoftMangle.cpp: name mangling                   │
│   · ItaniumCXXABI.cpp / MicrosoftCXXABI.cpp: C++ ABI    │
│   · RecordLayoutBuilder.cpp / RecordLayout.cpp:          │
│     字段布局                                              │
│   · VTableBuilder.cpp / VTTBuilder.cpp: vtable / VTT    │
│   · ODRHash.cpp / ODRDiagsEmitter.cpp: ODR 检查         │
│   · ASTImporter.cpp / ASTStructuralEquivalence.cpp /     │
│     ExternalASTMerger.cpp: 跨 TU 合并 (clangd 用)        │
│   · ExprConstant.cpp: 旧 AST-walker constexpr 求值      │
│   · ByteCode/: 新字节码解释器 (栈式 VM + 字节码编译器) │
│   · RawCommentList.cpp / Comment*.cpp: Doxygen 注释    │
│   · AttrImpl.cpp / OSLog.cpp / FormatString*.cpp:       │
│     属性 + 平台 builtin + 格式串分析                    │
│   · DynamicRecursiveASTVisitor.cpp / ParentMap*.cpp:    │
│     AST walker                                          │
│   · TextNodeDumper.cpp / JSONNodeDumper.cpp: dump 格式  │
└─────────────────────────────────────────────────────────┘
                │
                ├─► clang/lib/CodeGen  (消费完整 AST, 输出 LLVM IR)
                ├─► clang/lib/Serialization (PCH / Module 序列化)
                ├─► clang/lib/StaticAnalyzer (CFG-based analysis)
                └─► libclang / clangd / clang-tidy (tooling)
```

### §0.1 公开接口 (`clang/include/clang/AST/`)

本目录 `.cpp` 文件依赖的 **公开头** 在
[`clang/include/clang/AST/`](../../include/clang/AST/), 最关键的:

- [`ASTContext.h`](../../include/clang/AST/ASTContext.h) — `class
  ASTContext` AST 顶层容器。
- [`Decl.h`](../../include/clang/AST/Decl.h) + [`DeclCXX.h`](../../include/clang/AST/DeclCXX.h) +
  [`DeclTemplate.h`](../../include/clang/AST/DeclTemplate.h) +
  [`DeclObjC.h`](../../include/clang/AST/DeclObjC.h) +
  [`DeclBase.h`](../../include/clang/AST/DeclBase.h) — Decl 体系。
- [`Expr.h`](../../include/clang/AST/Expr.h) +
  [`ExprCXX.h`](../../include/clang/AST/ExprCXX.h) +
  [`ExprObjC.h`](../../include/clang/AST/ExprObjC.h) +
  [`ExprConcepts.h`](../../include/clang/AST/ExprConcepts.h) — Expr 体系。
- [`Stmt.h`](../../include/clang/AST/Stmt.h) +
  [`StmtCXX.h`](../../include/clang/AST/StmtCXX.h) +
  [`StmtObjC.h`](../../include/clang/AST/StmtObjC.h) +
  [`StmtOpenMP.h`](../../include/clang/AST/StmtOpenMP.h) +
  [`StmtOpenACC.h`](../../include/clang/AST/StmtOpenACC.h) — Stmt 体系。
- [`Type.h`](../../include/clang/AST/Type.h) +
  [`TypeLoc.h`](../../include/clang/AST/TypeLoc.h) +
  [`NestedNameSpecifier.h`](../../include/clang/AST/NestedNameSpecifier.h) +
  [`TemplateName.h`](../../include/clang/AST/TemplateName.h) +
  [`TemplateBase.h`](../../include/clang/AST/TemplateBase.h) — 类型系统。
- [`Attr.h`](../../include/clang/AST/Attr.h) +
  [`Comment.h`](../../include/clang/AST/Comment.h) +
  [`RawCommentList.h`](../../include/clang/AST/RawCommentList.h) — 属性 + 注释。
- [`Mangle.h`](../../include/clang/AST/Mangle.h) +
  [`CXXABI.h`](../../include/clang/AST/CXXABI.h) +
  [`RecordLayout.h`](../../include/clang/AST/RecordLayout.h) +
  [`VTableBuilder.h`](../../include/clang/AST/VTableBuilder.h) +
  [`VTTBuilder.h`](../../include/clang/AST/VTTBuilder.h) — C++ ABI。
- [`APValue.h`](../../include/clang/AST/APValue.h) +
  [`ASTConcept.h`](../../include/clang/AST/ASTConcept.h) +
  [`ComparisonCategories.h`](../../include/clang/AST/ComparisonCategories.h)
  — 常量值 + C++20 concepts。
- [`ASTTypeTraits.h`](../../include/clang/AST/ASTTypeTraits.h) +
  [`ParentMapContext.h`](../../include/clang/AST/ParentMapContext.h) +
  [`RecursiveASTVisitor.h`](../../include/clang/AST/RecursiveASTVisitor.h)
  — AST walker 框架。

### §0.2 与 [`clang/lib/Sema/`](../Sema/) 的关系

Sema 调用 AST 节点 **Create 工厂** (`FooDecl::Create`、
`FooExpr::Create` 等) 建节点、然后经
[`ASTContext`](../../include/clang/AST/ASTContext.h) 唯一化。AST 层
**不感知 Sema**, 但提供所有底座类。

### §0.3 与 [`clang/lib/CodeGen/`](../CodeGen/0-overview.md) 的关系

CodeGen 是 AST 的主要消费者, 用 `ASTContext::getTypes()` /
`RecordLayout` / `MangleContext` / `vtable` / `APValue` 生成 LLVM IR。
CodeGen **只读** AST, 不修改。

### §0.4 与 [`clang/lib/Serialization/`](../Serialization/) 的关系

Serialization (PCH / Module 文件) 写 / 读 AST 节点到磁盘。AST 层提供
`ExternalASTSource` 接口让 AST 可延迟加载。

### §0.5 与 libclang / clangd / clang-tidy 的关系

这些工具直接消费 AST:
- `ASTDumper` + `JSONNodeDumper` (IDE 用);
- `ASTImporter` + `ASTStructuralEquivalence` (clangd 跨文件);
- `ExternalASTMerger` (clangd 跨 TU 合并);
- `RecursiveASTVisitor` (AST matcher 基础);
- `ODRHash` (clang-tidy 跨 TU 警告)。

---

## §1. 编译流水线概览

```
Sema (SemaDecl / SemaExpr / SemaStmt / SemaConcept / ...)
  └─ 创建 / 修改 AST 节点 (Decl::Create / Expr::Create)
       └─ ASTContext::get<Type>(unique 化)
                │
                ▼
┌─────────────────────────────────────────────────────────┐
│ clang/lib/AST  (本目录) — 完整类型化 AST 层              │
│   1. AST 节点 (Decl/Type/Expr/Stmt) 实现                  │
│   2. ASTContext (类型/Decl 唯一化, 内存管理)               │
│   3. Layout / vtable / C++ ABI / mangling               │
│   4. ODRHash / ASTImporter / ExternalASTSource          │
│   5. constexpr 字节码解释器 (ByteCode/)                   │
│   6. constexpr 旧求值器 (ExprConstant)                   │
│   7. Dump/Print/Walker/Comment/Attr/FormatString       │
└─────────────────────────────────────────────────────────┘
                │
                ├─► clang/lib/CodeGen  (LLVM IR emission)
                ├─► clang/lib/Serialization (PCH/Module 写读)
                ├─► clang/lib/StaticAnalyzer (CFG/Analysis)
                └─► libclang / clangd / clang-tidy
                     │
                     ▼
                完全类型化 AST → LLVM IR (clang/lib/CodeGen/)
```

辅助入口:

- **公开 API**: [`clang/include/clang/AST/`](../../include/clang/AST/)
  (每个 `.cpp` 的同名 `.h`)。
- **TableGen 源**: [`clang/include/clang/AST/*.td`](../../include/clang/AST/)
  (`DeclNodes.td`、`StmtNodes.td`、`TypeNodes.td`、`CommentNodes.td`、
  `Attr.td`、`OpenMPClause.h` 等), `clang-tblgen` 生成 `.inc` 后被
  `.cpp` 直接 `#include`。
- **constexpr VM**: `ByteCode/` 子目录 — 完整栈式字节码解释器, 64 个
  文件。

---

## §2. 文件目录结构

```
clang/lib/AST/  (158 文件, 1 子目录 ByteCode/)
├── §3.1  Core AST context & utility                  (9 文件)
├── §3.2  Declaration (Decl) 体系                     (12 文件)
├── §3.3  Type & TypeLoc 体系                         (6 文件)
├── §3.4  Expr & Stmt 体系                            (15 文件)
├── §3.5  Constexpr 求值 (旧 AST-walker)              (4 文件)
├── §3.6  C++ ABI / Mangling / Layout                 (11 文件)
├── §3.7  Attribute / Comment / Platform 扩展        (12 文件)
├── §3.8  Format string 分析                          (4 文件)
├── §3.9  Helper / analysis / AST walker              (16 文件)
├── §3.10 Dump / Print                                (3 文件)
├── §3.11 Build / misc                                (1 文件 CMakeLists.txt)
└── §3.12 ByteCode/ 子目录                            (64 文件)
      ├── 解释器核心 (Context/State/Program)
      ├── 字节码编译器 (Compiler/ByteCodeEmitter/EvalEmitter)
      ├── 内存模型 (Pointer/Block/Record/Descriptor)
      ├── 调用栈与栈实现 (Frame/InterpStack)
      ├── 原始类型包装 (Integral/Floating/Boolean/Char/...)
      └── 字节码调度 (Interp/Opcode/Source/Builtin)
```

### 2.1 文件数量统计

| 区域 | .cpp | .h | .td/.txt | 合计 |
|------|------|-----|----------|------|
| Core AST context & utility | 9 | 0 | 0 | 9 |
| Declaration (Decl) | 12 | 0 | 0 | 12 |
| Type & TypeLoc | 6 | 0 | 0 | 6 |
| Expr & Stmt | 14 | 0 | 0 | 14 |
| Constexpr 旧求值 | 4 | 0 | 0 | 4 |
| C++ ABI / Mangling / Layout | 9 | 1 | 0 | 10 |
| Attribute / Comment / Platform | 12 | 0 | 0 | 12 |
| Format string | 4 | 0 | 0 | 4 |
| Helper / analysis / walker | 16 | 0 | 0 | 16 |
| Dump / Print | 2 | 0 | 0 | 2 |
| Build | 0 | 0 | 1 | 1 |
| ByteCode 子目录 | 41 | 22 | 1 | 64 |
| **总计** | **~119** | **~23** | **~2** | **~158** |

---

## §3. 文件详解

### 3.1 Core AST context & utility

[`ASTContext.cpp`](ASTContext.cpp) — `ASTContext` 主实现: 类型/声
明 unique 化、内存分配器、源位置映射、目标信息集成。
- 上游: [`Sema/*.cpp`](../Sema/)、[`Parse/*.cpp`](../Parse/)、
  [`Clang Frontend`](../Frontend/)。
- 下游: [`ByteCode/Context.h`](ByteCode/Context.h),
  [`CXXABI.h`](CXXABI.h)、[`Linkage.h`](Linkage.h)、
  [`ASTContext.h`](../../include/clang/AST/ASTContext.h)。
- 关键类/函数: `class ASTContext`, `ASTContext::get<Type>`,
  `ASTContext::Allocate`, `getTypeOfType`, `getTranslationUnitDecl`。

[`ASTConsumer.cpp`](ASTConsumer.cpp) — `ASTConsumer` 默认实现: 仅
空操作, 通过接口传递顶层 Decl 给上层 (CodeGen 等)。
- 上游: [`CompilerInstance.cpp`](../Frontend/CompilerInstance.cpp)、
  [`ModuleBuilder.cpp`](../CodeGen/ModuleBuilder.cpp)。
- 下游: [`ASTConsumer.h`](../../include/clang/AST/ASTConsumer.h)、
  [`Decl.h`](../../include/clang/AST/Decl.h)。
- 关键类/函数: `class ASTConsumer`, `HandleTopLevelDecl`,
  `HandleInterestingDecl`, `HandleImplicitImportDecl`。

[`ASTConcept.cpp`](ASTConcept.cpp) — C++20 concepts unsatisfied
-constraint 记录与诊断数据结构。
- 上游: [`Sema/SemaConcept.cpp`](../Sema/SemaConcept.cpp)、
  [`CodeGenModule.cpp`](../CodeGen/CodeGenModule.cpp)。
- 下游: [`ASTConcept.h`](../../include/clang/AST/ASTConcept.h)、
  [`ExprConcepts.h`](../../include/clang/AST/ExprConcepts.h)、
  [`NestedNameSpecifier.h`](../../include/clang/AST/NestedNameSpecifier.h)。
- 关键类/函数: `ASTContext::getUnsatisfiedConstraints`,
  `UnsatisfiedConstraintRecord`,
  `CreateUnsatisfiedConstraintRecord`。

[`ASTDiagnostic.cpp`](ASTDiagnostic.cpp) — 为诊断格式化器提供 AST 节
点 (类型/表达式/声明) 的 pretty-print 钩子。
- 上游: [`Diagnostic.cpp`](../Basic/Diagnostic.cpp)、
  [`Sema.cpp`](../Sema/Sema.cpp)。
- 下游: [`ASTDiagnostic.h`](../../include/clang/AST/ASTDiagnostic.h)、
  [`ASTContext.h`](../../include/clang/AST/ASTContext.h)、
  [`TemplateBase.h`](../../include/clang/AST/TemplateBase.h)。
- 关键类/函数: `DiagnosticBuilder::AddASTTypeSource`, `Desugar`,
  `formatTemplateArg`。

[`ASTDumper.cpp`](ASTDumper.cpp) — `Decl/Stmt/Type::dump()` 实现;
人类可读的 AST 转储 (调度 `TextNodeDumper` 或 `JSONNodeDumper`)。
- 上游: [`TextNodeDumper.cpp`](TextNodeDumper.cpp)、
  `clang -ast-dump` driver。
- 下游: [`ASTDumper.h`](../../include/clang/AST/ASTDumper.h)、
  [`JSONNodeDumper.h`](../../include/clang/AST/JSONNodeDumper.h)、
  [`ASTContext.h`](../../include/clang/AST/ASTContext.h)。
- 关键类/函数: `ASTDumper::dumpDecl`, `ASTDumper::dumpStmt`,
  `ASTDumper::dumpType`, `showColorsForStream`。

[`ASTImporter.cpp`](ASTImporter.cpp) — 把另一个 `ASTContext` 中的
Decl/Type/Expr 导入到当前上下文 (跨 TU/clangd/clang-tidy 用)。
- 上游: 调用方如 Tooling/ASTDiff、Clangd。
- 下游: [`ASTStructuralEquivalence.h`](../../include/clang/AST/ASTStructuralEquivalence.h)、
  [`ASTContext.h`](../../include/clang/AST/ASTContext.h)、
  [`Decl.h`](../../include/clang/AST/Decl.h)。
- 关键类/函数: `class ASTImporter`, `Import`,
  `ASTImporterSharedState`, `ImportDecl`。

[`ASTImporterLookupTable.cpp`](ASTImporterLookupTable.cpp) —
`ASTImporter` 的高效名字查找表 (按 `DeclarationName` 索引源 AST 的
Decl)。
- 上游: [`ASTImporter.cpp`](ASTImporter.cpp)。
- 下游: [`RecursiveASTVisitor.h`](../../include/clang/AST/RecursiveASTVisitor.h)、
  [`Decl.h`](../../include/clang/AST/Decl.h)。
- 关键类/函数: `class ASTImporterLookupTable`,
  `Builder : RecursiveASTVisitor<Builder>`。

[`ASTStructuralEquivalence.cpp`](ASTStructuralEquivalence.cpp) — 判
断两个 Decl/Type/Stmt 节点结构上是否等价 (模板实例化、ODR 检查的核
心)。
- 上游: [`ASTImporter.cpp`](ASTImporter.cpp)、
  [`ODRDiagsEmitter.cpp`](ODRDiagsEmitter.cpp)、
  [`Sema/SemaTemplate.cpp`](../Sema/SemaTemplate.cpp)。
- 下游: [`ASTStructuralEquivalence.h`](../../include/clang/AST/ASTStructuralEquivalence.h)、
  [`Decl.h`](../../include/clang/AST/Decl.h)、
  [`Type.h`](../../include/clang/AST/Type.h)。
- 关键类/函数: `class StructuralEquivalenceContext`, `IsEquivalent`,
  `FinishFlags`。

[`ASTTypeTraits.cpp`](ASTTypeTraits.cpp) — `ASTNodeKind` 运行时类型
标识表 (所有 Decl/Stmt/Type/Attr 的 kind 元数据)。
- 上游: Clang Tooling (AST matchers)、clangd。
- 下游: [`ASTTypeTraits.h`](../../include/clang/AST/ASTTypeTraits.h)、
  `DeclNodes.inc`、`StmtNodes.inc`、`TypeNodes.inc` (TableGen 生成)。
- 关键类/函数: `class ASTNodeKind`, `class DynTypedNode`,
  `isBaseOf`, `getMostDerivedType`, `AllKindInfo`。

### 3.2 Declaration (Decl) 体系

[`DeclBase.cpp`](DeclBase.cpp) — `Decl` / `DeclContext` 基类的创建、
查找链、序列化与外部源集成。
- 上游: [`Sema/DeclSpec.cpp`](../Sema/DeclSpec.cpp)、
  [`Sema/SemaDecl.cpp`](../Sema/SemaDecl.cpp)、所有具体 Decl 的
  Create。
- 下游: [`DeclBase.h`](../../include/clang/AST/DeclBase.h)、
  [`DeclContextInternals.h`](../../include/clang/AST/DeclContextInternals.h)、
  [`ExternalASTSource.h`](../../include/clang/AST/ExternalASTSource.h)。
- 关键类/函数: `Decl::Create`, `DeclContext::lookup`,
  `DeclContext::addDecl`, `ExternalASTSource`。

[`Decl.cpp`](Decl.cpp) — 通用 Decl 子类 (`Named`/`Value`/`Declarator`/
`Tag` 等) 实现, 含链接性/可见性。
- 上游: [`Sema/SemaDecl.cpp`](../Sema/SemaDecl.cpp)。
- 下游: [`Linkage.h`](Linkage.h)、
  [`Decl.h`](../../include/clang/AST/Decl.h)、
  [`ODRHash.h`](../../include/clang/AST/ODRHash.h)、
  [`PrettyDeclStackTrace.h`](../../include/clang/AST/PrettyDeclStackTrace.h)。
- 关键类/函数: `NamedDecl`, `ValueDecl`, `DeclaratorDecl`,
  `getLinkageInternal`, `isLinkageValid`。

[`DeclCXX.cpp`](DeclCXX.cpp) — C++ Decl (`CXXRecord`/`Method`/
`Constructor`/`Destructor`/`AccessSpec` 等) 实现。
- 上游: [`Sema/SemaDeclCXX.cpp`](../Sema/SemaDeclCXX.cpp)。
- 下游: [`DeclCXX.h`](../../include/clang/AST/DeclCXX.h)、
  [`CXXInheritance.h`](../../include/clang/AST/CXXInheritance.h)、
  [`ODRHash.h`](../../include/clang/AST/ODRHash.h)、
  [`LambdaCapture.h`](../../include/clang/AST/LambdaCapture.h)。
- 关键类/函数: `CXXRecordDecl`, `CXXMethodDecl`,
  `CXXConstructorDecl`, `CXXDestructorDecl`, `getODRHashInfo`。

[`DeclFriend.cpp`](DeclFriend.cpp) — `FriendDecl` (C++ 友元) 实现,
含延迟加载的友元链。
- 上游: [`DeclCXX.cpp`](DeclCXX.cpp)、
  [`Sema/SemaDeclCXX.cpp`](../Sema/SemaDeclCXX.cpp)。
- 下游: [`DeclFriend.h`](../../include/clang/AST/DeclFriend.h)、
  [`ExternalASTSource.h`](../../include/clang/AST/ExternalASTSource.h)。
- 关键类/函数: `FriendDecl::Create`, `getNextFriendSlowCase`,
  `CXXRecordDecl::loadLazyFriends`。

[`DeclGroup.cpp`](DeclGroup.cpp) — `DeclGroupRef` / `DeclGroup` 存储
与创建 (多于一个 decl 的组)。
- 上游: [`Parser/ParseDecl.cpp`](../Parse/ParseDecl.cpp)、
  [`ASTConsumer.cpp`](ASTConsumer.cpp)。
- 下游: [`DeclGroup.h`](../../include/clang/AST/DeclGroup.h)、
  [`ASTContext.h`](../../include/clang/AST/ASTContext.h)。
- 关键类/函数: `DeclGroup::Create`, `DeclGroup`, `DeclGroupRef`。

[`DeclObjC.cpp`](DeclObjC.cpp) — ObjC Decl
(`Interface`/`Implementation`/`Property`/`Method`/`Category`) 全部实现。
- 上游: [`Sema/SemaDeclObjC.cpp`](../Sema/SemaDeclObjC.cpp)。
- 下游: [`DeclObjC.h`](../../include/clang/AST/DeclObjC.h)、
  [`ODRHash.h`](../../include/clang/AST/ODRHash.h)、
  [`Attr.h`](../../include/clang/AST/Attr.h)。
- 关键类/函数: `ObjCInterfaceDecl`, `ObjCImplementationDecl`,
  `ObjCMethodDecl`, `ObjCPropertyDecl`。

[`DeclOpenACC.cpp`](DeclOpenACC.cpp) — OpenACC Decl 子类
(`DeclareDecl`、`RoutineDecl` 等) 实现。
- 上游: [`Sema/SemaOpenACC.cpp`](../Sema/SemaOpenACC.cpp)。
- 下游: [`DeclOpenACC.h`](../../include/clang/AST/DeclOpenACC.h)、
  [`OpenACCClause.h`](../../include/clang/AST/OpenACCClause.h)。
- 关键类/函数: `OpenACCDeclareDecl`, `OpenACCRoutineDecl`,
  `classofKind`。

[`DeclOpenMP.cpp`](DeclOpenMP.cpp) — OpenMP Decl
(`ThreadPrivate`/`CapturedExpr`/`DeclareReduction` 等) 实现。
- 上游: [`Sema/SemaOpenMP.cpp`](../Sema/SemaOpenMP.cpp)。
- 下游: [`DeclOpenMP.h`](../../include/clang/AST/DeclOpenMP.h)、
  [`Expr.h`](../../include/clang/AST/Expr.h)。
- 关键类/函数: `OMPThreadPrivateDecl`, `OMPCapturedExprDecl`,
  `OMPDeclareReductionDecl`。

[`DeclPrinter.cpp`](DeclPrinter.cpp) — `Decl::print()` — 把 Decl 反
向 pretty-print 成 C/C++/ObjC 源码。
- 上游: Clang Serialization、Clang Tooling。
- 下游: [`DeclVisitor.h`](../../include/clang/AST/DeclVisitor.h)、
  [`PrettyPrinter.h`](../../include/clang/AST/PrettyPrinter.h)、
  [`Attr.h`](../../include/clang/AST/Attr.h)。
- 关键类/函数: `class DeclPrinter`, `VisitNamedDecl`,
  `VisitCXXRecordDecl`, `VisitFunctionDecl`。

[`DeclTemplate.cpp`](DeclTemplate.cpp) — C++ 模板 Decl
(`TemplateDecl`/`Specialization`/`TemplateParam` 等) 实现。
- 上游: [`Sema/SemaTemplate.cpp`](../Sema/SemaTemplate.cpp)。
- 下游: [`DeclTemplate.h`](../../include/clang/AST/DeclTemplate.h)、
  [`ODRHash.h`](../../include/clang/AST/ODRHash.h)、
  [`TemplateBase.h`](../../include/clang/AST/TemplateBase.h)、
  [`ExternalASTSource.h`](../../include/clang/AST/ExternalASTSource.h)。
- 关键类/函数: `TemplateDecl`, `ClassTemplateSpecializationDecl`,
  `FunctionTemplateDecl`, `TemplateArgumentList`。

[`DeclarationName.cpp`](DeclarationName.cpp) — `DeclarationName`
(`Identifier`/`CXXOperatorName`/`Using` 等) 存储与查表。
- 上游: [`Decl.cpp`](Decl.cpp)、[`DeclCXX.cpp`](DeclCXX.cpp)、
  NameLookup。
- 下游: [`DeclarationName.h`](../../include/clang/AST/DeclarationName.h)、
  [`TypeOrdering.h`](../../include/clang/AST/TypeOrdering.h)、
  [`IdentifierTable.h`](../Basic/IdentifierTable.h)。
- 关键类/函数: `class DeclarationName`, `DeclarationNameTable`,
  `getName`, `CXXOperatorName`。

### 3.3 Type & TypeLoc 体系

[`Type.cpp`](Type.cpp) — `Type` 基类与所有 Type 子类实现,
canonical type 缓存, 依赖性计算。
- 上游: [`ASTContext.cpp`](ASTContext.cpp)、
  [`Sema/SemaType.cpp`](../Sema/SemaType.cpp)。
- 下游: [`Linkage.h`](Linkage.h)、
  [`Type.h`](../../include/clang/AST/Type.h)、
  [`DependenceFlags.h`](../../include/clang/AST/DependenceFlags.h)、
  [`PrettyPrinter.h`](../../include/clang/AST/PrettyPrinter.h)。
- 关键类/函数: `class Type`, `class QualType`, `CanonicalType`,
  `getAs`, `getCanonicalTypeInternal`。

[`TypeLoc.cpp`](TypeLoc.cpp) — `TypeLoc` 子类 (类型 + 源码位置) 实
现, source-location-aware type 表示。
- 上游: [`Sema/SemaType.cpp`](../Sema/SemaType.cpp)、CodeGen。
- 下游: [`TypeLoc.h`](../../include/clang/AST/TypeLoc.h)、
  [`TypeLocVisitor.h`](../../include/clang/AST/TypeLocVisitor.h)、
  [`NestedNameSpecifier.h`](../../include/clang/AST/NestedNameSpecifier.h)。
- 关键类/函数: `class TypeLoc`, `TypeSourceInfo`,
  `getLocalSourceRange`, `initialize`。

[`TypePrinter.cpp`](TypePrinter.cpp) — 把 Type 漂亮地 print 回
C/C++ 源码 (含 template/elaborated/no-qualifier 等模式)。
- 上游: Clang Serialization / ASTDumper。
- 下游: [`PrettyPrinter.h`](../../include/clang/AST/PrettyPrinter.h)、
  [`TemplateName.h`](../../include/clang/AST/TemplateName.h)、
  [`NestedNameSpecifier.h`](../../include/clang/AST/NestedNameSpecifier.h)。
- 关键类/函数: `class TypePrinter`, `print`, `printingPolicy`,
  `ElaboratedTypePrinter`。

[`NestedNameSpecifier.cpp`](NestedNameSpecifier.cpp) —
`NestedNameSpecifier` (`A::B::C` 嵌套名说明符) 构造、查表与打印。
- 上游: [`Sema/SemaCXXScopeSpec.cpp`](../Sema/SemaCXXScopeSpec.cpp)、
  [`TypePrinter.cpp`](TypePrinter.cpp)。
- 下游: [`NestedNameSpecifier.h`](../../include/clang/AST/NestedNameSpecifier.h)、
  [`TemplateName.h`](../../include/clang/AST/TemplateName.h)、
  [`PrettyPrinter.h`](../../include/clang/AST/PrettyPrinter.h)。
- 关键类/函数: `class NestedNameSpecifier`, `Create`, `print`,
  `getAsNamespace`。

[`TemplateBase.cpp`](TemplateBase.cpp) — `TemplateArgument` 与相关
通用结构 (依赖性、打印、构造)。
- 上游: [`DeclTemplate.cpp`](DeclTemplate.cpp)、
  [`Sema/SemaTemplate.cpp`](../Sema/SemaTemplate.cpp)。
- 下游: [`TemplateBase.h`](../../include/clang/AST/TemplateBase.h)、
  [`DependenceFlags.h`](../../include/clang/AST/DependenceFlags.h)、
  [`PrettyPrinter.h`](../../include/clang/AST/PrettyPrinter.h)。
- 关键类/函数: `class TemplateArgument`, `TemplateArgumentLoc`,
  `isDependent`, `print`。

[`TemplateName.cpp`](TemplateName.cpp) — `TemplateName` 子类
(`Template`/`Overloaded`/`Assumed`/`Dependent` 等) 实现。
- 上游: [`DeclTemplate.cpp`](DeclTemplate.cpp)、[`Type.cpp`](Type.cpp)。
- 下游: [`TemplateName.h`](../../include/clang/AST/TemplateName.h)、
  [`DeclTemplate.h`](../../include/clang/AST/DeclTemplate.h)、
  [`PrettyPrinter.h`](../../include/clang/AST/PrettyPrinter.h)。
- 关键类/函数: `class TemplateName`, `OverloadedTemplateStorage`,
  `AssumedTemplateStorage`, `DependentTemplateName`。

### 3.4 Expr & Stmt 体系

[`Expr.cpp`](Expr.cpp) — `Expr` 基类与所有平台无关 Expr (`Unary`/
`Binary`/`Cast`/`Call`/`Literal` 等) 实现。
- 上游: [`Sema/SemaExpr.cpp`](../Sema/SemaExpr.cpp)。
- 下游: [`Expr.h`](../../include/clang/AST/Expr.h)、
  [`ComputeDependence.h`](../../include/clang/AST/ComputeDependence.h)、
  [`DeclCXX.h`](../../include/clang/AST/DeclCXX.h)、
  [`Mangle.h`](../../include/clang/AST/Mangle.h)。
- 关键类/函数: `Expr`, `CallExpr`, `CastExpr`, `DeclRefExpr`,
  `getResultType`, `EvaluateAsInt`。

[`ExprCXX.cpp`](ExprCXX.cpp) — C++ Expr 子类 (`Lambda`/
`CXXConstruct`/`MemberCall`/`New`/`TypeTrait` 等) 实现。
- 上游: [`Sema/SemaExprCXX.cpp`](../Sema/SemaExprCXX.cpp)。
- 下游: [`ExprCXX.h`](../../include/clang/AST/ExprCXX.h)、
  [`LambdaCapture.h`](../../include/clang/AST/LambdaCapture.h)、
  [`ComparisonCategories.h`](../../include/clang/AST/ComparisonCategories.h)。
- 关键类/函数: `LambdaExpr`, `CXXConstructExpr`, `CXXNewExpr`,
  `CXXMemberCallExpr`, `CXXTypeidExpr`。

[`ExprObjC.cpp`](ExprObjC.cpp) — ObjC Expr 子类 (`Message`/
`PropertyRef`/`IvarRef` 等) 实现。
- 上游: [`Sema/SemaExprObjC.cpp`](../Sema/SemaExprObjC.cpp)。
- 下游: [`ExprObjC.h`](../../include/clang/AST/ExprObjC.h)、
  [`SelectorLocationsKind.h`](../../include/clang/AST/SelectorLocationsKind.h)。
- 关键类/函数: `ObjCMessageExpr`, `ObjCPropertyRefExpr`,
  `ObjCIvarRefExpr`。

[`ExprConcepts.cpp`](ExprConcepts.cpp) — Concepts Expr
(`ConceptReference`/`Satisfaction`) 实现。
- 上游: [`Sema/SemaConcept.cpp`](../Sema/SemaConcept.cpp)。
- 下游: [`ExprConcepts.h`](../../include/clang/AST/ExprConcepts.h)、
  [`ASTConcept.h`](../../include/clang/AST/ASTConcept.h)、
  [`ComputeDependence.h`](../../include/clang/AST/ComputeDependence.h)。
- 关键类/函数: `ConceptReference`, `ConstraintSatisfaction`,
  `SatisfiedRequirement`。

[`ExprClassification.cpp`](ExprClassification.cpp) — `Expr::classify()`
— 把表达式分类为 lvalue/xvalue/prvalue (供 Sema/CodeGen)。
- 上游: [`Sema/SemaExpr.cpp`](../Sema/SemaExpr.cpp)、
  [`CodeGen/CGExpr.cpp`](../CodeGen/CGExpr.cpp)。
- 下游: [`Expr.h`](../../include/clang/AST/Expr.h)、
  [`ExprCXX.h`](../../include/clang/AST/ExprCXX.h)。
- 关键类/函数: `Expr::classify`, `Classification`, `Kinds`。

[`Stmt.cpp`](Stmt.cpp) — `Stmt` 基类与所有平台无关 Stmt (`If`/`For`/
`While`/`Decl`/`Compound` 等) 实现。
- 上游: [`Sema/SemaStmt.cpp`](../Sema/SemaStmt.cpp)、
  [`Parser/ParseStmt.cpp`](../Parse/ParseStmt.cpp)。
- 下游: [`Stmt.h`](../../include/clang/AST/Stmt.h)、
  [`Expr.h`](../../include/clang/AST/Expr.h)、
  [`ASTDiagnostic.h`](../../include/clang/AST/ASTDiagnostic.h)、
  [`DeclGroup.h`](../../include/clang/AST/DeclGroup.h)。
- 关键类/函数: `Stmt`, `CompoundStmt`, `DeclStmt`, `IfStmt`,
  `getSourceRange`。

[`StmtCXX.cpp`](StmtCXX.cpp) — C++ Stmt
(`CXXCatch`/`CXXTry`/`CXXForRange`) 实现。
- 上游: [`Sema/SemaStmt.cpp`](../Sema/SemaStmt.cpp)。
- 下游: [`StmtCXX.h`](../../include/clang/AST/StmtCXX.h)、
  [`ExprCXX.h`](../../include/clang/AST/ExprCXX.h)。
- 关键类/函数: `CXXCatchStmt`, `CXXTryStmt`, `CXXForRangeStmt`。

[`StmtObjC.cpp`](StmtObjC.cpp) — ObjC Stmt
(`ObjCForCollection`/`ObjCAtCatch`/`ObjCAtFinally`) 实现。
- 上游: [`Sema/SemaStmtObjC.cpp`](../Sema/)。
- 下游: [`StmtObjC.h`](../../include/clang/AST/StmtObjC.h)、
  [`Expr.h`](../../include/clang/AST/Expr.h)。
- 关键类/函数: `ObjCForCollectionStmt`, `ObjCAtCatchStmt`,
  `ObjCAtFinallyStmt`。

[`StmtOpenACC.cpp`](StmtOpenACC.cpp) — OpenACC 构造 Stmt
(`ComputeConstruct`/`LoopConstruct`/`Combined` 等) 实现。
- 上游: [`Sema/SemaOpenACC.cpp`](../Sema/SemaOpenACC.cpp)。
- 下游: [`StmtOpenACC.h`](../../include/clang/AST/StmtOpenACC.h)、
  [`ExprCXX.h`](../../include/clang/AST/ExprCXX.h)。
- 关键类/函数: `OpenACCComputeConstruct`, `OpenACCLoopConstruct`,
  `OpenACCCombinedConstruct`。

[`StmtOpenMP.cpp`](StmtOpenMP.cpp) — OpenMP 指令 Stmt
(`Parallel`/`Simd`/`For`/`Distribute` 等) 实现, 极大文件。
- 上游: [`Sema/SemaOpenMP.cpp`](../Sema/SemaOpenMP.cpp)。
- 下游: [`StmtOpenMP.h`](../../include/clang/AST/StmtOpenMP.h)、
  [`ExprOpenMP.h`](../../include/clang/AST/ExprOpenMP.h)。
- 关键类/函数: `OMPParallelDirective`, `OMPSimdDirective`,
  `OMPLoopDirective`, `getIntraTileHint`。

[`StmtIterator.cpp`](StmtIterator.cpp) — `StmtIterator` 内部辅助函
数, 用于 CFG 反向遍历 children。
- 上游: [`Analysis/CFG.cpp`](../Analysis/CFG.cpp)、
  [`CodeGen/CGStmt.cpp`](../CodeGen/CGStmt.cpp)。
- 下游: [`StmtIterator.h`](../../include/clang/AST/StmtIterator.h)、
  [`Type.h`](../../include/clang/AST/Type.h)。
- 关键类/函数: `StmtIterator`, `FindVA`, `VariableArrayType`。

[`StmtPrinter.cpp`](StmtPrinter.cpp) — `Stmt::printPretty()` — 把
Stmt 反向 pretty-print 成 C/C++ 源码。
- 上游: AST Dumper、Clangd、CodeGen debug info。
- 下游: [`PrettyPrinter.h`](../../include/clang/AST/PrettyPrinter.h)、
  [`StmtVisitor.h`](../../include/clang/AST/StmtVisitor.h)、
  [`DeclPrinter`](DeclPrinter.cpp)。
- 关键类/函数: `class StmtPrinter`, `VisitCompoundStmt`,
  `VisitIfStmt`, `PrintExpr`。

[`StmtProfile.cpp`](StmtProfile.cpp) — `Stmt::Profile` — 为
FoldingSet 生成 Stmt/Expr 的稳定 bit 表示 (含 ODR hash)。
- 上游: [`ODRHash.cpp`](ODRHash.cpp)、ASTMatcher。
- 下游: [`StmtVisitor.h`](../../include/clang/AST/StmtVisitor.h)、
  [`ODRHash.h`](../../include/clang/AST/ODRHash.h)、
  [`ExprCXX.h`](../../include/clang/AST/ExprCXX.h)。
- 关键类/函数: `Stmt::Profile`, `StmtProfiler`, `Visit`。

[`StmtViz.cpp`](StmtViz.cpp) — `Stmt::viewAST()` — 用 Graphviz 把
AST 可视化 (调试用)。
- 上游: (debug 调用)。
- 下游: [`StmtGraphTraits.h`](../../include/clang/AST/StmtGraphTraits.h)、
  [`llvm/Support/GraphWriter.h`](../../../llvm/include/llvm/Support/GraphWriter.h)。
- 关键类/函数: `Stmt::viewAST`, `ViewGraph`。

### 3.5 Constexpr 求值 (旧 AST-walker)

[`APValue.cpp`](APValue.cpp) — `APValue` (untyped constant value 容
器, 持 Int/Float/Complex/Vector/Struct/Array 等)。
- 上游: [`ExprConstant.cpp`](ExprConstant.cpp)、
  [`ByteCode/Context.cpp`](ByteCode/Context.cpp)、CodeGen。
- 下游: [`Linkage.h`](Linkage.h)、
  [`APValue.h`](../../include/clang/AST/APValue.h)、
  [`DeclCXX.h`](../../include/clang/AST/DeclCXX.h)、
  [`Expr.h`](../../include/clang/AST/Expr.h)。
- 关键类/函数: `class APValue`, `TypeInfoLValue`, `print`,
  `getAsString`。

[`ExprConstant.cpp`](ExprConstant.cpp) — 旧版 AST-walker-based 常
量求值器: 把 Expr 求值为 APValue (可继续由 ByteCode 取代)。
- 上游: [`Sema/SemaExpr.cpp`](../Sema/SemaExpr.cpp)、
  [`CodeGen/CGExprConstant.cpp`](../CodeGen/CGExprConstant.cpp)、
  [`ByteCode/Context.cpp`](ByteCode/Context.cpp)。
- 下游: [`ExprConstShared.h`](ExprConstShared.h)、
  [`Expr.h`](../../include/clang/AST/Expr.h)、
  [`DeclCXX.h`](../../include/clang/AST/DeclCXX.h)、
  [`TemplateBase.h`](../../include/clang/AST/TemplateBase.h)。
- 关键类/函数: `Expr::EvaluateAsInt`, `Evaluate`,
  `CallExpr::isConstant`。

[`ExprConstShared.h`](ExprConstShared.h) — 共享工具: complex 乘除
法、`__builtin_object_size`/`strlen`、x86 builtin ID 映射。
- 上游: [`ExprConstant.cpp`](ExprConstant.cpp)、
  [`ByteCode/InterpBuiltin.cpp`](ByteCode/InterpBuiltin.cpp)、
  [`ByteCode/Compiler.cpp`](ByteCode/Compiler.cpp)、
  [`ByteCode/Interp.cpp`](ByteCode/Interp.cpp)。
- 下游: [`BuiltinTraits.h`](../Basic/BuiltinTraits.h)。
- 关键类/函数: `HandleComplexComplexMul`,
  `ConvertBuiltinIDToX86BuiltinID`, `GCCTypeClass`, `GFNIAffine`。

[`InferAlloc.cpp`](InferAlloc.cpp) — 为 `new`/`alloca`/`realloc` 等
内建函数推断分配类型 (辅助 constexpr 求值)。
- 上游: [`ByteCode/InterpBuiltin.cpp`](ByteCode/InterpBuiltin.cpp)、
  [`Sema/SemaExpr.cpp`](../Sema/SemaExpr.cpp)。
- 下游: [`InferAlloc.h`](../../include/clang/AST/InferAlloc.h)、
  [`ASTContext.h`](../../include/clang/AST/ASTContext.h)。
- 关键类/函数: `infer_alloc::inferType`, `MallocAlloc`, `AllocaKind`。

### 3.6 C++ ABI / Mangling / Layout

[`CXXABI.h`](CXXABI.h) — `CXXABI` 抽象接口 (成员指针宽度、异常对
象拷贝构造映射等)。
- 上游: [`ItaniumCXXABI.cpp`](ItaniumCXXABI.cpp)、
  [`MicrosoftCXXABI.cpp`](MicrosoftCXXABI.cpp)、
  [`ASTContext.cpp`](ASTContext.cpp)。
- 下游: [`Type.h`](../../include/clang/AST/Type.h)。
- 关键类/函数: `class CXXABI`, `CreateItaniumCXXABI`,
  `CreateMicrosoftCXXABI`, `MemberPointerInfo`。

[`ItaniumCXXABI.cpp`](ItaniumCXXABI.cpp) — Itanium C++ ABI 具体实
现 (成员指针、对齐、复制构造映射等)。
- 上游: [`ASTContext.cpp`](ASTContext.cpp)。
- 下游: [`CXXABI.h`](CXXABI.h)、
  [`MangleNumberingContext.h`](../../include/clang/AST/MangleNumberingContext.h)、
  [`RecordLayout.h`](../../include/clang/AST/RecordLayout.h)。
- 关键类/函数: `class ItaniumCXXABI`, `getMemberPointerInfo`,
  `addCopyConstructorForExceptionObject`。

[`MicrosoftCXXABI.cpp`](MicrosoftCXXABI.cpp) — MSVC C++ ABI 具体实
现 (VBTable 布局、异常对象布局等)。
- 上游: [`ASTContext.cpp`](ASTContext.cpp)。
- 下游: [`CXXABI.h`](CXXABI.h)、
  [`Mangle.h`](../../include/clang/AST/Mangle.h)、
  [`RecordLayout.h`](../../include/clang/AST/RecordLayout.h)。
- 关键类/函数: `class MicrosoftCXXABI`,
  `isVirtualToCompleteObjectLocator`, `getAddrOfVTable`。

[`Mangle.cpp`](Mangle.cpp) — `MangleContext` 抽象基类 + 入口分派
(ObjC/Itanium/Microsoft) + Block mangling。
- 上游: [`ItaniumMangle.cpp`](ItaniumMangle.cpp)、
  [`MicrosoftMangle.cpp`](MicrosoftMangle.cpp)、
  [`CodeGenModule.cpp`](../CodeGen/CodeGenModule.cpp)。
- 下游: [`Mangle.h`](../../include/clang/AST/Mangle.h)、
  [`VTableBuilder.h`](../../include/clang/AST/VTableBuilder.h)、
  [`DeclObjC.h`](../../include/clang/AST/DeclObjC.h)。
- 关键类/函数: `class MangleContext`, `createMangleContext`,
  `mangleBlock`。

[`ItaniumMangle.cpp`](ItaniumMangle.cpp) — Itanium C++ ABI 名
mangler 完整实现 (极大文件)。
- 上游: [`Mangle.cpp`](Mangle.cpp)、CodeGen。
- 下游: [`Mangle.h`](../../include/clang/AST/Mangle.h)、
  [`DeclTemplate.h`](../../include/clang/AST/DeclTemplate.h)、
  [`DeclObjC.h`](../../include/clang/AST/DeclObjC.h)。
- 关键类/函数: `class ItaniumMangleContext`, `mangle`, `mangleName`,
  `mangleType`。

[`MicrosoftMangle.cpp`](MicrosoftMangle.cpp) — MSVC C++ ABI 名
mangler 完整实现 (极大文件)。
- 上游: [`Mangle.cpp`](Mangle.cpp)、CodeGen。
- 下游: [`Mangle.h`](../../include/clang/AST/Mangle.h)、
  [`VTableBuilder.h`](../../include/clang/AST/VTableBuilder.h)、
  [`DeclObjC.h`](../../include/clang/AST/DeclObjC.h)。
- 关键类/函数: `class MicrosoftMangleContext`, `mangle`, `mangleName`,
  `mangleType`。

[`CXXInheritance.cpp`](CXXInheritance.cpp) — C++ 类继承分析: 路径
查找、菱形检测、子对象布局查询。
- 上游: [`VTableBuilder.cpp`](VTableBuilder.cpp)、
  [`RecordLayoutBuilder.cpp`](RecordLayoutBuilder.cpp)、
  [`Sema/SemaDeclCXX.cpp`](../Sema/SemaDeclCXX.cpp)。
- 下游: [`CXXInheritance.h`](../../include/clang/AST/CXXInheritance.h)、
  [`RecordLayout.h`](../../include/clang/AST/RecordLayout.h)、
  [`TemplateName.h`](../../include/clang/AST/TemplateName.h)。
- 关键类/函数: `class CXXBasePaths`, `lookupInBases`,
  `getSubDeclAtOffset`, `isVirtuallyDerivedFrom`。

[`RecordLayout.cpp`](RecordLayout.cpp) — `ASTRecordLayout` 构造与
构造 (含 `CXXRecordLayoutInfo`)。
- 上游: [`ASTContext.cpp`](ASTContext.cpp)、
  [`RecordLayoutBuilder.cpp`](RecordLayoutBuilder.cpp)。
- 下游: [`RecordLayout.h`](../../include/clang/AST/RecordLayout.h)、
  [`TargetCXXABI.h`](../Basic/TargetCXXABI.h)。
- 关键类/函数: `class ASTRecordLayout`, `Destroy`, `getPrimaryBase`,
  `getBaseClassOffset`。

[`RecordLayoutBuilder.cpp`](RecordLayoutBuilder.cpp) — 按 ABI 规则
计算 struct/union 实际字段偏移/对齐 (Itanium + MSVC)。
- 上游: [`ASTContext.cpp`](ASTContext.cpp)、
  [`VTableBuilder.cpp`](VTableBuilder.cpp)。
- 下游: [`RecordLayout.h`](../../include/clang/AST/RecordLayout.h)、
  [`CXXInheritance.h`](../../include/clang/AST/CXXInheritance.h)、
  [`TargetInfo.h`](../Basic/TargetInfo.h)。
- 关键类/函数: `class RecordLayoutBuilder`, `LayoutField`,
  `LayoutBase`, `BaseSubobjectInfo`。

[`VTableBuilder.cpp`](VTableBuilder.cpp) — C++ vtable 构建: 虚函
数调整、vtable 顺序、primary vtable。
- 上游: [`CodeGen/CGVTables.cpp`](../CodeGen/CGVTables.cpp)、
  [`ItaniumMangle.cpp`](ItaniumMangle.cpp)。
- 下游: [`VTableBuilder.h`](../../include/clang/AST/VTableBuilder.h)、
  [`RecordLayout.h`](../../include/clang/AST/RecordLayout.h)、
  [`CXXInheritance.h`](../../include/clang/AST/CXXInheritance.h)。
- 关键类/函数: `class VTableBuilder`, `ItaniumVTableBuilder`,
  `computeVTable`, `VTableLayout`。

[`VTTBuilder.cpp`](VTTBuilder.cpp) — 构造 VTT (virtual table table),
用来构造 derived vtable 指针。
- 上游: [`CodeGen/CGVTT.cpp`](../CodeGen/CGVTT.cpp)、
  [`VTableBuilder.cpp`](VTableBuilder.cpp)。
- 下游: [`VTTBuilder.h`](../../include/clang/AST/VTTBuilder.h)、
  [`CharUnits.h`](../../include/clang/AST/CharUnits.h)、
  [`RecordLayout.h`](../../include/clang/AST/RecordLayout.h)。
- 关键类/函数: `class VTTBuilder`, `BuildVTT`, `getVTTComponents`。

### 3.7 Attribute / Comment / Platform 扩展

[`AttrImpl.cpp`](AttrImpl.cpp) — Attr 子类 (`LoopHint`/`TypeVisibility`
等) 的 out-of-line 方法与 pretty-print。
- 上游: [`Sema/SemaAttr.cpp`](../Sema/SemaAttr.cpp)、CodeGen。
- 下游: [`ASTContext.h`](../../include/clang/AST/ASTContext.h)、
  [`Attr.h`](../../include/clang/AST/Attr.h)、
  [`ASTStructuralEquivalence.h`](../../include/clang/AST/ASTStructuralEquivalence.h)。
- 关键类/函数: `LoopHintAttr::printPrettyPragma`, `AllocAlignAttr`,
  `InheritableAttr`。

[`AttrDocTable.cpp`](AttrDocTable.cpp) — 把生成的 `AttrDocTable.inc`
装入 `AttrDoc[]` 数组, 提供 `Attr::getDocumentation`。
- 上游: [`Attr.h`](../../include/clang/AST/Attr.h) 调用者 (Doxygen 生成)。
- 下游: `AttrDocTable.inc` (TableGen 生成)、
  [`Attr.h`](../../include/clang/AST/Attr.h)、
  `AttrList.inc` (TableGen 生成)。
- 关键类/函数: `Attr::getDocumentation`, `AttrDoc`, `attr::Kind`。

[`Availability.cpp`](Availability.cpp) — `AvailabilityInfo` 合并与
创建 (跨平台 availability attribute 归并)。
- 上游: [`Sema/SemaAttr.cpp`](../Sema/SemaAttr.cpp)、CodeGen。
- 下游: [`Availability.h`](../../include/clang/AST/Availability.h)、
  [`Attr.h`](../../include/clang/AST/Attr.h)、
  [`TargetInfo.h`](../Basic/TargetInfo.h)。
- 关键类/函数: `AvailabilityInfo::mergeWith`,
  `AvailabilityInfo::createFromDecl`, `AvailabilitySet`。

[`Comment.cpp`](Comment.cpp) — Comment AST 节点的 trivial 静态断言
+ 部分辅助函数。
- 上游: [`CommentParser.cpp`](CommentParser.cpp)、
  [`CommentSema.cpp`](CommentSema.cpp)。
- 下游: [`Comment.h`](../../include/clang/AST/Comment.h)、
  `CommentNodes.inc` (TableGen 生成)。
- 关键类/函数: `Comment class hierarchy check`。

[`CommentBriefParser.cpp`](CommentBriefParser.cpp) — 极简 brief 注
释解析器 (从原始注释文本提取第一段)。
- 上游: [`RawCommentList.cpp`](RawCommentList.cpp)。
- 下游: [`CommentBriefParser.h`](../../include/clang/AST/CommentBriefParser.h)、
  [`CommentCommandTraits.h`](../../include/clang/AST/CommentCommandTraits.h)。
- 关键类/函数: `cleanupBrief`, `parseBriefComment`, `getBriefText`。

[`CommentCommandTraits.cpp`](CommentCommandTraits.cpp) — Comment 命
令 (`\brief`、`\param` 等) 注册与 ID 分配表。
- 上游: [`CommentParser.cpp`](CommentParser.cpp)、
  [`CommentLexer.cpp`](CommentLexer.cpp)。
- 下游: [`CommentCommandTraits.h`](../../include/clang/AST/CommentCommandTraits.h)、
  `CommentCommandInfo.inc` (TableGen 生成)。
- 关键类/函数: `class CommandTraits`, `registerCommentOptions`,
  `NextID`。

[`CommentLexer.cpp`](CommentLexer.cpp) — Doxygen 注释 Lexer, 把原
始注释文本切分为 token。
- 上游: [`CommentParser.cpp`](CommentParser.cpp)、
  [`RawCommentList.cpp`](RawCommentList.cpp)。
- 下游: [`CommentLexer.h`](../../include/clang/AST/CommentLexer.h)、
  [`Comment.h`](../../include/clang/AST/Comment.h)、
  [`CommentCommandTraits.h`](../../include/clang/AST/CommentCommandTraits.h)。
- 关键类/函数: `class Lexer::lex`, `Token`, `getSpelling`。

[`CommentParser.cpp`](CommentParser.cpp) — Doxygen 注释 Parser, 把
token 流构造成 Comment AST。
- 上游: [`CommentSema.cpp`](CommentSema.cpp)。
- 下游: [`CommentParser.h`](../../include/clang/AST/CommentParser.h)、
  [`CommentLexer.h`](../../include/clang/AST/CommentLexer.h)、
  [`CommentSema.h`](../../include/clang/AST/CommentSema.h)。
- 关键类/函数: `class Parser::parseComment`, `parseParamCommand`,
  `parseTParamCommand`。

[`CommentSema.cpp`](CommentSema.cpp) — Doxygen 注释的 Sema: 命令名
检查、HTML tag 校验、引用链接解析。
- 上游: [`Sema.cpp`](../Sema/Sema.cpp) (LateBound Templates)、
  Serialization。
- 下游: [`CommentSema.h`](../../include/clang/AST/CommentSema.h)、
  [`Decl.h`](../../include/clang/AST/Decl.h)、
  `CommentHTMLTagsProperties.inc` (TableGen 生成)。
- 关键类/函数: `class Sema::checkDeclReference`, `checkCommand`,
  `isHTMLTag`。

[`NSAPI.cpp`](NSAPI.cpp) — `NSAPI`: Apple Foundation 框架
(`NSObject`、`NSString`、`NSInteger`) 辅助识别。
- 上游: [`Sema/SemaDeclObjC.cpp`](../Sema/SemaDeclObjC.cpp)、
  [`Sema/SemaExprObjC.cpp`](../Sema/SemaExprObjC.cpp)。
- 下游: [`NSAPI.h`](../../include/clang/AST/NSAPI.h)、
  [`DeclObjC.h`](../../include/clang/AST/DeclObjC.h)。
- 关键类/函数: `NSAPI::getNSClassId`, `NSClassIdKindKind`,
  `NSIntegerId`。

[`OSLog.cpp`](OSLog.cpp) — OS Log (`__builtin_os_log_format`) 格式
分析与缓冲区布局计算。
- 上游: [`Sema/SemaChecking.cpp`](../Sema/SemaChecking.cpp)、
  [`CodeGen/CGBuiltin.cpp`](../CodeGen/CGBuiltin.cpp)。
- 下游: [`OSLog.h`](../../include/clang/AST/OSLog.h)、
  [`FormatString.h`](../../include/clang/AST/FormatString.h)、
  [`Builtins.h`](../Basic/Builtins.h)。
- 关键类/函数: `OSLogFormatStringHandler`,
  `computeOSLogBufferLayout`, `OSLogBufferLayout`。

[`OpenACCClause.cpp`](OpenACCClause.cpp) — OpenACC Clause 子类
(`DeviceType`/`Bind`/`Self` 等) 实现。
- 上游: [`Sema/SemaOpenACC.cpp`](../Sema/SemaOpenACC.cpp)。
- 下游: [`OpenACCClause.h`](../../include/clang/AST/OpenACCClause.h)、
  [`Expr.h`](../../include/clang/AST/Expr.h)。
- 关键类/函数: `class OpenACCClauseWithParams`,
  `OpenACCDeviceTypeClause`, `OpenACCBindClause`。

[`OpenMPClause.cpp`](OpenMPClause.cpp) — OpenMP Clause 子类
(`Schedule`/`Reduction`/`Align` 等) 实现, 极大文件。
- 上游: [`Sema/SemaOpenMP.cpp`](../Sema/SemaOpenMP.cpp)。
- 下游: [`OpenMPClause.h`](../../include/clang/AST/OpenMPClause.h)、
  [`DeclOpenMP.h`](../../include/clang/AST/DeclOpenMP.h)、
  [`OpenMPKinds.h`](../Basic/OpenMPKinds.h)。
- 关键类/函数: `class OMPClause`, `OMPScheduleClause`,
  `OMPReductionClause`, `OMPAlignedClause`。

[`HLSLResource.cpp`](HLSLResource.cpp) — HLSL 资源类型的内嵌名构造
(`BaseClass`+helper+index path builder)。
- 上游: [`Sema/SemaHLSL.cpp`](../Sema/SemaHLSL.cpp)、
  [`CodeGenModule.cpp`](../CodeGen/CodeGenModule.cpp)。
- 下游: [`HLSLResource.h`](../../include/clang/AST/HLSLResource.h)、
  [`DeclCXX.h`](../../include/clang/AST/DeclCXX.h)。
- 关键类/函数: `class EmbeddedResourceNameBuilder`, `pushBaseName`,
  `pushArrayIndex`。

[`Randstruct.cpp`](Randstruct.cpp) — 实现 `-frandomize-layout` 字段
重排 (Randstruct 算法)。
- 上游: [`Sema/SemaDecl.cpp`](../Sema/SemaDecl.cpp)、
  Attribute 处理。
- 下游: [`Randstruct.h`](../../include/clang/AST/Randstruct.h)、
  [`ASTContext.h`](../../include/clang/AST/ASTContext.h)。
- 关键类/函数: `randomizeStructureLayout`, `sortRandomizedFields`。

### 3.8 Format string 分析

[`FormatString.cpp`](FormatString.cpp) — printf/scanf 格式串分析
共享细节 (转换说明符、参数类型检查)。
- 上游: [`PrintfFormatString.cpp`](PrintfFormatString.cpp)、
  [`ScanfFormatString.cpp`](ScanfFormatString.cpp)。
- 下游: [`FormatStringParsing.h`](FormatStringParsing.h)、
  [`FormatString.h`](../../include/clang/AST/FormatString.h)、
  [`TargetInfo.h`](../Basic/TargetInfo.h)。
- 关键类/函数: `class FormatStringHandler`, `FormatSpecifier`,
  `ConversionSpecifier`, `ArgType`。

[`FormatStringParsing.h`](FormatStringParsing.h) — printf/scanf 共
享解析辅助: `ParseAmount`/`ParseFieldWidth` 等模板。
- 上游: [`FormatString.cpp`](FormatString.cpp)、
  [`PrintfFormatString.cpp`](PrintfFormatString.cpp)、
  [`ScanfFormatString.cpp`](ScanfFormatString.cpp)。
- 下游: [`FormatString.h`](../../include/clang/AST/FormatString.h)、
  [`Type.h`](../../include/clang/AST/Type.h)。
- 关键类/函数: `ParseAmount`, `ParseFieldWidth`,
  `ParseLengthModifier`, `UpdateOnReturn`。

[`PrintfFormatString.cpp`](PrintfFormatString.cpp) — printf/fprintf
系列格式串完整 Sema 检查。
- 上游: [`Sema/SemaChecking.cpp`](../Sema/SemaChecking.cpp)、
  [`Analysis/Printf.cpp`](../Analysis/)。
- 下游: [`FormatStringParsing.h`](FormatStringParsing.h)、
  [`FormatString.h`](../../include/clang/AST/FormatString.h)、
  [`OSLog.h`](../../include/clang/AST/OSLog.h)。
- 关键类/函数: `class PrintfFormatStringHandler`, `PrintfSpecifier`,
  `checkFormatString`。

[`ScanfFormatString.cpp`](ScanfFormatString.cpp) — scanf/fscanf 系
列格式串完整 Sema 检查。
- 上游: [`Sema/SemaChecking.cpp`](../Sema/SemaChecking.cpp)、
  [`Analysis/Scanf.cpp`](../Analysis/)。
- 下游: [`FormatStringParsing.h`](FormatStringParsing.h)、
  [`FormatString.h`](../../include/clang/AST/FormatString.h)。
- 关键类/函数: `class ScanfFormatStringHandler`, `ScanfSpecifier`,
  `checkFormatString`。

### 3.9 Helper / analysis / AST walker

[`DataCollection.cpp`](DataCollection.cpp) — 为 ODR-diagnostic 等场
景提取宏展开栈 (`MacroStack`)。
- 上游: [`ODRDiagsEmitter.cpp`](ODRDiagsEmitter.cpp)、
  [`TextNodeDumper.cpp`](TextNodeDumper.cpp)。
- 下游: [`DataCollection.h`](../../include/clang/AST/DataCollection.h)、
  [`Lexer.h`](../Lex/Lexer.h)。
- 关键类/函数: `data_collection::getMacroStack`, `printMacroName`。

[`ExternalASTSource.cpp`](ExternalASTSource.cpp) — `ExternalASTSource`
抽象基类: 从 PCH/Module 文件延迟加载 AST 节点的默认实现。
- 上游: [`Serialization/ASTReader.cpp`](../Serialization/ASTReader.cpp)、
  Clang Frontend。
- 下游: [`ExternalASTSource.h`](../../include/clang/AST/ExternalASTSource.h)、
  [`DeclarationName.h`](../../include/clang/AST/DeclarationName.h)。
- 关键类/函数: `class ExternalASTSource`, `getExternalDecl`,
  `FindExternalVisibleDeclsByName`。

[`ExternalASTMerger.cpp`](ExternalASTMerger.cpp) — 把多个
`ASTContext` (不同 module/源) 的 Decl 合成到单个逻辑视图 (Clangd
跨文件)。
- 上游: Clangd。
- 下游: [`ExternalASTMerger.h`](../../include/clang/AST/ExternalASTMerger.h)、
  [`DeclCXX.h`](../../include/clang/AST/DeclCXX.h)。
- 关键类/函数: `class ExternalASTMerger`, `tryForceExternalSource`。

[`InheritViz.cpp`](InheritViz.cpp) — `CXXRecordDecl::viewInheritance`
— 用 Graphviz 把 C++ 继承图可视化 (调试)。
- 上游: (debug 调用)。
- 下游: [`TypeOrdering.h`](../../include/clang/AST/TypeOrdering.h)、
  [`llvm/Support/GraphWriter.h`](../../../llvm/include/llvm/Support/GraphWriter.h)。
- 关键类/函数: `CXXRecordDecl::viewInheritance`, `ViewGraph`。

[`GlobalDecl.cpp`](GlobalDecl.cpp) — `GlobalDecl` 的 OpenMP/OpenACC/
CUDA 构造实现, 隔离大型头依赖。
- 上游: [`CodeGenModule.cpp`](../CodeGen/CodeGenModule.cpp)、
  [`Sema/SemaOpenMP.cpp`](../Sema/SemaOpenMP.cpp)。
- 下游: [`GlobalDecl.h`](../../include/clang/AST/GlobalDecl.h)、
  [`DeclOpenACC.h`](../../include/clang/AST/DeclOpenACC.h)、
  [`DeclOpenMP.h`](../../include/clang/AST/DeclOpenMP.h)。
- 关键类/函数: `class GlobalDecl`, `isKernelReference`,
  `hasCUDAGlobalAttr`。

[`Linkage.h`](Linkage.h) — AST 内部用: `LinkageComputer` +
`LVComputationKind` (链接性/可见性计算缓存)。
- 上游: [`APValue.cpp`](APValue.cpp)、[`Decl.cpp`](Decl.cpp)、
  [`Type.cpp`](Type.cpp)。
- 下游: [`Decl.h`](../../include/clang/AST/Decl.h)、
  [`DeclCXX.h`](../../include/clang/AST/DeclCXX.h)、
  [`Type.h`](../../include/clang/AST/Type.h)。
- 关键类/函数: `class LinkageComputer`, `computeLVForDecl`,
  `LVComputationKind`, `getDeclLinkageAndVisibility`。

[`ODRHash.cpp`](ODRHash.cpp) — `ODRHash`: 给 Decl/Type/Stmt 计算
稳定 hash (跨 TU 诊断 ODR 违反)。
- 上游: [`ODRDiagsEmitter.cpp`](ODRDiagsEmitter.cpp)、
  [`ASTStructuralEquivalence.cpp`](ASTStructuralEquivalence.cpp)。
- 下游: [`ODRHash.h`](../../include/clang/AST/ODRHash.h)、
  [`DeclVisitor.h`](../../include/clang/AST/DeclVisitor.h)、
  [`TypeVisitor.h`](../../include/clang/AST/TypeVisitor.h)。
- 关键类/函数: `class ODRHash`, `AddDecl`, `AddType`,
  `CalculateHash`。

[`ODRDiagsEmitter.cpp`](ODRDiagsEmitter.cpp) — 检测 ODR 违反时,
用 `ODRHash` 比较并产出诊断信息。
- 上游: [`Serialization/ASTReader.cpp`](../Serialization/ASTReader.cpp)。
- 下游: [`ODRHash.h`](../../include/clang/AST/ODRHash.h)、
  [`ODRDiagsEmitter.h`](../../include/clang/AST/ODRDiagsEmitter.h)。
- 关键类/函数: `class ODRDiagsEmitter`, `diagnoseMismatch`,
  `computeODRHash`。

[`ParentMap.cpp`](ParentMap.cpp) — `ParentMap`: 旧版 Stmt → 父
Stmt 映射 (CFG 分析使用)。
- 上游: [`Analysis/CFG.cpp`](../Analysis/CFG.cpp)。
- 下游: [`ParentMap.h`](../../include/clang/AST/ParentMap.h)、
  [`StmtObjC.h`](../../include/clang/AST/StmtObjC.h)。
- 关键类/函数: `class ParentMap`, `getParent`, `addStmt`,
  `OpaqueValueMode`。

[`ParentMapContext.cpp`](ParentMapContext.cpp) — `ParentMapContext`:
新版 `DynTypedNode`-based 多类型父节点映射 (AST Matcher)。
- 上游: AST Matchers、clangd。
- 下游: [`ParentMapContext.h`](../../include/clang/AST/ParentMapContext.h)、
  [`RecursiveASTVisitor.h`](../../include/clang/AST/RecursiveASTVisitor.h)、
  [`TemplateBase.h`](../../include/clang/AST/TemplateBase.h)。
- 关键类/函数: `class ParentMapContext`, `getParents`,
  `matchParents`。

[`QualTypeNames.cpp`](QualTypeNames.cpp) — `QualTypeNames`: 为类型
生成完全限定名 (含 template args), 用于诊断/dump。
- 上游: [`TextNodeDumper.cpp`](TextNodeDumper.cpp)、
  [`ODRDiagsEmitter.cpp`](ODRDiagsEmitter.cpp)。
- 下游: [`QualTypeNames.h`](../../include/clang/AST/QualTypeNames.h)、
  [`Mangle.h`](../../include/clang/AST/Mangle.h)、
  [`NestedNameSpecifier.h`](../../include/clang/AST/NestedNameSpecifier.h)。
- 关键类/函数: `TypeName::getFullyQualifiedName`, `getNameForType`。

[`RawCommentList.cpp`](RawCommentList.cpp) — 源文件 → RawComment 列
表的建立; brief-comment 提取; lazy comment 解析。
- 上游: [`Serialization/ASTWriter.cpp`](../Serialization/ASTWriter.cpp)、
  Clang Frontend。
- 下游: [`RawCommentList.h`](../../include/clang/AST/RawCommentList.h)、
  [`CommentBriefParser.h`](../../include/clang/AST/CommentBriefParser.h)。
- 关键类/函数: `class RawComment`, `getCommentKind`,
  `getSourceText`。

[`SelectorLocationsKind.cpp`](SelectorLocationsKind.cpp) — 判定
ObjC Selector 标识符位置是否在"标准"位置 (供
`-Wselector-type-mismatch`)。
- 上游: [`Sema/SemaExprObjC.cpp`](../Sema/SemaExprObjC.cpp)。
- 下游: [`SelectorLocationsKind.h`](../../include/clang/AST/SelectorLocationsKind.h)、
  [`Expr.h`](../../include/clang/AST/Expr.h)。
- 关键类/函数: `hasStandardSelectorLocs`,
  `getStandardSelectorLoc`, `getArgLoc`。

[`DynamicRecursiveASTVisitor.cpp`](DynamicRecursiveASTVisitor.cpp)
— `DynamicRecursiveASTVisitor` 把 `RecursiveASTVisitor` 包装成动态
分派版本。
- 上游: Clangd、AST Matchers (dynamic 部分)。
- 下游: [`RecursiveASTVisitor.h`](../../include/clang/AST/RecursiveASTVisitor.h)、
  [`DynamicRecursiveASTVisitor.h`](../../include/clang/AST/DynamicRecursiveASTVisitor.h)。
- 关键类/函数: `class DynamicRecursiveASTVisitor`,
  `TraverseDecl`, `TraverseStmt`。

[`ComputeDependence.cpp`](ComputeDependence.cpp) — 为 Type/Expr/Decl
计算 type/value/template dependence flags。
- 上游: [`Sema/SemaType.cpp`](../Sema/SemaType.cpp)、
  [`ASTContext.cpp`](ASTContext.cpp)。
- 下游: [`ComputeDependence.h`](../../include/clang/AST/ComputeDependence.h)、
  [`DependenceFlags.h`](../../include/clang/AST/DependenceFlags.h)、
  [`ExprConcepts.h`](../../include/clang/AST/ExprConcepts.h)。
- 关键类/函数: `computeDependence`, `TypeDependence`,
  `ExprDependence`。

[`ComparisonCategories.cpp`](ComparisonCategories.cpp) — 为 C++20
三路比较选择 `ComparisonCategoryType`。
- 上游: [`Sema/SemaExprCXX.cpp`](../Sema/SemaExprCXX.cpp)、
  [`ExprCXX.cpp`](ExprCXX.cpp)。
- 下游: [`ComparisonCategories.h`](../../include/clang/AST/ComparisonCategories.h)、
  [`DeclCXX.h`](../../include/clang/AST/DeclCXX.h)。
- 关键类/函数: `getComparisonCategoryForBuiltinCmp`,
  `ComparisonCategoryInfo`, `ComparisonCategoryType`。

### 3.10 Dump / Print

[`TextNodeDumper.cpp`](TextNodeDumper.cpp) — `ASTNodeDumper` 文本
格式: 人可读 AST 详细 dump (含 qualifiers/source loc)。
- 上游: [`ASTDumper.cpp`](ASTDumper.cpp)、`clang -ast-dump`。
- 下游: [`TextNodeDumper.h`](../../include/clang/AST/TextNodeDumper.h)、
  [`TypeLocVisitor.h`](../../include/clang/AST/TypeLocVisitor.h)、
  [`DeclFriend.h`](../../include/clang/AST/DeclFriend.h)。
- 关键类/函数: `class TextNodeDumper`, `VisitDecl`, `VisitStmt`,
  `VisitType`。

[`JSONNodeDumper.cpp`](JSONNodeDumper.cpp) — `ASTNodeDumper` JSON
格式输出 (供 IDE/clangd 解析 AST)。
- 上游: [`ASTDumper.cpp`](ASTDumper.cpp)、clangd。
- 下游: [`JSONNodeDumper.h`](../../include/clang/AST/JSONNodeDumper.h)、
  [`Type.h`](../../include/clang/AST/Type.h)、
  `DeclNodes.inc` (TableGen 生成)。
- 关键类/函数: `JSONNodeDumper::Visit`, `addPreviousDeclaration`,
  `writePreviousDeclImpl`。

### 3.11 Build / misc

[`CMakeLists.txt`](CMakeLists.txt) — 构建 `clangAST` 库的 CMake 脚
本, tablegen `Opcodes.inc` 和 `AttrDocTable.inc`。

### 3.12 ByteCode/ 子目录 — constexpr 字节码解释器

`ByteCode/` 是 Clang 的 **新 constexpr 字节码解释器**: 把 C++
constexpr 表达式/函数编译成字节码, 在栈式 VM 上求值, 与
[`ExprConstant.cpp`](ExprConstant.cpp) (旧 AST-walker) 共存并逐步替
换。

#### 3.12.1 解释器核心 (Context/State/Program)

[`ByteCode/Context.cpp`](ByteCode/Context.cpp) — per-ASTContext
constexpr VM 入口: `evaluateAsRValue` / `evaluate` / `Run` /
`isPotentialConstantExpr`。
- 上游: [`ASTContext.cpp`](ASTContext.cpp)、
  [`Sema/SemaExpr.cpp`](../Sema/SemaExpr.cpp)。
- 下游: [`ByteCode/Program.h`](ByteCode/Program.h)、
  [`ByteCode/EvalEmitter.h`](ByteCode/EvalEmitter.h)、
  [`ByteCode/Compiler.h`](ByteCode/Compiler.h)。
- 关键类/函数: `class Context`, `evaluateAsRValue`, `evaluate`,
  `Run`, `isPotentialConstantExpr`。

[`ByteCode/Program.cpp`](ByteCode/Program.cpp) — `Program` 类实
现: 跨函数字节码链接、全局变量表、Record 表、函数表。
- 上游: [`ByteCode/Context.cpp`](ByteCode/Context.cpp)。
- 下游: [`ByteCode/Function.h`](ByteCode/Function.h)、
  [`ByteCode/Descriptor.h`](ByteCode/Descriptor.h)、
  [`DeclTemplate.h`](../../include/clang/AST/DeclTemplate.h)。
- 关键类/函数: `class Program`, `Globals`, `Records`, `Funcs`,
  `getOrCreateFunction`。

[`ByteCode/State.cpp`](ByteCode/State.cpp) — `State` 基类的诊断/
常量上下文/副作用追踪; 与 VM 共享。
- 上游: [`ByteCode/Interp.cpp`](ByteCode/Interp.cpp)、
  [`ByteCode/EvalEmitter.cpp`](ByteCode/EvalEmitter.cpp)。
- 下游: [`ByteCode/Frame.h`](ByteCode/Frame.h)、
  [`ByteCode/Source.h`](ByteCode/Source.h)、
  [`OptionalDiagnostic.h`](../../include/clang/AST/OptionalDiagnostic.h)。
- 关键类/函数: `class State`, `FFDiag`, `emitRelaxedDiag`,
  `EvalStatus`。

[`ByteCode/InterpState.cpp`](ByteCode/InterpState.cpp) —
`InterpState`: VM 实际的求值状态 (Stack + Frames + Steps +
Allocator)。
- 上游: [`ByteCode/Context.cpp`](ByteCode/Context.cpp)、
  [`ByteCode/EvalEmitter.cpp`](ByteCode/EvalEmitter.cpp)。
- 下游: [`ByteCode/InterpFrame.h`](ByteCode/InterpFrame.h)、
  [`ByteCode/Program.h`](ByteCode/Program.h)、
  [`DeclTemplate.h`](../../include/clang/AST/DeclTemplate.h)。
- 关键类/函数: `class InterpState`, `cleanup`, `getCurrentFrame`,
  `BottomFrame`。

#### 3.12.2 字节码发射器与编译器

[`ByteCode/ByteCodeEmitter.cpp`](ByteCode/ByteCodeEmitter.cpp) —
`ByteCodeEmitter` 实现: 把 AST 编译成字节码 (`compileFunc`), 链接
到 `Program`。
- 上游: [`ByteCode/Context.cpp`](ByteCode/Context.cpp)。
- 下游: [`ByteCode/Compiler.h`](ByteCode/Compiler.h)、
  [`ByteCode/Program.h`](ByteCode/Program.h)、
  [`ByteCode/Opcode.h`](ByteCode/Opcode.h)。
- 关键类/函数: `ByteCodeEmitter::compileFunc`, `emitLabel`,
  `getLabel`。

[`ByteCode/Compiler.cpp`](ByteCode/Compiler.cpp) —
`Compiler<Emitter>`: AST → Bytecode 编译主体 (极大文件), 包含所有
Expr 的 emitter。
- 上游: [`ByteCode/Context.cpp`](ByteCode/Context.cpp)。
- 下游: [`ByteCode/ByteCodeEmitter.h`](ByteCode/ByteCodeEmitter.h)、
  [`ByteCode/EvalEmitter.h`](ByteCode/EvalEmitter.h)、
  [`ExprConstShared.h`](ExprConstShared.h)、
  [`FixedPoint.h`](ByteCode/FixedPoint.h)。
- 关键类/函数: `class Compiler<Emitter>`, `VisitDeclStmt`,
  `VisitBinaryOperator`, `compileFunc`, `VariableScope`。

[`ByteCode/EvalEmitter.cpp`](ByteCode/EvalEmitter.cpp) —
`EvalEmitter` 实现: 边发射边求值 (用于 evaluateExpr/Decl 而不保存
字节码)。
- 上游: [`ByteCode/Context.cpp`](ByteCode/Context.cpp)。
- 下游: [`ByteCode/InterpState.h`](ByteCode/InterpState.h)、
  [`ByteCode/IntegralAP.h`](ByteCode/IntegralAP.h)、
  [`ByteCode/Interp.h`](ByteCode/Interp.h)。
- 关键类/函数: `EvalEmitter::interpretExpr`,
  `EvalEmitter::interpretDecl`, `Cleanup`。

[`ByteCode/Disasm.cpp`](ByteCode/Disasm.cpp) — 字节码反汇编: 把
`Function` 的字节码 dump 成人类可读文本 (调试)。
- 上游: [`ByteCode/Context.cpp`](ByteCode/Context.cpp)。
- 下游: [`ByteCode/Opcode.h`](ByteCode/Opcode.h)、
  [`ByteCode/Program.h`](ByteCode/Program.h)、
  [`ByteCode/MemberPointer.h`](ByteCode/MemberPointer.h)。
- 关键类/函数: `Function::dump`, `printArg`, `disasm`。

#### 3.12.3 内存模型 (Pointer/Block/Record/Descriptor)

[`ByteCode/Pointer.cpp`](ByteCode/Pointer.cpp) — `Pointer` 类实现:
块内偏移、字段访问、Record/数组下标、cast 验证。
- 上游: [`ByteCode/Interp.cpp`](ByteCode/Interp.cpp)、
  [`ByteCode/EvalEmitter.cpp`](ByteCode/EvalEmitter.cpp)。
- 下游: [`ByteCode/Record.h`](ByteCode/Record.h)、
  [`ByteCode/MemberPointer.h`](ByteCode/MemberPointer.h)、
  [`RecordLayout.h`](../../include/clang/AST/RecordLayout.h)。
- 关键类/函数: `Pointer::atField`, `Pointer::advance`,
  `Pointer::toPointer`, `PtrView`。

[`ByteCode/InterpBlock.cpp`](ByteCode/InterpBlock.cpp) — `Block`/
`DeadBlock` 类实现: 分配/释放、descriptor 元数据、活跃 pointer 跟
踪。
- 上游: [`ByteCode/DynamicAllocator.cpp`](ByteCode/DynamicAllocator.cpp)。
- 下游: [`ByteCode/Pointer.h`](ByteCode/Pointer.h)。
- 关键类/函数: `Block::addPointer`, `Block::removePointers`,
  `Block::invokeDtor`。

[`ByteCode/Record.cpp`](ByteCode/Record.cpp) — `Record` 类实现:
字段映射、基类/虚基类布局、bitfield 信息。
- 上游: [`ByteCode/Compiler.cpp`](ByteCode/Compiler.cpp)。
- 下游: [`ASTContext.h`](../../include/clang/AST/ASTContext.h)。
- 关键类/函数: `Record::Field`, `Record::Base`,
  `Record::VirtualBase`, `getName`。

[`ByteCode/Descriptor.cpp`](ByteCode/Descriptor.cpp) — `Descriptor`
实现: 类型/字段/全局的元数据 + ctor/dtor 函数指针表。
- 上游: [`ByteCode/Compiler.cpp`](ByteCode/Compiler.cpp)。
- 下游: [`ByteCode/Record.h`](ByteCode/Record.h)、
  [`ByteCode/MemberPointer.h`](ByteCode/MemberPointer.h)、
  [`ByteCode/Integral.h`](ByteCode/Integral.h)。
- 关键类/函数: `class Descriptor`, `BlockCtorFn`, `BlockDtorFn`,
  `GlobalInlineDescriptor`。

[`ByteCode/InitMap.cpp`](ByteCode/InitMap.cpp) — `InitMap` 实现:
跟踪原始数组中哪些元素已初始化 (bitfield)。
- 上游: [`ByteCode/Descriptor.cpp`](ByteCode/Descriptor.cpp)。
- 下游: [`ByteCode/InitMap.h`](ByteCode/InitMap.h)。
- 关键类/函数: `InitMap::initializeElement`,
  `InitMap::isElementInitialized`, `InitMapPtr`。

[`ByteCode/DynamicAllocator.cpp`](ByteCode/DynamicAllocator.cpp) —
`DynamicAllocator` 实现: 跟踪并释放 constexpr 解释期间的
`new`/`new[]` 分配。
- 上游: [`ByteCode/InterpState.cpp`](ByteCode/InterpState.cpp)。
- 下游: [`ByteCode/InterpBlock.h`](ByteCode/InterpBlock.h)。
- 关键类/函数: `DynamicAllocator::cleanup`, `AllocationSite`,
  `Form`。

#### 3.12.4 调用栈与栈实现

[`ByteCode/InterpFrame.cpp`](ByteCode/InterpFrame.cpp) —
`InterpFrame` 实现: 调用帧 — 局部变量 + This 指针 + 返回地址。
- 上游: [`ByteCode/Interp.cpp`](ByteCode/Interp.cpp)。
- 下游: [`ByteCode/Function.h`](ByteCode/Function.h)、
  [`ByteCode/Pointer.h`](ByteCode/Pointer.h)、
  [`ExprCXX.h`](../../include/clang/AST/ExprCXX.h)。
- 关键类/函数: `InterpFrame::InterpFrame`, `describe`,
  `getFrame`。

[`ByteCode/InterpStack.cpp`](ByteCode/InterpStack.cpp) —
`InterpStack` 实现: 链式 chunk 的生长栈 + 原始类型 destroy-on-pop。
- 上游: [`ByteCode/InterpFrame.cpp`](ByteCode/InterpFrame.cpp)、
  [`ByteCode/InterpState.cpp`](ByteCode/InterpState.cpp)。
- 下游: [`ByteCode/Pointer.h`](ByteCode/Pointer.h)、
  [`ByteCode/MemberPointer.h`](ByteCode/MemberPointer.h)。
- 关键类/函数: `InterpStack::~InterpStack`, `grow`, `ItemTypes`。

#### 3.12.5 原始类型包装 (Integral/Float/Bool/...)

[`ByteCode/Integral.h`](ByteCode/Integral.h) — `Integral<Bits,Signed>`:
固定位宽整数包装 (模板, 隐式内存布局)。
- 上游: [`ByteCode/Boolean.h`](ByteCode/Boolean.h)、
  [`ByteCode/Char.h`](ByteCode/Char.h)、
  [`ByteCode/Interp.h`](ByteCode/Interp.h)。
- 下游: [`ByteCode/Descriptor.h`](ByteCode/Descriptor.h)、
  [`ByteCode/InterpBlock.h`](ByteCode/InterpBlock.h)、
  [`ByteCode/Primitives.h`](ByteCode/Primitives.h)。
- 关键类/函数: `class Integral<Bits,Signed>`, `toAPSInt`,
  `Compare`, `operator`。

[`ByteCode/IntegralAP.h`](ByteCode/IntegralAP.h) —
`IntegralAP<Signed>`: 任意精度整数包装, 与 `APInt`/`APSInt` 互操作。
- 上游: [`ByteCode/Interp.cpp`](ByteCode/Interp.cpp)、
  [`ByteCode/Compiler.cpp`](ByteCode/Compiler.cpp)。
- 下游: [`APValue.h`](../../include/clang/AST/APValue.h)、
  [`llvm/ADT/APSInt.h`](../../../llvm/include/llvm/ADT/APSInt.h)、
  [`ByteCode/Primitives.h`](ByteCode/Primitives.h)。
- 关键类/函数: `class IntegralAP<Signed>`, `toAPSInt`, `from`。

[`ByteCode/Boolean.h`](ByteCode/Boolean.h) — `Boolean`: VM 中
`bool` 包装类。
- 上游: [`ByteCode/Integral.h`](ByteCode/Integral.h)、
  [`ByteCode/Interp.h`](ByteCode/Interp.h)、
  [`ByteCode/Primitives.h`](ByteCode/Primitives.h)。
- 下游: [`APValue.h`](../../include/clang/AST/APValue.h)、
  [`ComparisonCategories.h`](../../include/clang/AST/ComparisonCategories.h)。
- 关键类/函数: `class Boolean`, `operator<`, `Compare`。

[`ByteCode/Char.h`](ByteCode/Char.h) — `Char<Signed>`: 单字节整数
字符包装 (与 `Integral` 共享代码)。
- 上游: [`ByteCode/Integral.h`](ByteCode/Integral.h)、
  [`ByteCode/Interp.h`](ByteCode/Interp.h)。
- 下游: `<limits>`。
- 关键类/函数: `class Char<Signed>`, `CharRepr<Signed>`。

[`ByteCode/Floating.cpp`](ByteCode/Floating.cpp) — `Floating` 实现:
主要是 `operator<<` / `getSwappedBytes`。
- 上游: [`ByteCode/Interp.cpp`](ByteCode/Interp.cpp)、
  [`ByteCode/Compiler.cpp`](ByteCode/Compiler.cpp)。
- 下游: [`ByteCode/Floating.h`](ByteCode/Floating.h)。
- 关键类/函数: `operator<<`, `getSwappedBytes`。

[`ByteCode/Floating.h`](ByteCode/Floating.h) — `Floating`:
IEEE-754 浮点包装 (与 `APFloat` 互操作)。
- 上游: [`ByteCode/Interp.h`](ByteCode/Interp.h)、
  [`ByteCode/Program.h`](ByteCode/Program.h)、
  [`ByteCode/InterpState.h`](ByteCode/InterpState.h)。
- 下游: [`ByteCode/Primitives.h`](ByteCode/Primitives.h)、
  [`APValue.h`](../../include/clang/AST/APValue.h)、
  [`llvm/ADT/APFloat.h`](../../../llvm/include/llvm/ADT/APFloat.h)。
- 关键类/函数: `class Floating`, `APFloat`, `Compare`,
  `operator`。

[`ByteCode/FixedPoint.h`](ByteCode/FixedPoint.h) — `FixedPoint`:
定点数包装 (与 `APFixedPoint` 互操作)。
- 上游: [`ByteCode/Interp.h`](ByteCode/Interp.h)、
  [`ByteCode/Compiler.cpp`](ByteCode/Compiler.cpp)。
- 下游: [`APValue.h`](../../include/clang/AST/APValue.h)、
  [`llvm/ADT/APFixedPoint.h`](../../../llvm/include/llvm/ADT/APFixedPoint.h)。
- 关键类/函数: `class FixedPoint`, `zero`, `from`, `Compare`。

[`ByteCode/MemberPointer.cpp`](ByteCode/MemberPointer.cpp) —
`MemberPointer` 实现: 指针 → 具体 Decl + base-derived 路径 + 偏移。
- 上游: [`ByteCode/Interp.cpp`](ByteCode/Interp.cpp)。
- 下游: [`ByteCode/Context.h`](ByteCode/Context.h)、
  [`ByteCode/Program.h`](ByteCode/Program.h)、
  [`ByteCode/Record.h`](ByteCode/Record.h)。
- 关键类/函数: `MemberPointer::toPointer`, `getDecl`,
  `PtrOffset`。

[`ByteCode/PrimType.cpp`](ByteCode/PrimType.cpp) — `primSize()` 工
具: 给定 `PrimType` 返回 `sizeof(T)`。
- 上游: [`ByteCode/Program.cpp`](ByteCode/Program.cpp)、
  [`ByteCode/Descriptor.cpp`](ByteCode/Descriptor.cpp)。
- 下游: [`ByteCode/Boolean.h`](ByteCode/Boolean.h)、
  [`ByteCode/Char.h`](ByteCode/Char.h)、
  [`ByteCode/Floating.h`](ByteCode/Floating.h)。
- 关键类/函数: `primSize`, `TYPE_SWITCH`。

#### 3.12.6 字节码 (源/调度/反汇编)

[`ByteCode/Interp.cpp`](ByteCode/Interp.cpp) — `Interp` 实现: opcode
调度 + Jmp/Jt/Jf/Ret/Call 等核心 VM 循环。
- 上游: [`ByteCode/Context.cpp`](ByteCode/Context.cpp)、
  [`Sema/SemaExpr.cpp`](../Sema/SemaExpr.cpp)。
- 下游: [`ByteCode/Opcode.h`](ByteCode/Opcode.h)、
  [`ByteCode/InterpFrame.h`](ByteCode/InterpFrame.h)、
  [`ByteCode/InterpStack.h`](ByteCode/InterpStack.h)、
  [`ExprConstShared.h`](ExprConstShared.h)。
- 关键类/函数: `Run`, `Jmp`, `Jt`, `Jf`, `Ret`, `Call`。

[`ByteCode/InterpBuiltin.cpp`](ByteCode/InterpBuiltin.cpp) —
`__builtin_*` 在 VM 内的实现 (大半 intrinsics: `memcpy`/`strlen`/
`abs`/…)。
- 上游: [`ByteCode/Interp.cpp`](ByteCode/Interp.cpp)。
- 下游: [`ExprConstShared.h`](ExprConstShared.h)、
  [`ByteCode/InterpBuiltinBitCast.h`](ByteCode/InterpBuiltinBitCast.h)、
  [`ByteCode/Program.h`](ByteCode/Program.h)、
  [`OSLog.h`](../../include/clang/AST/OSLog.h)。
- 关键类/函数: `InterpretBuiltin`, `isNoopBuiltin`, `BIas_const`,
  `BI__builtin_memcpy`。

[`ByteCode/InterpBuiltinBitCast.cpp`](ByteCode/InterpBuiltinBitCast.cpp)
— `__builtin_bit_cast` 在 VM 内的实现 (字节级 bit manipulation)。
- 上游: [`ByteCode/Interp.cpp`](ByteCode/Interp.cpp)。
- 下游: [`ByteCode/BitcastBuffer.h`](ByteCode/BitcastBuffer.h)、
  [`ByteCode/Record.h`](ByteCode/Record.h)、
  [`ByteCode/InterpState.h`](ByteCode/InterpState.h)。
- 关键类/函数: `DoBitCast`, `DoBitCastPtr`, `DoMemcpy`,
  `readPointerToBuffer`。

[`ByteCode/InterpBuiltinObjectSize.cpp`](ByteCode/InterpBuiltinObjectSize.cpp)
— `__builtin_object_size` / `__builtin_dynamic_object_size` 的 VM
实现。
- 上游: [`ByteCode/Interp.cpp`](ByteCode/Interp.cpp)。
- 下游: [`ByteCode/InterpHelpers.h`](ByteCode/InterpHelpers.h)、
  [`ByteCode/Pointer.h`](ByteCode/Pointer.h)、
  [`ByteCode/Record.h`](ByteCode/Record.h)。
- 关键类/函数: `InterpretBuiltin`, `Regular`, `IgnoreBaseCasts`,
  `SurroundingArray`。

[`ByteCode/EvaluationResult.cpp`](ByteCode/EvaluationResult.cpp) —
`EvaluationResult` 实现: 把 VM 求值结果转化为 `APValue` (供外部使
用)。
- 上游: [`ByteCode/EvalEmitter.cpp`](ByteCode/EvalEmitter.cpp)。
- 下游: [`ByteCode/InterpState.h`](ByteCode/InterpState.h)、
  [`ByteCode/Pointer.h`](ByteCode/Pointer.h)、
  [`ByteCode/Record.h`](ByteCode/Record.h)。
- 关键类/函数: `EvaluationResult::toAPValue`,
  `DiagnoseUninitializedSubobject`, `CheckFieldsInitialized`。

[`ByteCode/Opcodes.td`](ByteCode/Opcodes.td) — TableGen: 定义
constexpr VM 的所有 opcode + 类型/参数模板, 生成 `Opcodes.inc`。
- 上游: [`CMakeLists.txt`](CMakeLists.txt) (`clang_tablegen
  Opcodes.inc`)。
- 下游: [`ByteCode/Opcode.h`](ByteCode/Opcode.h) (`#include
  "Opcodes.inc"`)、[`ByteCode/Interp.cpp`](ByteCode/Interp.cpp) /
  [`Compiler.cpp`](ByteCode/Compiler.cpp) /
  [`EvalEmitter.cpp`](ByteCode/EvalEmitter.cpp) /
  [`Disasm.cpp`](ByteCode/Disasm.cpp)。
- 关键类/函数: `def Jmp`, `def Call`, `def Ret`, `AluOpcode`,
  `IntegerTypeClass`, `FloatTypeClass`。

[`ByteCode/Opcode.h`](ByteCode/Opcode.h) — `Opcode` 枚举声明 (由
`Opcodes.inc` 展开)。
- 上游: [`ByteCode/Interp.cpp`](ByteCode/Interp.cpp)、
  [`ByteCode/Compiler.cpp`](ByteCode/Compiler.cpp)、
  [`ByteCode/Disasm.cpp`](ByteCode/Disasm.cpp)。
- 下游: `Opcodes.inc` (TableGen 生成)。
- 关键类/函数: `enum Opcode`, `GET_OPCODE_NAMES`。

[`ByteCode/Source.cpp`](ByteCode/Source.cpp) — `SourceInfo` 实现:
把 `CodePtr`/`Decl`/`Expr` 映射回源代码位置。
- 上游: [`ByteCode/Interp.cpp`](ByteCode/Interp.cpp)、
  [`ByteCode/Compiler.cpp`](ByteCode/Compiler.cpp)。
- 下游: [`Expr.h`](../../include/clang/AST/Expr.h)。
- 关键类/函数: `SourceInfo::getLoc`, `SourceInfo::getRange`,
  `SourceInfo`。

[`ByteCode/BitcastBuffer.cpp`](ByteCode/BitcastBuffer.cpp) —
`BitcastBuffer` 实现: 位级缓冲, 支持任意 endianness 的 push。
- 上游: [`ByteCode/InterpBuiltinBitCast.cpp`](ByteCode/InterpBuiltinBitCast.cpp)。
- 下游: [`ByteCode/BitcastBuffer.h`](ByteCode/BitcastBuffer.h)、
  [`llvm/ADT/STLExtras.h`](../../../llvm/include/llvm/ADT/STLExtras.h)。
- 关键类/函数: `BitcastBuffer::pushData`, `Bits`, `Bytes`,
  `bitof`。

[`ByteCode/Function.cpp`](ByteCode/Function.cpp) — `Function` 类实
现: 函数元数据 (params/has-this/RVO/source) + 字节码容器。
- 上游: [`ByteCode/Program.cpp`](ByteCode/Program.cpp)。
- 下游: [`ByteCode/Program.h`](ByteCode/Program.h)、
  [`ASTLambda.h`](../../include/clang/AST/ASTLambda.h)。
- 关键类/函数: `Function::Function`, `ParamDescriptor`,
  `FunctionKind`, `IsLambdaCallOperator`。

[`ByteCode/DeclOrExpr.h`](ByteCode/DeclOrExpr.h) — `DeclOrExpr`:
`PointerUnion<Decl*, Expr*>` — 一个值既可以是 Decl 也可以是 Expr。
- 上游: [`ByteCode/Descriptor.h`](ByteCode/Descriptor.h)、
  [`ByteCode/EvaluationResult.h`](ByteCode/EvaluationResult.h)。
- 下游: [`Decl.h`](../../include/clang/AST/Decl.h)、
  [`Expr.h`](../../include/clang/AST/Expr.h)、
  [`llvm/ADT/PointerUnion.h`](../../../llvm/include/llvm/ADT/PointerUnion.h)。
- 关键类/函数: `class DeclOrExpr`, `asDecl`, `asExpr`,
  `asValueDecl`。

---

## §4. 关键调用链

### 4.1 Sema 创建 AST 节点链

```
Sema::ActOnDeclarator / ActOnIdentifierExpr / ActOnCallExpr
  [clang/lib/Sema/...]
  │
  ├─► Decl::Create (DeclBase.cpp)
  │    └─ ASTContext::Allocate (ASTContext.cpp)
  │         ├─ 类型 unique 化 (get<Type>)
  │         └─ DeclContext::addDecl (DeclBase.cpp)
  │
  ├─► Type::getAs / getCanonicalType (Type.cpp)
  │    └─ ASTContext::get<Type> (ASTContext.cpp)
  │
  └─► Expr::Create (Expr.cpp)
       └─ ASTContext::Allocate
            │
            ▼
  完全类型化 AST
```

### 4.2 Layout 计算链

```
ASTContext::getRecordLayout (ASTContext.cpp)
  │
  ├─► RecordLayoutBuilder::Layout (RecordLayoutBuilder.cpp)
  │    ├─ ItaniumCXXABI::getMemberPointerInfo (ItaniumCXXABI.cpp)
  │    │    OR
  │    └─ MicrosoftCXXABI::getMemberPointerInfo (MicrosoftCXXABI.cpp)
  │
  └─► 写回 ASTRecordLayout (RecordLayout.cpp)
       │
       ▼
  ASTRecordLayout 缓存 → CodeGen CGR 查 offset/size
```

### 4.3 VTable 构建链

```
CodeGen CGVTables::GenerateVTable (clang/lib/CodeGen/CGVTables.cpp)
  │
  ├─► VTableBuilder::BuildVTable (VTableBuilder.cpp)
  │    ├─ ItaniumVTableContext::computeVTable
  │    │    OR
  │    └─ MicrosoftVTableContext::computeVTable
  │    └─ ItaniumMangleContext::mangle (ItaniumMangle.cpp)
  │
  └─► 输出 vtable 全局符号 + VTT (VTTBuilder.cpp)
       └─ VTTBuilder::BuildVTT (VTTBuilder.cpp)
```

### 4.4 Name Mangling 链

```
CodeGenModule::GetMangledName (clang/lib/CodeGen/CodeGenModule.cpp)
  │
  ├─► MangleContext::createMangleContext (Mangle.cpp)
  │    ├─► ItaniumMangleContext::mangle (ItaniumMangle.cpp)
  │    │    ├─ mangleName (template params / operators / ...)
  │    │    ├─ mangleType (qualifiers / pointers / refs / ...)
  │    │    └─ mangleExpression / mangleMember
  │    └─► MicrosoftMangleContext::mangle (MicrosoftMangle.cpp)
  │
  └─► 输出 mangled symbol 给 LLVM IR emitter
```

### 4.5 constexpr 求值链 (ByteCode 新解释器)

```
Sema::CheckForConstantExpression / EvaluateAsConstantExpr
  [clang/lib/Sema/...]
  │
  ├─► ByteCode::Context::evaluate / evaluateAsRValue
  │    [ByteCode/Context.cpp]
  │    │
  │    ├─► Compiler<ByteCodeEmitter>::compile (ByteCode/Compiler.cpp)
  │    │    └─► ByteCodeEmitter::emit (ByteCode/ByteCodeEmitter.cpp)
  │    │         └─► 函数字节码链接到 Program
  │    │
  │    └─► Interp::Run (ByteCode/Interp.cpp)
  │         └─► 调度 opcode (Jmp/Jt/Call/...) 到 Operand Stack
  │              │
  │              ├─► Integral<Bits> / Floating / Boolean (prim 类型包装)
  │              ├─► Pointer (Pointer.cpp)
  │              ├─► Record::getField (Record.cpp)
  │              ├─► InterpBuiltin::InterpretBuiltin
  │              │    └─► InterpBuiltinBitCast::DoBitCast
  │              │
  │              └─► EvaluationResult::toAPValue (EvaluationResult.cpp)
  │                   └─► APValue (APValue.h)
  │
  └─► 求值结果回传给 Sema (constant value)
       │
       ▼
  CodeGen 用 APValue 当 const initializer
```

### 4.6 旧版 AST-walker constexpr 求值链

```
Sema::EvaluateAsConstantExpr → Expr::EvaluateAsInt (Expr.cpp)
  │
  ├─► ExprConstant::Evaluate (ExprConstant.cpp)
  │    ├─ Visit 各类 Expr 子类
  │    │    ├─ CallExpr → CallExpr::isConstant
  │    │    ├─ BinaryOperator → EvaluateBinary
  │    │    └─ LambdaExpr → EvaluateLambda
  │    └─ 产出 APValue
  │
  └─► 求值结果回传给 Sema
```

### 4.7 ODR 检查链

```
ASTImporter::Import (ASTImporter.cpp) [clangd / module 合并]
  │
  ├─► ASTStructuralEquivalence::IsEquivalent
  │    [ASTStructuralEquivalence.cpp]
  │    ├─► ODRHash::CalculateHash (ODRHash.cpp)
  │    └─► 比较两个 Decl 的 hash 是否一致
  │
  └─► 不一致 → ODRDiagsEmitter::diagnoseMismatch
       [ODRDiagsEmitter.cpp]
       └─► 收集宏展开栈 + 诊断 (DataCollection.cpp)
            │
            ▼
       输出 ODR violation 诊断
```

### 4.8 格式串检查链

```
Sema::CheckFormatString (SemaChecking.cpp)
  │
  ├─► PrintfFormatStringHandler::HandlePrintfSpecifier
  │    [PrintfFormatString.cpp]
  │    ├─► FormatString::ParseFormatSpecifier
  │    │    [FormatString.cpp]
  │    └─► 检查每个转换说明符的参数类型
  │
  └─► 不匹配 → 诊断
       │
       ▼
  类似 ScanfFormatStringHandler (scanf/fscanf 系列)
       │
       ▼
  同样 OSLogFormatStringHandler (NSLog/os_log)
```

### 4.9 注释分析链

```
Parser 见到 /// 或 /** ... */
  │
  ▼
RawCommentList::addComment (RawCommentList.cpp)
  │
  ├─► CommentLexer::lex (CommentLexer.cpp)
  │    └─► tokenize (含 \brief / \param / @command)
  │
  ├─► CommentParser::parseComment (CommentParser.cpp)
  │    └─► 构造 Comment AST (CommentNodes.inc)
  │
  ├─► CommentSema::checkDeclReference (CommentSema.cpp)
  │    └─► resolve \param 引用, 检查 HTML tag
  │
  └─► CommentBriefParser::parseBriefComment (CommentBriefParser.cpp)
       └─► 提取 brief 文本 (供 IDE hover)
```

---

## §5. 推荐阅读顺序

### 阶段 1: AST 基础与公共容器 (1.5 小时)
1. [`ASTContext.cpp`](ASTContext.cpp)
2. [`DeclBase.cpp`](DeclBase.cpp)
3. [`ASTTypeTraits.cpp`](ASTTypeTraits.cpp)
4. [`ASTConsumer.cpp`](ASTConsumer.cpp)

### 阶段 2: 通用声明系统 (2 小时)
- [`Decl.cpp`](Decl.cpp)
- [`DeclarationName.cpp`](DeclarationName.cpp)
- [`Type.cpp`](Type.cpp)
- [`TemplateBase.cpp`](TemplateBase.cpp)
- [`TemplateName.cpp`](TemplateName.cpp)
- [`NestedNameSpecifier.cpp`](NestedNameSpecifier.cpp)

### 阶段 3: 表达式系统 (2 小时)
- [`Expr.cpp`](Expr.cpp)
- [`Stmt.cpp`](Stmt.cpp)
- [`ExprCXX.cpp`](ExprCXX.cpp)
- [`ComputeDependence.cpp`](ComputeDependence.cpp)
- [`ExprClassification.cpp`](ExprClassification.cpp)

### 阶段 4: C++ 与 ObjC 扩展 Decl/Stmt (2 小时)
- [`DeclCXX.cpp`](DeclCXX.cpp)
- [`DeclTemplate.cpp`](DeclTemplate.cpp)
- [`DeclObjC.cpp`](DeclObjC.cpp)
- [`DeclPrinter.cpp`](DeclPrinter.cpp)
- [`StmtPrinter.cpp`](StmtPrinter.cpp)

### 阶段 5: 模板 / Concepts / ABI (2 小时)
- [`ASTConcept.cpp`](ASTConcept.cpp)
- [`ComparisonCategories.cpp`](ComparisonCategories.cpp)
- [`Mangle.cpp`](Mangle.cpp)
- [`ItaniumMangle.cpp`](ItaniumMangle.cpp) (极大)
- [`MicrosoftMangle.cpp`](MicrosoftMangle.cpp) (极大)

### 阶段 6: Layout / ODR / 导入 (2 小时)
- [`RecordLayoutBuilder.cpp`](RecordLayoutBuilder.cpp)
- [`RecordLayout.cpp`](RecordLayout.cpp)
- [`VTableBuilder.cpp`](VTableBuilder.cpp)
- [`VTTBuilder.cpp`](VTTBuilder.cpp)
- [`ODRHash.cpp`](ODRHash.cpp)
- [`ODRDiagsEmitter.cpp`](ODRDiagsEmitter.cpp)
- [`ASTStructuralEquivalence.cpp`](ASTStructuralEquivalence.cpp)
- [`ASTImporter.cpp`](ASTImporter.cpp)

### 阶段 7: Constexpr 解释器 — 字节码核心 (2 小时)
- [`ByteCode/Context.cpp`](ByteCode/Context.cpp)
- [`ByteCode/State.cpp`](ByteCode/State.cpp)
- [`ByteCode/Program.cpp`](ByteCode/Program.cpp)
- [`ByteCode/Function.cpp`](ByteCode/Function.cpp)
- [`ByteCode/Pointer.cpp`](ByteCode/Pointer.cpp)
- [`ByteCode/Descriptor.cpp`](ByteCode/Descriptor.cpp)
- [`ByteCode/Record.cpp`](ByteCode/Record.cpp)

### 阶段 8: Constexpr 字节码编译与求值 (3 小时)
- [`ByteCode/Compiler.cpp`](ByteCode/Compiler.cpp) (极大)
- [`ByteCode/ByteCodeEmitter.cpp`](ByteCode/ByteCodeEmitter.cpp)
- [`ByteCode/EvalEmitter.cpp`](ByteCode/EvalEmitter.cpp)
- [`ByteCode/Interp.cpp`](ByteCode/Interp.cpp)
- [`ByteCode/Opcodes.td`](ByteCode/Opcodes.td)
- [`ByteCode/Opcode.h`](ByteCode/Opcode.h)

### 阶段 9: 旧版常量求值 + 共享工具 (1 小时)
- [`ExprConstant.cpp`](ExprConstant.cpp)
- [`APValue.cpp`](APValue.cpp)
- [`ExprConstShared.h`](ExprConstShared.h)
- [`InterpShared.cpp`](ByteCode/InterpShared.cpp)

### 阶段 10: 注释 / 属性 / 平台扩展 (2 小时)
- [`RawCommentList.cpp`](RawCommentList.cpp)
- [`CommentLexer.cpp`](ByteCode/../CommentLexer.cpp) (实为 CommentLexer.cpp)
- [`CommentParser.cpp`](CommentParser.cpp)
- [`CommentSema.cpp`](CommentSema.cpp)
- [`AttrImpl.cpp`](AttrImpl.cpp)
- [`OSLog.cpp`](OSLog.cpp)
- [`FormatString.cpp`](FormatString.cpp)
- [`PrintfFormatString.cpp`](PrintfFormatString.cpp)
- [`HLSLResource.cpp`](HLSLResource.cpp)
- [`Randstruct.cpp`](Randstruct.cpp)

### 阶段 11: 辅助分析 (1 小时)
- [`InheritViz.cpp`](InheritViz.cpp)
- [`ParentMap.cpp`](ParentMap.cpp)
- [`ParentMapContext.cpp`](ParentMapContext.cpp)
- [`ExternalASTMerger.cpp`](ExternalASTMerger.cpp)
- [`DynamicRecursiveASTVisitor.cpp`](DynamicRecursiveASTVisitor.cpp)
- [`JSONNodeDumper.cpp`](JSONNodeDumper.cpp)
- [`TextNodeDumper.cpp`](TextNodeDumper.cpp)

---

## §6. 常用操作指南

### 6.1 添加新 AST 节点 (Decl/Type/Expr/Stmt)

1. **Decl 节点**:
   - 在 [`clang/include/clang/AST/DeclNodes.td`](../../include/clang/AST/DeclNodes.td)
     用 `def X : Decl<...>` 加节点 (含父类 + 字段)。
   - 在 [`Decl*.cpp`](DeclBase.cpp) 加 `Create` 工厂与 getter/setter。
   - 在 [`DeclPrinter.cpp`](DeclPrinter.cpp) 加 pretty-print。
   - 在 [`DeclBase.h`](../../include/clang/AST/DeclBase.h) 或新
     `DeclX.h` 加 `classof` / traversal helper。
2. **Type 节点**: 在
   [`TypeNodes.td`](../../include/clang/AST/TypeNodes.td) 加; 在
   [`Type.cpp`](Type.cpp) 加 impl; 在
   [`TypePrinter.cpp`](TypePrinter.cpp) 加打印。
3. **Expr/Stmt 节点**: 同上, 分别用
   [`ExprNodes.td`](../../include/clang/AST/StmtNodes.td) / 等等。
4. 测试: `clang/test/AST/<feature>` (用 `-Xclang -ast-dump` 校验)。

### 6.2 给 ByteCode constexpr 解释器加新 builtin

1. 在 [`ByteCode/InterpBuiltin.cpp`](ByteCode/InterpBuiltin.cpp) 的
   `InterpretBuiltin` switch 加新 case。
2. 如涉及类型转换, 经
   [`ByteCode/InterpBuiltinBitCast.cpp`](ByteCode/InterpBuiltinBitCast.cpp)
   的 `DoBitCast`。
3. 测试: `clang/test/SemaCXX/constexpr-<name>.cpp` + C++23 constexpr
   测试。

### 6.3 给 ODR 检查加新检查项

1. 在 [`ODRHash.cpp`](ODRHash.cpp) 的 `Add*` 加新节点类型 (给
   Decl/Type/Stmt hash 贡献)。
2. 在 [`ASTStructuralEquivalence.cpp`](ASTStructuralEquivalence.cpp)
   加新节点比较逻辑。
3. 测试: `clang/test/Modules/odr-<feature>.cpp`。

### 6.4 调试 AST

1. 用 `clang -Xclang -ast-dump -ast-dump-all -ast-dump-filter=foo` 看
   AST。
2. 用 `clang -Xclang -ast-dump=json` 看 JSON 格式 (供工具解析)。
3. 用 `clang -Xclang -ast-dump-decls -ast-dump-filter=foo` 只看
   decls。
4. 临时 `Decl->dump()` / `Type->dump()` / `Stmt->dumpPretty()` 加
   调试输出。
5. 看 [`ParentMapContext.cpp`](ParentMapContext.cpp) 的 `getParents`
   找父节点。

### 6.5 调试 constexpr 求值

1. 用 `-fconstexpr-depth=N` 调深。
2. 用 `-fconstexpr-steps=N` 调步数。
3. 看 [`ByteCode/State.cpp`](ByteCode/State.cpp) 的 `emitDiag` 看哪
   一步失败。
4. 看 [`ByteCode/Interp.cpp`](ByteCode/Interp.cpp) 的 `Run` 入口
   trace opcode。
5. 用 `-fexperimental-strict-constexpr-backtrace` 看求值栈 (旧
   AST-walker)。

### 6.6 调试 vtable / record layout

1. 用 `clang -Xclang -fdump-record-layouts` 看 layout (Itanium) /
   `-Xclang -fdump-record-layouts-complete` 看完整。
2. 看 [`RecordLayoutBuilder.cpp`](RecordLayoutBuilder.cpp) 的
   `LayoutField` / `LayoutBase` trace。
3. 看 [`VTableBuilder.cpp`](VTableBuilder.cpp) 的
   `computeVTable`。
4. 用 `-Xclang -emit-llvm` 看 LLVM IR 的 vtable 结构。

### 6.7 调试 mangling

1. 用 `clang -Xclang -emit-llvm -S` 看 mangled symbol。
2. 用 `echo "_Z1fv" | c++filt` 反 demangle 看 Itanium 名字。
3. 看 [`ItaniumMangle.cpp`](ItaniumMangle.cpp) 的 `mangleName` /
   `mangleType` trace。
4. 看 [`Mangle.cpp`](Mangle.cpp) 的 `createMangleContext` 看 ABI 选
   择。

### 6.8 调试 AST import / clangd 跨 TU

1. 用 `clangd --check-lines=...` 看 import 行为。
2. 看 [`ASTImporter.cpp`](ASTImporter.cpp) 的 `ImportDecl` /
   `ImportType`。
3. 看 [`ASTStructuralEquivalence.cpp`](ASTStructuralEquivalence.cpp)
   的 `IsEquivalent` 看是否等价。
4. 看 [`ExternalASTMerger.cpp`](ExternalASTMerger.cpp) 看跨 context
   合并。

---

## §7. NT 注释索引

当前 `clang/lib/AST/` 下尚无 `// <NT>` 注释。已建立目录索引, 姊妹
overview:

- [`clang/lib/Basic/0-overview.md`](../Basic/0-overview.md) — Clang
  基础设置 + 共享数据
- [`clang/lib/CodeGen/0-overview.md`](../CodeGen/0-overview.md) —
  Clang AST → LLVM IR
- [`clang/lib/Driver/0-overview.md`](../Driver/0-overview.md) — Clang
  命令行驱动
- `clang/lib/Frontend/` — Clang 前端桥接 (待写)
- `clang/lib/Lex/` — Clang Lexer (待写)
- `clang/lib/Parse/` — Clang Parser (待写)
- [`clang/lib/Sema/0-overview.md`](../Sema/0-overview.md) — Clang 语
  义分析

按"少而精"原则, 加 NT 注释建议优先级:

1. [`ASTContext.cpp`](ASTContext.cpp) — 8-12 段 (AST 容器, 所有节
   点创建入口)
2. [`ByteCode/Compiler.cpp`](ByteCode/Compiler.cpp) — 8-15 段 (极大
   单文件, AST → 字节码编译核心)
3. [`ByteCode/Interp.cpp`](ByteCode/Interp.cpp) — 6-10 段 (VM 调度
   循环)
4. [`ByteCode/Context.cpp`](ByteCode/Context.cpp) — 5-7 段 (per-AST
   求值入口)
5. [`ByteCode/Pointer.cpp`](ByteCode/Pointer.cpp) — 5-7 段 (VM 内存
   模型)
6. [`ItaniumMangle.cpp`](ItaniumMangle.cpp) — 6-10 段 (极大, Itanium
   mangler)
7. [`MicrosoftMangle.cpp`](MicrosoftMangle.cpp) — 6-10 段 (极大, MSVC
   mangler)
8. [`ItaniumCXXABI.cpp`](ItaniumCXXABI.cpp) +
   [`MicrosoftCXXABI.cpp`](MicrosoftCXXABI.cpp) — 各 5-7 段
9. [`ExprConstant.cpp`](ExprConstant.cpp) — 5-8 段 (旧 AST-walker 常
   量求值)
10. [`ASTImporter.cpp`](ASTImporter.cpp) +
    [`ASTStructuralEquivalence.cpp`](ASTStructuralEquivalence.cpp) —
    各 4-6 段
11. [`VTableBuilder.cpp`](VTableBuilder.cpp) +
    [`RecordLayoutBuilder.cpp`](RecordLayoutBuilder.cpp) — 各 4-6
    段
12. [`Expr.cpp`](Expr.cpp) + [`Stmt.cpp`](Stmt.cpp) +
    [`Type.cpp`](Type.cpp) — 各 3-5 段

---

**姊妹文档**: 本目录对应 LLVM 流水线中的 **Clang 完全类型化 AST**
层。它消费 [`clang/lib/Sema/`](../Sema/0-overview.md) 的输出, 被
[`clang/lib/CodeGen/`](../CodeGen/0-overview.md) 消费生成 LLVM IR,
与 [`clang/lib/Serialization/`](../Serialization/) 配合实现 PCH 与
Module 文件, 与 [`clang/lib/StaticAnalyzer/`](../StaticAnalyzer/) 配合
做流敏感分析。Clang 用户可见的选项在
[`clang/include/clang/Driver/`](../../include/clang/Driver/)。