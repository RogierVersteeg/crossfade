#include "HomeActivity.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <I18n.h>
#include <LibraryBuilder.h>
#include <LibraryIndexFile.h>
#include <Memory.h>
#include <Utf8.h>
#include <Xtc.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "components/themes/lyra/LyraCarouselTheme.h"
#include "fontIds.h"
#include "activities/reader/EpubReaderUtils.h"

int HomeActivity::getMenuItemCount() const {
  int count = 4;  // File Browser, Library, File transfer, Settings
  if (!recentBooks.empty()) {
    count += recentBooks.size();
  }
  if (hasOpdsServers) {
    count++;
  }
  return count;
}

void HomeActivity::loadRecentBooks(int maxBooks) {
  recentBooks.clear();
  const auto& books = RECENT_BOOKS.getBooks();
  recentBooks.reserve(coverGridUi ? maxBooks : std::min(static_cast<int>(books.size()), maxBooks));

  for (const RecentBook& book : books) {
    // Limit to maximum number of recent books
    if (recentBooks.size() >= maxBooks) {
      break;
    }

    // Skip if file no longer exists
    if (RecentBooksStore::isMissing(book)) {
      continue;
    }

    recentBooks.push_back(book);
  }
}

void HomeActivity::fillCoverGridFromLibrary() {
  if (recentBooks.size() >= CoverGridHomeUi::MAX_BOOKS) return;
  // Keep the index and record together off the task stack; reuse for every row.
  struct LibraryReader {
    library::LibraryIndexFile index;
    library::ClixRecord record;
  };
  auto reader = makeUniqueNoThrow<LibraryReader>();
  if (!reader) {
    LOG_ERR("HOME", "OOM: library index");
    return;
  }
  auto& index = reader->index;
  auto& record = reader->record;
  if (!index.open(library::libraryIndexPath())) {
    index.close();
    GUI.drawPopup(renderer, tr(STR_LIBRARY_REBUILDING));
    library::BuildStats stats;
    if (!library::buildLibraryIndex("/", stats, SETTINGS.libraryUseMetadata != 0) ||
        !index.open(library::libraryIndexPath())) {
      LOG_ERR("HOME", "Cannot populate cover grid from library");
      return;
    }
  }
  for (uint16_t row = 0; row < index.bookCount() && recentBooks.size() < CoverGridHomeUi::MAX_BOOKS; ++row) {
    RecentBook book;
    if (!index.readRecord(index.ordinalForRow(library::SortOrder::RecentDesc, row), record) ||
        !index.readPath(record, book.path))
      continue;
    if (std::any_of(recentBooks.begin(), recentBooks.end(),
                    [&](const RecentBook& existing) { return existing.path == book.path; }) ||
        RecentBooksStore::isMissing(book))
      continue;
    if (!index.readTitle(record, book.title) && !index.readName(record, book.title)) continue;
    index.readAuthor(record, book.author);
    if (index.ioFailed()) break;
    recentBooks.push_back(std::move(book));
  }
}

void HomeActivity::resolveGridCoverPaths() {
  for (auto& book : recentBooks) {
    if (!book.coverBmpPath.empty()) continue;
    // Constructors only derive cache paths; no metadata parsing or image generation.
    // Keep these large objects off the task stack and release each before the next book.
    if (FsHelpers::hasEpubExtension(book.path)) {
      auto epub = makeUniqueNoThrow<Epub>(book.path, "/.crosspoint");
      if (!epub) {
        LOG_ERR("HOME", "OOM: EPUB thumbnail path");
        continue;
      }
      book.coverBmpPath = epub->getThumbBmpPath();
    } else if (FsHelpers::hasXtcExtension(book.path)) {
      auto xtc = makeUniqueNoThrow<Xtc>(book.path, "/.crosspoint");
      if (!xtc) {
        LOG_ERR("HOME", "OOM: XTC thumbnail path");
        continue;
      }
      book.coverBmpPath = xtc->getThumbBmpPath();
    }
  }
}

