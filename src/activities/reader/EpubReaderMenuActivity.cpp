#include "EpubReaderMenuActivity.h"

#include <GfxRenderer.h>
#include <HalFrontlight.h>
#include <I18n.h>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "ReaderUtils.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

namespace {
constexpr StrId TAB_NAME_IDS[] = {StrId::STR_READER_TAB_MAIN, StrId::STR_BOOKMARKS, StrId::STR_TEXT_SETTINGS};
}  // namespace

EpubReaderMenuActivity::EpubReaderMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                               const std::string& title, const int currentPage, const int totalPages,
                                               const int bookProgressPercent, const uint8_t currentOrientation,
                                               const bool hasFootnotes, const bool hasBookmarks, const bool isFinished,
                                               const bool statsEnabled)
    : UiTabListActivity("EpubReaderMenu", renderer, mappedInput),
      menuItems(buildMenuItems(hasFootnotes, hasBookmarks, isFinished, statsEnabled)),
      title(title),
      pendingOrientation(currentOrientation),
      currentPage(currentPage),
      totalPages(totalPages),
      bookProgressPercent(bookProgressPercent) {}

EpubReaderMenuActivity::TabMenuItems EpubReaderMenuActivity::buildMenuItems(const bool hasFootnotes,
                                                                            const bool hasBookmarks,
                                                                            const bool isFinished,
                                                                            const bool statsEnabled) {
  TabMenuItems items;
  auto& mainItems = items[static_cast<size_t>(Tab::Main)];
  auto& bookmarkItems = items[static_cast<size_t>(Tab::Bookmarks)];
  auto& textItems = items[static_cast<size_t>(Tab::Text)];
  mainItems.reserve(16);
  bookmarkItems.reserve(2);

  mainItems.push_back({MenuAction::SELECT_CHAPTER, StrId::STR_SELECT_CHAPTER});
  if (hasFootnotes) {
    mainItems.push_back({MenuAction::FOOTNOTES, StrId::STR_FOOTNOTES});
  }
  mainItems.push_back({MenuAction::NIGHT_MODE, StrId::STR_NIGHT_MODE});
  if (Frontlight.present()) {
    mainItems.push_back({MenuAction::FRONTLIGHT, StrId::STR_FRONTLIGHT});
  }
  mainItems.push_back({MenuAction::DICTIONARY, StrId::STR_LOOKUP});
  mainItems.push_back({MenuAction::ROTATE_SCREEN, StrId::STR_ORIENTATION});
  mainItems.push_back({MenuAction::AUTO_PAGE_TURN, StrId::STR_AUTO_TURN_PAGES_PER_MIN});
  mainItems.push_back({MenuAction::GO_TO_PERCENT, StrId::STR_GO_TO_PERCENT});
  mainItems.push_back({MenuAction::SCREENSHOT, StrId::STR_SCREENSHOT_BUTTON});
  mainItems.push_back({MenuAction::DISPLAY_QR, StrId::STR_DISPLAY_QR});
  mainItems.push_back({MenuAction::GO_HOME, StrId::STR_GO_HOME_BUTTON});
  mainItems.push_back({MenuAction::SYNC, StrId::STR_SYNC_PROGRESS});
  mainItems.push_back({MenuAction::DELETE_CACHE, StrId::STR_DELETE_CACHE});
  mainItems.push_back(
      {MenuAction::TOGGLE_FINISHED, isFinished ? StrId::STR_MARK_UNFINISHED : StrId::STR_MARK_FINISHED});
  // Hidden entirely when reading-stats tracking is off: nothing meaningful to show.
  if (statsEnabled) {
    mainItems.push_back({MenuAction::READING_STATS, StrId::STR_READING_STATS});
  }

  if (hasBookmarks) {
    bookmarkItems.push_back({MenuAction::BOOKMARKS, StrId::STR_BOOKMARKS});
  }
  bookmarkItems.push_back({MenuAction::TOGGLE_BOOKMARK, StrId::STR_TOGGLE_BOOKMARK});

  textItems.push_back({MenuAction::TEXT_SETTINGS, StrId::STR_TEXT_SETTINGS});
  return items;
}

