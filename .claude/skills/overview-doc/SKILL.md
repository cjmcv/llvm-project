---
name: overview-doc
description: 给大型源码目录（如 LLVM 后端的 lib/Target/RISCV/）生成"文件目录 + 导读"文档。一份文档同时承担两个角色：(a) 文件目录索引（每个文件的职责 / 上下游 / 关键类）；(b) 阅读路径指南（推荐按什么顺序读懂整个模块）。当用户说"梳理 X 目录"、"写 overview"、"生成导读"、"分析 Y 的所有文件"时触发。
---

# Overview 文档 Skill

## 何时使用

- 用户说: "梳理 X 目录"、"写 overview"、"生成导读"、"分析 Y 下的所有文件"
- 用户要求: 给一个源码目录（含 50+ 文件 / 多个子目录）生成单文档索引 + 入门指南
- 用户强调: 中文、技术详实、能像 "导游手册" 一样让新人快速摸清模块

## 何时不要使用

- 用户只想要单文件级别注释 → 用 `nt-comments` skill
- 用户想要 API 参考 / class reference → 用 Doxygen / 通用文档生成
- 目录很小（< 20 个文件） → 直接回答即可，不必写文档
- 用户要英文文档 → 不触发（本 skill 默认中文）

## 目标与权衡

**目标**: 让一个新人读完文档后，能：
1. 知道这个目录在 LLVM（或目标系统）哪一层、上下游是什么
2. 知道每个文件做什么、上游谁调它、下游它调谁、关键类/函数名是什么
3. 知道按什么顺序读能最快理解整个模块
4. 知道添加新功能（特别是新指令 / 新 Pass）需要碰哪些文件

**权衡**:
- **目录**性质（查得到所有文件） vs **导读**性质（读得懂脉络）
- 一个文档承担两个角色, 都要兼顾, 但避免重复

## 文档结构（硬性七节）

文档必须按以下顺序组织, 每节单独一个 `##` 标题, 缺节即视为不完整:

| 节号 | 标题 | 作用 | 必须内容 |
|------|------|------|----------|
| §0 | 后端在编译流水线中的位置 (或同等定位段) | 回答"我在哪" | 上游层、下游层、入口是什么 |
| §1 | 编译流水线概览 | 回答"东西怎么流" | ASCII 流程图（主路径 + 反汇编 / 汇编 / 其它旁路） |
| §2 | 文件目录结构 | 回答"目录长啥样" | 子目录树状图 + TableGen 生成器清单表 (如适用) |
| §3 | 文件详解 | 回答"每个文件做什么" | 按子目录分组, 每个文件 4-6 行 |
| §4 | 关键调用链 | 回答"几条主线怎么走" | 主流程的 ASCII 或缩进调用链 |
| §5 | 推荐阅读顺序 | 回答"我该从哪读起" | 6-8 个阶段, 每阶段 3-6 个文件 |
| §6 | 自定义扩展指南 (或同等扩展节) | 回答"我要加东西该改哪" | 7 步左右, 链接到完整教程文档 |
| §7 (可选) | 注释 / NT 索引 | 列出已加注释的位置 | 用于回顾 |

**不能整节删除**。若某个模块没有"自定义扩展"流程, §6 改为"常用操作指南"
或"调试建议"等; 但 §0~§5 必须存在。

## 文件详解（§3）的密度与写法

每条文件说明 **4-6 行**, 严格按下表组织:

```
#### 路径（相对文档所在目录）
- 作用: 一句话讲清做什么（不超过 80 字）
- 上游: 谁会调用（可能多源, 列 1-3 个）
- 下游: 它会调用什么（可能多个, 列 1-3 个）
- 关键类/函数: 列出 3-8 个关键标识符（让读者有锚点）
```

**.td 文件特殊处理**: 只写一行"作用" + "被谁 include", 不展开列指令.

**.cpp / .h 文件**: 必须给出关键类名 / 函数名, 不只描述抽象职责. 例如
不要写"实现指令选择", 要写"`RISCVTargetLowering`、`RISCVISD::*`、
`getRegisterType`".

