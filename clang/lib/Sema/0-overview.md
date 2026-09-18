<!-- <NT>overview:clang/lib/Sema/ -->

# Clang Sema 库导读 — `clang/lib/Sema/`

> 本文档梳理 `clang/lib/Sema/` 目录下所有源文件 (96 个 `.cpp/.h` + 1
> 个 [`OpenCLBuiltins.td`](OpenCLBuiltins.td), 全在顶层, 无子目录) 的
> 职责、上下游与推荐阅读顺序。
>
> 目标读者: 想理解 **Clang 语义分析层** (C/C++/ObjC/HLSL/SYCL/OpenMP/
> OpenACC/... AST 类型检查 + 名字查找 + 重载 + 模板实例化 + 诊断) 的
> 开发者, 以及要给 Clang 加新语言特性 / 新 builtin 检查 / 新 arch
> builtin validation 的人。
>
> 所有路径相对 `clang/lib/Sema/`。同名公开头文件位于
> `clang/include/clang/Sema/` (如 `Sema.h`, `SemaOpenMP.h`,
> `SemaRISCV.h` 等); 私有头文件位于同目录 `.cpp` 旁的 `.h`。

---

## §0. Clang Sema 库在编译流水线中的位置

`clang/lib/Sema` 是 Clang **语义分析** 层, 位于 Parse 之后、CodeGen 之
前。它消费 Parse 出的 AST + 解析结果, 做完整的 **类型检查** (type
checking)、**名字查找** (name lookup)、**重载解析** (overload
resolution)、**模板实例化** (template instantiation)、**属性校验**
(attribute validation)、**诊断** (diagnostics)、**代码补全** (code
completion), 然后产出**完全类型化的 AST** 给 CodeGen。

它在 Clang 内部的层次:

```
┌────────────────────────────────────────────────────────┐
│ Clang Lex + Parse  (clang/lib/Lex + clang/lib/Parse)    │
│   输出: 半解析的 AST (只有 Decl/Stmt/Expr 的 skeleton,    │
│         没有类型信息或只有部分)                          │
└────────────────────────────────────────────────────────┘
                │
                ▼
┌────────────────────────────────────────────────────────┐
│ clang/lib/Sema  (本目录) ← 你在这里                    │
│   · Sema.cpp / SemaLookup.cpp / SemaDecl.cpp: 核心驱动 │
│   · SemaExpr.cpp / SemaOverload.cpp / SemaCast.cpp:   │
│     表达式检查 + 重载 + cast                            │
│   · SemaTemplate*.cpp: 模板参数推导 + 实例化            │
│   · SemaCoroutine.cpp / SemaExceptionSpec.cpp /        │
│     CheckExprLifetime.cpp: C++20 / 异常 / 生命周期       │
│   · SemaOpenMP.cpp / SemaOpenACC*.cpp / SemaCUDA.cpp /│
│     SemaHLSL.cpp / SemaOpenCL.cpp / SemaSYCL.cpp:     │
│     pragma + accelerator + kernel validation           │
│   · SemaObjC.cpp / SemaObjCProperty.cpp: ObjC          │
│   · Sema<X>.cpp (X ∈ {X86,ARM,AMDGPU,RISCV,...,M68k}): │
│     per-arch builtin validation                        │
│   · SemaChecking.cpp: format-string / fortified / ...  │
│   · SemaCodeComplete.cpp: 代码补全                      │
│   · AnalysisBasedWarnings.cpp: flow-sensitive warnings │
└────────────────────────────────────────────────────────┘
                │
                ▼
┌────────────────────────────────────────────────────────┐
│ clang/lib/AST (clang/lib/AST/0-overview.md 待写)       │
│   完全类型化 AST + 序列化                               │
└────────────────────────────────────────────────────────┘
                │
                ▼
┌────────────────────────────────────────────────────────┐
│ clang/lib/CodeGen  (clang/lib/CodeGen/0-overview.md)   │
│   AST → LLVM IR                                        │
└────────────────────────────────────────────────────────┘
```

### §0.1 公开接口 (`clang/include/clang/Sema/`)

本目录 `.cpp` 文件依赖的 **公开头** 在
[`clang/include/clang/Sema/`](../../include/clang/Sema/), 最关键的有:

- [`Sema.h`](../../include/clang/Sema/Sema.h) — `class Sema` 的核心接口
  (构造函数、`Initialize`/`TearDown`、所有 `ActOn*`/`Build*` 入口、诊断
  builder)。
- [`SemaInternal.h`](../../include/clang/Sema/SemaInternal.h) —
  libSema 内部细节 (不在公开 API)。
- [`Lookup.h`](../../include/clang/Sema/Lookup.h) — `class LookupResult`。
- [`Scope.h`](../../include/clang/Sema/Scope.h) +
  [`ScopeInfo.h`](../../include/clang/Sema/ScopeInfo.h) — lexical scope。
- [`Initialization.h`](../../include/clang/Sema/Initialization.h) —
  InitList / 初始化解析。
- [`DeclSpec.h`](../../include/clang/Sema/DeclSpec.h) —
  `class DeclSpec` 声明说明符。
- [`ParsedAttr.h`](../../include/clang/Sema/ParsedAttr.h) —
  `class ParsedAttr` 原始属性。
- [`Template.h`](../../include/clang/Sema/Template.h) +
  [`TemplateDeduction.h`](../../include/clang/Sema/TemplateDeduction.h)
  — 模板推导接口。
- [`SemaOpenMP.h`](../../include/clang/Sema/SemaOpenMP.h) +
  [`SemaOpenACC.h`](../../include/clang/Sema/SemaOpenACC.h) +
  [`SemaHLSL.h`](../../include/clang/Sema/SemaHLSL.h) +
  [`SemaRISCV.h`](../../include/clang/Sema/SemaRISCV.h) +
  [`SemaX86.h`](../../include/clang/Sema/SemaX86.h) +
  [`SemaARM.h`](../../include/clang/Sema/SemaARM.h) +
  每个 per-language / per-arch 子模块的公开接口。
- [`SemaBase.h`](../../include/clang/Sema/SemaBase.h) — 所有
  `Sema<X>` 子类的基类 (`SemaObjC`、`SemaOpenCL`、`SemaHLSL`、
  `SemaSYCL`、`SemaSwift`、`SemaCUDA`、`SemaAMDGPU`、`SemaX86` 等都派生)。

### §0.2 与 [`clang/lib/AST/`](../AST/) 的关系

Sema 消费 [`clang/lib/AST/`](../AST/) 的类 (`Decl`, `Stmt`, `Expr`,
`Type`, `QualType`, `ASTContext` 等) 来**构造 / 校验**它们; AST 不感知
Sema, 但提供底座。Sema 调 `ASTContext::get<...>()`、`Decl::set*`、
`ASTNodeImporter`、`RecursiveASTVisitor` 等来构建完整树。**反过来**,
Sema 也会 emit warning 触发 AST 上的访问者 (例如
[`AnalysisBasedWarnings.cpp`](AnalysisBasedWarnings.cpp) 用
[`clang/lib/Analysis`](../Analysis/) 的 CFG 做
`-Wunreachable-code`/`-Wnull-dereference`)。

### §0.3 与 [`clang/lib/Parse/`](../Parse/) 的关系

[`Parse/`](../Parse/) 是 Sema 的 **唯一外部调用者**: Parse 每完成一个
声明 / 表达式 / 语句, 就调 `Sema::ActOnDeclarator`、
`ActOnIdentifierExpr`、`ActOnIfStmt`、`ActOnTagStartDefinition` 等。本
目录所有 `Sema::ActOn*` / `Build*` 方法**几乎只在 Parse / ParseAST
路径上被调用**。

---

## §1. 编译流水线概览

```
clang::Parser::ParseAST
  └─ Parser::ParseDeclOrFunctionDef / ParseStatement / ParseExpression
       └─ 调用 Sema::ActOn* / Build* 入口
            │
            ▼
┌────────────────────────────────────────────────────────┐
│ Sema 调度层 (Sema.cpp / SemaDecl.cpp / SemaExpr.cpp /  │
│ SemaStmt.cpp / SemaLookup.cpp)                          │
│   · ActOnDeclarator → getTypeForDeclarator →          │
│     ABIInfo::computeInfo + ABIArgInfo                 │
│   · ActOnTagStartDefinition → 建立 CXXRecordDecl      │
│   · ActOn*Expr → Expr 分发 (Scalar / Agg / Complex /  │
│     CXX / ObjC)                                       │
│   · ActOn*Stmt → Stmt 分发 (If / For / Switch / ...)  │
│   · LookupName / LookupQualifiedName (SemaLookup.cpp) │
└────────────────────────────────────────────────────────┘
                │
                ├─► SemaOverload.cpp (重载 + 实参转换)
                ├─► SemaInit.cpp (初始化 + narrowing)
                ├─► SemaCast.cpp (C/C++ cast)
                ├─► SemaTemplate*.cpp (模板参数 + 实例化)
                ├─► SemaType.cpp / SemaTypeTraits.cpp (类型)
                ├─► SemaDeclCXX.cpp / SemaDeclObjC.cpp (decl)
                ├─► SemaChecking.cpp (printf / fortified / builtin)
                ├─► SemaExceptionSpec.cpp / SemaCoroutine.cpp (异常+协程)
                ├─► SemaOpenMP.cpp / SemaOpenACC.cpp / SemaCUDA.cpp
                ├─► SemaHLSL.cpp / SemaOpenCL.cpp / SemaSYCL.cpp
                ├─► SemaObjC.cpp / SemaExprObjC.cpp (ObjC)
                ├─► Sema<X>.cpp (per-arch builtin 校验)
                └─► SemaCodeComplete.cpp (代码补全)
                │
                ▼
┌────────────────────────────────────────────────────────┐
│ 共同 helper:                                          │
│   · SemaBase.cpp (per-language 子类的基类)             │
│   · Scope.cpp / ScopeInfo.cpp / IdentifierResolver.cpp│
│   · JumpDiagnostics.cpp (goto 检查)                    │
│   · AnalysisBasedWarnings.cpp (CFG-based warnings)    │
│   · DelayedDiagnostic.cpp / SemaAttr.cpp (属性)        │
│   · CheckExprLifetime.cpp / SemaLifetimeSafety.h      │
│     (生命周期 + use-after-free 检查)                   │
│   · TypeLocBuilder.cpp / TreeTransform.h              │
│   · UsedDeclVisitor.h                                 │
└────────────────────────────────────────────────────────┘
                │
                ▼
   完全类型化的 AST → clang/lib/AST/ → clang/lib/CodeGen/
```

辅助入口:

- **公共 API**: [`clang/include/clang/Sema/`](../../include/clang/Sema/)
  提供 `Sema.h`、`Initialization.h`、`Lookup.h` 等, Sema 的所有公开
  hook 都从这里导出。
- **共用 helper**: `Scope` + `ScopeInfo` + `IdentifierResolver` +
  `TypeLocBuilder` + `TreeTransform` + `UsedDeclVisitor`。
- **Polymorphism hub**: `SemaBase` — 派生 `SemaObjC` / `SemaOpenCL` /
  `SemaHLSL` / `SemaSYCL` / `SemaSwift` / `SemaCUDA` / `SemaOpenACC` /
  `SemaAMDGPU` / `SemaX86` / `SemaARM` / ... 等 22 个 per-language /
  per-arch 子模块。

---

## §2. 文件目录结构

```
clang/lib/Sema/  (96 文件, 全顶层, 无子目录)
├── §3.1  Sema core / driver                    (~6 文件)
├── §3.2  Declaration emission                   (~9 文件)
├── §3.3  Type / cast / type-trait               (3 文件)
├── §3.4  Expression checking                    (~12 文件)
├── §3.5  Scope / identifier resolution          (5 文件)
├── §3.6  Diagnostics / fix-it / warnings        (~6 文件)
├── §3.7  Code completion                         (2 文件)
├── §3.8  ObjC & Pseudo-object                    (4 文件)
├── §3.9  C++ exception / coroutine / lifetime    (~5 文件)
├── §3.10 OpenMP / OpenACC / CUDA / HLSL / SYCL   (~10 文件)
├── §3.11 Per-target architecture                 (~17 文件)
├── §3.12 Other languages / API                  (~10 文件)
├── §3.13 Helper utilities                        (~5 文件)
└── §3.14 Build / misc                            (2 文件)
```

### 2.1 文件数量统计

| 区域 | 顶层 .cpp | 顶层 .h | 合计 |
|------|----------|--------|------|
| Sema core / driver | 5 | 0 | 5 |
| Declaration emission | 7 | 0 | 7 |
| Type / cast / type-trait | 3 | 0 | 3 |
| Expression checking | 9 | 0 | 9 |
| Scope / identifier resolution | 4 | 0 | 4 |
| Diagnostics / fix-it / warnings | 5 | 1 | 6 |
| Code completion | 2 | 0 | 2 |
| ObjC & Pseudo-object | 3 | 0 | 3 |
| C++ exception / coroutine / lifetime | 3 | 2 | 5 |
| OpenMP / OpenACC / CUDA / HLSL / OpenCL / SYCL | 8 | 2 | 10 |
| Per-target architecture (17 个) | 17 | 0 | 17 |
| Other languages / API | 10 | 0 | 10 |
| Helper utilities | 2 | 3 | 5 |
| Build / misc | 1 (`CMakeLists.txt`) + 1 (`.td`) | 0 | 2 |
| **总计** | **~81** | **~8** | **~96** + `.td` |

---

## §3. 文件详解

### 3.1 Sema core / driver

[`Sema.cpp`](Sema.cpp) — 顶层 driver, 实现 `Sema` 构造函数、
`Initialize()`/`TearDown()`、所有 `ActOn*`/`Build*` 入口、名字查找
helper (`LookupName`, `LookupQualifiedName`)、source manager 与
macro 展开粘合。
- 上游: 几乎所有 `.cpp` 文件经
  [`Sema.h`](../../include/clang/Sema/Sema.h);
  [`SemaDecl.cpp`](SemaDecl.cpp)、[`SemaExpr.cpp`](SemaExpr.cpp)、
  [`SemaLookup.cpp`](SemaLookup.cpp)。
- 下游: [`ASTContext.h`](../../include/clang/AST/ASTContext.h),
  [`Decl.h`](../../include/clang/AST/Decl.h),
  [`Expr.h`](../../include/clang/AST/Expr.h),
  [`Preprocessor.h`](../../include/clang/Lex/Preprocessor.h),
  [`Scope.h`](../../include/clang/Sema/Scope.h)。
- 关键类/函数: `class Sema`,
  `Sema::Sema(Preprocessor &, ASTContext &, ...)`,
  `Sema::Initialize(Preprocessor &)`,
  `Sema::TearDown()`,
  `Sema::LookupName(LookupResult &, Scope *, bool)`,
  `Sema::LookupQualifiedName(LookupResult &, DeclContext *, bool)`,
  `Sema::getCurScope()`, `Sema::AddComment`。