void HomeActivity::loadGridCover(RecentBook& book, int height, bool& showingLoading, Rect& popupRect) {
  if (!book.coverBmpPath.empty() && Storage.exists(UITheme::getCoverThumbPath(book.coverBmpPath, height).c_str()))
    return;
  // Only one parser lives at a time; EPUB/XTC objects exceed the stack budget.
  if (FsHelpers::hasEpubExtension(book.path)) {
    auto epub = makeUniqueNoThrow<Epub>(book.path, "/.crosspoint");
    if (!epub) {
      LOG_ERR("HOME", "OOM: cover EPUB");
      return;
    }
    book.coverBmpPath = epub->getThumbBmpPath();
    if (Storage.exists(epub->getThumbBmpPath(height).c_str())) return;
    if (!showingLoading) {
      showingLoading = true;
      popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
      GUI.fillPopupProgress(renderer, popupRect, 0);
    }
    if (epub->generateThumbBmpFromSource(height)) {
      return;
    }
  } else if (FsHelpers::hasXtcExtension(book.path)) {
    auto xtc = makeUniqueNoThrow<Xtc>(book.path, "/.crosspoint");
    if (!xtc) {
      LOG_ERR("HOME", "OOM: cover XTC");
      return;
    }
    book.coverBmpPath = xtc->getThumbBmpPath();
    if (Storage.exists(xtc->getThumbBmpPath(height).c_str())) return;
    if (!showingLoading) {
      showingLoading = true;
      popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
      GUI.fillPopupProgress(renderer, popupRect, 0);
    }
    if (xtc->load() && xtc->generateThumbBmp(height)) {
      return;
    }
  }
  book.coverBmpPath.clear();
}

