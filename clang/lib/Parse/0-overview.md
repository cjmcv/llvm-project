<!-- <NT>overview:clang/lib/Parse/ -->

# Clang Parser 库导读 — `clang/lib/Parse/`

> 本文档梳理 `clang/lib/Parse/` 目录下所有源文件 (19 个 .cpp/.h + 1
> 个 `CMakeLists.txt` = 20 个) 的职责、上下游与推荐阅读顺序。
>
> 目标读者: 想理解 **Clang 语法分析器** (Token 流 → AST 骨架:
> Decl / Stmt / Expr 节点) 的开发者, 以及要给 Clang 加新语法 / 新
> 表达式形式 / 新声明 / 新 pragma handler / 新扩展 (OpenACC /
> OpenMP / HLSL / ObjC / C++ 反射) 的人。
>
> 所有路径相对 `clang/lib/Parse/`。同名公开头文件位于
> `clang/include/clang/Parse/` (如 `Parser.h`、`ParseAST.h`、
> `RAIIObjectsForParser.h`、`LoopHint.h`、
> `ParseHLSLRootSignature.h`)。本目录编译为 `clangParse` 库, 链接
> `clangAST/Basic/Lex/Sema` + LLVM MCParser。

---

## §0. Clang Parser 库在编译流水线中的位置

`clang/lib/Parse` 是 Clang 的 **语法分析器**, 从
[`clang/lib/Lex/`](../Lex/) 的 `Preprocessor::Lex` 取 token, 按
C / C++ / ObjC / OpenACC / OpenMP / HLSL / C++ 反射 等不同语法,
递归下降地构造 **AST 骨架节点** (Decl / Stmt / Expr / TypeLoc)。
Parser **不** 做语义分析 (那由 [`clang/lib/Sema/`](../Sema/) 负责),
它只保证语法正确 + 调用 `Sema::ActOnXxx` 让 Sema 做类型检查 / 名字
查找 / 模板实例化等。

它在 Clang 内部的层次:

```
┌─────────────────────────────────────────────────────────┐
│ Token 流 (来自 Preprocessor::Lex)                        │
└─────────────────────────────────────────────────────────┘
                │
                ▼
┌─────────────────────────────────────────────────────────┐
│ clang/lib/Parse  (本目录) ← 你在这里                     │
│   · Parser.cpp: 主类 (ConsumeToken / TryAnnotateName /  │
│     ParseFirstTopLevelDecl / ParseExternalDeclaration / │
│     ParseStatement / ParseExpression 顶层分派)          │
│   · ParseAST.cpp: 顶层 ParseAST(Sema&) 入口             │
│   · ParseDecl.cpp + ParseDeclCXX.cpp +                 │
│     ParseTemplate.cpp: 声明解析                         │
│   · ParseExpr.cpp + ParseExprCXX.cpp +                 │
│     ParseInit.cpp: 表达式 / initializer 解析            │
│   · ParseStmt.cpp + ParseStmtAsm.cpp +                  │
│     ParseCXXInlineMethods.cpp: 语句 / 内联 asm /        │
│     延迟类成员解析                                       │
│   · ParseTentative.cpp: 试探性 lookahead 消歧           │
│   · ParsePragma.cpp: 全部 #pragma handler 实例化 +      │
│     调用 (pack / weak / visibility / FP / GCC poison)   │
│   · ParseObjc.cpp: ObjC 声明 / 语句 (@interface /       │
│     @property / @synchronized)                          │
│   · ParseOpenACC.cpp + ParseOpenMP.cpp: #pragma acc /  │
│     omp 指令 + 子句解析                                  │
│   · ParseHLSL.cpp + ParseHLSLRootSignature.cpp: HLSL    │
│     专用语法 + Root Signature 子解析器                  │
│   · ParseReflect.cpp: C++26 静态反射 (^^) 表达式解析    │
└─────────────────────────────────────────────────────────┘
                │
                ▼
┌─────────────────────────────────────────────────────────┐
│ Parser 调用下游:                                         │
│   · Sema::ActOnXxx → 语义分析 → AST 完善                │
│   · AST::Decl / AST::Stmt / AST::Expr 节点创建            │
│   · 在解析失败时 → Sema::Diag → DiagnosticEngine         │
│   · 解析 inline method 时 → LexedMethod 暂存 token,     │
│     类完成后 → ParseCXXInlineMethods.cpp 重新解析        │
└─────────────────────────────────────────────────────────┘
```

### §0.1 公开接口 (`clang/include/clang/Parse/`)

本目录 `.cpp` 文件依赖的 **公开头** 在
[`clang/include/clang/Parse/`](../../include/clang/Parse/), 最关键的:

- [`Parser.h`](../../include/clang/Parse/Parser.h) — `class Parser`
  主体, 包含 `ParsingDeclarator` / `ParsingDeclSpec` /
  `BalancedDelimiterTracker` / `LateParsedDeclaration` /
  `ParsingClassDefinition` / `TentativeCXXTypeIdContext` / `TPResult`。
- [`ParseAST.h`](../../include/clang/Parse/ParseAST.h) — 顶层
  `clang::ParseAST(Sema&, ...)` 自由函数。
- [`RAIIObjectsForParser.h`](../../include/clang/Parse/RAIIObjectsForParser.h)
  — RAII 助手: `SuppressAccessChecks` /
  `ParsingDeclRAIIObject` / `BalancedDelimiterTracker` /
  `ColonProtectionRAIIObject`。
- [`LoopHint.h`](../../include/clang/Parse/LoopHint.h) —
  `LoopHint` 结构: `#pragma clang loop` / `#pragma unroll` 的
  name + option + state + value 表达式。
- [`ParseHLSLRootSignature.h`](../../include/clang/Parse/ParseHLSLRootSignature.h)
  — `hlsl::RootSignatureParser` 子解析器接口。
- [`CMakeLists.txt`](../../include/clang/Parse/CMakeLists.txt) —
  AttrParserStringSwitches 生成的 string-switch 表。

### §0.2 与 [`clang/lib/Lex/`](../Lex/) 的关系

Parser 通过持有 `Preprocessor &PP` 取 token: `ConsumeToken` /
`ConsumeAnyToken` / `LookAhead` / `TryAnnotateName` /
`EnterTokenStream` (用于 `#pragma _Pragma` 注入的 token)。
Parser 也通过 `Preprocessor::EnableBacktrackAtThisPos` / `Backtrack`
/ `CommitBacktrackedTokens` 做试探性 lookahead (见
[`ParseTentative.cpp`](ParseTentative.cpp))。`Lexer::Lex` 不直接
被 Parser 调用, 全经 `Preprocessor`。

### §0.3 与 [`clang/lib/Sema/`](../Sema/) 的关系

Parser 的每个 `Parse*` 函数 **不直接构造最终 AST**, 而是调
`Sema::ActOnXxx` 接口让 Sema 完成:
- 类型检查 (`Sema::ActOnTypeName`)
- 名字查找 (`Sema::ActOnIdentifier`)
- 模板实例化 (`Sema::ActOnTemplateArgument`)
- 隐式转换 (`Sema::ActOnCondition`)
- 范围构造 (`Sema::ActOnDeclStmt`)
等。Parser 与 Sema **紧密耦合**: Parser 是语法骨架, Sema 是语义
填充。

### §0.4 与 [`clang/lib/AST/`](../AST/) 的关系

Parser 通过 Sema 间接构造 [`AST/Decl.h`](../AST/Decl.h) /
[`AST/Stmt.h`](../AST/Stmt.h) / [`AST/Expr.h`](../AST/Expr.h) 中
的 AST 节点; 部分 helper (e.g. `ParsedingDeclarator`) 内部直接
持有 `TypeLoc` / `SourceLocation` 信息, 等 Sema 拿到
`ParsedAttributesView` 后再构造最终 `TypeLoc`。
`ParseCXXInlineMethods.cpp` 的 `LexedMethod` 类把未解析的 token
存为 `LateParsedDeclaration`, 经
[`Serialization/ASTReader.cpp`](../Serialization/ASTReader.cpp) 跨
PCH 序列化。