[`SemaLookup.cpp`](SemaLookup.cpp) — C/C++/ObjC 名字查找, unqualified /
qualified / dependent / using-directive / builtin lookup; 实现
`LookupResult` resolution (221 KB, 最大的 Sema 文件之一)。
- 上游: [`SemaExpr.cpp`](SemaExpr.cpp) (`ActOnIdentifierExpr`),
  [`SemaDecl.cpp`](SemaDecl.cpp) (`ActOnTag`),
  [`SemaExprMember.cpp`](SemaExprMember.cpp) (member access lookup)。
- 下游: [`CXXInheritance.h`](../../include/clang/AST/CXXInheritance.h),
  [`DeclCXX.h`](../../include/clang/AST/DeclCXX.h),
  [`DeclTemplate.h`](../../include/clang/AST/DeclTemplate.h),
  [`Lookup.h`](../../include/clang/Sema/Lookup.h),
  [`Sema.h`](../../include/clang/Sema/Sema.h)。
- 关键类/函数: `class LookupResult`,
  `LookupResult::configure()`, `LookupResult::resolveKind()`,
  `Sema::LookupName`, `Sema::LookupQualifiedName`,
  `Sema::LookupBuiltin`, `Sema::LookupTemplateName`。

[`SemaConsumer.cpp`](SemaConsumer.cpp) — 抽象 `SemaConsumer` class 的
anchor 定义; tiny stub (0.5 KB)。
- 上游: [`ParseAST.cpp`](../Parse/ParseAST.cpp) (实例化),
  [`SemaCodeComplete.cpp`](SemaCodeComplete.cpp)。
- 下游: [`SemaConsumer.h`](../../include/clang/Sema/SemaConsumer.h)。
- 关键类/函数: `SemaConsumer::anchor()`。

[`SemaBase.cpp`](SemaBase.cpp) — `SemaBase` helper class 实现
(`SemaObjC` / `SemaOpenCL` / `SemaHLSL` / `SemaSYCL` / `SemaSwift` /
`SemaCUDA` / `SemaAMDGPU` / `SemaX86` ... 的基类)。
- 上游: 所有 per-language `Sema<X>.cpp` 实例化 `SemaBase`。
- 下游: [`SemaBase.h`](../../include/clang/Sema/SemaBase.h),
  [`Sema.h`](../../include/clang/Sema/Sema.h),
  [`SemaCUDA.h`](../../include/clang/Sema/SemaCUDA.h)。
- 关键类/函数: `class SemaBase`, `SemaBase::SemaBase(Sema &)`,
  `SemaBase::Diag(SourceLocation, unsigned)`,
  `SemaBase::ImmediateDiagBuilder`,
  `SemaBase::SemaDiagnosticBuilder`。

[`SemaModule.cpp`](SemaModule.cpp) — C++ Modules 语义 (`module` /
`import` / `export`)、ObjC `@import`、header units、Clang header
modules。
- 上游: Parse actions `ActOnModuleDecl` / `ActOnImport`,
  [`SemaDecl.cpp`](SemaDecl.cpp) (export decls)。
- 下游: [`Decl.h`](../../include/clang/AST/Decl.h),
  [`HeaderSearch.h`](../../include/clang/Lex/HeaderSearch.h),
  [`ParsedAttr.h`](../../include/clang/Sema/ParsedAttr.h),
  [`SemaInternal.h`](../../include/clang/Sema/SemaInternal.h)。
- 关键类/函数: `Sema::ActOnGlobalModuleFragmentDecl`,
  `Sema::ActOnModuleDecl`,
  `Sema::ActOnPrivateModuleFragmentDecl`,
  `Sema::HandleStartOfHeaderUnit`,
  `Sema::ActOnAnnotModuleInclude`, `Sema::BuildModuleInclude`。

[`SemaPseudoObject.cpp`](SemaPseudoObject.cpp) — 重写伪对象表达式
(ObjC property / subscript、OpenCL `vec.s.x`、generic selection) 成
具体 getter/setter/method call。
- 上游: [`SemaExpr.cpp`](SemaExpr.cpp) (atomic property),
  [`SemaExprObjC.cpp`](SemaExprObjC.cpp),
  [`SemaExprMember.cpp`](SemaExprMember.cpp)。
- 下游: [`ExprObjC.h`](../../include/clang/AST/ExprObjC.h),
  [`Type.h`](../../include/clang/AST/Type.h),
  [`Sema.h`](../../include/clang/Sema/Sema.h)。
- 关键类/函数: `class PseudoOpBuilder`,
  `class ObjCPropertyOpBuilder`, `class ObjCSubscriptOpBuilder`,
  `PseudoOpBuilder::buildAssignmentOperation`,
  `ObjCPropertyOpBuilder::findGetter`。

### 3.2 Declaration emission

[`SemaDecl.cpp`](SemaDecl.cpp) — 声明处理心脏, `ActOnDeclarator`、
type-name classification、function/var/typedef/enum decl 创建、tag-name
resolution、GNU/C11 inline 规则 (869 KB, **Sema 最大单文件**)。
- 上游: Parser 经 `Sema::ActOnDeclarator`; [`SemaDeclCXX.cpp`](SemaDeclCXX.cpp),
  [`SemaDeclAttr.cpp`](SemaDeclAttr.cpp),
  [`SemaTemplateInstantiateDecl.cpp`](SemaTemplateInstantiateDecl.cpp)。
- 下游: [`Decl.h`](../../include/clang/AST/Decl.h),
  [`DeclCXX.h`](../../include/clang/AST/DeclCXX.h),
  [`DeclTemplate.h`](../../include/clang/AST/DeclTemplate.h),
  [`DeclSpec.h`](../../include/clang/Sema/DeclSpec.h),
  [`TypeLocBuilder.h`](TypeLocBuilder.h)。
- 关键类/函数: `Sema::ActOnDeclarator`, `Sema::ClassifyName`,
  `Sema::ActOnTagStartDefinition`,
  `Sema::ActOnStartOfFunctionDef`, `Sema::isTagName`,
  `Sema::CheckRedeclaration`, `Sema::AddInitializerToDecl`,
  `class TypeNameValidatorCCC`。

[`SemaDeclAttr.cpp`](SemaDeclAttr.cpp) — 声明属性的语义检查
(alignment / format / visibility / locking / thread-safety / ownership /
lifetime / alloc-size, 等)。
- 上游: [`SemaDecl.cpp`](SemaDecl.cpp) (after creating decls),
  [`SemaDeclCXX.cpp`](SemaDeclCXX.cpp),
  [`SemaTemplateInstantiateDecl.cpp`](SemaTemplateInstantiateDecl.cpp)。
- 下游: [`Attr.h`](../../include/clang/AST/Attr.h),
  [`Decl.h`](../../include/clang/AST/Decl.h),
  [`DiagnosticSema.h`](../../include/clang/Basic/DiagnosticSema.h),
  [`TargetInfo.h`](../../include/clang/Basic/TargetInfo.h)。
- 关键类/函数: `Sema::ProcessDeclAttributes`,
  `Sema::AddAssumeAlignedAttr`, `Sema::AddAllocAlignAttr`,
  `Sema::CheckSpanLikeType`,
  `Sema::isValidPointerAttrType`,
  `class ArgumentDependenceChecker`, `inferNoReturnAttr`。

[`SemaDeclCXX.cpp`](SemaDeclCXX.cpp) — C++ 声明, classes
(`ActOnCXXMemberDeclarator`)、bases、access specifiers、ctor/dtor、
conversion operator、friend、default/delete、using-decl、namespace
alias、显式实例化声明 (780 KB)。
- 上游: [`SemaDecl.cpp`](SemaDecl.cpp) (转发 class 相关 `ActOn*`),
  [`SemaDeclObjC.cpp`](SemaDeclObjC.cpp),
  [`SemaTemplateInstantiateDecl.cpp`](SemaTemplateInstantiateDecl.cpp)。
- 下游: [`DeclCXX.h`](../../include/clang/AST/DeclCXX.h),
  [`CXXInheritance.h`](../../include/clang/AST/CXXInheritance.h),
  [`ComparisonCategories.h`](../../include/clang/AST/ComparisonCategories.h),
  [`TypeLocBuilder.h`](TypeLocBuilder.h)。
- 关键类/函数: `Sema::ActOnClass`,
  `Sema::ActOnCXXMemberDeclarator`,
  `Sema::CheckBaseSpecifier`,
  `Sema::ActOnStartCXXMemberDeclarations`,
  `class CheckDefaultArgumentVisitor`,
  `Sema::ImplicitExceptionSpecification::CalledDecl`。

[`SemaDeclObjC.cpp`](SemaDeclObjC.cpp) — Objective-C 声明, `@interface` /
`@implementation` / `@protocol` / method 声明 / categories / class
extension / method pool。
- 上游: Parser 经 `SemaObjC::ActOnStartOfObjCMethodDef`;
  [`SemaExprObjC.cpp`](SemaExprObjC.cpp)。
- 下游: [`DeclObjC.h`](../../include/clang/AST/DeclObjC.h),
  [`ExprObjC.h`](../../include/clang/AST/ExprObjC.h),
  [`TypeLocBuilder.h`](TypeLocBuilder.h)。
- 关键类/函数: `SemaObjC::checkInitMethod`,
  `SemaObjC::CheckObjCMethodOverride`,
  `SemaObjC::CheckARCMethodDecl`,
  `SemaObjC::AddAnyMethodToGlobalPool`,
  `class ObjCInterfaceValidatorCCC`。

[`SemaTemplate.cpp`](SemaTemplate.cpp) — C++ 模板语法处理,
`ActOnTemplateParameterList`、dependent name resolution for templates、
dependent decl-ref exprs、template-id expressions。
- 上游: Parser (`ActOnClassTemplateSpecialization` /
  `ActOnTemplateDeclaration`),
  [`SemaLookup.cpp`](SemaLookup.cpp) (filter template names)。
- 下游: [`TreeTransform.h`](TreeTransform.h),
  [`DeclTemplate.h`](../../include/clang/AST/DeclTemplate.h),
  [`TemplateName.h`](../../include/clang/AST/TemplateName.h),
  [`Template.h`](../../include/clang/Sema/Template.h)。
- 关键类/函数: `Sema::FilterAcceptableTemplateNames`,
  `Sema::isTemplateName`, `Sema::LookupTemplateName`,
  `Sema::ActOnDependentIdExpression`,
  `Sema::BuildDependentDeclRefExpr`。

[`SemaTemplateInstantiate.cpp`](SemaTemplateInstantiate.cpp) — 模板实
例化驱动, `InstantiatingTemplate` RAII、instantiation depth tracking、
friend instantiation、dependent types/exprs。
- 上游: [`SemaTemplateInstantiateDecl.cpp`](SemaTemplateInstantiateDecl.cpp),
  [`SemaExpr.cpp`](SemaExpr.cpp), [`SemaDecl.cpp`](SemaDecl.cpp)。
- 下游: [`TreeTransform.h`](TreeTransform.h),
  [`ASTLambda.h`](../../include/clang/AST/ASTLambda.h),
  [`DeclTemplate.h`](../../include/clang/AST/DeclTemplate.h),
  [`Sema.h`](../../include/clang/Sema/Sema.h)。
- 关键类/函数: `class InstantiatingTemplate`,
  `Sema::InstantiatingTemplate::InstantiatingTemplate`,
  `struct Response`,
  `Sema::CodeSynthesisContext::isInstantiationRecord`。

[`SemaTemplateInstantiateDecl.cpp`](SemaTemplateInstantiateDecl.cpp) —
模板声明的实例化, class members、static data members、variable
templates、function templates; 内含 `TemplateDeclInstantiator`。
- 上游: [`SemaDecl.cpp`](SemaDecl.cpp) (instantiate
  ClassTemplateSpecializationDecl),
  [`SemaDeclCXX.cpp`](SemaDeclCXX.cpp)。
- 下游: [`TreeTransform.h`](TreeTransform.h),
  [`ASTMutationListener.h`](../../include/clang/AST/ASTMutationListener.h),
  [`DeclTemplate.h`](../../include/clang/AST/DeclTemplate.h)。
- 关键类/函数: `class TemplateDeclInstantiator`,
  `TemplateDeclInstantiator::VisitNamespaceDecl`,
  `TemplateDeclInstantiator::VisitTagDecl`,
  `Sema::InstantiateAttrsForDecl`, `Sema::InstantiateAttrs`。

[`SemaTemplateDeduction.cpp`](SemaTemplateDeduction.cpp) — C++ 模板参
数推导, overload resolution for partial specialization、function
template deduction、deduction from `auto`、CTAD。
- 上游: [`SemaOverload.cpp`](SemaOverload.cpp),
  [`SemaTemplate.cpp`](SemaTemplate.cpp),
  [`SemaExpr.cpp`](SemaExpr.cpp)。
- 下游: [`TreeTransform.h`](TreeTransform.h),
  [`TypeLocBuilder.h`](TypeLocBuilder.h),
  [`DeclTemplate.h`](../../include/clang/AST/DeclTemplate.h),
  [`TemplateName.h`](../../include/clang/AST/TemplateName.h)。
- 关键类/函数: `class NonTypeOrVarTemplateParmDecl`,
  `class PackDeductionScope`,
  `Sema::DeduceTemplateArguments`,
  `Sema::isSameOrCompatibleFunctionType`,
  `Sema::getTrivialTemplateArgumentLoc`,
  `struct clang::DeducedPack`。

[`SemaTemplateDeductionGuide.cpp`](SemaTemplateDeductionGuide.cpp) —
C++17 CTAD deduction guide 生成与检查, implicit deduction guides from
constructors。
- 上游: [`SemaDeclCXX.cpp`](SemaDeclCXX.cpp),
  [`SemaTemplateDeduction.cpp`](SemaTemplateDeduction.cpp)。
- 下游: [`TreeTransform.h`](TreeTransform.h),
  [`TypeLocBuilder.h`](TypeLocBuilder.h),
  [`DeclTemplate.h`](../../include/clang/AST/DeclTemplate.h)。
- 关键类/函数: `class ExtractTypeForDeductionGuide`,
  `struct ConvertConstructorToDeductionGuideTransform`,
  `Sema::DeclareImplicitDeductionGuides`,
  `hasDeclaredDeductionGuides`。

[`SemaTemplateVariadic.cpp`](SemaTemplateVariadic.cpp) — variadic
templates, parameter pack expansion、`sizeof...`、pack expansion
diagnostics。
- 上游: [`SemaTemplate.cpp`](SemaTemplate.cpp),
  [`SemaExpr.cpp`](SemaExpr.cpp)。
- 下游: [`TypeLocBuilder.h`](TypeLocBuilder.h),
  [`DynamicRecursiveASTVisitor.h`](../../include/clang/AST/DynamicRecursiveASTVisitor.h),
  [`Expr.h`](../../include/clang/AST/Expr.h)。