void HomeActivity::loadRecentCovers(int coverHeight) {
  recentsLoading = true;
  bool showingLoading = false;
  Rect popupRect;

  // Carousel needs two purpose-sized caches (center box, side trapezoids) instead of the one
  // height-keyed cache every other theme's single Continue Reading card uses -- see
  // LyraCarouselTheme::kCenterThumbW/kSideCoverW's comment for why sharing one cache produced
  // near-blank covers for off-aspect-ratio art.
  const bool isCarouselTheme =
      static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme) == CrossPointSettings::UI_THEME::LYRA_CAROUSEL;

  int progress = 0;
  for (RecentBook& book : recentBooks) {
    // The cover grid shares one slot size; generating at any other height
    // would rescale the dithered thumb at draw time and alias badly.
    const int thumbHeight = coverGridUi ? coverGridUi->thumbHeightFor() : coverHeight;
    if (coverGridUi) {
      loadGridCover(book, thumbHeight, showingLoading, popupRect);
      ++progress;
      if (showingLoading) GUI.fillPopupProgress(renderer, popupRect, progress * 100 / recentBooks.size());
      continue;
    }
    if (!book.coverBmpPath.empty()) {
      // Self-heal a stale coverBmpPath template. Confirmed on real hardware: a book added to
      // Recents by a different build's cover-path scheme (e.g. a "[WIDTH]x[HEIGHT]"-shaped
      // template no CrossFade code has ever produced -- this fork's own history never contained
      // that string) leaves a path that getCoverThumbPath's substitution -- it only knows
      // "[HEIGHT]" -- can never fully resolve, so it looks up a filename like
      // "thumb_[WIDTH]x400.bmp" that will never exist, on every theme, forever. The template is
      // wrong, not merely the file missing, so re-derive it fresh every time rather than retrying
      // a path that can't succeed -- cheap (no I/O until .load()), and guarantees this always
      // matches what EpubReaderActivity writes on open and what generateThumbBmp below writes.
      std::string freshTemplate;
      if (FsHelpers::hasEpubExtension(book.path)) {
        freshTemplate = Epub(book.path, "/.crosspoint").getThumbBmpPath();
      } else if (FsHelpers::hasXtcExtension(book.path)) {
        freshTemplate = Xtc(book.path, "/.crosspoint").getThumbBmpPath();
      }
      if (!freshTemplate.empty() && freshTemplate != book.coverBmpPath) {
        book.coverBmpPath = freshTemplate;
        RECENT_BOOKS.updateBook(book.path, book.title, book.author, freshTemplate);
        // Also invalidate coverBufferStored, not just coverRendered: HomeActivity::render()
        // restores the stored buffer (if any) BEFORE calling into the theme on every render, off
        // whatever coverBufferStored was left at here. Leaving it true would let that restore
        // keep re-serving the pre-fix (stale) buffer, silently suppressing the redraw this
        // coverRendered reset was meant to force -- same class of bug as the centerIdx gate race.
        coverRendered = false;
        coverBufferStored = false;
        requestUpdate();
      }

      bool needsCenter = false;
      bool needsSide = false;
      bool needsLegacy = false;
      if (isCarouselTheme) {
        const std::string centerPath = UITheme::getCoverThumbPath(
            book.coverBmpPath, LyraCarouselTheme::kCenterThumbW, LyraCarouselTheme::kCenterThumbH);
        const std::string sidePath = UITheme::getCoverThumbPath(book.coverBmpPath, LyraCarouselTheme::kSideCoverW,
                                                                 LyraCarouselTheme::kSideCoverH);
        needsCenter = !Storage.exists(centerPath.c_str());
        needsSide = !Storage.exists(sidePath.c_str());
      } else {
        const std::string coverPath = UITheme::getCoverThumbPath(book.coverBmpPath, thumbHeight);
        needsLegacy = !Storage.exists(coverPath.c_str());
      }

      if (needsCenter || needsSide || needsLegacy) {
        // If epub, try to load the metadata for title/author and cover
        if (FsHelpers::hasEpubExtension(book.path)) {
          Epub epub(book.path, "/.crosspoint");
          // Skip loading css since we only need metadata here. buildIfMissing=false, so a book
          // whose book.bin cache isn't already built (anything except the just-opened book, whose
          // cache is guaranteed fresh) legitimately fails to load here -- previously this return
          // value was ignored and generateThumbBmp() got called anyway, almost always failing too,
          // which permanently wiped coverBmpPath (see below) for a book that may just need its
          // cache built later, not one with no cover. Skip it for this pass instead; loadRecentBooks
          // reloads coverBmpPath fresh next time Home is entered, so this isn't a permanent loss.
          if (epub.load(false, true)) {
            // Try to generate thumbnail image for Continue Reading card
            if (!showingLoading) {
              showingLoading = true;
              popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
            }
            GUI.fillPopupProgress(renderer, popupRect, 10 + progress * (90 / recentBooks.size()));
            bool success = true;
            if (isCarouselTheme) {
              if (needsCenter)
                success = epub.generateThumbBmp(LyraCarouselTheme::kCenterThumbW, LyraCarouselTheme::kCenterThumbH) &&
                          success;
              if (needsSide)
                success =
                    epub.generateThumbBmp(LyraCarouselTheme::kSideCoverW, LyraCarouselTheme::kSideCoverH) && success;
            } else {
              success = epub.generateThumbBmp(thumbHeight);
            }
            if (!success) {
              RECENT_BOOKS.updateBook(book.path, book.title, book.author, "");
              book.coverBmpPath = "";
            }
            coverRendered = false;
            coverBufferStored = false;
            requestUpdate();
          }
        } else if (FsHelpers::hasXtcExtension(book.path)) {
          // Handle XTC file
          Xtc xtc(book.path, "/.crosspoint");
          if (xtc.load()) {
            // Try to generate thumbnail image for Continue Reading card
            if (!showingLoading) {
              showingLoading = true;
              popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
            }
            GUI.fillPopupProgress(renderer, popupRect, 10 + progress * (90 / recentBooks.size()));
            bool success = true;
            if (isCarouselTheme) {
              if (needsCenter)
                success = xtc.generateThumbBmp(LyraCarouselTheme::kCenterThumbW, LyraCarouselTheme::kCenterThumbH) &&
                          success;
              if (needsSide)
                success =
                    xtc.generateThumbBmp(LyraCarouselTheme::kSideCoverW, LyraCarouselTheme::kSideCoverH) && success;
            } else {
              success = xtc.generateThumbBmp(thumbHeight);
            }
            if (!success) {
              RECENT_BOOKS.updateBook(book.path, book.title, book.author, "");
              book.coverBmpPath = "";
            }
            coverRendered = false;
            coverBufferStored = false;
            requestUpdate();
          }
        }
      }
    }
    progress++;
  }

  recentsLoaded = true;
  recentsLoading = false;
}

