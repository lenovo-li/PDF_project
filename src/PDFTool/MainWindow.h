// MainWindow.h — top-level PDFTool window.
//
// Starting shell: an "Open" button and a status line. Opening runs on the
// engine thread; the result is marshalled back with Application::Post, so the
// UI thread never touches PDFium or does file I/O. Page view, thumbnails and
// tool pages are added by later features through /new-feature.
#pragma once

#include <FluentUI/FluentUI.h>

#include <memory>
#include <string>

namespace fluent { class ConfigStore; }
namespace pdf { class Engine; }

class MainWindow final : public fluent::Window {
public:
    MainWindow(fluent::ConfigStore* cfg, pdf::Engine* engine);
    ~MainWindow() override;

protected:
    void OnInitialize() override;
    void OnUnloaded(fluent::WindowEventArgs&) override;

private:
    void OnOpenClicked(fluent::Button&, fluent::RoutedEventArgs&);
    void OpenFile(const std::wstring& path);
    void ShowStatus(const std::wstring& text);

    fluent::ConfigStore* cfg_ = nullptr;
    pdf::Engine* engine_ = nullptr;
    fluent::TextBlock* status_ = nullptr;  // owned by the element tree
    fluent::Subscription openSub_;
    // Bumped on every open and on destruction; a stale engine callback that
    // arrives after a newer open (or after the window is gone) is dropped.
    std::shared_ptr<int> generation_ = std::make_shared<int>(0);
};
