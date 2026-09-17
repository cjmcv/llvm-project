<!-- <NT>overview:clang/lib/CodeGen/ -->

# Clang CodeGen 库导读 — `clang/lib/CodeGen/`

> 本文档梳理 `clang/lib/CodeGen/` 目录下所有源文件 (顶层 114 个 + 子
> 目录 [`TargetBuiltins/`](TargetBuiltins/) 12 个 + [`Targets/`](Targets/)
> 25 个, 共 ~151 个 `.cpp/.h`) 的职责、上下游与推荐阅读顺序。
>
> 目标读者: 想理解 **Clang AST → LLVM IR 编译期** 的开发者, 以及要给
> Clang 加新后端 / 新 builtin / 新 C++ 特性的人。
>
> 所有路径相对 `clang/lib/CodeGen/`。同名头文件位于
> `clang/include/clang/CodeGen/` 和 `clang/include/clang/AST/` 等。

---

## §0. Clang CodeGen 库在编译流水线中的位置

`clang/lib/CodeGen` 是 **Clang AST → LLVM IR** 的代码生成层, 是
**Clang 前端的最后一步** (之后交给 LLVM 后端做优化 + 指令选择 +
汇编/对象写出)。它在 Clang 内部处于 Sema 之后、LLVM 之前的位置。

它**没有**任何词法/语法层, 也不做 LLVM 端优化。它的全部职责是把
`clang::AST::Decl` / `clang::AST::Stmt` / `clang::AST::Expr` /
`clang::AST::Type` (已经经过 Sema 语义检查 + 类型推导) 转成
`llvm::Module` + `llvm::IRBuilder` 调用。

它在流水线中的位置:

```
┌────────────────────────────────────────────────────────┐
│ Clang Lex + Parse + Sema                              │
│   (clang/lib/Lex + clang/lib/Parse + clang/lib/Sema)  │
│   输出: 已类型化的 clang::AST                         │
└────────────────────────────────────────────────────────┘
                │
                ▼
┌────────────────────────────────────────────────────────┐
│ clang/lib/CodeGen  (本目录) ← 你在这里                  │
│   · CodeGenModule.cpp: 每 TU 的全局 codegen 状态       │
│   · CodeGenFunction.cpp: 每个函数体的 stmt emission    │
│   · CodeGenTypes.cpp + CGRecordLayout: AST type → LLVM type │
│   · Targets/*.cpp: ABI / calling-conv (Itanium/MSVC/x86/AArch64/...) │
│   · CGObjC*.cpp / CGCXX*.cpp / CGOpenMPRuntime.cpp:   │
│     ObjC / C++ ABI / OpenMP runtime 调用               │
│   · CGBuiltin.cpp + TargetBuiltins/*.cpp: __builtin_*  │
│   · CodeGenAction.cpp + BackendConsumer.h + BackendUtil.cpp: │
│     FrontendAction 入口 → LLVM 后端对接                │
└────────────────────────────────────────────────────────┘
                │
                ▼
┌────────────────────────────────────────────────────────┐
│ llvm/lib/IR / llvm/lib/CodeGen / llvm/lib/Target       │
│   LLVM 优化 + 指令选择 + MC → 写出 .o/.bc/.ll/.s       │
└────────────────────────────────────────────────────────┘
                │
                ├─→ driver (`clang -emit-obj` → .o 文件)
                ├─→ 链接器 (lld / ld)
                ├─→ debugger / DWARF consumers (llvm-dwarfdump)
                └─→ LTO (libLTO)
```

### §0.1 与 [`llvm/lib/CodeGen/`](../../../llvm/lib/CodeGen/0-overview.md) 的区别

| 维度 | `llvm/lib/CodeGen/` (LLVM 后端) | `clang/lib/CodeGen/` (本目录, 前端) |
|------|--------------------------------|---------------------------------------|
| **输入** | LLVM IR (平台无关) | Clang AST (C/C++/ObjC/HLSL 特定) |
| **输出** | 平台特定汇编 / MC 层 | 平台无关 `llvm::Module` (LLVM IR) |
| **职责** | 指令选择 / 寄存器分配 / 调度 / 窥孔优化 | 类型映射 / ABI / 表达式发射 / 调试信息 |
| **典型类** | `SelectionDAGISel`, `MachineFunction`, `TargetLowering` | `CodeGenFunction`, `CodeGenModule`, `ABIInfo` |

二者通过 [`llvm::Module`](../../../llvm/include/llvm/IR/Module.h) +
[`llvm::IRBuilder`](../../../llvm/include/llvm/IR/IRBuilder.h) +
[`llvm::DataLayout`](../../../llvm/include/llvm/IR/DataLayout.h)
衔接 — Clang CodeGen 只产出 IR, 所有 platform-specific lowering 都由
LLVM 后端负责。

### §0.2 与 `clang/lib/Frontend/` 的协作

`clang/lib/Frontend` 提供 `CompilerInstance` (`clang/lib/Frontend/CompilerInstance.cpp`)
+ `FrontendAction` (`clang/lib/Frontend/FrontendActions.cpp`); 本目录的
[`CodeGenAction`](CodeGenAction.cpp) 是它的子类, 持有
[`BackendConsumer.h`](BackendConsumer.h), 把 ASTConsumer 跟 LLVM
PassManager 串起来。

---

## §1. 编译流水线概览

```
 clang::AST (Decl / Stmt / Type)
   │
   ▼
┌────────────────────────────────────────────────────────┐
│ CodeGenModule::EmitTopLevelDecl (decl)                │
│   · 决定 defer / emit-now                             │
│   · 转发到 CodeGenFunction::EmitDecl                   │
└────────────────────────────────────────────────────────┘
   │
   ▼ (对每个函数)
┌────────────────────────────────────────────────────────┐
│ CodeGenFunction::StartFunction (FunctionDecl)                       │
│   · CodeGenTypes::GetFunctionType: AST signature → LLVM type │
│   · ABIInfo::computeInfo (Targets/<Arch>.cpp): 选 calling-conv │
│   · SetFuncAttributes + CGDebugInfo::EmitFunctionDecl │
└────────────────────────────────────────────────────────┘
   │
   ▼
┌────────────────────────────────────────────────────────┐
│ CodeGenFunction::EmitStmt (Stmt) — StmtVisitor dispatch │
│   · EmitIfStmt / EmitForStmt / EmitCompoundStmt       │
│   · EmitDeclStmt → CodeGenFunction::EmitVarDecl / EmitDecl │
│   · EmitBinaryOperator → ScalarExprEmitter / AggExprEmitter │
│   · EmitCallExpr → EmitCall (经 CGCall 调 ABI / 协变) │
│   · EmitCXXConstructExpr / EmitCXXNewExpr (经 CGCXXABI) │
│   · EmitObjCMessageExpr (经 CGObjCRuntime)            │
│   · EmitOMPParallelDirective / EmitOMPTargetDirective │
└────────────────────────────────────────────────────────┘
   │
   ▼
┌────────────────────────────────────────────────────────┐
│ CodeGenFunction::FinishFunction                        │
│   · 清栈 / 收尾 PHI / 收尾 debug info / PGO counter    │
└────────────────────────────────────────────────────────┘
   │
   ▼
┌────────────────────────────────────────────────────────┐
│ CodeGenAction::ExecuteAction → BackendConsumer        │
│   · BackendUtil::EmitAssemblyHelper: 跑 PassManager   │
│   · 输出 .bc / .ll / .o / .s                          │
└────────────────────────────────────────────────────────┘
```

辅助入口:

- **公共 API**: [`clang/include/clang/CodeGen/`](../../include/clang/CodeGen/)
  提供 `CGFunctionInfo.h`、`CodeGenABITypes.h`、`ModuleBuilder.h`、
  `BackendUtil.h`、`ConstantInitBuilder.h` 等给其它 clang 子库 (Sema /
  Frontend / serialization) 调用。
- **共用 utility**: [`CodeGenTypeCache.h`](CodeGenTypeCache.h) (常用
  LLVM type cache)、[`CGBuilder.h`](CGBuilder.h) (debug-info-aware
  IRBuilder)、[`Address.h`](Address.h) (signed `Address` 包装)。
- **Polymorphism hubs**: `CGObjCRuntime` (Mac / GNU / NonFragile)、
  `CGCXXABI` (Itanium / Microsoft)、`CGOpenMPRuntime` (host /
  `CGOpenMPRuntimeGPU`)、`CGOpenCLRuntime`、`CGHLSLRuntime`、
  `CGCUDARuntime` (NV / AMDGPU)。

---

## §2. 文件目录结构

```
clang/lib/CodeGen/   (114 个顶层文件, 2 个子目录)
├── §3.1  ABI / Calling Conventions       (~10 文件)
├── §3.2  Core IR-Generation Classes      (~8 文件)
├── §3.3  Statement / Expression emission (~9 文件)
├── §3.4  C++ ABI                          (~6 文件)
├── §3.5  C++ Stmts / EH / Coroutines / Blocks (~10 文件)
├── §3.6  Builtins                          (2 文件)
├── §3.7  Debug Info / TBAA                 (4 文件)
├── §3.8  PGO                               (3 文件)
├── §3.9  Objective-C                       (6 文件)
├── §3.10 OpenMP                            (4 文件)
├── §3.11 OpenCL / GPU / CUDA               (5 文件)
├── §3.12 HLSL / DirectX / SPIR-V           (4 文件)
├── §3.13 Constant init / Coverage / Sanitizer / Non-trivial (~12 文件)
├── §3.14 Type cache / Layout / Builder / Address (~9 文件)
├── §3.15 PCH / Module linking              (5 文件)
├── TargetBuiltins/    (12 个 per-arch builtin 文件)
└── Targets/           (25 个 per-arch ABI 文件)
```

### 2.1 文件数量统计

| 区域 | 顶层 .cpp | 顶层 .h | 子目录 .cpp | 合计 |
|------|----------|--------|-----------|------|
| ABI / Calling-Conventions | 5 | 5 | 0 | 10 |
| Core IR-generation | 5 | 3 | 0 | 8 |
| Stmt / Expr emission | 7 | 0 | 0 | 7 |
| C++ ABI | 5 | 2 | 0 | 7 |
| C++ Stmts / EH / Coroutines / Blocks | 6 | 5 | 0 | 11 |
| Builtins | 2 | 1 | 0 | 3 |
| Debug Info / TBAA | 4 | 2 | 0 | 6 |
| PGO | 2 | 2 | 0 | 4 |
| Objective-C | 4 | 2 | 0 | 6 |
| OpenMP | 2 | 2 | 0 | 4 |
| OpenCL / GPU / CUDA | 4 | 3 | 0 | 7 |
| HLSL / DirectX / SPIR-V | 4 | 1 | 0 | 5 |
| Constant init / Coverage / Sanitizer | 8 | 6 | 0 | 14 |
| Type cache / Layout / Builder | 6 | 5 | 0 | 11 |
| PCH / Module linking | 4 | 1 | 0 | 5 |
| Misc | 1 (.txt + pch.h) | 0 | 0 | 1 |
| `TargetBuiltins/` 子目录 | 0 | 0 | 12 | 12 |
| `Targets/` 子目录 | 0 | 0 | 25 | 25 |
| **总计** | ~69 | **~46** | **37** | **~151** |

---

## §3. 文件详解

### 3.1 ABI / Calling Conventions

[`ABIInfo.h`](ABIInfo.h) — `ABIInfo` + `SwiftABIInfo` 抽象基类;
target-specific hooks for arg/retval 分类。
- 上游: `ABIInfo.cpp`, [`Targets/X86.cpp`](Targets/X86.cpp),
  [`Targets/AArch64.cpp`](Targets/AArch64.cpp),
  [`Targets/ARM.cpp`](Targets/ARM.cpp),
  [`MicrosoftCXXABI.cpp`](MicrosoftCXXABI.cpp)。
- 下游: [`clang/AST/Attr.h`](../../include/clang/AST/Attr.h),
  [`clang/AST/Type.h`](../../include/clang/AST/Type.h),
  [`llvm/IR/CallingConv.h`](../../../llvm/include/llvm/IR/CallingConv.h)。
- 关键类/函数: `class ABIInfo`, `class SwiftABIInfo`, `computeInfo`,
  `EmitVAArg`, `EmitMSVAArg`, `isHomogeneousAggregate`。

[`ABIInfo.cpp`](ABIInfo.cpp) — `ABIInfo` / `SwiftABIInfo` 默认方法实现
(HFA / VAArg / attribute mangling)。
- 上游: 所有 `Targets/*.cpp`; [`CGCall.cpp`](CGCall.cpp),
  [`CodeGenTypes.cpp`](CodeGenTypes.cpp)。
- 下游: `ABIInfo.h`, [`ABIInfoImpl.h`](ABIInfoImpl.h),
  [`clang/Basic/TargetInfo.h`](../../include/clang/Basic/TargetInfo.h)。
- 关键类/函数: `ABIInfo::getCXXABI`,
  `isHomogeneousAggregate`,
  `isZeroLengthBitfieldPermittedInHomogeneousAggregate`,
  `appendAttributeMangling`, `getOptimalVectorMemoryType`。

[`ABIInfoImpl.h`](ABIInfoImpl.h) — `DefaultABIInfo` + 聚合/空记录
predicate, 各 ABI target 共用 helper。
- 上游: `ABIInfo.cpp`, [`CGCall.cpp`](CGCall.cpp),
  [`CGExpr.cpp`](CGExpr.cpp), [`CGExprConstant.cpp`](CGExprConstant.cpp),
  [`CGClass.cpp`](CGClass.cpp), [`CodeGenPGO.cpp`](CodeGenPGO.cpp),
  所有 [`Targets/*.cpp`](Targets/)。
- 下游: `ABIInfo.h`, [`CGCXXABI.h`](CGCXXABI.h),
  [`clang/AST/RecordLayout.h`](../../include/clang/AST/RecordLayout.h)。
- 关键类/函数: `class DefaultABIInfo`, `isAggregateTypeForABI`,
  `isEmptyField`, `isEmptyRecord`, `isSingleElementStruct`,
  `emitVoidPtrDirectVAArg`, `emitVoidPtrVAArg`。

[`ABIInfoImpl.cpp`](ABIInfoImpl.cpp) — `DefaultABIInfo` 与 helper
predicates 的实现 (通用的 lowering 规则核心)。
- 上游: `ABIInfo.cpp`, [`CGCall.cpp`](CGCall.cpp),
  所有 [`Targets/*.cpp`](Targets/) (子类 fallback)。
- 下游: `ABIInfoImpl.h`, [`clang/AST/ASTContext.h`](../../include/clang/AST/ASTContext.h),
  [`llvm/IR/DataLayout.h`](../../../llvm/include/llvm/IR/DataLayout.h)。
- 关键类/函数: `DefaultABIInfo::computeInfo`,
  `classifyArgumentType`, `classifyReturnType`, `EmitVAArg`,
  `useFirstFieldIfTransparentUnion`。

[`CodeGenABITypes.cpp`](CodeGenABITypes.cpp) — 公开 API 包装器
([`CodeGenABITypes.h`](../../include/clang/CodeGen/CodeGenABITypes.h)),
给 Sema 等其它子库用。
- 上游: [`CodeGenModule.cpp`](CodeGenModule.cpp); 通过
  [`clang/CodeGen/CodeGenABITypes.h`](../../include/clang/CodeGen/CodeGenABITypes.h)
  给 Sema 调用。
- 下游: [`CGCXXABI.h`](CGCXXABI.h), [`CGRecordLayout.h`](CGRecordLayout.h),
  [`CodeGenFunction.h`](CodeGenFunction.h),
  [`CodeGenModule.h`](CodeGenModule.h)。
- 关键类/函数: `arrangeFreeFunctionType`, `arrangeCXXMethodType`,
  `arrangeCXXMethodCall`, `arrangeObjCMessageSendSignature`,
  `getImplicitCXXConstructorArgs`。

[`CGCall.cpp`](CGCall.cpp) + [`CGCall.h`](CGCall.h) — calling-conv 机制:
`CGFunctionInfo`、`CallArgList`、`CGCallee`、attribute emission、
`ABIArgInfo` 接线 (最大的 ABI 文件)。
- 上游: [`CodeGenFunction.cpp`](CodeGenFunction.cpp),
  [`CodeGenModule.cpp`](CodeGenModule.cpp),
  [`CGVTables.cpp`](CGVTables.cpp), [`CGClass.cpp`](CGClass.cpp),
  [`CGExpr.cpp`](CGExpr.cpp), 每个 emitter 调 call 都用它。
- 下游: `ABIInfo.h`, `ABIInfoImpl.h`, [`CGBlocks.h`](CGBlocks.h),
  [`CGCXXABI.h`](CGCXXABI.h), [`CGCleanup.h`](CGCleanup.h),
  [`CGDebugInfo.h`](CGDebugInfo.h), [`CGRecordLayout.h`](CGRecordLayout.h),
  [`clang/CodeGen/CGFunctionInfo.h`](../../include/clang/CodeGen/CGFunctionInfo.h),
  [`clang/CodeGen/SwiftCallingConv.h`](../../include/clang/CodeGen/SwiftCallingConv.h),
  [`llvm/ABI/TargetInfo.h`](../../../llvm/include/llvm/ABI/TargetInfo.h),
  [`llvm/IR/Attributes.h`](../../../llvm/include/llvm/IR/Attributes.h)。
- 关键类/函数: `class CGFunctionInfo`, `class CGCallee`,
  `class CallArgList`, `CodeGenTypes::arrangeLLVMFunctionInfo`,
  `ClangCallConvToLLVMCallConv`, `class ReturnValueSlot`。

[`SwiftCallingConv.cpp`](SwiftCallingConv.cpp) — Swift 调用约定的抽象
降低 (record expansion / swifterror / vector legality), 任何 opt-in 的
  target 都能用。
- 上游: `ABIInfo.cpp` (间接); 通过
  `CGM.getTargetCodeGenInfo().getSwiftABIInfo()` 调。
- 下游: [`clang/CodeGen/SwiftCallingConv.h`](../../include/clang/CodeGen/SwiftCallingConv.h),
  `ABIInfo.h`, [`CodeGenModule.h`](CodeGenModule.h),
  [`TargetInfo.h`](TargetInfo.h)。
- 关键类/函数: `class SwiftAggLowering`, `addTypedData`,
  `shouldPassIndirectly`, `isLegalVectorType`, `getCommonType`。

[`CGPointerAuth.cpp`](CGPointerAuth.cpp) + [`CGPointerAuthInfo.h`](CGPointerAuthInfo.h)
— pointer-auth discriminator + Siphash-based stable hash。
- 上游: `CGCall.cpp`, [`CodeGenFunction.cpp`](CodeGenFunction.cpp),
  签指针的 target 文件 (Darwin / AArch64)。
