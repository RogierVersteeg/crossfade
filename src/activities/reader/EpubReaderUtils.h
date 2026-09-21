#pragma once

#include <Epub.h>
#include <Epub/PageLink.h>
#include <FsHelpers.h>
#include <Logging.h>

#include <optional>
#include <vector>

#include "ProgressFile.h"

namespace EpubReaderUtils {

// Persists reader progress for an EPUB to its cache directory. Returns true on success.
inline bool saveProgress(const Epub& epub, int spineIndex, int pageNumber, int pageCount,
                         std::optional<uint32_t> visibleTextOffset = std::nullopt) {
  if (spineIndex < 0 || spineIndex > 0xFFFF || pageNumber < 0 || pageNumber > 0xFFFF || pageCount < 0 ||
      pageCount > 0xFFFF) {
    LOG_ERR("ERS", "Progress values out of range: spine=%d page=%d count=%d", spineIndex, pageNumber, pageCount);
    return false;
  }
  uint8_t data[10];
  data[0] = spineIndex & 0xFF;
  data[1] = (spineIndex >> 8) & 0xFF;
  data[2] = pageNumber & 0xFF;
  data[3] = (pageNumber >> 8) & 0xFF;
  data[4] = pageCount & 0xFF;
  data[5] = (pageCount >> 8) & 0xFF;
  size_t dataSize = 6;
  if (visibleTextOffset.has_value()) {
    data[6] = *visibleTextOffset & 0xFF;
    data[7] = (*visibleTextOffset >> 8) & 0xFF;
    data[8] = (*visibleTextOffset >> 16) & 0xFF;
    data[9] = (*visibleTextOffset >> 24) & 0xFF;
    dataSize = sizeof(data);
  }
  if (!ProgressFile::writeAtomic(epub.getCachePath(), data, dataSize)) {
    return false;
  }
  LOG_DBG("ERS", "Progress saved: spine=%d offset=%u page=%d", spineIndex, visibleTextOffset.value_or(0), pageNumber);
  return true;
}

// 0-100 completion percentage for an EPUB's saved reading progress, or -1.0f if path isn't an
// EPUB or its book.bin cache can't be loaded from disk as-is (buildIfMissing=false -- callers use
// this only for books that should already have been opened once, e.g. recent/library covers, so
// this deliberately doesn't pay for a full rebuild just to decorate a cover).
inline float recentBookProgressPercent(const std::string& path) {
  if (!FsHelpers::hasEpubExtension(path)) return -1.0f;

  Epub epub(path, "/.crosspoint");
  if (!epub.load(/*buildIfMissing=*/false, /*skipLoadingCss=*/true)) return -1.0f;

  HalFile f;
  if (!Storage.openFileForRead("ERS", epub.getCachePath() + "/progress.bin", f)) return -1.0f;
  uint8_t data[10];
  const int dataSize = f.read(data, sizeof(data));
  if (dataSize != 4 && dataSize != 6 && dataSize != 10) return -1.0f;

  const int spineIndex = data[0] + (data[1] << 8);
  int pageCount = 0;
  if (dataSize == 6 || dataSize == 10) {
    pageCount = data[4] + (data[5] << 8);
  }
  int pageNumber = data[2] + (data[3] << 8);
  if (pageNumber == UINT16_MAX) pageNumber = 0;  // in-memory navigation sentinel, never persisted state

  const float chapterProgress = pageCount > 0 ? static_cast<float>(pageNumber) / static_cast<float>(pageCount) : 0.0f;
  return epub.calculateProgress(spineIndex, chapterProgress) * 100.0f;
}

inline const PageLink* linkAtPoint(const std::vector<PageLink>& links, const int x, const int y, const int marginLeft,
                                   const int marginTop) {
  // Finger slop, plus a floor on the target width: a note marker is often a single superscript
  // digit only a few pixels wide. The box is never grown vertically beyond its own line, so
  // taps on the lines above and below still reach the page-turn zones.
  constexpr int TOUCH_SLOP = 6;
  constexpr int MIN_TOUCH_WIDTH = 28;
  const int pageX = x - marginLeft;
  const int pageY = y - marginTop;
  for (const auto& link : links) {
    if (link.contains(pageX, pageY, TOUCH_SLOP, MIN_TOUCH_WIDTH)) {
      return &link;
    }
  }
  return nullptr;
}

}  // namespace EpubReaderUtils