- 关键类/函数: `class CollectUnexpandedParameterPacksVisitor`,
  `Sema::isUnexpandedParameterPackPermitted`,
  `Sema::DiagnoseUnexpandedParameterPacks`,
  `Sema::DiagnoseUnexpandedParameterPack`。

[`SemaConcept.cpp`](SemaConcept.cpp) — C++20 concepts 语义, `concept` /
`requires` expressions、constraint normalization、satisfaction checking。
- 上游: [`SemaDecl.cpp`](SemaDecl.cpp) (concept decl),
  [`SemaTemplateDeduction.cpp`](SemaTemplateDeduction.cpp) (constraint
  eval),
  [`SemaTemplate.cpp`](SemaTemplate.cpp)。
- 下游: [`ASTConcept.h`](../../include/clang/AST/ASTConcept.h),
  [`ExprConcepts.h`](../../include/clang/AST/ExprConcepts.h),
  [`SemaConcept.h`](../../include/clang/Sema/SemaConcept.h),
  [`TreeTransform.h`](TreeTransform.h)。
- 关键类/函数: `class ConstraintSatisfactionChecker`,
  `class AdjustConstraints`, `Sema::CheckConstraintExpression`,
  `Sema::CheckConstraintSatisfaction`,
  `Sema::SetupConstraintScope`, `class LogicalBinOp`。

### 3.3 Type / cast / type-trait checking

[`SemaType.cpp`](SemaType.cpp) — 类型说明符处理, type-of expression
`typeof`、GNU attribute、ObjC bridge type、nullability、address space、
vector/atomic type、function-type CC。
- 上游: [`SemaDecl.cpp`](SemaDecl.cpp) (`getTypeForDeclarator`),
  [`SemaExpr.cpp`](SemaExpr.cpp) (`ActOnTypeOf`),
  [`SemaLambda.cpp`](SemaLambda.cpp)。
- 下游: [`TypeLocBuilder.h`](TypeLocBuilder.h),
  [`Type.h`](../../include/clang/AST/Type.h),
  [`TypeLoc.h`](../../include/clang/AST/TypeLoc.h),
  [`DeclTemplate.h`](../../include/clang/AST/DeclTemplate.h)。
- 关键类/函数: `Sema::CheckQualifiedFunctionForTypeId`,
  `Sema::CheckFunctionReturnType`,
  `Sema::CheckImplicitNullabilityTypeSpecifier`,
  `Sema::CheckVarDeclSizeAddressSpace`,
  `LocInfoType::getAsStringInternal`。

[`SemaCast.cpp`](SemaCast.cpp) — C/C++ cast 语义检查, C-style、functional
cast、`static_cast`、`dynamic_cast`、`reinterpret_cast`、`const_cast`、
`addrspace_cast`。
- 上游: [`SemaExpr.cpp`](SemaExpr.cpp),
  [`SemaExprCXX.cpp`](SemaExprCXX.cpp)。
- 下游: [`ExprCXX.h`](../../include/clang/AST/ExprCXX.h),
  [`Type.h`](../../include/clang/AST/Type.h),
  [`Sema.h`](../../include/clang/Sema/Sema.h)。
- 关键类/函数: `class CastOperation`, `Sema::ActOnCXXNamedCast`,
  `Sema::BuildCXXNamedCast`, `CastOperation::CheckStaticCast`,
  `CastOperation::CheckDynamicCast`,
  `CastOperation::CheckConstCast`,
  `Sema::CheckCompatibleReinterpretCast`。

[`SemaTypeTraits.cpp`](SemaTypeTraits.cpp) — `__has_*` type-trait builtin
语义分析 (`__is_base_of`、`__is_trivially_*`、`__builtin_is_*`)、relocation、
comparison categories。
- 上游: [`SemaExpr.cpp`](SemaExpr.cpp),
  [`SemaChecking.cpp`](SemaChecking.cpp)。
- 下游: [`ComparisonCategories.h`](../../include/clang/AST/ComparisonCategories.h),
  [`DeclCXX.h`](../../include/clang/AST/DeclCXX.h),
  [`Mangle.h`](../../include/clang/AST/Mangle.h)。
- 关键类/函数: `Sema::CheckCXX2CRelocatable`,
  `Sema::IsCXXTriviallyRelocatableType`,
  `Sema::CheckTypeTraitArity`, `Sema::BuiltinIsBaseOf`,
  `Sema::DiagnoseTypeTraitDetails`, `DiagnoseBuiltinDeprecation`。

### 3.4 Expression checking

[`SemaExpr.cpp`](SemaExpr.cpp) — 通用 expression 处理, primary
expression、binary/unary op、call、conditional、comma、sizeof/alignof、
statement expression、lambda introducer (900 KB, Sema 第二大)。
- 上游: Parser 调所有 `ActOn*Expr`; [`SemaChecking.cpp`](SemaChecking.cpp),
  [`SemaOverload.cpp`](SemaOverload.cpp)。
- 下游: [`CheckExprLifetime.h`](CheckExprLifetime.h),
  [`TreeTransform.h`](TreeTransform.h),
  [`UsedDeclVisitor.h`](UsedDeclVisitor.h),
  [`Expr.h`](../../include/clang/AST/Expr.h),
  [`ExprCXX.h`](../../include/clang/AST/ExprCXX.h)。
- 关键类/函数: `Sema::ActOnIdentifierExpr`, `Sema::ActOnCallExpr`,
  `Sema::BuildDeclRefExpr`, `Sema::DiagnoseUseOfDecl`,
  `Sema::ActOnStringLiteral`, `Sema::checkVariadicArgument`。

[`SemaExprCXX.cpp`](SemaExprCXX.cpp) — C++ 表达式, `typeid`、`noexcept`、
`throw`、`new`/`delete`、`this`、user-defined literal、explicit object
member function、C++23 `T()` value-init、paren-init。
- 上游: [`SemaExpr.cpp`](SemaExpr.cpp),
  [`SemaOverload.cpp`](SemaOverload.cpp)。
- 下游: [`TreeTransform.h`](TreeTransform.h),
  [`TypeLocBuilder.h`](TypeLocBuilder.h),
  [`ExprCXX.h`](../../include/clang/AST/ExprCXX.h)。
- 关键类/函数: `Sema::ActOnCXXTypeid`, `Sema::ActOnCXXThrow`,
  `Sema::CheckCXXThrowOperand`, `Sema::CXXThisScopeRAII`,
  `Sema::CheckCXXThisCapture`, `Sema::checkLiteralOperatorId`。

[`SemaExprMember.cpp`](SemaExprMember.cpp) — member access 表达式, `.` /
`->` / `.*` / `->*`, 含 dependent member expression 和 anonymous
struct/union member access。
- 上游: [`SemaExpr.cpp`](SemaExpr.cpp) (`ActOnMemberAccess`),
  [`SemaExprObjC.cpp`](SemaExprObjC.cpp)。
- 下游: [`DeclCXX.h`](../../include/clang/AST/DeclCXX.h),
  [`DeclObjC.h`](../../include/clang/AST/DeclObjC.h),
  [`DeclTemplate.h`](../../include/clang/AST/DeclTemplate.h),
  [`Expr.h`](../../include/clang/AST/Expr.h)。
- 关键类/函数: `Sema::BuildMemberReferenceExpr`,
  `Sema::BuildFieldReferenceExpr`,
  `Sema::BuildImplicitMemberExpr`,
  `Sema::PerformMemberExprBaseConversion`,
  `Sema::CheckQualifiedMemberReference`,
  `Sema::BuildAnonymousStructUnionMemberReference`。

[`SemaExprObjC.cpp`](SemaExprObjC.cpp) — ObjC 表达式, message send
`[receiver msg]`、`@"string"`、`@selector`、`@encode`、`@available`、
`@synchronized`、`@try`/`@catch`/`@finally`、collection literal、ARC cast。
- 上游: [`SemaExpr.cpp`](SemaExpr.cpp) (`ActOnObjCMessageExpr` 转发),
  [`SemaPseudoObject.cpp`](SemaPseudoObject.cpp)。
- 下游: [`DeclObjC.h`](../../include/clang/AST/DeclObjC.h),
  [`ExprObjC.h`](../../include/clang/AST/ExprObjC.h),
  [`Availability.h`](../../include/clang/AST/Availability.h)。
- 关键类/函数: `SemaObjC::CheckMessageArgumentTypes`,
  `SemaObjC::isSelfExpr`, `SemaObjC::getObjCMessageKind`,
  `SemaObjC::CheckTollFreeBridgeCast`,
  `class ObjCInterfaceOrSuperCCC`。

[`SemaOverload.cpp`](SemaOverload.cpp) — C++ overload resolution,
`OverloadCandidateSet`、conversion sequence 分类、ranking、
tie-breaking、user-defined conversion、built-in operator overload set
(715 KB)。
- 上游: [`SemaExpr.cpp`](SemaExpr.cpp),
  [`SemaDecl.cpp`](SemaDecl.cpp),
  [`SemaTemplateDeduction.cpp`](SemaTemplateDeduction.cpp)。
- 下游: [`CheckExprLifetime.h`](CheckExprLifetime.h),
  [`CXXInheritance.h`](../../include/clang/AST/CXXInheritance.h),
  [`DeclCXX.h`](../../include/clang/AST/DeclCXX.h),
  [`ExprCXX.h`](../../include/clang/AST/ExprCXX.h)。
- 关键类/函数: `class OverloadCandidateSet`,
  `class StandardConversionSequence`,
  `class UserDefinedConversionSequence`,
  `class AmbiguousConversionSequence`,
  `class DeductionFailureInfo`, `Sema::AddOverloadCandidate`,
  `Sema::BestViableFunction`。

[`SemaLambda.cpp`](SemaLambda.cpp) — C++11 lambda, lambda closure class
创建、capture 处理 (init-capture, `this`, by reference)、lambda call
operator setup、return type deduction。
- 上游: [`SemaExpr.cpp`](SemaExpr.cpp) (`ActOnLambdaExpr`),
  [`SemaExprCXX.cpp`](SemaExprCXX.cpp)。
- 下游: [`TypeLocBuilder.h`](TypeLocBuilder.h),
  [`ASTLambda.h`](../../include/clang/AST/ASTLambda.h),
  [`ExprCXX.h`](../../include/clang/AST/ExprCXX.h),
  [`Initialization.h`](../../include/clang/Sema/Initialization.h)。
- 关键类/函数: `Sema::createLambdaClosureType`,
  `Sema::buildLambdaScope`, `Sema::handleLambdaNumbering`,
  `Sema::finishLambdaExplicitCaptures`,
  `Sema::deduceClosureReturnType`, `Sema::addInitCapture`。

[`SemaInit.cpp`](SemaInit.cpp) — initializer 分析, `InitListChecker`、
brace-enclosed init、copy/move/direct-init、narrowing diagnostics、
C++20 designated initializer、lifetime extension (`temporary
materialization`)。
- 上游: [`SemaDecl.cpp`](SemaDecl.cpp) (`AddInitializerToDecl`),
  [`SemaExpr.cpp`](SemaExpr.cpp) (`ActOnInitList`)。
- 下游: [`CheckExprLifetime.h`](CheckExprLifetime.h),
  [`DeclObjC.h`](../../include/clang/AST/DeclObjC.h),
  [`Expr.h`](../../include/clang/AST/Expr.h),
  [`ExprCXX.h`](../../include/clang/AST/ExprCXX.h)。
- 关键类/函数: `class InitListChecker`,
  `InitListChecker::CheckEmptyInitializable`,
  `InitListChecker::FillInEmptyInitForBase`,
  `Sema::IsStringInit`,
  `emitUninitializedExplicitInitFields`。

[`SemaStmt.cpp`](SemaStmt.cpp) — 语句级语义 actions, `if` / `switch` /
`while` / `for` / `do` / `goto`、label 处理、compound statement、
case/default、range-for、asm-stmt dispatch。
- 上游: Parser 调所有 `ActOn*Stmt`;
  [`JumpDiagnostics.cpp`](JumpDiagnostics.cpp) (scope info)。
- 下游: [`CheckExprLifetime.h`](CheckExprLifetime.h),
  [`Stmt.h`](../../include/clang/AST/Stmt.h),
  [`Sema.h`](../../include/clang/Sema/Sema.h)。
- 关键类/函数: `Sema::ActOnIfStmt`, `Sema::ActOnForStmt`,
  `Sema::ActOnCaseStmt`, `Sema::ActOnStartOfCompoundStmt`,
  `Sema::ActOnFinishOfCompoundStmt`,
  `Sema::DiagnoseUnusedExprResult`。

[`SemaStmtAsm.cpp`](SemaStmtAsm.cpp) — inline assembly, operand 校验、
`__asm__` 约束、MS-style `__asm` block、`asm`-identifier (register,
label) lookup。
- 上游: [`SemaStmt.cpp`](SemaStmt.cpp), parser (`ActOnAsmStmt`)。
- 下游: [`ExprCXX.h`](../../include/clang/AST/ExprCXX.h),
  [`RecordLayout.h`](../../include/clang/AST/RecordLayout.h),
  [`TypeLoc.h`](../../include/clang/AST/TypeLoc.h)。
- 关键类/函数: `Sema::FillInlineAsmIdentifierInfo`,
  `Sema::LookupInlineAsmField`,
  `Sema::LookupInlineAsmVarDeclField`。

[`SemaStmtAttr.cpp`](SemaStmtAttr.cpp) — 语句属性处理, `[[likely]]` /
`[[unlikely]]` / `[[fallthrough]]` / `[[nodiscard]]` on expr /
`[[assume(expr)]]`。
- 上游: [`SemaStmt.cpp`](SemaStmt.cpp),
  [`SemaExpr.cpp`](SemaExpr.cpp)。
- 下游: [`ASTContext.h`](../../include/clang/AST/ASTContext.h),
  [`EvaluatedExprVisitor.h`](../../include/clang/AST/EvaluatedExprVisitor.h),
  [`TargetInfo.h`](../../include/clang/Basic/TargetInfo.h)。
- 关键类/函数: `Sema::ProcessStmtAttributes`,
  `Sema::CheckNoInlineAttr`, `Sema::CheckAlwaysInlineAttr`,
  `Sema::CheckRebuiltStmtAttributes`, `class CallExprFinder`。

[`SemaChecking.cpp`](SemaChecking.cpp) — C 类型系统之外的额外检查,
format string (`printf`/`scanf`/NSString)、fortified libc (`strcpy`
family)、atomic/builtin 实参数、statement-expression warning、算术
conversion、sanitizer attribute validation (667 KB)。
- 上游: [`SemaExpr.cpp`](SemaExpr.cpp) (after `ActOnCallExpr`),
  [`SemaDecl.cpp`](SemaDecl.cpp) (init expr 检查)。
- 下游: [`CheckExprLifetime.h`](CheckExprLifetime.h),
  [`FormatString.h`](../../include/clang/AST/FormatString.h),
  [`EvaluatedExprVisitor.h`](../../include/clang/AST/EvaluatedExprVisitor.h),
  [`Sema.h`](../../include/clang/Sema/Sema.h)。
