<!-- <NT>overview:clang/lib/ASTMatchers/ -->

# Clang ASTMatchers 库导读 — `clang/lib/ASTMatchers/`

> 本文档梳理 `clang/lib/ASTMatchers/` 目录下所有源文件 (5 个顶层
> .cpp/.h + 7 个 `Dynamic/` 子目录 .cpp/.h = 12 个) 的职责、上下游
> 与推荐阅读顺序。
>
> 目标读者: 想理解 **Clang AST Matcher DSL** (声明式 AST 模式匹
> 配) 的开发者, 以及要给 clang-tidy / clang-query 写新 matcher /
> 自定义 check / 集成静态分析工具的人。
>
> 所有路径相对 `clang/lib/ASTMatchers/`。同名公开头文件位于
> `clang/include/clang/ASTMatchers/` (如 `ASTMatchers.h`、
> `ASTMatchFinder.h`、`ASTMatchersInternal.h`、
> `ASTMatchersMacros.h`、`LowLevelHelpers.h` + `Dynamic/` 子目录)。
> 本目录编译为 `clangASTMatchers` 库, 链接 `clangAST` + LLVM Support。

---

## §0. Clang ASTMatchers 库在编译流水线中的位置

`clang/lib/ASTMatchers` 提供 Clang 的 **声明式 AST 模式匹配框架**,
分两部分:

1. **静态 DSL** (`ASTMatchers.h` + `ASTMatchersInternal.cpp`):
   C++ 模板 matcher 工厂函数 (`recordDecl()` /
   `hasName("foo")` / `cxxRecordDecl(...)`), 给 clang-tidy /
   static-analyzer / libclang / 各种 refactoring 工具直接调
   用; `MatchFinder` 跑 matcher, 触发 `MatchCallback::run`。
2. **动态 Registry** (`Dynamic/` 子目录): 让用户用 **S-表达式**
   (`recordDecl().bind("x")`) 字符串构造 matcher, 给
   `clang-query` (交互式 matcher 输入) 和 YAML/JSON 配置用;
   通过 `Parser` → `Registry` → `VariantMatcher` 桥接到静态
   matcher。

它在 Clang 工具链内部的层次:

```
┌─────────────────────────────────────────────────────────┐
│ 用户工具:                                                 │
│   · clang-tidy (每个 check 用 AST_MATCHER 宏定义 matcher) │
│   · clang-query (交互式输入 S-表达式)                    │
│   · static-analyzer checks (custom AST matchers)        │
│   · clang-refactor / clang-rename (libclang + matchers) │
│   · libclang (CXCursor 走 matcher 后端)                  │
└─────────────────────────────────────────────────────────┘
                │
                ▼
┌─────────────────────────────────────────────────────────┐
│ clang/lib/ASTMatchers  (本目录) ← 你在这里               │
│   · 静态 DSL:                                              │
│      · ASTMatchersInternal.cpp: 所有 matcher 工厂实现   │
│        (recordDecl / hasName / ofClass / eachOf ...),   │
│        DynTypedMatcher / BoundNodesTreeBuilder          │
│      · ASTMatchFinder.cpp: MatchFinder +                │
│        MatchASTVisitor/MatchChildASTVisitor (递归 AST   │
│        访问 + memoization)                              │
│      · LowLevelHelpers.cpp: CallExpr / CXXConstructExpr  │
│        实参与形参对齐                                     │
│   · 动态 Registry:                                         │
│      · Dynamic/Registry.cpp: named matcher →            │
│        MatcherDescriptor 查表 + 构造                     │
│      · Dynamic/Parser.cpp: S-表达式 → VariantMatcher 树 │
│      · Dynamic/Marshallers.cpp: 字面量 → typed matcher  │
│        (enum/regex/best-guess 桥接)                       │
│      · Dynamic/VariantValue.cpp: 多态参数 + matcher      │
│      · Dynamic/Diagnostics.cpp: 解析 / 构造期错误报告    │
└─────────────────────────────────────────────────────────┘
                │
                ▼
┌─────────────────────────────────────────────────────────┐
│ 上游依赖:                                                │
│   · clang/lib/AST (RecursiveASTVisitor 访问 AST 节点,    │
│     ASTContext 拿 ParentMap / Identifier, Decl/Stmt/     │
│     Type 节点遍历, SourceManager 找 token 文本)          │
│   · clang/lib/Lex (取 token 文本给 hasName matcher)     │
│   · clang/lib/Frontend/CompilerInstance (作为             │
│     ASTConsumer / FrontendAction 嵌入 MatchFinder)       │
│   · llvm/Support/Regex (hasName 正则)                    │
│   · llvm/ADT/StringMap / Twine (错误格式化)              │
└─────────────────────────────────────────────────────────┘
```