void HomeActivity::onEnter() {
  Activity::onEnter();

  hasOpdsServers = OPDS_STORE.hasServers();

  const auto& metrics = UITheme::getInstance().getMetrics();
  if (UITheme::getInstance().hasCoverGridHome()) {
    // Screen-lifetime interaction tables and component properties exceed the stack budget.
    coverGridUi = makeUniqueNoThrow<CoverGridHomeUi>(renderer);
    if (!coverGridUi) LOG_ERR("HOME", "OOM: cover grid UI; using standard home");
  }
  loadRecentBooks(coverGridUi ? CoverGridHomeUi::MAX_BOOKS : metrics.homeRecentBooksCount);
  hasContinueReading = !recentBooks.empty();
  if (coverGridUi) {
    fillCoverGridFromLibrary();
    resolveGridCoverPaths();
    coverGridUi->begin(recentBooks, hasOpdsServers, hasContinueReading);
  }

  const auto base = static_cast<int>(recentBooks.size());
  selectorIndex = initialMenuItem == HomeMenuItem::NONE ? 0 : base + menuItemToIndex(initialMenuItem, hasOpdsServers);

  // Trigger first update
  requestUpdate();
}

void HomeActivity::onExit() {
  Activity::onExit();

  coverGridUi.reset();

  // Free the stored cover buffer if any
  freeCoverBuffer();
}

bool HomeActivity::storeCoverBuffer() {
  // render() must have already set the cover rect; without it we'd be back to
  // cloning the whole framebuffer.
  if (coverRectW <= 0 || coverRectH <= 0) return false;
  freeCoverBuffer();
  const size_t needed = renderer.getRegionByteSize(coverRectX, coverRectY, coverRectW, coverRectH);
  if (needed == 0) return false;
  coverBuffer = static_cast<uint8_t*>(malloc(needed));
  if (!coverBuffer) {
    LOG_ERR("HOME", "OOM: cover buffer (%u bytes)", (unsigned)needed);
    return false;
  }
  coverBufferSize = needed;
  if (!renderer.copyRegionToBuffer(coverRectX, coverRectY, coverRectW, coverRectH, coverBuffer, coverBufferSize)) {
    free(coverBuffer);
    coverBuffer = nullptr;
    coverBufferSize = 0;
    return false;
  }
  return true;
}

bool HomeActivity::restoreCoverBuffer() {
  if (!coverBuffer || coverRectW <= 0 || coverRectH <= 0) return false;
  return renderer.copyBufferToRegion(coverRectX, coverRectY, coverRectW, coverRectH, coverBuffer, coverBufferSize);
}

void HomeActivity::freeCoverBuffer() {
  if (coverBuffer) {
    free(coverBuffer);
    coverBuffer = nullptr;
  }
  coverBufferSize = 0;
  coverBufferStored = false;
}

