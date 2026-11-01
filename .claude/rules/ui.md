---
paths:
  - "src/PDFTool/**"
---

## UI rules (FluentUI)

The framework's own rules are in `../Fluent/.claude/rules/`. For UI work read
at least `element-model.md`, `theming.md` and `control-non-negotiables.md`
there; read `rendering.md` before drawing page bitmaps yourself. Get exact
signatures through `pdf-explorer`, not from memory.

**Structure.** The control tree is built in a `Window` subclass's
`OnInitialize()` (`SetRoot` is protected). Event handlers are member
functions subscribed with `Subscribe<&Class::Method>(this)`; the returned
`Subscription` is stored as a member or it unsubscribes immediately.

**Custom controls** (page view, thumbnail strip) live in
`src/PDFTool/controls/` and inherit FluentUI controls through the
`protected` API in `../Fluent/docs/controls/inheritance-guide.md`. They obey
the FluentUI non-negotiables: O(visible) draw and hit-test, zero idle cost,
smallest correct invalidation, theme tokens instead of literal colors.

**Never block the UI thread.** Opening, rendering, saving, converting all go
to the `PDFCore` engine thread. Show progress, keep the window responsive,
allow cancel for long jobs. Results come back to the UI thread before they
touch any element.

**Page bitmaps.** Render at `pageSizeDip × GetDpiForWindow(hwnd) / 96`, cache
a bounded number of pages, and re-render visible pages on `WM_DPICHANGED`
and zoom changes. Never cache a global DPI.

**Configuration.** `fluent::ConfigStore` (`../Fluent/FluentSettings/ConfigStore.h`)
initialized with `L"PDFTool"`. Store window placement (`GetWindowPlacement`
before destroy, validated with `MonitorFromRect(MONITOR_DEFAULTTONULL)` on
restore, primary-monitor center as fallback), recent files, last folder,
theme, zoom. Save immediately on settings changes, debounced for anything
that changes continuously.

**Trace.** `OutputDebugStringA` with `[PDFTool.<Module>]` on window/resource
creation, sizes, DPI, document open/close and every engine error.

Visual results (rendering sharpness, scrolling smoothness, DPI switching
between monitors) can only be checked by the user on real hardware. Say
exactly where to look.