void EpubReaderMenuActivity::buildFlatMenuItems(std::vector<MenuItem>& items, const bool hasFootnotes,
                                                const bool hasBookmarks, const bool isFinished,
                                                const bool statsEnabled) {
  const TabMenuItems tabs = buildMenuItems(hasFootnotes, hasBookmarks, isFinished, statsEnabled);
  items.clear();
  for (const auto& tab : tabs) {
    items.insert(items.end(), tab.begin(), tab.end());
  }
}

void EpubReaderMenuActivity::onEnter() {
  UiTabListActivity::onEnter();
  // Open on the Main tab with its first row focused, matching the flat menu's behaviour (one
  // Confirm reaches the first action); the other tabs remember their own rows.
  for (auto& n : tabNavs) n.selected = 1;
  rebuildRowItems();
}

void EpubReaderMenuActivity::rebuildRowItems() {
  const auto& items = activeMenuItems();
  rowItems_.clear();
  rowValues_.clear();
  rowItems_.reserve(items.size());
  rowValues_.resize(items.size());
  for (size_t i = 0; i < items.size(); i++) {
    fui::ListItem item;
    item.label = I18N.get(items[i].labelId);
    item.actionValue = static_cast<int16_t>(i);
    rowItems_.push_back(item);
  }
}

const char* EpubReaderMenuActivity::tabLabel(const int index) const { return I18N.get(TAB_NAME_IDS[index]); }

void EpubReaderMenuActivity::switchTab(const int direction) {
  const bool onTabBar = ringPos() == 0;
  constexpr int count = static_cast<int>(Tab::Count);
  tab_ = static_cast<Tab>((static_cast<int>(tab_) + direction + count) % count);
  rebuildRowItems();
  auto& n = activeNav();
  if (onTabBar) n.selected = 0;
  if (n.selected > listCount()) n.selected = listCount();
  n.followOnBuild = true;  // pull the new tab's viewport to its remembered selection
  requestUpdate();
}

void EpubReaderMenuActivity::stepTab(const int direction) {
  if (optionPopup.isActive()) return;
  switchTab(direction);
}

void EpubReaderMenuActivity::onTabAction(const int index) {
  if (optionPopup.isActive()) return;
  if (tab_ != static_cast<Tab>(index)) {
    tab_ = static_cast<Tab>(index);
    rebuildRowItems();
    auto& n = activeNav();
    n.selected = 0;  // tab taps land with the tab bar focused
    n.followOnBuild = true;
    requestUpdate();
  }
  app.clearTapFlash();
}

void EpubReaderMenuActivity::closeCancelled() {
  ActivityResult result;
  result.isCancelled = true;
  result.data = MenuResult{-1, pendingOrientation, selectedPageTurnOption};
  setResult(std::move(result));
  finish();
}

bool EpubReaderMenuActivity::handleHomeGesture() {
  closeCancelled();
  return true;
}

void EpubReaderMenuActivity::activateIndex(const int index) {
  if (optionPopup.isActive()) return;
  const auto& items = activeMenuItems();
  if (index < 0 || index >= static_cast<int>(items.size())) return;
  // The activated row leaves this screen (popup or finish); a lingering flash
  // would gray an unrelated element on the next render.
  app.clearTapFlash();
  activeNav().selected = index + 1;

  const auto selectedAction = items[index].action;
  if (selectedAction == MenuAction::ROTATE_SCREEN) {
    optionPopup.show(StrId::STR_ORIENTATION, orientationLabels.data(), static_cast<int>(orientationLabels.size()),
                     pendingOrientation, [this](int idx) {
                       pendingOrientation = idx;
                       // Rotate the menu immediately. Only the renderer turns;
                       // SETTINGS.orientation stays unchanged so the reader's
                       // result handler still detects the change and reflows.
                       ReaderUtils::applyOrientation(renderer, pendingOrientation);
                       app.setDevice(uiTarget.deviceContext());  // hit rects follow the new frame
                       requestUpdate(true);
                     });
    requestUpdate();
    return;
  }

  if (selectedAction == MenuAction::AUTO_PAGE_TURN) {
    optionPopup.show(I18N.get(StrId::STR_AUTO_TURN_PAGES_PER_MIN), pageTurnLabels.data(),
                     static_cast<int>(pageTurnLabels.size()), selectedPageTurnOption, [this](int idx) {
                       selectedPageTurnOption = idx;
                       requestUpdate();
                     });
    requestUpdate();
    return;
  }

  if (selectedAction == MenuAction::NIGHT_MODE) {
    SETTINGS.screenInverted = SETTINGS.screenInverted == 0 ? 1 : 0;
    SETTINGS.saveToFile();
    requestUpdate();
    return;
  }

  if (selectedAction == MenuAction::FRONTLIGHT) {
    const bool lightOn = !Frontlight.isOn();
    Frontlight.setOn(lightOn);
    SETTINGS.frontlightOn = lightOn ? 1 : 0;
    SETTINGS.saveToFile();
    requestUpdate();
    return;
  }

  setResult(MenuResult{static_cast<int>(selectedAction), pendingOrientation, selectedPageTurnOption});
  finish();
}