- 下游: [`CGCXXABI.h`](CGCXXABI.h),
  [`CodeGenModule.h`](CodeGenModule.h),
  [`clang/CodeGen/ConstantInitBuilder.h`](../../include/clang/CodeGen/ConstantInitBuilder.h),
  [`llvm/Support/SipHash.h`](../../../llvm/include/llvm/Support/SipHash.h)。
- 关键类/函数: `class CGPointerAuthInfo`,
  `CodeGenModule::getPointerAuthOtherDiscriminator`,
  `getPointerAuthDeclDiscriminator`, `PtrAuthDiscriminatorHashes`。

### 3.2 Core IR-Generation Classes

[`CodeGenFunction.cpp`](CodeGenFunction.cpp) + [`CodeGenFunction.h`](CodeGenFunction.h)
— per-function codegen 状态, function prologue/epilogue、stmt emission
入口 (最大的单文件)。
- 上游: [`CodeGenModule.cpp`](CodeGenModule.cpp),
  [`ModuleBuilder.cpp`](ModuleBuilder.cpp),
  [`BackendUtil.cpp`](BackendUtil.cpp), 每个 `CG*.cpp` emitter。
- 下游: [`CGBlocks.h`](CGBlocks.h),
  [`CGCUDARuntime.h`](CGCUDARuntime.h), [`CGCXXABI.h`](CGCXXABI.h),
  [`CGCleanup.h`](CGCleanup.h), [`CGDebugInfo.h`](CGDebugInfo.h),
  [`CGHLSLRuntime.h`](CGHLSLRuntime.h),
  [`CGOpenMPRuntime.h`](CGOpenMPRuntime.h),
  [`CodeGenModule.h`](CodeGenModule.h),
  [`CodeGenPGO.h`](CodeGenPGO.h), [`CodeGenTypes.h`](CodeGenTypes.h),
  [`TargetInfo.h`](TargetInfo.h),
  [`llvm/Frontend/OpenMP/OMPIRBuilder.h`](../../../llvm/include/llvm/Frontend/OpenMP/OMPIRBuilder.h),
  [`llvm/IR/Intrinsics.h`](../../../llvm/include/llvm/IR/Intrinsics.h)。
- 关键类/函数: `class CodeGenFunction`,
  `class CGCapturedStmtInfo`, `class RunCleanupsScope`, `LexicalScope`,
  `JumpDest`, `EmitStmt`, `EmitDecl`, `EmitCall`, `StartFunction`,
  `FinishFunction`, `CGFPOptionsRAII`。

[`CodeGenModule.cpp`](CodeGenModule.cpp) + [`CodeGenModule.h`](CodeGenModule.h)
— per-translation-unit 状态: globals、function emission、mangling、
attribute defaults (中心 hub)。
- 上游: [`ModuleBuilder.cpp`](ModuleBuilder.cpp),
  [`CodeGenAction.cpp`](CodeGenAction.cpp),
  `BackendConsumer.cpp`,
  [`BackendUtil.cpp`](BackendUtil.cpp),
  [`ObjectFilePCHContainerWriter.cpp`](ObjectFilePCHContainerWriter.cpp)。
- 下游: 所有 `CG*.h` 和 `Targets/*.cpp`,
  [`clang/AST/Mangle.h`](../../include/clang/AST/Mangle.h),
  [`clang/CodeGen/BackendUtil.h`](../../include/clang/CodeGen/BackendUtil.h),
  [`clang/CodeGen/ConstantInitBuilder.h`](../../include/clang/CodeGen/ConstantInitBuilder.h),
  [`clang/Frontend/CompilerInstance.h`](../../include/clang/Frontend/CompilerInstance.h),
  [`llvm/ABI/IRTypeMapper.h`](../../../llvm/include/llvm/ABI/IRTypeMapper.h),
  [`llvm/IR/Module.h`](../../../llvm/include/llvm/IR/Module.h),
  [`llvm/TargetParser/Triple.h`](../../../llvm/include/llvm/TargetParser/Triple.h)。
- 关键类/函数: `class CodeGenModule`, `ForDefinition_t`, `CounterPair`,
  `ObjCEntrypoints`, `OrderGlobalInitsOrStermFinalizers`,
  `EmitGlobal`, `EmitFunctionDefinition`, `EmitTopLevelDecl`,
  `getMangledName`。

[`CodeGenTypes.cpp`](CodeGenTypes.cpp) + [`CodeGenTypes.h`](CodeGenTypes.h)
— AST → LLVM type 降低, function-type arrangement, record-layout cache
(`CodeGenModule` 的姊妹)。
- 上游: [`CodeGenModule.cpp`](CodeGenModule.cpp),
  [`CodeGenFunction.cpp`](CodeGenFunction.cpp),
  [`CGCall.cpp`](CGCall.cpp), [`CGExpr*.cpp`](CGExpr.cpp),
  [`CGDecl.cpp`](CGDecl.cpp)。
- 下游: [`CGCXXABI.h`](CGCXXABI.h), [`CGCall.h`](CGCall.h),
  [`CGDebugInfo.h`](CGDebugInfo.h),
  [`CGOpenCLRuntime.h`](CGOpenCLRuntime.h),
  [`CGRecordLayout.h`](CGRecordLayout.h),
  [`clang/CodeGen/CGFunctionInfo.h`](../../include/clang/CodeGen/CGFunctionInfo.h),
  [`llvm/IR/DataLayout.h`](../../../llvm/include/llvm/IR/DataLayout.h),
  [`llvm/IR/DerivedTypes.h`](../../../llvm/include/llvm/IR/DerivedTypes.h)。
- 关键类/函数: `class CodeGenTypes`, `ConvertType`,
  `ConvertTypeForMem`, `convertTypeForLoadStore`,
  `GetFunctionType`, `typeRequiresSplitIntoByteArray`,
  `RefreshTypeCacheForClass`。

[`CodeGenTypeCache.h`](CodeGenTypeCache.h) — POD, 缓存常用 LLVM type
(`VoidTy`、`Int8Ty..Int64Ty`、pointer variants、`IntPtrTy`、size/align),
按值复制进每个 `CodeGenFunction`。
- 上游: [`CodeGenFunction.h`](CodeGenFunction.h) (成员),
  [`CGBuilder.h`](CGBuilder.h), [`CGLoopInfo.h`](CGLoopInfo.h),
  [`CodeGenPGO.h`](CodeGenPGO.h), [`TargetInfo.h`](TargetInfo.h)。
- 下游: [`clang/AST/CharUnits.h`](../../include/clang/AST/CharUnits.h),
  [`clang/Basic/AddressSpaces.h`](../../include/clang/Basic/AddressSpaces.h),
  [`llvm/IR/CallingConv.h`](../../../llvm/include/llvm/IR/CallingConv.h)。
- 关键类/函数: `struct CodeGenTypeCache`, `RuntimeCC`,
  `getIntSize`, `getPointerAlign`, `getSizeSize`, `DefaultPtrTy`。

[`ModuleBuilder.cpp`](ModuleBuilder.cpp) — `CodeGeneratorImpl`, 驱动
AST→IR 经 `HandleTopLevelDecl`/`HandleTagDecl`, 延迟 inline 定义
(`ASTConsumer` 前端 hook)。
- 上游: [`CodeGenAction.cpp`](CodeGenAction.cpp) (实例化 `CodeGenerator`)。
- 下游: [`CGCXXABI.h`](CGCXXABI.h), [`CGDebugInfo.h`](CGDebugInfo.h),
  [`CodeGenModule.h`](CodeGenModule.h),
  [`clang/Frontend/CompilerInstance.h`](../../include/clang/Frontend/CompilerInstance.h),
  [`llvm/IR/LLVMContext.h`](../../../llvm/include/llvm/IR/LLVMContext.h),
  [`llvm/IR/Module.h`](../../../llvm/include/llvm/IR/Module.h)。
- 关键类/函数: `class CodeGeneratorImpl`,
  `HandlingTopLevelDeclRAII`, `EmitDeferredDecls`, `Initialize`,
  `HandleInterestingDecl`, `CompleteTentativeDefinition`。

[`CodeGenAction.cpp`](CodeGenAction.cpp) — `CodeGenAction`,
`clang::FrontendAction` 子类, 拥有 `BackendConsumer`, 驱动 codegen
pipeline, 跑 backend pass。
- 上游: Driver (`clang::EmitObjAction`, `EmitBCAction` 等)。
- 下游: [`BackendConsumer.h`](BackendConsumer.h), [`CGCall.h`](CGCall.h),
  [`CodeGenModule.h`](CodeGenModule.h),
  [`CoverageMappingGen.h`](CoverageMappingGen.h),
  [`MacroPPCallbacks.h`](MacroPPCallbacks.h),
  [`clang/CodeGen/BackendUtil.h`](../../include/clang/CodeGen/BackendUtil.h),
  [`clang/CodeGen/ModuleBuilder.h`](../../include/clang/CodeGen/ModuleBuilder.h),
  [`llvm/LTO/LTOBackend.h`](../../../llvm/include/llvm/LTO/LTOBackend.h),
  [`llvm/Linker/Linker.h`](../../../llvm/include/llvm/Linker/Linker.h),
  [`llvm/IR/Verifier.h`](../../../llvm/include/llvm/IR/Verifier.h)。
- 关键类/函数: `class CodeGenAction`, `ExecuteAction`,
  `class ClangDiagnosticHandler`, `reportOptRecordError`。

[`BackendUtil.cpp`](BackendUtil.cpp) — 构造 `TargetMachine`, 配置 pass
managers, 跑 sanitizer/PGO/ThinLTO/HipStdPar passes, 写出
bitcode/assembly/object。
- 上游: `BackendConsumer.cpp`,
  [`CodeGenAction.cpp`](CodeGenAction.cpp)。
- 下游: [`BackendConsumer.h`](BackendConsumer.h),
  [`LinkInModulesPass.h`](LinkInModulesPass.h),
  [`clang/Frontend/Utils.h`](../../include/clang/Frontend/Utils.h),
  [`llvm/Passes/PassBuilder.h`](../../../llvm/include/llvm/Passes/PassBuilder.h),
  [`llvm/Target/TargetMachine.h`](../../../llvm/include/llvm/Target/TargetMachine.h),
  每个 [`llvm/Transforms/`](../../../llvm/include/llvm/Transforms/)
  子目录的 .h。
- 关键类/函数: `EmitAssemblyHelper`, `RunPasses`,
  `setupTargetMachine`, `EmbedBitcode`, `ThinLTOBitcodeWriter`,
  `registerSanitizers`。

[`BackendConsumer.h`](BackendConsumer.h) — `class BackendConsumer`, 是
`ASTConsumer`, 拥有 `CodeGenerator`, 把 LLVM 诊断翻译回 Clang source
locations。
- 上游: [`CodeGenAction.cpp`](CodeGenAction.cpp),
  [`BackendUtil.cpp`](BackendUtil.cpp),
  [`LinkInModulesPass.cpp`](LinkInModulesPass.cpp)。
- 下游: [`clang/CodeGen/BackendUtil.h`](../../include/clang/CodeGen/BackendUtil.h),
  [`clang/CodeGen/CodeGenAction.h`](../../include/clang/CodeGen/CodeGenAction.h),
  [`clang/CodeGen/ModuleLinker.h`](../../include/clang/CodeGen/ModuleLinker.h),
  [`llvm/IR/DiagnosticInfo.h`](../../../llvm/include/llvm/IR/DiagnosticInfo.h)。
- 关键类/函数: `class BackendConsumer`, `LinkInModules`,
  `DiagnosticHandlerImpl`, `OptimizationRemarkHandler`,
  `InlineAsmDiagHandler`, `getBestLocationFromDebugLoc`。

### 3.3 Statement / Expression emission

[`CGStmt.cpp`](CGStmt.cpp) — `EmitStmt` 顶层分发, 覆盖 compound/
switch/for/while/do/return/label/goto/asm。
- 上游: [`CodeGenFunction.cpp`](CodeGenFunction.cpp),
  [`CGCoroutine.cpp`](CGCoroutine.cpp),
  [`CGStmtOpenMP.cpp`](CGStmtOpenMP.cpp)。
- 下游: [`CGDebugInfo.h`](CGDebugInfo.h),
  [`CGOpenMPRuntime.h`](CGOpenMPRuntime.h),
  [`CodeGenFunction.h`](CodeGenFunction.h),
  [`CodeGenModule.h`](CodeGenModule.h),
  [`CodeGenPGO.h`](CodeGenPGO.h), [`TargetInfo.h`](TargetInfo.h),
  [`clang/AST/StmtVisitor.h`](../../include/clang/AST/StmtVisitor.h)。
- 关键类/函数: `CodeGenFunction::EmitStmt`, `EmitCompoundStmt`,
  `EmitIfStmt`, `EmitSwitchStmt`, `EmitAsmStmt`, `EmitLabelStmt`,
  `EmitStopPoint`, `EmitAttributedStmt`。

[`CGStmtOpenMP.cpp`](CGStmtOpenMP.cpp) — OpenMP construct emission
(parallel/for/sections/distribute/target/simd/cancel/teams/loop)。
- 上游: [`CGStmt.cpp`](CGStmt.cpp) (经 `EmitStmt`)。
- 下游: [`CGCleanup.h`](CGCleanup.h),
  [`CGDebugInfo.h`](CGDebugInfo.h),
  [`CGOpenMPRuntime.h`](CGOpenMPRuntime.h),
  [`CodeGenFunction.h`](CodeGenFunction.h),
  [`CodeGenModule.h`](CodeGenModule.h),
  [`CodeGenPGO.h`](CodeGenPGO.h), [`TargetInfo.h`](TargetInfo.h),
  [`clang/AST/StmtOpenMP.h`](../../include/clang/AST/StmtOpenMP.h),
  [`clang/AST/OpenMPClause.h`](../../include/clang/AST/OpenMPClause.h),
  [`llvm/Frontend/OpenMP/OMPIRBuilder.h`](../../../llvm/include/llvm/Frontend/OpenMP/OMPIRBuilder.h)。
- 关键类/函数: `class OMPLexicalScope`, `class OMPTeamsScope`,
  `EmitOMPParallelForSimdDirective`,
  `EmitOMPTargetTeamsDistributeDirective`,
  `canEmitGPUFusedDistSchedule`, `emitOMPAtomicExpr`。

[`CGExpr.cpp`](CGExpr.cpp) — 通用 expression emitter, ObjC box/string、
property、sanitizer-coverage 发射点、`OpaqueValueExpr`。
- 上游: [`CGExprScalar.cpp`](CGExprScalar.cpp),
  [`CGExprAgg.cpp`](CGExprAgg.cpp),
  [`CGExprComplex.cpp`](CGExprComplex.cpp),
  [`CGExprCXX.cpp`](CGExprCXX.cpp),
  [`CGExprConstant.cpp`](CGExprConstant.cpp)。
- 下游: [`ABIInfoImpl.h`](ABIInfoImpl.h),
  [`CGCUDARuntime.h`](CGCUDARuntime.h),
  [`CGCXXABI.h`](CGCXXABI.h), [`CGCall.h`](CGCall.h),
  [`CGCleanup.h`](CGCleanup.h),
  [`CGDebugInfo.h`](CGDebugInfo.h),
  [`CGHLSLRuntime.h`](CGHLSLRuntime.h),
  [`CGObjCRuntime.h`](CGObjCRuntime.h),
  [`CGOpenMPRuntime.h`](CGOpenMPRuntime.h),
  [`CGRecordLayout.h`](CGRecordLayout.h),
  [`CodeGenFunction.h`](CodeGenFunction.h),
  [`CodeGenModule.h`](CodeGenModule.h),
  [`CodeGenPGO.h`](CodeGenPGO.h),
  [`ConstantEmitter.h`](ConstantEmitter.h),
  [`TargetInfo.h`](TargetInfo.h)。
- 关键类/函数: `EmitAnyExpr`, `EmitDeclRefExpr`, `EmitCallExpr`,
  `EmitObjCBoxedExpr`, `EmitObjCStringLiteral`, `EmitMemberExpr`,
  `EmitCastExpr`, `EmitOpaqueValueExpr`, `EmitDecltypeTypeExpr`。

[`CGExprAgg.cpp`](CGExprAgg.cpp) — aggregate-typed expression emission
(struct/array/union init/copy、comma、init-list、paren)。
- 上游: [`CGExpr.cpp`](CGExpr.cpp), [`CGDecl.cpp`](CGDecl.cpp),
  [`CGClass.cpp`](CGClass.cpp), [`CGStmt.cpp`](CGStmt.cpp)。
- 下游: [`CGCXXABI.h`](CGCXXABI.h), [`CGDebugInfo.h`](CGDebugInfo.h),
  [`CGHLSLRuntime.h`](CGHLSLRuntime.h),
  [`CGObjCRuntime.h`](CGObjCRuntime.h),
  [`CGRecordLayout.h`](CGRecordLayout.h),
  [`CodeGenFunction.h`](CodeGenFunction.h),
  [`CodeGenModule.h`](CodeGenModule.h),
  [`ConstantEmitter.h`](ConstantEmitter.h),
  [`EHScopeStack.h`](EHScopeStack.h), [`TargetInfo.h`](TargetInfo.h)。
- 关键类/函数: `class AggExprEmitter`, `EmitAggExpr`,
  `EmitAggregateCopy`, `EmitInitListExpr`,
  `EmitConditionalOperator`, `EmitMoveOrCopyOption`,
  `DoZeroInitPadding`。

[`CGExprCXX.cpp`](CGExprCXX.cpp) — C++ expression emission (new/delete、
member call、this、typeid、throw、virt call)。
- 上游: [`CGExpr.cpp`](CGExpr.cpp) (经 `StmtVisitor` 分发)。
- 下游: [`CGCUDARuntime.h`](CGCUDARuntime.h),
  [`CGCXXABI.h`](CGCXXABI.h), [`CGDebugInfo.h`](CGDebugInfo.h),
  [`CGObjCRuntime.h`](CGObjCRuntime.h),
  [`CodeGenFunction.h`](CodeGenFunction.h),
  [`ConstantEmitter.h`](ConstantEmitter.h),
  [`TargetInfo.h`](TargetInfo.h),
  [`clang/CodeGen/CGFunctionInfo.h`](../../include/clang/CodeGen/CGFunctionInfo.h),
  [`llvm/IR/Intrinsics.h`](../../../llvm/include/llvm/IR/Intrinsics.h)。
- 关键类/函数: `EmitCXXNewExpr`, `EmitCXXDeleteExpr`,
  `EmitCXXConstructExpr`, `EmitCXXMemberCallExpr`,
  `EmitCXXOperatorCallExpr`, `EmitCXXTypeidExpr`, `EmitCXXThrowExpr`,
  `commonEmitCXXMemberOrOperatorCall`, `EmitLambdaExpr`。