**Pass 文件**: 单独标注"插入阶段"（Pre-RA / Post-RA / PreEmit / PostEmit /
IR 层 / SelectionDAG 等）, 让读者能立刻判断它在流水线中的位置.

## 流水线图（§1）规范

主流程用以下 ASCII 框架:

```
┌──────────────────────────┐     ┌──────────────────────────────┐
│ 输入名                    │     │ 输出名                        │
└──────────────────────────┘     └──────────────────────────────┘
              │
              ▼
┌────────────────────────────────────────────────────────┐
│ 模块名  (简短说明)                                      │
│   · 子项 1                                              │
│   · 子项 2                                              │
└────────────────────────────────────────────────────────┘
              │
              ▼
...
```

每层一行说明, 用箭头连接, 不要堆叠. 字符画对齐要规整.

**路径图（§4）用缩进式调用链**, 不用 ASCII 图. 格式:

```
入口函数 (一行说明)
  ├─ 步骤 1
  │   └─ 子步骤
  └─ 步骤 2
```

## 工作流程（硬性步骤）

1. **摸清目录**: `find . -maxdepth N -type f | sort` 列出所有文件, 估文件数.
2. **读顶层入口**: CMakeLists.txt / README / 顶层 .h, 确认编译组织、组件名.
3. **委托 Explorer 子代理**: 把目录交给 `Explore` 子代理, 让它按"组 → 文件"
   给出紧凑目录. 子代理的提示词要明确:
   - 按子目录分组
   - 每个 .cpp 文件: 作用 + 上游 + 下游 + 关键类/函数
   - 每个 .td 文件: 一行
   - 不要展开每条指令
4. **本agent 补充**: 读几个最关键的入口 .cpp 验证子代理输出.
5. **按七节起草**: 不按子代理输出顺序照抄, 按 §0~§7 重排.
6. **加交叉链接**: 文档中提到的关键 .cpp / .md 用相对链接（[name](path)）,
   方便跳转.
7. **加文件跳转链接 (硬性要求)**: 详见上一节「文件跳转链接规范」. 写完草稿后
   跑一遍: 每个被 mention 的 `.cpp` / `.h` / `.td` 文件名第一次出现时,
   包成 `` [`文件名`](相对路径) ``. 验证 0 missing file link.
8. **§7 NT 注释索引**: 如果目录里有带 `// <NT>` 注释的文件, 列出它们.

## 编辑策略（硬性规则）

```
1. 路径一律相对文档所在目录, 不要用绝对路径.
2. 所有 NT 注释 / 其他文档的引用用相对 markdown 链接, 不要只写文件名.
3. 表格用 GFM 表格语法, 不要 ASCII art.
4. ASCII 流程图用 monospace box (┌─┐│└─┘), 不要用 HTML 表格.
6. 不删除原内容: overview 是新文件, 不修改现有源码.
7. 一次创建完整文档, 不要分多次 Edit 拼凑 (Write 工具).
8. 文档大小控制在 500-800 行 (200 个文件级别). 超过 800 行说明
   信息密度太低, 应删除冗余; 少于 500 行说明过于简略.
9. **文档首行加 `<NT>` 标记**, 方便在 IDE / 文件管理器搜索栏搜出来.
   写法: 用 HTML 注释包裹, 放在 `# 标题` 之前. 标记**结构化、简短**,
   不要把完整标题再写一遍 (会与下方 `#` 标题重复):
   ```
   <!-- <NT>overview:相对目录路径/ -->

   # 模块导读 - 简短标题
   ```
   `overview:相对目录路径/` 是搜索关键字: 全局搜 `<NT>` 可列出所有 overview,
   搜 `<NT>overview:lib/Target/RISCV/` 可定位到具体某个 overview. 路径
   一律相对仓库根目录.
10. **每个被 mention 的源码文件名都必须带文件跳转链接** (硬性要求). 详见
    下一节「文件跳转链接规范」.