- 关键类/函数: `Sema::checkArgCount`,
  `class ScanfDiagnosticFormatHandler`,
  `class EstimateSizeFormatHandler`, `class FortifiedBufferChecker`,
  `Sema::checkFortifiedBuiltinMemoryFunction`,
  `struct BuiltinDumpStructGenerator`,
  `class FormatStringLiteral`。

### 3.5 Scope / identifier resolution

[`Scope.cpp`](Scope.cpp) — `Scope` class 实现, 跟踪 parent scope、depth、
flags (Fn/Block/Class/TemplateParam)、NRVO candidate bookkeeping。
- 上游: [`Sema.cpp`](Sema.cpp), [`SemaDecl.cpp`](SemaDecl.cpp),
  [`SemaStmt.cpp`](SemaStmt.cpp) (push/pop scope)。
- 下游: [`Scope.h`](../../include/clang/Sema/Scope.h),
  [`Decl.h`](../../include/clang/AST/Decl.h)。
- 关键类/函数: `class Scope`, `Scope::Init`, `Scope::setFlags`,
  `Scope::EnterLoopBody`, `Scope::LeaveLoopBody`,
  `Scope::updateNRVOCandidate`, `Scope::applyNRVO`。

[`ScopeInfo.cpp`](ScopeInfo.cpp) — `FunctionScopeInfo` 及相关, per-function
/ per-block / per-lambda state (ReturnStmt, Captures, WeakObjectProfile,
VLA, OpenMP data)。
- 上游: [`SemaDecl.cpp`](SemaDecl.cpp),
  [`SemaLambda.cpp`](SemaLambda.cpp),
  [`SemaStmt.cpp`](SemaStmt.cpp),
  [`SemaOpenMP.cpp`](SemaOpenMP.cpp)。
- 下游: [`ScopeInfo.h`](../../include/clang/Sema/ScopeInfo.h),
  [`Decl.h`](../../include/clang/AST/Decl.h)。
- 关键类/函数: `class FunctionScopeInfo`,
  `FunctionScopeInfo::Clear`,
  `FunctionScopeInfo::WeakObjectProfileTy`, `class Capture`。

[`IdentifierResolver.cpp`](IdentifierResolver.cpp) — `IdentifierResolver`,
lexical-scope identifier lookup 用 shadow chains per identifier。
- 上游: [`Sema.cpp`](Sema.cpp), [`SemaDecl.cpp`](SemaDecl.cpp),
  [`SemaLookup.cpp`](SemaLookup.cpp)。
- 下游: [`IdentifierResolver.h`](../../include/clang/Sema/IdentifierResolver.h),
  [`DeclarationName.h`](../../include/clang/AST/DeclarationName.h),
  [`IdentifierTable.h`](../../include/clang/Basic/IdentifierTable.h)。
- 关键类/函数: `class IdentifierResolver`,
  `IdentifierResolver::IdDeclInfoMap`,
  `IdentifierResolver::AddDecl`,
  `IdentifierResolver::InsertDeclAfter`,
  `IdentifierResolver::RemoveDecl`,
  `IdentifierResolver::isDeclInScope`。

[`SemaCXXScopeSpec.cpp`](SemaCXXScopeSpec.cpp) — C++ scope specifier `::`
/ `T::` / `T::~T` / `T::template N` parsing, validation、
dependent-vs-non-dependent、complete-context 检查。
- 上游: [`SemaLookup.cpp`](SemaLookup.cpp),
  [`SemaExprMember.cpp`](SemaExprMember.cpp),
  [`SemaExpr.cpp`](SemaExpr.cpp)。
- 下游: [`TypeLocBuilder.h`](TypeLocBuilder.h),
  [`DeclTemplate.h`](../../include/clang/AST/DeclTemplate.h),
  [`ExprCXX.h`](../../include/clang/AST/ExprCXX.h)。
- 关键类/函数: `Sema::ActOnCXXGlobalScopeSpecifier`,
  `Sema::ActOnSuperScopeSpecifier`,
  `Sema::BuildCXXNestedNameSpecifier`,
  `Sema::isDependentScopeSpecifier`,
  `class NestedNameSpecifierValidatorCCC`。

[`SemaAccess.cpp`](SemaAccess.cpp) — C++ access control, friend 匹配、
effective-context tracking、protected member access 检查、delayed
access diagnostics。
- 上游: [`SemaExpr.cpp`](SemaExpr.cpp) (member access),
  [`SemaDeclCXX.cpp`](SemaDeclCXX.cpp) (friend decls),
  [`Sema.cpp`](Sema.cpp)。
- 下游: [`CXXInheritance.h`](../../include/clang/AST/CXXInheritance.h),
  [`DeclCXX.h`](../../include/clang/AST/DeclCXX.h),
  [`DeclFriend.h`](../../include/clang/AST/DeclFriend.h)。
- 关键类/函数: `struct EffectiveContext`, `struct AccessTarget`,
  `class FriendTemplateMatchContext`,
  `struct ProtectedFriendContext`,
  `Sema::HandleDelayedAccessCheck`,
  `Sema::CheckUnresolvedLookupAccess`。

### 3.6 Diagnostics / fix-it / warnings

[`SemaFixItUtils.cpp`](SemaFixItUtils.cpp) — `ConversionFixItGenerator`,
建议缺失的转换、zero-init fixit。
- 上游: [`SemaInit.cpp`](SemaInit.cpp),
  [`SemaOverload.cpp`](SemaOverload.cpp)。
- 下游: [`ExprCXX.h`](../../include/clang/AST/ExprCXX.h),
  [`ExprObjC.h`](../../include/clang/AST/ExprObjC.h),
  [`Sema.h`](../../include/clang/Sema/Sema.h)。
- 关键类/函数: `class ConversionFixItGenerator`,
  `ConversionFixItGenerator::tryToFixConversion`,
  `Sema::getFixItZeroInitializerForType`,
  `Sema::getFixItZeroLiteralForType`。

[`JumpDiagnostics.cpp`](JumpDiagnostics.cpp) — `JumpScopeChecker`,
诊断 illegal jump (goto into protected scope、jumping over VLA/init
stmt、indirect goto/goto out of `finally`/`catch`)。
- 上游: [`SemaStmt.cpp`](SemaStmt.cpp) (`ActOnGotoStmt`,
  function-body analysis)。
- 下游: [`DeclCXX.h`](../../include/clang/AST/DeclCXX.h),
  [`ExprCXX.h`](../../include/clang/AST/ExprCXX.h),
  [`StmtOpenMP.h`](../../include/clang/AST/StmtOpenMP.h)。
- 关键类/函数: `class JumpScopeChecker`,
  `JumpScopeChecker::BuildScopeInformation`,
  `JumpScopeChecker::VerifyJumps`,
  `JumpScopeChecker::VerifyIndirectJumps`,
  `JumpScopeChecker::NoteJumpIntoScopes`。

[`DelayedDiagnostic.cpp`](DelayedDiagnostic.cpp) — `DelayedDiagnostic`
storage, 诊断 produced during declarator parsing whose target is
later-known (e.g. access check depending on class definition)。
- 上游: [`SemaDecl.cpp`](SemaDecl.cpp),
  [`SemaDeclCXX.cpp`](SemaDeclCXX.cpp),
  [`SemaAttr.cpp`](SemaAttr.cpp)。
- 下游: [`DelayedDiagnostic.h`](../../include/clang/Sema/DelayedDiagnostic.h)。
- 关键类/函数: `class DelayedDiagnostic`,
  `DelayedDiagnostic::makeAvailability`,
  `DelayedDiagnostic::Destroy`, `class AccessedEntity`。

[`AnalysisBasedWarnings.cpp`](AnalysisBasedWarnings.cpp) —
`analysis_warnings::Policy` / `Executor`, flow-sensitive warning 基于
libAnalysis CFG (`-Wunreachable-code`、`-Wnull-dereference`、
`-Wreturn-type`、`-Wswitch`、`-Wself-assign`、uninitialized-use)。
- 上游: [`SemaDecl.cpp`](SemaDecl.cpp) (function-body completion),
  [`SemaChecking.cpp`](SemaChecking.cpp) (stmt expr)。
- 下游: [`DeclCXX.h`](../../include/clang/AST/DeclCXX.h),
  [`clang/Analysis/Analyses/LifetimeSafety/LifetimeSafety.h`](../../include/clang/Analysis/Analyses/LifetimeSafety/LifetimeSafety.h),
  [`SemaLifetimeSafety.h`](SemaLifetimeSafety.h),
  [`TypeLocBuilder.h`](TypeLocBuilder.h)。
- 关键类/函数: `class LogicalErrorHandler`,
  `struct TransferFunctions`, `class ContainsReference`,
  `analysis_warnings::Policy`, `analysis_warnings::Executor`。

[`CheckExprLifetime.cpp`](CheckExprLifetime.cpp) +
[`CheckExprLifetime.h`](CheckExprLifetime.h) — statement-local
lifetime 分析, detect use-after-free, track `IndirectLocalPath` 与
`Initializers` lifetime paths through expr/init。
- 上游: [`SemaInit.cpp`](SemaInit.cpp),
  [`SemaOverload.cpp`](SemaOverload.cpp),
  [`SemaExpr.cpp`](SemaExpr.cpp),
  [`SemaChecking.cpp`](SemaChecking.cpp),
  [`SemaStmt.cpp`](SemaStmt.cpp)。
- 下游: [`clang/Analysis/Analyses/LifetimeSafety/LifetimeAnnotations.h`](../../include/clang/Analysis/Analyses/LifetimeSafety/LifetimeAnnotations.h),
  [`Initialization.h`](../../include/clang/Sema/Initialization.h),
  [`Sema.h`](../../include/clang/Sema/Sema.h)。
- 关键类/函数: `struct IndirectLocalPathEntry`, `checkInitLifetime`,
  `checkExprLifetimeMustTailArg`, `checkAssignmentLifetime`,
  `checkCaptureByLifetime`。

[`SemaAttr.cpp`](SemaAttr.cpp) — 通用属性处理, pragma 处理
(`#pragma pack`、`#pragma align`、`#pragma clang`)、非平凡属性检查
(alignment、MS struct、GSL pointer/owner inference、lifetime-bounds
inference)。
- 上游: [`SemaDecl.cpp`](SemaDecl.cpp),
  [`SemaDeclAttr.cpp`](SemaDeclAttr.cpp), parser pragma。
- 下游: [`Attr.h`](../../include/clang/AST/Attr.h),
  [`DeclCXX.h`](../../include/clang/AST/DeclCXX.h),
  [`clang/Analysis/Analyses/LifetimeSafety/LifetimeAnnotations.h`](../../include/clang/Analysis/Analyses/LifetimeSafety/LifetimeAnnotations.h),
  [`TargetInfo.h`](../../include/clang/Basic/TargetInfo.h)。
- 关键类/函数: `Sema::PragmaStackSentinelRAII`,
  `Sema::AddAlignmentAttributesForRecord`,
  `Sema::AddMsStructLayoutForRecord`,
  `Sema::inferGslPointerAttribute`,
  `Sema::inferGslOwnerPointerAttribute`,
  `Sema::inferLifetimeBoundAttribute`,
  `Sema::inferNullableClassAttribute`。

### 3.7 Code completion

[`CodeCompleteConsumer.cpp`](CodeCompleteConsumer.cpp) —
`CodeCompleteConsumer` API, `CodeCompletionString` builder (`Chunk`
创建)、`CodeCompletionResult` / `CodeCompletionContext` logic。
- 上游: [`SemaCodeComplete.cpp`](SemaCodeComplete.cpp)。
- 下游: [`CodeCompleteConsumer.h`](../../include/clang/Sema/CodeCompleteConsumer.h),
  [`Decl.h`](../../include/clang/AST/Decl.h),
  [`DeclTemplate.h`](../../include/clang/AST/DeclTemplate.h)。
- 关键类/函数: `class CodeCompleteConsumer`,
  `class CodeCompletionString`,
  `CodeCompletionString::Chunk::CreateText`,
  `CodeCompletionString::Chunk::CreateOptional`,
  `CodeCompletionContext::wantConstructorResults`。

[`SemaCodeComplete.cpp`](SemaCodeComplete.cpp) — 语义代码补全,
驱动 `Sema::CodeComplete*` action、用 `PreferredTypeBuilder`。
- 上游: Parser 经 `CodeCompleteAt` / `CodeCompleteOrdinaryName`;
  [`CodeCompleteConsumer.cpp`](CodeCompleteConsumer.cpp)。
- 下游: [`Decl.h`](../../include/clang/AST/Decl.h),
  [`DeclCXX.h`](../../include/clang/AST/DeclCXX.h),
  [`CodeCompleteConsumer.h`](../../include/clang/Sema/CodeCompleteConsumer.h)。
- 关键类/函数: `class ResultBuilder`, `class PreferredTypeBuilder`,
  `Sema::CodeCompleteOrdinaryName`,
  `Sema::CodeCompleteMemberAccessExpr`,
  `Sema::CodeCompleteObjCMessageReceiver`,
  `Sema::CodeCompleteCallArg`。

### 3.8 ObjC & Pseudo-object

[`SemaObjC.cpp`](SemaObjC.cpp) — `SemaObjC`, ObjC 语义 actions,
retain-cycle 检测 (`@property (weak)`/block)、`@autoreleasepool`、
ObjC `@try/@catch/@finally` 异常处理、`CheckObjCString`、ARC、
related-result-type note。
- 上游: [`SemaDecl.cpp`](SemaDecl.cpp) (method/ivar),
  [`SemaDeclObjC.cpp`](SemaDeclObjC.cpp),
  [`SemaExprObjC.cpp`](SemaExprObjC.cpp)。
- 下游: [`DeclObjC.h`](../../include/clang/AST/DeclObjC.h),
  [`EvaluatedExprVisitor.h`](../../include/clang/AST/EvaluatedExprVisitor.h),
  [`SemaObjC.h`](../../include/clang/Sema/SemaObjC.h)。
- 关键类/函数: `class SemaObjC`,
  `SemaObjC::CheckObjCCircularContainer`,
  `SemaObjC::checkRetainCycles`,
  `SemaObjC::CheckObjCMethodCall`,
  `struct RetainCycleOwner`, `struct FindCaptureVisitor`。

[`SemaObjCProperty.cpp`](SemaObjCProperty.cpp) — ObjC `@property` /
`@synthesize` 语义, getter/setter synthesis、missing-designated-init
diagnostics、owning-property getter synthesis、atomic-property setter
rules、ivar/method consistency。
- 上游: [`SemaDeclObjC.cpp`](SemaDeclObjC.cpp),
  [`SemaExprObjC.cpp`](SemaExprObjC.cpp),
  [`SemaPseudoObject.cpp`](SemaPseudoObject.cpp)。
- 下游: [`DeclObjC.h`](../../include/clang/AST/DeclObjC.h),
  [`ASTMutationListener.h`](../../include/clang/AST/ASTMutationListener.h),
  [`Sema.h`](../../include/clang/Sema/Sema.h)。
