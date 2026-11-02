# 已知问题

功能限制（某功能故意不做的部分）写在 `docs/features.md` 的"限制"列。
这里记录的是缺陷和跨功能的风险：知道它存在、暂时没修，以及修它需要什么。

## 引擎线程没有异常保护

`src/PDFCore/Engine.cpp` 主循环直接调用 `job()`，外面没有 `try/catch`。任何任务抛出
异常（例如恶意 PDF 引起的 `std::bad_alloc`）都会 `std::terminate`，整个程序退出。

已在局部兜住的：`SavePng`（导出 PNG）、`ListAnnotations`（读注释列表）内部捕获
`bad_alloc` 并返回错误。

不能简单地在主循环里吞掉异常：抛异常的任务不会把结果 `Post` 回界面，于是
- `MainWindow::pendingEdits_` 永远不减，之后的注释操作一直被拒绝，工具一直要求"先保存"；
- `PageBitmapSource::pending_` 留着该页，这一页不再重新请求渲染；
- `Engine::RunSync`（关窗前保存）等不到完成信号会卡住。

修法：每个 `Post` 出去的任务都要保证"无论成功、失败还是异常，都回报一次"。
可以在 `Engine` 提供 `PostWithResult(job, onDone)`，由引擎负责 catch 并把错误交给
`onDone`；再把所有调用点迁过去。改动跨引擎和全部界面代码，需要单独做一次并完整验证。

## qpdf 长任务不可取消

合并、拆分、加密、压缩在引擎线程上一次跑完，期间不能取消，也会挡住排在后面的渲染。
大文件上会感觉界面里的页面"卡住不刷新"（窗口本身仍然响应）。

修法：把 qpdf 工作放到单独的工作线程（qpdf 对象不跨线程共享即可，不碰 PDFium），
或者在 qpdf 的进度回调里检查取消标志。

## 非 ASCII 密码

加密方式 R2–R4（旧的 RC4/AES-128）的 PDF，如果密码含非 ASCII 字符，qpdf 可能打不开
（PDFium 能打开）。R6（AES-256，本程序加密时使用的方式）不受影响。

## 编辑不检查文档权限

PDF 的权限位（禁止修改、禁止注释等）目前不检查：用所有者密码或无密码打开的文件都能编辑。
这是产品决定，不是遗漏；如需遵守，在 `EditDocument` 和注释手势入口查 `FPDF_GetDocPermissions`。

## 注释

- 便签的 `/Popup` 删除路径没有测试：本程序的 `AddNote` 不生成 Popup，需要手工构造带 Popup 的 PDF。
- 文字框 `/DA` 里的颜色用 `std::to_wstring` 格式化，如果进程的 C locale 被改成用逗号作小数点，
  会写出无效的 `/DA`。目前项目和依赖都不调用 `setlocale`。
- `FPDFAnnot_GetRect` 失败的注释，矩形记为 0，点页面左上角附近可能选中它（只发生在畸形文件上）。
- 注释菜单不显示当前选用的工具（`MenuItem::checked` 需要在打开菜单前同步）。
