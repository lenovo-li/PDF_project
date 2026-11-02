// MainWindowSearch.cpp — search box, match navigation and window shortcuts.
// Split from MainWindow.cpp; the search itself runs in SearchRunner.

#include "MainWindow.h"

#include "controls/PdfPageView.h"

#include <FluentUI/window/NativeWindowHost.h>

#include <string>

using namespace fluent;

void MainWindow::OnSearchBoxKeyDown(UIElement&, KeyEventArgs& e) {
    if (e.vk == VK_RETURN) {
        e.handled = true;
        StepMatch(HasModifier(e.modifiers, ModifierKeys::Shift) ? -1 : +1);
    } else if (e.vk == VK_ESCAPE) {
        e.handled = true;
        if (search_) search_->Cancel();
        searchQuery_.clear();
        matches_.clear();
        perPage_.clear();
        currentMatch_ = -1;
        if (view_) view_->ClearHighlights();
        ShowSearchCount();
    }
}

void MainWindow::OnSearchPrevClicked(Button&, RoutedEventArgs&) { StepMatch(-1); }
void MainWindow::OnSearchNextClicked(Button&, RoutedEventArgs&) { StepMatch(+1); }

void MainWindow::OnRootPreviewKeyDown(UIElement&, KeyEventArgs& e) {
    const bool ctrl = HasModifier(e.modifiers, ModifierKeys::Ctrl);
    if (ctrl && e.vk == 'F' && searchBox_) {
        SetFocusElement(searchBox_);
        searchBox_->Select(0, static_cast<UINT32>(searchBox_->Text().size()));
        e.handled = true;
    } else if (ctrl && e.vk == 'S') {
        Save(HasModifier(e.modifiers, ModifierKeys::Shift));
        e.handled = true;
    } else if (e.vk == VK_F3) {
        StepMatch(HasModifier(e.modifiers, ModifierKeys::Shift) ? -1 : +1);
        e.handled = true;
    }
}

void MainWindow::StartSearch() {
    if (!view_ || !search_ || !searchBox_) return;
    view_->ClearHighlights();
    matches_.clear();
    perPage_.assign(static_cast<std::size_t>(view_->PageCount()), 0);
    currentMatch_ = -1;
    searchedPages_ = 0;
    searchCapped_ = false;
    searchQuery_ = searchBox_->Text();
    if (searchQuery_.empty() || view_->DocumentId() == pdf::kNoDocument) {
        ShowSearchCount();
        return;
    }
    SearchRunner::Callbacks cb;
    cb.onMatches = [this](std::vector<pdf::TextMatch>& batch) {
        view_->AddHighlights(batch);
        for (const pdf::TextMatch& m : batch) {
            if (m.page < 0 || m.page >= static_cast<int>(perPage_.size())) continue;
            matches_.emplace_back(m.page, perPage_[m.page]++);
        }
        // Show the first hit at or after the page being read as soon as it
        // exists, instead of waiting for the whole document.
        if (currentMatch_ < 0) {
            for (int i = 0; i < static_cast<int>(matches_.size()); ++i) {
                if (matches_[i].first >= view_->CurrentPage()) {
                    currentMatch_ = i;
                    view_->SetCurrentMatch(matches_[i].first, matches_[i].second);
                    break;
                }
            }
        }
        ShowSearchCount();
    };
    cb.onProgress = [this](int done, int) {
        searchedPages_ = done;
        ShowSearchCount();
    };
    cb.onDone = [this](int, bool capped) {
        searchCapped_ = searchCapped_ || capped;
        // Hits were all before the current page: wrap to the first one. Set it
        // directly; StepMatch could start a new search (if the box was edited)
        // and destroy this callback while it runs.
        if (currentMatch_ < 0 && !matches_.empty()) {
            currentMatch_ = 0;
            view_->SetCurrentMatch(matches_[0].first, matches_[0].second);
        }
        ShowSearchCount();
    };
    pdf::SearchOptions options;  // case-insensitive, substring
    search_->Start(view_->DocumentId(), view_->PageCount(), searchQuery_, options, std::move(cb));
    ShowSearchCount();
}

void MainWindow::StepMatch(int delta) {
    if (!view_ || !searchBox_) return;
    if (searchBox_->Text() != searchQuery_ || (matches_.empty() && !search_->Running())) {
        StartSearch();  // new query: the first result is shown as it arrives
        return;
    }
    if (matches_.empty()) return;
    const int n = static_cast<int>(matches_.size());
    if (currentMatch_ < 0) {
        currentMatch_ = delta < 0 ? n - 1 : 0;  // Shift+F3 with nothing shown: the last
    } else {
        currentMatch_ = ((currentMatch_ + delta) % n + n) % n;  // wraps both ways
    }
    view_->SetCurrentMatch(matches_[currentMatch_].first, matches_[currentMatch_].second);
    ShowSearchCount();
}

void MainWindow::ShowSearchCount() {
    if (!searchCount_) return;
    if (searchQuery_.empty()) {
        searchCount_->SetText(L"");
        return;
    }
    const int total = view_ ? view_->PageCount() : 0;
    std::wstring text;
    if (matches_.empty()) {
        text = search_ && search_->Running() ? L"搜索中…" : L"无结果";
    } else {
        text = std::to_wstring(currentMatch_ + 1) + L" / " + std::to_wstring(matches_.size());
        if (searchCapped_) text += L"+";
        text += L" 处";
    }
    if (search_ && search_->Running() && total > 0)
        text += L"（" + std::to_wstring(searchedPages_ * 100 / total) + L"%）";
    searchCount_->SetText(text);
}