- 关键类/函数: `SemaObjC::DiagnosePropertyMismatch`,
  `SemaObjC::DiagnosePropertyAccessorMismatch`,
  `SemaObjC::DefaultSynthesizeProperties`,
  `SemaObjC::DiagnoseUnimplementedProperties`,
  `SemaObjC::AtomicPropertySetterGetterRules`,
  `SemaObjC::DiagnoseMissingDesignatedInitOverrides`。

### 3.9 C++ exception / coroutine / lifetime

[`SemaCoroutine.cpp`](SemaCoroutine.cpp) — C++20 协程, `co_await` /
`co_yield` / `co_return` 分析、promise type extraction、frame
allocation、final-suspend 处理、dependent-coroutine body 构建。
- 上游: [`SemaExpr.cpp`](SemaExpr.cpp),
  [`SemaStmt.cpp`](SemaStmt.cpp)。
- 下游: [`CoroutineStmtBuilder.h`](CoroutineStmtBuilder.h),
  [`ASTLambda.h`](../../include/clang/AST/ASTLambda.h),
  [`Decl.h`](../../include/clang/AST/Decl.h),
  [`ExprCXX.h`](../../include/clang/AST/ExprCXX.h)。
- 关键类/函数: `struct ReadySuspendResumeResult`,
  `Sema::checkFinalSuspendNoThrow`,
  `Sema::ActOnCoroutineBodyStart`,
  `Sema::CheckCompletedCoroutineBody`,
  `CoroutineStmtBuilder::CoroutineStmtBuilder`,
  `CoroutineStmtBuilder::buildStatements`,
  `CoroutineStmtBuilder::makePromiseStmt`。

[`SemaExceptionSpec.cpp`](SemaExceptionSpec.cpp) — C++ exception
specifications, `noexcept` / `throw()` 分析、`handlerCanCatch` set
inclusion、implicit exception spec computation、equivalent-exception-spec
比较。
- 上游: [`SemaDecl.cpp`](SemaDecl.cpp),
  [`SemaDeclCXX.cpp`](SemaDeclCXX.cpp),
  [`SemaExpr.cpp`](SemaExpr.cpp),
  [`SemaChecking.cpp`](SemaChecking.cpp)。
- 下游: [`CXXInheritance.h`](../../include/clang/AST/CXXInheritance.h),
  [`ExprCXX.h`](../../include/clang/AST/ExprCXX.h),
  [`Diagnostic.h`](../../include/clang/Basic/Diagnostic.h)。
- 关键类/函数: `Sema::CheckSpecifiedExceptionType`,
  `Sema::CheckDistantExceptionSpec`,
  `Sema::ResolveExceptionSpec`, `Sema::UpdateExceptionSpec`,
  `Sema::CheckEquivalentExceptionSpec`,
  `Sema::handlerCanCatch`。

[`SemaLifetimeSafety.h`](SemaLifetimeSafety.h) — 桥接
`clang::lifetimes` framework 到 Sema 的诊断 engine; 定义
`LifetimeSafetySemaHelperImpl` 用于发诊断。
- 上游: [`AnalysisBasedWarnings.cpp`](AnalysisBasedWarnings.cpp),
  [`SemaAttr.cpp`](SemaAttr.cpp)。
- 下游: [`clang/Analysis/Analyses/LifetimeSafety/LifetimeSafety.h`](../../include/clang/Analysis/Analyses/LifetimeSafety/LifetimeSafety.h),
  [`clang/Analysis/Analyses/LifetimeSafety/LifetimeAnnotations.h`](../../include/clang/Analysis/Analyses/LifetimeSafety/LifetimeAnnotations.h),
  [`DiagnosticSema.h`](../../include/clang/Basic/DiagnosticSema.h)。
- 关键类/函数: `class LifetimeSafetySemaHelperImpl`,
  `LifetimeSafetySemaHelper`。

[`CoroutineStmtBuilder.h`](CoroutineStmtBuilder.h) — `CoroutineStmtBuilder`
class 声明, 编排 implicit statement 构建 (promise init、initial/final
suspend、return-on-alloc-failure、destroy/handle) 为协程 body。
- 上游: [`SemaCoroutine.cpp`](SemaCoroutine.cpp)。
- 下游: [`ExprCXX.h`](../../include/clang/AST/ExprCXX.h),
  [`StmtCXX.h`](../../include/clang/AST/StmtCXX.h),
  [`SemaInternal.h`](../../include/clang/Sema/SemaInternal.h)。
- 关键类/函数: `class CoroutineStmtBuilder`。

### 3.10 OpenMP / OpenACC / CUDA / HLSL / SYCL

[`SemaOpenMP.cpp`](SemaOpenMP.cpp) — OpenMP directives/clauses,
`#pragma omp parallel` / `for` / `simd` / `task` / `target` 等, 含
data-sharing attribute 分析 (`DSAStackTy`)、declare-variants、
metadirectives (1.1 MB, **Sema 单文件之最**, 也是整个 Clang 最大)。
- 上游: [`SemaStmt.cpp`](SemaStmt.cpp) (OMP-aware stmt),
  [`SemaDecl.cpp`](SemaDecl.cpp) (OMP declare-* attr)。
- 下游: [`OpenMPClause.h`](../../include/clang/AST/OpenMPClause.h),
  [`StmtOpenMP.h`](../../include/clang/AST/StmtOpenMP.h),
  [`SemaOpenMP.h`](../../include/clang/Sema/SemaOpenMP.h)。
- 关键类/函数: `class DSAStackTy`, `DSAStackTy::addDSA`,
  `DSAStackTy::getDSA`,
  `Sema::ActOnOpenMPExecutableDirective`,
  `isImplicitTaskingRegion`,
  `isImplicitOrExplicitTaskingRegion`。

[`SemaOpenACC.cpp`](SemaOpenACC.cpp) — OpenACC construct 入口,
`#pragma acc parallel/kernels/loop/data`、associated-stmt RAII、
appertainment helper。
- 上游: Parser 经 `ActOnConstruct`;
  [`SemaOpenACCClause.cpp`](SemaOpenACCClause.cpp)。
- 下游: [`OpenACCClause.h`](../../include/clang/AST/OpenACCClause.h),
  [`SemaOpenACC.h`](../../include/clang/Sema/SemaOpenACC.h)。
- 关键类/函数: `class SemaOpenACC`,
  `SemaOpenACC::ActOnConstruct`,
  `SemaOpenACC::AssociatedStmtRAII`,
  `diagnoseConstructAppertainment`,
  `CollectActiveReductionClauses`。

[`SemaOpenACCAtomic.cpp`](SemaOpenACCAtomic.cpp) — OpenACC `atomic`
construct, atomic operand 检查 (read/write/update/capture)、verify
operand 为 scalar。
- 上游: [`SemaOpenACC.cpp`](SemaOpenACC.cpp) (construct 分发)。
- 下游: [`ExprCXX.h`](../../include/clang/AST/ExprCXX.h),
  [`SemaOpenACC.h`](../../include/clang/Sema/SemaOpenACC.h)。
- 关键类/函数: `class AtomicOperandChecker`。

[`SemaOpenACCClause.cpp`](SemaOpenACCClause.cpp) — OpenACC per-clause
校验 (`private`、`firstprivate`、`reduction`、`gang`、`vector`、`worker`、
`tile`、`collapse`、`link`, ...)。
- 上游: [`SemaOpenACC.cpp`](SemaOpenACC.cpp)。
- 下游: [`OpenACCClause.h`](../../include/clang/AST/OpenACCClause.h),
  [`SemaOpenACC.h`](../../include/clang/Sema/SemaOpenACC.h)。
- 关键类/函数: `class SemaOpenACCClauseVisitor`,
  `SemaOpenACC::ActOnClause`,
  `SemaOpenACC::CheckReductionVarType`,
  `SemaOpenACC::CheckGangClause`,
  `SemaOpenACC::CheckLinkClauseVarList`。

[`SemaOpenACCClauseAppertainment.cpp`](SemaOpenACCClauseAppertainment.cpp)
— TableGen-generated 自 `OpenACCClauses.td`, directive/clause
appertainment (allowed, required, exclusive) 诊断表。
- 上游: [`SemaOpenACC.cpp`](SemaOpenACC.cpp),
  [`SemaOpenACCClause.cpp`](SemaOpenACCClause.cpp)。
- 下游: [`OpenACCClause.h`](../../include/clang/AST/OpenACCClause.h),
  [`SemaOpenACC.h`](../../include/clang/Sema/SemaOpenACC.h)。
- 关键类/函数: `class AccClauseSet`, `struct LLVMClauseLists`,
  `struct LLVMDirectiveClauseRelationships`,
  `SemaOpenACC::DiagnoseRequiredClauses`,
  `SemaOpenACC::DiagnoseAllowedClauses`。

[`SemaCUDA.cpp`](SemaCUDA.cpp) — CUDA host/device call 检查, target
identification、implicit host/device function 处理、function-callability、
callable-by-host/device、NVCC-compat、CUDA stream 语义。
- 上游: [`SemaDecl.cpp`](SemaDecl.cpp),
  [`SemaDeclAttr.cpp`](SemaDeclAttr.cpp),
  [`SemaExpr.cpp`](SemaExpr.cpp)。
- 下游: [`ASTContext.h`](../../include/clang/AST/ASTContext.h),
  [`Decl.h`](../../include/clang/AST/Decl.h),
  [`SemaCUDA.h`](../../include/clang/Sema/SemaCUDA.h)。
- 关键类/函数: `class SemaCUDA`,
  `SemaCUDA::PushForceHostDevice`,
  `SemaCUDA::PopForceHostDevice`,
  `SemaCUDA::IdentifyTarget`,
  `SemaCUDA::IdentifyPreference`,
  `SemaCUDA::isImplicitHostDeviceFunction`。

[`SemaHLSL.cpp`](SemaHLSL.cpp) — HLSL 语义分析, entry-point 校验、
parameter modifier、resource binding 分析、shader attribute 合并、
Vulkan/Metal-specific HLSL 语义。
- 上游: [`HLSLExternalSemaSource.cpp`](HLSLExternalSemaSource.cpp),
  [`SemaDecl.cpp`](SemaDecl.cpp),
  [`SemaDeclAttr.cpp`](SemaDeclAttr.cpp)。
- 下游: [`ASTContext.h`](../../include/clang/AST/ASTContext.h),
  [`Attr.h`](../../include/clang/AST/Attr.h),
  [`SemaHLSL.h`](../../include/clang/Sema/SemaHLSL.h),
  [`HLSLBuiltinTypeDeclBuilder.h`](HLSLBuiltinTypeDeclBuilder.h)。
- 关键类/函数: `class SemaHLSL`, `class ResourceBindings`,
  `SemaHLSL::ActOnTopLevelFunction`,
  `SemaHLSL::mergeVkConstantIdAttr`,
  `SemaHLSL::mergeShaderAttr`,
  `SemaHLSL::CheckEntryPoint`,
  `SemaHLSL::determineActiveSemantic`。

[`HLSLBuiltinTypeDeclBuilder.cpp`](HLSLBuiltinTypeDeclBuilder.cpp) +
[`HLSLBuiltinTypeDeclBuilder.h`](HLSLBuiltinTypeDeclBuilder.h) — 构建
HLSL builtin class type (vector、matrix、texture、sampler、buffer) 的
helper, fluent builder pattern。
- 上游: [`HLSLExternalSemaSource.cpp`](HLSLExternalSemaSource.cpp),
  [`SemaHLSL.cpp`](SemaHLSL.cpp)。
- 下游: [`ASTContext.h`](../../include/clang/AST/ASTContext.h),
  [`Decl.h`](../../include/clang/AST/Decl.h),
  [`DeclCXX.h`](../../include/clang/AST/DeclCXX.h),
  [`DeclTemplate.h`](../../include/clang/AST/DeclTemplate.h)。
- 关键类/函数: `class HLSLBuiltinTypeDeclBuilder`,
  `HLSLBuiltinTypeDeclBuilder::addMemberVariable`,
  `HLSLBuiltinTypeDeclBuilder::addMemberFunction`,
  `HLSLBuiltinTypeDeclBuilder::complete`。

[`HLSLExternalSemaSource.cpp`](HLSLExternalSemaSource.cpp) — external
sema-source 提供 HLSL-specific type (vector/matrix/texture/atomic)、
intrinsics、namespace scope。
- 上游: Sema 在 HLSL target 检测时构造 `HLSLExternalSemaSource`
  (由 [`SemaHLSL.cpp`](SemaHLSL.cpp) 注册)。
- 下游: [`HLSLBuiltinTypeDeclBuilder.h`](HLSLBuiltinTypeDeclBuilder.h),
  [`ASTContext.h`](../../include/clang/AST/ASTContext.h),
  [`Decl.h`](../../include/clang/AST/Decl.h),
  [`DeclCXX.h`](../../include/clang/AST/DeclCXX.h)。
- 关键类/函数: `class HLSLExternalSemaSource`,
  `HLSLExternalSemaSource::InitializeSema`,
  `HLSLExternalSemaSource::defineHLSLTypesWithForwardDeclarations`,
  `HLSLExternalSemaSource::defineHLSLAtomicIntrinsics`,
  `struct TextureTypeInfo`。

[`SemaOpenCL.cpp`](SemaOpenCL.cpp) — OpenCL extension attribute
(`__opencl_*`)、access qualifier、subgroup-size attribute、kernel
builtin (`ndrange_*`、`get_*_work_group_size`、`enqueue_kernel`、
read/write pipes)。
- 上游: [`SemaDecl.cpp`](SemaDecl.cpp) (kernel/prog attr),
  [`SemaDeclAttr.cpp`](SemaDeclAttr.cpp)。
- 下游: [`Attr.h`](../../include/clang/AST/Attr.h),
  [`Decl.h`](../../include/clang/AST/Decl.h),
  [`SemaOpenCL.h`](../../include/clang/Sema/SemaOpenCL.h)。
- 关键类/函数: `class SemaOpenCL`,
  `SemaOpenCL::handleNoSVMAttr`,
  `SemaOpenCL::handleAccessAttr`,
  `SemaOpenCL::checkSubgroupExt`,
  `SemaOpenCL::checkBuiltinNDRangeAndBlock`,
  `SemaOpenCL::checkBuiltinKernelWorkGroupSize`。

[`SemaSYCL.cpp`](SemaSYCL.cpp) — SYCL 语义检查, `__kernel`、host-only、
device 深度类型检查 (pointer escape)、kernel launch argument 处理。
- 上游: [`SemaDecl.cpp`](SemaDecl.cpp),
  [`SemaDeclAttr.cpp`](SemaDeclAttr.cpp),
  [`SemaTemplateInstantiateDecl.cpp`](SemaTemplateInstantiateDecl.cpp)
  (kernel 实例化)。