### §0.1 公开接口 (`clang/include/clang/ASTMatchers/`)

本目录 `.cpp` 文件依赖的 **公开头** 在
[`clang/include/clang/ASTMatchers/`](../../include/clang/ASTMatchers/),
最关键的:

- [`ASTMatchers.h`](../../include/clang/ASTMatchers/ASTMatchers.h)
  — **声明式 AST matcher DSL** 全部命名 matcher
  (`recordDecl` / `cxxRecordDecl` / `functionDecl` / `hasName` /
  `eachOf` / `anyOf` / `allOf` / ...)。
- [`ASTMatchFinder.h`](../../include/clang/ASTMatchers/ASTMatchFinder.h)
  — `class MatchFinder` + `class MatchFinder::MatchCallback` +
  `class MatchFinder::MatchResult`; 顶层 `ASTConsumer` 给 clang-tidy
  / static-analyzer 嵌入。
- [`ASTMatchersInternal.h`](../../include/clang/ASTMatchers/ASTMatchersInternal.h)
  — 内部类型: `Matcher<T>` / `DynTypedMatcher` /
  `ASTMatchFinder` / `BoundNodesTreeBuilder` /
  `MatcherInterface<T>`。
- [`ASTMatchersMacros.h`](../../include/clang/ASTMatchers/ASTMatchersMacros.h)
  — `AST_MATCHER` / `AST_MATCHER_P` / `AST_MATCHER_FUNCTION` 宏:
  用户自定义 matcher。
- [`LowLevelHelpers.h`](../../include/clang/ASTMatchers/LowLevelHelpers.h)
  — 纯 AST helper: `matchEachArgumentWithParamType` (CallExpr /
  CXXConstructExpr 实参-形参对齐)。
- `Dynamic/Parser.h` — S-表达式 matcher parser, `Parser::Sema`
  接口。
- `Dynamic/Registry.h` — `class Registry` (named matcher 查表 +
  构造 + code completion)。
- `Dynamic/VariantValue.h` — `ArgKind` + `VariantValue` +
  `VariantMatcher` 多态值/matcher。
- `Dynamic/Diagnostics.h` — Parser/Registry 错误栈。

### §0.2 与 [`clang/lib/AST/`](../AST/) 的关系

- `MatchASTVisitor` / `MatchChildASTVisitor` 是
  [`RecursiveASTVisitor`](../AST/RecursiveASTVisitor.cpp) 的
  子类, 走遍 AST 全部节点。
- 各 matcher (`recordDecl` / `functionDecl` / `cxxRecordDecl`)
  直接遍历 `AST` 的 Decl / Stmt / Type / TypeLoc / QualType 节点。
- `Matcher<T>` 与 `ASTContext` 的 ParentMapContext 配合, 实现
  `hasAncestor` / `hasParent` (父链查找, O(1) 经 ParentMap)。

### §0.3 与 [`clang/lib/Frontend/`](../Frontend/) 的关系

- `MatchFinder` 是 `ASTConsumer` 子类, 由 clang-tidy 在
  `ClangTidyASTConsumerFactory::addClangTidyChecks` 中装入。
- clang-tidy 走 `ClangTidyAction` (派生自 `FrontendAction`),
  `CreateASTConsumer` 返回包装 `MatchFinder` 的 consumer。

### §0.4 与 [`clang/lib/Tooling/`](../Tooling/) 的关系

- clang-query 经 `clang-query-tool.cpp` + `Query.cpp` 接收用
  户输入的 S-表达式, 调 `Dynamic::Parser::parse` 后转
  `DynTypedMatcher`, 再交给 `MatchFinder::match`。
- static-analyzer checks 经 `ento::check::ASTCodeBody` 等
  MatchFinder 集成。

### §0.5 与 LLVM Support 的关系

- `llvm::Regex` 给 `hasName` 正则匹配。
- `llvm::StringMap` 给 `Registry` 内部查表。
- `llvm::ManagedStatic` 给 `RegistryMaps` 单例。
- `llvm::IntrusiveRefCntPtr` 给 `MatcherInterface<T>` 引用计数。

---

## §1. 编译流水线概览

