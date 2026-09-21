#include "LibraryListActivity.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "activities/reader/EpubReaderUtils.h"
#include "activities/util/BookContextMenuActivity.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#include "fontIds.h"

namespace fui = freeink::ui;

namespace {
// Physical-button hold threshold for the context-menu gesture (matches RecentBooksActivity's
// inline equivalent and CoverGridBrowserActivity's LongPressAction).
constexpr unsigned long LONG_PRESS_MS = 1000;
}  // namespace

LibraryListActivity::LibraryListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("LibraryList", renderer, mappedInput, /*wantsTouchLongPress=*/true), longPressAction(LONG_PRESS_MS) {}

std::vector<LibraryGrouping::Entry>& LibraryListActivity::currentEntries() {
  if (seriesTopIndex >= 0 && seriesTopIndex < static_cast<int>(topLevelEntries.size())) {
    return topLevelEntries[seriesTopIndex].members;
  }
  return topLevelEntries;
}

const std::vector<LibraryGrouping::Entry>& LibraryListActivity::currentEntries() const {
  if (seriesTopIndex >= 0 && seriesTopIndex < static_cast<int>(topLevelEntries.size())) {
    return topLevelEntries[seriesTopIndex].members;
  }
  return topLevelEntries;
}

void LibraryListActivity::loadBooks() {
  topLevelEntries = LibraryGrouping::loadLibraryEntries(SETTINGS.groupBySeries);
  rebuildRowItems();
}

// Derives rowItems from currentEntries(). Called whenever the current page's entry set changes
// (loadBooks(), enterSeries(), exitSeries()) so buildScreen() reuses the cached rows on every
// repaint instead of rebuilding them per render -- same convention as RecentBooksActivity.
void LibraryListActivity::rebuildRowItems() {
  const auto& entries = currentEntries();
  rowItems.clear();
  rowItems.reserve(entries.size());
  seriesLabelStorage.clear();
  seriesLabelStorage.reserve(entries.size());
  for (const auto& entry : entries) {
    fui::ListItem item;
    // Text is the only row element every theme actually renders -- RoundedRaffTheme discards
    // rowIcon entirely and BaseTheme never draws it on a subtitle row, so a series row signalled
    // only by icon is invisible on half the themes. A title suffix works identically everywhere,
    // so it's the row's sole "this is a series" signal. entry.title already owns stable storage
    // (topLevelEntries/members aren't mutated until the next loadBooks()), so only the suffixed
    // case needs its own copy.
    item.label = entry.isSeries ? nullptr : entry.title.c_str();
    if (!entry.author.empty()) item.subtitle = entry.author.c_str();
    // Reuses the codebase's existing "this row navigates into a container, not a leaf item"
    // signal -- the Folder icon FileBrowserActivity shows for directories -- rather than
    // inventing a second visual language (chevron, count, prefix glyph) for the same idea.
    item.icon = listIconFor(entry.isSeries ? UIIcon::Folder : UITheme::getFileIcon(entry.path), 32);
    item.actionValue = static_cast<int16_t>(rowItems.size());
    rowItems.push_back(item);
  }
  // Series rows need a suffixed label with its own persistent storage (entry.title alone isn't
  // enough) -- filled in as a second pass so rowItems is done reallocating first and the c_str()
  // pointers assigned into it are stable. seriesLabelStorage was reserve()d to entries.size()
  // above, so pushing at most one entry per row here can't reallocate it either.
  for (size_t i = 0; i < entries.size(); i++) {
    if (entries[i].isSeries) {
      seriesLabelStorage.push_back(entries[i].title + tr(STR_SERIES_SUFFIX));
      rowItems[i].label = seriesLabelStorage.back().c_str();
    }
  }

  const auto count = static_cast<uint32_t>(entries.size());
  renderer.prewarmFallbackText(
      uiScaleSpec().smallFontId,
      [](const void* ctx, uint32_t i) -> const char* {
        return (*static_cast<const std::vector<LibraryGrouping::Entry>*>(ctx))[i].title.c_str();
      },
      &entries, count, EpdFontFamily::BOLD);
  renderer.prewarmFallbackText(
      uiScaleSpec().smallFontId,
      [](const void* ctx, uint32_t i) -> const char* {
        return (*static_cast<const std::vector<LibraryGrouping::Entry>*>(ctx))[i].author.c_str();
      },
      &entries, count);
}

void LibraryListActivity::computeHeaderText(const int index, std::string& outTitle, std::string& outSubtitle) const {
  outTitle.clear();
  outSubtitle.clear();
  const auto& entries = currentEntries();
  if (index < 0 || index >= static_cast<int>(entries.size())) {
    return;
  }
  const auto& entry = entries[index];
  outTitle = entry.title;

  if (entry.isSeries) {
    // No single book's reading progress applies to a collapsed multi-book entry.
    return;
  }

  if (FsHelpers::hasEpubExtension(entry.path)) {
    const Epub epub(entry.path, "/.crosspoint");
    int spineIndex = 0;
    int pageNumber = 0;
    int pageCount = 0;
    if (EpubReaderUtils::loadProgress(epub.getCachePath(), spineIndex, pageNumber, pageCount) && pageCount > 0) {
      outSubtitle = std::to_string(pageNumber + 1) + "/" + std::to_string(pageCount);
    }
  }
}

void LibraryListActivity::enterSeries(const int topLevelIndex) {
  savedTopLevelSelectedIndex = topLevelIndex;
  seriesTopIndex = topLevelIndex;
  rebuildRowItems();
  moveSelectionTo(0);
}

void LibraryListActivity::exitSeries() {
  seriesTopIndex = -1;
  rebuildRowItems();
  moveSelectionTo(savedTopLevelSelectedIndex);
}

