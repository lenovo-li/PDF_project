# CLAUDE.md

PDFTool：Windows 桌面 PDF 工具（查看、编辑、合并/拆分、加密、压缩、转 Word 等）。
UI 用同级仓库 `../Fluent` 的 FluentUI 框架；PDF 底层用 PDFium + qpdf。
开发由 Claude Code 主会话 + 项目子 Agent 协作完成。产品本身不调用 AI。

## 仓库布局

| 路径 | 内容 |
|---|---|
| `PDFTool.slnx` | 解决方案，引用本仓库工程和 `../Fluent` 的 `FluentUI`、`FluentSettings` |
| `src/PDFCore/` | 静态库：PDF 引擎封装（PDFium/qpdf），不依赖 UI，可无头测试 |
| `src/PDFTool/` | GUI 应用：FluentUI 窗口、页面、命令 |
| `tests/` | 无头单元测试（控制台），只测 `PDFCore` 与纯函数 |
| `third_party/pdfium/` | PDFium 预编译包（bblanchon/pdfium-binaries，不入库，由脚本下载） |
| `build/PDFCore.Consumer.props` | PDFium/qpdf 的头文件、库、DLL 复制；PDFCore 及其使用者导入 |
| `scripts/` | `restore_deps.py`、`check_project.py`、`gen_filters.py`（从 .vcxproj 重新生成 .filters） |
| `vcpkg.json` | qpdf 等依赖（VS 自带 vcpkg，manifest 模式，装到 `vcpkg_installed/`） |
| `docs/` | 永久文档；`docs/design/` 为临时设计文档，任务完成后删除 |

**依赖方向是单向的**：`PDFTool → PDFCore → (PDFium, qpdf)`；`PDFTool → FluentUI`。
`PDFCore` 不得 include 任何 FluentUI 头文件，这样引擎逻辑能在测试里无窗口运行。

## Fluent 库

FluentUI 是用户自己的库，位于 `../Fluent`，有自己的 `CLAUDE.md`、`.claude/rules/`、
子 Agent。写 UI 代码前：

- 需要的控件 API 交给 `pdf-explorer`（会读 `../Fluent` 源码和 `docs/controls/`）查，不要凭记忆写；
  FluentUI 的真实 API 和"看起来惯用"的写法经常不同。
- 需要新控件时，**优先在本仓库 `src/PDFTool/controls/` 里继承 FluentUI 控件**
  （见 `../Fluent/docs/controls/inheritance-guide.md`）。只有通用且属于框架缺陷的问题才改
  `../Fluent`，改之前先问用户，因为那是另一个仓库，有它自己的验证流程（其 `/verify`）。
- 遵守 FluentUI 的约定：DIP 坐标、`SetProperty`、`Subscription` 存成员、主题 token 不硬编码颜色、
  idle 零 CPU、长列表 O(visible)。规则全文见 `../Fluent/.claude/rules/`，按需直接读。

## 构建与测试

工具集 v145（VS 2026），C++20，x64，静态 CRT（`/MT`、`/MTd`，与 FluentUI 一致）。
从仓库根目录：

```bash
python scripts/restore_deps.py      # 首次：PDFium → third_party/pdfium/，qpdf → vcpkg_installed/
python scripts/check_project.py     # 磁盘源文件与 .vcxproj/.filters 是否一致，只读、秒级
"C:/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe" PDFTool.slnx -p:Configuration=Debug -p:Platform=x64 -verbosity:minimal
"C:/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe" PDFTool.slnx -p:Configuration=Release -p:Platform=x64 -verbosity:minimal
./x64/Debug/PDFCoreTests.exe        # 全部测试；可带子串过滤
```

报告完成前 Debug + Release 都必须干净编译，测试全绿。

注意：

- `FluentUI`/`FluentSettings` 的中间目录（`IntDir`）在 `../Fluent` 工程目录下，
  本解决方案和 Fluent 自己的解决方案共用它们。不要同时从两边跑 MSBuild。
- 每个源文件都必须注册在所属工程的 `.vcxproj` 和 `.filters`（没有通配符）。
  加文件后改 `.vcxproj`，再跑 `python scripts/gen_filters.py <工程>` 生成 `.filters`。
  编辑工程文件或源文件后 PostToolUse hook 会自动跑 `check_project.py`。
- 依赖通过 vcpkg manifest（`vcpkg.json`，triplet `x64-windows-static`）或 `third_party/` 引入。
  新增依赖前先问用户，并在 `docs/dependencies.md` 记录许可证。
  **禁止引入 AGPL/GPL 依赖**（MuPDF、Ghostscript、PyMuPDF、pdf2docx 等），保持可闭源发布。

