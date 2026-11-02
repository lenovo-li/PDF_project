// MainWindow.h — top-level PDFTool window.
//
// Layout: a toolbar (open, zoom, status) docked at the top and the page view
// filling the rest. Opening runs on the engine thread: the document goes into
// the engine's DocumentStore and only its id and page sizes come back, through
// Application::Post, so the UI thread never touches PDFium or does file I/O.
#pragma once

#include <FluentUI/FluentUI.h>
#include <FluentUI/controls/MenuFlyout.h>
#include <PDFCore/DocumentStore.h>
#include <PDFCore/QpdfOps.h>
#include <PDFCore/Result.h>

#include "ConvertRunner.h"
#include "SearchRunner.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace fluent { class ConfigStore; class StatusBar; }
namespace pdf { class Engine; class Document; }
class PdfPageView;
class ThumbnailStrip;
struct PageViewChangedArgs;
struct PageClickedArgs;
struct AnnotationGestureArgs;

class MainWindow final : public fluent::Window {
public:
    MainWindow(fluent::ConfigStore* cfg, pdf::Engine* engine);
    ~MainWindow() override;

    // Opens `path` given on the command line; call after Show.
    void OpenOnStart(const std::wstring& path);
    // Applies the saved theme setting ("system", "light", "dark").
    void ApplyThemeSetting();

protected:
    void OnInitialize() override;
    void OnUnloaded(fluent::WindowEventArgs&) override;
    // Asks to save unsaved edits; Cancel keeps the window open.
    void OnClosing(fluent::WindowClosingArgs& e) override;

private:
    void OnOpenClicked(fluent::Button&, fluent::RoutedEventArgs&);
    void OnZoomInClicked(fluent::Button&, fluent::RoutedEventArgs&);
    void OnZoomOutClicked(fluent::Button&, fluent::RoutedEventArgs&);
    void OnFitWidthClicked(fluent::Button&, fluent::RoutedEventArgs&);
    void OnViewChanged(PdfPageView&, PageViewChangedArgs& e);
    void OnThumbnailClicked(ThumbnailStrip&, PageClickedArgs& e);
    void OnPageBoxKeyDown(fluent::UIElement&, fluent::KeyEventArgs& e);
    void OnPageBoxTextChanged(fluent::TextEditBase&, std::wstring&);
    void ShowPageNumber(int currentPage, int pageCount);

    // Search.
    void OnSearchBoxKeyDown(fluent::UIElement&, fluent::KeyEventArgs& e);
    void OnSearchPrevClicked(fluent::Button&, fluent::RoutedEventArgs&);
    void OnSearchNextClicked(fluent::Button&, fluent::RoutedEventArgs&);
    // Window-wide shortcuts (Ctrl+F, F3, Shift+F3) in the tunnel phase, so they
    // work whichever control has focus.
    void OnRootPreviewKeyDown(fluent::UIElement&, fluent::KeyEventArgs& e);
    void StartSearch();
    void StepMatch(int delta);  // +1 next, -1 previous; starts a search if needed
    void ShowSearchCount();
    // `password` is retried after a password prompt; empty on first attempt.
    void OpenFile(const std::wstring& path, const std::string& password = {});
    void CloseCurrentDocument();
    void ShowStatus(const std::wstring& text);

