#pragma once
#include "activities/Activity.h"
#include "components/OptionPopup.h"

// CrossFade: only entered when OPDS servers are configured -- HomeActivity's Transfer & Sync row
// goes straight to File Transfer otherwise (see HomeActivity::onTransferAndSyncOpen), so this
// picker never needs a "no servers" state of its own. A thin Activity wrapping a single
// OptionPopup, entered via replaceActivity like the other Home destinations, so a dismissal
// returns Home instead of finishing with a result.
class TransferAndSyncActivity final : public Activity {
 public:
  TransferAndSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  OptionPopup optionPopup;

  void selectAction(int index);
};