[`CGExprComplex.cpp`](CGExprComplex.cpp) — complex-typed expression
emission (`_Complex`、complex ops、complex load/store)。
- 上游: [`CGExpr.cpp`](CGExpr.cpp) (TEK_Complex 分发)。
- 下游: [`CGDebugInfo.h`](CGDebugInfo.h),
  [`CGOpenMPRuntime.h`](CGOpenMPRuntime.h),
  [`CodeGenFunction.h`](CodeGenFunction.h),
  [`CodeGenModule.h`](CodeGenModule.h),
  [`ConstantEmitter.h`](ConstantEmitter.h),
  [`clang/AST/StmtVisitor.h`](../../include/clang/AST/StmtVisitor.h),
  [`llvm/IR/Constants.h`](../../../llvm/include/llvm/IR/Constants.h)。
- 关键类/函数: `class ComplexExprEmitter`, `EmitComplexExpr`,
  `EmitComplexBinOp`, `EmitComplexConditionalOperator`,
  `EmitCastToComplex`。

[`CGExprConstant.cpp`](CGExprConstant.cpp) — constant-expression
emission: globals 初始化器、`constexpr`、designated initializer、
matrix constants。
- 上游: [`CGExpr.cpp`](CGExpr.cpp), [`CGDecl.cpp`](CGDecl.cpp),
  [`CGDeclCXX.cpp`](CGDeclCXX.cpp), [`CGVTables.cpp`](CGVTables.cpp)。
- 下游: [`ABIInfoImpl.h`](ABIInfoImpl.h),
  [`CGCXXABI.h`](CGCXXABI.h),
  [`CGObjCRuntime.h`](CGObjCRuntime.h),
  [`CGRecordLayout.h`](CGRecordLayout.h),
  [`CodeGenFunction.h`](CodeGenFunction.h),
  [`CodeGenModule.h`](CodeGenModule.h),
  [`ConstantEmitter.h`](ConstantEmitter.h),
  [`TargetInfo.h`](TargetInfo.h),
  [`clang/AST/APValue.h`](../../include/clang/AST/APValue.h),
  [`llvm/Analysis/ConstantFolding.h`](../../../llvm/include/llvm/Analysis/ConstantFolding.h)。
- 关键类/函数: `class ConstExprEmitter`,
  `class ConstantAggregateBuilder`, `EmitConstantExpr`,
  `EmitArrayConstant`, `EmitStructConstant`,
  `EmitCompoundLiteralExpr`, `getPadding`,
  `ConstantAggregateBuilderUtils`。

[`CGExprScalar.cpp`](CGExprScalar.cpp) — scalar-typed expression emission,
所有 binary/unary、cast、reference、fixed-point、ternary。
- 上游: [`CGExpr.cpp`](CGExpr.cpp) (TEK_Scalar 分发)。
- 下游: [`CGCXXABI.h`](CGCXXABI.h), [`CGCleanup.h`](CGCleanup.h),
  [`CGDebugInfo.h`](CGDebugInfo.h),
  [`CGHLSLRuntime.h`](CGHLSLRuntime.h),
  [`CGObjCRuntime.h`](CGObjCRuntime.h),
  [`CGOpenMPRuntime.h`](CGOpenMPRuntime.h),
  [`CGRecordLayout.h`](CGRecordLayout.h),
  [`CodeGenFunction.h`](CodeGenFunction.h),
  [`CodeGenModule.h`](CodeGenModule.h),
  [`ConstantEmitter.h`](ConstantEmitter.h),
  [`TargetInfo.h`](TargetInfo.h),
  [`TrapReasonBuilder.h`](TrapReasonBuilder.h),
  [`llvm/IR/Constants.h`](../../../llvm/include/llvm/IR/Constants.h),
  [`llvm/IR/Intrinsics*.h`](../../../llvm/include/llvm/IR/Intrinsics.h)。
- 关键类/函数: `class ScalarExprEmitter`, `EmitScalarExpr`,
  `EmitBinOp`, `EmitUnaryOp`, `EmitCastExpr`, `EmitVectorSubscriptExpr`,
  `EmitMatrixExpr`, `mayHaveIntegerOverflow`, `EmitFixedPointBinOp`。

[`CGNonTrivialStruct.cpp`](CGNonTrivialStruct.cpp) — 给 C struct 生成
默认 copy/move/destructor/compare (`NonTrivialTypeVisitor`)。
- 上游: [`CodeGenModule.cpp`](CodeGenModule.cpp)
  (default-strategy 查询)。
- 下游: [`CGDebugInfo.h`](CGDebugInfo.h),
  [`CodeGenFunction.h`](CodeGenFunction.h),
  [`CodeGenModule.h`](CodeGenModule.h),
  [`clang/AST/NonTrivialTypeVisitor.h`](../../include/clang/AST/NonTrivialTypeVisitor.h),
  [`clang/CodeGen/CodeGenABITypes.h`](../../include/clang/CodeGen/CodeGenABITypes.h)。
- 关键类/函数: `struct StructVisitor`, `visitStructFields`,
  `visitTrivial`, `emitMemcpyForNonTrivialCopy`, `emitDestroy`,
  `EmitNonTrivialCStructMemberCopy`。

### 3.4 C++ ABI

[`CGCXX.cpp`](CGCXX.cpp) — C++-specific declaration emission
(alias-of-base-dtor opt、vtables for non-ABI work、key functions)。
- 上游: [`CodeGenModule.cpp`](CodeGenModule.cpp)。
- 下游: [`CGCXXABI.h`](CGCXXABI.h),
  [`CodeGenFunction.h`](CodeGenFunction.h),
  [`CodeGenModule.h`](CodeGenModule.h),
  [`clang/AST/Mangle.h`](../../include/clang/AST/Mangle.h),
  [`llvm/IR/IRBuilder.h`](../../../llvm/include/llvm/IR/IRBuilder.h)。
- 关键类/函数:
  `CodeGenModule::TryEmitBaseDestructorAsAlias`, `TryEmitDefinition`,
  `EmitNamespace`, `EmitLinkageSpec`,
  `GetOrCreateMSStringMangler`。

[`CGCXXABI.h`](CGCXXABI.h) + [`CGCXXABI.cpp`](CGCXXABI.cpp) — 抽象
`class CGCXXABI` 接口 + 通用 member-pointer / this-arg helper,
Itanium vs Microsoft 的多态点。
- 上游: [`CodeGenModule.cpp`](CodeGenModule.cpp),
  [`CGCXX.cpp`](CGCXX.cpp), [`CGClass.cpp`](CGClass.cpp),
  [`CGDeclCXX.cpp`](CGDeclCXX.cpp), [`CGVTT.cpp`](CGVTT.cpp),
  [`CGVTables.cpp`](CGVTables.cpp), 所有需要 ABI hook 的 emitter。
- 下游: [`CodeGenFunction.h`](CodeGenFunction.h),
  [`clang/CodeGen/CodeGenABITypes.h`](../../include/clang/CodeGen/CodeGenABITypes.h),
  [`clang/AST/Type.h`](../../include/clang/AST/Type.h),
  [`llvm/IR/Value.h`](../../../llvm/include/llvm/IR/Value.h)。
- 关键类/函数: `class CGCXXABI`, `RecordArgABI`,
  `isThisCompleteObject`, `GetBogusMemberPointer`,
  `EmitLoadOfMemberFunctionPointer`,
  `EmitMemberDataPointerAddress`, `EmitMemberPointerConversion`,
  `getThisArgumentTypeForMethod`。

[`ItaniumCXXABI.cpp`](ItaniumCXXABI.cpp) — 具体 Itanium-ABI lowering:
vtable layout、VTT、guard variables、thread wrappers、ARM
method-ptr ABI。
- 上游: [`CodeGenModule.cpp`](CodeGenModule.cpp) (在
  `createItaniumCXXABI` 工厂)。
- 下游: [`CGCXXABI.h`](CGCXXABI.h), [`CGCleanup.h`](CGCleanup.h),
  [`CGDebugInfo.h`](CGDebugInfo.h),
  [`CGRecordLayout.h`](CGRecordLayout.h),
  [`CGVTables.h`](CGVTables.h),
  [`CodeGenFunction.h`](CodeGenFunction.h),
  [`CodeGenModule.h`](CodeGenModule.h),
  [`TargetInfo.h`](TargetInfo.h),
  [`clang/AST/Mangle.h`](../../include/clang/AST/Mangle.h),
  [`clang/Basic/PointerAuthOptions.h`](../../include/clang/Basic/PointerAuthOptions.h),
  [`clang/CodeGen/ConstantInitBuilder.h`](../../include/clang/CodeGen/ConstantInitBuilder.h),
  [`llvm/IR/GlobalValue.h`](../../../llvm/include/llvm/IR/GlobalValue.h),
  [`llvm/Support/ConvertEBCDIC.h`](../../../llvm/include/llvm/Support/ConvertEBCDIC.h)。
- 关键类/函数: `class ItaniumCXXABI`,
  `class ItaniumRTTIBuilder`, `UseARMMethodPtrABI`,
  `UseARMGuardVarABI`, `Use32BitVTableOffsetABI`, `BuildVTT`,
  `EmitVTable`, `EmitThreadLocalInitFunc`。

[`MicrosoftCXXABI.cpp`](MicrosoftCXXABI.cpp) — MSVC ABI: vbtable、
complete-object locators、catchable types、throwinfo、vfptr layout、
ctor/dtor returning-this。
- 上游: [`CodeGenModule.cpp`](CodeGenModule.cpp) (MSVC ABI 选中时的
  工厂)。
- 下游: `ABIInfo.h`, [`CGCXXABI.h`](CGCXXABI.h),
  [`CGCleanup.h`](CGCleanup.h),
  [`CGDebugInfo.h`](CGDebugInfo.h),
  [`CGVTables.h`](CGVTables.h),
  [`CodeGenModule.h`](CodeGenModule.h),
  [`CodeGenTypes.h`](CodeGenTypes.h),
  [`TargetInfo.h`](TargetInfo.h),
  [`clang/AST/VTableBuilder.h`](../../include/clang/AST/VTableBuilder.h),
  [`clang/CodeGen/ConstantInitBuilder.h`](../../include/clang/CodeGen/ConstantInitBuilder.h)。
- 关键类/函数: `class MicrosoftCXXABI`, `struct VBTableGlobals`,
  `BaseClassDescriptorType`, `ClassHierarchyDescriptorType`,
  `CompleteObjectLocatorType`, `CatchableTypeType`,
  `ThrowInfoType`, `isSRetParameterAfterThis`。

[`CGVTT.cpp`](CGVTT.cpp) — Itanium VTT (virtual table table) global 输出
(`EmitVTTDefinition`)。
- 上游: [`CodeGenModule.cpp`](CodeGenModule.cpp) (`getAddrOfVTT`)。
- 下游: [`CodeGenModule.h`](CodeGenModule.h),
  [`CGCXXABI.h`](CGCXXABI.h),
  [`clang/AST/VTTBuilder.h`](../../include/clang/AST/VTTBuilder.h)。
- 关键类/函数: `CodeGenVTables::EmitVTTDefinition`,
  `GetAddrOfVTTVTable`, `VTTBuilder`。

[`CGVTables.cpp`](CGVTables.cpp) + [`CGVTables.h`](CGVTables.h) —
vtables for both ABIs: `createVTableInitializer`, thunks
(virtual / this-adjustment), secondary-virtual-pointer indices。
- 上游: [`CodeGenModule.cpp`](CodeGenModule.cpp),
  [`CGCXX.cpp`](CGCXX.cpp), [`CGClass.cpp`](CGClass.cpp),
  [`CGVTT.cpp`](CGVTT.cpp)。
- 下游: [`CGCXXABI.h`](CGCXXABI.h),
  [`CGDebugInfo.h`](CGDebugInfo.h),
  [`CodeGenFunction.h`](CodeGenFunction.h),
  [`CodeGenModule.h`](CodeGenModule.h),
  [`clang/AST/CXXInheritance.h`](../../include/clang/AST/CXXInheritance.h),
  [`clang/CodeGen/CGFunctionInfo.h`](../../include/clang/CodeGen/CGFunctionInfo.h),
  [`clang/CodeGen/ConstantInitBuilder.h`](../../include/clang/CodeGen/ConstantInitBuilder.h),
  [`llvm/IR/IRBuilder.h`](../../../llvm/include/llvm/IR/IRBuilder.h),
  [`llvm/Transforms/Utils/Cloning.h`](../../../llvm/include/llvm/Transforms/Utils/Cloning.h)。
- 关键类/函数: `class CodeGenVTables`, `maybeEmitThunk`,
  `GenerateConstructionVTable`, `GetAddrOfVTable`, `GetAddrOfThunk`,
  `addVTableComponent`, `createVTableInitializer`,
  `setThunkProperties`, `PerformReturnAdjustment`。

### 3.5 C++ Statements / EH / Coroutines / Blocks

[`CGDecl.cpp`](CGDecl.cpp) — 函数作用域 + 全局 VarDecl emission,
lifetime intrinsics, automatic/temporary storage。
- 上游: [`CGStmt.cpp`](CGStmt.cpp) (DeclStmt),
  [`CodeGenFunction.cpp`](CodeGenFunction.cpp)。
- 下游: [`CGBlocks.h`](CGBlocks.h), [`CGCXXABI.h`](CGCXXABI.h),
  [`CGCleanup.h`](CGCleanup.h),
  [`CGDebugInfo.h`](CGDebugInfo.h),
  [`CGOpenCLRuntime.h`](CGOpenCLRuntime.h),
  [`CGOpenMPRuntime.h`](CGOpenMPRuntime.h),
  [`CodeGenFunction.h`](CodeGenFunction.h),
  [`CodeGenModule.h`](CodeGenModule.h),
  [`CodeGenPGO.h`](CodeGenPGO.h),
  [`ConstantEmitter.h`](ConstantEmitter.h),
  [`EHScopeStack.h`](EHScopeStack.h),
  [`PatternInit.h`](PatternInit.h),
  [`TargetInfo.h`](TargetInfo.h),
  [`clang/AST/Decl.h`](../../include/clang/AST/Decl.h),
  [`clang/Sema/Sema.h`](../../include/clang/Sema/Sema.h)。
- 关键类/函数: `CodeGenFunction::EmitDecl`, `EmitVarDecl`,
  `EmitParmDecl`, `EmitStaticVarDecl`, `EmitAutoVarDecl`,
  `EmitAutoVarInit`, `shouldEmitLifetimeMarkers`。

[`CGDeclCXX.cpp`](CGDeclCXX.cpp) — C++ decl emission: dynamic init,
thread-locals, guarded init, destruction registration,
`__cxa_atexit` / `__cxa_thread_atexit`。
- 上游: [`CGDecl.cpp`](CGDecl.cpp), [`CodeGenModule.cpp`](CodeGenModule.cpp)。
- 下游: [`CGCXXABI.h`](CGCXXABI.h),
  [`CGDebugInfo.h`](CGDebugInfo.h),
  [`CGHLSLRuntime.h`](CGHLSLRuntime.h),
  [`CGObjCRuntime.h`](CGObjCRuntime.h),
  [`CGOpenMPRuntime.h`](CGOpenMPRuntime.h),
  [`CodeGenFunction.h`](CodeGenFunction.h),
  [`TargetInfo.h`](TargetInfo.h),
  [`clang/AST/Attr.h`](../../include/clang/AST/Attr.h),
  [`clang/Basic/LangOptions.h`](../../include/clang/Basic/LangOptions.h),
  [`llvm/ADT/StringExtras.h`](../../../llvm/include/llvm/ADT/StringExtras.h)。
- 关键类/函数: `EmitDeclInit`, `EmitDeclDestroy`,
  `EmitCXXGlobalVarDeclInit`, `EmitCXXThreadLocalInit`,
  `EmitGuardedInit`, `EmitDynamicInit`,
  `AddInitializerToStaticVarDecl`。

[`CGClass.cpp`](CGClass.cpp) — C++ class codegen: vtable/VB-table
查询、base/derived destructor emission、construction vtables、
placement-new helper。
- 上游: [`CGDeclCXX.cpp`](CGDeclCXX.cpp),
  [`CGExprCXX.cpp`](CGExprCXX.cpp),
  [`CGVTables.cpp`](CGVTables.cpp)。
- 下游: [`ABIInfoImpl.h`](ABIInfoImpl.h),
  [`CGBlocks.h`](CGBlocks.h), [`CGCXXABI.h`](CGCXXABI.h),
  [`CGDebugInfo.h`](CGDebugInfo.h),
  [`CGRecordLayout.h`](CGRecordLayout.h),
  [`CodeGenFunction.h`](CodeGenFunction.h),
  [`TargetInfo.h`](TargetInfo.h),
  [`clang/AST/RecordLayout.h`](../../include/clang/AST/RecordLayout.h),
  [`llvm/IR/Intrinsics.h`](../../../llvm/include/llvm/IR/Intrinsics.h),
  [`llvm/Support/SaveAndRestore.h`](../../../llvm/include/llvm/Support/SaveAndRestore.h),
  [`llvm/Transforms/Utils/ModuleUtils.h`](../../../llvm/include/llvm/Transforms/Utils/ModuleUtils.h)。
- 关键类/函数: `CodeGenModule::getClassPointerAlignment`,
  `getMinimumClassObjectSize`, `getVBaseAlignment`,
  `EmitClassDestructor`, `EmitClassConstructorCall`,
  `EmitDestructorBody`, `EmitBaseDestructor`。

[`CGBlocks.cpp`](CGBlocks.cpp) + [`CGBlocks.h`](CGBlocks.h) — Clang
Block codegen: layout, copy/dispose helper, global blocks, byref
variable lowering。
- 上游: [`CGExpr.cpp`](CGExpr.cpp) (`BlockExpr`),
  [`CGDecl.cpp`](CGDecl.cpp) (BlockDecl),
  [`CodeGenFunction.cpp`](CodeGenFunction.cpp)。
- 下游: [`CGCXXABI.h`](CGCXXABI.h),
  [`CGDebugInfo.h`](CGDebugInfo.h),
  [`CGObjCRuntime.h`](CGObjCRuntime.h),
  [`CGOpenCLRuntime.h`](CGOpenCLRuntime.h),
  [`CodeGenFunction.h`](CodeGenFunction.h),
  [`CodeGenModule.h`](CodeGenModule.h),
  [`CodeGenPGO.h`](CodeGenPGO.h),
  [`ConstantEmitter.h`](ConstantEmitter.h),
  [`TargetInfo.h`](TargetInfo.h),
  [`clang/AST/DeclObjC.h`](../../include/clang/AST/DeclObjC.h),
  [`clang/CodeGen/ConstantInitBuilder.h`](../../include/clang/CodeGen/ConstantInitBuilder.h),
  [`llvm/IR/DataLayout.h`](../../../llvm/include/llvm/IR/DataLayout.h)。
- 关键类/函数: `class CGBlockInfo`, `BlockByrefInfo`,
  `BlockByrefHelpers`, `enum BlockLiteralFlags`,
  `GenerateCopyHelperFunction`, `GenerateDestroyHelperFunction`,
  `buildGlobalBlock`, `getBlockCaptureStr`。