### §0.5 与 [`clang/lib/Frontend/`](../Frontend/) 的关系

- [`Frontend/CompilerInstance.cpp`](../Frontend/CompilerInstance.cpp)
  在 `FrontendAction::Execute` 中调
  [`ParseAST.cpp`](ParseAST.cpp) 的 `clang::ParseAST(Sema&)`
  跑整个 TU。
- [`Frontend/ASTUnit.cpp`](../Frontend/ASTUnit.cpp) 也调
  `ParseAST` 跑 `ASTUnit::LoadFromCompilerInvocation`。
- [`Frontend/Rewrite/InclusionRewriter.cpp`](../Frontend/Rewrite/InclusionRewriter.cpp)
  / `FixItRewriter.cpp` 不经过 Parser, 但 Parser 的产物 (Token
  流 / source ranges) 是它们的输入。

### §0.6 与 [`clang/lib/Basic/`](../Basic/) 的关系

- [`SourceManager`](../Basic/SourceManager.h) / `LangOptions` /
  `IdentifierTable` / `TokenKinds` / `Attr.td` 等都由 Basic 提供。
- `TokenKinds` (tok::identifier, tok::l_paren 等) 决定哪条语法
  分支被激活。
- `LangOptions` 决定 `bool CPlusPlus` / `bool ObjC` / `bool
  HLSL` / `bool OpenMP` / `bool OpenACC` 等, Parser 据此启用
  相应的语法。

---

## §1. 编译流水线概览

```
                       Token 流 (来自 Preprocessor::Lex)
                                  │
                                  ▼
┌─────────────────────────────────────────────────────────┐
│ clang::ParseAST (ParseAST.cpp)                           │
│   · 构造 Parser (持有 PP + Sema + LangOptions)         │
│   · PrettyStackTrace + CrashRecoveryContextCleanup     │
│   · Parser::Initialize (Parser.cpp)                     │
│        ├─ initializePragmaHandlers (ParsePragma.cpp)   │
│        └─ 准备 Ident_* / keyword 查找                  │
└─────────────────────────────────────────────────────────┘
                │
                ▼
                ┌──────────────────────────────────┐
                │ 循环: ParseFirstTopLevelDecl     │
                │ → ParseTopLevelDecl             │
                │ → ParseExternalDeclaration      │
                └──────────────────────────────────┘
                          │
                          ▼
┌─────────────────────────────────────────────────────────┐
│ Parser::ParseExternalDeclaration (Parser.cpp)            │
│   · 试探: isCXXSimpleDeclaration / isCXXTypeId /       │
│     isCXXDeclarationStatement (ParseTentative.cpp)     │
│   · 分派:                                            │
│      ├─ function/scope declaration → ParseDeclaration │
│      │    (ParseDecl.cpp)                              │
│      │    ├─ 声明说明符 → ParseDeclarationSpecifiers   │
│      │    ├─ 声明符 → ParseDeclarator                  │
│      │    │    (declarator 内含 pointer/array/        │
│      │    │    function/parenthesised 类型)            │
│      │    ├─ template → ParseTemplateParameters       │
│      │    │    (ParseTemplate.cpp)                     │
│      │    ├─ C++ class → ParseClassSpecifier           │
│      │    │    (ParseDeclCXX.cpp)                      │
│      │    │    └─ ParseCXXMemberSpecification          │
│      │    │         └─ 类成员函数体 → LexedMethod      │
│      │    │              (ParseCXXInlineMethods.cpp)   │
│      │    ├─ namespace/using → ParseNamespace          │
│      │    ├─ enum → ParseEnumSpecifier                 │
│      │    └─ attr → ParseAttributes                   │
│      ├─ 表达式语句 → ParseStatementOrDeclaration       │
│      │    (ParseStmt.cpp)                              │
│      │    ├─ ParseIfStatement / ParseForStatement /   │
│      │    │   ParseSwitchStatement / ParseWhile /      │
│      │    │   ParseDoStatement / ParseCaseStatement /  │
│      │    │   ParseLabelStatement / ParseGotoStatement │
│      │    ├─ ParseCompoundStatement ({ })              │
│      │    ├─ ParseAsmStatement → ParseGCCAsm / MSAsm   │
│      │    │    (ParseStmtAsm.cpp)                      │
│      │    ├─ ParseReturnStatement / ParseDeclStmt      │
│      │    └─ ObjC: ParseObjCAtStatement                │
│      │         (ParseObjc.cpp)                        │
│      ├─ pragma → ParsePragma.cpp / PragmaHandler       │
│      │    ├─ #pragma omp → ParseOpenMPDirective        │
│      │    │    (ParseOpenMP.cpp)                       │
│      │    ├─ #pragma acc → ParseOpenACCDirective       │
│      │    │    (ParseOpenACC.cpp)                      │
│      │    └─ #pragma clang loop → LoopHint             │
│      └─ 顶层 ASM / module declaration 等               │
└─────────────────────────────────────────────────────────┘
                │
                ▼
┌─────────────────────────────────────────────────────────┐
│ Parser 调 Sema::ActOnXxx:                               │
│   · ActOnDeclarator → ActOnFunctionDecl /              │
│     ActOnVarDecl / ActOnFieldDecl                      │
│   · ActOnStmt → ActOnCompoundStmt / ActOnIfStmt /      │
│     ActOnForStmt / ...                                 │
│   · ActOnExpr → ActOnCallExpr / ActOnBinaryOp / ...    │
│   · ActOnTypeName / ActOnClassSpecifier /              │
│     ActOnNamespaceDefinition / ...                     │
│   · Sema 内部做名字查找 / 类型检查 / 重载 / 模板,        │
│     把完整 AST 节点挂到 ASTContext                      │
└─────────────────────────────────────────────────────────┘
                │
                ▼
┌─────────────────────────────────────────────────────────┐
│ 产物: 完整 AST (Sema 完成后) + Token 用尽 → ParseAST 退出 │
└─────────────────────────────────────────────────────────┘

旁路:
  · ParseCXXInlineMethods.cpp: 延迟解析
  · ParseTentative.cpp: TPResult 试探 (lookahead 1-2 token)
  · ParseReflect.cpp: C++26 反射操作数解析
  · ParseHLSLRootSignature.cpp: __declspec(RootSignature(...))
```

---

## §2. 文件目录结构

```
clang/lib/Parse/  (20 文件, 0 子目录)
├── §3.1  Core Parser (入口 + 主类)                    (2 文件)
├── §3.2  Declarations (C/C++/template)               (3 文件)
├── §3.3  Expressions (C/C++/initializer)             (3 文件)
├── §3.4  Statements (普通 + asm + inline-method)     (3 文件)
├── §3.5  C++ extensions (inline method + 反射)        (2 文件)
├── §3.6  ObjC 语法                                    (1 文件)
├── §3.7  OpenACC / OpenMP                             (2 文件)
├── §3.8  HLSL                                         (2 文件)
├── §3.9  Pragma handler                               (1 文件)
└── §3.10 Tentative parsing (lookahead 消歧)           (1 文件 + CMakeLists)
```

### 2.1 文件数量统计

| 区域 | .cpp | .h | 合计 |
|------|------|-----|------|
| Core Parser | 2 | 0 | 2 |
| Declarations | 3 | 0 | 3 |
| Expressions | 3 | 0 | 3 |
| Statements | 3 | 0 | 3 |
| C++ extensions | 2 | 0 | 2 |
| ObjC | 1 | 0 | 1 |
| OpenACC / OpenMP | 2 | 0 | 2 |
| HLSL | 2 | 0 | 2 |
| Pragma handler | 1 | 0 | 1 |
| Tentative parsing | 1 | 0 | 1 |
| Build | 1 | 0 | 1 (`CMakeLists.txt`) |
| **总计** | **~19** | **0** | **~19** + `CMakeLists.txt` |

---

## §3. 文件详解

### 3.1 Core Parser (入口 + 主类)

