#pragma once
#include <Epub.h>
#include <I18n.h>

#include <array>
#include <string>
#include <vector>

#include "activities/UiTabListActivity.h"
#include "components/OptionPopup.h"

// CrossFade: the reader menu is a tabbed list (Main / Bookmarks / Text) on upstream's
// UiTabListActivity, the same tab-bar-plus-list base Settings and Text Settings use. Tabs switch
// with Confirm on the tab bar, a continuous hold of the navigation buttons, or a tap on a pill.
class EpubReaderMenuActivity final : public UiTabListActivity {
 public:
  // Menu actions available from the reader menu.
  enum class MenuAction {
    SELECT_CHAPTER,
    FOOTNOTES,
    TEXT_SETTINGS,
    NIGHT_MODE,
    FRONTLIGHT,
    GO_TO_PERCENT,
    AUTO_PAGE_TURN,
    ROTATE_SCREEN,
    BOOKMARKS,
    TOGGLE_BOOKMARK,
    SCREENSHOT,
    DISPLAY_QR,
    GO_HOME,
    SYNC,
    DELETE_CACHE,
    DICTIONARY,
    TOGGLE_FINISHED,
    READING_STATS
  };

  struct MenuItem {
    MenuAction action;
    StrId labelId;
  };

  enum class Tab : uint8_t { Main = 0, Bookmarks = 1, Text = 2, Count = 3 };
  using TabMenuItems = std::array<std::vector<MenuItem>, static_cast<size_t>(Tab::Count)>;

  static TabMenuItems buildMenuItems(bool hasFootnotes, bool hasBookmarks, bool isFinished, bool statsEnabled);
  // All tabs flattened into one list, for the touch toolbar's More panel.
  static void buildFlatMenuItems(std::vector<MenuItem>& items, bool hasFootnotes, bool hasBookmarks, bool isFinished,
                                 bool statsEnabled);

  explicit EpubReaderMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& title,
                                  int currentPage, int totalPages, int bookProgressPercent, uint8_t currentOrientation,
                                  bool hasFootnotes, bool hasBookmarks, bool isFinished = false,
                                  bool statsEnabled = false);

  void onEnter() override;
  void render(RenderLock&&) override;
  bool handleHomeGesture() override;

 private:
  // UiTabListActivity contract
  int tabCount() const override { return static_cast<int>(Tab::Count); }
  int activeTab() const override { return static_cast<int>(tab_); }
  const char* tabLabel(int index) const override;
  void onTabAction(int index) override;
  void stepTab(int direction) override;
  bool handleButtons() override;
  // UiListActivity contract
  int listCount() const override { return static_cast<int>(activeMenuItems().size()); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  // Popup input runs before any button or touch handling.
  bool handleCustomInput() override;
  // Header via GUI.drawHeader inside the safe area for the battery indicator.
  void drawChrome() override;
  void drawFooter() override;

  const std::vector<MenuItem>& activeMenuItems() const { return menuItems[static_cast<size_t>(tab_)]; }
  void rebuildRowItems();
  void switchTab(int direction);
  void closeCancelled();

  const TabMenuItems menuItems;
  Tab tab_ = Tab::Main;
  // Row storage for the active tab: rowItems_ (label/actionValue) is rebuilt on every tab switch;
  // rowValues_ backs the live value text of rows that have one.
  std::vector<std::string> rowValues_;
  std::vector<freeink::ui::ListItem> rowItems_;

  OptionPopup optionPopup;
  std::string title = "Reader Menu";
  uint8_t pendingOrientation = 0;
  uint8_t selectedPageTurnOption = 0;
  const std::vector<StrId> orientationLabels = {StrId::STR_PORTRAIT, StrId::STR_LANDSCAPE_CW, StrId::STR_INVERTED,
                                                StrId::STR_LANDSCAPE_CCW};
  const std::vector<const char*> pageTurnLabels = {I18N.get(StrId::STR_STATE_OFF), "1", "3", "6", "12"};
  int currentPage = 0;
  int totalPages = 0;
  int bookProgressPercent = 0;
};