void HomeActivity::loop() {
  const int menuCount = getMenuItemCount();
  const auto& metrics = UITheme::getInstance().getMetrics();

  auto activateSelection = [this] {
    if (selectorIndex < recentBooks.size()) {
      onSelectBook(recentBooks[selectorIndex].path);
      return;
    }
    const int menuIndex = selectorIndex - static_cast<int>(recentBooks.size());
    switch (indexToMenuItem(menuIndex, hasOpdsServers)) {
      case HomeMenuItem::FILE_BROWSER:
        onFileBrowserOpen();
        break;
      case HomeMenuItem::LIBRARY:
        onLibraryOpen();
        break;
      case HomeMenuItem::OPDS_BROWSER:
        onOpdsBrowserOpen();
        break;
      case HomeMenuItem::FILE_TRANSFER:
        onFileTransferOpen();
        break;
      case HomeMenuItem::SETTINGS_MENU:
        onSettingsOpen();
        break;
      default:
        break;
    }
  };

  const bool isCarouselTheme =
      static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme) == CrossPointSettings::UI_THEME::LYRA_CAROUSEL;

  if (isCarouselTheme) {
    // Two-level nav, this theme only: Left/Right move within whichever level selectorIndex is
    // currently in (carousel books, or menu icons); Up/Down -- either one, since there are only
    // two levels here, so both mean "switch level" -- cross between them, restoring each level's
    // own last position (lastCarouselIndex/lastMenuIndex) instead of resetting to index 0.
    // Bypasses NavNext/NavPrevious (which composite side Up/Down with front Left/Right into one
    // axis -- see MappedInputManager::mapButton) so the two physical pairs can mean different
    // things here. Back/Confirm are untouched, so Resume/Select on the front-left buttons behave
    // exactly as before.
    const int bookCount = static_cast<int>(recentBooks.size());
    const int menuOnlyCount = menuCount - bookCount;

    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Right}, [this, bookCount, menuOnlyCount] {
      if (selectorIndex < bookCount) {
        selectorIndex = ButtonNavigator::nextIndex(selectorIndex, bookCount);
      } else {
        selectorIndex = bookCount + ButtonNavigator::nextIndex(selectorIndex - bookCount, menuOnlyCount);
      }
      requestUpdate();
    });

    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Left}, [this, bookCount, menuOnlyCount] {
      if (selectorIndex < bookCount) {
        selectorIndex = ButtonNavigator::previousIndex(selectorIndex, bookCount);
      } else {
        selectorIndex = bookCount + ButtonNavigator::previousIndex(selectorIndex - bookCount, menuOnlyCount);
      }
      requestUpdate();
    });

    const auto switchLevel = [this, bookCount, menuOnlyCount] {
      if (selectorIndex < bookCount) {
        if (menuOnlyCount <= 0) return;
        lastCarouselIndex = selectorIndex;
        selectorIndex = bookCount + std::clamp(lastMenuIndex, 0, menuOnlyCount - 1);
      } else {
        if (bookCount <= 0) return;
        lastMenuIndex = selectorIndex - bookCount;
        selectorIndex = std::clamp(lastCarouselIndex, 0, bookCount - 1);
      }
      requestUpdate();
    };
    buttonNavigator.onPress({MappedInputManager::Button::Up, MappedInputManager::Button::Down}, switchLevel);
  } else if (!coverGridUi) {
    // Cover grid home splits navigation by button group (see below); the flat
    // next/previous cycle is for the classic list home only.
    buttonNavigator.onNext([this, menuCount] {
      selectorIndex = ButtonNavigator::nextIndex(selectorIndex, menuCount);
      requestUpdate();
    });

    buttonNavigator.onPrevious([this, menuCount] {
      selectorIndex = ButtonNavigator::previousIndex(selectorIndex, menuCount);
      requestUpdate();
    });
  }

  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up) {
    selectorIndex = ButtonNavigator::nextIndex(selectorIndex, menuCount);
    requestUpdate();
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Down) {
    selectorIndex = ButtonNavigator::previousIndex(selectorIndex, menuCount);
    requestUpdate();
    return;
  }

  // Back is otherwise unused on the home menu: open the most recently read
  // book directly (recentBooks is most-recent-first and already pruned of
  // files missing from the SD card).
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) && hasContinueReading && !recentBooks.empty()) {
    onSelectBook(recentBooks[0].path);
    return;
  }

  if (isCarouselTheme) {
    // Touch boards (X4 Pro): the carousel's covers and icon tiles sit nowhere near the
    // metrics-driven rows the generic touch mapping below assumes, so hit-test the theme's own
    // geometry instead. Left/Right swipes walk the carousel; a tap on a side cover centers it, a
    // tap on the centered cover opens it, and a tap on an icon tile activates that menu entry.
    const int bookCount = static_cast<int>(recentBooks.size());
    const int menuOnlyCount = menuCount - bookCount;
    const int center = bookCount == 0 ? -1
                       : (selectorIndex < bookCount ? selectorIndex : std::clamp(lastCarouselIndex, 0, bookCount - 1));
    const auto carouselSwipe = mappedInput.wasSwipe();
    if (center >= 0 &&
        (carouselSwipe == MappedInputManager::SwipeDir::Left || carouselSwipe == MappedInputManager::SwipeDir::Right)) {
      selectorIndex = carouselSwipe == MappedInputManager::SwipeDir::Left
                          ? ButtonNavigator::nextIndex(center, bookCount)
                          : ButtonNavigator::previousIndex(center, bookCount);
      lastCarouselIndex = selectorIndex;
      requestUpdate();
      return;
    }
    int tx = 0;
    int ty = 0;
    if (mappedInput.wasScreenTapped(tx, ty)) {
      const auto& metrics = UITheme::getInstance().getMetrics();
      const int book = LyraCarouselTheme::hitTestCover(
          renderer, Rect{0, metrics.homeTopPadding, renderer.getScreenWidth(), metrics.homeCoverTileHeight}, bookCount,
          center, tx, ty);
      if (book >= 0) {
        if (book == center) {
          selectorIndex = book;
          activateSelection();
        } else {
          lastCarouselIndex = book;
          selectorIndex = book;
          requestUpdate();
        }
        return;
      }
      const int tile = LyraCarouselTheme::hitTestMenuTile(renderer, menuOnlyCount, tx, ty);
      if (tile >= 0) {
        selectorIndex = bookCount + tile;
        activateSelection();
        return;
      }
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      activateSelection();
    }
    return;
  }

  if (coverGridUi) {
    const int touched = coverGridUi->selectedAction(mappedInput);
    if (touched >= 0 && touched < menuCount) {
      selectorIndex = touched;
      activateSelection();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      activateSelection();
      return;
    }
    // Side page buttons walk the covers, front Left/Right walk the tabs
    // (selectorIndex is flat: books first, then the tab items). A press while
    // selection sits in the other band jumps into this band first.
    const int bookCount = static_cast<int>(recentBooks.size());
    const auto cycleBand = [this](const int base, const int count, const int dir) {
      if (count <= 0) return;
      int idx = selectorIndex - base;
      if (idx < 0 || idx >= count) {
        idx = dir > 0 ? 0 : count - 1;
      } else {
        idx = (idx + count + dir) % count;
      }
      selectorIndex = base + idx;
      requestUpdate();
    };
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Up},
                                         [&cycleBand, bookCount] { cycleBand(0, bookCount, -1); });
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Down},
                                         [&cycleBand, bookCount] { cycleBand(0, bookCount, +1); });
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Left}, [&cycleBand, bookCount, menuCount] {
      cycleBand(bookCount, menuCount - bookCount, -1);
    });
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Right}, [&cycleBand, bookCount, menuCount] {
      cycleBand(bookCount, menuCount - bookCount, +1);
    });
    return;
  }

  const int coverColumnCount = std::max(1, metrics.homeRecentBooksCount);
  const int recentCount = std::min(static_cast<int>(recentBooks.size()), coverColumnCount);
  const int coverColumnWidth = (renderer.getScreenWidth() - 2 * metrics.contentSidePadding) / coverColumnCount;
  int touchedBook = -1;
  const auto coverTouch = mappedInput.colTouch(touchedBook, metrics.contentSidePadding, coverColumnWidth, recentCount,
                                               metrics.homeTopPadding,
                                               metrics.homeTopPadding + metrics.homeCoverTileHeight, coverColumnWidth);
  if (coverTouch != MappedInputManager::RowTouch::None) {
    if (coverTouch == MappedInputManager::RowTouch::Down) {
      if (selectorIndex != touchedBook) {
        selectorIndex = touchedBook;
        requestUpdate();
      }
    } else {
      selectorIndex = touchedBook;
      activateSelection();
    }
    return;
  }

  const int menuTop = metrics.homeTopPadding + metrics.homeCoverTileHeight + metrics.homeMenuTopOffset;
  const int renderedMenuCount =
      menuCount - (metrics.homeContinueReadingInMenu ? 0 : static_cast<int>(recentBooks.size()));
  int menuRow = -1;
  // Row height from the theme, not the metrics table: RoundedRaff draws
  // font-derived rows and the touch grid must match the visuals exactly.
  const int menuRowHeight = GUI.getMenuRowHeight(renderer);
  const auto menuTouch = mappedInput.rowTouch(menuRow, menuTop, menuRowHeight + metrics.menuSpacing, renderedMenuCount,
                                              0, INT32_MAX, menuRowHeight);
  if (menuTouch != MappedInputManager::RowTouch::None) {
    const int touchedIndex =
        metrics.homeContinueReadingInMenu ? menuRow : menuRow + static_cast<int>(recentBooks.size());
    if (menuTouch == MappedInputManager::RowTouch::Down) {
      if (selectorIndex != touchedIndex) {
        selectorIndex = touchedIndex;
        requestUpdate();
      }
    } else {
      selectorIndex = touchedIndex;
      activateSelection();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateSelection();
  }
}

void HomeActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  if (coverGridUi) {
    coverGridUi->setSelection(selectorIndex);
    UITheme::getInstance().drawCoverGridHome(*coverGridUi);
    // Front Left/Right walk the tabs, so their hints read Left/Right; the
    // side page buttons (unhinted) walk the covers.
    const auto labels = mappedInput.mapLabels(hasContinueReading ? tr(STR_RESUME) : "", tr(STR_SELECT),
                                              tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer(cleanInitialRefresh && !firstRenderDone ? HalDisplay::HALF_REFRESH
                                                                   : HalDisplay::FAST_REFRESH);
    // Slot heights are recorded during the draw above; a change (first layout
    // pass, orientation switch) means the paths must point at those sizes and
    // any missing thumbs must be generated. Refreshing the paths right away
    // lets the next pass draw already-cached thumbs before generation runs.
    const bool coverSpecChanged = coverGridUi->takeThumbHeightChanged();
    if (coverSpecChanged) {
      coverGridUi->refreshCoverPaths();
      recentsLoaded = false;
    }
    if (!firstRenderDone) {
      firstRenderDone = true;
      requestUpdate();
    } else if (!recentsLoaded && !recentsLoading) {
      loadRecentCovers(CoverGridHomeUi::THUMB_HEIGHT);
      coverGridUi->refreshCoverPaths();
      requestUpdate();
    }
    return;
  }
  bool bufferRestored = coverBufferStored && restoreCoverBuffer();

  // Band spans topPadding..homeTopPadding: the cover tile starts at the fixed
  // homeTopPadding, so the height must shrink by topPadding or the band (and a
  // centered title, e.g. RoundedRaff's book title) sinks into the tile.
  // Home is the stack root: no back button in its header.
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.homeTopPadding - metrics.topPadding},
                 metrics.homeContinueReadingInMenu && !recentBooks.empty() ? recentBooks[0].title.c_str() : nullptr,
                 nullptr, false);

  // Record the tile rect so storeCoverBuffer (called from the theme) knows
  // which sub-region of the framebuffer to snapshot. ~16 KB in Portrait
  // instead of the 48 KB full framebuffer the previous bind captured.
  coverRectX = 0;
  coverRectY = metrics.homeTopPadding;
  coverRectW = pageWidth;
  coverRectH = metrics.homeCoverTileHeight;

  const float recentProgressPercent =
      (!recentBooks.empty() && selectorIndex >= 0 && selectorIndex < static_cast<int>(recentBooks.size()))
          ? EpubReaderUtils::recentBookProgressPercent(recentBooks[selectorIndex].path)
          : -1.0f;
  GUI.drawRecentBookCover(renderer, Rect{0, metrics.homeTopPadding, pageWidth, metrics.homeCoverTileHeight},
                          recentBooks, selectorIndex, coverRendered, coverBufferStored, bufferRestored,
                          std::bind(&HomeActivity::storeCoverBuffer, this), recentProgressPercent);

  // Build menu items dynamically
  std::vector<const char*> menuItems = {tr(STR_BROWSE_FILES), tr(STR_LIBRARY), tr(STR_FILE_TRANSFER),
                                        tr(STR_SETTINGS_TITLE)};
  std::vector<UIIcon> menuIcons = {Folder, Library, Transfer, Settings};

  if (hasOpdsServers) {
    menuItems.insert(menuItems.begin() + 2, tr(STR_OPDS_BROWSER));
    menuIcons.insert(menuIcons.begin() + 2, Blocks);
  }

  if (metrics.homeContinueReadingInMenu && !recentBooks.empty()) {
    // Insert Continue Reading at the top if enabled in theme
    menuItems.insert(menuItems.begin(), tr(STR_CONTINUE_READING));
    menuIcons.insert(menuIcons.begin(), Book);
  }

  GUI.drawButtonMenu(
      renderer,
      Rect{0, metrics.homeTopPadding + metrics.homeCoverTileHeight + metrics.homeMenuTopOffset, pageWidth,
           pageHeight - (metrics.headerHeight + metrics.homeTopPadding + metrics.verticalSpacing +
                         metrics.homeMenuTopOffset + metrics.buttonHintsHeight)},
      static_cast<int>(menuItems.size()),
      metrics.homeContinueReadingInMenu ? selectorIndex : selectorIndex - recentBooks.size(),
      [&menuItems](int index) { return std::string(menuItems[index]); },
      [&menuIcons](int index) { return menuIcons[index]; });

  // See frontLeftRightAreHorizontal()'s comment: on X4C the Left/Right-role buttons hinted here are
  // a genuine horizontal pair (distinct from Lyra Carousel's own separate Up/Down level-switch
  // buttons), not the vertically-oriented page-turn keys "Up"/"Down" text describes on X3/X4(Pro).
  const bool horizontalHint = mappedInput.frontLeftRightAreHorizontal();
  const auto labels =
      mappedInput.mapLabels(recentBooks.empty() ? "" : tr(STR_RESUME), tr(STR_SELECT),
                            horizontalHint ? tr(STR_DIR_LEFT) : tr(STR_DIR_UP),
                            horizontalHint ? tr(STR_DIR_RIGHT) : tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  // Lyra Carousel only: redraw the header as the LAST thing before the frame is pushed, so it's
  // guaranteed correct in what actually reaches the panel regardless of what the tile draw (or its
  // store/restore cover-buffer snapshot, a raw region memcpy bypassing normal drawing) did earlier
  // in this same pass. Reported on X4C: after scrolling to a 2-line-wrapped title, a blank bar
  // covering part of the battery percentage appears at the top and persists across every
  // subsequent cover (even 1-line ones) until a full refresh. The header IS already redrawn once
  // above, before the tile -- this is a second, deliberately redundant pass specifically to rule
  // out anything the tile's own drawing does afterward from being the last word on those rows.
  // Scoped to Carousel only so every other theme's already-correct rendering is untouched.
  if (static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme) == CrossPointSettings::UI_THEME::LYRA_CAROUSEL) {
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.homeTopPadding - metrics.topPadding},
                   metrics.homeContinueReadingInMenu && !recentBooks.empty() ? recentBooks[0].title.c_str() : nullptr);
  }

  renderer.displayBuffer(cleanInitialRefresh && !firstRenderDone ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);

  if (!firstRenderDone) {
    firstRenderDone = true;
    requestUpdate();
  } else if (!recentsLoaded && !recentsLoading) {
    recentsLoading = true;
    const int themeThumbHeight = GUI.homeCoverThumbHeight(renderer);
    loadRecentCovers(themeThumbHeight > 0 ? themeThumbHeight : metrics.homeCoverHeight);
  }
}