- 下游: [`TreeTransform.h`](TreeTransform.h),
  [`Mangle.h`](../../include/clang/AST/Mangle.h),
  [`SYCLKernelInfo.h`](../../include/clang/AST/SYCLKernelInfo.h),
  [`SemaSYCL.h`](../../include/clang/Sema/SemaSYCL.h)。
- 关键类/函数: `class SemaSYCL`,
  `class OutlinedFunctionDeclBodyInstantiator`,
  `SemaSYCL::deepTypeCheckForDevice`,
  `SemaSYCL::handleKernelAttr`,
  `SemaSYCL::CheckDeviceUseOfDecl`,
  `BuildSYCLKernelLaunchCallArgs`。

[`OpenCLBuiltins.td`](OpenCLBuiltins.td) — TableGen source for OpenCL
builtins, 供 TableGen 生成 `OpenCLBuiltins.inc`, 驱动
[`SemaOpenCL.cpp`](SemaOpenCL.cpp) 与 CodeGen。
- 上游: `clang-tblgen` (build tool)。
- 下游: generated `OpenCLBuiltins.inc`。
- 关键类/函数: `class OpenCLBuiltin`, `class OpenCLBuiltinsTableGen`。

### 3.11 Per-target architecture (17 个 Sema<X>.cpp)

每个文件实现一个架构的 `__builtin_*` validation, 经
[`SemaChecking.cpp::CheckBuiltinCall`](SemaChecking.cpp) 分发,
并全部派生自 [`SemaBase`](SemaBase.cpp)。

| Path | 作用 | 关键类/函数 |
|------|-----|------------|
| [`SemaX86.cpp`](SemaX86.cpp) | x86 builtin 检查, MMX/SSE/AVX/AVX-512 operand 校验 (`__builtin_*` gather/scatter/tile/rounding-mode/SAE/AMX)、target attribute `__attribute__((target(...)))` | `class SemaX86`, `SemaX86::CheckBuiltinRoundingOrSAE`, `SemaX86::CheckBuiltinGatherScatterScale`, `SemaX86::CheckBuiltinTileArguments`, `SemaX86::CheckBuiltinFunctionCall`, `SemaX86::handleAnyInterruptAttr` |
| [`SemaARM.cpp`](SemaARM.cpp) | ARM/AArch32/AArch64 target attr、`_Interlocked*` MSVC builtin、Neon immediate 值校验、SVE/SME builtin | `class SemaARM`, `SemaARM::BuiltinARMMemoryTaggingCall`, `SemaARM::BuiltinARMSpecialReg`, `SemaARM::PerformNeonImmChecks`, `SemaARM::PerformSVEImmChecks`, `SemaARM::CheckSMEBuiltinFunctionCall`, `ArmStreamingType` |
| [`SemaAMDGPU.cpp`](SemaAMDGPU.cpp) | AMDGPU builtin 检查, flat work-group size attr、atomic ordering C ABI、mov-dpp、cooperative atomic、AV-load/store、atomic-monitor load | `class SemaAMDGPU`, `SemaAMDGPU::CheckAMDGCNBuiltinFunctionCall`, `SemaAMDGPU::checkAtomicOrderingCABIArg`, `SemaAMDGPU::checkAVLoadStore`, `SemaAMDGPU::checkCoopAtomicFunctionCall`, `SemaAMDGPU::CreateAMDGPUFlatWorkGroupSizeAttr` |
| [`SemaRISCV.cpp`](SemaRISCV.cpp) | RISC-V RVV vector intrinsic 检查, overload resolution between RVV intrinsics、signature 校验、vector type 检查 | `struct RVVIntrinsicDef`, `struct RVVOverloadIntrinsicDef`, `class RISCVIntrinsicManagerImpl`, `RISCVIntrinsicManagerImpl::ConstructRVVIntrinsics`, `RISCVIntrinsicManagerImpl::CreateRVVIntrinsicDecl` |
| [`SemaHexagon.cpp`](SemaHexagon.cpp) | Hexagon builtin argument/function 校验 | `class SemaHexagon`, `SemaHexagon::CheckHexagonBuiltinArgument`, `SemaHexagon::CheckHexagonBuiltinFunctionCall` |
| [`SemaPPC.cpp`](SemaPPC.cpp) | PowerPC builtin 检查 (VSX、MMA、Altivec)、AIX member-alignment 检查、target-clones attribute | `class SemaPPC`, `SemaPPC::checkAIXMemberAlignment`, `SemaPPC::CheckPPCBuiltinFunctionCall`, `SemaPPC::CheckPPCMMAType`, `SemaPPC::BuiltinVSX`, `SemaPPC::checkTargetClonesAttr` |
| [`SemaLoongArch.cpp`](SemaLoongArch.cpp) | LoongArch LA664/LSX/LASX builtin function 校验 | `class SemaLoongArch`, `SemaLoongArch::CheckLoongArchBuiltinFunctionCall` |
| [`SemaWasm.cpp`](SemaWasm.cpp) | WebAssembly builtin 检查, `ref.null_extern`/`ref.is_null_extern`、`table.get`/`table.set`/`table.size`/`table.grow`/`table.fill` | `class SemaWasm`, `SemaWasm::BuiltinWasmRefNullExtern`, `SemaWasm::BuiltinWasmTableGet`, `SemaWasm::BuiltinWasmTableSet`, `SemaWasm::BuiltinWasmTableGrow` |
| [`SemaMIPS.cpp`](SemaMIPS.cpp) | MIPS builtin function 校验、target-cpu builtin、`interrupt` attribute | `class SemaMIPS`, `SemaMIPS::CheckMipsBuiltinFunctionCall`, `SemaMIPS::CheckMipsBuiltinCpu`, `SemaMIPS::CheckMipsBuiltinArgument`, `SemaMIPS::handleInterruptAttr` |
| [`SemaBPF.cpp`](SemaBPF.cpp) | BPF builtin 检查、`preserve_access_index`/`preserve_ai` record attribute | `class SemaBPF`, `SemaBPF::CheckBPFBuiltinFunctionCall`, `SemaBPF::handlePreserveAIRecord`, `SemaBPF::handlePreserveAccessIndexAttr` |
| [`SemaDirectX.cpp`](SemaDirectX.cpp) | DirectX shader-related builtins/attributes; tiny stub | `class SemaDirectX`, `SemaDirectX::CheckDirectXBuiltinFunctionCall` |
| [`SemaSPIRV.cpp`](SemaSPIRV.cpp) | SPIR-V target builtin 检查 | `namespace spirv`, `class SemaSPIRV`, `SemaSPIRV::CheckSPIRVBuiltinFunctionCall` |
| [`SemaNVPTX.cpp`](SemaNVPTX.cpp) | NVPTX builtin function 校验; tiny stub | `class SemaNVPTX`, `SemaNVPTX::CheckNVPTXBuiltinFunctionCall` |
| [`SemaAVR.cpp`](SemaAVR.cpp) | AVR `signal`/`interrupt` attribute 处理 | `class SemaAVR`, `SemaAVR::handleInterruptAttr`, `SemaAVR::handleSignalAttr` |
| [`SemaMSP430.cpp`](SemaMSP430.cpp) | MSP430 `interrupt` attribute 处理 | `class SemaMSP430`, `SemaMSP430::handleInterruptAttr` |
| [`SemaM68k.cpp`](SemaM68k.cpp) | M68k `interrupt` attribute 处理 | `class SemaM68k`, `SemaM68k::handleInterruptAttr` |
| [`SemaSystemZ.cpp`](SemaSystemZ.cpp) | SystemZ (s390x) builtin function 校验 | `class SemaSystemZ`, `SemaSystemZ::CheckSystemZBuiltinFunctionCall` |

### 3.12 Other languages / API

[`SemaSwift.cpp`](SemaSwift.cpp) — Swift-specific attribute 当 Clang
编译时, `swift_attr` / `swift_bridge` / `swift_error` / `swift_name` /
`swift_newtype` / `async` / `__async`。
- 上游: [`SemaDeclAttr.cpp`](SemaDeclAttr.cpp)。
- 下游: [`DeclBase.h`](../../include/clang/AST/DeclBase.h),
  [`AttributeCommonInfo.h`](../../include/clang/Basic/AttributeCommonInfo.h),
  [`Sema.h`](../../include/clang/Sema/Sema.h)。
- 关键类/函数: `class SemaSwift`, `SemaSwift::handleAttrAttr`,
  `SemaSwift::handleBridge`, `SemaSwift::handleError`,
  `SemaSwift::handleName`, `SemaSwift::handleNewType`。

[`SemaBoundsSafety.cpp`](SemaBoundsSafety.cpp) — `-fbounds-safety`
(Bounds Safety, 源自 Apple `bounds.h`), `counted_by`/`sized_by` pointer
校验、range-checked type。
- 上游: [`SemaInit.cpp`](SemaInit.cpp),
  [`SemaExpr.cpp`](SemaExpr.cpp)。
- 下游: [`Type.h`](../../include/clang/AST/Type.h),
  [`Initialization.h`](../../include/clang/Sema/Initialization.h),
  [`Sema.h`](../../include/clang/Sema/Sema.h)。
- 关键类/函数: `Sema::CheckCountedByAttrOnField`,
  `Sema::BoundsSafetyCheckAssignmentToCountAttrPtr`,
  `Sema::BoundsSafetyCheckInitialization`,
  `Sema::BoundsSafetyCheckUseOfCountAttrPtr`,
  `CountAttributedType::DynamicCountPointerKind`。

[`SemaExpand.cpp`](SemaExpand.cpp) — C++26 expansion statement (`template
for`), `ExpansionStmt` 的 sema、template-parameterized loop。
- 上游: [`SemaStmt.cpp`](SemaStmt.cpp)。
- 下游: [`StmtCXX.h`](../../include/clang/AST/StmtCXX.h),
  [`Preprocessor.h`](../../include/clang/Lex/Preprocessor.h),
  [`EnterExpressionEvaluationContext.h`](../../include/clang/Sema/EnterExpressionEvaluationContext.h)。
- 关键类/函数: `struct IterableExpansionStmtData`,
  `Sema::ActOnCXXExpansionStmtDecl`,
  `Sema::BuildCXXExpansionStmtDecl`,
  `Sema::ComputeExpansionSize`。

[`SemaFunctionEffects.cpp`](SemaFunctionEffects.cpp) — C++26
`[[clang::nonblocking]]` / `[[clang::nonallocating]]` 等 function-effect
attribute, violation 检测与分析。
- 上游: [`SemaDeclAttr.cpp`](SemaDeclAttr.cpp),
  [`SemaChecking.cpp`](SemaChecking.cpp)。
- 下游: [`Decl.h`](../../include/clang/AST/Decl.h),
  [`DeclCXX.h`](../../include/clang/AST/DeclCXX.h),
  [`DynamicRecursiveASTVisitor.h`](../../include/clang/AST/DynamicRecursiveASTVisitor.h)。
- 关键类/函数: `class ViolationSite`, `struct Violation`,
  `class EffectToViolationMap`, `class PendingFunctionAnalysis`,
  `class Analyzer`, `Sema::diagnoseConflictingFunctionEffect`。

[`SemaAPINotes.cpp`](SemaAPINotes.cpp) — maps API Notes annotation
(Swift name, nullability, error-handling, availability) onto parsed
declaration。
- 上游: [`Sema.cpp`](Sema.cpp) (`Sema::ProcessAPINotes`),
  [`SemaAPINotesInternal.h`](SemaAPINotesInternal.h)。
- 下游: [`APINotes/APINotesReader.h`](../../include/clang/APINotes/APINotesReader.h),
  [`APINotes/Types.h`](../../include/clang/APINotes/Types.h),
  [`Decl.h`](../../include/clang/AST/Decl.h)。
- 关键类/函数: `struct VersionedInfoMetadata`,
  `handleAPINotedAttribute`, `Sema::ApplyAPINotesType`,
  `Sema::ApplyNullability`, `Sema::ProcessAPINotes`,
  `struct APINotesParameterSelector`。

[`SemaAPINotesInternal.h`](SemaAPINotesInternal.h) — 内部 header, 给
[`SemaAPINotes.cpp`](SemaAPINotes.cpp) 与 [`Sema.cpp`](Sema.cpp) 共享的
type/structure。
- 上游: [`SemaAPINotes.cpp`](SemaAPINotes.cpp),
  [`Sema.cpp`](Sema.cpp)。
- 下游: [`APINotes/Types.h`](../../include/clang/APINotes/Types.h),
  [`SourceLocation.h`](../../include/clang/Basic/SourceLocation.h)。
- 关键类/函数: `struct APINotesSelectorDiagnosticState`,
  `struct APINotesSelectorDiagnosticReaderState`。

[`SemaAvailability.cpp`](SemaAvailability.cpp) — `availability`
attribute / `__builtin_available` / `__has_feature`, versioned
availability 检查、fallback diagnostics、deferred availability 检查。
- 上游: [`SemaDecl.cpp`](SemaDecl.cpp),
  [`SemaDeclAttr.cpp`](SemaDeclAttr.cpp),
  [`SemaExpr.cpp`](SemaExpr.cpp)。
- 下游: [`Attr.h`](../../include/clang/AST/Attr.h),
  [`Decl.h`](../../include/clang/AST/Decl.h),
  [`DynamicRecursiveASTVisitor.h`](../../include/clang/AST/DynamicRecursiveASTVisitor.h),
  [`DiagnosticSema.h`](../../include/clang/Basic/DiagnosticSema.h)。
- 关键类/函数: `Sema::ShouldDiagnoseAvailabilityOfDecl`,
  `class DiagnoseUnguardedAvailability`, `class StmtUSEFinder`,
  `class LastDeclUSEFinder`, `Sema::handleDelayedAvailabilityCheck`,
  `struct ExtractedAvailabilityExpr`。

[`MultiplexExternalSemaSource.cpp`](MultiplexExternalSemaSource.cpp) —
`MultiplexExternalSemaSource`, fan-out `ExternalASTSource` query 到多个
底层 source、merge 结果。
- 上游: [`Sema.cpp`](Sema.cpp) (构造 multiplexer)。
- 下游: [`MultiplexExternalSemaSource.h`](../../include/clang/Sema/MultiplexExternalSemaSource.h),
  [`Lookup.h`](../../include/clang/Sema/Lookup.h)。
- 关键类/函数: `class MultiplexExternalSemaSource`,
  `MultiplexExternalSemaSource::AddSource`,
  `MultiplexExternalSemaSource::FindExternalVisibleDeclsByName`。

[`HeuristicResolver.cpp`](HeuristicResolver.cpp) — `HeuristicResolver`,
名字查找 suggestion 与 recovery path 用的 "best-guess" resolution for
dependent expr/type。
- 上游: [`SemaLookup.cpp`](SemaLookup.cpp),
  [`SemaExpr.cpp`](SemaExpr.cpp)。
- 下游: [`HeuristicResolver.h`](../../include/clang/Sema/HeuristicResolver.h),
  [`DeclTemplate.h`](../../include/clang/AST/DeclTemplate.h),
  [`ExprCXX.h`](../../include/clang/AST/ExprCXX.h)。