[`Parser.cpp`](Parser.cpp) — 主 `Parser` 类: `ConsumeToken` /
`ConsumeAnyToken` / `LookAhead` / `TryAnnotateName` 等 token 操作;
`ParseFirstTopLevelDecl` / `ParseTopLevelDecl` /
`ParseExternalDeclaration` / `ParseStatement` / `ParseExpression`
顶层分派; scope 管理 + `BalancedDelimiterTracker` 实现。
- 上游: [`ParseAST.cpp`](ParseAST.cpp) 构造; Frontend 的
  `CompilerInstance` / `ASTUnit`。
- 下游: [`ParseDecl.cpp`](ParseDecl.cpp),
  [`ParseDeclCXX.cpp`](ParseDeclCXX.cpp),
  [`ParseExpr.cpp`](ParseExpr.cpp),
  [`ParseStmt.cpp`](ParseStmt.cpp),
  [`Lex/Preprocessor.h`](../include/clang/Lex/Preprocessor.h),
  [`Sema/Sema.h`](../include/clang/Sema/Sema.h)。
- 关键类/函数: `Parser::Parser`, `Parser::Initialize`,
  `ParseFirstTopLevelDecl`, `ParseTopLevelDecl`,
  `ParseExternalDeclaration`,
  `BalancedDelimiterTracker::expectAndConsume`, `ParseScope`,
  `ParseScopeFlags`。

[`ParseAST.cpp`](ParseAST.cpp) — 顶层 `clang::ParseAST(Sema&)`
入口: 跑 Parser 走完整个 TU, 配
`PrettyStackTraceParserEntry` + `ResetStackCleanup` +
`CrashRecoveryContextCleanupRegistrar` 异常安全包装。
- 上游:
  [`Frontend/CompilerInstance.cpp`](../Frontend/CompilerInstance.cpp)
  (调 `clang::ParseAST`)。
- 下游:
  [`clang/Parse/Parser.h`](../include/clang/Parse/Parser.h)
  (`Parser`),
  [`clang/Sema/Sema.h`](../include/clang/Sema/Sema.h),
  [`clang/AST/ASTConsumer.h`](../include/clang/AST/ASTConsumer.h)。
- 关键类/函数: `clang::ParseAST(Sema&)`,
  `PrettyStackTraceParserEntry`, `ResetStackCleanup`,
  `CrashRecoveryContextCleanupRegistrar`。

### 3.2 Declarations

[`ParseDecl.cpp`](ParseDecl.cpp) — C/C++ 声明解析:
`ParseDeclaration` / `ParseDeclarator` /
`ParseDeclarationSpecifiers` / `ParseStructDeclaration` /
`ParseEnumSpecifier` / `ParseTypeName` / `ParseAttributes` /
`ParseAsmLabel`。
- 上游: [`Parser.cpp`](Parser.cpp) (ParseExternalDeclaration,
  ParseDeclOrFunctionDefInternal)。
- 下游: [`Sema/Sema.h`](../include/clang/Sema/Sema.h),
  [`AST/Decl.h`](../include/clang/AST/Decl.h),
  [`Basic/DeclSpec.h`](../include/clang/Basic/DeclSpec.h),
  [`ParseDeclCXX.cpp`](ParseDeclCXX.cpp),
  [`ParseTemplate.cpp`](ParseTemplate.cpp)。
- 关键类/函数: `Parser::ParseDeclaration`,
  `Parser::ParseDeclarator`,
  `Parser::ParseDeclarationSpecifiers`,
  `Parser::ParseStructDeclaration`, `Parser::ParseTypeName`,
  `Parser::ParseAttributes`。

[`ParseDeclCXX.cpp`](ParseDeclCXX.cpp) — C++ 声明:
`ParseClassSpecifier` / `ParseCXXMemberSpecification` /
`ParseNamespace` / `ParseUsingDeclaration` /
`ParseConstructorInitializer` / `ParseCXX11Attributes` /
`ParseBaseClause` / exception specifications。
- 上游: [`Parser.cpp`](Parser.cpp) (ParseExternalDeclaration),
  [`ParseDecl.cpp`](ParseDecl.cpp) (declarators)。
- 下游: [`Sema/Sema.h`](../include/clang/Sema/Sema.h),
  [`AST/DeclCXX.h`](../include/clang/AST/DeclCXX.h),
  [`AST/DeclTemplate.h`](../include/clang/AST/DeclTemplate.h),
  [`ParseTemplate.cpp`](ParseTemplate.cpp)。
- 关键类/函数: `Parser::ParseClassSpecifier`,
  `Parser::ParseCXXMemberSpecification`,
  `Parser::ParseNamespace`,
  `Parser::ParseUsingDeclaration`,
  `Parser::ParseConstructorInitializer`,
  `Parser::ParseCXX11Attributes`, `Parser::ParseBaseClause`。

[`ParseTemplate.cpp`](ParseTemplate.cpp) — 模板参数列表、template-id
注释、显式实例化/特化、concept 定义。`LateTemplateParserCallback`
在 PCH 加载后回调 Sema 完成模板实例化。
- 上游: [`Parser.cpp`](Parser.cpp),
  [`ParseDecl.cpp`](ParseDecl.cpp),
  [`ParseDeclCXX.cpp`](ParseDeclCXX.cpp) (decls starting with
  `template` 或 `<`)。
- 下游: [`Sema/SemaTemplate.h`](../include/clang/Sema/SemaTemplate.h),
  [`AST/DeclTemplate.h`](../include/clang/AST/DeclTemplate.h),
  [`ParseTentative.cpp`](ParseTentative.cpp)。
- 关键类/函数: `Parser::ParseTemplateParameters`,
  `Parser::ParseTemplateParameterList`,
  `Parser::ParseTemplateArgument`,
  `Parser::AnnotateTemplateIdToken`,
  `Parser::ParseConceptDefinition`,
  `Parser::ParseExplicitInstantiation`,
  `Parser::LateTemplateParserCallback`。

### 3.3 Expressions

[`ParseExpr.cpp`](ParseExpr.cpp) — C/C++ 表达式解析:
`ParseExpression` / `ParseAssignmentExpression` /
`ParseConditionalExpression` / `ParseCastExpression` /
`ParseUnaryExpression` / `ParsePostfixExpressionSuffix` /
`ParseRHSOfBinaryExpression` / `ParseExpressionList` / 各种字面
量与 primary expression。
- 上游: [`Parser.cpp`](Parser.cpp),
  [`ParseDecl.cpp`](ParseDecl.cpp),
  [`ParseStmt.cpp`](ParseStmt.cpp),
  [`ParseInit.cpp`](ParseInit.cpp)。
- 下游: [`Sema/Sema.h`](../include/clang/Sema/Sema.h),
  [`AST/Expr.h`](../include/clang/AST/Expr.h),
  [`AST/ExprCXX.h`](../include/clang/AST/ExprCXX.h),
  [`Basic/TokenKinds.h`](../include/clang/Basic/TokenKinds.h)。
- 关键类/函数: `Parser::ParseExpression`,
  `Parser::ParseAssignmentExpression`,
  `Parser::ParseConditionalExpression`,
  `Parser::ParseCastExpression`,
  `Parser::ParseUnaryExpression`,
  `Parser::ParsePostfixExpressionSuffix`,
  `Parser::ParseRHSOfBinaryExpression`。

[`ParseExprCXX.cpp`](ParseExprCXX.cpp) — C++ 表达式: lambda / new
/ delete / type trait / noexcept / fold / requires / CXX-id /
用户自定义字面量 / `ParseOptionalCXXScopeSpecifier`。
- 上游: [`ParseExpr.cpp`](ParseExpr.cpp) (delegated for C++
  constructs),
  [`ParseTentative.cpp`](ParseTentative.cpp)。
- 下游: [`Sema/Sema.h`](../include/clang/Sema/Sema.h),
  [`AST/ExprCXX.h`](../include/clang/AST/ExprCXX.h),
  [`AST/DeclCXX.h`](../include/clang/AST/DeclCXX.h),
  [`ParseTemplate.cpp`](ParseTemplate.cpp)。