```
                           完整 AST (Sema 完成后)
                                      │
                                      ▼
┌─────────────────────────────────────────────────────────┐
│ clang-tidy / clang-query / static-analyzer check        │
│   · 用户调用 recordDecl(hasName("foo")).bind("x")        │
│   · 构造 Matcher<Decl> / DynTypedMatcher 树             │
│   · 调 MatchFinder::addMatcher(Matcher, &MyCallback)    │
└─────────────────────────────────────────────────────────┘
                │
                ▼
┌─────────────────────────────────────────────────────────┐
│ MatchFinder::matchAST (ASTContext)                       │
│   · 装 MatchASTConsumer (matcher ASTConsumer)           │
│   · 装 MatchASTVisitor (RecursiveASTVisitor 子类)       │
│   · ASTContext.getParentMapContext() 准备 ParentMap     │
└─────────────────────────────────────────────────────────┘
                │
                ▼
                ┌──────────────────────────────────┐
                │ 循环: RecursiveASTVisitor 遍历    │
                │ (Decl / Stmt / TypeLoc / Nested) │
                └──────────────────────────────────┘
                          │
                          ▼
┌─────────────────────────────────────────────────────────┐
│ MatchASTVisitor::TraverseDecl / TraverseStmt /          │
│ TraverseTypeLoc / TraverseNestedNameSpecifierLoc        │
│   ├─ MatchKey 算子 → MemoizedMatchResult 缓存          │
│   ├─ BindableMatcher::matches (BoundNodesTreeBuilder)   │
│   └─ 命中 → 回调 MatchCallback::run(MatchResult)       │
│        └─ 用户: clang-tidy check 生成 diagnostics       │
│              / static-analyzer 报告 bug /               │
│              refactor 改写代码                          │
└─────────────────────────────────────────────────────────┘
                │
                ▼
┌─────────────────────────────────────────────────────────┐
│ 产物: MatchFinder::MatchResult { Nodes, Context,        │
│                                 SourceManager, ASTContext,│
│                                 BoundNodes }             │
└─────────────────────────────────────────────────────────┘

动态 (clang-query) 旁路:
  用户输入 S-表达式 → Dynamic::Parser (tokenize + parse)
    └─ Dynamic::Registry::lookupMatcherCtor / constructMatcher
         └─ Dynamic::Marshallers (字面量 → typed Matcher)
              └─ VariantMatcher → DynTypedMatcher → MatchFinder
                   └─ ... 同上

错误流:
  Parser / Registry 错误 → Dynamic::Diagnostics
    └─ 输出 SourceRange + Stack + 格式化消息
         └─ clang-query 报告给用户
```

---

## §2. 文件目录结构

```
clang/lib/ASTMatchers/  (12 文件, 1 子目录 Dynamic/)
├── §3.1  Match finder 顶层入口                            (1 文件)
├── §3.2  Static matcher DSL 内部实现                      (1 文件)
├── §3.3  Low-level helper                                 (1 文件)
└── §3.4  Dynamic/ (S-表达式 Registry + Parser + ...)     (7 文件)
    ├── §3.4.1  Dynamic Registry 查表                       (1 文件)
    ├── §3.4.2  Dynamic Parser (S-表达式)                   (1 文件)
    ├── §3.4.3  Marshallers (静态 ↔ 动态桥接)               (1 文件)
    ├── §3.4.4  VariantValue / VariantMatcher               (1 文件)
    └── §3.4.5  Diagnostics                                (1 文件)
```

### 2.1 文件数量统计

| 区域 | .cpp | .h | 合计 |
|------|------|-----|------|
| Match finder | 1 | 0 | 1 |
| Static matcher DSL | 1 | 0 | 1 |
| Low-level helper | 1 | 0 | 1 |
| Dynamic/ Registry | 1 | 0 | 1 |
| Dynamic/ Parser | 1 | 0 | 1 |
| Dynamic/ Marshallers | 1 | 1 | 2 |
| Dynamic/ VariantValue | 1 | 0 | 1 |
| Dynamic/ Diagnostics | 1 | 0 | 1 |
| Build | 1 | 0 | 1 (`CMakeLists.txt`) + `Dynamic/CMakeLists.txt` |
| **总计** | **~9** | **~1** | **~10** + 2 × `CMakeLists.txt` |

(实际为 12 个文件: 5 顶层 + 7 Dynamic/)

---

## §3. 文件详解

### 3.1 Match finder

[`ASTMatchFinder.cpp`](ASTMatchFinder.cpp) — `MatchFinder` 实现:
通过 `RecursiveASTVisitor` 走 AST, 用 `MatchKey` 算子缓存匹配
结果 (`MemoizedMatchResult`), 把命中结果派发给用户的
`MatchCallback::run(MatchResult)`。
- 上游: `clang-tidy` 各 check, `clang-query`, static-analyzer
  checks, `clang-refactor` / `clang-rename`。
- 下游: `RecursiveASTVisitor` (`AST/RecursiveASTVisitor.cpp`),
  `ASTContext` (ParentMapContext), `DynTypedMatcher` /
  `BoundNodesTreeBuilder`。
- 关键类/函数: `MatchFinder`, `MatchFinder::MatchCallback`,
  `MatchFinder::MatchResult`, `MatchASTVisitor`,
  `MatchChildASTVisitor`, `MatchASTConsumer`, `MatchKey` /
  `MemoizedMatchResult`。