- 关键类/函数: `class HeuristicResolverImpl`,
  `HeuristicResolverImpl::resolveDeclRefExpr`,
  `HeuristicResolverImpl::resolveCalleeOfCallExpr`,
  `HeuristicResolverImpl::resolveDependentNameType`,
  `HeuristicResolverImpl::resolveTemplateSpecializationType`。

[`DeclSpec.cpp`](DeclSpec.cpp) — `DeclSpec` 实现, 声明 specifier parsing
storage (storage class, type spec, function specifier), short-circuit
type-name resolution, conversion to `QualType`。
- 上游: Parser 在收集 specifier 时;
  [`SemaDecl.cpp`](SemaDecl.cpp) (`getTypeForDeclarator`)。
- 下游: [`DeclSpec.h`](../../include/clang/Sema/DeclSpec.h),
  [`ASTContext.h`](../../include/clang/AST/ASTContext.h),
  [`DeclCXX.h`](../../include/clang/AST/DeclCXX.h)。
- 关键类/函数: `class DeclSpec`, `DeclSpec::Finish`,
  `DeclSpec::getTypeSpecType`, `DeclSpec::SetTypeSpecType`,
  `DeclSpec::isEmpty`。

[`ParsedAttr.cpp`](ParsedAttr.cpp) — `ParsedAttr` (raw form of an
attribute, before semantic), appertainment, subject matching rules。
- 上游: Parser 在遇到 `[[attr]]` / `__attribute__((...))`;
  [`SemaAttr.cpp`](SemaAttr.cpp)。
- 下游: [`ParsedAttr.h`](../../include/clang/Sema/ParsedAttr.h),
  [`AttrSubjectMatchRules.h`](../../include/clang/Basic/AttrSubjectMatchRules.h),
  [`ASTContext.h`](../../include/clang/AST/ASTContext.h)。
- 关键类/函数: `class ParsedAttr`, `ParsedAttr::getMinArgs`,
  `ParsedAttr::getMaxArgs`, `ParsedAttr::isTargetSpecificAttr`。

### 3.13 Helper utilities

[`TypeLocBuilder.cpp`](TypeLocBuilder.cpp) +
[`TypeLocBuilder.h`](TypeLocBuilder.h) — `TypeLocBuilder`, 从 `Type`
节点增量构造 `TypeLoc` (自底向上), 用于 declarator / type source info。
- 上游: 18 个 .cpp 文件包括 [`SemaDecl.cpp`](SemaDecl.cpp),
  [`SemaType.cpp`](SemaType.cpp),
  [`SemaLambda.cpp`](SemaLambda.cpp),
  [`SemaTemplateInstantiateDecl.cpp`](SemaTemplateInstantiateDecl.cpp)。
- 下游: [`TypeLoc.h`](../../include/clang/AST/TypeLoc.h),
  [`Type.h`](../../include/clang/AST/Type.h)。
- 关键类/函数: `class TypeLocBuilder`, `TypeLocBuilder::push`,
  `TypeLocBuilder::pushFullDesugared`,
  `TypeLocBuilder::getTypeSourceInfo`, `class TypeLocTLRet`。

[`TreeTransform.h`](TreeTransform.h) — `TreeTransform<Derived>` CRTP 基
类, 访问者 rebuild AST tree 同时 substitute template arguments; 模板
实例化的基石 (731 KB, 最大 header)。
- 上游: [`SemaTemplateInstantiate.cpp`](SemaTemplateInstantiate.cpp),
  [`SemaTemplateInstantiateDecl.cpp`](SemaTemplateInstantiateDecl.cpp),
  [`SemaConcept.cpp`](SemaConcept.cpp),
  [`SemaTemplateDeduction.cpp`](SemaTemplateDeduction.cpp),
  [`SemaExprCXX.cpp`](SemaExprCXX.cpp),
  [`SemaSYCL.cpp`](SemaSYCL.cpp)。
- 下游: [`Expr.h`](../../include/clang/AST/Expr.h),
  [`ExprCXX.h`](../../include/clang/AST/ExprCXX.h),
  [`Type.h`](../../include/clang/AST/Type.h),
  [`Sema.h`](../../include/clang/Sema/Sema.h)。
- 关键类/函数: `class TreeTransform<Derived>`,
  `TreeTransform::Transform`, `TreeTransform::TransformType`,
  `TreeTransform::TransformExpr`,
  `TreeTransform::RebuildTemplateInstantiation`,
  `TreeTransform::AlwaysRebuild`。

[`UsedDeclVisitor.h`](UsedDeclVisitor.h) — `UsedDeclVisitor` CRTP
visitor, 走 expr/stmt 把 decl 标为 ODR-used (并发 unused value warning)。
- 上游: [`Sema.cpp`](Sema.cpp), [`SemaExpr.cpp`](SemaExpr.cpp)。
- 下游: [`Decl.h`](../../include/clang/AST/Decl.h),
  [`Expr.h`](../../include/clang/AST/Expr.h)。
- 关键类/函数: `class UsedDeclVisitor<VisitorImpl>`。

### 3.14 Build / misc

[`CMakeLists.txt`](CMakeLists.txt) — 列出 `clangSema` library target 的
所有 `.cpp` 文件; 引用 [`OpenCLBuiltins.td`](OpenCLBuiltins.td) 供
TableGen 使用。

---

## §4. 关键调用链

### 4.1 顶层 Parser → Sema 入口

```
Parser::ParseAST [clang/lib/Parse/ParseAST.cpp]
  └─ ParseTranslationUnit → ParseDeclOrFunctionDef / ParseStatement / ParseExpression
       └─ 每个语法节点调对应 Sema::ActOn*:
            ├─ Sema::ActOnDeclarator [SemaDecl.cpp]
            │    └─ Sema::getTypeForDeclarator [SemaType.cpp]
            │         ├─ 处理 declarator chunk (TypeLocBuilder)
            │         └─ 检查 function-typed CC (SemaChecking 配合)
            ├─ Sema::ActOnTagStartDefinition [SemaDecl.cpp]
            │    └─ 建立 CXXRecordDecl (SemaDeclCXX.cpp 接力)
            ├─ Sema::ActOnIdentifierExpr [SemaExpr.cpp]
            │    ├─ Sema::LookupName [SemaLookup.cpp]
            │    ├─ IdentifierResolver::isDeclInScope
            │    └─ overload resolution [SemaOverload.cpp]
            ├─ Sema::ActOnCallExpr [SemaExpr.cpp]
            │    ├─ 实参转换 (SemaOverload.cpp)
            │    ├─ 可调用性 / CUDA host-device 关系 (SemaCUDA.cpp)
            │    ├─ builtin dispatch (SemaChecking.cpp)
            │    │    └─ per-arch Sema<X>::Check<X>BuiltinFunctionCall
            │    │       (SemaX86 / SemaARM / SemaAMDGPU / SemaRISCV / ...)
            │    └─ format string 检查 (SemaChecking.cpp)
            ├─ Sema::ActOnIfStmt / ActOnForStmt / ActOnSwitchStmt [SemaStmt.cpp]
            │    └─ JumpScopeChecker [JumpDiagnostics.cpp]
            ├─ Sema::ActOnCompoundStmt
            │    └─ push/pop scope [Scope.cpp]
            ├─ Sema::ActOnLambdaExpr [SemaExpr.cpp]
            │    └─ Sema::createLambdaClosureType [SemaLambda.cpp]
            ├─ Sema::ActOnTemplateDeclaration [SemaTemplate.cpp]
            │    ├─ Parse template parameter list
            │    ├─ Sema::FilterAcceptableTemplateNames [SemaTemplate.cpp]
            │    └─ 模板参数推导 (SemaTemplateDeduction.cpp)
            │         └─ instantiation (SemaTemplateInstantiate.cpp)
            │              └─ TreeTransform [TreeTransform.h]
            │                   └─ SemaTemplateInstantiateDecl.cpp
            ├─ Sema::ActOnObjCMessageExpr [SemaExprObjC.cpp]
            │    └─ SemaObjC::CheckMessageArgumentTypes [SemaObjC.cpp]
            │         └─ SemaPseudoObject.cpp [PseudoOpBuilder]
            ├─ Sema::ActOnOpenMPExecutableDirective [SemaOpenMP.cpp]
            │    ├─ DSAStackTy::addDSA
            │    └─ per-clause 校验 (clause visitor)
            ├─ Sema::ActOnCXXTypeid / ActOnCXXThrow [SemaExprCXX.cpp]
            │    └─ 异常 spec 检查 [SemaExceptionSpec.cpp]
            └─ Sema::ActOnCoroutineBodyStart [SemaCoroutine.cpp]
                 └─ CoroutineStmtBuilder [CoroutineStmtBuilder.h]
```

### 4.2 重载解析链 (`SemaOverload.cpp`)

```
Sema::AddOverloadCandidate [SemaOverload.cpp]
  ├─ 对每个 candidate:
  │    ├─ Conversion sequence 分类 (Standard / UserDefined / Ambiguous / Ellipsis)
  │    ├─ built-in operator overload set 查找
  │    ├─ inherited constructor 候选检查
  │    └─ DeductionFailureInfo 缓存
  └─ 返回 OverloadCandidateSet
       └─ Sema::BestViableFunction [SemaOverload.cpp]
            ├─ tie-break (cv-qual / reference / trivial / ...)
            ├─ TemplateDeduction [SemaTemplateDeduction.cpp]
            └─ CheckInitLifetime / checkAssignmentLifetime
                 [CheckExprLifetime.cpp]
```

### 4.3 模板推导 + 实例化链

```
Sema::DeduceTemplateArguments [SemaTemplateDeduction.cpp]
  ├─ PackDeductionScope 处理 pack expansion
  ├─ isSameOrCompatibleFunctionType 比较
  ├─ CTAD: ConvertConstructorToDeductionGuideTransform
  │        [SemaTemplateDeductionGuide.cpp]
  └─ 选模板参数 → response
       └─ Sema::InstantiateFunctionDeclaration [SemaTemplateInstantiate.cpp]
            ├─ InstantiatingTemplate RAII (depth tracking)
            └─ Sema::InstantiateTemplateFunctionDecl
                 └─ TemplateDeclInstantiator
                      [SemaTemplateInstantiateDecl.cpp]
                      └─ TreeTransform<Derived>::Transform
                           [TreeTransform.h]
                           ├─ Rebuild template instantiation
                           ├─ Sema::InstantiateAttrsForDecl
                           └─ SYCL: OutlinedFunctionDeclBodyInstantiator
                                [SemaSYCL.cpp]
```

### 4.4 OpenMP 处理链

```
Sema::ActOnOpenMPExecutableDirective [SemaOpenMP.cpp]
  ├─ Parse 关联 statement + clause 列表
  ├─ DSAStackTy::push (data-sharing analysis)
  │    └─ 添加 implicit / explicit data-sharing attribute
  ├─ 对每条 clause:
  │    ├─ private / firstprivate / reduction / ...
  │    ├─ shared / default / proc_bind / ...
  │    └─ metadirective / declare-variant 解析
  └─ emit outlined 函数:
       └─ emitParallel / emitTask / emitTeams
            └─ outline kernel (CGOpenMPRuntime 接力)
```

### 4.5 Per-arch builtin 校验链

```
Sema::CheckBuiltinCall [SemaChecking.cpp]
  ├─ switch (BuiltinID):
  │    ├─ X86: SemaX86::CheckBuiltinFunctionCall [SemaX86.cpp]
  │    │    └─ CheckBuiltinRoundingOrSAE / CheckBuiltinTileArguments / ...
  │    ├─ ARM: SemaARM::CheckARMBuiltinFunctionCall / SVE/SME/Neon
  │    ├─ AMDGPU: SemaAMDGPU::CheckAMDGCNBuiltinFunctionCall
  │    │    └─ checkAtomicOrderingCABIArg / checkAVLoadStore
  │    ├─ RISCV: RISCVIntrinsicManagerImpl (构造 + overload)
  │    ├─ Hexagon: SemaHexagon::CheckHexagonBuiltinFunctionCall
  │    ├─ PPC: SemaPPC::CheckPPCBuiltinFunctionCall / CheckPPCMMAType
  │    ├─ LoongArch: SemaLoongArch::CheckLoongArchBuiltinFunctionCall
  │    ├─ Wasm: SemaWasm::BuiltinWasmRef* / BuiltinWasmTable*
  │    ├─ MIPS: SemaMIPS::CheckMipsBuiltinFunctionCall
  │    ├─ BPF: SemaBPF::CheckBPFBuiltinFunctionCall
  │    ├─ DirectX: SemaDirectX::CheckDirectXBuiltinFunctionCall
  │    ├─ SPIRV: SemaSPIRV::CheckSPIRVBuiltinFunctionCall
  │    ├─ NVPTX: SemaNVPTX::CheckNVPTXBuiltinFunctionCall
  │    └─ SystemZ: SemaSystemZ::CheckSystemZBuiltinFunctionCall
  └─ 返回是否诊断错
```

### 4.6 HLSL / OpenCL / SYCL 处理链

```
Sema 初始化时:
  ├─ HLSL target 检测 → SemaHLSL 构造 → HLSLExternalSemaSource 注册
  │    └─ defineHLSLTypesWithForwardDeclarations
  │         └─ HLSLBuiltinTypeDeclBuilder (vector/matrix/texture/...)
  ├─ OpenCL target → SemaOpenCL 构造
  │    └─ OpenCLBuiltins.inc 由 OpenCLBuiltins.td TableGen 生成
  └─ SYCL target → SemaSYCL 构造

每条 kernel / entry point declaration:
  ├─ SemaHLSL::CheckEntryPoint (HLSL)
  │    └─ mergeVkConstantIdAttr / mergeShaderAttr
  │         └─ determineActiveSemantic
  ├─ SemaOpenCL::checkBuiltinKernelWorkGroupSize (OpenCL)
  └─ SemaSYCL::handleKernelAttr (SYCL)
       └─ deepTypeCheckForDevice
            └─ instantiation 时 OutlinedFunctionDeclBodyInstantiator
```

---

## §5. 推荐阅读顺序

### 阶段 1: 框架入门 (1-2 小时)
1. [`Sema.h`](../../include/clang/Sema/Sema.h) — `class Sema` 表面。
2. [`SemaInternal.h`](../../include/clang/Sema/SemaInternal.h) —
   libSema 内部细节。
3. [`Sema.cpp`](Sema.cpp) — 看构造函数 + `Initialize`/`TearDown` +
   `ActOn*`/`Build*` 入口。
4. [`Scope.h`](../../include/clang/Sema/Scope.h) +
   [`ScopeInfo.h`](../../include/clang/Sema/ScopeInfo.h) +
   [`Scope.cpp`](Scope.cpp) + [`ScopeInfo.cpp`](ScopeInfo.cpp) —
   lexical / function scoping。
5. [`IdentifierResolver.cpp`](IdentifierResolver.cpp) — identifier
   lookup mechanism。

