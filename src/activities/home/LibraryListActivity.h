#pragma once

#include <string>
#include <vector>

#include "activities/UiListActivity.h"
#include "util/LibraryGrouping.h"

// Third fileBrowserView option alongside the stock FileBrowserActivity (Files) and
// CoverGridBrowserActivity (Covers): a flat, paginated title+author list over the whole library,
// using the same UiListActivity row presentation RecentBooksActivity already draws. Reached from
// HomeActivity::onFileBrowserOpen() -- see ActivityManager::goToLibraryList().
//
// Like CoverGridBrowserActivity's Library source, this flattens the whole SD card rather than
// preserving folder navigation -- a title+author row has nothing useful to show for a folder.
//
// Series grouping (SETTINGS.groupBySeries): when a group of entries collapses into a series (see
// LibraryGrouping), selecting it drills into a second-level page listing that series' books in
// index order -- title as headline, author as subtitle, exactly like any other row. currentEntries()
// is the only thing that differs between the top-level page and a series page.
class LibraryListActivity final : public UiListActivity {
 public:
  explicit LibraryListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  void onEnter() override;
  void onExit() override;

 private:
  int listCount() const override { return currentCount(); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onRowLongPress(int index) override;
  // Long-press Confirm opens the per-book context menu; handled before the base's
  // short-press-activates/Back handling (see handleButtons()).
  bool handleCustomInput() override;
  // Back exits a series drill-down first, only going home once back at the top level.
  void onBackButton() override;
  void drawChrome() override;
  void drawFooter() override;

  std::vector<LibraryGrouping::Entry> topLevelEntries;
  // >= 0 while viewing a series page: the index into topLevelEntries of the series being viewed.
  // -1 at the top level.
  int seriesTopIndex = -1;
  // nav.selected at the top level, saved when drilling into a series and restored on Back out of
  // it.
  int savedTopLevelSelectedIndex = 0;

  // Row buffer, rebuilt whenever currentEntries() changes (loadBooks(), enterSeries(),
  // exitSeries()) rather than in buildScreen(), which reuses it on every repaint.
  std::vector<freeink::ui::ListItem> rowItems;
  // Owns the "title + series suffix" strings rowItems' series-row labels point into -- entry.title
  // alone isn't enough for those rows (see rebuildRowItems()). Parallel to rowItems, but only
  // series rows get an entry; non-series rows alias their Entry's own (already-stable) title.
  std::vector<std::string> seriesLabelStorage;
  void rebuildRowItems();

  std::vector<LibraryGrouping::Entry>& currentEntries();
  const std::vector<LibraryGrouping::Entry>& currentEntries() const;
  int currentCount() const { return static_cast<int>(currentEntries().size()); }

  // Library scan (or, when SETTINGS.groupBySeries is on and a valid index exists, the collapsed
  // index) sorted by filename.
  void loadBooks();
  // Header title + page-position subtitle. A series entry (only reachable at the top level) shows
  // its name with no subtitle; an individual book (standalone or drilled-in member) shows its
  // title and chapter-progress subtitle.
  void computeHeaderText(int index, std::string& outTitle, std::string& outSubtitle) const;

  void enterSeries(int topLevelIndex);
  void exitSeries();
  void activateSelected();
  // Two-level last-read search -- see CoverGridBrowserActivity::selectLastRead for the reasoning;
  // identical here since both views share LibraryGrouping's collapsed shape.
  void selectLastRead();
  // Opens the per-book context menu (long-press Confirm) -- see
  // CoverGridBrowserActivity::openContextMenu for the full reasoning (series-entry gating, why a
  // change snaps back to the top level). Never offers Remove from Recents: this view is always the
  // whole library, never Recent Books.
  void openContextMenu(const LibraryGrouping::Entry& entry);
};
