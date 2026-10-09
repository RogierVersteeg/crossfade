#include "KOReaderAutoSync.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <KOReaderCredentialStore.h>
#include <KOReaderDocumentId.h>
#include <KOReaderSyncClient.h>
#include <Logging.h>
#include <ProgressComparison.h>
#include <ProgressMapper.h>
#include <WiFi.h>

#include <memory>
#include <optional>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "WifiCredentialStore.h"
#include "activities/reader/EpubReaderUtils.h"

namespace KOReaderAutoSync {

namespace {

// Direct connect to the last-known network, no scan -- a scan alone can cost several seconds,
// which neither a sleep transition nor a book-open can afford. Bounded and abortable.
constexpr unsigned long WIFI_CONNECT_TIMEOUT_MS = 3500;

bool userRequestedAbort() {
  gpio.update();
  return gpio.wasAnyPressed();
}

bool gatesOpen() {
  if (!KOREADER_STORE.getAutoSyncEnabled()) return false;
  if (WIFI_STORE.getCredentialCount() == 0 || !KOREADER_STORE.hasCredentials()) return false;
  return true;
}

// Returns true once actually connected. Callers only reach here after confirming a saved
// credential exists, so false always means "attempted and failed/timed out/aborted".
bool connectToSavedWifi() {
  std::optional<WifiCredential> cred;
  const std::string lastSsid = WIFI_STORE.getLastConnectedSsid();
  if (!lastSsid.empty()) cred = WIFI_STORE.findCredential(lastSsid);
  if (!cred) cred = WIFI_STORE.getCredentialAt(0);
  if (!cred) return false;

  WiFi.mode(WIFI_STA);
  if (cred->password.empty()) {
    WiFi.begin(cred->ssid.c_str());
  } else {
    WiFi.begin(cred->ssid.c_str(), cred->password.c_str());
  }

  const unsigned long deadline = millis() + WIFI_CONNECT_TIMEOUT_MS;
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() >= deadline || userRequestedAbort()) {
      LOG_DBG("KOAuto", "WiFi connect timed out or aborted");
      return false;
    }
    delay(50);
  }
  return true;
}

void disconnectWifi() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

std::string computeDocumentHash(const std::string& path) {
  return KOREADER_STORE.getMatchMethod() == DocumentMatchMethod::FILENAME
             ? KOReaderDocumentId::calculateFromFilename(path)
             : KOReaderDocumentId::calculate(path);
}

// Local position straight from progress.bin (spine/page/count, plus the 10-byte format's
// visibleTextOffset when present) -- the same file the reader writes and reads.
bool loadLocalPosition(const std::string& cachePath, CrossPointPosition& pos) {
  if (!EpubReaderUtils::loadProgress(cachePath, pos.spineIndex, pos.pageNumber, pos.totalPages)) return false;
  HalFile f;
  if (Storage.openFileForRead("KOAuto", cachePath + "/progress.bin", f)) {
    uint8_t data[10];
    const int n = f.read(data, sizeof(data));
    f.close();
    if (n == 10) {
      pos.visibleTextOffset = static_cast<uint32_t>(data[6]) | (static_cast<uint32_t>(data[7]) << 8) |
                              (static_cast<uint32_t>(data[8]) << 16) | (static_cast<uint32_t>(data[9]) << 24);
      pos.hasVisibleTextOffset = true;
    }
  }
  return true;
}

// Mirrors KOReaderSyncActivity's mapRemoteProgress: xpath/percentage mapping first, then the
// CrossPoint sync server's rich position when it adds precision.
CrossPointPosition mapRemote(const std::shared_ptr<Epub>& epub, const KOReaderProgress& remote,
                             const CrossPointPosition& local, GfxRenderer& renderer) {
  const SavedProgressPosition koPos{remote.progress, remote.percentage};
  CrossPointPosition mapped = ProgressMapper::toCrossPoint(epub, koPos, renderer, local.spineIndex, local.totalPages);
  if (!mapped.hasVisibleTextOffset && remote.position.has_value()) {
    const bool sameXPath = remote.position->xpath == remote.progress;
    if (const auto rich = ProgressMapper::fromRichPosition(epub, *remote.position, renderer, sameXPath)) {
      mapped = *rich;
    }
  }
  return mapped;
}

// Same payload the manual upload (KOReaderSyncActivity::performUpload) sends, minus metadata.
KOReaderProgress buildUpload(const std::string& hash, const CrossPointPosition& local,
                             const SavedProgressPosition& localKo) {
  KOReaderProgress progress;
  progress.document = hash;
  progress.progress = localKo.xpath;
  progress.percentage = localKo.percentage;
  progress.device = SETTINGS.getEffectiveDeviceName();
  if (KOREADER_STORE.usesCrossPointSyncServer()) {
    KOReaderRichPosition pos;
    const float pct = localKo.percentage < 0.0f ? 0.0f : localKo.percentage > 1.0f ? 1.0f : localKo.percentage;
    pos.pctQ = static_cast<uint32_t>(pct * 1000000.0f + 0.5f);
    pos.spineIndex = static_cast<uint16_t>(local.spineIndex);
    pos.pageNumber = static_cast<uint16_t>(local.pageNumber);
    pos.totalPages = static_cast<uint16_t>(local.totalPages > 0 ? local.totalPages : 1);
    if (local.hasParagraphIndex) pos.paragraphIndex = local.paragraphIndex;
    pos.xpath = localKo.xpath;
    progress.position = std::move(pos);
  }
  return progress;
}

}  // namespace