bool EpubReaderMenuActivity::handleCustomInput() {
  return optionPopup.handleInput(mappedInput, [this] { requestUpdate(); });
}

bool EpubReaderMenuActivity::handleButtons() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    closeCancelled();
    return true;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (ringPos() == 0) {
      switchTab(1);
    } else {
      activateIndex(ringPos() - 1);
    }
    return true;
  }

  return false;
}

void EpubReaderMenuActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  // Content: the safe area minus the header band GUI.drawHeader paints.
  screen.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
      static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height)), static_cast<int16_t>(safe.x)});

  // Progress summary where the old sub-header band sat.
  std::string progressLine;
  if (totalPages > 0) {
    progressLine = std::string(tr(STR_CHAPTER_PREFIX)) + std::to_string(currentPage) + "/" +
                   std::to_string(totalPages) + std::string(tr(STR_PAGES_SEPARATOR));
  }
  progressLine += std::string(tr(STR_BOOK_PREFIX)) + std::to_string(bookProgressPercent) + "%";
  const fui::Rect band = screen.takeTop(static_cast<int16_t>(metrics.tabBarHeight));
  const int16_t pad = screen.theme().headerSidePadding;
  screen.target().text(band.inset(fui::Insets{0, pad, 0, pad}), progressLine.c_str(), screen.theme().smallText);

  buildTabBar(screen);
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  // rowItems_'s labels/actionValue were set by rebuildRowItems(); only rows with live values need
  // refreshing here.
  const auto& items = activeMenuItems();
  for (size_t i = 0; i < items.size() && i < rowItems_.size(); i++) {
    const auto action = items[i].action;
    if (action == MenuAction::ROTATE_SCREEN) {
      rowValues_[i] = I18N.get(orientationLabels[pendingOrientation]);
    } else if (action == MenuAction::AUTO_PAGE_TURN) {
      rowValues_[i] = pageTurnLabels[selectedPageTurnOption];
    } else if (action == MenuAction::NIGHT_MODE) {
      rowValues_[i] = I18N.get(SETTINGS.screenInverted ? StrId::STR_STATE_ON : StrId::STR_STATE_OFF);
    } else if (action == MenuAction::FRONTLIGHT) {
      rowValues_[i] = I18N.get(Frontlight.isOn() ? StrId::STR_STATE_ON : StrId::STR_STATE_OFF);
    } else {
      rowValues_[i].clear();
    }
    rowItems_[i].value = rowValues_[i].empty() ? nullptr : rowValues_[i].c_str();
  }

  fui::ListProps props;
  props.items = rowItems_.data();
  props.count = static_cast<uint16_t>(rowItems_.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.valueInset = 8;               // air between the value and the row edge
  // Label at the value's font size: both sides of the row read as one unit.
  // maxLines=2 also marks the style caller-owned (see textStyleUnset).
  props.labelText = screen.theme().smallText;
  props.labelText.maxLines = 2;
  syncTabListViewport(screen, props);
  screen.list(props);
}

void EpubReaderMenuActivity::drawChrome() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);

  // Header via GUI.drawHeader (already FreeInkUI-themed) for the battery
  // indicator; the rest of the screen renders through the app.
  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                 title.c_str());
}

void EpubReaderMenuActivity::drawFooter() {
  const char* confirmLabel = ringPos() == 0
                                 ? I18N.get(TAB_NAME_IDS[(static_cast<int>(tab_) + 1) % static_cast<int>(Tab::Count)])
                                 : tr(STR_SELECT);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void EpubReaderMenuActivity::render(RenderLock&&) {
  if (optionPopup.processRender(renderer, mappedInput)) return;

  renderer.clearScreen();
  drawChrome();

  renderUi();

  drawFooter();
  renderer.displayBuffer();
}