[`CGException.cpp`](CGException.cpp) — C++ EH: `throw`, `try`/`catch`,
SEH (`__try`/`__finally`/`__except`), terminate/unexpected, MSVC
table-based EH。
- 上游: [`CGStmt.cpp`](CGStmt.cpp),
  [`CodeGenFunction.cpp`](CodeGenFunction.cpp)。
- 下游: [`CGCXXABI.h`](CGCXXABI.h),
  [`CGCleanup.h`](CGCleanup.h),
  [`CGDebugInfo.h`](CGDebugInfo.h),
  [`CGObjCRuntime.h`](CGObjCRuntime.h),
  [`CodeGenFunction.h`](CodeGenFunction.h),
  [`ConstantEmitter.h`](ConstantEmitter.h),
  [`TargetInfo.h`](TargetInfo.h),
  [`clang/AST/StmtCXX.h`](../../include/clang/AST/StmtCXX.h),
  [`llvm/IR/Intrinsics.h`](../../../llvm/include/llvm/IR/Intrinsics.h),
  `llvm/IR/IntrinsicsWebAssembly.h` (tablegen 生成, 通常存在 build 目录)。
- 关键类/函数: `CodeGenModule::getTerminateFn`,
  `EmitCXXThrowExpr`, `EmitEHCatch`, `EmitSEHTryStmt`,
  `EmitSEHExceptStmt`, `EmitSEHFinallyStmt`, `class EHPersonality`,
  `getFreeExceptionFn`, `getSehTryBeginFn`。

[`CGCleanup.cpp`](CGCleanup.cpp) + [`CGCleanup.h`](CGCleanup.h) —
EH-scope stack, lazy cleanup emission, branch fixups, lifetime/
fake-use marker, cleanup helper。
- 上游: [`CodeGenFunction.cpp`](CodeGenFunction.cpp),
  [`CGDecl.cpp`](CGDecl.cpp),
  [`CGCleanup.cpp`](CGCleanup.cpp)。
- 下游: [`CodeGenFunction.h`](CodeGenFunction.h),
  [`EHScopeStack.h`](EHScopeStack.h),
  [`llvm/Support/SaveAndRestore.h`](../../../llvm/include/llvm/Support/SaveAndRestore.h)。
- 关键类/函数: `class EHScope`, `class EHCleanupScope`,
  `class EHCatchScope`, `class EHFilterScope`,
  `class EHTerminateScope`, `class EHScopeStack::Cleanup`,
  `DominatingValue<RValue>`, `pushCleanup`, `popCleanup`,
  `addBranchAfter`, `addBranchThrough`。

[`EHScopeStack.h`](EHScopeStack.h) — `EHScopeStack` POD + `stable_iterator`
+ `Cleanup` ABC; `CGCleanup.h` 的可见声明。
- 上游: [`CGCall.h`](CGCall.h), [`CGValue.h`](CGValue.h),
  [`CGBlocks.h`](CGBlocks.h), [`CGCleanup.h`](CGCleanup.h),
  [`CodeGenFunction.h`](CodeGenFunction.h)。
- 下游: [`clang/Basic/LLVM.h`](../../include/clang/Basic/LLVM.h),
  [`llvm/IR/BasicBlock.h`](../../../llvm/include/llvm/IR/BasicBlock.h),
  [`llvm/IR/Instructions.h`](../../../llvm/include/llvm/IR/Instructions.h)。
- 关键类/函数: `class EHScopeStack`, `class stable_iterator`,
  `class Cleanup`, `enum CleanupKind`, `BranchFixup`,
  `DominatingValue`, `InvariantValue`。

[`CGCoroutine.cpp`](CGCoroutine.cpp) — C++20 coroutine lowering,
`llvm.coro.*` intrinsics, suspend points, frame management, promise
hook。
- 上游: [`CGStmt.cpp`](CGStmt.cpp) (CoroutineBodyStmt)。
- 下游: [`CGCleanup.h`](CGCleanup.h),
  [`CGDebugInfo.h`](CGDebugInfo.h),
  [`CodeGenFunction.h`](CodeGenFunction.h),
  [`clang/AST/StmtCXX.h`](../../include/clang/AST/StmtCXX.h),
  [`clang/AST/StmtVisitor.h`](../../include/clang/AST/StmtVisitor.h),
  [`llvm/ADT/ScopeExit.h`](../../../llvm/include/llvm/ADT/ScopeExit.h)。
- 关键类/函数: `struct CGCoroData`, `enum AwaitKind`,
  `EmitCoroutineBody`, `EmitCoawaitExpr`, `EmitCoyieldExpr`,
  `EmitCoreturnStmt`, `EmitCoroutineMove`, `buildSuspendBlock`。

### 3.6 Builtins

[`CGBuiltin.cpp`](CGBuiltin.cpp) + [`CGBuiltin.h`](CGBuiltin.h) —
所有 target-agnostic 和许多 target-specific `__builtin_*` 的主分发
(`BuiltinID` 中心 switch)。
- 上游: [`CodeGenFunction.cpp`](CodeGenFunction.cpp) (经
  `EmitBuiltinExpr`)。
- 下游: `ABIInfo.h`, [`CGCUDARuntime.h`](CGCUDARuntime.h),
  [`CGCXXABI.h`](CGCXXABI.h), [`CGDebugInfo.h`](CGDebugInfo.h),
  [`CGObjCRuntime.h`](CGObjCRuntime.h),
  [`CGOpenCLRuntime.h`](CGOpenCLRuntime.h),
  [`CGRecordLayout.h`](CGRecordLayout.h),
  [`CGValue.h`](CGValue.h),
  [`CodeGenFunction.h`](CodeGenFunction.h),
  [`CodeGenModule.h`](CodeGenModule.h),
  [`ConstantEmitter.h`](ConstantEmitter.h),
  [`PatternInit.h`](PatternInit.h),
  [`TargetInfo.h`](TargetInfo.h), 每个
  [`TargetBuiltins/*.cpp`](TargetBuiltins/),
  [`clang/AST/OSLog.h`](../../include/clang/AST/OSLog.h),
  [`clang/Basic/TargetInfo.h`](../../include/clang/Basic/TargetInfo.h),
  [`llvm/IR/Intrinsics.h`](../../../llvm/include/llvm/IR/Intrinsics.h),
  `llvm/IR/IntrinsicsX86.h` (tablegen 生成, 通常存在 build 目录),
  [`llvm/Support/ConvertUTF.h`](../../../llvm/include/llvm/Support/ConvertUTF.h)。
- 关键类/函数: `enum class MSVCIntrin`,
  `EmitTargetArchBuiltinExpr`, `shouldEmitBuiltinAsIR`,
  `EmitToInt`, `EmitFromInt`, `CheckAtomicAlignment`,
  `MakeBinaryAtomicValue`, `MakeAtomicCmpXchgValue`,
  `EmitOverflowIntrinsic`, `emitBuiltinWithOneOverloadedType`,
  `appendDefaultIntrinsicArgs`。

[`CGAtomic.cpp`](CGAtomic.cpp) — `_Atomic` / `__atomic_*` builtin
lowering: atomic info, libcalls vs intrinsics, lock-free 检测。
- 上游: [`CGExpr.cpp`](CGExpr.cpp) (AtomicExpr),
  [`CGBuiltin.cpp`](CGBuiltin.cpp)。
- 下游: [`CGCall.h`](CGCall.h),
  [`CGRecordLayout.h`](CGRecordLayout.h),
  [`CodeGenFunction.h`](CodeGenFunction.h),
  [`CodeGenModule.h`](CodeGenModule.h),
  [`TargetInfo.h`](TargetInfo.h),
  [`clang/AST/ASTContext.h`](../../include/clang/AST/ASTContext.h),
  [`clang/CodeGen/CGFunctionInfo.h`](../../include/clang/CodeGen/CGFunctionInfo.h),
  [`llvm/ADT/DenseMap.h`](../../../llvm/include/llvm/ADT/DenseMap.h),
  [`llvm/IR/Intrinsics.h`](../../../llvm/include/llvm/IR/Intrinsics.h)。
- 关键类/函数: `class AtomicInfo`, `EmitAtomicOp`,
  `EmitAtomicCmpXchg`, `EmitAtomicRMW`, `IsLockFree`,
  `EmitAtomicInit`。

### 3.7 Debug Info / TBAA

[`CGDebugInfo.cpp`](CGDebugInfo.cpp) + [`CGDebugInfo.h`](CGDebugInfo.h)
— 构造 LLVM `DIBuilder` debug info: types、scopes、variables、
lines、inlinedAt (最大的 debug-info emitter)。
- 上游: [`CodeGenFunction.cpp`](CodeGenFunction.cpp),
  [`CodeGenModule.cpp`](CodeGenModule.cpp),
  [`CGStmt.cpp`](CGStmt.cpp), 每个调 `DI->Emit*` 的 emitter。
- 下游: [`CGBlocks.h`](CGBlocks.h), [`CGCXXABI.h`](CGCXXABI.h),
  [`CGObjCRuntime.h`](CGObjCRuntime.h),
  [`CGRecordLayout.h`](CGRecordLayout.h),
  [`CodeGenFunction.h`](CodeGenFunction.h),
  [`CodeGenModule.h`](CodeGenModule.h),
  [`ConstantEmitter.h`](ConstantEmitter.h),
  [`TargetInfo.h`](TargetInfo.h),
  [`clang/AST/RecordLayout.h`](../../include/clang/AST/RecordLayout.h),
  [`clang/AST/RecursiveASTVisitor.h`](../../include/clang/AST/RecursiveASTVisitor.h),
  [`clang/Basic/CodeGenOptions.h`](../../include/clang/Basic/CodeGenOptions.h),
  [`clang/Frontend/FrontendOptions.h`](../../include/clang/Frontend/FrontendOptions.h),
  [`llvm/IR/DebugInfo.h`](../../../llvm/include/llvm/IR/DebugInfo.h),
  [`llvm/IR/Metadata.h`](../../../llvm/include/llvm/IR/Metadata.h),
  [`llvm/Support/MD5.h`](../../../llvm/include/llvm/Support/MD5.h),
  [`llvm/Support/SHA1.h`](../../../llvm/include/llvm/Support/SHA1.h),
  [`llvm/Support/SHA256.h`](../../../llvm/include/llvm/Support/SHA256.h),
  [`llvm/Support/TimeProfiler.h`](../../../llvm/include/llvm/Support/TimeProfiler.h)。
- 关键类/函数: `class CGDebugInfo`, `class ApplyDebugLocation`,
  `ApplyAtomGroup`, `SaveAndRestoreLocation`, `PrintingCallbacks`,
  `EmitFunctionDecl`, `EmitGlobalVariable`, `EmitType`, `EmitDeclare`,
  `EmitMember`, `EmitInlinedAt`。

[`CGLoopInfo.cpp`](CGLoopInfo.cpp) + [`CGLoopInfo.h`](CGLoopInfo.h) —
loop-attach metadata (`llvm.loop.vectorize`、`llvm.loop.unroll`、
pipeline、followup、distribution)。
- 上游: [`CodeGenFunction.cpp`](CodeGenFunction.cpp) (loop emission)。
- 下游: [`clang/AST/Attr.h`](../../include/clang/AST/Attr.h),
  [`clang/Basic/CodeGenOptions.h`](../../include/clang/Basic/CodeGenOptions.h),
  [`llvm/IR/BasicBlock.h`](../../../llvm/include/llvm/IR/BasicBlock.h),
  [`llvm/IR/CFG.h`](../../../llvm/include/llvm/IR/CFG.h),
  [`llvm/IR/Metadata.h`](../../../llvm/include/llvm/IR/Metadata.h)。
- 关键类/函数: `class LoopInfo`, `LoopAttributes`, `Push`, `Pop`,
  `createFollowupMetadata`, `createPipeliningMetadata`,
  `createPartialUnrollMetadata`, `createVectorizeMetadata`。

[`CodeGenTBAA.cpp`](CodeGenTBAA.cpp) + [`CodeGenTBAA.h`](CodeGenTBAA.h)
— Type-Based Alias Analysis metadata tree, scalar/struct path tag,
`!tbaa` 注解到 load/store。
- 上游: [`CGValue.h`](CGValue.h),
  [`CGExprScalar.cpp`](CGExprScalar.cpp),
  [`CGExprAgg.cpp`](CGExprAgg.cpp),
  [`CodeGenModule.cpp`](CodeGenModule.cpp)。
- 下游: [`ABIInfoImpl.h`](ABIInfoImpl.h),
  [`CGCXXABI.h`](CGCXXABI.h),
  [`CGRecordLayout.h`](CGRecordLayout.h),
  [`CodeGenTypes.h`](CodeGenTypes.h),
  [`clang/AST/Mangle.h`](../../include/clang/AST/Mangle.h),
  [`clang/Basic/CodeGenOptions.h`](../../include/clang/Basic/CodeGenOptions.h),
  [`llvm/IR/Metadata.h`](../../../llvm/include/llvm/IR/Metadata.h)。
- 关键类/函数: `class CodeGenTBAA`, `enum TBAAAccessKind`,
  `struct TBAAAccessInfo`, `getRoot`, `getChar`,
  `createScalarTypeNode`, `getTypeInfo`, `CollectFields`。

### 3.8 PGO

[`CodeGenPGO.cpp`](CodeGenPGO.cpp) + [`CodeGenPGO.h`](CodeGenPGO.h) —
Profile-Guided Optimization 插桩、region-counter assignment、
MC/DC state。
- 上游: [`CodeGenFunction.cpp`](CodeGenFunction.cpp),
  [`CGStmt.cpp`](CGStmt.cpp), [`CGStmtOpenMP.cpp`](CGStmtOpenMP.cpp),
  [`CoverageMappingGen.cpp`](CoverageMappingGen.cpp)。
- 下游: [`CGDebugInfo.h`](CGDebugInfo.h),
  [`CodeGenFunction.h`](CodeGenFunction.h),
  [`CoverageMappingGen.h`](CoverageMappingGen.h),
  [`clang/AST/RecursiveASTVisitor.h`](../../include/clang/AST/RecursiveASTVisitor.h),
  [`llvm/IR/Intrinsics.h`](../../../llvm/include/llvm/IR/Intrinsics.h),
  [`llvm/Support/CommandLine.h`](../../../llvm/include/llvm/Support/CommandLine.h),
  [`llvm/Support/Endian.h`](../../../llvm/include/llvm/Support/Endian.h),
  [`llvm/Support/MD5.h`](../../../llvm/include/llvm/Support/MD5.h)。
- 关键类/函数: `class CodeGenPGO`, `class PGOHash`,
  `enum PGOHashVersion`, `setFuncName`, `assignRegionCounters`,
  `emitCounterRegionMapping`, `valueProfile`,
  `emitMCDCTestVectorBitmapUpdate`, `emitMCDCCondBitmapUpdate`。

[`MCDCState.h`](MCDCState.h) — per-function MC/DC (Modified Condition/
Decision Coverage) state。
- 上游: [`CodeGenPGO.h`](CodeGenPGO.h),
  [`CoverageMappingGen.h`](CoverageMappingGen.h)。
- 下游: [`Address.h`](Address.h),
  [`llvm/ProfileData/Coverage/MCDCTypes.h`](../../../llvm/include/llvm/ProfileData/Coverage/MCDCTypes.h)。
- 关键类/函数: `struct State`, `struct Decision`, `struct Branch`,
  `BitmapIdx`, `Indices`, `DecisionByStmt`, `BranchByStmt`。

### 3.9 Objective-C

[`CGObjC.cpp`](CGObjC.cpp) — 通用 ObjC expression emission、message-send
lowering、string literal、property access、ARC。
- 上游: [`CGExpr.cpp`](CGExpr.cpp), [`CGStmt.cpp`](CGStmt.cpp),
  [`CodeGenFunction.cpp`](CodeGenFunction.cpp)。
- 下游: [`CGDebugInfo.h`](CGDebugInfo.h),
  [`CGObjCRuntime.h`](CGObjCRuntime.h),
  [`CodeGenFunction.h`](CodeGenFunction.h),
  [`CodeGenModule.h`](CodeGenModule.h),
  [`CodeGenPGO.h`](CodeGenPGO.h),
  [`ConstantEmitter.h`](ConstantEmitter.h),
  [`TargetInfo.h`](TargetInfo.h),
  [`clang/AST/DeclObjC.h`](../../include/clang/AST/DeclObjC.h),
  [`clang/AST/NSAPI.h`](../../include/clang/AST/NSAPI.h),
  [`clang/AST/StmtObjC.h`](../../include/clang/AST/StmtObjC.h),
  [`clang/CodeGen/CGFunctionInfo.h`](../../include/clang/CodeGen/CGFunctionInfo.h),
  [`llvm/Analysis/ObjCARCUtil.h`](../../../llvm/include/llvm/Analysis/ObjCARCUtil.h),
  [`llvm/BinaryFormat/MachO.h`](../../../llvm/include/llvm/BinaryFormat/MachO.h)。
- 关键类/函数: `CodeGenFunction::EmitObjCStringLiteral`,
  `EmitObjCBoxedExpr`, `EmitObjCArrayLiteral`,
  `EmitObjCDictionaryLiteral`, `EmitObjCMessageExpr`,
  `tryEmitARCRetainScalarExpr`, `EmitObjCPropertyRefExpr`。

[`CGObjCRuntime.cpp`](CGObjCRuntime.cpp) + [`CGObjCRuntime.h`](CGObjCRuntime.h)
— 抽象 `class CGObjCRuntime` 接口 + 共享 helper (ivar offsets、
try-catch 脚手架、`@synchronized`)。
- 上游: [`CGObjC.cpp`](CGObjC.cpp),
  [`CGObjCMac.cpp`](CGObjCMac.cpp),
  [`CGObjCGNU.cpp`](CGObjCGNU.cpp),
  [`CodeGenModule.cpp`](CodeGenModule.cpp)。
- 下游: [`CGBuilder.h`](CGBuilder.h),
  [`CGCall.h`](CGCall.h), [`CGCleanup.h`](CGCleanup.h),
  [`CGValue.h`](CGValue.h),
  [`clang/AST/DeclObjC.h`](../../include/clang/AST/DeclObjC.h),
  [`clang/Basic/IdentifierTable.h`](../../include/clang/Basic/IdentifierTable.h),
  [`llvm/ADT/UniqueVector.h`](../../../llvm/include/llvm/ADT/UniqueVector.h)。
- 关键类/函数: `class CGObjCRuntime`, `ComputeIvarBaseOffset`,
  `EmitValueForIvarAtOffset`, `EmitTryCatchStmt`,
  `EmitAtSynchronizedStmt`, `GenerateConstantString`, `GetClass`,
  `GenerateProtocol`。

[`CGObjCMac.cpp`](CGObjCMac.cpp) +
[`CGObjCMacConstantLiteralUtil.h`](CGObjCMacConstantLiteralUtil.h) —
Apple ObjC runtime: `objc_msgSend` 家族、ivar layout、class structure、
`-fobjc-constant-literals` cache。
- 上游: [`CGObjC.cpp`](CGObjC.cpp), [`CodeGenModule.cpp`](CodeGenModule.cpp)
  (工厂)。
