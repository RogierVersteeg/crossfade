#pragma once

#include <HalStorage.h>

class Print;

class PngToBmpConverter {
  static bool pngFileToBmpStreamInternal(HalFile& pngFile, Print& bmpOut, int targetWidth, int targetHeight,
                                         bool oneBit, bool crop = true);

 public:
  static bool pngFileToBmpStream(HalFile& pngFile, Print& bmpOut, bool crop = true);
  static bool pngFileToBmpStreamWithSize(HalFile& pngFile, Print& bmpOut, int targetMaxWidth, int targetMaxHeight);
  // crop=true (default): cover-crop scaled, may overflow the nominal target size -- matches every
  // existing caller's behavior. crop=false: letterbox-contained, so the output BMP's own
  // dimensions never exceed targetMaxWidth/targetMaxHeight and a caller that draws it at that
  // exact box size needs no further scaling.
  static bool pngFileTo1BitBmpStreamWithSize(HalFile& pngFile, Print& bmpOut, int targetMaxWidth, int targetMaxHeight,
                                             bool crop = true);
};
