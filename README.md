# CrossFade

A personal fork of [CrossPoint](https://github.com/crosspoint-reader/crosspoint-reader) firmware for the Xteink X3, X4 and **X4 Pro** e-readers, focused on making your book covers and library the center of the experience.

This repository is a fork of [Chuckthe5th/crossfade](https://github.com/Chuckthe5th/crossfade). It takes the `crossfade-1.6.0` feature set from that project, re-bases it onto **CrossPoint 1.6.5**, and adds **X4 Pro** (ESP32-S3, touch) support. CrossFade keeps CrossPoint's on-disk format and MIT license.

---

## ⚠️ Read this before flashing

**Flashing custom firmware can permanently brick a locked device.**

- Some Xteink units — especially those bought from third-party stores like AliExpress — ship with **USB flashing locked**. On a locked unit, a bad flash can leave the device stuck with no recovery path. Units bought directly from xteink.com are generally not locked.
- **Check first:** connect the device over USB-C and try the web flasher. If the device shows up and can be read, you're not locked. If it never appears, do not proceed without the Xteink Unlocker.
- **Back up first:** use the flasher's full-flash backup/read option and save the file somewhere safe. This is your only guaranteed way back.
- **You flash at your own risk.** This is a hobbyist fork provided as-is, with no warranty. See the license.
- Every image carries a board tag. Both the web flasher path and OTA reject an image built for a different board before writing it.

### Device support status

| Device | Build env | Status |
|--------|-----------|--------|
| **Xteink X3** | `default` (shared with X4) | Builds; runtime device detection as upstream. |
| **Xteink X4** | `default` (shared with X3) | Builds; runtime device detection as upstream. |
| **Xteink X4 Pro** | `x4pro` | Builds; touch navigation added for the CrossFade screens (see below). Needs on-device testing. |
| **Xteink X4 Classic** | `x4c` | Builds (inherited from the 1.6.0 branch). |
| Other CrossPoint devices | `sticky`, `papermono` | Compile only, untested. |

The X3 and X4 share one binary; the X4 Pro is a separate ESP32-S3 binary (`firmware-x4pro.bin`).

---

## What CrossFade adds

Everything below is on top of stock CrossPoint 1.6.5.

### Browse Books — three ways to see your library

Selectable in **Settings → Display → File Browser View**:

- **Files** — the stock file browser, unchanged.
- **Titles** — a text list showing each book's title and author.
- **Covers** — a paginated grid of cover thumbnails.

Covers and Titles show the same library in the same order, with a page-position indicator. Covers view has a choice of **vertical or horizontal pagination** — vertical turns pages with the side buttons, horizontal with the front rocker. Thumbnails are generated once at the exact cell size and cached, so the grid stays responsive.

### Library — list or covers

Upstream 1.6.5 replaced the old Recent Books screen with a tabbed **Library** (Recent / Title / Author / Search). **Settings → Display → Library View** chooses what the Home screen's Library entry opens: upstream's **List**, or CrossFade's **Covers** grid of your recent books.

### Lyra Carousel theme

A **Settings → Display → UI Theme** option: a large center cover flanked by smaller side covers, dot pagination, a progress bar and an icon-only button menu. A functional port of [CrossInk](https://github.com/uxjulia/CrossInk)'s Lyra Carousel theme. Left/Right move within the carousel or the icon row; Up/Down switch between the two.

### Completion indicator on covers

Optional — **Settings → System → Show Completion Indicator** (on by default). Shows each book's real reading progress on its cover on the Home screen: a bar under the cover in most themes, a filled side line on the Classic theme, and Lyra Carousel's own built-in progress bar.

### Series grouping

Optional — **Settings → Display → Group by Series**. Books that share a series collapse into a single entry; selecting one opens a page of just that series in reading order. Reads series and index from Calibre metadata (`calibre:series` / `calibre:series_index`), with EPUB3 collections as a fallback. A manual **Rebuild Library Index** action is available under Settings → System, and the build can be cancelled.

### Per-book context menu

Long-press **Confirm** on a book (in Covers or Titles) opens a context menu with per-book actions, including **Mark as finished** and, when the setting below is on, **Pin to Home**. *Mark as finished* is also available from the reader menu.

### Pin a book to Home

Optional — **Settings → System → Pin Book to Home**. Long-press **Confirm** on any book and choose **Pin to Home** to give it its own permanent Home-screen entry, independent of Recent Books — useful for a book you return to daily. Selecting it resumes at its saved position. The entry only appears when there's genuinely enough room for it on screen for your theme; otherwise it stays hidden rather than crowding out other rows. (Not shown on the PSRAM-only Cover Grid home theme, which has its own fixed layout.)

### Transfer & Sync

OPDS Browser and File Transfer are combined into a single **Transfer & Sync** Home entry. With no OPDS servers configured it goes straight to File Transfer; with servers configured it opens a small picker between the two.

### Automatic KOReader Sync

Optional — **Settings → KOReader Sync → Auto-Sync on Sleep/Open** (on by default once KOReader Sync is configured). Pushes your progress automatically when you sleep from the reader, and pulls the furthest progress between device and server when you open a book from Home, the library or the file browser — never moving your position backward. Both directions are silent, bounded to a few seconds, abortable by a button press, and skipped entirely with no saved network. They use the same furthest-wins comparison as CrossPoint's manual smart sync, including the CrossPoint sync server's precise positions.

### Custom device name

**Settings → System → Device Name** sets the name used anywhere the device identifies itself (currently the device field sent to a KOReader Sync server) instead of the hardware default (CrossFade X3 / X4 / X4 Pro).

### Reading Stats

Optional — **Settings → System → Track Reading Stats** (off by default). Tracks per-book and all-books reading time, forward-page pace, and an estimated time-left for the book you're in. Turning it on adds a **Reading Stats** entry to the reader menu's Main tab and shows a time-read/time-left label under each cover's progress bar on the Lyra Carousel.

### Reader menu tabs

The in-book reader menu is tabbed — **Main**, **Bookmarks** and **Text** — on the same tab-bar-and-list navigation as Settings. Confirm on the tab bar, a continuous hold of the navigation buttons, or a tap on a tab switches tabs. Bookmark actions live on their own tab; text settings on theirs.

### Sleep screen

The sleep screen keeps the original **CrossPoint** logo and name; only the boot screen and UI carry the CrossFade name.

### Hide button hints

Optional — **Settings → Controls → Hide Button Hints**. Removes the on-screen row showing what each physical button does, and lets whatever's above it reclaim that space — the same behaviour touch devices get automatically.

### 10pt reader font

A smaller Noto Serif / Noto Sans size for the reader.

### Smaller fixes carried over

OPDS server picker returns to its caller instead of Home; WiFi credential store hydrated at boot; X4C top-bar and button-hint fixes; bounded `content.opf` parsing and a fork marker on `book.bin` so a cache written by another firmware is never misread.

---

## X4 Pro

The X4 Pro has only Left, Right and Power buttons plus a capacitive Home key and a touch screen, so the button-driven CrossFade screens got touch equivalents:

- **Cover grid:** tap a cover to open it, swipe up/left or down/right to turn pages, **hold** a cover to open its context menu. Back is the header back button or a swipe from the left edge.
- **Titles list:** taps, swipes and long-press come from upstream's list framework.
- **Lyra Carousel home:** swipe left/right to move the carousel, tap a side cover to center it, tap the centered cover to open it, tap an icon to activate that menu entry. The icon row sits at the real bottom edge, since touch boards have no button-hints row.

Everything else (frontlight, warm light, USB Drive mode, KOReader sync, OTA) is upstream 1.6.5 behaviour.

---

## Installing

### Flash a release

1. Read the warning above and take a full-flash backup.
2. Download the firmware for your device from the [Releases page](https://github.com/RogierVersteeg/crossfade/releases): the `x3-x4` image for X3/X4, the `x4pro` image for the X4 Pro.
3. In **Chrome or Edge**, open the CrossPoint web flasher at [crosspointreader.com](https://crosspointreader.com/#flash-tools), select your device, choose **Custom .bin**, and upload the downloaded file.

If something goes wrong: press Reset, then hold **Back + Power** to reach the home screen. If it boots but behaves oddly around covers, delete the `.crosspoint` folder on the SD card to clear the caches. Worst case, re-flash an official CrossPoint release or restore your backup from the same flasher.

OTA update checks (Settings → System → Check for updates) look at this repository's releases, not upstream CrossPoint's, so an update never silently replaces CrossFade with stock firmware.

### Build from source

CrossFade builds with [pioarduino](https://github.com/pioarduino/pioarduino) (a PlatformIO fork for the ESP32 Arduino 3.x core).

```
git clone --recursive https://github.com/RogierVersteeg/crossfade
cd crossfade
pio run -e default     # X3 / X4
pio run -e x4pro       # X4 Pro
pio run -e x4c         # X4 Classic
```

The build artifact is `.pio/build/<env>/firmware.bin`, which you flash via the web flasher's Custom .bin option. Requires Python 3.8+, clang-format 21, and a USB-C data cable. See upstream's [contributing docs](./docs/contributing/README.md) for the rest.

---

## Compatibility

CrossFade uses CrossPoint's standard `.crosspoint` SD-card folder, so switching between the two firmwares does not lose your covers, caches, or reading positions. Its own index and cache files carry a fork marker so they can't be confused with upstream's. Settings are shared with upstream (`settings.json`); note that CrossFade numbers the UI themes `Classic, Lyra, Lyra Extended, RoundedRaff, Lyra Carousel, Cover Grid`, so a theme chosen on stock 1.6.5 may map to a different one here.

---

## Credit

- [CrossPoint](https://github.com/crosspoint-reader/crosspoint-reader) — the firmware this is built on. MIT.
- [Chuckthe5th/crossfade](https://github.com/Chuckthe5th/crossfade) — the CrossFade features. MIT.
- [CrossInk](https://github.com/uxjulia/CrossInk) — the Lyra Carousel design that was ported. MIT.

CrossFade is **not affiliated with Xteink or any device manufacturer**.
