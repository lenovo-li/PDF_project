// main.cpp — PDFTool entry point.
//
// Declaration order is a lifetime contract (see ../Fluent/.claude/rules/
// com-lifetime.md); locals are destroyed in reverse: window, engine, config,
// application.
// - Application owns the COM apartment and the UI work queue, so it goes last:
//   engine jobs still running while the engine drains can Post to it safely
//   (nothing pumps after Run returns, so those closures never run).
// - The window goes first. Its posted callbacks hold weak generation tokens
//   that it invalidates on destruction, so none touches it afterwards.
// - The Engine drains its queue and closes every open document on its own
//   thread before destroying PDFium. Closing right after a heavy zoom can
//   therefore keep the process alive briefly after the window disappears.
// ConfigStore is loaded before the window so the saved placement is passed to
// Show() as plain data.

#include "MainWindow.h"

#include <FluentUI/Application.h>
#include <ConfigStore.h>
#include <PDFCore/CommandLine.h>
#include <PDFCore/Engine.h>

#include <shellapi.h>

#include <string>
#include <vector>

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
    win.ApplyThemeSetting();
    // "PDFTool.exe file.pdf" (Explorer "Open with", file association).
    int argc = 0;
    if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        const std::wstring file = pdf::FileFromArgs(std::vector<std::wstring>(argv, argv + argc));
        LocalFree(argv);
        if (!file.empty()) win.OpenOnStart(file);
    }
    return app.Run();
}