- 关键类/函数: `Parser::ParseLambdaExpression`,
  `Parser::ParseCXXNewExpression`,
  `Parser::ParseCXXDeleteExpression`,
  `Parser::ParseTypeTrait`, `Parser::ParseRequiresExpression`,
  `Parser::ParseCXXIdExpression`,
  `Parser::ParseOptionalCXXScopeSpecifier`。

[`ParseInit.cpp`](ParseInit.cpp) — initializer 列表与 designator
解析: C/C++ brace 与 equals initializer, MSVC `if_exists` 风格。
- 上游: [`ParseDecl.cpp`](ParseDecl.cpp) (ParseDeclGroup),
  [`ParseDeclCXX.cpp`](ParseDeclCXX.cpp) (member-init),
  [`ParseExpr.cpp`](ParseExpr.cpp) (function call init)。
- 下游: [`Sema/Sema.h`](../include/clang/Sema/Sema.h),
  [`AST/Expr.h`](../include/clang/AST/Expr.h),
  [`AST/InitListExpr.h`](../include/clang/AST/InitListExpr.h)。
- 关键类/函数: `Parser::ParseInitializer`,
  `Parser::ParseBraceInitializer`,
  `Parser::ParseInitializerWithPotentialDesignator`,
  `Parser::MayBeDesignationStart`,
  `Parser::ParseMicrosoftIfExistsBraceInitializer`。

### 3.4 Statements

[`ParseStmt.cpp`](ParseStmt.cpp) — 语句解析 dispatch + 具体语句:
if / while / for / switch / do / case / labeled / compound /
return / goto / break / continue / try / declaration-as-statement。
- 上游: [`Parser.cpp`](Parser.cpp) (ParseStatement entry);
  [`ParseDecl.cpp`](ParseDecl.cpp) (declarations-as-statements)。
- 下游: [`ParseExpr.cpp`](ParseExpr.cpp) (expressions-as-statements),
  [`ParseStmtAsm.cpp`](ParseStmtAsm.cpp),
  [`ParseObjc.cpp`](ParseObjc.cpp),
  [`Sema/Sema.h`](../include/clang/Sema/Sema.h),
  [`AST/Stmt.h`](../include/clang/AST/Stmt.h)。
- 关键类/函数: `Parser::ParseStatement`,
  `Parser::ParseStatementOrDeclaration`,
  `Parser::ParseCompoundStatement`, `Parser::ParseIfStatement`,
  `Parser::ParseForStatement`, `Parser::ParseSwitchStatement`,
  `Parser::ParseCaseStatement`, `Parser::ParseLabelStatement`。

[`ParseStmtAsm.cpp`](ParseStmtAsm.cpp) — GCC / MSVC 内联汇编解析
(`asm` / `__asm` 块, 限定符, 操作数); 通过
`ClangAsmParserCallback` 把 input operand / output operand 调
LLVM 的 `MCParser` 解析约束与表达式。
- 上游: [`ParseStmt.cpp`](ParseStmt.cpp)
  (`ParseAsmStatement` dispatch)。
- 下游: MCParser (`ClangAsmParserCallback`),
  [`Sema/Sema.h`](../include/clang/Sema/Sema.h),
  [`AST/StmtAsm.h`](../include/clang/AST/StmtAsm.h),
  [`Lex/Preprocessor.h`](../include/clang/Lex/Preprocessor.h)。
- 关键类/函数: `ClangAsmParserCallback`,
  `Parser::ParseAsmStatement`,
  `Parser::ParseGCCAsmStatement`,
  `Parser::ParseMSAsmStatement`,
  `Parser::parseGNUAsmQualifierListOpt`,
  `Parser::GNUAsmQualifiers`。

[`ParseCXXInlineMethods.cpp`](ParseCXXInlineMethods.cpp) — 延迟
解析 C++ 类内成员函数体、成员初始化器、延迟解析的属性与
`#pragma`; `LexedMethod` / `LateParsedDeclaration` /
`LateParsedMemberInitializer` 缓冲 token, 类完成后重新解析。
- 上游: [`ParseDeclCXX.cpp`](ParseDeclCXX.cpp) (stores
  `LexedMethod` into `ParsingClass`),
  [`ParsePragma.cpp`](ParsePragma.cpp)。
- 下游: [`ParseDecl.cpp`](ParseDecl.cpp) (ParseDeclarator),
  [`ParseExpr.cpp`](ParseExpr.cpp) (ParseAssignmentExpression),
  [`Sema/Sema.h`](../include/clang/Sema/Sema.h),
  [`AST/DeclCXX.h`](../include/clang/AST/DeclCXX.h)。
- 关键类/函数: `Parser::ParseCXXInlineMethodDef`,
  `Parser::ParseLexedMethodDef`,
  `Parser::ParseLexedMemberInitializer`,
  `Parser::LateParsedClass`, `Parser::LexedMethod`,
  `LateParsedDeclaration`, `LateParsedMemberInitializer`,
  `Parser::ConsumeAndStoreUntil`。

### 3.5 C++ extensions (inline method + 反射)

[`ParseReflect.cpp`](ParseReflect.cpp) — C++26 静态反射表达式解析
(`^^` 反射操作符, 只在 unevaluated context 内合法)。
- 上游: [`ParseExpr.cpp`](ParseExpr.cpp) (`ParsePrimaryExpression`
  delegated when `Tok` 是 `^^`)。
- 下游: [`Sema/Sema.h`](../include/clang/Sema/Sema.h)
  (`ActOnCXXReflectExpr`),
  [`AST/LocInfoType.h`](../include/clang/AST/LocInfoType.h),
  [`Basic/DiagnosticParse.h`](../include/clang/Basic/DiagnosticParse.h)。
- 关键类/函数: `Parser::ParseCXXReflectExpression`,
  `EnterExpressionEvaluationContext`。

### 3.6 ObjC 语法

[`ParseObjc.cpp`](ParseObjc.cpp) — Objective-C 声明和语句:
`@interface` / `@implementation` / `@property` / `@synchronized` /
`@try` / `@autoreleasepool` / ObjC message expression
(`[receiver msg]`)。
- 上游: [`Parser.cpp`](Parser.cpp) (`ParseExternalDeclaration`,
  `ParseStatementOrDeclaration`)。
- 下游: [`Sema/SemaObjC.h`](../include/clang/Sema/SemaObjC.h),
  [`AST/DeclObjC.h`](../include/clang/AST/DeclObjC.h),
  [`ParseDecl.cpp`](ParseDecl.cpp),
  [`ParseStmt.cpp`](ParseStmt.cpp)。
- 关键类/函数: `Parser::ParseObjCAtDirectives`,
  `Parser::ParseObjCAtInterfaceDeclaration`,
  `Parser::ParseObjCAtImplementationDeclaration`,
  `Parser::ParseObjCMethodDecl`,
  `Parser::ParseObjCPropertyAttribute`,
  `Parser::ParseObjCMessageExpression`,
  `Parser::ParseObjCAtStatement`。

### 3.7 OpenACC / OpenMP

[`ParseOpenACC.cpp`](ParseOpenACC.cpp) — `#pragma acc` 指令与子句
解析: `enter` / `exit` / `data` / `parallel` / `kernels` / `loop`
及其子句列表 (`copy` / `copyin` / `copyout` / `create` / `device`
...)。
- 上游: [`ParsePragma.cpp`](ParsePragma.cpp) (dispatch for
  `#pragma acc`)。
- 下游: [`Sema/SemaOpenACC.h`](../include/clang/Sema/SemaOpenACC.h),
  [`Basic/OpenACCKinds.h`](../include/clang/Basic/OpenACCKinds.h),
  [`Lex/Preprocessor.h`](../include/clang/Lex/Preprocessor.h)。
- 关键类/函数: `Parser::ParseOpenACCDirectiveKind`,
  `Parser::ParseOpenACCClauseList`, `Parser::ParseOpenACCClause`,
  `Parser::ParseOpenACCIntExpr`,
  `Parser::ParseOpenACCSizeExpr`,
  `OpenACCSpecialTokenKind`, `OpenACCDirectiveKind`。

