#pragma once

#include <string>

#include "activities/Activity.h"
#include "activities/ScreenTransitionRefresh.h"

/**
 * Downloads new Substack posts from the x4 server into /Substack.
 *
 * Constructed twice: first as a handoff that reboots into network mode, letting the
 * current activity (e.g. the reader) save its state on exit; then after the reboot to
 * join Wi-Fi, list posts newer than the last one downloaded, and fetch each EPUB.
 *
 * Reads the server from /substack.json: {"url": "...", "username": "...", "password": "..."}.
 * Remembers the newest downloaded post id in /.substack-cursor.
 */
class SubstackFetchActivity final : public Activity {
 public:
  static constexpr const char* NAME = "SubstackFetch";

  enum class Stage : uint8_t { Handoff, Network };

  explicit SubstackFetchActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const Stage stage)
      : Activity(NAME, renderer, mappedInput), stage(stage) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == CONNECTING || state == FETCHING; }

 private:
  enum State { CONNECTING, FETCHING, DONE, FAILED };

  void onWifiSelectionComplete(bool success);
  bool fetchNewPosts();

  Stage stage;
  State state = CONNECTING;
  std::string serverUrl;
  std::string username;
  std::string password;
  std::string statusMessage;
  int downloadedCount = 0;
  bool wifiActivated = false;
  unsigned long autoReturnAt = 0;
  ScreenTransitionRefresh screenTransitionRefresh;
};