### 3.2 Static matcher DSL

[`ASTMatchersInternal.cpp`](ASTMatchersInternal.cpp) — 内部 matcher
基础设施 (`MatcherCtor` / `DynMatcherInterface` /
`BoundNodesTreeBuilder` / variadic operators) +
[`ASTMatchers.h`](../../include/clang/ASTMatchers/ASTMatchers.h)
中所有 matcher 工厂函数的实现。
- 上游:
  [`ASTMatchers.h`](../../include/clang/ASTMatchers/ASTMatchers.h)
  (user-facing matchers),
  [`ASTMatchersMacros.h`](../../include/clang/ASTMatchers/ASTMatchersMacros.h),
  [`ASTMatchFinder.cpp`](ASTMatchFinder.cpp),
  [`Dynamic/Registry.cpp`](Dynamic/Registry.cpp)。
- 下游: `AST` (Decl/Stmt/TypeLoc traversal), `ParentMapContext`,
  `Lexer` (token 文本), `llvm::Regex`。
- 关键类/函数: `DynTypedMatcher`, `VariadicMatcher` /
  `IdDynMatcher` / `TrueMatcherImpl` / `DynTraversalMatcherImpl`,
  `BoundNodesTreeBuilder`, `HasNameMatcher` / `PatternSet`,
  `matchesAnyBase`, `allOfVariadicOperator` /
  `anyOfVariadicOperator` / `eachOfVariadicOperator` /
  `notUnaryOperator` / `optionallyVariadicOperator`,
  `hasAnyNameFunc` / `hasAnySelectorFunc`。

### 3.3 Low-level helper

[`LowLevelHelpers.cpp`](LowLevelHelpers.cpp) — 纯 AST helper: 给
matcher 复用, `matchEachArgumentWithParamType` 把 `CallExpr` /
`CXXConstructExpr` 实参与 `FunctionProtoType` 形参对齐 (供
`hasArgument` matcher 用)。
- 上游:
  [`ASTMatchersInternal.cpp`](ASTMatchersInternal.cpp),
  static-analyzer / clang-tidy checks, `libclang`。
- 下游: `CallExpr` / `CXXConstructExpr` / `FunctionProtoType`,
  `Type` / `QualType`。
- 关键类/函数: `matchEachArgumentWithParamType(CallExpr)`,
  `matchEachArgumentWithParamType(CXXConstructExpr)`,
  `matchEachArgumentWithParamTypeImpl`, `getCallee`。

### 3.4 Dynamic/ (S-表达式 Registry + Parser + ...)

#### 3.4.1 Dynamic Registry 查表

[`Dynamic/Registry.cpp`](Dynamic/Registry.cpp) — 静态、延迟构建的
`StringMap<MatcherDescriptor>` (named matcher → constructor); 查
找 / 构造 / 重载决议 / code completion; 用 `REGISTER_MATCHER` 宏
注册全部 named matcher。
- 上游:
  [`Dynamic/Parser.cpp`](Dynamic/Parser.cpp),
  `clang-query`,
  `libclang` (CXCursor API)。
- 下游:
  `Dynamic/Marshallers.h` (`MatcherDescriptor` /
  `OverloadedMatcherDescriptor` 等),
  `Dynamic/VariantValue.h`,
  [`clang/ASTMatchers/ASTMatchers.h`](../../include/clang/ASTMatchers/ASTMatchers.h)。
- 关键类/函数: `RegistryMaps` (private), `Registry`
  (`lookupMatcherCtor` / `constructMatcher` /
  `constructBoundMatcher`), `Registry::getMatcherCompletions`,
  `Registry::getAcceptedCompletionTypes`,
  `REGISTER_MATCHER` / `REGISTER_OVERLOADED_2` /
  `REGISTER_REGEX_MATCHER` 宏,
  `ManagedStatic<RegistryMaps> RegistryData`。

#### 3.4.2 Dynamic Parser (S-表达式)

[`Dynamic/Parser.cpp`](Dynamic/Parser.cpp) — 递归下降 tokenizer +
parser for S-表达式 matcher DSL (`recordDecl().bind("x")`,
`cxxRecordDecl(hasMethod(...)).bind(...)`); 构造 `VariantMatcher`
树 (经 `Parser::Sema` → `Registry`) + code completion。
- 上游: `clang-query` (交互式输入), YAML/JSON matcher 配置。
- 下游:
  [`Dynamic/Registry.cpp`](Dynamic/Registry.cpp)
  (lookup / build),
  [`Dynamic/Diagnostics.cpp`](Dynamic/Diagnostics.cpp),
  [`Dynamic/VariantValue.cpp`](Dynamic/VariantValue.cpp)。