[`ParseOpenMP.cpp`](ParseOpenMP.cpp) — `#pragma omp` 指令与子句
解析: `parallel` / `for` / `simd` / `atomic` / `declare-reduction`
/ `declare-mapper` / traits (`OpenMPTraitInfo` /
`OpenMPTraitSelector`)。
- 上游: [`ParsePragma.cpp`](ParsePragma.cpp) (dispatch for
  `#pragma omp`)。
- 下游: [`Sema/SemaOpenMP.h`](../include/clang/Sema/SemaOpenMP.h),
  [`Basic/OpenMPKinds.h`](../include/clang/Basic/OpenMPKinds.h),
  [`Lex/Preprocessor.h`](../include/clang/Lex/Preprocessor.h)。
- 关键类/函数: `Parser::ParseOpenMPDirectiveKind`,
  `Parser::ParseOpenMPClause`,
  `Parser::ParseOpenMPDeclareReductionDirective`,
  `Parser::ParseOpenMPDeclareMapperDirective`,
  `Parser::ParseOMPDeclareSimdClauses`,
  `OMPTraitInfo`, `OMPTraitSelector`。

### 3.8 HLSL

[`ParseHLSL.cpp`](ParseHLSL.cpp) — HLSL 专用语法: `cbuffer` /
`tbuffer` / 语义 (`SV_Position` 等) / HLSL 限定符与属性。
- 上游: [`ParseDecl.cpp`](ParseDecl.cpp),
  [`ParseDeclCXX.cpp`](ParseDeclCXX.cpp) (attribute / qualifier
  hooks)。
- 下游: [`Sema/SemaHLSL.h`](../include/clang/Sema/SemaHLSL.h),
  `FrontendHLSL`,
  [`Basic/DiagnosticParse.h`](../include/clang/Basic/DiagnosticParse.h)。
- 关键类/函数: `Parser::ParseHLSLBuffer`,
  `Parser::ParseHLSLAnnotations`,
  `Parser::ParseHLSLSemantic`,
  `Parser::MaybeParseHLSLAnnotations`,
  `Parser::ParseHLSLQualifiers`。

[`ParseHLSLRootSignature.cpp`](ParseHLSLRootSignature.cpp) — HLSL
Root Signature 子解析器: token-driven parser for
`RootFlags` / `RootConstants` / `DescriptorTable` /
`StaticSampler`。
- 上游: [`ParsePragma.cpp`](ParsePragma.cpp) (handles `__declspec
  RootSignature`) /
  [`ParseDeclCXX.cpp`](ParseDeclCXX.cpp) (attribute arg parsing)。
- 下游: [`Lex/LexHLSLRootSignature.h`](../include/clang/Lex/LexHLSLRootSignature.h),
  `llvm/Frontend/HLSL/HLSLRootSignature.h`,
  [`Sema/SemaHLSL.h`](../include/clang/Sema/SemaHLSL.h)。
- 关键类/函数: `RootSignatureParser::parse`,
  `RootSignatureParser::parseRootFlags`,
  `RootSignatureParser::parseRootConstants`,
  `RootSignatureParser::parseDescriptorTable`,
  `RootSignatureToken`, `RootSignatureElement`。

### 3.9 Pragma handler

[`ParsePragma.cpp`](ParsePragma.cpp) — Pragma handler 调用 + 内建
pragma 实现: `pack` / `weak` / `visibility` / `float_control` /
`FP_contract` / GCC poison / MSVC `comment` /
`include_alias`。
- 上游: [`Parser.cpp`](Parser.cpp) (`initializePragmaHandlers`),
  [`Lex/Preprocessor.cpp`](../Lex/Preprocessor.cpp)
  (`#pragma` 触发)。
- 下游: [`Lex/Pragma.h`](../include/clang/Lex/Pragma.h)
  (`PragmaHandler`),
  [`Sema/Sema.h`](../include/clang/Sema/Sema.h),
  [`Frontend/CompilerInstance.cpp`](../Frontend/CompilerInstance.cpp),
  [`Basic/LangOptions.h`](../include/clang/Basic/LangOptions.h)。
- 关键类/函数: `Parser::initializePragmaHandlers`,
  `Parser::resetPragmaHandlers`,
  `Parser::HandlePragmaPack`,
  `Parser::HandlePragmaWeak`,
  `Parser::HandlePragmaVisibility`,
  `Parser::HandlePragmaFloatControl`,
  `Parser::HandlePragmaFPContract`。

### 3.10 Tentative parsing

[`ParseTentative.cpp`](ParseTentative.cpp) — 试探性 lookahead
解析: 消歧 "语句 vs 声明"、"表达式 vs 类型"、"init-vs-condition";
返回 `TPResult` (`True` / `False` / `Ambiguous` / `Error`)。
- 上游: [`ParseStmt.cpp`](ParseStmt.cpp),
  [`ParseExpr.cpp`](ParseExpr.cpp),
  [`ParseExprCXX.cpp`](ParseExprCXX.cpp),
  [`ParseDecl.cpp`](ParseDecl.cpp),
  [`ParseDeclCXX.cpp`](ParseDeclCXX.cpp)。
- 下游: [`Lexer/Preprocessor.h`](../include/clang/Lex/Preprocessor.h)
  (`ConsumeToken` / `EnterToken`),
  [`Sema/Sema.h`](../include/clang/Sema/Sema.h) (attribute
  lookups)。
- 关键类/函数: `TPResult`,
  `Parser::isCXXDeclarationStatement`,
  `Parser::isCXXSimpleDeclaration`,
  `Parser::TryParseSimpleDeclaration`,
  `Parser::TryParseDeclarator`,
  `Parser::TryParseInitDeclaratorList`, `Parser::isCXXTypeId`,
  `Parser::isStartOfTemplateTypeParameter`,
  `TentativeCXXTypeIdContext`。

[`CMakeLists.txt`](CMakeLists.txt) — `clangParse` 库定义: 列出 19
个 `.cpp` 源, 链接 `clangAST clangBasic clangLex clangSema` +
LLVM `MCParser` / `FrontendHLSL` / `FrontendOpenMP` / `MC` /
`MCParser` 等。

---

## §4. 关键调用链

### 4.1 顶层 TU 解析链