void LibraryListActivity::activateSelected() {
  auto& entries = currentEntries();
  const int selected = nav.selected;
  if (selected < 0 || selected >= static_cast<int>(entries.size())) {
    return;
  }
  if (entries[selected].isSeries) {
    enterSeries(selected);
  } else {
    app.clearTapFlash();
    activityManager.goToReader(entries[selected].path, /*allowFastInitialRefresh=*/false);
  }
}

void LibraryListActivity::activateIndex(const int index) {
  if (index < 0 || index >= listCount()) return;
  nav.selected = index;
  activateSelected();
}

void LibraryListActivity::onRowLongPress(const int index) {
  const auto& entries = currentEntries();
  if (index < 0 || index >= static_cast<int>(entries.size()) || entries[index].isSeries) return;
  app.clearTapFlash();
  openContextMenu(entries[index]);
}

void LibraryListActivity::selectLastRead() {
  seriesTopIndex = -1;
  if (topLevelEntries.empty()) {
    return;
  }
  const auto& recents = RECENT_BOOKS.getBooks();
  if (recents.empty()) {
    return;
  }
  const std::string& lastReadPath = recents[0].path;

  for (int i = 0; i < static_cast<int>(topLevelEntries.size()); i++) {
    const auto& entry = topLevelEntries[i];
    if (!entry.isSeries) {
      if (entry.path == lastReadPath) {
        nav.selected = i;
        return;
      }
      continue;
    }
    for (int m = 0; m < static_cast<int>(entry.members.size()); m++) {
      if (entry.members[m].path == lastReadPath) {
        // See CoverGridBrowserActivity::selectLastRead() for the full reasoning -- identical here
        // since both views share LibraryGrouping's collapsed shape.
        if (SETTINGS.browseBooksStartInSeries) {
          seriesTopIndex = i;
          savedTopLevelSelectedIndex = i;
          nav.selected = m;
        } else {
          nav.selected = i;
        }
        return;
      }
    }
  }
}

void LibraryListActivity::openContextMenu(const LibraryGrouping::Entry& entry) {
  const std::string path = entry.path;
  const std::string title = entry.title;

  auto handler = [this](const ActivityResult& res) {
    const auto* result = std::get_if<BookActionResult>(&res.data);
    if (!result || !result->changed) {
      return;
    }
    seriesTopIndex = -1;  // the list just changed underneath any drill-down; snap back to top level
    loadBooks();
    const auto& entries = currentEntries();
    int selected = nav.selected;
    if (entries.empty()) {
      selected = 0;
    } else if (selected >= static_cast<int>(entries.size())) {
      selected = static_cast<int>(entries.size()) - 1;
    }
    moveSelectionTo(selected);
  };

  startActivityForResult(std::make_unique<BookContextMenuActivity>(renderer, mappedInput, path, title,
                                                                    BookContextMenuActivity::Available{}),
                          std::move(handler));
}

void LibraryListActivity::onEnter() {
  UiListActivity::onEnter();

  loadBooks();
  // Lands on the last-read book, same as CoverGridBrowserActivity -- both are whole-library
  // metadata views with no folder concept of their own to return to. Drills directly into a
  // series if the last-read book is one of its members (see selectLastRead()).
  selectLastRead();
  if (seriesTopIndex >= 0) {
    rebuildRowItems();
  }
}

void LibraryListActivity::onExit() {
  Activity::onExit();
  rowItems.clear();
  seriesLabelStorage.clear();
  topLevelEntries.clear();
}

bool LibraryListActivity::handleCustomInput() {
  // Long-press Confirm opens the per-book context menu; short-press (handled by the base's
  // handleButtons()) activates/drills in. update() must run every tick regardless of selection
  // validity -- it also owns swallowing the eventual release so it doesn't also fall through to
  // the base's short-press handler.
  if (longPressAction.update(mappedInput, MappedInputManager::Button::Confirm)) {
    const auto& entries = currentEntries();
    const int selected = nav.selected;
    if (selected >= 0 && selected < static_cast<int>(entries.size()) && !entries[selected].isSeries) {
      openContextMenu(entries[selected]);
    }
    return true;
  }
  return false;
}

void LibraryListActivity::onBackButton() {
  if (seriesTopIndex >= 0) {
    exitSeries();
  } else {
    onGoHome();
  }
}

void LibraryListActivity::drawChrome() {
  const auto pageWidth = renderer.getScreenWidth();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto& entries = currentEntries();

  std::string headerTitle;
  std::string subtitle;
  if (entries.empty()) {
    headerTitle = tr(STR_NO_BOOKS_FOUND);
  } else {
    computeHeaderText(nav.selected, headerTitle, subtitle);
  }
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 headerTitle.empty() ? nullptr : headerTitle.c_str(), subtitle.empty() ? nullptr : subtitle.c_str());
}

void LibraryListActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  const auto& entries = currentEntries();
  if (entries.empty()) {
    screen.centeredText(tr(STR_NO_BOOKS_FOUND), screen.theme().bodyText);
    return;
  }

  fui::ListProps props;
  props.items = rowItems.data();
  props.count = static_cast<uint16_t>(rowItems.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch | fui::InputLongPress;
  fui::TextStyle label = screen.theme().smallText;
  label.bold = true;
  props.labelText = label;
  syncListViewport(screen, props, /*hasSubtitle=*/true);
  screen.list(props);
}

void LibraryListActivity::drawFooter() {
  const bool empty = currentEntries().empty();
  const auto labels =
      mappedInput.mapLabels(seriesTopIndex >= 0 ? tr(STR_BACK) : tr(STR_HOME), empty ? "" : tr(STR_OPEN),
                            empty ? "" : tr(STR_DIR_UP), empty ? "" : tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
