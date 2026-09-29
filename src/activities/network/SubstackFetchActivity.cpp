#include "SubstackFetchActivity.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "SilentRestart.h"
#include "activities/ActivityManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "network/WifiUtils.h"

namespace {
constexpr const char* CONFIG_PATH = "/substack.json";
constexpr const char* CURSOR_PATH = "/.substack-cursor";
constexpr const char* POSTS_DIR = "/Substack";
constexpr const char* PART_PATH = "/Substack/.download.part";
constexpr unsigned long AUTO_RETURN_DELAY_MS = 2500;
}  // namespace

void SubstackFetchActivity::onEnter() {
  Activity::onEnter();

  if (stage == Stage::Handoff) {
    silentRestartToNetwork(NetworkBootTarget::SUBSTACK_FETCH);
    return;
  }

  JsonDocument config;
  if (!Storage.exists(CONFIG_PATH) || deserializeJson(config, Storage.readFile(CONFIG_PATH).c_str())) {
    state = FAILED;
    statusMessage = "Add /substack.json to the SD card";
    requestUpdate();
    return;
  }
  serverUrl = config["url"] | "";
  username = config["username"] | "";
  password = config["password"] | "";
  while (!serverUrl.empty() && serverUrl.back() == '/') serverUrl.pop_back();
  if (serverUrl.empty()) {
    state = FAILED;
    statusMessage = "/substack.json has no url";
    requestUpdate();
    return;
  }

  sdFontSystem.releaseForNetwork(renderer);
  wifiActivated = true;
  if (hasActiveStationWifiConnection()) {
    onWifiSelectionComplete(true);
    return;
  }
  WiFi.mode(WIFI_STA);
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void SubstackFetchActivity::onExit() {
  Activity::onExit();
  if (wifiActivated) {
    WiFi.disconnect(false);
    delay(100);
    WiFi.mode(WIFI_OFF);
    delay(100);
    silentRestartToReader(true);
  }
}

void SubstackFetchActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    RenderLock lock(*this);
    state = FAILED;
    statusMessage = "Couldn't join Wi-Fi";
    return;
  }

  // Wi-Fi power saving makes downloads crawl (CrossInk issue #160)
  WiFi.setSleep(false);
  {
    RenderLock lock(*this);
    state = FETCHING;
    statusMessage = "Checking for new posts...";
  }
  requestUpdateAndWait();

  const bool fetched = fetchNewPosts();
  {
    RenderLock lock(*this);
    state = fetched ? DONE : FAILED;
    if (fetched) {
      statusMessage = downloadedCount == 0   ? "No new posts"
                      : downloadedCount == 1 ? "1 new post"
                                             : std::to_string(downloadedCount) + " new posts";
      autoReturnAt = millis() + AUTO_RETURN_DELAY_MS;
    }
  }
  requestUpdate(true);
}

bool SubstackFetchActivity::fetchNewPosts() {
  Storage.mkdir(POSTS_DIR);
  int cursor = Storage.exists(CURSOR_PATH) ? Storage.readFile(CURSOR_PATH).toInt() : 0;

  bool more = true;
  while (more) {
    std::string listing;
    if (!HttpDownloader::fetchUrl(serverUrl + "/substack/posts?after=" + std::to_string(cursor), listing, username,
                                  password)) {
      statusMessage = "Couldn't reach the server";
      return false;
    }
    JsonDocument page;
    if (deserializeJson(page, listing)) {
      statusMessage = "Server sent an unreadable post list";
      return false;
    }
    more = page["more"] | false;

    for (JsonObject post : page["posts"].as<JsonArray>()) {
      const int id = post["id"] | 0;
      const std::string filename = post["filename"] | "";
      // Server filenames are sanitised; refuse anything that could escape the folder
      if (id <= cursor || filename.empty() || filename.find('/') != std::string::npos ||
          filename.find("..") != std::string::npos) {
        continue;
      }
      const std::string finalPath = std::string(POSTS_DIR) + "/" + filename;
      if (!Storage.exists(finalPath.c_str())) {
        {
          RenderLock lock(*this);
          statusMessage = "Downloading " + std::to_string(downloadedCount + 1) + ": " + (post["title"] | "");
        }
        requestUpdateAndWait();
        Storage.remove(PART_PATH);
        const auto result = HttpDownloader::downloadToFile(
            serverUrl + "/substack/posts/" + std::to_string(id) + ".epub", PART_PATH, nullptr, nullptr, username,
            password);
        if (result != HttpDownloader::OK || !Storage.rename(PART_PATH, finalPath.c_str())) {
          LOG_ERR("SUBSTACK", "Download of post %d failed (error=%d)", id, result);
          Storage.remove(PART_PATH);
          statusMessage = "Download failed";
          return false;
        }
        downloadedCount++;
      }
      // Persist per post so an interrupted fetch resumes where it stopped
      cursor = id;
      Storage.writeFile(CURSOR_PATH, String(cursor));
    }
  }
  return true;
}

void SubstackFetchActivity::loop() {
  if (state != DONE && state != FAILED) return;
  if ((autoReturnAt != 0 && millis() >= autoReturnAt) ||
      mappedInput.wasReleased(MappedInputManager::Button::Back) ||
      mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activityManager.goHome();
  }
}

void SubstackFetchActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                 tr(STR_FETCH_SUBSTACK));

  const int top = screen.y + screen.height / 2 - 40;
  const char* message = state == CONNECTING ? "Connecting to Wi-Fi..." : statusMessage.c_str();
  UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top, message, true, EpdFontFamily::BOLD);

  if (state == DONE || state == FAILED) {
    const auto labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
  }
  renderer.displayBuffer(screenTransitionRefresh.modeFor(static_cast<uint8_t>(state)));
}