```
clang::ParseAST (ParseAST.cpp)
  ├─ PrettyStackTraceParserEntry (debug stack)
  ├─ CrashRecoveryContextCleanupRegistrar (cleanup on crash)
  ├─ new Parser (持有 PP + Sema)
  │    └─ Parser::Initialize (Parser.cpp)
  │         └─ initializePragmaHandlers (ParsePragma.cpp)
  │              ├─ 注册 #pragma pack/weak/visibility/fp_contract/MSVC
  │              └─ 注册 vendor-specific pragmas
  │
  └─ 循环 ParseFirstTopLevelDecl → ParseTopLevelDecl → EOF
       └─ Parser::ParseExternalDeclaration (Parser.cpp)
            ├─ isCXXDeclarationStatement / isCXXSimpleDeclaration
            │    (ParseTentative.cpp: TPResult 消歧)
            ├─ 分派:
            │    ├─ ParseDeclaration (ParseDecl.cpp)
            │    │    ├─ ParseDeclarationSpecifiers (type/storage)
            │    │    ├─ ParseDeclarator (name + suffix)
            │    │    │    └─ ParsePointerDeclarator /
            │    │    │       ParseArrayDeclarator /
            │    │    │       ParseFunctionDeclarator /
            │    │    │       ParseParenDeclarator
            │    │    └─ ParseAttributes + asm-label 等
            │    ├─ ParseTemplateParameters / ParseConceptDefinition
            │    │    (ParseTemplate.cpp)
            │    ├─ ParseClassSpecifier (ParseDeclCXX.cpp)
            │    │    └─ ParseCXXMemberSpecification
            │    │         ├─ ParseCXXMemberDeclarator
            │    │         ├─ 成员函数体 → LexedMethod 暂存
            │    │         │    (ParseCXXInlineMethods.cpp)
            │    │         ├─ LateParsedAttribute / LateParsedPragma
            │    │         └─ Nested class 等
            │    ├─ ParseEnumSpecifier (ParseDecl.cpp)
            │    ├─ ParseNamespace / ParseUsingDeclaration /
            │    │    ParseBaseClause (ParseDeclCXX.cpp)
            │    ├─ ParseAsmLabel (ParseDecl.cpp)
            │    ├─ ParseStatementOrDeclaration (ParseStmt.cpp)
            │    │    ├─ ParseStatement → if/while/for/switch/...
            │    │    ├─ ParseDeclarationAsStatement
            │    │    ├─ ParseAsmStatement (ParseStmtAsm.cpp)
            │    │    └─ ParseObjCAtStatement (ParseObjc.cpp)
            │    ├─ Pragma → ParsePragma.cpp / PragmaHandler
            │    │    ├─ #pragma omp → ParseOpenMP.cpp
            │    │    ├─ #pragma acc → ParseOpenACC.cpp
            │    │    └─ #pragma clang loop → LoopHint
            │    └─ @interface / @implementation (ParseObjc.cpp)
            │
            └─ 调 Sema::ActOnXxx 完成 AST 节点构造:
                 ├─ ActOnFunctionDecl / ActOnVarDecl / ActOnFieldDecl
                 ├─ ActOnClassSpecifier / ActOnNamespaceDefinition
                 ├─ ActOnStmt → CompoundStmt/IfStmt/ForStmt/...
                 ├─ ActOnExpr → BinaryOperator/CallExpr/CXXNewExpr...
                 ├─ ActOnDeclarator → 把 ParsingDeclarator 转 Decl
                 └─ ActOnTemplateArgument / ActOnTemplateParameter

最终 Sema 把完整 AST 节点挂到 ASTContext。
```

### 4.2 表达式解析链

```
Parser::ParseExpression (ParseExpr.cpp)
  └─ ParseAssignmentExpression
       └─ ParseConditionalExpression
            └─ ParseCastExpression
                 └─ ParseUnaryExpression
                      └─ ParsePostfixExpressionSuffix
                           ├─ ParseExpressionList (function call)
                           ├─ ParseCXXIdExpression
                           │    (ParseExprCXX.cpp:
                           │     ParseOptionalCXXScopeSpecifier)
                           ├─ [expr] ObjC message
                           │    (ParseObjc.cpp)
                           ├─ Lambda 表达式 (ParseExprCXX.cpp)
                           ├─ ParseCXXNewExpression /
                           │    ParseCXXDeleteExpression
                           │    (ParseExprCXX.cpp)
                           └─ ^^(...) 反射 (ParseReflect.cpp)

ParseRHSOfBinaryExpression (ParseExpr.cpp)
  ├─ 优先级表驱动 binary operator parsing
  ├─ C++ user-defined operator name (ParseExprCXX.cpp)
  └─ 三元 / throw / fold 表达式

Initializer:
  ParseInitializer (ParseInit.cpp)
    ├─ ParseBraceInitializer (C99/C++11 {a, b, c})
    ├─ ParseInitializerWithPotentialDesignator (.x = 1)
    │    └─ ParseDesignation (.field-name : value)
    └─ ParseAssignmentExpression (singleton init)

Literal handling:
  ParseNumericLiteral / ParseStringLiteral / ParseCharLiteral
  → NumericLiteralParser / StringLiteralParser /
    CharLiteralParser (Lex/LiteralSupport.h)
```

### 4.3 模板解析链

```
Parser 看到 'template' 关键字或 '<' 触发 template-id 试探
  │
  ▼
ParseTemplate.cpp
  ├─ ParseTemplateParameters (template <...>)
  │    ├─ ParseTypeParameter (typename T / class T / T ...)
  │    ├─ ParseTemplateTypeParameter (T)
  │    ├─ ParseNonTypeTemplateParameter (int N)
  │    └─ ParseTemplateTemplateParameter (template<class> class TT)
  │
  ├─ ParseTemplateArgument (Foo<int, 0>)
  │    ├─ ParseTemplateArgumentType
  │    ├─ ParseTemplateArgumentExpression
  │    │    └─ Sema::ActOnTemplateArgument
  │    └─ 处理 default argument
  │
  ├─ AnnotateTemplateIdToken (template-id 在 expression 内)
  │    └─ 把 'Foo<int>' 作为一个 annotation token
  │
  ├─ ParseConceptDefinition (concept C = requires(T x){ x.foo(); };)
  │    └─ ParseRequiresExpression (ParseExprCXX.cpp)
  │
  └─ ParseExplicitInstantiation / ParseExplicitSpecialization
       └─ Sema::ActOnExplicitInstantiation / ActOnExplicitSpecialization

LateTemplateParserCallback:
  └─ PCH/Module 加载后, 调 Sema 二次完成模板实例化
```

### 4.4 Inline method 延迟解析链

```
ParseCXXMemberSpecification (ParseDeclCXX.cpp)
  │
  ▼
发现类内成员函数体:
  │
  ▼
Parser::ConsumeAndStoreUntil (Parser.cpp)
  └─ 把函数体的 token 流存入 LexedMethod
       └─ LateParsedDeclaration 子类
            (ParseCXXInlineMethods.cpp)
  │
  ▼
类完成后, Parser::ParseLexedMethodDef
  └─ EnterTokenStream 把存的 token 重新喂给 Parser
       └─ 重新跑 ParseDeclaration / ParseStatement / ParseExpression
            └─ Sema::ActOnXxx 完成 inline method AST 节点

类似机制也用于:
  · LateParsedMemberInitializer: 成员初始化器
  · LateParsedAttribute: C++11 attributes (final / noreturn / ...)
  · LateParsedPragma: #pragma clang loop 等
```

### 4.5 OpenMP / OpenACC 解析链

```
Preprocessor 看到 #pragma omp / acc
  │
  ▼
Preprocessor::HandlePragma (Lex/Preprocessor.cpp)
  └─ PragmaNamespace::HandlePragma (Lex/Pragma.cpp)
       └─ 找到 ParseOpenMPPragmaHandler / ParseOpenACCPragmaHandler
            (由 ParsePragma.cpp 注册)
             │
             ▼
ParseOpenMP.cpp:
  Parser::ParseOpenMPDirective
    ├─ ParseOpenMPDirectiveKind (parallel / for / simd / ...)
    ├─ ParseOpenMPClauseList
    │    ├─ ParseOpenMPClause (default / shared / private / ...)
    │    │    ├─ ParseExpression → Parser::ParseAssignmentExpression
    │    │    └─ Sema::ActOnOpenMPClause
    │    └─ ParseOMPDeclareSimdClauses
    ├─ ParseOpenMPDeclareReductionDirective
    ├─ ParseOpenMPDeclareMapperDirective
    └─ OMPTraitInfo (trait set / selector)

ParseOpenACC.cpp:
  Parser::ParseOpenACCDirective
    ├─ ParseOpenACCDirectiveKind (parallel / kernels / data / ...)
    ├─ ParseOpenACCClauseList
    │    └─ ParseOpenACCClause (copy / copyin / create / ...)
    │         ├─ ParseOpenACCIntExpr
    │         ├─ ParseOpenACCSizeExpr
    │         └─ Sema::ActOnOpenACCClause
    └─ OpenACCSpecialTokenKind (deviceptr / present_or_copy ...)
```

### 4.6 Tentative parsing 消歧链

