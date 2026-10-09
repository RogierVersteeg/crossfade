#pragma once
#include <string>

class GfxRenderer;

// CrossFade: automatic, silent KOSync push-on-sleep / pull-on-open. Reuses the same client code
// the manual "Sync Progress" reader-menu action does (KOReaderSyncClient, KOReaderDocumentId,
// ProgressMapper, compareProgress, KOReaderCredentialStore) -- headless callers of the existing
// sync, not a second implementation. Both entry points:
//  - No-op (no radio, no delay) unless auto-sync is enabled in KOReader settings and a saved WiFi
//    network plus KOReader credentials exist.
//  - Use a short, direct connect (last-connected SSID, no scan) bounded by a few seconds.
//  - Are abortable mid-connect/mid-request via a button press.
//  - Never surface UI and treat every failure (no WiFi, auth, network, server) as "give up
//    silently, proceed as if sync wasn't attempted."
//  - Decide furthest-wins with the same compareProgress() the manual smart sync uses.
namespace KOReaderAutoSync {

// Call from main.cpp's enterDeepSleep() after the sleep screen is painted and before its WiFi
// teardown. Pushes APP_STATE.openEpubPath's local progress if it's ahead of the server's; no-ops
// unless APP_STATE.lastSleepFromReader is true.
void pushOnSleep(GfxRenderer& renderer);

// Call when a book is freshly opened from a browsing screen (Home/Covers/Titles/Library) -- NOT
// on boot-resume or quick-resume. Pulls the server's progress for epubPath and, if it's further
// than local, writes it into progress.bin before the reader is constructed, so the reader's own
// EpubReaderUtils::loadProgress() picks it up. EPUB files only.
void pullFurthestOnOpen(const std::string& epubPath, GfxRenderer& renderer);

}  // namespace KOReaderAutoSync
