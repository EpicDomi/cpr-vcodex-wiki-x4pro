#pragma once

#include <memory>

#include "WikiPicture.h"
#include "WikiSession.h"
#include "activities/Activity.h"

// One picture full screen: fitted to the screen, or enlarged (to its stored
// resolution, at least twice the fitted size) and moved a half screen at a
// time. A tap or Select switches between the two.
class WikiPictureActivity final : public Activity {
 public:
  WikiPictureActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::shared_ptr<WikiSession> session,
                      uint32_t image);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  void layout();
  void decode();

  std::shared_ptr<WikiSession> session_;
  uint32_t image_;
  int srcW_ = 0, srcH_ = 0;
  bool zoomed_ = false;
  int scaledW_ = 0, scaledH_ = 0;  // the picture at the current zoom
  int viewX_ = 0, viewY_ = 0;      // its top left part shown
  int areaX_ = 0, areaY_ = 0, areaW_ = 0, areaH_ = 0;
  wiki::Pixmap pixmap_;  // what is on screen, when memory allows
  bool direct_ = false;  // drawn straight to the screen in black and white
  bool failed_ = false;
};