```
Parser 看到 ambiguous token (e.g. `T(a);`)
  │
  ▼
ParseTentative.cpp:
  isCXXSimpleDeclaration / isCXXTypeId / isCXXDeclarationStatement
  │
  ├─ 备份 token 位置 (Preprocessor::EnableBacktrackAtThisPos)
  │
  ├─ TryParseSimpleDeclaration / TryParseDeclarator / TryParseInitDeclaratorList
  │    ├─ 尝试作为声明解析
  │    └─ 必要时进 C++ scope 试探 (looking ahead 1-3 token)
  │
  ├─ TryParseAsExpression / TryParseAsType
  │    ├─ 尝试作为表达式 / 类型解析
  │    └─ 必要时调 Sema::isAcceptableStartOfCXXTypeId
  │
  └─ 返回 TPResult:
       ├─ TPResult::True: 明确是 X
       ├─ TPResult::False: 明确不是 X
       ├─ TPResult::Ambiguous: 模糊, 需根据上下文定
       └─ TPResult::Error: 解析失败

Parser 据此分派:
  ├─ T(a); 是 variable declaration 还是 expression statement?
  ├─ A<B>(c) 是 template-id 还是 less-than comparison?
  ├─ cast<T>(x) 是函数式 cast 还是 C-style cast?
  ├─ for (T x : ...) 是 range-for 还是 init-statement?
  └─ ... 等等

回溯与提交:
  ├─ Backtrack: 恢复 token 位置
  └─ CommitBacktrackedTokens: 保留 lookahead 结果, 不再 parse
```

### 4.7 ObjC 消息发送 / HLSL 解析链

```
ObjC:
  Parser 看到 [receiver message]
    └─ ParseObjCMessageExpression (ParseObjc.cpp)
         ├─ ParseObjCMessageExprReceiver
         ├─ ParseObjCMessageSelector
         │    ├─ 单 keyword: foo
         │    └─ 多 keyword: foo:bar:baz
         └─ Sema::ActOnObjCMessageExpr
  Parser 看到 @interface Foo : Bar
    └─ ParseObjCAtInterfaceDeclaration (ParseObjc.cpp)
         ├─ ParseObjCInterfaceDeclList (ivars / methods / props)
         ├─ ParseObjCMethodDecl (return type + selector + params)
         └─ ParseObjCPropertyAttribute (copy / strong / atomic / ...)

HLSL:
  Parser 看到 cbuffer / tbuffer
    └─ ParseHLSLBuffer (ParseHLSL.cpp)
         ├─ ParseHLSLQualifiers
         ├─ ParseHLSLAnnotations (semantics: register(b0) / ...)
         ├─ ParseHLSLSemantic (SV_Position 等)
         └─ MaybeParseHLSLAnnotations (other positions)
  Parser 看到 __declspec(RootSignature(...))
    └─ RootSignatureParser::parse (ParseHLSLRootSignature.cpp)
         ├─ parseRootFlags (allow_input_assembler_...)
         ├─ parseRootConstants (bN = { c0, c1, ... })
         ├─ parseDescriptorTable (resource register map)
         └─ parseStaticSampler
              └─ 把 RootSignatureElement 交给 Sema/SemaHLSL
```

### 4.8 Pragma handler 派发链

```
Preprocessor 看到 #pragma
  │
  ▼
Preprocessor::HandlePragma (Lex/Preprocessor.cpp)
  └─ PragmaNamespace::HandlePragma (Lex/Pragma.cpp)
       └─ 找 registered PragmaHandler
            │
            ▼
已注册的 handler (由 ParsePragma.cpp 注册):
  ├─ PragmaPackHandler (#pragma pack) → Parser::HandlePragmaPack
  ├─ PragmaWeakHandler (#pragma weak) → Parser::HandlePragmaWeak
  ├─ PragmaVisibilityHandler (#pragma GCC visibility)
  ├─ PragmaFloatControlHandler → Parser::HandlePragmaFloatControl
  ├─ PragmaFPContractHandler → Parser::HandlePragmaFPContract
  ├─ PragmaCommentHandler (MSVC __pragma(comment(...)))
  ├─ PragmaIncludeAliasHandler
  ├─ PragmaDetectMismatchHandler (MSVC)
  ├─ #pragma omp → Parser::ParseOpenMPDirective
  │    (ParseOpenMP.cpp)
  ├─ #pragma acc → Parser::ParseOpenACCDirective
  │    (ParseOpenACC.cpp)
  └─ #pragma clang loop / #pragma unroll
       └─ Parser::ParsePragmaLoopHint
            └─ 构造 LoopHint (clang/Parse/LoopHint.h)
                 └─ Sema::ActOnPragmaLoopHint
```

---

## §5. 推荐阅读顺序

### 阶段 1: Parser 入口与主类 (1.5 小时)
- [`ParseAST.cpp`](ParseAST.cpp) — 顶层 ParseAST 入口
- [`Parser.cpp`](Parser.cpp) — 主 Parser 类与顶层分派

### 阶段 2: 公开头与辅助类型 (30 分钟)
- [`include/clang/Parse/Parser.h`](../../include/clang/Parse/Parser.h)
  — Parser + ParsingDeclarator + ParsingDeclSpec + TPResult
- [`include/clang/Parse/ParseAST.h`](../../include/clang/Parse/ParseAST.h)
  — `ParseAST(Sema&)` 声明
- [`include/clang/Parse/RAIIObjectsForParser.h`](../../include/clang/Parse/RAIIObjectsForParser.h)
  — RAII helper

### 阶段 3: 声明解析 (2 小时)
- [`ParseDecl.cpp`](ParseDecl.cpp) — C/C++ 通用声明
- [`ParseDeclCXX.cpp`](ParseDeclCXX.cpp) — C++ 声明 (类 / 命名空间)
- [`ParseTemplate.cpp`](ParseTemplate.cpp) — 模板参数 + concept

### 阶段 4: 表达式与 initializer (2 小时)
- [`ParseExpr.cpp`](ParseExpr.cpp) — C/C++ 通用表达式
- [`ParseExprCXX.cpp`](ParseExprCXX.cpp) — C++ 表达式 (lambda / new / requires)
- [`ParseInit.cpp`](ParseInit.cpp) — initializer + designator

### 阶段 5: 语句 / asm / inline-method 延迟 (2 小时)
- [`ParseStmt.cpp`](ParseStmt.cpp) — 语句分派
- [`ParseStmtAsm.cpp`](ParseStmtAsm.cpp) — GCC/MSVC 内联 asm
- [`ParseCXXInlineMethods.cpp`](ParseCXXInlineMethods.cpp) — 延迟解析

### 阶段 6: 试探性 lookahead 消歧 (1 小时)
- [`ParseTentative.cpp`](ParseTentative.cpp) — `TPResult` /
  `isCXXSimpleDeclaration` / `TryParseSimpleDeclaration`

### 阶段 7: 前端子语言 (OpenACC/OpenMP/HLSL/ObjC/反射) (2 小时)
- [`ParseObjc.cpp`](ParseObjc.cpp) — `@interface` / `@property`
- [`ParseOpenACC.cpp`](ParseOpenACC.cpp) — `#pragma acc`
- [`ParseOpenMP.cpp`](ParseOpenMP.cpp) — `#pragma omp`
- [`ParseHLSL.cpp`](ParseHLSL.cpp) — HLSL 限定符 / 语义
- [`ParseHLSLRootSignature.cpp`](ParseHLSLRootSignature.cpp) —
  Root Signature 子解析器
- [`ParseReflect.cpp`](ParseReflect.cpp) — C++26 `^^` 反射

### 阶段 8: Pragma handler (按需)
[`ParsePragma.cpp`](ParsePragma.cpp) +
[`include/clang/Parse/LoopHint.h`](../../include/clang/Parse/LoopHint.h)
+ [`CMakeLists.txt`](CMakeLists.txt)

---

## §6. 自定义扩展指南

### 6.1 添加新声明语法

1. 在 [`Parser.cpp`](Parser.cpp) 的 `ParseExternalDeclaration` /
   `ParseStatementOrDeclaration` 加 dispatch case (按 token kind)。
2. 在 [`ParseDecl.cpp`](ParseDecl.cpp) 加 `Parser::ParseXxx`
   函数, 调 [`Sema::ActOnXxx`](../include/clang/Sema/Sema.h)。
3. 测试: `clang/test/Parser/xxx.c` + `clang/test/AST/xxx.cpp`。