- 关键类/函数: `Parser`, `Parser::Sema`,
  `Parser::CodeTokenizer`, `Parser::TokenInfo`,
  `parseMatcherExpression` / `parseMatcherBuilder` /
  `parseBindID`, `addCompletion` /
  `addExpressionCompletions`。

#### 3.4.3 Marshallers

[`Dynamic/Marshallers.h`](Dynamic/Marshallers.h) +
[`Dynamic/Marshallers.cpp`](Dynamic/Marshallers.cpp) — `ArgTypeTraits`
特化: best-guess (编辑距离建议) / enum-name / regex-flag, 把字
符串参数转换为 typed 值 (给 `Registry` 用)。
- 上游: [`Dynamic/Registry.cpp`](Dynamic/Registry.cpp),
  [`Dynamic/Marshallers.h`](Dynamic/Marshallers.h) (模板定义)。
- 下游: `clang::attr::Kind`, `clang::CastKind`,
  `clang::OpenMPClauseKind`, `clang::UnaryExprOrTypeTrait`,
  `llvm::Regex`。
- 关键类/函数: `getBestGuess` (编辑距离建议),
  `ArgTypeTraits<attr::Kind>::getBestGuess`,
  `ArgTypeTraits<CastKind>::getBestGuess`,
  `ArgTypeTraits<OpenMPClauseKind>::getBestGuess`,
  `ArgTypeTraits<UnaryExprOrTypeTrait>::getBestGuess`,
  `ArgTypeTraits<llvm::Regex::RegexFlags>::getFlags/getBestGuess`。

#### 3.4.4 Variant values

[`Dynamic/VariantValue.cpp`](Dynamic/VariantValue.cpp) — 多态值
(`VariantValue`): 装任意 matcher 实参; `VariantMatcher`: 单 / 多态
/ variadic matcher 抽象。
- 上游: [`Dynamic/Registry.cpp`](Dynamic/Registry.cpp),
  [`Dynamic/Parser.cpp`](Dynamic/Parser.cpp),
  `Dynamic/Marshallers.h`。
- 下游: `DynTypedMatcher`, `ASTNodeKind`, `Twine`。
- 关键类/函数: `ArgKind`, `VariantValue`, `VariantMatcher`,
  `VariantMatcher::SinglePayload` / `PolymorphicPayload` /
  `VariadicOpPayload`, `VariantMatcher::MatcherOps`,
  `isConvertibleTo` / `getTypedMatcher`。

#### 3.4.5 Diagnostics

[`Dynamic/Diagnostics.cpp`](Dynamic/Diagnostics.cpp) — Parser /
Registry 错误报告: 上下文栈 (Context frames) + 重载跟踪 + 人可
读 format string。
- 上游: [`Dynamic/Parser.cpp`](Dynamic/Parser.cpp),
  [`Dynamic/Registry.cpp`](Dynamic/Registry.cpp),
  `Dynamic/Marshallers.h`。
- 下游: `Twine`, `SourceRange` / `SourceLocation` (dynamic
  namespace)。
- 关键类/函数: `Diagnostics::addError` / `pushContextFrame`,
  `Diagnostics::Context` / `OverloadContext`,
  `Diagnostics::ArgStream`,
  `contextTypeToFormatString` /
  `errorTypeToFormatString`。

[`CMakeLists.txt`](CMakeLists.txt) + [`Dynamic/CMakeLists.txt`](Dynamic/CMakeLists.txt)
— `clangASTMatchers` 库定义: 列出 12 个 .cpp, 链接 `clangAST` +
`LLVM Support`。

---

## §4. 关键调用链

### 4.1 静态 matcher 使用链 (clang-tidy check)

```
用户 clang-tidy check 派生 AST_MATCHER / 注册 MatchCallback
  │
  ▼
MyCheck::registerMatchers(MatchFinder *Finder) {
    auto Matcher = recordDecl(
        hasName("foo"),
        has(cxxMethodDecl(...))
    ).bind("x");
    Finder->addMatcher(Matcher, this);
}
  │
  ▼
FrontendAction 跑 ClangTidyAction::CreateASTConsumer
  └─ return ClangTidyASTConsumerFactory
       └─ 包装 MatchFinder
            │
            ▼
Sema 完成后 → ASTConsumer::HandleTranslationUnit
  │
  ▼
MatchFinder::matchAST (ASTContext)
  ├─ 装 MatchASTVisitor (RecursiveASTVisitor 子类)
  ├─ ASTContext.getParentMapContext()
  └─ 触发 MatchASTConsumer::HandleTranslationUnit
       │
       ▼
       循环: MatchASTVisitor::TraverseDecl / TraverseStmt /
              TraverseTypeLoc / TraverseNestedNameSpecifierLoc
            ├─ 对每个节点:
            │    ├─ MatchKey 算子 (node identity + matcher ID)
            │    ├─ 查 MemoizedMatchResult 缓存
            │    ├─ 缓存 miss → 跑 DynTypedMatcher::matches
            │    │    ├─ BoundNodesTreeBuilder::addBound(id, node)
            │    │    ├─ 子 matcher (递归)
            │    │    └─ matches → true
            │    └─ 缓存命中 → 直接返回
            ├─ 命中 → 收集到 MatchCallback::Matches
            └─ 全部命中后 → MatchCallback::run(MatchResult)
                 └─ MyCheck::run(MatchResult const &Result) {
                       const auto *RD = Result.Nodes.getNodeAs<Decl>("x");
                       diag(RD->getLocation(), "...");
                    }
```