void pushOnSleep(GfxRenderer& renderer) {
  if (!APP_STATE.lastSleepFromReader) return;
  const std::string& epubPath = APP_STATE.openEpubPath;
  if (epubPath.empty() || !FsHelpers::hasEpubExtension(epubPath)) return;
  if (!gatesOpen()) return;

  const std::string hash = computeDocumentHash(epubPath);
  if (hash.empty()) return;

  // The book was just being read, so its book.bin cache exists: buildIfMissing=false on purpose --
  // if it unexpectedly can't load, guessing a position is worse than skipping the push.
  auto epub = std::make_shared<Epub>(epubPath, "/.crosspoint");
  if (!epub->load(/*buildIfMissing=*/false, /*skipLoadingCss=*/true)) {
    LOG_DBG("KOAuto", "Push: could not load epub from cache, skipping");
    return;
  }
  CrossPointPosition local{};
  if (!loadLocalPosition(epub->getCachePath(), local)) return;

  if (!connectToSavedWifi()) {
    disconnectWifi();
    return;
  }

  KOReaderProgress remote;
  const auto getResult = KOReaderSyncClient::getProgress(hash, remote, userRequestedAbort);
  if (getResult != KOReaderSyncClient::OK && getResult != KOReaderSyncClient::NOT_FOUND) {
    LOG_DBG("KOAuto", "Push: remote fetch failed (%d), giving up", getResult);
    disconnectWifi();
    return;
  }

  SavedProgressPosition localKo;
  bool localAhead = getResult == KOReaderSyncClient::NOT_FOUND;
  {
    // The sleep frame is already on the panel, so the framebuffer can be lent to the mappers
    // (they need the headroom for chapter parsing), exactly as the reader does before manual sync.
    GfxRenderer::FrameBufferLoan loan(renderer);
    localKo = ProgressMapper::toSavedProgress(epub, local);
    if (getResult == KOReaderSyncClient::OK) {
      const CrossPointPosition remotePos = mapRemote(epub, remote, local, renderer);
      localAhead =
          compareProgress(local, localKo.percentage, remotePos, remote.percentage) == ProgressComparison::LocalAhead;
    }
  }
  if (!localAhead) {
    LOG_DBG("KOAuto", "Push: remote (%.4f) is not behind local (%.4f), nothing to do", remote.percentage,
            localKo.percentage);
    disconnectWifi();
    return;
  }

  const KOReaderProgress toPush = buildUpload(hash, local, localKo);
  epub.reset();
  const auto putResult = KOReaderSyncClient::updateProgress(toPush, userRequestedAbort);
  LOG_DBG("KOAuto", "Push result: %d (local=%.4f)", putResult, localKo.percentage);
  disconnectWifi();
}

void pullFurthestOnOpen(const std::string& epubPath, GfxRenderer& renderer) {
  if (epubPath.empty() || !FsHelpers::hasEpubExtension(epubPath)) return;
  if (!gatesOpen()) return;

  const std::string hash = computeDocumentHash(epubPath);
  if (hash.empty()) return;

  if (!connectToSavedWifi()) {
    disconnectWifi();
    return;
  }

  KOReaderProgress remote;
  const auto getResult = KOReaderSyncClient::getProgress(hash, remote, userRequestedAbort);
  disconnectWifi();  // one request either way; done with the radio regardless of outcome
  if (getResult != KOReaderSyncClient::OK) return;

  // A book never opened on this device has no book.bin yet; building it here is the only way the
  // mappers get spine sizes. Deferred until after the network round trip so the common "no remote
  // progress" case never pays for it.
  auto epub = std::make_shared<Epub>(epubPath, "/.crosspoint");
  epub->setupCacheDir();
  if (!epub->load(/*buildIfMissing=*/true, /*skipLoadingCss=*/true)) {
    LOG_DBG("KOAuto", "Pull: could not load epub, skipping");
    return;
  }

  CrossPointPosition local{};
  const bool hadLocal = loadLocalPosition(epub->getCachePath(), local);

  CrossPointPosition remotePos;
  bool remoteAhead = false;
  {
    GfxRenderer::FrameBufferLoan loan(renderer);
    remotePos = mapRemote(epub, remote, local, renderer);
    if (hadLocal) {
      const SavedProgressPosition localKo = ProgressMapper::toSavedProgress(epub, local);
      remoteAhead =
          compareProgress(local, localKo.percentage, remotePos, remote.percentage) == ProgressComparison::RemoteAhead;
    } else {
      remoteAhead = remote.percentage > 0.0f;
    }
  }
  if (!remoteAhead) {
    LOG_DBG("KOAuto", "Pull: local is not behind remote (%.4f), leaving local position", remote.percentage);
    return;
  }

  std::optional<uint32_t> offset;
  if (remotePos.hasVisibleTextOffset) offset = remotePos.visibleTextOffset;
  if (!EpubReaderUtils::saveProgress(*epub, remotePos.spineIndex, remotePos.pageNumber, 0, offset)) {
    LOG_ERR("KOAuto", "Pull: found further remote progress but failed to save it locally");
    return;
  }
  LOG_DBG("KOAuto", "Pull: applied remote position spine=%d page=%d (remote %.4f)", remotePos.spineIndex,
          remotePos.pageNumber, remote.percentage);
}

}  // namespace KOReaderAutoSync