    // Menus (MainWindowDocument.cpp / MainWindowTools.cpp / MainWindowAnnot.cpp).
    void BuildMenus(fluent::StackPanel* bar);
    void BuildAnnotMenu();
    // Annotation gestures from the view; text prompts and document edits here.
    void OnAnnotationGesture(PdfPageView&, AnnotationGestureArgs& e);
    // Document edits on the engine thread, then the views are reset to the new
    // page list. `edit` runs on the engine thread against the open Document.
    void EditDocument(const wchar_t* what,
                      std::function<pdf::Status(pdf::Document&, int currentPage)> edit,
                      int pageAfter);
    void OnEditDone(pdf::DocumentId id, const std::wstring& label, const std::wstring& error,
                    const std::vector<std::pair<double, double>>& sizes, int current,
                    int pageAfter);
    void RotateCurrent(int quarterTurns);
    void DeleteCurrent();
    void InsertBlankAfterCurrent();
    void MoveCurrent(int delta);
    void InsertFromFile();
    void Save(bool saveAs);
    void SetDirty(bool dirty);
    // Page content changed (annotation added/edited/deleted): invalidate its
    // bitmaps, reload its annotation list, mark dirty. Page geometry unchanged.
    void PageContentChanged(int page);
    // Unsaved edits: ask Save / Don't save / Cancel before `action` (closing,
    // opening another file). Returns false to abort the action.
    bool ConfirmDiscard(const std::wstring& action);
    // Blocking save to `path` for the confirm-on-close path (the window is
    // about to go away, so an async save could not report back).
    bool SaveNow(const std::wstring& path);
    // True if `out` is the open file or one of `inputs`: a tool must never
    // overwrite its own input, nor the file the view and password describe.
    bool OutputClashes(const std::wstring& out, const std::vector<std::wstring>& inputs);
    // Tools on the saved file (qpdf). The target is captured once, before any
    // modal prompt: a prompt pumps messages, so an open or edit can land while
    // it is up. StillCurrent() then refuses to run on a different document.
    struct ToolTarget {
        pdf::DocumentId id = pdf::kNoDocument;
        std::wstring path, name;
        std::string password;
    };
    std::optional<ToolTarget> RequireSavedFile();
    bool StillCurrent(const ToolTarget& t);
    void RunFileTool(const wchar_t* what, std::function<pdf::Status()> job);
    void MergeWithOthers();
    void ExtractCurrent();
    void SplitCurrent();
    void EncryptCurrent();
    void DecryptCurrent();
    void CompressCurrent();
    void ExportPagePng();
    void FillForm();
    void ConvertToWord();
    // Recent files, settings, file drop (MainWindowRecent.cpp).
    void LoadRecent();
    void AddRecent(const std::wstring& path);
    void ClearRecent();
    void RebuildRecentMenu();
    void ShowSettings();
    void OnFilesDropped(std::vector<std::wstring> files);
    void CancelConvert();
    // Modal prompts. Return false on cancel.
    bool PromptText(const std::wstring& title, const std::wstring& label, std::wstring* value,
                    bool password = false);
    bool PromptEncryption(std::string* user, std::string* owner, pdf::Permissions* perms);
    void ShowMessage(const std::wstring& title, const std::wstring& text);

    fluent::ConfigStore* cfg_ = nullptr;
    pdf::Engine* engine_ = nullptr;
    // Owned by the element tree.
    fluent::StatusBar* status_ = nullptr;   // bottom bar; left text = ShowStatus
    fluent::TextBlock* docInfo_ = nullptr;  // its right side: name, pages, zoom
    PdfPageView* view_ = nullptr;
    ThumbnailStrip* thumbs_ = nullptr;
    fluent::TextBox* pageBox_ = nullptr;
    fluent::TextBlock* pageTotal_ = nullptr;
    std::wstring docName_;
    // True from the first keystroke in the page box until Enter: while set,
    // scrolling does not overwrite the number being typed.
    bool pageBoxEditing_ = false;
    fluent::Subscription openSub_, zoomInSub_, zoomOutSub_, fitSub_, viewSub_, thumbSub_,
        pageKeySub_, pageTextSub_, searchKeySub_, searchPrevSub_, searchNextSub_, rootKeySub_,
        annotGestureSub_;

    // Search state. matches_ holds (page, index-on-page) in document order,
    // appended as chunks arrive; current_ indexes it (-1 = none shown yet).
    fluent::TextBox* searchBox_ = nullptr;
    fluent::TextBlock* searchCount_ = nullptr;
    std::unique_ptr<SearchRunner> search_;
    std::unique_ptr<ConvertRunner> convert_;
    std::wstring searchQuery_;  // query of the results currently shown
    std::vector<std::pair<int, int>> matches_;
    std::vector<int> perPage_;  // matches so far on each page, to number new ones
    int currentMatch_ = -1;
    int searchedPages_ = 0;
    bool searchCapped_ = false;
    // Bumped on every open and on destruction; a stale engine callback that
    // arrives after a newer open (or after the window is gone) is dropped.
    std::shared_ptr<int> generation_ = std::make_shared<int>(0);

    // Open document: its path, the password that opened it (memory only; never
    // traced or saved) for the qpdf tools, and unsaved-edit state.
    std::wstring docPath_;
    std::string docPassword_;
    bool dirty_ = false;
    // Edits posted but not yet applied. A tool or Save that starts while one is
    // queued would see the file without it, so they wait for zero.
    int pendingEdits_ = 0;
    // Menus must outlive the buttons that show them (Button keeps a raw pointer).
    std::unique_ptr<fluent::MenuFlyout> pageMenu_, toolMenu_, annotMenu_, recentMenu_;
    std::vector<std::wstring> recent_;  // newest first; mirrors config "recent.N"
};
