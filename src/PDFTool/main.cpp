// main.cpp — PDFTool entry point.
//
// Declaration order is a lifetime contract (see ../Fluent/.claude/rules/
// com-lifetime.md): Application owns the COM apartment and must be destroyed
// last; the Engine owns PDFium and is destroyed before the window/app so no
// engine job can Post to a dead Application. ConfigStore is loaded before the
// window so the saved placement is passed to Show() as plain data.

#include "MainWindow.h"

#include <FluentUI/Application.h>
#include <ConfigStore.h>
#include <PDFCore/Engine.h>

using namespace fluent;

int APIENTRY wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int) {
    Application app(hInst);
    if (!app.ComReady()) return 1;

    ConfigStore cfg;
    cfg.Initialize(L"PDFTool");

    WindowState restore;
    if (cfg.Has("window.width") && cfg.Has("window.height")) {
        restore.x = static_cast<int>(cfg.GetInt("window.x", 0));
        restore.y = static_cast<int>(cfg.GetInt("window.y", 0));
        restore.width = static_cast<int>(cfg.GetInt("window.width", 0));
        restore.height = static_cast<int>(cfg.GetInt("window.height", 0));
        restore.maximized = cfg.GetBool("window.maximized", false);
        restore.dpi = static_cast<UINT>(cfg.GetInt("window.dpi", 96));
        // Off-screen (monitor unplugged) falls back to the default placement.
        restore.valid = restore.width > 0 && restore.height > 0 && restore.OnMonitor();
    }
    char buf[160];
    sprintf_s(buf, "[PDFTool.App] config=%ls restore=%d (%dx%d @%u dpi)\n",
              cfg.Path().c_str(), restore.valid ? 1 : 0, restore.width, restore.height,
              restore.dpi);
    OutputDebugStringA(buf);

    pdf::Engine engine;
    MainWindow win(&cfg, &engine);
    if (FAILED(win.Show(app, restore.valid ? &restore : nullptr))) return 1;
    win.SetMinClientSizeDip(480.0f, 320.0f);
    win.SetDarkMode(cfg.GetBool("theme.dark", SystemUsesDarkMode()));
    return app.Run();
}