### 阶段 2: Lookup + Decl (2-3 小时)
- [`SemaLookup.cpp`](SemaLookup.cpp) — name lookup + `LookupResult`
- [`SemaDecl.cpp`](SemaDecl.cpp) → [`SemaDeclAttr.cpp`](SemaDeclAttr.cpp) →
  [`SemaDeclCXX.cpp`](SemaDeclCXX.cpp) → [`SemaDeclObjC.cpp`](SemaDeclObjC.cpp) —
  声明 emission 顺序 (C → C++ → ObjC)
- [`SemaType.cpp`](SemaType.cpp) → [`SemaCast.cpp`](SemaCast.cpp) →
  [`SemaTypeTraits.cpp`](SemaTypeTraits.cpp) — 类型系统

### 阶段 3: Expression + Overload (3-4 小时)
- [`SemaExpr.cpp`](SemaExpr.cpp) → [`SemaOverload.cpp`](SemaOverload.cpp) →
  [`SemaExprCXX.cpp`](SemaExprCXX.cpp) →
  [`SemaExprMember.cpp`](SemaExprMember.cpp) →
  [`SemaExprObjC.cpp`](SemaExprObjC.cpp) →
  [`SemaLambda.cpp`](SemaLambda.cpp) — 表达式检查
- [`SemaInit.cpp`](SemaInit.cpp) — initializer
- [`SemaChecking.cpp`](SemaChecking.cpp) — format string / fortified /
  builtin 实参数

### 阶段 4: Template (3 小时)
1. [`SemaTemplate.cpp`](SemaTemplate.cpp) → [`SemaTemplateInstantiate.cpp`](SemaTemplateInstantiate.cpp)
2. [`SemaTemplateInstantiateDecl.cpp`](SemaTemplateInstantiateDecl.cpp)
3. [`SemaTemplateDeduction.cpp`](SemaTemplateDeduction.cpp) →
   [`SemaTemplateDeductionGuide.cpp`](SemaTemplateDeductionGuide.cpp)
4. [`SemaTemplateVariadic.cpp`](SemaTemplateVariadic.cpp)
5. [`SemaConcept.cpp`](SemaConcept.cpp) — C++20 concepts

### 阶段 5: Stmt / Exception / Coroutine (1-2 小时)
- [`SemaStmt.cpp`](SemaStmt.cpp) → [`SemaStmtAsm.cpp`](SemaStmtAsm.cpp) →
  [`SemaStmtAttr.cpp`](SemaStmtAttr.cpp)
- [`SemaExceptionSpec.cpp`](SemaExceptionSpec.cpp)
- [`SemaCoroutine.cpp`](SemaCoroutine.cpp) +
  [`CoroutineStmtBuilder.h`](CoroutineStmtBuilder.h)
- [`CheckExprLifetime.cpp`](CheckExprLifetime.cpp) +
  [`SemaLifetimeSafety.h`](SemaLifetimeSafety.h)

### 阶段 6: 诊断 (1 小时)
[`Access`](SemaAccess.cpp) + [`CXXScopeSpec`](SemaCXXScopeSpec.cpp) +
[`FixItUtils`](SemaFixItUtils.cpp) + [`JumpDiagnostics`](JumpDiagnostics.cpp)
+ [`AnalysisBasedWarnings`](AnalysisBasedWarnings.cpp) +
[`SemaAttr`](SemaAttr.cpp) + [`SemaAvailability`](SemaAvailability.cpp)
+ [`DelayedDiagnostic.cpp`](DelayedDiagnostic.cpp)

### 阶段 7: Code completion + ObjC (1-2 小时)
- [`SemaCodeComplete.cpp`](SemaCodeComplete.cpp) +
  [`CodeCompleteConsumer.cpp`](CodeCompleteConsumer.cpp)
- [`SemaObjC.cpp`](SemaObjC.cpp) +
  [`SemaObjCProperty.cpp`](SemaObjCProperty.cpp) +
  [`SemaPseudoObject.cpp`](SemaPseudoObject.cpp)

### 阶段 8: 语言子模块 (按需)
- OpenMP: [`SemaOpenMP.cpp`](SemaOpenMP.cpp) (最大单文件 1.1 MB)
- OpenACC: [`SemaOpenACC.cpp`](SemaOpenACC.cpp) + 系列 clause 文件
- CUDA: [`SemaCUDA.cpp`](SemaCUDA.cpp)
- HLSL: [`SemaHLSL.cpp`](SemaHLSL.cpp) +
  [`HLSLExternalSemaSource.cpp`](HLSLExternalSemaSource.cpp) +
  [`HLSLBuiltinTypeDeclBuilder.cpp`](HLSLBuiltinTypeDeclBuilder.cpp)
- OpenCL: [`SemaOpenCL.cpp`](SemaOpenCL.cpp) +
  [`OpenCLBuiltins.td`](OpenCLBuiltins.td)
- SYCL: [`SemaSYCL.cpp`](SemaSYCL.cpp)
- Swift / Bounds Safety / Expand: [`SemaSwift.cpp`](SemaSwift.cpp) /
  [`SemaBoundsSafety.cpp`](SemaBoundsSafety.cpp) /
  [`SemaExpand.cpp`](SemaExpand.cpp)
- Function Effects: [`SemaFunctionEffects.cpp`](SemaFunctionEffects.cpp)

### 阶段 9: Per-arch builtin (1-2 小时)
从 [`SemaX86.cpp`](SemaX86.cpp) (最有代表性) 开始, 然后 [`SemaARM.cpp`](SemaARM.cpp)、
[`SemaAMDGPU.cpp`](SemaAMDGPU.cpp)、[`SemaRISCV.cpp`](SemaRISCV.cpp);
小的 (AVR / M68k / MSP430) 只是 attribute handler。

### 阶段 10: Utilities + API (30 分钟)
[`SemaAPINotes.cpp`](SemaAPINotes.cpp) +
[`MultiplexExternalSemaSource.cpp`](MultiplexExternalSemaSource.cpp) +
[`HeuristicResolver.cpp`](HeuristicResolver.cpp) +
[`DeclSpec.cpp`](DeclSpec.cpp) + [`ParsedAttr.cpp`](ParsedAttr.cpp) +
[`SemaBase.cpp`](SemaBase.cpp) + [`SemaConsumer.cpp`](SemaConsumer.cpp) +
[`TypeLocBuilder.cpp`](TypeLocBuilder.cpp) +
[`TreeTransform.h`](TreeTransform.h) +
[`UsedDeclVisitor.h`](UsedDeclVisitor.h)

---

## §6. 常用操作指南

### 6.1 添加新 builtin (target-agnostic, e.g. `__builtin_myop`)

1. 在 `clang/include/clang/Basic/Builtins.def` 加 builtin 声明
   (指定类型签名 + 副作用)。
2. 在 [`SemaChecking.cpp`](SemaChecking.cpp) 加 builtin 实参数检查
   (实参数类型 / 副作用一致性)。
4. 测试: `clang/test/Sema/builtin-myop.c`。

### 6.2 给现有 arch 添加新 builtin

1. 在 `clang/include/clang/Basic/Builtins<Arch>.def` 加 builtin 声明。
2. 在 [`Sema<Arch>.cpp`](SemaX86.cpp) 的 `Check<Arch>BuiltinFunctionCall`
   switch 加新 case。
3. 在 [`Targets/<Arch>.cpp`](../CodeGen/TargetBuiltins/) 加 codegen
   实现。
4. 测试: `clang/test/Sema/<arch>-builtins-<name>.c`。

### 6.3 给 Clang 加新语言特性 (AST + Sema)

1. 在 [`clang/lib/AST/`](../AST/) 加 AST 节点 (Stmt / Expr / Decl 子类)。
2. 在 [`SemaDecl.cpp`](SemaDecl.cpp) /
   [`SemaExpr.cpp`](SemaExpr.cpp) /
   [`SemaStmt.cpp`](SemaStmt.cpp) 加对应 `ActOn*` /
   `Build*` 入口。
3. 在 [`Parse/`](../Parse/) 加 parser 入口, 调用 Sema hook。
4. 在 [`CodeGen/`](../CodeGen/) 加 IR 发射。
5. 测试: `clang/test/SemaCXX/<feature>` + `clang/test/CodeGenCXX/<feature>`。

### 6.4 添加新 OpenMP directive / clause

1. 在 `clang/include/clang/AST/StmtOpenMP.h` 加 AST 节点;
   在 `OpenMPClause.h` 加 clause 节点 (一般 Sema 处理)。
2. 在 [`SemaOpenMP.cpp`](SemaOpenMP.cpp) 的 `ActOn*` 分发加新 case
   (经 `ActOnOpenMPExecutableDirective`)。
3. 在 `llvm/include/llvm/Frontend/OpenMP/` (OpenMPIRBuilder) 加
   codegen outline (一般由 CodeGen 接力)。
4. 测试: `clang/test/OpenMP/<directive>*.c`。

### 6.5 添加新 HLSL 内置

1. 在 `clang/include/clang/Basic/BuiltinsHLSL.def` 加 builtin。
2. 在 [`CGHLSLBuiltins.cpp`](../CodeGen/CGHLSLBuiltins.cpp) 加 codegen。
3. 在 [`SemaHLSL.cpp`](SemaHLSL.cpp) 加 Sema validation (必要时)。
4. 测试: `clang/test/SemaHLSL/<feature>.hlsl`。

### 6.6 添加新 arch (假设 `MyArch`)

1. 在 `clang/include/clang/Basic/BuiltinsMyArch.def` 加 builtin 声明。
2. 在 [`Targets/MyArch.cpp`](../CodeGen/Targets/) 创建
   `MyArchTargetCodeGenInfo` + `MyArchABIInfo` (在
   [`CodeGen`](../CodeGen/) 目录)。
3. 在 [`Targets/TargetBuiltins/MyArch.cpp`](../CodeGen/TargetBuiltins/)
   加 codegen 实现。
4. 在 [`SemaMyArch.cpp`](SemaX86.cpp) 加 builtin 校验
   (派生自 [`SemaBase`](SemaBase.cpp))。
5. 在 [`SemaChecking.cpp`](SemaChecking.cpp) 的 builtin dispatch
   switch 加 `MyArch::CheckMyArchBuiltinFunctionCall`。
6. 注册到 [`clang/Basic/Targets/`](../CodeGen/Targets/)。
7. 测试: `clang/test/Sema/<arch>-builtins.c`。

### 6.7 调试 Sema 诊断

1. 加 `-Xclang -ast-dump` 看 AST。
2. 加 `-Xclang -ast-dump=json` 看 JSON AST。
3. 在 [`Sema.cpp`](Sema.cpp) 的 `LookupName` / `ActOnDeclarator` 加
   `llvm::errs() << ...` 临时 trace。
4. 看 `Sema::Diag(...)` 的 ID (从
   [`clang/include/clang/Basic/DiagnosticSemaKinds.td`](../../include/clang/Basic/DiagnosticSemaKinds.td)
   来)。
5. 单步跟 [`clang/AST`](../AST/) 节点创建, 检查每个 `set*` 调用。

### 6.8 调试 template 推导 / 实例化

1. 加 `-Xclang -ast-dump -ast-dump-decls -ast-dump-filter=MyClass` 看
   模板实例化结果。
2. 加 `-Xclang -print-stats` 看模板实例化统计。
3. 看 [`SemaTemplateDeduction.cpp`](SemaTemplateDeduction.cpp) 的
   `DeduceTemplateArguments` 输入/输出。
4. 跟 [`TreeTransform.h`](TreeTransform.h) 的 `Transform<...>` 调用
   栈, 看每个节点 rebuild。

---

## §7. NT 注释索引

当前 `clang/lib/Sema/` 下尚无 `// <NT>` 注释。已建立目录索引, 姊妹
overview:

- `clang/lib/AST/` — Clang AST 层 (待写)
- [`clang/lib/CodeGen/0-overview.md`](../CodeGen/0-overview.md) — Clang
  AST → LLVM IR
- `clang/lib/Frontend/` — Clang 前端桥接 (待写)
- `clang/lib/Lex/` — Clang Lexer (待写)
- `clang/lib/Parse/` — Clang Parser (待写)
- [`llvm/lib/IR/0-overview.md`](../../../llvm/lib/IR/0-overview.md) — LLVM IR 层
- [`llvm/lib/CodeGen/0-overview.md`](../../../llvm/lib/CodeGen/0-overview.md) — LLVM 后端
- [`llvm/lib/Object/0-overview.md`](../../../llvm/lib/Object/0-overview.md) — LLVM 二进制对象解析

按"少而精"原则, 加 NT 注释建议优先级:

1. [`SemaOpenMP.cpp`](SemaOpenMP.cpp) — 8-12 段 (最大单文件 1.1 MB, OpenMP 核心)
2. [`SemaDecl.cpp`](SemaDecl.cpp) — 6-10 段 (Sema 最大单文件, 声明 emission)
3. [`SemaExpr.cpp`](SemaExpr.cpp) — 6-8 段 (表达式入口)
4. [`SemaChecking.cpp`](SemaChecking.cpp) — 5-8 段 (额外检查)
5. [`SemaOverload.cpp`](SemaOverload.cpp) — 5-8 段 (重载解析)
6. [`SemaLookup.cpp`](SemaLookup.cpp) — 4-6 段 (名字查找)
7. [`SemaTemplateInstantiateDecl.cpp`](SemaTemplateInstantiateDecl.cpp) — 5-7 段
   (模板实例化)
8. [`TreeTransform.h`](TreeTransform.h) — 5-8 段 (最大 header, 模板 substitute 核心)
9. [`SemaInit.cpp`](SemaInit.cpp) — 4-6 段 (初始化)
10. [`SemaStmt.cpp`](SemaStmt.cpp) — 4-6 段 (语句入口)
11. 单 arch 文件 ([`SemaX86.cpp`](SemaX86.cpp) /
    [`SemaARM.cpp`](SemaARM.cpp) /
    [`SemaAMDGPU.cpp`](SemaAMDGPU.cpp) /
    [`SemaRISCV.cpp`](SemaRISCV.cpp)) — 各 3-5 段
12. 单语言模块 ([`SemaHLSL.cpp`](SemaHLSL.cpp) /
    [`SemaOpenACC.cpp`](SemaOpenACC.cpp) /
    [`SemaCUDA.cpp`](SemaCUDA.cpp) /
    [`SemaSYCL.cpp`](SemaSYCL.cpp)) — 各 3-5 段

---

**姊妹文档**: 本目录对应 LLVM 流水线中的 **Clang 语义分析层** (Parse
之后, CodeGen/AST 之前)。它与 [`clang/lib/CodeGen/`](../CodeGen/0-overview.md)
(下游: 完全类型化的 AST → LLVM IR) 形成完整 Clang 编译流水线。Clang 用户
可见的选项在 [`clang/include/clang/Driver/`](../../include/clang/Driver/)
+ [`clang/lib/Driver/`](../Driver/)。