### 4.2 动态 matcher 解析链 (clang-query)

```
用户输入: recordDecl(hasName("foo")).bind("x")
  │
  ▼
clang-query / 配置文件 调 Dynamic::Parser::parse
  │
  ▼
Parser::CodeTokenizer
  ├─ 字符流 → Token 流
  │    (recordDecl, (, hasName, (, "foo", ), ), ., bind, (, "x", ), EOF)
  └─ TokenInfo (位置 + kind + 文本)
       │
       ▼
Parser::parseMatcherExpression
  ├─ 读 identifier "recordDecl"
  ├─ Parser::Sema::lookupMatcherCtor
  │    └─ Dynamic::Registry::lookupMatcherCtor
  │         ├─ RegistryMaps[name] → MatcherDescriptor
  │         ├─ 查 OverloadedMatcherDescriptor (重载)
  │         └─ 校验 arg 数 + 类型
  ├─ 解析 args (递归 parseMatcherExpression)
  │    ├─ hasName("foo") → Matcher<std::string> arg
  │    │    └─ Marshallers::convert("foo") → string
  │    │         └─ VariantValue::String = "foo"
  │    └─ 返回 VariantMatcher (SinglePayload)
  ├─ Parser::Sema::constructMatcher
  │    └─ Registry::constructMatcher
  │         └─ 调 MatcherDescriptor 的 ctor
  │              └─ 返回 VariantMatcher (DynTypedMatcher 实现)
  ├─ .bind("x")
  │    └─ Parser::Sema::actOnBind
  │         └─ constructBoundMatcher
  │              └─ 用 BindableMatcher 包装
  │                   └─ VariantMatcher (BoundNodesTreeBuilder)
  └─ 返回 DynTypedMatcher 给 MatchFinder
       │
       ▼
(同上静态匹配链 4.1)
```

### 4.3 hasName / hasType / hasArgument 等子 matcher 递归匹配链

```
MatchASTVisitor 找到某节点 (e.g. CXXRecordDecl)
  │
  ▼
DynTypedMatcher::matches(DynTypedNode, ASTContext, BoundNodesTreeBuilder)
  ├─ 转基类 (e.g. Matcher<CXXRecordDecl>)
  ├─ 子 matcher 求值:
  │    ├─ hasName("foo"):
  │    │    └─ HasNameMatcher::matches
  │    │         └─ IdentifierTable lookup → 名匹配
  │    │              └─ hasAnyNameFunc → PatternSet 匹配
  │    │                   └─ llvm::Regex::match
  │    ├─ ofClass(X):
  │    │    └─ ofClassImpl → QualType::getAsCXXRecordDecl
  │    ├─ hasType(T):
  │    │    └─ hasTypeMatcher::matches
  │    │         └─ QualType::getCanonicalType → 比较
  │    └─ eachOf(...):
  │         └─ EachOfMatcher::matches (AND)
  ├─ BoundNodesTreeBuilder 累积 .bind("x") 绑定
  └─ 全 AND / OR / NOT 求值 → matches → 派发
```

### 4.4 Memoization 缓存链

```
MatchKey 算子:
  ├─ Matcher descriptor pointer (身份)
  ├─ bound names 列表 (影响 BoundNodesTreeBuilder)
  ├─ TraversalKind scope (AsIs / IgnoreImplicitCastsAndParentheses / ...)
  └─ ParentMap node ID (hasParent / hasAncestor 时)

MemoizedMatchResult 缓存:
  ├─ (ASTContext, MatchKey) → result (true/false) + BoundNodes
  ├─ 同 matcher + 同节点 → 缓存命中, 跳过子求值
  ├─ 不同 matcher 上下文 (e.g. hasParent 与 hasAncestor 不同) → 错开
  └─ MatchFinder::TraversalKindScope (RAII 切换)

性能优化:
  └─ 大型 clang-tidy 检查 (上万个 nested matchers) 上提升 2-10x
```

