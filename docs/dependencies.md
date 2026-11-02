# 第三方依赖

全部依赖都允许闭源商用。禁止引入 AGPL/GPL 依赖（MuPDF、Ghostscript、Poppler、
pdf2docx、PyMuPDF 等）。新增依赖前先问用户，并在这里登记。

| 依赖 | 版本 | 许可证 | 来源 | 用途 |
|---|---|---|---|---|
| PDFium | chromium/8125 | BSD-3-Clause（部分 Apache-2.0） | bblanchon/pdfium-binaries 预编译包，`scripts/fetch_pdfium.py` | 渲染、文本提取、页面对象编辑、表单、注释 |
| qpdf | 12.4.2 | Apache-2.0 | vcpkg（`vcpkg.json`，builtin-baseline 固定） | 合并、拆分、加密/解密、线性化、修复 |
| zlib | vcpkg | Zlib | qpdf 依赖；PDFCore 也直接使用 | 转 Word 时生成 .docx（zip deflate、CRC-32） |
| libjpeg-turbo | 3.2.0 | IJG / BSD-3-Clause | qpdf 依赖 | |
| FluentUI / FluentSettings | 同级仓库 `../Fluent` | 用户自有（MIT） | ProjectReference | UI 框架、配置持久化 |
| nlohmann/json | Fluent 内置 | MIT | `../Fluent/FluentSettings/third_party` | 配置 JSON |

发布时需要随程序附带各许可证文本（PDFium 的在 `third_party/pdfium/licenses/`）。

PDFium 以 DLL 形式链接（`pdfium.dll`，构建时复制到 exe 旁边）；
qpdf 及其依赖以静态库链接，triplet `x64-windows-static`，与 FluentUI 一样用静态 CRT。

升级：PDFium 改 `scripts/fetch_pdfium.py` 的 `PDFIUM_TAG`；qpdf 改 `vcpkg.json` 的
`builtin-baseline`。之后跑 `python scripts/restore_deps.py`，再完整 `/verify`。