- 下游: [`CGBlocks.h`](CGBlocks.h), [`CGCleanup.h`](CGCleanup.h),
  [`CGObjCMacConstantLiteralUtil.h`](CGObjCMacConstantLiteralUtil.h),
  [`CGObjCRuntime.h`](CGObjCRuntime.h),
  [`CGRecordLayout.h`](CGRecordLayout.h),
  [`CodeGenFunction.h`](CodeGenFunction.h),
  [`CodeGenModule.h`](CodeGenModule.h),
  [`clang/AST/DeclObjC.h`](../../include/clang/AST/DeclObjC.h),
  [`clang/AST/Mangle.h`](../../include/clang/AST/Mangle.h),
  [`clang/CodeGen/CodeGenABITypes.h`](../../include/clang/CodeGen/CodeGenABITypes.h),
  [`clang/CodeGen/ConstantInitBuilder.h`](../../include/clang/CodeGen/ConstantInitBuilder.h),
  [`llvm/ADT/CachedHashString.h`](../../../llvm/include/llvm/ADT/CachedHashString.h),
  [`llvm/IR/InlineAsm.h`](../../../llvm/include/llvm/IR/InlineAsm.h),
  [`llvm/IR/Module.h`](../../../llvm/include/llvm/IR/Module.h)。
- 关键类/函数: `class ObjCCommonTypesHelper`,
  `class IvarLayoutBuilder`, `class CGObjCCommonMac`, `class CGObjCMac`,
  `class CGObjCNonFragileABIMac`, `NSConstantNumberMapInfo`,
  `EmitClassExtension`, `emitObjCProtocol`, `GenerateCategoryList`。

[`CGObjCGNU.cpp`](CGObjCGNU.cpp) — GNU runtime (和 ObjFW、GCC ABI 变体),
较慢的路径、手动 layout、lazy runtime function。
- 上游: [`CodeGenModule.cpp`](CodeGenModule.cpp) (工厂
  `-fobjc-runtime=gnu*`)。
- 下游: [`CGCXXABI.h`](CGCXXABI.h), [`CGCleanup.h`](CGCleanup.h),
  [`CGObjCRuntime.h`](CGObjCRuntime.h),
  [`CodeGenFunction.h`](CodeGenFunction.h),
  [`CodeGenModule.h`](CodeGenModule.h),
  [`CodeGenTypes.h`](CodeGenTypes.h),
  [`SanitizerMetadata.h`](SanitizerMetadata.h),
  [`clang/AST/DeclObjC.h`](../../include/clang/AST/DeclObjC.h),
  [`clang/CodeGen/ConstantInitBuilder.h`](../../include/clang/CodeGen/ConstantInitBuilder.h),
  [`llvm/IR/DataLayout.h`](../../../llvm/include/llvm/IR/DataLayout.h),
  [`llvm/IR/Intrinsics.h`](../../../llvm/include/llvm/IR/Intrinsics.h),
  [`llvm/IR/LLVMContext.h`](../../../llvm/include/llvm/IR/LLVMContext.h)。
- 关键类/函数: `class CGObjCGNU`, `class LazyRuntimeFunction`,
  `class SelectorTable`, `class ClassNames`, `GenerateClass`,
  `GenerateProtocolList`, `ModuleInitFunction`, `EmitClassRef`。

### 3.10 OpenMP

[`CGOpenMPRuntime.cpp`](CGOpenMPRuntime.cpp) +
[`CGOpenMPRuntime.h`](CGOpenMPRuntime.h) — OpenMP host runtime 主函数:
outlined parallel/task/teams function、reduction、declare-reduction、
OMP regions、requires directive。
- 上游: [`CGStmt.cpp`](CGStmt.cpp),
  [`CGStmtOpenMP.cpp`](CGStmtOpenMP.cpp),
  [`CodeGenFunction.cpp`](CodeGenFunction.cpp)。
- 下游: [`ABIInfoImpl.h`](ABIInfoImpl.h),
  [`CGCXXABI.h`](CGCXXABI.h), [`CGCleanup.h`](CGCleanup.h),
  [`CGDebugInfo.h`](CGDebugInfo.h),
  [`CGRecordLayout.h`](CGRecordLayout.h),
  [`CodeGenFunction.h`](CodeGenFunction.h),
  [`TargetInfo.h`](TargetInfo.h),
  [`clang/AST/OpenMPClause.h`](../../include/clang/AST/OpenMPClause.h),
  [`clang/AST/StmtOpenMP.h`](../../include/clang/AST/StmtOpenMP.h),
  [`clang/CodeGen/ConstantInitBuilder.h`](../../include/clang/CodeGen/ConstantInitBuilder.h),
  [`llvm/Bitcode/BitcodeReader.h`](../../../llvm/include/llvm/Bitcode/BitcodeReader.h),
  [`llvm/Support/AtomicOrdering.h`](../../../llvm/include/llvm/Support/AtomicOrdering.h)。
- 关键类/函数: `class CGOpenMPRuntime`,
  `class PrePostActionTy`, `class RegionCodeGenTy`,
  `class CGOpenMPRegionInfo`, `emitParallel`, `emitTask`, `emitTeams`,
  `emitReduction`, `emitOMPRuntimeCall`,
  `getAddrOfDeclareReduction`。

[`CGOpenMPRuntimeGPU.cpp`](CGOpenMPRuntimeGPU.cpp) +
[`CGOpenMPRuntimeGPU.h`](CGOpenMPRuntimeGPU.h) — 专用 GPU OpenMP runtime
(NVPTX、AMDGCN、SPIR-V), kernel-launch、SPMD/Generic、shared-memory、
grid-value 常量。
- 上游: [`CGOpenMPRuntime.cpp`](CGOpenMPRuntime.cpp) (在 `isGPU()` 时
  的工厂)。
- 下游: [`CGDebugInfo.h`](CGDebugInfo.h),
  [`CodeGenFunction.h`](CodeGenFunction.h),
  [`TargetInfo.h`](TargetInfo.h),
  [`clang/AST/DeclOpenMP.h`](../../include/clang/AST/DeclOpenMP.h),
  [`clang/AST/OpenMPClause.h`](../../include/clang/AST/OpenMPClause.h),
  [`llvm/ADT/SmallPtrSet.h`](../../../llvm/include/llvm/ADT/SmallPtrSet.h),
  [`llvm/Frontend/OpenMP/OMPDeviceConstants.h`](../../../llvm/include/llvm/Frontend/OpenMP/OMPDeviceConstants.h),
  [`llvm/Frontend/OpenMP/OMPGridValues.h`](../../../llvm/include/llvm/Frontend/OpenMP/OMPGridValues.h),
  [`llvm/TargetParser/NVPTXTargetParser.h`](../../../llvm/include/llvm/TargetParser/NVPTXTargetParser.h)。
- 关键类/函数: `class CGOpenMPRuntimeGPU`, `enum ExecutionMode`,
  `enum DataSharingMode`, `class NVPTXActionTy`, `emitKernel`,
  `emitGenericVarsProlog`, `emitGenericVarsEpilog`, `emitSPMDEntry`,
  `syncCTAThreads`。

### 3.11 OpenCL / GPU / CUDA

[`CGOpenCLRuntime.cpp`](CGOpenCLRuntime.cpp) +
[`CGOpenCLRuntime.h`](CGOpenCLRuntime.h) — 抽象 OpenCL runtime: pipe/
sampler/image type、enqueued block、workgroup-local var。
- 上游: [`CGDecl.cpp`](CGDecl.cpp), [`CGExpr.cpp`](CGExpr.cpp),
  [`CGExprConstant.cpp`](CGExprConstant.cpp),
  [`CGStmt.cpp`](CGStmt.cpp), [`CodeGenModule.cpp`](CodeGenModule.cpp)。
- 下游: [`CodeGenFunction.h`](CodeGenFunction.h),
  [`TargetInfo.h`](TargetInfo.h),
  [`clang/CodeGen/ConstantInitBuilder.h`](../../include/clang/CodeGen/ConstantInitBuilder.h),
  [`llvm/IR/DerivedTypes.h`](../../../llvm/include/llvm/IR/DerivedTypes.h),
  [`llvm/IR/GlobalValue.h`](../../../llvm/include/llvm/IR/GlobalValue.h)。
- 关键类/函数: `class CGOpenCLRuntime`, `convertOpenCLSpecificType`,
  `getPipeType`, `getSamplerType`, `EmitWorkGroupLocalVarDecl`,
  `getPipeElemSize`, `EnqueuedBlockInfo`。

[`CGGPUBuiltin.cpp`](CGGPUBuiltin.cpp) — GPU-agnostic builtin codegen:
`printf` → `vprintf` rewrite for NVPTX, AMDGPU printf buffer packing。
- 上游: [`CGBuiltin.cpp`](CGBuiltin.cpp) (printf 分发)。
- 下游: [`CodeGenFunction.h`](CodeGenFunction.h),
  [`clang/Basic/Builtins.h`](../../include/clang/Basic/Builtins.h),
  [`llvm/IR/DataLayout.h`](../../../llvm/include/llvm/IR/DataLayout.h),
  [`llvm/IR/Instruction.h`](../../../llvm/include/llvm/IR/Instruction.h),
  [`llvm/Transforms/Utils/AMDGPUEmitPrintf.h`](../../../llvm/include/llvm/Transforms/Utils/AMDGPUEmitPrintf.h)。
- 关键类/函数: `GetVprintfDeclaration`,
  `packArgsIntoNVPTXFormatBuffer`, `EmitAMDGPUPrintfCall`,
  `EmitNVPTXPrintfCall`。

[`CGCUDARuntime.cpp`](CGCUDARuntime.cpp) +
[`CGCUDARuntime.h`](CGCUDARuntime.h) — 抽象 CUDA runtime + 通用
`EmitCUDADeviceKernelCallExpr`。
- 上游: [`CGExpr.cpp`](CGExpr.cpp) (CUDAKernelCallExpr),
  [`CGBuiltin.cpp`](CGBuiltin.cpp)。
- 下游: [`CGCall.h`](CGCall.h),
  [`CodeGenFunction.h`](CodeGenFunction.h),
  [`clang/AST/ExprCXX.h`](../../include/clang/AST/ExprCXX.h)。
- 关键类/函数: `class CGCUDARuntime`, `class DeviceVarFlags`,
  `EmitCUDADeviceKernelCallExpr`, `EmitCUDAKernelCallExpr`。

[`CGCUDANV.cpp`](CGCUDANV.cpp) — NVIDIA/HIP CUDA runtime: kernel
stubs、fatbin 注册、gpu binary handle、managed variable。
- 上游: [`CodeGenModule.cpp`](CodeGenModule.cpp) (工厂
  `-fcuda-runtime=...` 或 HIP)。
- 下游: [`CGCUDARuntime.h`](CGCUDARuntime.h),
  [`CGCXXABI.h`](CGCXXABI.h),
  [`CodeGenFunction.h`](CodeGenFunction.h),
  [`CodeGenModule.h`](CodeGenModule.h),
  [`clang/Basic/Cuda.h`](../../include/clang/Basic/Cuda.h),
  [`clang/CodeGen/CodeGenABITypes.h`](../../include/clang/CodeGen/CodeGenABITypes.h),
  [`llvm/Frontend/Offloading/Utility.h`](../../../llvm/include/llvm/Frontend/Offloading/Utility.h),
  [`llvm/ProfileData/InstrProf.h`](../../../llvm/include/llvm/ProfileData/InstrProf.h),
  [`llvm/Support/Format.h`](../../../llvm/include/llvm/Support/Format.h),
  [`llvm/Support/MD5.h`](../../../llvm/include/llvm/Support/MD5.h),
  [`llvm/Transforms/Utils/ModuleUtils.h`](../../../llvm/include/llvm/Transforms/Utils/ModuleUtils.h)。
- 关键类/函数: `class CGNVCUDARuntime`, `constexpr CudaFatMagic`,
  `constexpr HIPFatMagic`, `KernelInfo`, `KernelHandles`, `DeviceVars`,
  `ModuleCtorFunction`, `ModuleDtorFunction`。

### 3.12 HLSL / DirectX / SPIR-V

[`CGHLSLRuntime.cpp`](CGHLSLRuntime.cpp) +
[`CGHLSLRuntime.h`](CGHLSLRuntime.h) — HLSL runtime: buffer/resource
binding、intrinsics (`GENERATE_HLSL_INTRINSIC_FUNCTION` 宏)、root
signature、DXIL/SPIR-V dispatch。
- 上游: [`CGDecl.cpp`](CGDecl.cpp), [`CGExpr.cpp`](CGExpr.cpp),
  [`CGExprScalar.cpp`](CGExprScalar.cpp),
  [`CGExprAgg.cpp`](CGExprAgg.cpp),
  [`CGExprConstant.cpp`](CGExprConstant.cpp),
  [`CodeGenModule.cpp`](CodeGenModule.cpp),
  [`CGHLSLBuiltins.cpp`](CGHLSLBuiltins.cpp),
  [`CGBuiltin.cpp`](CGBuiltin.cpp)。
- 下游: [`CGDebugInfo.h`](CGDebugInfo.h),
  [`CGRecordLayout.h`](CGRecordLayout.h),
  [`CodeGenFunction.h`](CodeGenFunction.h),
  [`CodeGenModule.h`](CodeGenModule.h),
  [`HLSLBufferLayoutBuilder.h`](HLSLBufferLayoutBuilder.h),
  [`TargetInfo.h`](TargetInfo.h),
  [`clang/AST/HLSLResource.h`](../../include/clang/AST/HLSLResource.h),
  [`clang/AST/RecursiveASTVisitor.h`](../../include/clang/AST/RecursiveASTVisitor.h),
  [`llvm/Frontend/HLSL/HLSLResource.h`](../../../llvm/include/llvm/Frontend/HLSL/HLSLResource.h),
  [`llvm/Frontend/HLSL/RootSignatureMetadata.h`](../../../llvm/include/llvm/Frontend/HLSL/RootSignatureMetadata.h),
  [`llvm/IR/Metadata.h`](../../../llvm/include/llvm/IR/Metadata.h),
  [`llvm/Transforms/Utils/ModuleUtils.h`](../../../llvm/include/llvm/Transforms/Utils/ModuleUtils.h)。
- 关键类/函数: `class CGHLSLRuntime`, `class CGHLSLOffsetInfo`,
  `GENERATE_HLSL_INTRINSIC_FUNCTION`, `addDxilValVersion`,
  `emitResourceHandleFromBinding`, `emitBufferMethods`,
  `emitResourceBinding`, `emitRootSignature`。

[`CGHLSLBuiltins.cpp`](CGHLSLBuiltins.cpp) — HLSL-specific `__builtin_*`
lowering (`asdouble`、`clip`、dot/mul/lerp/etc.)。经
[`CGBuiltin.cpp`](CGBuiltin.cpp) 调用。
- 上游: [`CGBuiltin.cpp`](CGBuiltin.cpp) (经 `EmitHLSLBuiltinExpr`)。
- 下游: [`CGBuiltin.h`](CGBuiltin.h),
  [`CGHLSLRuntime.h`](CGHLSLRuntime.h),
  [`CodeGenFunction.h`](CodeGenFunction.h),
  [`clang/AST/HLSLResource.h`](../../include/clang/AST/HLSLResource.h),
  [`clang/AST/MatrixUtils.h`](../../include/clang/AST/MatrixUtils.h),
  [`llvm/IR/MatrixBuilder.h`](../../../llvm/include/llvm/IR/MatrixBuilder.h)。
- 关键类/函数: `handleAsDoubleBuiltin`, `handleHlslClip`,
  `EmitHLSLBuiltinExpr`。

[`HLSLBufferLayoutBuilder.cpp`](HLSLBufferLayoutBuilder.cpp) +
[`HLSLBufferLayoutBuilder.h`](HLSLBufferLayoutBuilder.h) — 构造
cbuffer/tbuffer layout: 16 字节 row padding、honour
`packoffset` / `register(c#)`、base class flattening。
- 上游: [`CGHLSLRuntime.cpp`](CGHLSLRuntime.cpp),
  [`CodeGenModule.cpp`](CodeGenModule.cpp) (HLSL 路径)。
- 下游: [`CGHLSLRuntime.h`](CGHLSLRuntime.h),
  [`CodeGenModule.h`](CodeGenModule.h),
  [`TargetInfo.h`](TargetInfo.h),
  [`clang/AST/Type.h`](../../include/clang/AST/Type.h)。
- 关键类/函数: `class HLSLBufferLayoutBuilder`, `layOutStruct`,
  `layOutArray`, `layOutMatrix`, `layOutType`, `padArrayElements`。

[`CodeGenSYCL.cpp`](CodeGenSYCL.cpp) — SYCL kernel caller offload
entry-point 生成 (`sycl_kernel_entry_point`)。
- 上游: [`CodeGenFunction.cpp`](CodeGenFunction.cpp) (经
  `EmitSYCLKernelCallStmt`)。
- 下游: [`CodeGenFunction.h`](CodeGenFunction.h),
  [`CodeGenModule.h`](CodeGenModule.h),
  [`clang/Basic/DiagnosticFrontend.h`](../../include/clang/Basic/DiagnosticFrontend.h),
  [`llvm/Frontend/Offloading/OffloadWrapper.h`](../../../llvm/include/llvm/Frontend/Offloading/OffloadWrapper.h),
  [`llvm/Support/MemoryBuffer.h`](../../../llvm/include/llvm/Support/MemoryBuffer.h)。
- 关键类/函数: `CodeGenFunction::EmitSYCLKernelCallStmt`,
  `CodeGenModule::EmitSYCLKernelCaller`, `SetSYCLKernelAttributes`。

### 3.13 Constant init / Coverage / Sanitizer / Non-trivial

[`ConstantEmitter.h`](ConstantEmitter.h) — helper, 把 expression /
APValue / ConstantExpr 降为 `llvm::Constant*`; 公开 API 声明。
- 上游: [`CGExpr.cpp`](CGExpr.cpp), [`CGExprAgg.cpp`](CGExprAgg.cpp),
  [`CGExprConstant.cpp`](CGExprConstant.cpp),
  [`CGExprScalar.cpp`](CGExprScalar.cpp),
  [`CGExprComplex.cpp`](CGExprComplex.cpp),
  [`CGObjC.cpp`](CGObjC.cpp), [`CGDecl.cpp`](CGDecl.cpp),
  [`CGVTables.cpp`](CGVTables.cpp)。
- 下游: [`CodeGenFunction.h`](CodeGenFunction.h),
  [`CodeGenModule.h`](CodeGenModule.h)。
- 关键类/函数: `class ConstantEmitter`, `tryEmitForInitializer`,
  `tryEmitAbstract`, `emitAbstract`, `tryEmitAbstractForMemory`,
  `tryEmitConstantSignedPointer`, `finalize`, `tryEmitConstantExpr`。