### 4.5 ParentMap 父链查找链

```
hasAncestor(...) / hasParent(...) matcher
  │
  ▼
HasParentMatcher / HasAncestorMatcher::matches
  ├─ ASTContext.getParentMapContext().getParent(node)
  │    └─ ParentMap 内部用 ChainImpl (Linear/Unique)
  │         ├─ O(1) 单步父节点查询
  │         └─ O(N) 整链查询
  ├─ 递归查直到 root 或命中
  └─ 子 matcher 验证
       └─ 命中 → true

ParentMap 构造:
  └─ ParentMapContext::ensure(ASTContext)
       ├─ 第一次 → 经 RecursiveASTVisitor 遍历 AST
       │    收集每个节点 → parent 映射
       └─ 复用缓存
```

---

## §5. 推荐阅读顺序

### 阶段 1: DSL surface 与宏 (1 小时)
- [`include/clang/ASTMatchers/ASTMatchers.h`](../../include/clang/ASTMatchers/ASTMatchers.h)
  — 全部 named matcher
- [`include/clang/ASTMatchers/ASTMatchersMacros.h`](../../include/clang/ASTMatchers/ASTMatchersMacros.h)
  — `AST_MATCHER` / `AST_MATCHER_P` 宏
- [`include/clang/ASTMatchers/ASTMatchersInternal.h`](../../include/clang/ASTMatchers/ASTMatchersInternal.h)
  — 内部类型

### 阶段 2: MatchFinder 入口 (1.5 小时)
- [`include/clang/ASTMatchers/ASTMatchFinder.h`](../../include/clang/ASTMatchers/ASTMatchFinder.h)
  — `MatchFinder` API
- [`ASTMatchFinder.cpp`](ASTMatchFinder.cpp) — `MatchASTVisitor`
  + memoization

### 阶段 3: 静态 matcher 内部 (2 小时)
- [`ASTMatchersInternal.cpp`](ASTMatchersInternal.cpp) — 全部
  matcher 工厂实现 + DynTypedMatcher
- [`LowLevelHelpers.cpp`](LowLevelHelpers.cpp) — 实参-形参对齐
  helper

### 阶段 4: 动态值层 (1 小时)
- [`include/clang/ASTMatchers/Dynamic/VariantValue.h`](../../include/clang/ASTMatchers/Dynamic/VariantValue.h)
- [`Dynamic/VariantValue.cpp`](Dynamic/VariantValue.cpp)
- [`include/clang/ASTMatchers/Dynamic/Diagnostics.h`](../../include/clang/ASTMatchers/Dynamic/Diagnostics.h)
- [`Dynamic/Diagnostics.cpp`](Dynamic/Diagnostics.cpp)

### 阶段 5: Marshallers (静态 ↔ 动态桥接) (1 小时)
- [`Dynamic/Marshallers.h`](Dynamic/Marshallers.h) +
  [`Dynamic/Marshallers.cpp`](Dynamic/Marshallers.cpp) — enum /
  regex 转换

### 阶段 6: 动态 Registry (1.5 小时)
- [`include/clang/ASTMatchers/Dynamic/Registry.h`](../../include/clang/ASTMatchers/Dynamic/Registry.h)
- [`Dynamic/Registry.cpp`](Dynamic/Registry.cpp) — 查表 + 构造

### 阶段 7: 动态 Parser (1.5 小时)
- [`include/clang/ASTMatchers/Dynamic/Parser.h`](../../include/clang/ASTMatchers/Dynamic/Parser.h)
- [`Dynamic/Parser.cpp`](Dynamic/Parser.cpp) — S-表达式 tokenize
  + parse + completion

---

## §6. 自定义扩展指南

### 6.1 用 `AST_MATCHER` 宏定义新 matcher

1. 在你的 check 文件 (clang-tidy check / static-analyzer check) 加:

   ```cpp
   AST_MATCHER(Decl, hasInterestingAttribute) {
     // Node 是 const Decl* 类型
     return Node->hasAttr<MyAttr>();
   }
   ```

2. 用 `AST_MATCHER_P(Class, Name, ParamType, ParamName)` (一个参
   数) / `AST_MATCHER_FUNCTION(ReturnType, Name, (Params...))` 扩
   展。

3. 测试: `clang-tidy` 的 check 测试 `clang/test/clang-tidy/...`。

### 6.2 用 `AST_MATCHER_REGEX` / `AST_MATCHER_REGEX_P` 支持正则

1. 同上, 模板参数改为 `AST_MATCHER_REGEX` / `AST_MATCHER_REGEX_P`。
2. matcher 体内可调 `findMatch(...)` 经 `llvm::Regex` 匹配。

### 6.3 给动态 Registry 注册 named matcher