```

## 文件跳转链接规范 (硬性要求)

overview 是"文件目录索引", 必须让读者**点击文件名就跳到对应源文件**.
任何裸文件名 (`SelectionDAGISel.cpp`、`Verifier.cpp`、`AtomicExpandPass.cpp`)
都必须包成 markdown 链接.

### 标准格式

```
✅ [`SelectionDAGISel.cpp`](SelectionDAGISel.cpp) — 顶层文件
✅ [`SelectionDAG/SelectionDAG.cpp`](SelectionDAG/SelectionDAG.cpp) — 子目录文件
✅ [`SelectionDAGISel.cpp`](SelectionDAGISel.cpp) — 显示文本和 target 一致即可

❌ `SelectionDAGISel.cpp` — 无跳转
❌ SelectionDAGISel.cpp — 无跳转
❌ `[SelectionDAGISel.cpp]` (无 target) — 浏览器不知道往哪跳
❌ `[SelectionDAGISel.cpp](SelectionDAGISel.cpp)` 包在反引号里 — 反引号把整个
   链接语法当成 inline code, 不会渲染为可点击链接.
```

**backtick 与链接的搭配**: backtick 用于代码样式 (`.` 在某些字体里不点明),
markdown 链接用于跳转. 两者**不可相互覆盖**:

```
✅ [`文件名.cpp`](文件名.cpp)   ← backtick 只包显示文本
❌ `[`文件名.cpp`](文件名.cpp)` ← backtick 把链接语法也吞了
❌ `[文件名.cpp](文件名.cpp)` 包在反引号里 (即整个 `[..](..)` 在 `..` 内)
```

### 子目录文件

如果文件在子目录 (例如 `llvm/lib/CodeGen/SelectionDAG/SelectionDAGISel.cpp`),
overview 文档位于 `llvm/lib/CodeGen/0-overview.md`, 链接 target 必须是
**相对文档位置的完整路径**:

```
[`SelectionDAG/SelectionDAGISel.cpp`](SelectionDAG/SelectionDAGISel.cpp)
```

不能用裸文件名 `SelectionDAGISel.cpp` —— 从 `lib/CodeGen/` 起跳找不到这个文件.

### 实施步骤 (工作流)

1. **生成 overview 后**: 列出文档中所有裸文件名 (`.cpp` / `.h` / `.td`),
   对比文档所在目录的真实文件清单.
2. **每个文件**在文档中**第一次**出现的位置加 `` [`文件名`](文件名) ``.
3. **跳过**已在 markdown 链接里的 (`[..](..)` 已是链接).
4. **保留 backtick 风格**: 如果原文档是 `` `SelectionDAGISel.cpp` ``,
   改成 `` [`SelectionDAGISel.cpp`](SelectionDAGISel.cpp) `` — 视觉上仍是
   代码字体, 同时可点击.
5. **子目录文件**: target 包含子目录前缀, 如
   `` [`SelectionDAGISel.cpp`](SelectionDAG/SelectionDAGISel.cpp) ``.
6. **重复 mention 不重复加链接**: 一个文件名只在第一次出现处加链接,
   后续提及保留原样 (避免文档被一堆重复链接淹没). 例外: 跨章节的
   重要 mention (如 `Verifier.cpp` 在 §3 / §4 / §6 都出现) 可考虑
   在每章第一次出现时都加, 但不要同一章内重复.
7. **验证**: 最后跑一次可达性检查, 确保 0 个 missing file link.
   ```bash
   python3 -c "
   import re, os
   with open('0-overview.md') as f: c = f.read()
   for l in re.findall(r'\]\(([^)]+)\)', c):
       if l.startswith(('http', '#', '..')) or '\`' in l: continue
       if not os.path.exists(l): print(f'MISSING: {l}')
   "
   ```

### 反例 (会让链接失效)

```
❌ `[SelectionDAGISel.cpp](SelectionDAGISel.cpp)` 包在反引号内:
   反引号让 markdown 把整段识别为 inline code, 不会解析为链接.
   修正: 改成 `` [`SelectionDAGISel.cpp`](SelectionDAGISel.cpp) ``.