[`ConstantInitBuilder.cpp`](ConstantInitBuilder.cpp) — out-of-line
methods, 对应 [`ConstantInitBuilder.h`](../../include/clang/CodeGen/ConstantInitBuilder.h)
— global-init builder for vtables / RTTI, sealed
`ConstantInitFuture`。
- 上游: 通过公开 header
  [`ConstantInitBuilder.h`](../../include/clang/CodeGen/ConstantInitBuilder.h)
  用。
- 下游: [`clang/CodeGen/ConstantInitBuilder.h`](../../include/clang/CodeGen/ConstantInitBuilder.h),
  [`CodeGenModule.h`](CodeGenModule.h)。
- 关键类/函数: `class ConstantInitFuture`,
  `ConstantInitBuilderBase::createFuture`, `installInGlobal`,
  `createGlobal`, `resolveSelfReferences`。

[`CoverageMappingGen.cpp`](CoverageMappingGen.cpp) +
[`CoverageMappingGen.h`](CoverageMappingGen.h) — Source-based code
coverage (`-fprofile-instr-generate`) mapping region 生成, 含 MC/DC。
- 上游: [`CodeGenFunction.cpp`](CodeGenFunction.cpp),
  [`CodeGenModule.cpp`](CodeGenModule.cpp),
  [`CodeGenPGO.cpp`](CodeGenPGO.cpp)。
- 下游: [`CodeGenFunction.h`](CodeGenFunction.h),
  [`CodeGenPGO.h`](CodeGenPGO.h),
  [`clang/AST/StmtVisitor.h`](../../include/clang/AST/StmtVisitor.h),
  [`clang/Basic/DiagnosticFrontend.h`](../../include/clang/Basic/DiagnosticFrontend.h),
  [`clang/Lex/Lexer.h`](../../include/clang/Lex/Lexer.h),
  [`llvm/ProfileData/Coverage/CoverageMapping.h`](../../../llvm/include/llvm/ProfileData/Coverage/CoverageMapping.h),
  [`llvm/ProfileData/Coverage/CoverageMappingWriter.h`](../../../llvm/include/llvm/ProfileData/Coverage/CoverageMappingWriter.h),
  [`llvm/Support/FileSystem.h`](../../../llvm/include/llvm/Support/FileSystem.h),
  [`llvm/Support/Path.h`](../../../llvm/include/llvm/Support/Path.h)。
- 关键类/函数: `class CoverageMappingGen`,
  `class CoverageMappingModuleGen`, `class CoverageSourceInfo`,
  `struct SkippedRange`, `setUpCoverageCallbacks`,
  `emitCounterMapping`, `emitEmptyMapping`, `addFunctionMappingRecord`。

[`SanitizerMetadata.cpp`](SanitizerMetadata.cpp) +
[`SanitizerMetadata.h`](SanitizerMetadata.h) — 每全局 LLVM
`SanitizerMetadata` flag (NoAddress、NoHWAddress、IsDynInit、Memtag)
用于 sanitizer pass。
- 上游: [`CodeGenModule.cpp`](CodeGenModule.cpp)。
- 下游: [`CodeGenModule.h`](CodeGenModule.h),
  [`clang/AST/Attr.h`](../../include/clang/AST/Attr.h),
  [`clang/AST/Type.h`](../../include/clang/AST/Type.h)。
- 关键类/函数: `class SanitizerMetadata`, `reportGlobal`,
  `disableSanitizerForGlobal`, `isAsanHwasanMemTagOrTysan`,
  `expandKernelSanitizerMasks`。

[`SanitizerHandler.h`](SanitizerHandler.h) — X-macro 列表, 定义 UBSan
trap handler (`enum SanitizerHandler`)。
- 上游: [`CGExprScalar.cpp`](CGExprScalar.cpp),
  [`CodeGenFunction.cpp`](CodeGenFunction.cpp) (经 `TrapReasonBuilder`)。
- 下游: `LIST_SANITIZER_CHECKS` 宏。
- 关键类/函数: `enum SanitizerHandler`, `SANITIZER_CHECK` 宏
  (`AddOverflow`、`NullabilityArg` 等)。

[`TrapReasonBuilder.cpp`](TrapReasonBuilder.cpp) +
[`TrapReasonBuilder.h`](TrapReasonBuilder.h) — 把诊断消息/category
捕获进 `TrapReason`, 出现于 trap 点 (ubsan 等)。
- 上游: [`CodeGenFunction.cpp`](CodeGenFunction.cpp),
  [`CGExprScalar.cpp`](CGExprScalar.cpp)。
- 下游: [`clang/Basic/Diagnostic.h`](../../include/clang/Basic/Diagnostic.h)。
- 关键类/函数: `class TrapReason`, `class TrapReasonBuilder`,
  `getMessage`, `getCategory`。

[`VarBypassDetector.cpp`](VarBypassDetector.cpp) +
[`VarBypassDetector.h`](VarBypassDetector.h) — 检测 jump-over-VarDecl
(如 `goto L; int x; L:`) 用于正确的 cleanup。
- 上游: [`CodeGenFunction.cpp`](CodeGenFunction.cpp)。
- 下游: [`CodeGenModule.h`](CodeGenModule.h),
  [`clang/AST/Decl.h`](../../include/clang/AST/Decl.h),
  [`clang/AST/Stmt.h`](../../include/clang/AST/Stmt.h)。
- 关键类/函数: `class VarBypassDetector`, `Init`, `IsBypassed`,
  `BuildScopeInformation`, `Detect`。

[`PatternInit.cpp`](PatternInit.cpp) + [`PatternInit.h`](PatternInit.h)
— `initializationPatternFor` 产生未初始化 stack value 的 sentinel
(Clang 的 `0xAA` / `0xFF` pattern, qNaN for float)。
- 上游: [`CGDecl.cpp`](CGDecl.cpp),
  [`CGBuiltin.cpp`](CGBuiltin.cpp)。
- 下游: [`CodeGenModule.h`](CodeGenModule.h),
  [`clang/Basic/TargetInfo.h`](../../include/clang/Basic/TargetInfo.h),
  [`llvm/IR/Constant.h`](../../../llvm/include/llvm/IR/Constant.h),
  [`llvm/IR/Type.h`](../../../llvm/include/llvm/IR/Type.h)。
- 关键类/函数: `initializationPatternFor`。

### 3.14 Type cache / Layout / Builder / Address

[`Address.h`](Address.h) — `RawAddress` + signed `Address` 包装,
pointer + element type + alignment + signed-pointer auth。
- 上游: [`CGBuilder.h`](CGBuilder.h),
  [`CGValue.h`](CGValue.h), [`CGCleanup.h`](CGCleanup.h),
  [`MCDCState.h`](MCDCState.h),
  [`CodeGenFunction.h`](CodeGenFunction.h),
  [`CGCall.h`](CGCall.h)。
- 下游: [`CGPointerAuthInfo.h`](CGPointerAuthInfo.h),
  [`clang/AST/CharUnits.h`](../../include/clang/AST/CharUnits.h),
  [`llvm/ADT/PointerIntPair.h`](../../../llvm/include/llvm/ADT/PointerIntPair.h),
  [`llvm/IR/Constants.h`](../../../llvm/include/llvm/IR/Constants.h),
  [`llvm/Support/MathExtras.h`](../../../llvm/include/llvm/Support/MathExtras.h)。
- 关键类/函数: `class RawAddress`, `class Address`,
  `enum KnownNonNull_t`, `withElementType`, `emitRawPointer`,
  `getElementType`, `getAddressSpace`。

[`CGBuilder.h`](CGBuilder.h) — `IRBuilder` 子类, 通过
`CodeGenFunction::InsertHelper` 转发 insert, 用于 debug-info
attachment。
- 上游: [`CodeGenFunction.h`](CodeGenFunction.h) (成员),
  所有 `CG*.cpp`。
- 下游: [`Address.h`](Address.h), [`CGValue.h`](CGValue.h),
  [`CodeGenModule.h`](CodeGenModule.h),
  [`CodeGenTypeCache.h`](CodeGenTypeCache.h),
  [`llvm/Analysis/TargetFolder.h`](../../../llvm/include/llvm/Analysis/TargetFolder.h),
  [`llvm/IR/IRBuilder.h`](../../../llvm/include/llvm/IR/IRBuilder.h),
  [`llvm/IR/Type.h`](../../../llvm/include/llvm/IR/Type.h)。
- 关键类/函数: `class CGBuilderTy`, `class CGBuilderInserter`,
  `InsertHelper`, `createConstGEP2_32`, `TypeCache`。

[`CGValue.h`](CGValue.h) — `RValue` / `LValue` / `AggValueSlot`
包 `llvm::Value*` 的包装, 加 `LValueBaseInfo` / `TBAAAccessInfo`。
- 上游: [`CGBuilder.h`](CGBuilder.h),
  [`CGCall.h`](CGCall.h), [`CGObjCRuntime.h`](CGObjCRuntime.h),
  [`CodeGenFunction.h`](CodeGenFunction.h),
  [`CGBlocks.h`](CGBlocks.h), [`EHScopeStack.h`](EHScopeStack.h),
  [`CGCleanup.h`](CGCleanup.h), [`CGCXXABI.h`](CGCXXABI.h)。
- 下游: [`Address.h`](Address.h),
  [`CGPointerAuthInfo.h`](CGPointerAuthInfo.h),
  [`CodeGenTBAA.h`](CodeGenTBAA.h),
  [`EHScopeStack.h`](EHScopeStack.h),
  [`clang/AST/ASTContext.h`](../../include/clang/AST/ASTContext.h),
  [`clang/AST/Type.h`](../../include/clang/AST/Type.h),
  [`llvm/IR/Type.h`](../../../llvm/include/llvm/IR/Type.h),
  [`llvm/IR/Value.h`](../../../llvm/include/llvm/IR/Value.h)。
- 关键类/函数: `class RValue`, `class LValue`, `class AggValueSlot`,
  `enum ARCPreciseLifetime_t`, `enum AlignmentSource`,
  `LValueBaseInfo`, `MakeAddr`, `MakeBitfield`, `MakeVectorElt`,
  `MakeMatrixRow`。

[`QualTypeMapper.cpp`](QualTypeMapper.cpp) + [`QualTypeMapper.h`](QualTypeMapper.h)
— 把 `clang::QualType` 映射到新的 `llvm::abi::Type` 表示
([`llvm/ABI/`](../../../llvm/include/llvm/ABI/) 中, SVE、base ABI
lowering)。
- 上游: [`CGCall.cpp`](CGCall.cpp),
  [`CodeGenModule.cpp`](CodeGenModule.cpp)。
- 下游: [`clang/AST/RecordLayout.h`](../../include/clang/AST/RecordLayout.h),
  [`clang/AST/TypeOrdering.h`](../../include/clang/AST/TypeOrdering.h),
  [`clang/Basic/TargetInfo.h`](../../include/clang/Basic/TargetInfo.h),
  [`llvm/ABI/Types.h`](../../../llvm/include/llvm/ABI/Types.h),
  [`llvm/Support/Alignment.h`](../../../llvm/include/llvm/Support/Alignment.h),
  [`llvm/Support/ErrorHandling.h`](../../../llvm/include/llvm/Support/ErrorHandling.h),
  [`llvm/Support/TypeSize.h`](../../../llvm/include/llvm/Support/TypeSize.h)。
- 关键类/函数: `class QualTypeMapper`, `convertType`,
  `convertBuiltinType`, `convertArrayType`, `convertVectorType`,
  `convertSVEBuiltinType`, `convertRecordType`,
  `isSVEPredicateBuiltinType`, `getABIVectorKind`。

[`CGRecordLayout.h`](CGRecordLayout.h) +
[`CGRecordLayoutBuilder.cpp`](CGRecordLayoutBuilder.cpp) — AST
`RecordDecl` → `llvm::StructType` 映射 (complete vs base subobject),
bitfield packing、virtual base layout、zero-init tracking。
- 上游: [`CodeGenTypes.cpp`](CodeGenTypes.cpp),
  [`CGCall.cpp`](CGCall.cpp), [`CGDecl.cpp`](CGDecl.cpp),
  [`CGExprConstant.cpp`](CGExprConstant.cpp),
  [`CGExprScalar.cpp`](CGExprScalar.cpp),
  [`CGVTables.cpp`](CGVTables.cpp),
  [`MicrosoftCXXABI.cpp`](MicrosoftCXXABI.cpp),
  [`ItaniumCXXABI.cpp`](ItaniumCXXABI.cpp)。
- 下游: [`ABIInfoImpl.h`](ABIInfoImpl.h),
  [`CGCXXABI.h`](CGCXXABI.h),
  [`CodeGenTypes.h`](CodeGenTypes.h),
  [`clang/AST/RecordLayout.h`](../../include/clang/AST/RecordLayout.h),
  [`clang/Basic/CodeGenOptions.h`](../../include/clang/Basic/CodeGenOptions.h),
  [`clang/CodeGenUtils/CodeGenUtils.h`](../../include/clang/CodeGenUtils/CodeGenUtils.h),
  [`llvm/IR/DataLayout.h`](../../../llvm/include/llvm/IR/DataLayout.h),
  [`llvm/IR/DerivedTypes.h`](../../../llvm/include/llvm/IR/DerivedTypes.h),
  [`llvm/Support/Debug.h`](../../../llvm/include/llvm/Support/Debug.h)。
- 关键类/函数: `struct CGBitFieldInfo`, `class CGRecordLayout`,
  `struct CGRecordLowering`, `MemberInfo`, `accumulateBitfields`,
  `clipTailPadding`, `buildLayout`, `isZeroInitializable`。

[`TargetInfo.cpp`](TargetInfo.cpp) + [`TargetInfo.h`](TargetInfo.h) —
`TargetCodeGenInfo` 基类 + `SwiftABIInfo` 接线 + per-target hook
(`setTargetAttributes`、`getOpenCLType`、`getHLSLType` 等)。
- 上游: [`CodeGenModule.cpp`](CodeGenModule.cpp),
  [`CGBuiltin.cpp`](CGBuiltin.cpp),
  [`CGCall.cpp`](CGCall.cpp), [`CGStmt.cpp`](CGStmt.cpp),
  每个 [`Targets/*.cpp`](Targets/) (工厂 + 子类)。
- 下游: `ABIInfo.h`, `ABIInfoImpl.h`,
  [`CGBuilder.h`](CGBuilder.h),
  [`CGValue.h`](CGValue.h),
  [`CodeGenModule.h`](CodeGenModule.h),
  [`clang/AST/Type.h`](../../include/clang/AST/Type.h),
  [`clang/Basic/TargetInfo.h`](../../include/clang/Basic/TargetInfo.h),
  [`llvm/TargetParser/AtomicScope.h`](../../../llvm/include/llvm/TargetParser/AtomicScope.h)。
- 关键类/函数: `class TargetCodeGenInfo`, `getSwiftABIInfo`,
  `getABIInfo`, `setTargetAttributes`, `emitTargetMetadata`,
  `getTargetCodeGenInfo` (工厂), `inline getAtomicScope`。

### 3.15 PCH / Module linking

[`ObjectFilePCHContainerWriter.cpp`](ObjectFilePCHContainerWriter.cpp)
— 把 PCH 序列化成 object 文件 (`-cpch-in-object`), 通过发出 type/
variable debug info 到完整的 `llvm::Module`。
- 上游: [`clang/Frontend/CompilerInstance`](../Frontend/CompilerInstance.cpp)
  (PCH write 路径)。
- 下游: [`CGDebugInfo.h`](CGDebugInfo.h),
  [`CodeGenModule.h`](CodeGenModule.h),
  [`clang/AST/RecursiveASTVisitor.h`](../../include/clang/AST/RecursiveASTVisitor.h),
  [`clang/CodeGen/BackendUtil.h`](../../include/clang/CodeGen/BackendUtil.h),
  [`clang/Frontend/CompilerInstance.h`](../../include/clang/Frontend/CompilerInstance.h),
  [`clang/Lex/HeaderSearch.h`](../../include/clang/Lex/HeaderSearch.h),
  [`llvm/IR/Constants.h`](../../../llvm/include/llvm/IR/Constants.h),
  [`llvm/IR/Module.h`](../../../llvm/include/llvm/IR/Module.h),
  [`llvm/MC/TargetRegistry.h`](../../../llvm/include/llvm/MC/TargetRegistry.h),
  [`llvm/Object/COFF.h`](../../../llvm/include/llvm/Object/COFF.h)。
- 关键类/函数: `class PCHContainerGenerator`,
  `class DebugTypeVisitor`, `HandleTranslationUnitDecl`,
  `VisitTypeDecl`, `VisitImportDecl`。

[`ModuleLinker.cpp`](ModuleLinker.cpp) — `loadLinkModules` helper, 读
`-mlink-bitcode-file` bitcode 到 `LinkModule` entry。
- 上游: [`clang/Frontend`](../Frontend/) (codegen pipeline),
  `BackendConsumer.cpp`。
- 下游: [`clang/CodeGen/ModuleLinker.h`](../../include/clang/CodeGen/ModuleLinker.h),
  [`clang/Basic/CodeGenOptions.h`](../../include/clang/Basic/CodeGenOptions.h),
  [`clang/Frontend/CompilerInstance.h`](../../include/clang/Frontend/CompilerInstance.h),
  [`llvm/Bitcode/BitcodeReader.h`](../../../llvm/include/llvm/Bitcode/BitcodeReader.h),
  [`llvm/IR/Module.h`](../../../llvm/include/llvm/IR/Module.h)。
- 关键类/函数: `loadLinkModules`。

[`LinkInModulesPass.cpp`](LinkInModulesPass.cpp) +
[`LinkInModulesPass.h`](LinkInModulesPass.h) — `LinkInModulesPass`,
new-PM `ModulePass`, 调 `BackendConsumer::LinkInModules`。
- 上游: [`BackendUtil.cpp`](BackendUtil.cpp) (加到 pass pipeline)。
- 下游: [`BackendConsumer.h`](BackendConsumer.h),
  [`llvm/IR/PassManager.h`](../../../llvm/include/llvm/IR/PassManager.h)。
- 关键类/函数: `class LinkInModulesPass`, `run`, `BackendConsumer*`。

[`MacroPPCallbacks.cpp`](MacroPPCallbacks.cpp) +
[`MacroPPCallbacks.h`](MacroPPCallbacks.h) — 把 macro 定义记入 debug
info (`-fdebug-macro`), 发 `DIMacroFile` / `DIMacro` 节点。
- 上游: [`CodeGenAction.cpp`](CodeGenAction.cpp) (安装在
  `Preprocessor`)。
- 下游: [`CGDebugInfo.h`](CGDebugInfo.h),
  [`clang/CodeGen/ModuleBuilder.h`](../../include/clang/CodeGen/ModuleBuilder.h),
  [`clang/Lex/MacroInfo.h`](../../include/clang/Lex/MacroInfo.h),
  [`clang/Lex/Preprocessor.h`](../../include/clang/Lex/Preprocessor.h)。