### 6.2 添加新 C++ 表达式

1. 在 [`ParseExpr.cpp`](ParseExpr.cpp) 或
   [`ParseExprCXX.cpp`](ParseExprCXX.cpp) 加 `Parser::ParseXxx`。
2. 在 [`AST/Expr.h`](../include/clang/AST/Expr.h) /
   [`AST/ExprCXX.h`](../include/clang/AST/ExprCXX.h) 加新
   `Expr` 子类 (Stmt/Expr 节点)。
3. 在 [`Sema/Sema.h`](../include/clang/Sema/Sema.h) /
   [`Sema/SemaExpr.cpp`](../Sema/SemaExpr.cpp) 加
   `ActOnXxx`。
4. 在 [`ParseTentative.cpp`](ParseTentative.cpp) 更新 `isCXXTypeId`
   / `isCXXExpressionStart` 让消歧能找到新 token。
5. 测试: `clang/test/Parser/xxx.cpp` + `clang/test/Sema/xxx.cpp`。

### 6.3 添加新 `#pragma`

1. 在
   [`clang/include/clang/Lex/Pragma.h`](../../include/clang/Lex/Pragma.h)
   派生 `PragmaHandler` 子类 (或直接注册字符串 namespace)。
2. 在 [`ParsePragma.cpp`](ParsePragma.cpp) 加
   `Parser::HandlePragmaXxx`, 在 `initializePragmaHandlers` 注
   册。
3. 在 [`Options.td`](../../include/clang/Driver/Options.td) 加
   `-f<your-flag>` driver 选项 (可选)。
4. 测试: `clang/test/Preprocessor/pragma-xxx.c`。

### 6.4 添加新 #pragma omp 指令 / 子句

1. 更新
   [`Basic/OpenMPKinds.h`](../../include/clang/Basic/OpenMPKinds.h)
   (`OpenMPDirectiveKind` / `OpenMPClauseKind`) + `.def` 表。
2. 在 [`ParseOpenMP.cpp`](ParseOpenMP.cpp) 的
   `ParseOpenMPDirectiveKind` / `ParseOpenMPClause` 加 case。
3. 在 [`Sema/SemaOpenMP.cpp`](../Sema/SemaOpenMP.cpp) 加
   `ActOnOpenMPXxxClause` / `ActOnOpenMPXxxDirective`。
4. 测试: `clang/test/OpenMP/xxx.c`。

### 6.5 添加新 #pragma acc 指令 / 子句

参考 §6.4 的 OpenMP 流程, 文件对应
`ParseOpenACC.cpp` /
[`Basic/OpenACCKinds.h`](../../include/clang/Basic/OpenACCKinds.h) /
[`Sema/SemaOpenACC.cpp`](../Sema/SemaOpenACC.cpp)。

### 6.6 添加新 HLSL 语法

1. 在 [`ParseHLSL.cpp`](ParseHLSL.cpp) 加 token kind dispatch。
2. 在 [`Sema/SemaHLSL.cpp`](../Sema/SemaHLSL.cpp) 加
   `ActOnHLSLXxx`。
3. 如需新 root signature 元素: 在
   [`ParseHLSLRootSignature.cpp`](ParseHLSLRootSignature.cpp) 加
   `RootSignatureParser::parseXxx`。
4. 测试: `clang/test/HLSL/xxx.hlsl`。

### 6.7 添加新 C++ 反射语法

1. 在 [`ParseReflect.cpp`](ParseReflect.cpp) 加
   `Parser::ParseXxxReflectXxx`, 调
   [`Sema::ActOnCXXReflectExpr`](../include/clang/Sema/Sema.h)。
2. 在
   [`Basic/TokenKinds.def`](../../include/clang/Basic/TokenKinds.def)
   加新 token kind (如 `^^` 之前没有, 就加 `tok::caretcaret`)。
3. 在 `LangOptions` 加 `CPlusPlus26` 之类的开关。
4. 测试: `clang/test/Parser/cxx2b-reflection.cpp`。

### 6.8 调试 Parser 行为

1. 用 `clang -Xclang -ast-dump -Xclang -ast-dump-filter=foo foo.cpp`
   看 AST 结构。
2. 用 `clang -Xclang -print-stats` 看 Parse 统计。
3. 用 `clang -Xclang -debug-pass-manager foo.cpp` 不直接有用, 但
   `clang -Rpass-missed=...` 可观察后续 IR Pass。
4. 临时在 [`Parser.cpp`](Parser.cpp) 的 `ConsumeToken` /
   `ParseExternalDeclaration` 加 `llvm::errs() << ...` trace。
5. 用 `clang -Xclang -detailed-debuginfo` 让 ASTContext 记录详细
   `TypeLoc` (慢但有用)。
6. 在 `clang/lib/AST/ASTDumper.cpp` 的 `VisitXxx` 加临时 dump。

---

## §7. NT 注释索引

当前 `clang/lib/Parse/` 下尚无 `// <NT>` 注释。已建立目录索引,
姊妹 overview:

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
- [`clang/lib/Sema/0-overview.md`](../Sema/0-overview.md) — Clang 语义
  分析

按 "少而精" 原则, 加 NT 注释建议优先级:

1. [`Parser.cpp`](Parser.cpp) — 8-12 段 (主 Parser 类, 整个 Parse 库
   入口)
2. [`ParseDecl.cpp`](ParseDecl.cpp) — 5-8 段 (声明解析)
3. [`ParseExpr.cpp`](ParseExpr.cpp) — 5-8 段 (表达式解析)
4. [`ParseDeclCXX.cpp`](ParseDeclCXX.cpp) — 5-8 段 (C++ 声明)
5. [`ParseStmt.cpp`](ParseStmt.cpp) — 5-8 段 (语句解析)
6. [`ParseTentative.cpp`](ParseTentative.cpp) — 4-6 段 (试探
   消歧)
7. [`ParseExprCXX.cpp`](ParseExprCXX.cpp) — 4-6 段 (C++ 表达式)
8. [`ParseTemplate.cpp`](ParseTemplate.cpp) — 4-6 段 (模板)
9. [`ParseCXXInlineMethods.cpp`](ParseCXXInlineMethods.cpp) — 3-5
   段 (延迟解析)
10. [`ParseObjc.cpp`](ParseObjc.cpp) — 3-5 段 (ObjC 语法)
11. [`ParseOpenMP.cpp`](ParseOpenMP.cpp) + [`ParseOpenACC.cpp`](ParseOpenACC.cpp)
    — 各 3-5 段
12. [`ParsePragma.cpp`](ParsePragma.cpp) — 3-5 段 (`#pragma`
    分发)
13. [`ParseAST.cpp`](ParseAST.cpp) — 2-4 段 (顶层入口)
14. [`ParseHLSL.cpp`](ParseHLSL.cpp) +
    [`ParseHLSLRootSignature.cpp`](ParseHLSLRootSignature.cpp) —
    各 2-3 段
15. [`ParseInit.cpp`](ParseInit.cpp) +
    [`ParseStmtAsm.cpp`](ParseStmtAsm.cpp) +
    [`ParseReflect.cpp`](ParseReflect.cpp) — 各 2-3 段

---

**姊妹文档**: 本目录对应 LLVM 流水线中的 **Clang 语法分析层**,
是 Lex/PP 的 token 消费者, 也是 Sema 的 "语法骨架" 供应者。它与
[`clang/lib/Lex/`](../Lex/0-overview.md) 配合通过
`Preprocessor::Lex` 取 token, 与 [`clang/lib/Sema/`](../Sema/0-overview.md)
配合通过 `Sema::ActOnXxx` 完成 AST 节点, 与
[`clang/lib/AST/`](../AST/0-overview.md) 配合在 `ASTContext` 中
填 Decl/Stmt/Expr 节点, 与 [`clang/lib/Frontend/`](../Frontend/0-overview.md)
配合被 `clang::ParseAST(Sema&)` 驱动。Parser 自身 **不** 做语义
检查, 只保证语法结构合法并调 Sema。