1. 在 [`Dynamic/Registry.cpp`](Dynamic/Registry.cpp) 加:

   ```cpp
   REGISTER_MATCHER(myMatcher, AST_MATCHER(...)  // 或
                                           Matcher<Decl>);
   ```

2. 重载版本用 `REGISTER_OVERLOADED_2` / `REGISTER_OVERLOADED_3`。
3. 正则版本用 `REGISTER_REGEX_MATCHER`。

### 6.4 给 VariantMatcher 加新 VariantPayload

1. 在 [`Dynamic/VariantValue.h`](../../include/clang/ASTMatchers/Dynamic/VariantValue.h)
   加新 Payload 类型。
2. 在 [`Dynamic/VariantValue.cpp`](Dynamic/VariantValue.cpp) 实
   现构造 / 转换。
3. 在 [`Dynamic/Marshallers.cpp`](Dynamic/Marshallers.cpp) 加
   `ArgTypeTraits<NewType>::getBestGuess` / `convert`。

### 6.5 给 Parser 加新 S-表达式语法

1. 在 [`Dynamic/Parser.cpp`](Dynamic/Parser.cpp) 加新 token 类
   型 + 解析分支 (e.g. `[a-z]+` regex 字面量)。
2. 在 `Parser::Sema` 接口加新 action method (e.g. `actOnRegex`)。
3. 测试: 在 `clang-query` 加 unit test 验证新语法。

### 6.6 调试 matcher 行为

1. 用 `clang-query` 交互式调试:
   `clang-query foo.cpp --` 然后输入 `match recordDecl(...)`。
2. 用 `clang-tidy -checks='...' -list-checks` 验证 check 注册。
3. 用 `clang-tidy -fix` 看改写结果。
4. 在 [`ASTMatchFinder.cpp`](ASTMatchFinder.cpp) 的 `matches`
   加 `llvm::errs() << ...` trace match 结果。
5. 用 `-debug` LLVM flag 看 MatcherKey 缓存命中率。

### 6.7 集成到自定义工具

1. 在你的工具中:
   ```cpp
   MatchFinder Finder;
   Finder.addMatcher(myMatcher, &MyCallback);
   // ...
   ClangTool Tool(db, sources);
   Tool.run(newFrontendActionFactory(&Finder).get());
   ```
2. 见 [`clang/lib/Tooling/`](../Tooling/0-overview.md) (如已有)
   详细 API。

---

## §7. NT 注释索引

当前 `clang/lib/ASTMatchers/` 下尚无 `// <NT>` 注释。已建立目录
索引, 姊妹 overview:

- [`clang/lib/AST/0-overview.md`](../AST/0-overview.md) — Clang AST
  层 (本目录的最大上游消费者)
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
- [`clang/lib/Serialization/0-overview.md`](../Serialization/0-overview.md)
  — Clang AST 持久化

按 "少而精" 原则, 加 NT 注释建议优先级:

1. [`ASTMatchFinder.cpp`](ASTMatchFinder.cpp) — 8-12 段 (MatchFinder
   核心, 整个库入口)
2. [`ASTMatchersInternal.cpp`](ASTMatchersInternal.cpp) — 6-10 段
   (全部 matcher 工厂实现)
3. [`Dynamic/Registry.cpp`](Dynamic/Registry.cpp) — 4-6 段
   (named matcher 查表)
4. [`Dynamic/Parser.cpp`](Dynamic/Parser.cpp) — 4-6 段 (S-表达式
   解析)
5. [`Dynamic/VariantValue.cpp`](Dynamic/VariantValue.cpp) —
   3-4 段 (多态值/matcher)
6. [`LowLevelHelpers.cpp`](LowLevelHelpers.cpp) — 2-3 段
7. [`Dynamic/Marshallers.cpp`](Dynamic/Marshallers.cpp) +
   [`Dynamic/Diagnostics.cpp`](Dynamic/Diagnostics.cpp) — 各 2-3
   段

---

**姊妹文档**: 本目录对应 LLVM 工具链中的 **Clang AST Matcher DSL
层**, 是 clang-tidy / clang-query / static-analyzer / refactoring
工具的共同基础。它与 [`clang/lib/AST/`](../AST/0-overview.md) 配合
通过 `RecursiveASTVisitor` 走 AST 节点, 与
[`clang/lib/Frontend/`](../Frontend/0-overview.md) 配合被装入
`ASTConsumer` 链路, 与 [`clang/lib/Lex/`](../Lex/0-overview.md) 配
合取 token 文本给 `hasName` matcher, 与 LLVM Support 配合用
`llvm::Regex` / `llvm::StringMap` / `llvm::ManagedStatic`。本目录
自身 **不** 实现编译流水线, 仅作为 AST 上的声明式查询层。