- 关键类/函数: `class MacroPPCallbacks`, `FileScopeStatus`,
  `writeMacroDefinition`, `HandleMacroDefined`, `HandleMacroUndefined`,
  `InclusionDirective`。

### 3.16 `TargetBuiltins/` — 12 per-arch builtin 文件

每个文件实现一个架构的 `__builtin_*` lowering, 经
[`CGBuiltin.cpp::EmitTargetArchBuiltinExpr`](CGBuiltin.cpp) 分发。

| Path | 作用 | 关键函数/类 |
|------|-----|------------|
| [`TargetBuiltins/AMDGPU.cpp`](TargetBuiltins/AMDGPU.cpp) | AMDGPU/R600/SPIRV builtin: buffer-load、flat/global atomic、`s_buffer_load`、dispatch_ptr、printf | `emitAMDGPUSBufferLoadBuiltin`, `EmitAMDGPUDispatchPtr`, `emitBinaryExpMaybeConstrainedFPBuiltin`, `CodeGenFunction::EmitAMDGPUBuiltinExpr` |
| [`TargetBuiltins/ARM.cpp`](TargetBuiltins/ARM.cpp) | ARM/AArch64/ARM64EC builtin: NEON、ACLE (CRC/SHA/AES)、MSVC interlocked、MTE、SVE、SME、内置, `translateAarch64ToMsvcIntrin` | `translateAarch64ToMsvcIntrin`, `CodeGenFunction::EmitAArch64BuiltinExpr`, `EmitARMBuiltinExpr`, `EmitBPFBuiltinExpr`, `EmitAArch64ACLEFunctionCall`, `emitSMEIntrinsic` |
| [`TargetBuiltins/AVR.cpp`](TargetBuiltins/AVR.cpp) | AVR builtin (AVRTiny 变体): `__builtin_avr_nop/sei/cli/sleep/wdr/swap/atomic_*` | `CodeGenFunction::EmitAVRBuiltinExpr`, `avr_nop`, `avr_swap` |
| [`TargetBuiltins/DirectX.cpp`](TargetBuiltins/DirectX.cpp) | DirectX (DXIL) builtin (目前极少): `__builtin_dx_dot2add` | `CodeGenFunction::EmitDirectXBuiltinExpr`, `dx_dot2add` |
| [`TargetBuiltins/Hexagon.cpp`](TargetBuiltins/Hexagon.cpp) | Hexagon HVX builtin: `L2_loadrub_*`、`L2_loadrd_*`、packet/circular load | `getIntrinsicForHexagonNonClangBuiltin`, `CUSTOM_BUILTIN_MAPPING`, `CodeGenFunction::EmitHexagonBuiltinExpr` |
| [`TargetBuiltins/NVPTX.cpp`](TargetBuiltins/NVPTX.cpp) | NVPTX builtin: WMMA load/store (`__hmma_*_ld_*`)、WMMA mma、async-copy、barrier、mbarrier、cluster | `getNVPTXMmaLdstInfo`, `MMA_INTR` / `MMA_LDST` 宏, `CodeGenFunction::EmitNVPTXBuiltinExpr`, `NVPTXMmaLdstInfo` |
| [`TargetBuiltins/PPC.cpp`](TargetBuiltins/PPC.cpp) | PowerPC builtin: `__builtin_ppc_ldarx/lwarx/stwcx`、VSX/AltiVec、MMA、prefetch、atomic | `emitPPCLoadReserveIntrinsic`, `CodeGenFunction::EmitPPCBuiltinExpr`, `PPCVSXBuiltin` |
| [`TargetBuiltins/RISCV.cpp`](TargetBuiltins/RISCV.cpp) | RISC-V builtin: RVV vector (`__riscv_v*` VLA)、SiFive、Andes、THead、scalar crypto (`zk*`)、bitmanip | `emitRVVVLEFFBuiltin`, `RVV_VTA`, `RVV_VMA`, `CodeGenFunction::EmitRISCVBuiltinExpr`, `emitRVVIntrinsic` |
| [`TargetBuiltins/SPIR.cpp`](TargetBuiltins/SPIR.cpp) | SPIR-V builtin: `__builtin_spirv_distance/length/normalize/faceforward/reflect/rsqrt/...` | `CodeGenFunction::EmitSPIRVBuiltinExpr`, `spv_distance`, `spv_length` |
| [`TargetBuiltins/SystemZ.cpp`](TargetBuiltins/SystemZ.cpp) | SystemZ/s390x builtin: vector (vec_*) helper、transactional-memory、z/OS `__cs1` codegen、CC-return intrinsic | `EmitSystemZIntrinsicWithCC`, `CodeGenFunction::EmitSystemZBuiltinExpr`, `emitSystemZVectorBuiltin` |
| [`TargetBuiltins/WebAssembly.cpp`](TargetBuiltins/WebAssembly.cpp) | WebAssembly builtin: `__builtin_wasm_memory_size/grow`、SIMD lane、atomic、table、ref、threads、exception | `CodeGenFunction::EmitWebAssemblyBuiltinExpr`, `wasm_memory_size`, `wasm_memory_grow`, `wasm_atomic_*` |
| [`TargetBuiltins/X86.cpp`](TargetBuiltins/X86.cpp) | x86 builtin: MMX/SSE/AVX/AVX-512/AMX/F16C/SHA/GFNI/VAES/PCLMULQDQ, `_mm_*`、`_mm256_*`、`_mm512_*`、x86 intrinsic、translate MSVC | `translateX86ToMsvcIntrin`, `emitX86RoundImmediate`, `getMaskVecValue`, `CodeGenFunction::EmitX86BuiltinExpr`, `EmitX86FloatIntrinsic` |

### 3.17 `Targets/` — 25 per-arch ABI / TargetCodeGenInfo 文件

每个文件实现一个架构的 `ABIInfo` 子类 + `TargetCodeGenInfo` 子类,
通过 [`TargetInfo.cpp::getTargetCodeGenInfo`](TargetInfo.cpp) 工厂按
`Triple` 选取。

| Path | 作用 & ABI 子类 | 关键类 | 特殊 ABI 特性 |
|------|--------------|------|-------------|
| [`Targets/AArch64.cpp`](Targets/AArch64.cpp) | AArch64 ABI (AAPCS、AAPCSSoft、DarwinPCS、Windows) | `class AArch64ABIInfo`, `class AArch64SwiftABIInfo`, `class AArch64TargetCodeGenInfo`, `class WindowsAArch64TargetCodeGenInfo` | SVE/SME scalable vector、ARM64EC、AAPCS HFA/SIMD、Win64EC embed `WinX86_64`、`isHomogeneousAggregateBaseType` overload、Swift ABI |
| [`Targets/AMDGPU.cpp`](Targets/AMDGPU.cpp) | AMDGPU/HIP ABI | `class AMDGPUABIInfo final`, `class AMDGPUTargetCodeGenInfo` | Address-space aware (flat/global/local/constant/generic)、kernel ABI、`getOptimalVectorMemoryType`、`MaxNumRegsForArgsRet=16`、kernel-arg address-space coercion |
| [`Targets/ARC.cpp`](Targets/ARC.cpp) | Synopsys ARC (ARCtangent-A5) | `class ARCABIInfo` | 8-reg arg budget via `CCState`、indirect-in-reg pass |
| [`Targets/ARM.cpp`](Targets/ARM.cpp) | ARM (AAPCS、APCS、AAPCS-VFP) | `class ARMABIInfo` | `setCCs`、hard-float/soft-float detection、`IsFloatABISoftFP`、EABI/HF detection、VFP/SVE handling、Swift on Apple、AAPCS bitfield volatility |
| [`Targets/AVR.cpp`](Targets/AVR.cpp) | AVR / AVRTiny ABI | `class AVRABIInfo` | `ParamRegs=18` / `RetRegs=8` (AVR) 或 `6`/`4` (AVRTiny); large-return struct flag |
| [`Targets/BPF.cpp`](Targets/BPF.cpp) | eBPF ABI | `class BPFABIInfo` | Aggregate ≤64b in 1 reg; ≤128b in 2 regs; else indirect; pointer always in reg |
| [`Targets/CSKY.cpp`](Targets/CSKY.cpp) | C-SKY ABI | `class CSKYABIInfo`, `XLen=32`, `FLen` | NumArgGPRs=4, NumArgFPRs=4; ELF PSABI |
| [`Targets/DirectX.cpp`](Targets/DirectX.cpp) | DirectX target (DXIL) | `class DirectXTargetCodeGenInfo` | HLSL-aware: `getHLSLType`, `getHLSLPadding` (用 `dx.Padding` target-ext type) |
| [`Targets/Hexagon.cpp`](Targets/Hexagon.cpp) | Hexagon ABI | `class HexagonABIInfo` | `EmitVAArgForHexagon`, Linux vs non-Linux VAArg 路径 |
| [`Targets/Lanai.cpp`](Targets/Lanai.cpp) | Lanai ABI | `class LanaiABIInfo` | `regparm` honoured; default 4 arg regs |
| [`Targets/LoongArch.cpp`](Targets/LoongArch.cpp) | LoongArch LP64D/LP64 ABI | `class LoongArchABIInfo`, `GRLen`, `FRLen` | 8 GPRs + 8 FPRs, `detectFARsEligibleStructHelper` |
| [`Targets/M68k.cpp`](Targets/M68k.cpp) | Motorola 68000 ABI | `class M68kTargetCodeGenInfo` | `M68kInterruptAttr` → `llvm::CallingConv::M68k_INTR` |
| [`Targets/MSP430.cpp`](Targets/MSP430.cpp) | MSP430 ABI | `class MSP430ABIInfo` | 自定义 `_Complex` ABI (无 flattening) |
| [`Targets/Mips.cpp`](Targets/Mips.cpp) | MIPS o32/n32/n64 ABI | `class MipsABIInfo` | `IsO32`、stack-align special、`isComplexGnuABI` (post-Clang-23 GCC compat)、FPR/GPR pairs for complex |
| [`Targets/NVPTX.cpp`](Targets/NVPTX.cpp) | NVPTX target | `class NVPTXABIInfo`, `class NVPTXTargetCodeGenInfo` | Kernel ABI、address-space handling、NVVM attribute emission、sync-scope forwarding、`getDeviceKernelCallingConv` |
| [`Targets/PPC.cpp`](Targets/PPC.cpp) | PowerPC (ELF、AIX、Darwin) | `class PPCABIInfo` | ELFv2 homogeneous aggregate、AIX stack parity、complex vaarg、AltiVec/VSX vector 参数 |
| [`Targets/RISCV.cpp`](Targets/RISCV.cpp) | RISC-V ILP32/ILP32F/ILP32D/LP64/LP64D/LP64Q ABI | `class RISCVABIInfo`, `XLen`, `FLen` | 8 GPRs + 8 FPRs, RVV VLS/VLA FPCC struct, `detectFPCCEligibleStructHelper`, EABI |
| [`Targets/SPIR.cpp`](Targets/SPIR.cpp) | SPIR & SPIR-V 通用 ABI | `class CommonSPIRABIInfo`, `class SPIRVABIInfo`, `class SPIRTargetCodeGenInfo`, `class SPIRVTargetCodeGenInfo` | `setCCs` → OpenCL kernel CC、OpenCL type 经 target-ext、address-space (32-bit) |
| [`Targets/Sparc.cpp`](Targets/Sparc.cpp) | SPARC V8 ABI | `class SparcV8ABIInfo` | `_Complex` register passing (`isComplexGnuABI`) |
| [`Targets/SystemZ.cpp`](Targets/SystemZ.cpp) | s390x ELF ABI | `class SystemZABIInfo` | Soft-float detection、vector ABI (HasVector)、`getFPArgumentType` |
| [`Targets/TCE.cpp`](Targets/TCE.cpp) | TTA-based 自定义 arch (TCE) | `class TCETargetCodeGenInfo` | OpenCL attribute forwarding、默认 ABI |
| [`Targets/VE.cpp`](Targets/VE.cpp) | NEC SX-Aurora TSUBASA VE | `class VEABIInfo` | 自定义 `_Complex` 规则、小 int promotion |
| [`Targets/WebAssembly.cpp`](Targets/WebAssembly.cpp) | WASM ABI (MVP + exceptions) | `class WebAssemblyABIInfo`, `class WebAssemblyTargetCodeGenInfo` | Multivalue + reference type + stringret + named-block CC、default Info fallback、Swift ABI、`WasmTables` |
| [`Targets/X86.cpp`](Targets/X86.cpp) | x86-32 + x86-64 SysV + Win64 + Win32 | `class X86_32ABIInfo`, `class X86_32SwiftABIInfo`, `class X86_32TargetCodeGenInfo`, `class X86_64ABIInfo`, `class WinX86_64ABIInfo`, `class X86_64TargetCodeGenInfo`, `class WinX86_32TargetCodeGenInfo`, `class WinX86_64TargetCodeGenInfo` | AVX/AVX-512 vectorcall (`X86_64ABIInfo::getX86ABIAVXLevel`)、SEH (`WinX86_*`)、`__m64` MMX、indirect-aliased、RegCall |
| [`Targets/XCore.cpp`](Targets/XCore.cpp) | XCore XS1 ABI | `class XCoreTargetCodeGenInfo`, `SmallStringEnc`, `TypeStringCache` | 自定义 type-encoding for XCore metadata; rec/non-rec cache |

---

## §4. 关键调用链

### 4.1 顶层 driver → CodeGen → backend

```
clang driver (-emit-obj/-emit-llvm/-emit-bc/-emit-asm)
  └─ clang::ExecuteCompilerInvocation [clang/tools/driver/]
       └─ CompilerInstance::ExecuteAction
            └─ CodeGenAction::ExecuteAction [clang/lib/CodeGen/CodeGenAction.cpp]
                 ├─ 1. BackendConsumer 创建 + CodeGeneratorImpl::Initialize
                 │     ├─ createTargetMachine (Setup LLVM Target)
                 │     ├─ CodeGenModule::Create (CGM, TargetCodeGenInfo,
                 │     │   ABIInfo, CGCXXABI, CGObjCRuntime,
                 │     │   CGOpenMPRuntime, CGHLSLRuntime, ...)
                 │     └─ emit LLVM Module 名 + target triple + datalayout
                 ├─ 2. ASTConsumer::HandleTranslationUnit
                 │     ├─ ModuleBuilder::HandleTopLevelDecl (per decl)
                 │     │    ├─ CodeGenModule::EmitTopLevelDecl
                 │     │    │    ├─ function → defer / emit
                 │     │    │    │    └─ CodeGenModule::EmitGlobalFunctionDefinition
                 │     │    │    │         └─ CodeGenFunction::StartFunction
                 │     │    │    │              ├─ CodeGenTypes::GetFunctionType
                 │     │    │    │              ├─ CodeGenModule::SetFunctionAttributes
                 │     │    │    │              ├─ CGDebugInfo::EmitFunctionDecl
                 │     │    │    │              └─ CodeGenFunction::EmitStmt (per body)
                 │     │    │    │                   ├─ EmitCompoundStmt → EmitDeclStmt / EmitExprStmt
                 │     │    │    │                   ├─ EmitIfStmt / EmitForStmt / EmitWhileStmt
                 │     │    │    │                   ├─ EmitCallExpr → EmitCall → CGCall
                 │     │    │    │                   ├─ EmitBinaryOperator → ScalarExprEmitter
                 │     │    │    │                   ├─ EmitCXXConstructExpr → CGCXXABI
                 │     │    │    │                   ├─ EmitObjCMessageExpr → CGObjCRuntime
                 │     │    │    │                   └─ EmitOMPParallelDirective → CGOpenMPRuntime
                 │     │    │    │              └─ CodeGenFunction::FinishFunction
                 │     │    │    └─ globals → EmitGlobal / EmitGlobalVarDecl
                 │     │    │       └─ ConstantInitBuilder / CGVTables / CGCXX
                 │     │    └─ HandleTagDecl (per struct/union/class)
                 │     │       └─ CGRecordLayoutBuilder (cached)
                 │     └─ HandleInterestingDecl / CompleteTentativeDefinition
                 ├─ 3. ModuleBuilder::EmitDeferredDecls
                 ├─ 4. CodeGenModule::Release (finalize globals, dispose ctor list)
                 ├─ 5. BackendConsumer (Post-AST pipeline)
                 │    └─ BackendUtil::EmitAssemblyHelper
                 │         ├─ setupTargetMachine
                 │         ├─ RunPasses (新 PM PassBuilder)
                 │         │    ├─ 必备: AlwaysInliner, SROA, InstCombine,
                 │         │    │  LoopVectorize, SimplifyCFG, ...
                 │         │    ├─ 命令行驱动: O0/O1/O2/Os/Oz
                 │         │    ├─ sanitizer: -fsanitize=address/memory/...
                 │         │    ├─ PGO: -fprofile-instr-generate/use
                 │         │    ├─ ThinLTO: -flto=thin
                 │         │    └─ HipStdPar: -fhipstdpar
                 │         ├─ EmbedBitcode (for LTO)
                 │         └─ 写出 .bc/.ll/.s/.o
                 └─ 6. llvm Module 被 Linker 或 driver 接管
```

### 4.2 ABI 调用链 (`CGCall.cpp` + `Targets/<Arch>.cpp`)

```
CodeGenTypes::arrangeLLVMFunctionInfo [CodeGenTypes.cpp]
  └─ ABIInfo::computeInfo (per target, Targets/<Arch>.cpp)
       ├─ X86_64ABIInfo::computeInfo    [Targets/X86.cpp]
       ├─ AArch64ABIInfo::computeInfo   [Targets/AArch64.cpp]
       ├─ ARMABIInfo::computeInfo       [Targets/ARM.cpp]
       ├─ RISCVABIInfo::computeInfo     [Targets/RISCV.cpp]
       ├─ AMDGPUABIInfo::computeInfo    [Targets/AMDGPU.cpp]
       ├─ WebAssemblyABIInfo::computeInfo [Targets/WebAssembly.cpp]
       └─ DefaultABIInfo::computeInfo   [ABIInfoImpl.cpp] (fallback)
            └─ 选 calling conv (ClangCallConvToLLVMCallConv)
            └─ 决定每个参数 ABIArgInfo (Direct / Indirect / Ignore /
                 CoerceToMem / Expand / InAlloca / IndirectAliased)
            └─ HFA / Vector / HVA 探测 (用 isHomogeneousAggregate)
            └─ Swift ABI 调用 (Targets/<Arch>.cpp 的
                 getSwiftABIInfo → SwiftCallingConv)
  └─ 返回 CGFunctionInfo { RetInfo, Args[] }
       └─ CodeGenFunction::EmitCall [CodeGenFunction.cpp]
            └─ EmitCallArgs → CallArgList (CGCall.cpp)
                 └─ 对每个 arg 用 ABIArgInfo 决定 emit 方式
                      ├─ Direct → 直接 emit value
                      ├─ Indirect → store 到 byval arg slot
                      ├─ CoerceToMem → bitcast + store
                      ├─ Expand → 按 LLVM type 展开 (one or more)
                      ├─ InAlloca → push 到 inalloca stack
                      └─ IndirectAliased → 同 Indirect + alias marker
            └─ emit Invoke / CallSite (set attributes, fast-math, ...)
```