void HomeActivity::onSelectBook(const std::string& path) { activityManager.goToReader(path); }

void HomeActivity::onFileBrowserOpen() {
  // Grouping needs every book's series known before the first page can render correctly, and the
  // same rebuild also pre-renders any missing cover thumbnails (see LibraryIndexRebuildActivity /
  // LibraryIndexBuilder's coverBackfill) -- so Covers always routes through an index check first,
  // even with SETTINGS.groupBySeries off, or covers for never-opened books would only ever appear
  // lazily as their page happens to be scrolled to. Near-instant if nothing changed since the last
  // build, a cancellable rebuild otherwise. Titles only needs the index for grouping (it never
  // shows covers), so it stays gated behind groupBySeries. The stock file browser never groups
  // and is unaffected.
  switch (SETTINGS.fileBrowserView) {
    case CrossPointSettings::FILE_BROWSER_COVERS:
      activityManager.goToLibraryIndexRebuild([] { activityManager.goToCoverGridBrowser(); });
      break;
    case CrossPointSettings::FILE_BROWSER_TITLES:
      if (SETTINGS.groupBySeries) {
        activityManager.goToLibraryIndexRebuild([] { activityManager.goToLibraryList(); });
      } else {
        activityManager.goToLibraryList();
      }
      break;
    default:
      activityManager.goToFileBrowser();
      break;
  }
}

void HomeActivity::onLibraryOpen() {
  // CrossFade: the Library entry can show either upstream's tabbed list or CrossFade's
  // recent-books cover grid (Settings > Display > Library View).
  if (SETTINGS.recentBooksView == CrossPointSettings::RECENT_BOOKS_COVERS) {
    activityManager.goToCoverGridRecentBooks();
  } else {
    activityManager.goToLibrary();
  }
}

void HomeActivity::onSettingsOpen() { activityManager.goToSettings(); }

void HomeActivity::onFileTransferOpen() { activityManager.goToFileTransfer(); }

void HomeActivity::onOpdsBrowserOpen() { activityManager.goToBrowser(); }