测试是无头的：不开窗口、不用 GPU。渲染像素、滚动平滑度、DPI 切换等要用户在真机上检查，
要说清楚去哪里看。

## 多 Agent 协作

主会话负责规划、写代码、做决定；子 Agent 负责读、编译、审查，让大段输出不进主上下文。
简单修改直接在主会话做。

| Agent | 工具 | 用途 |
|---|---|---|
| `pdf-explorer` | 只读 | 写代码前查真实 API：FluentUI（`../Fluent`）、PDFium（`third_party/pdfium/include`）、qpdf 头文件、本仓库现有代码 |
| `pdf-spec-expert` | 只读 + 网页 | PDF 规范（ISO 32000）、OOXML/DOCX 格式、字体/编码问题；回答"这个结构是什么意思、该怎么写" |
| `pdf-builder` | Bash | `check_project.py`、Debug + Release 编译、测试；只返回错误、警告、失败用例 |
| `pdf-reviewer` | 只读 + git | 审查不是自己写的 diff：线程、生命周期、资源释放、恶意 PDF 输入、FluentUI 约定 |

规则：

- **写操作串行**：同一时刻只有一个 Agent 编辑文件或跑 MSBuild（`.vcxproj` 和 `x64/` 共用）。
- **只读工作可以并行**：例如 `pdf-explorer` 查 UI API 的同时 `pdf-spec-expert` 查格式。
- 独立的大功能可以用 `isolation: worktree` 的 Agent 并行开发，合并前必须各自 `/verify`。
- 审查交给没写代码的 Agent，给它代码范围，不要给它你的结论。

技能（`.claude/skills/`）：`/verify`（交给 `pdf-builder` 完整验证）、
`/new-feature <功能>`（调研 → 计划 → 实现 → 验证 → 审查 → 文档）、`task-state`（断点规程）。

## 架构规则

规则文件在 `.claude/rules/`，按 `paths:` 在读写匹配文件时自动加载：

| 规则文件 | 内容 | 加载于 |
|---|---|---|
| `pdf-engine.md` | PDFium/qpdf 封装、线程模型、句柄生命周期、不可信输入 | `src/PDFCore/**`、`tests/**` |
| `ui.md` | FluentUI 使用约定、后台任务回 UI 线程、DPI、配置持久化、trace | `src/PDFTool/**` |
| `conversion.md` | PDF → Word 等转换的分层设计与保真度验证 | `src/PDFCore/convert/**`、`tests/**` |

修改规则文件时，保持它的 `paths:` 与上表一致。

## 约定

- **PDFium 不是线程安全的**：所有 `FPDF_*` 调用经由 `PDFCore` 的单一引擎线程串行执行；
  UI 线程永远不直接调 PDFium，也不在输入路径上做文件 I/O 或渲染。
- **输入不可信**：任何 PDF 都可能是畸形或恶意的。解析结果做范围检查，页码/尺寸/数量有上限，
  失败返回错误而不是崩溃。
- **错误处理**：`PDFCore` 对外返回 `Result<T>`（值或带消息的错误），不向 UI 抛异常。
- **trace**：关键路径用 `OutputDebugStringA`，前缀 `[PDFTool.Module]`（如 `[PDFTool.Render]`）；
  PDFium/qpdf 失败打印错误码和页号。FluentUI 内部已经打 `[FluentUI.*]`。
- **配置**：用 `FluentSettings` 的 `ConfigStore`（`%APPDATA%\PDFTool\config.json`，
  `portable.txt` 切换便携），保存窗口位置、最近文件、主题等。不写注册表。
- **DPI**：Per-Monitor V2（manifest 已声明），几何一律 DIP；页面渲染位图按窗口当前 DPI 生成。
- **纯函数抽出来测**：坐标换算、版面分析、分页计算等写成无窗口的自由函数，放进 `PDFCore`，并写测试。
- **注释解释为什么**，与 FluentUI 风格一致。注释和文档不引用计划编号、不引用 git 历史。

## 断点与上下文压缩

多步任务维护 `.claude/TASK_STATE.md`（gitignore，本地草稿），规程见 `task-state` 技能。
恢复时：读它 → `git status --short` → 看 diff → 从 `Exact Next Actions` 第一条继续。
SessionStart hook 会在压缩/恢复后提醒。

工作区和真实命令输出是权威。没实际跑成功，不得说编译、测试或真机检查通过。

只在用户明确要求时 commit；push 前单独询问。