### 4.3 C++ ABI 调用链 (vtable + VTT)

```
CodeGenModule::EmitTopLevelDecl (CXXRecordDecl)
  └─ CodeGenModule::EmitVTable (调度)
            └─ CodeGenVTables::GetAddrOfVTable [CGVTables.cpp]
                 └─ CGCXXABI::EmitVTable (Itanium / Microsoft 派发)
                      ├─ ItaniumCXXABI::EmitVTable [ItaniumCXXABI.cpp]
                      │    ├─ 构造 vtable components (vfunc ptrs, RTTI)
                      │    ├─ ItaniumRTTIBuilder
                      │    ├─ thunks: maybeEmitThunk
                      │    └─ 生成 GlobalObject + initializer
                      └─ MicrosoftCXXABI::EmitVTable [MicrosoftCXXABI.cpp]
                           ├─ 构造 vbtable (BaseClassDescriptor, vbptr offsets)
                           ├─ CompleteObjectLocator
                           └─ CatchableType / ThrowInfo 链接
                 └─ CGVTT.cpp::EmitVTTDefinition (仅 Itanium)
                      └─ VTTBuilder [clang/AST/VTTBuilder.h]
```

### 4.4 OpenMP 调度

```
CodeGenFunction::EmitStmt (OMPExecutableDirective)
  └─ CGStmtOpenMP.cpp 路由到具体 Emit*OMP*Directive
       ├─ EmitOMPParallelDirective
       │    └─ CGOpenMPRuntime::emitParallel [CGOpenMPRuntime.cpp]
       │         ├─ 构造 outlined 函数 (lambda + emission)
       │         ├─ capture alloca / shared variable copy
       │         ├─ emit call __kmpc_fork_call (host)
       │         │  或 emitKernel (GPU, CGOpenMPRuntimeGPU.cpp)
       │         └─ debug info: CGDebugInfo::EmitFunctionDecl
       ├─ EmitOMPForDirective → CGOpenMPRuntime::emitFor
       ├─ EmitOMPTargetDirective → emitTarget
       ├─ EmitOMPReductionClause → emitReduction
       └─ EmitOMPTeamsDirective → emitTeams
```

### 4.5 Builtin 调用链

```
CodeGenFunction::EmitCallExpr (CallExpr to __builtin_*)
  └─ CodeGenFunction::EmitBuiltinExpr
       └─ CGBuiltin.cpp::EmitBuiltinExpr (主 switch)
            ├─ LLVM IR intrinsic (e.g. __builtin_sqrt → llvm.sqrt)
            ├─ Atomics: __atomic_load → CGAtomic.cpp::EmitAtomicOp
            ├─ ObjC: __builtin_objc_msgSend → CGObjCRuntime
            ├─ HLSL: CGHLSLBuiltins.cpp::EmitHLSLBuiltinExpr
            └─ Target-specific:
                 ├─ EmitX86BuiltinExpr    [TargetBuiltins/X86.cpp]
                 ├─ EmitAArch64BuiltinExpr [TargetBuiltins/ARM.cpp]
                 ├─ EmitRISCVBuiltinExpr  [TargetBuiltins/RISCV.cpp]
                 ├─ EmitNVPTXBuiltinExpr  [TargetBuiltins/NVPTX.cpp]
                 ├─ EmitAMDGPUBuiltinExpr [TargetBuiltins/AMDGPU.cpp]
                 ├─ EmitHexagonBuiltinExpr [TargetBuiltins/Hexagon.cpp]
                 ├─ EmitPPCBuiltinExpr    [TargetBuiltins/PPC.cpp]
                 ├─ EmitAVRBuiltinExpr    [TargetBuiltins/AVR.cpp]
                 ├─ EmitWebAssemblyBuiltinExpr [TargetBuiltins/WebAssembly.cpp]
                 ├─ EmitSystemZBuiltinExpr [TargetBuiltins/SystemZ.cpp]
                 ├─ EmitSPIRVBuiltinExpr  [TargetBuiltins/SPIR.cpp]
                 └─ EmitDirectXBuiltinExpr [TargetBuiltins/DirectX.cpp]
```

### 4.6 Debug info 调用链

```
CodeGenFunction::EmitDecl (VarDecl)
  └─ CGDebugInfo::EmitDeclare (auto var)
            └─ DBuilder.createLocalVariable + insertDeclare
CodeGenFunction::EmitStmt (含 Location)
  └─ ApplyDebugLocation (RAII) → DBuilder.setSourceLocation
CodeGenModule::EmitFunctionDefinition
  └─ CGDebugInfo::EmitFunctionDecl → DBuilder.createFunction +
       createSubroutineType + apply attribute
CodeGenModule::EmitGlobalVariable (VarDecl globals)
  └─ CGDebugInfo::EmitGlobalVariable → DBuilder.createGlobalVariable
CGDeclCXX.cpp / ItaniumCXXABI.cpp / MicrosoftCXXABI.cpp
  └─ CGDebugInfo::EmitType (记录 RTTI / vtable 等)
            └─ DBuilder.createStructType / createUnionType /
                 createPointerType / createArrayType
```

---

## §5. 推荐阅读顺序

### 阶段 1: 框架入门 (1-2 小时)
1. [`CodeGenModule.cpp`](CodeGenModule.cpp) — 顶层 TU 状态,
   看懂 `EmitTopLevelDecl` / `EmitGlobal` / `getMangledName`。
2. [`CodeGenFunction.cpp`](CodeGenFunction.cpp) — per-function
   codegen 状态, 看懂 `StartFunction` / `EmitStmt` / `FinishFunction`。
3. [`CodeGenTypes.cpp`](CodeGenTypes.cpp) + [`CGRecordLayout.h`](CGRecordLayout.h) +
   [`CodeGenTypeCache.h`](CodeGenTypeCache.h) — AST → LLVM type 降低。
4. [`CodeGenAction.cpp`](CodeGenAction.cpp) + [`BackendConsumer.h`](BackendConsumer.h) +
   [`BackendUtil.cpp`](BackendUtil.cpp) — FrontendAction / LLVM PassManager 衔接。

### 阶段 2: Stmt / Expr 深入 (2-3 小时)
- [`CGStmt.cpp`](CGStmt.cpp) — stmt dispatcher
- [`CGExpr.cpp`](CGExpr.cpp) — expr dispatcher
- [`CGExprScalar.cpp`](CGExprScalar.cpp) — 最大的 expr emitter
- [`CGExprAgg.cpp`](CGExprAgg.cpp) — aggregate init/copy
- [`CGDecl.cpp`](CGDecl.cpp) — VarDecl emission

### 阶段 3: ABI (1-2 小时, 任选一个 arch)
1. [`ABIInfo.h`](ABIInfo.h) + [`ABIInfo.cpp`](ABIInfo.cpp) +
   [`ABIInfoImpl.cpp`](ABIInfoImpl.cpp) — 通用 ABI 框架。
2. [`CGCall.cpp`](CGCall.cpp) — calling-conv 中心。
3. [`Targets/X86.cpp`](Targets/X86.cpp) — 最丰富的 ABI 示例
   (x86-32 / x86-64 SysV / Win64 / Win32)。
4. 或 [`Targets/AArch64.cpp`](Targets/AArch64.cpp) — AAPCS + SVE。

### 阶段 4: C++ (2 小时)
1. [`CGCXX.cpp`](CGCXX.cpp) + [`CGCXXABI.cpp`](CGCXXABI.cpp) —
   抽象 + 通用逻辑。
3. [`ItaniumCXXABI.cpp`](ItaniumCXXABI.cpp) — Itanium (Linux/Mac)。
4. [`MicrosoftCXXABI.cpp`](MicrosoftCXXABI.cpp) — MSVC (Windows)。
5. [`CGVTables.cpp`](CGVTables.cpp) + [`CGVTT.cpp`](CGVTT.cpp) —
   vtable / VTT。

### 阶段 5: 调试信息 (1 小时)
1. [`CGDebugInfo.h`](CGDebugInfo.h) — API 接口。
2. [`CGDebugInfo.cpp`](CGDebugInfo.cpp) — `DIBuilder` 构造细节。

### 阶段 6: Builtin (按需, 1-2 小时)
1. [`CGBuiltin.cpp`](CGBuiltin.cpp) — 主分发。
2. [`TargetBuiltins/X86.cpp`](TargetBuiltins/X86.cpp) — 样本量最大
   (~10000 行)。
3. [`TargetBuiltins/RISCV.cpp`](TargetBuiltins/RISCV.cpp) — RVV 复杂
   VLA 处理样本。

### 阶段 7: Runtime (按需)
- ObjC: [`CGObjC.cpp`](CGObjC.cpp) → [`CGObjCMac.cpp`](CGObjCMac.cpp) →
  [`CGObjCRuntime.h`](CGObjCRuntime.h)。
- OpenMP: [`CGOpenMPRuntime.cpp`](CGOpenMPRuntime.cpp) →
  [`CGStmtOpenMP.cpp`](CGStmtOpenMP.cpp)。
- HLSL: [`CGHLSLRuntime.cpp`](CGHLSLRuntime.cpp) →
  [`HLSLBufferLayoutBuilder.cpp`](HLSLBufferLayoutBuilder.cpp)。
- CUDA: [`CGCUDANV.cpp`](CGCUDANV.cpp)。

### 阶段 8: 跨架构对照 (1 小时)
对每个 ABI/Target, 比对[`Targets/X86.cpp`](Targets/X86.cpp) /
[`Targets/AArch64.cpp`](Targets/AArch64.cpp) /
[`Targets/RISCV.cpp`](Targets/RISCV.cpp) — 看同样的 hook 在不同
架构下的差异。

---

## §6. 常用操作指南

### 6.1 添加新后端 (假设 `MyArch`)

1. **新建 ABI**: 在 [`Targets/MyArch.cpp`](Targets/) 创建
   `MyArchTargetCodeGenInfo` + `MyArchABIInfo` (派生自
   [`ABIInfo`](ABIInfo.h) / [`TargetCodeGenInfo`](TargetInfo.h)),
   重写 `computeInfo` / `EmitVAArg` / `setTargetAttributes`。
2. **加 builtin 文件**: 创建 [`TargetBuiltins/MyArch.cpp`](TargetBuiltins/),
   实现 `CodeGenFunction::EmitMyArchBuiltinExpr`。
3. **注册 builtin id**: 在 `clang/include/clang/Basic/Builtins*.def`
   加新 builtin (一般 `BuiltinsMyArch.def`)。
4. **挂分发**: 在 [`CGBuiltin.cpp`](CGBuiltin.cpp) 的
   `EmitTargetArchBuiltinExpr` switch 加新 case;
   在 [`CGBuiltin.cpp`](CGBuiltin.cpp) 调
   `getTargetBuiltins()` helper 加新 case。
5. **注册 target**: 在 [`TargetInfo.cpp`](TargetInfo.cpp) 的
   `getTargetCodeGenInfo` switch 加新 Triple → 工厂 case。
6. **CMakeLists.txt**: 把 [`Targets/MyArch.cpp`](Targets/) +
   [`TargetBuiltins/MyArch.cpp`](TargetBuiltins/) 加进
   `clangCodeGen` 源列表。
7. **测试**: `clang/test/CodeGen/myarch-builtins.c` +
   `clang/test/CodeGenCXX/myarch-abi.cpp`。

### 6.2 给现有 arch 加新 builtin

1. 在 `clang/include/clang/Basic/Builtins<Arch>.def` 加 builtin
   声明。
2. 在 [`TargetBuiltins/<Arch>.cpp`](TargetBuiltins/) 的
   `Emit<Arch>BuiltinExpr` switch 加新 case。
3. 测试: `clang/test/CodeGen/<arch>-builtins/<builtin>.c`。

### 6.3 添加新 C++ 语言特性 codegen

1. 在 [`CGExprCXX.cpp`](CGExprCXX.cpp) 或新建 `CGCXXNew.cpp` 加
   expression emission。
2. 如果涉及 ABI: 在 [`CGCXXABI.cpp`](CGCXXABI.cpp) 加抽象 hook,
   在 [`ItaniumCXXABI.cpp`](ItaniumCXXABI.cpp) +
   [`MicrosoftCXXABI.cpp`](MicrosoftCXXABI.cpp) 加具体实现。
3. 如果涉及 destructor / vtable: 改 [`CGClass.cpp`](CGClass.cpp) +
   [`CGVTables.cpp`](CGVTables.cpp)。
4. 如果涉及 EH / cleanup: 改 [`CGException.cpp`](CGException.cpp) +
   [`CGCleanup.cpp`](CGCleanup.cpp)。
5. 测试: `clang/test/CodeGenCXX/<feature>.cpp`。

### 6.4 添加新 OpenMP directive

1. 在 `clang/include/clang/AST/StmtOpenMP.h` + `OpenMPClause.h` 加
   AST 节点 (一般由 Sema 处理)。
2. 在 [`CGStmtOpenMP.cpp`](CGStmtOpenMP.cpp) 的 `EmitStmt` switch
   加新 case (经 `EmitOMP*Directive`)。
4. 如果 runtime 调 libomp: 在
   [`CGOpenMPRuntime.cpp`](CGOpenMPRuntime.cpp) 加 emit 函数 +
   必要 enum。
5. 测试: `clang/test/OpenMP/<directive>*.c`。

### 6.5 添加新 HLSL 内置

1. 在 `clang/include/clang/Basic/BuiltinsHLSL.def` 加 builtin。
2. 在 [`CGHLSLBuiltins.cpp`](CGHLSLBuiltins.cpp) 的
   `EmitHLSLBuiltinExpr` switch 加新 case (或加新 helper 函数)。
3. 如果涉及资源 / buffer: 在 [`CGHLSLRuntime.cpp`](CGHLSLRuntime.cpp)
   加资源类型 + bind 处理。
4. 测试: `clang/test/CodeGenHLSL/<feature>.hlsl`。

### 6.6 调试 LLVM IR 输出

1. 加 `-emit-llvm -S` 看 IR (输出 .ll 文件)。
2. 用 `-Xclang -print-stats` 看 codegen 统计。
3. 在 [`CodeGenFunction.cpp`](CodeGenFunction.cpp) 的
   `EmitStmt` 加 `llvm::errs() << ... << "\n"` 临时 trace。
4. 用 `LLDB` 跟 `CodeGenModule::EmitFunctionDefinition` 看 codegen 状态。
5. 对 ABI 怀疑: 加 `-Xclang -triple -Xclang x86_64-linux-gnu` 限定,
   然后单步进 [`Targets/X86.cpp`](Targets/X86.cpp) 的
   `X86_64ABIInfo::computeInfo`。

### 6.7 调试 debug info

1. 加 `-g` 启用 (默认 DWARF, `dwarf-version` 控制版本)。
2. 用 `llvm-dwarfdump --verify <object>` 验证。
3. 看 [`CGDebugInfo.cpp`](CGDebugInfo.cpp) 的 `EmitFunctionDecl` +
   `EmitType` + `EmitDeclare`。
4. 对宏不显示: 加 `-fdebug-macro` (用
   [`MacroPPCallbacks.cpp`](MacroPPCallbacks.cpp))。

### 6.8 在 Clang CodeGen 加新 backend (e.g. MLIR)

1. 不推荐 (Clang CodeGen 强耦合 LLVM IR); 用 MLIR 的话, 写
   [`CodeGenAction`](CodeGenAction.cpp) 的子类, 替换
   [`BackendConsumer`](BackendConsumer.h)。
2. 走 MLIR pipeline 而非 LLVM backend。

---

## §7. NT 注释索引

当前 `clang/lib/CodeGen/` 下尚无 `// <NT>` 注释。已建立目录索引, 姊妹
overview:

- `clang/lib/AST/` — Clang AST 层 (待写)
- `clang/lib/Sema/` — Sema 语义分析 (待写)
- `clang/lib/Frontend/` — Clang 前端桥接 (待写)
- `clang/lib/Lex/` — Clang Lexer (待写)
- `clang/lib/Parse/` — Clang Parser (待写)
- [`llvm/lib/IR/0-overview.md`](../../../llvm/lib/IR/0-overview.md) — LLVM IR 层
- [`llvm/lib/CodeGen/0-overview.md`](../../../llvm/lib/CodeGen/0-overview.md) — LLVM 后端
- [`llvm/lib/Object/0-overview.md`](../../../llvm/lib/Object/0-overview.md) — LLVM 二进制对象解析

按"少而精"原则, 加 NT 注释建议优先级:

1. [`CodeGenFunction.cpp`](CodeGenFunction.cpp) + [`CodeGenModule.cpp`](CodeGenModule.cpp) — 各 8-10 段 (中心 hub, 最大文件)
2. [`CGCall.cpp`](CGCall.cpp) + [`Targets/X86.cpp`](Targets/X86.cpp) — 各 5-8 段 (ABI 中心)
3. [`CGDebugInfo.cpp`](CGDebugInfo.cpp) — 5-8 段 (debug info 全在它)
4. [`CGExpr.cpp`](CGExpr.cpp) + [`CGExprScalar.cpp`](CGExprScalar.cpp) — 各 5-8 段 (表达式发射)
5. [`CGOpenMPRuntime.cpp`](CGOpenMPRuntime.cpp) + [`CGStmtOpenMP.cpp`](CGStmtOpenMP.cpp) — 各 4-6 段 (OpenMP 协作)
6. [`CGObjCMac.cpp`](CGObjCMac.cpp) + [`CGObjCGNU.cpp`](CGObjCGNU.cpp) — 各 3-5 段 (ObjC runtime)
7. 单 arch 文件 ([`Targets/AMDGPU.cpp`](Targets/AMDGPU.cpp) /
   [`Targets/RISCV.cpp`](Targets/RISCV.cpp) /
   [`Targets/AArch64.cpp`](Targets/AArch64.cpp)) — 各 3-5 段 (cross-arch 对照)
8. 单 builtin 文件 ([`TargetBuiltins/X86.cpp`](TargetBuiltins/X86.cpp) /
   [`TargetBuiltins/RISCV.cpp`](TargetBuiltins/RISCV.cpp)) — 各 2-3 段

---

**姊妹文档**: 本目录对应 LLVM 流水线中的 **Clang 前端最后一步** (AST →
LLVM IR); 它与 [`llvm/lib/CodeGen/`](../../../llvm/lib/CodeGen/0-overview.md)
(LVM 后端: IR → MC) 形成完整编译流水线。Clang 用户可见的选项在
[`clang/include/clang/Driver/`](../../include/clang/Driver/) +
[`clang/lib/Driver/`](../Driver/)。