❌ 子目录文件写成裸名:
   `[SelectionDAGISel.cpp](SelectionDAGISel.cpp)` — 但文件实际在
   `SelectionDAG/` 子目录, 链接会 404.
   修正: target 写成 `SelectionDAG/SelectionDAGISel.cpp`.

❌ 用 `#anchor` 替代文件路径:
   `[Module.cpp](#modulecpp)` — `#anchor` 用于跨章节跳转, 不能跳到文件.
   修正: target 写成相对文件路径 `Module.cpp`.
```

## 反例（不要这样写）

```
❌ 只列文件名: "RISCVAsmPrinter.cpp 实现 AsmPrinter 子类。"
   (没说上下游, 没说关键类, 新人读完后还是不知道它在哪条线上)

❌ 把每个指令 / 每个 Pass 都展开: 写出 100+ 个函数签名.
   (文档不是 API 手册, 读者会迷失)

❌ 没有流水线图, 全是分类目录.
   (失去"导读"价值, 只是目录索引)

❌ 没有 §5 推荐阅读顺序.
   (读者不知道从哪读起)

❌ 路径用绝对路径或写两次完整目录.
   (破坏可移植性, 文档难维护)
```

## 正例（结构示例）

```
# XYZ 后端导读 — `path/to/dir/`

> 一句话描述本文档目标读者与覆盖范围.

## §0. 后端在系统中的位置
[回答"我在哪一层"]

## §1. 编译流水线概览
[ASCII 流程图]

## §2. 文件目录结构
[树状图 + 关键表格]

## §3. 文件详解
### 3.1 子目录 A/
#### path/to/file.cpp
- 作用: ...
- 上游: ...
- 下游: ...
- 关键类/函数: ...
### 3.2 子目录 B/
...

## §4. 关键调用链
[3-5 条主路径的缩进调用链]

## §5. 推荐阅读顺序
### 阶段 1: 全景 (1-2 小时)
1. file_a.cpp - ...
2. file_b.cpp - ...
...

## §6. 自定义扩展指南
1. 改 file_c.td
2. 改 file_d.cpp
...

## §7. NT 注释索引
- file_a.cpp: 12 段 (3 文件级 + 9 函数级)
- file_b.cpp: 8 段
...
```

## 触发参数

| 用户输入 | 动作 |
|----------|------|
| "梳理 X 目录"、"写 overview" | 启动 skill, 先确认输出路径 |
| "生成导读" | 同上 |
| "分析 Y 的所有文件" | 同上, 默认输出到 `Y/0-overview.md` |
| "给 X 写一份手册" | 同上 |
| "overview.md" (单独说) | 默认在当前目录下生成 `0-overview.md` |

## 文件命名约定

**所有 overview 文件必须以 `0-` 前缀开头**, 完整文件名固定为 `0-overview.md`,
不带任何其它修饰. 例如:
  - `llvm/lib/Target/RISCV/0-overview.md`
  - `llvm/lib/CodeGen/0-overview.md`
  - `llvm/lib/Target/X86/0-overview.md`

`0-` 前缀的作用:
  - 在文件管理器 / IDE 中按字母序排序时, overview 永远排第一, 醒目易找.
  - 与 README.md / LICENSE 等根目录文件保持"前置索引"风格一致.
  - 多个 overview 共存时, 全局搜 `0-overview.md` 一次列出全部.

若仓库已经存在不带 `0-` 前缀的旧 overview, 应一并重命名 (`mv`), 而不是
新建并存. 旧文件删除或保留均不推荐.

## 与其他 skill 的关系

- **nt-comments**: overview 文档 §7 索引 NT 注释位置, 但不重复加 NT 注释.
  两者互补: NT 注释给单文件看, overview 给整个模块看.
- 若用户先说"加 NT 注释", 再说"写 overview", 在 §7 中索引 NT 注释即可.