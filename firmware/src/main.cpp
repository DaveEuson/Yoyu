// Yoyu — Claude usage meters on a Waveshare ESP32-S3-Touch-LCD-2.
// Copyright (c) 2026 Dave Euson. Made with love in San Diego.
//
// v0 scope: join Wi-Fi (first boot: its own "Yoyu-Setup" hotspot with a
// phone setup page, like the Pi version), then speak the same HTTP API as the
// Pi tracker — GET /api/status with the "Yoyu" discovery marker and
// POST /api/push — so the existing desktop companion feeds it with no changes.
// Phase 2 (later): poll Anthropic's usage endpoint directly on-device.
//
// Board facts (cross-checked against community drivers for this exact board):
//   LCD  ST7789 240x320 IPS: SCLK=39 MOSI=38 MISO=40 DC=42 CS=45 BL=1 RST=soft
//   Touch CST816D (unused in v0): SDA=48 SCL=47 addr 0x15

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <math.h>
#include <ctype.h>
#include <time.h>
#ifdef HR_TLS_CURATED_ROOTS
#include "root_cas.h"          // only compiled in for the fallback build
#endif
#include <mbedtls/md.h>
#include <esp_random.h>
#include <mbedtls/pk.h>
#include "ota_pubkey.h"

// TLS trust for every outbound HTTPS call, so a MITM can't impersonate
// Anthropic/GitHub or feed a forged OTA image. All call sites go through here.
//
// Default: the ESP-IDF root certificate bundle that ships with the core — the
// full Mozilla root set. A hand-picked list of roots (what this used to do)
// only fails when a host we rarely reach rotates to an unlisted CA, and the
// token-refresh host is contacted only when a token expires, so such a gap
// could sit undetected for weeks. The bundle also costs *less* heap per
// connection: it binary-searches the roots and parses only the one that
// matches, instead of parsing every embedded PEM on each handshake.
//
// Build flags, neither of which ships:
//   -DHR_TLS_CURATED_ROOTS  fall back to the curated set in root_cas.h
//   -DHR_TLS_INSECURE       no verification at all — local development only
#if !defined(HR_TLS_INSECURE) && !defined(HR_TLS_CURATED_ROOTS)
extern const uint8_t rootca_crt_bundle_start[] asm("_binary_x509_crt_bundle_start");
#endif

static inline void tlsTrust(WiFiClientSecure &c) {
#if defined(HR_TLS_INSECURE)
  c.setInsecure();
#elif defined(HR_TLS_CURATED_ROOTS)
  c.setCACert(HR_ROOT_CAS);
#else
  c.setCACertBundle(rootca_crt_bundle_start);
#endif
}

// ------------------------------------------------------------ panel / board

// Pins and panel geometry for every supported board live in boards.h, selected
// by the -DYOYU_BOARD_* flag platformio.ini passes. Nothing board-specific is
// allowed to appear below this line.
#include "boards.h"

#if PANEL_IS_QSPI
// Arduino_ESP32QSPI names its four data lines (mosi, miso, quadwp, quadhd),
// which are D0..D3 on the panel datasheet.
static Arduino_DataBus *bus =
    new Arduino_ESP32QSPI(QSPI_CS, QSPI_CLK, QSPI_D0, QSPI_D1, QSPI_D2, QSPI_D3);
static Arduino_GFX *gfx =
    new Arduino_CO5300(bus, PANEL_RST, PANEL_ROTATION, PANEL_INVERT,
                       PANEL_W, PANEL_H);
#else
static Arduino_DataBus *bus =
    new Arduino_ESP32SPI(LCD_DC, LCD_CS, LCD_SCLK, LCD_MOSI, LCD_MISO);
// rotation 2 = portrait 240x320 flipped 180° (USB-C connector at the top)
static Arduino_GFX *gfx =
    new Arduino_ST7789(bus, LCD_RST, PANEL_ROTATION, PANEL_INVERT,
                       PANEL_W, PANEL_H);
#endif
// Off-screen framebuffer (PSRAM) for the animated kitsune screen: draw a whole
// frame into RAM, then blit it in one pass so the animation never flickers.
static Arduino_Canvas *mascotBuf = nullptr;

// ---- layout: one design space, many panels ------------------------------
// Every screen is authored against this 240x320 reference and mapped to the
// real panel at draw time. The alternative -- computing each coordinate from
// gfx->width()/height() at the point of use -- means touching every drawing
// call and re-tuning spacing that already works, on a panel nobody has yet.
//
// On the reference panel the mapping is the identity, so this refactor cannot
// move a single pixel on hardware that exists today. That is the property that
// makes it verifiable now rather than whenever a bigger board arrives.
static const int REF_W = DESIGN_W;
static const int REF_H = DESIGN_H;
static int     scrW    = REF_W;     // real panel, filled in at boot
static int     scrH    = REF_H;
static uint8_t uiScale = 1;         // bitmap-font multiplier for larger panels

// The scale is ONE number for both axes, in 8.8 fixed point, plus a centring
// offset. Scaling each axis to its own panel dimension is the obvious thing and
// it is wrong: the 2.16" board is 480x480 against a 240x320 design space, which
// works out at x2.00 across and x1.50 down, so everything renders a third wider
// than it is tall. A square panel is not a bigger portrait panel.
//
// Fitting the tighter axis instead costs the margins -- 60px down each side on
// that board, a quarter of the glass -- and keeps every proportion the design
// was drawn with. Reclaiming those margins means drawing square variants of
// each screen, which is a design job, not a scaling constant.
static int32_t mapQ = 256;          // uniform scale, 256 = 1:1
static int     offX = 0, offY = 0;  // letterbox offsets, centring the design space

// Named mapX/mapY rather than anything shorter: the mascot sprite already has
// locals called lx, rx, px and my, and a helper those would shadow is a trap
// for whoever edits that function next.
static inline int mapX(int x) { return offX + (int)(((int32_t)x * mapQ) >> 8); }
static inline int mapY(int y) { return offY + (int)(((int32_t)y * mapQ) >> 8); }
// A LENGTH in design units — scaled, but not shifted into place. mapX/mapY
// answer "where is this point"; mapLen answers "how big is this". They were the
// same arithmetic until the design space stopped filling the panel, and every
// width, height, radius and delta has to use this one instead: on a letterboxed
// panel, sizing a bar with mapX would add the 60px left margin to its width as
// well as its position. On the reference panel the two are identical, which is
// why the distinction could go unnoticed for as long as it did.
static inline int mapLen(int n) { return (int)(((int32_t)n * mapQ) >> 8); }
// Fonts are bitmaps, so they scale in whole steps or not at all: a 480px-wide
// panel would otherwise render the same 6px glyphs across twice the glass.
// Rounded rather than truncated, so a x1.5 panel reads as 2, not 1.
static inline uint8_t mapSz(uint8_t s) { return (uint8_t)(s * uiScale); }

static void initLayout() {
  scrW = gfx->width();
  scrH = gfx->height();
  int32_t qx = ((int32_t)scrW << 8) / REF_W;
  int32_t qy = ((int32_t)scrH << 8) / REF_H;
  mapQ = qx < qy ? qx : qy;         // never let one axis outgrow the other
  if (mapQ < 1) mapQ = 1;
  offX = (scrW - (int)(((int32_t)REF_W * mapQ) >> 8)) / 2;
  offY = (scrH - (int)(((int32_t)REF_H * mapQ) >> 8)) / 2;
  int32_t s = (mapQ + 128) >> 8;    // round to the nearest whole font step
  uiScale = (uint8_t)(s < 1 ? 1 : s);
}

// Claude night palette in RGB565 (macro provided by Arduino_GFX)
// The nine UI colours, now chosen at runtime rather than compiled in.
//
// The reason is the AMOLED board. On the IPS panel the ground is a *backlit*
// dark grey, so the whole screen sits at a similar luminance and the accents
// read as warm. On an AMOLED the same ground is pixels switched off, emitting
// nothing at all -- so every lit element runs at full blast against true black
// with no middle register. Identical values, completely different physics.
//
// Each theme supplies its own full three-tier ladder, so the rule that a state
// is never carried by hue alone (fill colour + fill length + a printed number)
// survives every one of them. Mono has no hue to carry it with and leans on
// luminance instead, which is exactly why the other two signals exist.
//
// The mascot's four colours are deliberately NOT themed: they are shared
// byte-for-byte with the SVG on the setup page, and a kitsune that changed
// colour with the background would stop being the same character.
static uint16_t C_BG, C_INK, C_MUTED;
static uint16_t C_ACC, C_ACC_T, C_WARN, C_WARN_T, C_CRIT, C_CRIT_T;

enum { THEME_NIGHT = 0, THEME_DIM, THEME_PAPER, THEME_MONO,
       THEME_NORD, THEME_TOKYO, THEME_COUNT };
static const char *const THEME_NAMES[THEME_COUNT] = {
    "Night", "Dim", "Paper", "Mono", "Nord", "Tokyo Night"};
static int uiTheme = -1;           // -1 = not applied yet

static void applyTheme(int t) {
  if (t < 0 || t >= THEME_COUNT) t = THEME_NIGHT;
  uiTheme = t;
  switch (t) {
    case THEME_DIM:
      // For an always-on AMOLED, especially after dark. A true-black ground
      // costs no light at all on that panel, and every accent is pulled down
      // to roughly 40% so nothing glares in a dim room. It also gives the
      // static labels far less to burn in with.
      C_BG    = RGB565(0x0A, 0x0A, 0x0A); C_INK   = RGB565(0x9A, 0x98, 0x90);
      C_MUTED = RGB565(0x55, 0x53, 0x4B);
      C_ACC   = RGB565(0x8A, 0x4C, 0x37); C_ACC_T = RGB565(0x2A, 0x20, 0x19);
      C_WARN  = RGB565(0x9E, 0x70, 0x10); C_WARN_T= RGB565(0x2A, 0x24, 0x10);
      C_CRIT  = RGB565(0x8E, 0x34, 0x34); C_CRIT_T= RGB565(0x2C, 0x18, 0x18);
      break;
    case THEME_PAPER:
      // The project's own paper palette, reused rather than reinvented: these
      // are the values DESIGN.md already vouches for on cream, including the
      // terracotta picked "to be read" rather than to fill a surface. Inverting
      // the night theme instead would have put #94907e on a light ground, which
      // measures 2.75:1 and fails AA -- the exact mistake the Polarity Rule
      // exists to prevent.
      C_BG    = RGB565(0xF0, 0xEE, 0xE6); C_INK   = RGB565(0x3D, 0x39, 0x29);
      C_MUTED = RGB565(0x6B, 0x67, 0x59);
      C_ACC   = RGB565(0xA8, 0x44, 0x2A); C_ACC_T = RGB565(0xE6, 0xD6, 0xCE);
      C_WARN  = RGB565(0x8A, 0x5A, 0x00); C_WARN_T= RGB565(0xEE, 0xE2, 0xC4);
      C_CRIT  = RGB565(0xA8, 0x2A, 0x2A); C_CRIT_T= RGB565(0xF0, 0xD6, 0xD6);
      break;
    case THEME_MONO:
      // No hue at all. The ladder climbs in brightness instead -- dim, brighter,
      // brightest -- which works because colour was never the only carrier.
      C_BG    = RGB565(0x0A, 0x0A, 0x0A); C_INK   = RGB565(0xE8, 0xE8, 0xE8);
      C_MUTED = RGB565(0x6E, 0x6E, 0x6E);
      C_ACC   = RGB565(0xA0, 0xA0, 0xA0); C_ACC_T = RGB565(0x26, 0x26, 0x26);
      C_WARN  = RGB565(0xD0, 0xD0, 0xD0); C_WARN_T= RGB565(0x2E, 0x2E, 0x2E);
      C_CRIT  = RGB565(0xFF, 0xFF, 0xFF); C_CRIT_T= RGB565(0x38, 0x38, 0x38);
      break;
    case THEME_NORD:
      // Nord (Arctic Ice Studio, MIT). Its own Aurora green/yellow/red are a
      // ready-made three-tier ladder, which is the only reason a borrowed
      // palette can be dropped in at all -- a scheme without semantic tiers
      // could not drive these meters however good it looked.
      //
      // Muted is nord9 rather than nord3. nord3 is Nord's comment colour and
      // measures 1.7:1 here: fine greyed out on a large monitor, unreadable as
      // body text on a 2" panel at arm's length. nord9 is still Nord, at 4.6:1.
      C_BG    = RGB565(0x2E, 0x34, 0x40); C_INK   = RGB565(0xEC, 0xEF, 0xF4);
      C_MUTED = RGB565(0x81, 0xA1, 0xC1);
      C_ACC   = RGB565(0xA3, 0xBE, 0x8C); C_ACC_T = RGB565(0x48, 0x52, 0x51);
      C_WARN  = RGB565(0xEB, 0xCB, 0x8B); C_WARN_T= RGB565(0x58, 0x55, 0x50);
      C_CRIT  = RGB565(0xBF, 0x61, 0x6A); C_CRIT_T= RGB565(0x4E, 0x3E, 0x49);
      break;
    case THEME_TOKYO:
      // Tokyo Night (enkia, MIT). Same story: muted is the palette's blue
      // rather than its comment colour, which measures 2.8:1 on this ground.
      // The tier colours are untouched -- they are the theme's identity, and
      // the meters carry state with fill length and a printed number as well.
      C_BG    = RGB565(0x1A, 0x1B, 0x26); C_INK   = RGB565(0xC0, 0xCA, 0xF5);
      C_MUTED = RGB565(0x7A, 0xA2, 0xF7);
      C_ACC   = RGB565(0x9E, 0xCE, 0x6A); C_ACC_T = RGB565(0x37, 0x42, 0x35);
      C_WARN  = RGB565(0xE0, 0xAF, 0x68); C_WARN_T= RGB565(0x46, 0x3C, 0x35);
      C_CRIT  = RGB565(0xF7, 0x76, 0x8E); C_CRIT_T= RGB565(0x4B, 0x2F, 0x3D);
      break;
    default:                                  // THEME_NIGHT -- the original
      C_BG    = RGB565(0x26, 0x26, 0x24); C_INK   = RGB565(0xF5, 0xF4, 0xEF);
      C_MUTED = RGB565(0x94, 0x90, 0x7E);
      C_ACC   = RGB565(0xD9, 0x77, 0x57); C_ACC_T = RGB565(0x4A, 0x38, 0x2F);
      C_WARN  = RGB565(0xFA, 0xB2, 0x19); C_WARN_T= RGB565(0x46, 0x3B, 0x1A);
      C_CRIT  = RGB565(0xE0, 0x52, 0x52); C_CRIT_T= RGB565(0x4A, 0x27, 0x27);
      break;
  }
}

// Yoyu, the kitsune
static const uint16_t C_SPRK  = RGB565(0xC9, 0x60, 0x3F);   // fox fur
static const uint16_t C_SPRK_D= RGB565(0x9E, 0x44, 0x29);   // ear + tail shade
static const uint16_t C_OUT   = RGB565(0x1A, 0x18, 0x16);   // outline / features
static const uint16_t C_FACE  = RGB565(0xFA, 0xF7, 0xEF);   // face screen

static const int K_COLS = 18, K_ROWS = 15;
static const int K_CELL = 13;   // px per cell at uiScale 1

// The kitsune, drawn on a 18x15 cell grid shared by the device screen and
// the web UI. K=outline B=fur W=markings S=shade  '.'=background
static const char *const KITSUNE_SPRITE[15] = {
    "..................", "..................", ".K.......K........",
    ".KSK...KSK........", "KBSSK.KSSBK.......", "KBBBKKKBBBK.......",
    "KBBBBBBBBBK.......", "KBBBBBBBBBK.......", ".KBBBBBBBK........",
    "..KBWWWBK.........", "...KWWWK..........", ".KBBBBBBBK........",
    ".KBBWWWBBK........", ".KBBWWWBBK........", "..KK...KK........."};

// Tails are the gauge: one when headroom is nearly gone, three when there is
// plenty. Each is a pre-rasterised arc leaving the same hip at its own angle,
// with a ring of background cells around it -- two tails of the same colour
// that touch read as one tail, which would make the count useless. Ordered
// innermost first so drawing the first N gives a fan that opens outward.
struct TailCell { uint8_t r, c; char ch; };
static const TailCell TAIL_0[35] = {
    {10,10,'.'},{10,11,'.'},{10,12,'.'},{10,13,'.'},{10,14,'.'},{11,15,'.'},
    {11,16,'.'},{12,17,'.'},{13,17,'.'},{14,10,'.'},{14,11,'.'},{14,12,'.'},
    {14,13,'.'},{14,14,'.'},{14,15,'.'},{14,16,'.'},{11,10,'S'},{11,11,'S'},
    {11,12,'S'},{11,13,'S'},{11,14,'S'},{12,10,'S'},{12,11,'S'},{12,12,'S'},
    {12,13,'S'},{12,14,'S'},{12,15,'W'},{12,16,'W'},{13,10,'S'},{13,11,'S'},
    {13,12,'S'},{13,13,'S'},{13,14,'S'},{13,15,'W'},{13,16,'W'}};
static const TailCell TAIL_1[48] = {
    { 6,14,'.'},{ 6,15,'.'},{ 6,16,'.'},{ 7,12,'.'},{ 7,13,'.'},{ 7,17,'.'},
    { 8,11,'.'},{ 8,17,'.'},{ 9, 9,'.'},{ 9,10,'.'},{ 9,17,'.'},{10, 8,'.'},
    {10,15,'.'},{10,16,'.'},{11,14,'.'},{12,13,'.'},{13,12,'.'},{14,10,'.'},
    {14,11,'.'},{ 7,14,'S'},{ 7,15,'W'},{ 7,16,'W'},{ 8,12,'S'},{ 8,13,'S'},
    { 8,14,'S'},{ 8,15,'W'},{ 8,16,'W'},{ 9,11,'S'},{ 9,12,'S'},{ 9,13,'S'},
    { 9,14,'S'},{ 9,15,'S'},{ 9,16,'W'},{10, 9,'S'},{10,10,'S'},{10,11,'S'},
    {10,12,'S'},{10,13,'S'},{10,14,'S'},{11,10,'S'},{11,11,'S'},{11,12,'S'},
    {11,13,'S'},{12,10,'S'},{12,11,'S'},{12,12,'S'},{13,10,'S'},{13,11,'S'}};
static const TailCell TAIL_2[46] = {
    { 1,13,'.'},{ 2,12,'.'},{ 2,14,'.'},{ 3,11,'.'},{ 3,15,'.'},{ 4,15,'.'},
    { 5,14,'.'},{ 6,14,'.'},{ 7,13,'.'},{ 8,13,'.'},{ 9,12,'.'},{10,12,'.'},
    {11,12,'.'},{12,12,'.'},{13,10,'.'},{13,11,'.'},{ 2,13,'W'},{ 3,12,'S'},
    { 3,13,'W'},{ 3,14,'W'},{ 4,11,'S'},{ 4,12,'S'},{ 4,13,'W'},{ 4,14,'W'},
    { 5,11,'S'},{ 5,12,'S'},{ 5,13,'S'},{ 6,11,'S'},{ 6,12,'S'},{ 6,13,'S'},
    { 7,11,'S'},{ 7,12,'S'},{ 8,10,'S'},{ 8,11,'S'},{ 8,12,'S'},{ 9, 9,'S'},
    { 9,10,'S'},{ 9,11,'S'},{10, 8,'S'},{10, 9,'S'},{10,10,'S'},{10,11,'S'},
    {11,10,'S'},{11,11,'S'},{12,10,'S'},{12,11,'S'}};
static const TailCell *const KITSUNE_TAILS[3] = {TAIL_0, TAIL_1, TAIL_2};
static const uint8_t KITSUNE_TAIL_N[3] = {35, 48, 46};

// The resting face, at whole-cell resolution. On the panel the features are
// drawn as sub-cell rects by drawKitsuneAnim, because they have to change with
// the mood; the SVG has no mood to show and gets this instead. Without it the
// web avatar renders faceless, which is how it shipped the first time.
static const TailCell KITSUNE_FACE[5] = {
    {6, 2, 'K'}, {6, 7, 'K'},            // eyes
    {9, 5, 'K'},                          // nose
    {10, 4, 'K'}, {10, 6, 'K'},           // mouth
};

// ------------------------------------------------------------------- state

struct Window {
  char key[24];
  char label[28];
  float utilization;      // % used, 0..100
  time_t resets_at;       // UTC epoch, 0 if unknown
};

// Anthropic reports a window per limit: session, weekly (all models), plus one
// per model (Opus, Sonnet, Fable...), connected apps and extra usage. Sized
// with headroom so a newly-introduced model doesn't push a real window off.
static const int MAX_WINDOWS = 8;
static Window windows[MAX_WINDOWS];
static int nWindows = 0;
// How many windows the source actually reported, which can exceed the
// MAX_WINDOWS we keep. The meters footer counts from this so "+N more" stays
// truthful instead of counting only what survived truncation.
static int nWindowsSeen = 0;
static char plan[16] = "";
static unsigned long lastPushMs = 0;   // millis() of last accepted push
static bool timeSynced = false;

static Preferences prefs;
static WebServer *server = nullptr;
static DNSServer dns;
static bool apMode = false;

// Same defaults as the Pi build
static const char *AP_SSID = "Yoyu-Setup";
// WPA2 will not accept fewer than 8 characters: softAP() refuses outright and
// no hotspot is broadcast at all. The rename shortened this from "headroom" (8)
// to "yoyu" (4), which shipped from v1.6.0 and quietly made first-run setup
// impossible -- every existing board already had Wi-Fi saved, so none of them
// ever entered the portal to show it. The assert below is the actual fix; the
// password is just a password.
static const char AP_PSK[] = "yoyusetup";
static_assert(sizeof(AP_PSK) - 1 >= 8,
              "AP_PSK must be 8+ chars or WiFi.softAP() fails and the setup "
              "hotspot never appears -- see v1.6.0");
static const int   API_PORT = 8080;   // what the companion probes
static const char *FW_VERSION = "1.7.0";

// Phase 2 — self-contained: poll Anthropic's usage endpoint directly, using an
// OAuth login pasted once via /connect. Same contract the companion uses.
static const char *CLIENT_ID   = "9d1c250a-e61b-44d9-88ed-5944d1962f5e";
static const char *REFRESH_URL = "https://platform.claude.com/v1/oauth/token";
static const char *USAGE_URL   = "https://api.anthropic.com/api/oauth/usage";
static const char *OAUTH_BETA  = "oauth-2025-04-20";
static const char *UA          = "Yoyu/1.7.0";
// OTA self-update (over-the-air from the GitHub release)
static const char *RELEASES_API =
    "https://api.github.com/repos/DaveEuson/Yoyu/releases/latest";
// Built from the board's own prefix. This is a safety property, not a naming
// one: both boards' images are validly signed, so the signature check would
// accept the LCD image onto an AMOLED board and leave it with a driver that
// cannot talk to its panel.
#define GH_DL "https://github.com/DaveEuson/Yoyu/releases/latest/download/"
// OTA_ASSET_PREFIX is per-board and lives in boards.h. The LCD board's prefix
// is deliberately still "headroom-mini": boards already in the field fetch
// those exact names, and renaming them turns every OTA into a 404 on hardware
// that can no longer be reached any other way.
static const char *APP_BIN_URL = GH_DL OTA_ASSET_PREFIX "-app.bin";
static const char *APP_SIG_URL = GH_DL OTA_ASSET_PREFIX "-app.bin.sig";
static const unsigned long POLL_INTERVAL_MS = 5UL * 60UL * 1000UL;
// How long a companion push keeps this board from polling for itself. Comfortably
// longer than the companion's own 2-minute cadence, so an occasional slow cycle
// doesn't have both of them reading at once.
static const unsigned long COMPANION_FRESH_MS = 8UL * 60UL * 1000UL;
static unsigned long pollBackoffMs = 0;   // extra wait after a 429, exponential

static String   accessTok;   // access only: see storeOauth() for why there
                             // is deliberately no refresh token here
// Issued once, when a companion proves physical possession by pairing.
// It buys exactly one power: handing this board a newer access token for
// the account it is already signed in to. That is worth far less than the
// token itself, which is why top-ups do not need someone at the screen.
static String   topupKey;
// When a companion last pushed usage of its own accord (as opposed to this
// board fetching it). Used to stand down our own polling: both read the same
// account, and Anthropic rate-limits per account, so two readers on one login
// spend the budget twice over for one set of numbers.
static unsigned long lastCompanionPushMs = 0;
static uint64_t tokenExpMs = 0;       // epoch ms, 0 = unknown
static bool     selfHosted = false;   // true once a login is stored
static String   pushToken;            // optional shared secret; when set, the
                                      // companion must send it (X-Push-Token) to
                                      // push data or pair. Empty = open (default).
static char     pollStatus[48] = "";  // last on-device poll result (shown when no data)

// UI / input state (Phase 1.5)
static const int BL_CHANNEL = 0;      // LEDC channel for backlight PWM
static const int BOOT_BTN    = 0;     // BOOT button -> hold to factory reset
#if HAS_BATTERY_ADC
static const int BAT_ADC_PIN = VBAT_PIN;  // via the onboard divider (x3)
#endif
static int       batPct      = -1;    // -1 = no battery / hidden
static bool      batCharging = false;
static uint8_t   backlight   = 255;   // 0..255
static bool      showUsed    = false; // false = "% left", true = "% used"
static bool      screenOff   = false; // face-down / manual dim
static char      tzEnv[48]   = "EST5EDT,M3.2.0,M11.1.0";  // POSIX TZ, set via /settings
static bool      clock24     = false; // false = 12-hour (3:45 PM), true = 24-hour
static bool      nightDim    = true;  // ease the backlight down overnight
static const uint8_t NIGHT_LEVEL = 40;
static int       uiScreen    = 0;     // 0 meters 1 focus 2 history 3 kitsune
                                      // 4 timer 5 actions 6 projects 7 settings
static const int UI_SCREENS  = 8;
static const char *SCREEN_NAMES[UI_SCREENS] =
    {"Meters", "Focus", "History", "Yoyu",
     "Timer", "Actions", "Projects", "Settings"};
static uint8_t   screenMask  = 0xFF;  // bit i set = screen i is in the rotation
// screenMask is one bit per screen in a uint8_t, and it is also what gets
// persisted to NVS — a ninth screen needs a wider type *and* a migration, not
// just a bigger UI_SCREENS.
static_assert(UI_SCREENS <= 8, "screenMask (uint8_t) holds at most 8 screens");
static const int SCREEN_TIMER    = 4;
static const int SCREEN_ACTIONS  = 5;
static const int SCREEN_PROJECTS = 6;
static const int SCREEN_SETTINGS = 7;

// ---- Actions: the board as an input device -------------------------------
// The Actions screen queues a shortcut; the companion polls /api/actions and
// synthesises the keystroke on the computer running Claude Code. The board
// never types anything itself and only ever queues on a physical touch, so
// nothing on the network can inject a keypress. The companion must be started
// with --actions to act on them at all.
struct ActionDef { const char *id; const char *label; const char *keys; };
static const ActionDef ACTIONS[] = {
    {"voice",  "Voice mode",  "Space"},
    {"mode",   "Mode toggle", "Shift+Tab"},
    {"cancel", "Interrupt",   "Esc"},
};
static const int N_ACTIONS = sizeof(ACTIONS) / sizeof(ACTIONS[0]);
static int actionSel = 0;             // which action a tap will fire

// ---- Projects: where the tokens actually went ----------------------------
// Anthropic's usage endpoint reports account-wide windows with no per-project
// breakdown, so this can only come from Claude Code's own session logs on the
// computer running the companion. That makes it "this computer", never "this
// account": work done from a second machine or the web app is invisible here.
// The screen says so out loud, because a ranking that silently omits half your
// work is worse than no ranking at all.
struct Proj { char name[22]; float share; };   // share = % of the window's tokens
static const int MAX_PROJ = 5;
static Proj      projects[MAX_PROJ];
static int       nProjects  = 0;
static int       projMore   = 0;       // ranked below the cut — "+N more"
static char      projWindow[12] = "";  // which window the shares cover, e.g. "5h"
static unsigned long projAt = 0;       // last time the companion sent any

// ---- On-device settings --------------------------------------------------
// The board never showed its own address once it was working, so there was no
// way to find /settings from the device itself. This screen is the answer to
// both halves of that: it prints the address, and it edits the one setting
// people actually want to change without a browser.
static int settingSel = 0;             // which row the next tap toggles
static char settingMsg[26] = "";       // why a toggle was refused, "" = nothing
static unsigned long settingMsgAt = 0;

// Pending presses, collected until the companion polls. Entries expire so a
// press made while the companion was closed can't fire minutes later.
static const int  ACTION_Q_MAX = 4;
static const unsigned long ACTION_TTL_MS = 15000;
static const char *actionQ[ACTION_Q_MAX];
static unsigned long actionQAt[ACTION_Q_MAX];
static int actionQN = 0;
static unsigned long lastActionPollMs = 0;   // last time a companion asked
static time_t    timerResetAt = 0;    // reset time the Timer screen is counting to
static bool      timerOut     = false; // that window is spent — we're counting a wait
static int       mascotShownMood = -2; // mood the caption/screen is currently drawn for
static int       mascotFrame  = 0;    // animation frame counter (kitsune screen)
static unsigned long lastActivityMs = 0;  // last time usage went UP (you're using Claude)
static float     prevHeadlineUtil = -1;   // float, so a fractional-% climb still counts
static const unsigned long ACTIVE_WINDOW_MS = 20UL * 60 * 1000;  // "active" if used within 20 min
static int       defaultScreen = 0;   // screen shown at power-on
static int       rotateSecs  = 0;     // 0 = tap-only; else auto-rotate every N s
static unsigned long lastUserTouch = 0;  // for pausing auto-rotate after a tap
static bool screenEnabled(int i) { return screenMask & (1 << i); }
static bool      updateAvailable = false; // a newer release has been seen online
static char      latestSeen[16]  = "";    // its tag, for the landing/update page
// Last tag we actually got from GitHub, and when. Lets one user action cost one
// round trip instead of two — see fetchLatestTag().
// cachedTag[0] is the validity marker, not cachedTagAt, so a fetch that lands
// on millis() == 0 isn't mistaken for "never fetched".
static char      cachedTag[16]   = "";
static unsigned long cachedTagAt = 0;
// Long enough that rendering /update and then POSTing its button reuses one
// answer, short enough that a release published while you are looking at the
// page is still picked up on a refresh.
static const unsigned long TAG_CACHE_MS     = 60000;
static const uint8_t       TAG_FETCH_TRIES  = 3;
static const unsigned long TAG_RETRY_DELAY_MS = 400;
// Release-check health, surfaced in /api/status. The check fails intermittently.
// Heap exhaustion was the theory; these numbers killed it on the first run —
// the fetch costs ~1.2KB against ~270KB free, so it is a network/TLS transient,
// not memory pressure. Kept because they are what settled it, and because the
// same instrumentation is what tells you the retry is still earning its place.
static uint32_t  tagHeapBefore   = 0;     // free heap entering the fetch
static uint32_t  tagHeapAfter    = 0;     // free heap once the handshake settled
static uint8_t   tagFetchTries   = 0;     // attempts the last fetch took (0 = never ran)
static bool      tagFetchOk      = false; // whether it eventually succeeded

// Usage history: a ring buffer of the headline utilization, one sample every
// SAMPLE_INTERVAL_MS, persisted to flash hourly so it survives reboots.
static const int HIST_LEN = 60;
static uint8_t   histBuf[HIST_LEN];
static int       histCount = 0;       // valid samples so far (<= HIST_LEN)
static int       histHead  = 0;       // ring write index
static const unsigned long SAMPLE_INTERVAL_MS = 10UL * 60UL * 1000UL;  // 10 min

// 10pm-7am local (once NTP has synced). Shared by night-dim and the kitsune.
static bool nightNow() {
  time_t now = time(nullptr);
  if (!timeSynced || now < 100000) return false;
  struct tm t;
  localtime_r(&now, &t);
  return t.tm_hour >= 22 || t.tm_hour < 7;
}

// Effective brightness = 0 if screen is off, capped to NIGHT_LEVEL overnight,
// else the user's setting. Keeps the daytime preference intact.
//
// The IPS panel dims a backlight LED behind the glass; the AMOLED is
// self-emissive and has no such pin, so the same number goes to the panel
// itself as a command. Same scale either way, so everything above this is
// unchanged -- including the settings UI and the swipe gesture.
// Set once gfx->begin() has run. On the LCD board brightness is a PWM pin that
// exists from boot, so applyBacklight() could be called whenever; on the AMOLED
// it is a command down the QSPI bus, and calling it early dereferences a bus
// that has not been constructed yet. That crashed on the first boot of the
// first AMOLED board -- a hard panic loop, from a line that had been correct on
// the other panel for months.
static bool displayReady = false;

static void applyBacklight() {
  uint8_t eff = backlight;
  if (nightDim && nightNow() && eff > NIGHT_LEVEL) eff = NIGHT_LEVEL;
  if (screenOff) eff = 0;
#if PANEL_HAS_BACKLIGHT
  ledcWrite(BL_CHANNEL, eff);
#else
  if (!displayReady) return;         // the bus that carries it isn't up yet
  static_cast<Arduino_CO5300 *>(gfx)->setBrightness(eff);
#endif
}

static void setBacklight(uint8_t v) {
  backlight = v;
  applyBacklight();
}

// ------------------------------------------------------------ small helpers

// Parse "YYYY-MM-DDTHH:MM:SS..." (assumed UTC) to epoch. Ignores the offset
// suffix — Anthropic reset times arrive as UTC (+00:00 / Z).
static time_t parseISO(const char *s) {
  int y, mo, d, h, mi, se;
  if (!s || sscanf(s, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se) != 6)
    return 0;
  int yy = y - (mo <= 2);
  int era = (yy >= 0 ? yy : yy - 399) / 400;
  unsigned yoe = (unsigned)(yy - era * 400);
  unsigned doy = (153u * (unsigned)(mo + (mo > 2 ? -3 : 9)) + 2u) / 5u + (unsigned)d - 1u;
  unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
  long days = (long)era * 146097L + (long)doe - 719468L;
  return (time_t)days * 86400 + h * 3600 + mi * 60 + se;
}

static void fmtCountdown(time_t resets, char *out, size_t n) {
  time_t now = time(nullptr);
  if (!resets || !timeSynced || now < 100000) { out[0] = 0; return; }
  long mins = (long)((resets - now) / 60);
  if (mins <= 0) { snprintf(out, n, "resetting..."); return; }
  long dd = mins / 1440, hh = (mins % 1440) / 60, mm = mins % 60;
  if (dd > 0)      snprintf(out, n, "resets in %ldd %ldh", dd, hh);
  else if (hh > 0) snprintf(out, n, "resets in %ldh %ldm", hh, mm);
  else             snprintf(out, n, "resets in %ldm", mm);
}

// "2h 10m" / "45m"
static void fmtDur(long mins, char *out, size_t n) {
  if (mins < 0) mins = 0;
  long h = mins / 60, m = mins % 60;
  if (h > 0) snprintf(out, n, "%ldh %ldm", h, m);
  else       snprintf(out, n, "%ldm", m);
}

// Live ticking countdown: "1:23:45" (H:MM:SS), for the Timer screen.
static void fmtClock(time_t resets, char *out, size_t n) {
  time_t now = time(nullptr);
  if (!resets || !timeSynced || now < 100000) { strlcpy(out, "--:--:--", n); return; }
  long s = (long)(resets - now);
  if (s < 0) s = 0;
  snprintf(out, n, "%ld:%02ld:%02ld", s / 3600, (s % 3600) / 60, s % 60);
}

// ------------------------------------------------------------------ drawing

// Draw text centered at (screen-width/2, y). If the string is wider than the
// screen it shrinks the text size step-by-step until it fits (down to size 1),
// so long strings clip to a smaller font instead of wrapping around the edge.
// The default GFX text-wrap is left ON only as a last-resort safety net for a
// single word that is still too wide at size 1.
// `y` and `size` are in design space; both are mapped to the real panel here,
// so callers keep reading as the 240x320 layout they were written against.
static void drawCentered(const char *text, int y, uint8_t size, uint16_t color) {
  const int MAXW = scrW - mapLen(4);        // 2px breathing room each side, scaled
  uint8_t sz = mapSz(size);
  int16_t x1, y1; uint16_t w, h;
  while (sz > 1) {
    gfx->setTextSize(sz);
    gfx->getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
    if ((int)w <= MAXW) break;
    sz--;
  }
  gfx->setTextSize(sz);
  gfx->getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
  int x = (scrW - (int)w) / 2;
  if (x < 0) x = 0;
  gfx->setCursor(x, mapY(y));
  gfx->setTextColor(color);
  gfx->print(text);
}

// Left-aligned text. Lists read as a column of labels rather than a stack of
// centred lines, so rows the eye can scan down want this instead.
static void drawLeft(const char *text, int x, int y, uint8_t size, uint16_t color) {
  gfx->setTextSize(mapSz(size));
  gfx->setCursor(mapX(x), mapY(y));
  gfx->setTextColor(color);
  gfx->print(text);
}

static void drawSplash(const char *line1, const char *line2) {
  gfx->fillScreen(C_BG);
  drawCentered("YOYU", 130, 3, C_ACC);
  if (line1) drawCentered(line1, 170, 1, C_INK);
  if (line2) drawCentered(line2, 190, 1, C_MUTED);
  char vbuf[16];
  snprintf(vbuf, sizeof(vbuf), "v%s", FW_VERSION);
  drawCentered(vbuf, 300, 1, C_MUTED);          // firmware version, bottom-center
}

// Read VBAT through the onboard divider and map to a rough Li-ion %.
//
// Boards without a documented divider get the no-battery answer rather than a
// reading off a pin that means something else there. batPct -1 is the value the
// rest of the UI already treats as "no battery", so the glyph simply never
// draws -- no second code path to keep in step.
static void readBattery() {
#if !HAS_BATTERY_ADC
  batPct = -1;
  batCharging = false;
  return;
#else
  uint32_t mv = 0;
  for (int i = 0; i < 8; i++) mv += analogReadMilliVolts(BAT_ADC_PIN);
  float v = (mv / 8) * 3.0f / 1000.0f;             // undo the divider
  if (v < 2.5f) { batPct = -1; batCharging = false; return; }  // no battery
  int pct;
  if      (v >= 4.15f) pct = 100;
  else if (v >= 3.72f) pct = 50 + (int)((v - 3.72f) / (4.15f - 3.72f) * 50);
  else if (v >= 3.49f) pct = 10 + (int)((v - 3.49f) / (3.72f - 3.49f) * 40);
  else if (v >= 3.30f) pct =  5 + (int)((v - 3.30f) / (3.49f - 3.30f) *  5);
  else                 pct = 0;
  batPct = pct > 100 ? 100 : (pct < 0 ? 0 : pct);
  batCharging = v >= 4.25f;                          // held above full = on USB
#endif
}

// Small battery glyph at (x,y) in design space; nothing drawn when no battery
// is present.
static void drawBattery(int x, int y) {
  if (batPct < 0) return;
  const int w = mapLen(24), h = mapLen(12);
  const int px = mapX(x), py = mapY(y);
  uint16_t c = batPct <= 10 ? C_CRIT : batPct <= 30 ? C_WARN : C_ACC;
  gfx->drawRect(px, py, w, h, C_MUTED);
  gfx->fillRect(px + w, py + mapLen(3), mapLen(2), h - mapLen(6), C_MUTED);  // nub
  int fw = (w - mapLen(4)) * batPct / 100;
  if (fw > 0) gfx->fillRect(px + mapLen(2), py + mapLen(2), fw, h - mapLen(4), c);
  if (batCharging) {                                 // '+' = charging / on USB
    gfx->setTextSize(mapSz(1));
    gfx->setTextColor(C_ACC);
    gfx->setCursor(px - mapLen(8), py + mapLen(3));
    gfx->print("+");
  }
}

// Small "update available" badge: an up-arrow in a filled dot. Only drawn when
// a newer release has been seen online; harmless no-op otherwise.
static void drawUpdateBadge(int cx, int cy) {
  if (!updateAvailable) return;
  const int x = mapX(cx), y = mapY(cy), r = mapLen(8);
  gfx->fillCircle(x, y, r, C_ACC);
  gfx->fillTriangle(x, y - mapLen(4), x - mapLen(4), y + mapLen(1),
                    x + mapLen(4), y + mapLen(1), C_BG);          // arrowhead
  gfx->fillRect(x - mapLen(1), y, mapLen(3), mapLen(5), C_BG);      // shaft
}

static void drawMeters() {
  gfx->fillScreen(C_BG);

  // header: clock (hidden until NTP syncs), with the plan as a caption under it
  char buf[40];
  time_t now = time(nullptr);
  if (timeSynced && now > 100000) {
    struct tm tmnow;
    localtime_r(&now, &tmnow);
    strftime(buf, sizeof(buf), clock24 ? "%H:%M" : "%I:%M %p", &tmnow);
    const char *clk = buf;
    if (!clock24 && buf[0] == '0') clk = buf + 1;   // "03:45 PM" -> "3:45 PM"
    gfx->setTextSize(mapSz(4));
    gfx->setTextColor(C_INK);
    gfx->setCursor(mapX(10), mapY(10));
    gfx->print(clk);
  } else {
    gfx->setTextSize(mapSz(2));
    gfx->setTextColor(C_MUTED);
    gfx->setCursor(mapX(10), mapY(16));
    gfx->print("--:--");
  }
  if (plan[0]) {
    snprintf(buf, sizeof(buf), "%s plan", plan);
    gfx->setTextSize(mapSz(1));
    gfx->setTextColor(C_MUTED);
    gfx->setCursor(mapX(12), mapY(46));
    gfx->print(buf);
  }

  if (nWindows == 0) {
    if (selfHosted) {
      bool err = pollStatus[0] && strcmp(pollStatus, "not paired yet") != 0;
      if (err)
        drawCentered(pollStatus, 150, 1, C_WARN);   // errors can be long: keep compact
      else
        drawCentered("Fetching usage...", 148, 2, C_MUTED);
      if (strstr(pollStatus, "re-pair")) {
        drawCentered("run the companion app on your", 176, 1, C_ACC);
        drawCentered("computer to sign the board back in", 192, 1, C_ACC);
        drawCentered("companion.py --pair", 212, 1, C_INK);
        // The step people get stuck on. Whatever signed this board out has
        // usually signed the computer out too -- they share one Claude login
        // and rotate each other's token -- so the instruction above fails with
        // its own "no login" error and the two look like separate faults.
        drawCentered("if that says no login: claude /login", 228, 1, C_MUTED);
      }
      // Always show where this board is. Auto-discovery can land on the wrong
      // device (another Yoyu on the LAN answers first), and without the
      // address there's no way to point the companion at the right one.
      if (err && WiFi.status() == WL_CONNECTED) {
        snprintf(buf, sizeof(buf), "%s:%d",
                 WiFi.localIP().toString().c_str(), API_PORT);
        drawCentered(buf, 246, 1, C_MUTED);
        drawCentered("--pi http://<that address>", 262, 1, C_MUTED);
      }
    } else if (lastPushMs == 0) {
      drawCentered("Set me up", 116, 3, C_INK);
      drawCentered("open in a browser:", 154, 1, C_MUTED);
      snprintf(buf, sizeof(buf), "%s:%d",
               WiFi.localIP().toString().c_str(), API_PORT);
      drawCentered(buf, 176, 2, C_ACC);            // big, legible IP:port
      drawCentered("or run the companion on your PC", 210, 1, C_MUTED);
    } else {
      drawCentered("No usage windows", 148, 2, C_MUTED);
    }
  }

  // meters: label / big % left / bar / countdown
  int y = 58;
  for (int i = 0; i < nWindows && i < 3; i++) {
    Window &w = windows[i];
    float left = 100.0f - w.utilization;
    if (left < 0) left = 0; if (left > 100) left = 100;
    uint16_t fill  = left <= 10 ? C_CRIT  : left <= 30 ? C_WARN  : C_ACC;
    uint16_t track = left <= 10 ? C_CRIT_T: left <= 30 ? C_WARN_T: C_ACC_T;

    gfx->setTextSize(mapSz(2));
    gfx->setTextColor(C_INK);
    gfx->setCursor(mapX(12), mapY(y));
    gfx->print(w.label);

    if (showUsed) snprintf(buf, sizeof(buf), "%d%% used", (int)(w.utilization + 0.5f));
    else          snprintf(buf, sizeof(buf), "%d%% left", (int)(left + 0.5f));
    int16_t x1, y1; uint16_t tw, th;
    gfx->setTextSize(mapSz(2));
    gfx->getTextBounds(buf, 0, 0, &x1, &y1, &tw, &th);
    gfx->setCursor(mapX(228) - (int)tw, mapY(y + 22));   // right-aligned
    gfx->print(buf);

    int barY = y + 44;
    const int barX = mapX(12), barW = mapLen(216), barH = mapLen(14);
    gfx->fillRoundRect(barX, mapY(barY), barW, barH, barH / 2, track);
    int wpx = (int)(barW * left / 100.0f);
    if (wpx < mapLen(8)) wpx = mapLen(8);
    gfx->fillRoundRect(barX, mapY(barY), wpx, barH, barH / 2, fill);

    fmtCountdown(w.resets_at, buf, sizeof(buf));
    gfx->setTextSize(mapSz(2));
    gfx->setTextColor(C_MUTED);
    gfx->setCursor(mapX(12), mapY(barY + 18));
    gfx->print(buf);

    y += 82;
  }

  // footer: how fresh the data is, plus a hint if more windows exist than fit.
  if (lastPushMs) {
    gfx->setTextSize(mapSz(1));
    gfx->setCursor(mapX(10), mapY(306));
    unsigned long age = (millis() - lastPushMs) / 1000;
    if (age > 600) {
      gfx->setTextColor(C_WARN);
      gfx->print("stale >10m");
    } else {
      gfx->setTextColor(C_MUTED);
      if (age < 90) gfx->print("updated just now");
      else { snprintf(buf, sizeof(buf), "updated %lum ago", age / 60); gfx->print(buf); }
    }
    if (nWindowsSeen > 3) {
      gfx->setTextColor(C_ACC);
      snprintf(buf, sizeof(buf), "   +%d more", nWindowsSeen - 3);
      gfx->print(buf);
    }
  }
  drawBattery(212, 305);
  drawUpdateBadge(222, 20);        // top-right (clock is on the left)
}

// Focus screen: "will I make it to reset?" — the session window big, plus a
// burn-rate projection from the board's own usage history.
static void drawFocus() {
  gfx->fillScreen(C_BG);
  if (nWindows == 0) { drawMeters(); return; }   // nothing to focus yet
  drawBattery(206, 8);
  drawUpdateBadge(16, 16);          // top-left (battery is top-right here)

  // Prefer the session window (that's what running-out-before-reset is about);
  // otherwise the most-constrained one.
  int idx = -1;
  for (int i = 0; i < nWindows; i++)
    if (!strcmp(windows[i].key, "five_hour")) idx = i;
  if (idx < 0) {
    idx = 0;
    for (int i = 1; i < nWindows; i++)
      if (windows[i].utilization > windows[idx].utilization) idx = i;
  }
  Window &w = windows[idx];
  float left = 100.0f - w.utilization;
  if (left < 0) left = 0; if (left > 100) left = 100;
  uint16_t fill = left <= 10 ? C_CRIT : left <= 30 ? C_WARN : C_ACC;

  drawCentered(w.label, 40, 3, C_INK);
  char buf[24];
  int val = showUsed ? (int)(w.utilization + 0.5f) : (int)(left + 0.5f);
  snprintf(buf, sizeof(buf), "%d%%", val);
  drawCentered(buf, 96, 8, fill);
  drawCentered(showUsed ? "used" : "left", 184, 2, C_MUTED);

  const int fbX = mapX(20), fbW = mapLen(200), fbH = mapLen(16), fbY = mapY(220);
  gfx->fillRoundRect(fbX, fbY, fbW, fbH, fbH / 2, C_ACC_T);
  int wpx = (int)(fbW * left / 100.0f);
  if (wpx < mapLen(10)) wpx = mapLen(10);
  gfx->fillRoundRect(fbX, fbY, wpx, fbH, fbH / 2, fill);

  // Projection (session window only): burn rate over the last ~hour of history
  // vs. time to reset.
  bool projected = false;
  if (!strcmp(w.key, "five_hour") && timeSynced && histCount > 6) {
    const int back = 6;                                   // 6 samples x 10 min
    int oi = (histHead - 1 - back + 2 * HIST_LEN) % HIST_LEN;
    float rate = (w.utilization - (float)histBuf[oi]) / (back * 10.0f);  // %/min
    time_t now = time(nullptr);
    long toReset = w.resets_at ? (long)((w.resets_at - now) / 60) : -1;
    char e[16], r[16], d[44];
    if (rate <= 0.03f) {                                  // barely burning
      drawCentered("holding steady", 256, 2, C_ACC);
      drawCentered("you'll reset with room to spare", 286, 1, C_MUTED);
      projected = true;
    } else {
      long toEmpty = (long)((100.0f - w.utilization) / rate);
      fmtDur(toEmpty, e, sizeof(e));
      if (toReset < 0) {
        snprintf(d, sizeof(d), "runs out in ~%s", e);
        drawCentered(d, 262, 2, toEmpty < 60 ? C_CRIT : C_WARN);
      } else if (toEmpty >= toReset) {
        fmtDur(toReset, r, sizeof(r));
        drawCentered("resets before you run out", 256, 2, C_ACC);
        snprintf(d, sizeof(d), "empty ~%s   resets ~%s", e, r);
        drawCentered(d, 286, 1, C_MUTED);
      } else {
        char gap[16]; fmtDur(toReset - toEmpty, gap, sizeof(gap));
        snprintf(d, sizeof(d), "runs dry ~%s early", gap);
        drawCentered(d, 256, 2, (toReset - toEmpty) < 60 ? C_CRIT : C_WARN);
        fmtDur(toReset, r, sizeof(r));
        snprintf(d, sizeof(d), "empty ~%s   resets ~%s", e, r);
        drawCentered(d, 286, 1, C_MUTED);
      }
      projected = true;
    }
  }
  if (!projected) {
    fmtCountdown(w.resets_at, buf, sizeof(buf));
    drawCentered(buf, 262, 2, C_MUTED);
  }
}

// Headline metric to trend: the session window if present, else the fullest.
// Float version (for exact trend detection) and a rounded one (for display).
static float headlineUtilF() {
  float best = -1;
  for (int i = 0; i < nWindows; i++) {
    if (!strcmp(windows[i].key, "five_hour")) return windows[i].utilization;
    if (windows[i].utilization > best) best = windows[i].utilization;
  }
  return best;   // -1 if no data yet
}
static int headlineUtil() {
  float f = headlineUtilF();
  return f < 0 ? -1 : (int)(f + 0.5f);
}

// The window with the least headroom left -- the one that actually stops you
// working, and so the one the mascot reacts to. Deliberately separate from
// headlineUtilF(), which prefers the 5-hour window because it feeds the history
// graph: that has to stay on one series over time or the graph would jump every
// time a different window became the fullest.
static int tightestWindow() {
  int idx = -1;
  for (int i = 0; i < nWindows; i++)
    if (idx < 0 || windows[i].utilization > windows[idx].utilization) idx = i;
  return idx;
}

// % used of that window, or -1 with no data.
static int mascotUtil() {
  int i = tightestWindow();
  return i < 0 ? -1 : (int)(windows[i].utilization + 0.5f);
}

// A window reads as "out" when its rounded percentage reaches 100% used — the
// same rounding the meters print and the kitsune's "out of tokens" mood uses, so
// the three can never disagree about whether you've run out.
static bool windowOut(const Window &w) {
  return (int)(w.utilization + 0.5f) >= 100;
}

static void sampleHistory() {
  int u = headlineUtil();
  if (u < 0) return;
  histBuf[histHead] = (uint8_t)(u < 0 ? 0 : (u > 100 ? 100 : u));
  histHead = (histHead + 1) % HIST_LEN;
  if (histCount < HIST_LEN) histCount++;
}

static void saveHistory() {
  prefs.begin("headroom", false);
  prefs.putBytes("hist", histBuf, HIST_LEN);
  prefs.putInt("histc", histCount);
  prefs.putInt("histh", histHead);
  prefs.end();
}

static void loadHistory() {
  prefs.begin("headroom", true);
  size_t n = prefs.getBytes("hist", histBuf, HIST_LEN);
  if (n == HIST_LEN) {
    histCount = prefs.getInt("histc", 0);
    histHead  = prefs.getInt("histh", 0);
    // Clamp against a corrupted/hostile NVS so indexing stays in bounds.
    if (histHead < 0 || histHead >= HIST_LEN) histHead = 0;
    if (histCount < 0) histCount = 0;
    if (histCount > HIST_LEN) histCount = HIST_LEN;
  } else {
    memset(histBuf, 0, HIST_LEN);
    histCount = histHead = 0;
  }
  prefs.end();
}

// Bar-graph of the session usage over the last ~10 hours.
static void drawHistory() {
  gfx->fillScreen(C_BG);
  drawCentered("History", 34, 3, C_INK);
  drawCentered("session usage over time", 74, 1, C_MUTED);
  drawBattery(206, 8);
  drawUpdateBadge(16, 16);          // top-left (battery is top-right here)
  if (histCount == 0) {
    drawCentered("collecting...", 150, 1, C_MUTED);
    return;
  }
  const int gx = mapX(16), gy = mapY(104), gw = mapLen(208), gh = mapLen(150);
  gfx->drawFastHLine(gx, gy + gh, gw, C_MUTED);          // baseline (0%)
  gfx->drawFastHLine(gx, gy, gw, C_ACC_T);               // 100% guide
  float bw = (float)gw / HIST_LEN;
  int cur = 0, peak = 0;
  for (int i = 0; i < histCount; i++) {
    int idx = (histHead - histCount + i + 2 * HIST_LEN) % HIST_LEN;
    int v = histBuf[idx];
    if (i == histCount - 1) cur = v;
    if (v > peak) peak = v;
    int bh = gh * v / 100;
    int x = gx + (int)((HIST_LEN - histCount + i) * bw);
    int left = 100 - v;
    uint16_t c = left <= 10 ? C_CRIT : left <= 30 ? C_WARN : C_ACC;
    if (bh > 0) gfx->fillRect(x, gy + gh - bh, (int)bw + 1, bh, c);
  }
  char buf[32];
  snprintf(buf, sizeof(buf), "now %d%%   peak %d%%", cur, peak);
  drawCentered(buf, gy + gh + 16, 2, C_INK);
}

// --- the kitsune's mood is driven by activity + headroom ---
// You're "active" if usage has climbed recently (you're burning tokens now).
static bool mascotActive() {
  return lastActivityMs && (millis() - lastActivityMs) < ACTIVE_WINDOW_MS;
}

// dead when out, panic when low, party while you're actively using it, asleep
// when idle, and a waiting face before any data arrives.
static int mascotMoodNow() {
  int u = mascotUtil();
  if (u < 0) return 4;                    // 4 = waiting
  int left = 100 - u;
  if (left <= 0)  return 5;               // 5 = dead / out
  if (left <= 20) return 2;               // 2 = panic
  return mascotActive() ? 6 : 3;          // 6 = party, 3 = asleep
}

// Called after each usage update: a rise in utilization means tokens are being
// spent right now, so the mascot wakes up and parties.
static void noteUsageActivity() {
  float u = headlineUtilF();
  if (u >= 0) {
    // Any real upward move (even a fraction of a percent) means you're active.
    if (prevHeadlineUtil >= 0 && u > prevHeadlineUtil + 0.01f) lastActivityMs = millis();
    prevHeadlineUtil = u;
  }
}

// How many tails to show. The kitsune is a gauge, not decoration, so this is
// the headline window's remaining share bucketed into three states -- the same
// number the meters print, said in a way you can read from across a desk.
static int kitsuneTails() {
  int u = mascotUtil();                   // % used, -1 when there's no data yet
  if (u < 0) return 2;                    // unknown: sit in the middle
  int left = 100 - u;
  return left > 60 ? 3 : left > 25 ? 2 : 1;
}

// Draw the animated kitsune for `mood` at animation `frame`. Repaints only the
// band above the caption, so the caption/version/badge are left intact and the
// per-frame tick stays cheap. Each mood has its own motion.
// The cell size, fitted to the glass. Scale the cell once and centre the grid:
// mapping each of the ~200 fillRects individually would round each separately
// and pull the pixel art out of alignment with itself.
//
// Fitted to the panel width rather than multiplied by uiScale, which is the
// bitmap-font step. The two agreed while the sprite was 11 cells wide; at 18
// they don't. 18 x 13 x uiScale 2 is 468px of sprite on a 410px AMOLED, which
// centres to a negative x and clips both flanks, and pushes the stat line under
// the caption off the bottom of the screen.
// Both axes, because either can be the tight one: a tall panel runs out of
// width first, a wide one runs out of height. The vertical budget is design-row
// 40 (where the sprite starts) to 258 -- far enough up that the caption at +12
// and the two stat lines below it still land on the glass.
static int kitsuneCell() {
  int byW = (mapLen(REF_W) - mapLen(6)) / K_COLS;    // 6px of breathing room, scaled
  int byH = mapLen(218) / K_ROWS;
  int s = byW < byH ? byW : byH;
  return s < 1 ? 1 : s;                   // identity (13) on the 240px panel
}

static void drawKitsuneAnim(int mood, int frame) {
  // Centred within the design area, not the glass: on a letterboxed panel the
  // caption below it is, so centring on scrW would visibly misalign the two.
  const int S = kitsuneCell();
  const int ox = offX + (mapLen(REF_W) - K_COLS * S) / 2, oy0 = mapY(40);

  int bob = 0, shake = 0;
  if      (mood == 6) bob   = (frame % 4 < 2) ? -4 : 0;   // party: bounce
  else if (mood == 3) bob   = (frame % 8 < 4) ? 0 : 3;    // sleep: slow breathing
  else if (mood == 2) shake = (frame % 2) ? 3 : -3;       // panic: jitter
  else if (mood == 5) bob   = 4;                          // dead: slumped
  int oy = oy0 + bob, px = ox + shake;

  // The ear interiors are the fox's own shade unless the mood is worth
  // shouting about; a permanently tinted ear just reads as a sticker.
  uint16_t ear = C_SPRK_D;
  if      (mood == 2) ear = C_CRIT;
  else if (mood == 5) ear = C_MUTED;
  else if (mood == 6) ear = (frame % 3 == 0) ? C_ACC
                          : (frame % 3 == 1) ? C_WARN : C_SPRK;

  // Tails first: the body is opaque and sits in front of where they root, so
  // painting them under it hides the join instead of drawing a seam.
  int tails = kitsuneTails();
  for (int t = 0; t < tails; t++)
    for (int i = 0; i < KITSUNE_TAIL_N[t]; i++) {
      const TailCell &tc = KITSUNE_TAILS[t][i];
      uint16_t c = tc.ch == 'K' ? C_OUT : tc.ch == 'W' ? C_FACE
                 : tc.ch == 'S' ? C_SPRK_D : C_BG;
      gfx->fillRect(px + tc.c * S, oy + tc.r * S, S, S, c);
    }

  for (int y = 0; y < K_ROWS; y++)
    for (int x = 0; x < K_COLS; x++) {
      uint16_t c;
      switch (KITSUNE_SPRITE[y][x]) {
        case 'K': c = C_OUT;    break;
        case 'W': c = C_FACE;   break;
        case 'B': c = C_SPRK;   break;
        case 'S': c = C_SPRK_D; break;
        default:  continue;
      }
      gfx->fillRect(px + x * S, oy + y * S, S, S, c);
    }
  gfx->fillRect(px + 2 * S, oy + 3 * S, S, S, ear);
  gfx->fillRect(px + 8 * S, oy + 3 * S, S, S, ear);

  // Eyes on row 6, muzzle on row 9 (the white markings). Cols 2 and 7 put the
  // pair either side of the head's centre line at col 5.
  int ey = oy + 6 * S, lx = px + 2 * S, rx = px + 7 * S;
  int my = oy + 9 * S + S / 2, mx = px + 4 * S;
  if (mood != 5) gfx->fillRect(px + 5 * S, oy + 9 * S, S, S / 2, C_OUT);   // nose

  if (mood == 3) {                                 // ASLEEP: shut eyes + rising Zzz
    gfx->fillRect(lx, ey + S / 2, S, S / 4, C_OUT);
    gfx->fillRect(rx, ey + S / 2, S, S / 4, C_OUT);
    gfx->fillRect(mx + S, my, S, S / 4, C_OUT);
    int n = 1 + (frame % 3);                       // z .. z z .. z z z ..
    gfx->setTextColor(C_MUTED);
    gfx->setTextSize(uiScale);
    for (int i = 0; i < n; i++) {                  // above the head: the space
      gfx->setCursor(px + (4 + i) * S, oy + S - i * 9);   // beside it is tails
      gfx->print("z");
    }
  } else if (mood == 6) {                          // PARTY: happy arcs + open grin
    // Curved-up eyes, not blocks. A square eye over a wide mouth reads as a
    // stare rather than a smile, which is how the previous grin went wrong.
    for (int e = 0; e < 2; e++) {
      int bx = e ? rx : lx;
      gfx->fillRect(bx,             ey + S / 2, S / 3, S / 4, C_OUT);
      gfx->fillRect(bx + S / 3,     ey + S / 5, S / 3, S / 4, C_OUT);
      gfx->fillRect(bx + 2 * S / 3, ey + S / 2, S / 3, S / 4, C_OUT);
    }
    gfx->fillRect(mx, my, 3 * S, S / 2, C_OUT);
    gfx->fillRect(mx + S, my + S / 5, S, S / 5, C_CRIT);           // tongue
    // Design-space coords like everything else -- raw pixels here bunched the
    // confetti into the top-left corner of a larger panel. The 200 keeps the
    // lowest piece above the caption instead of clipping its top row.
    for (int i = 0; i < 9; i++) {
      int cxp = mapX(12 + (i * 71) % 214);
      int cyp = mapY(30 + ((i * 37 + frame * 12) % 200));
      uint16_t cc = (i % 3 == 0) ? C_ACC : (i % 3 == 1) ? C_WARN : C_SPRK;
      gfx->fillRect(cxp, cyp, mapLen(5), mapLen(5), cc);
    }
  } else if (mood == 2) {                          // PANIC: wide eyes + dripping sweat
    gfx->fillRect(lx, ey - S / 4, S, S + S / 4, C_FACE);
    gfx->fillRect(rx, ey - S / 4, S, S + S / 4, C_FACE);
    gfx->fillRect(lx + S / 3, ey + S / 4, S / 3, S / 3, C_OUT);
    gfx->fillRect(rx + S / 3, ey + S / 4, S / 3, S / 3, C_OUT);
    gfx->fillRect(mx + S, my - S / 4, S, S / 2, C_OUT);
    int dy = (frame % 4) * 7;
    // Sweat has to stay pale: on a red fox a coloured droplet reads as blood.
    gfx->fillRect(rx + S + S / 4, ey - S / 3 + dy, S / 3, S / 2, C_FACE);
  } else if (mood == 5) {                          // DEAD: X eyes, KO mouth (still)
    for (int t = -1; t <= 1; t++) {
      gfx->drawLine(lx, ey + t, lx + S - 1, ey + S - 1 + t, C_OUT);
      gfx->drawLine(lx + S - 1, ey + t, lx, ey + S - 1 + t, C_OUT);
      gfx->drawLine(rx, ey + t, rx + S - 1, ey + S - 1 + t, C_OUT);
      gfx->drawLine(rx + S - 1, ey + t, rx, ey + S - 1 + t, C_OUT);
    }
    gfx->fillRect(mx + S / 2, my, 2 * S, S / 5, C_OUT);
  } else {                                         // WAITING: narrow fox eyes
    // Stepped, not square: the slant is most of what separates a fox from a
    // round-eyed cartoon animal at this size, and it costs two rects an eye.
    for (int e = 0; e < 2; e++) {
      int bx = e ? rx : lx, d = e ? -1 : 1;
      gfx->fillRect(bx, ey + S / 3, 2 * S / 3, S / 3, C_OUT);
      gfx->fillRect(bx + (d > 0 ? S / 2 : -S / 6), ey + S / 8, 2 * S / 3, S / 3, C_OUT);
    }
    gfx->fillRect(mx + S / 2, my, 2 * S, S / 5, C_OUT);
  }
}

// Full kitsune screen, rendered into the off-screen buffer and blitted in one
// pass (no flicker). Draw helpers all use the global `gfx`, so we point it at
// the RAM canvas for the duration, then flush. Every animation frame is a full
// redraw of this whole screen into the buffer.
static void drawMascot() {
  if (!mascotBuf) {                  // lazily allocate the framebuffer (PSRAM)
    mascotBuf = new Arduino_Canvas(scrW, scrH, gfx);
    mascotBuf->begin(GFX_SKIP_OUTPUT_BEGIN);          // display is already begun
    if (!mascotBuf->getFramebuffer()) { delete mascotBuf; mascotBuf = nullptr; }
  }
  Arduino_GFX *real = gfx;
  if (mascotBuf) gfx = mascotBuf;    // route all drawing to the off-screen buffer

  gfx->fillScreen(C_BG);
  drawUpdateBadge(222, 20);          // top-right (no battery on this screen)
  char vbuf[16];
  snprintf(vbuf, sizeof(vbuf), "v%s", FW_VERSION);
  gfx->setTextSize(mapSz(1));
  gfx->setTextColor(C_MUTED);
  gfx->setCursor(mapX(8), mapY(12));   // firmware version, top-left
  gfx->print(vbuf);

  int mood = mascotMoodNow();
  mascotShownMood = mood;
  uint16_t mc = (mood == 6) ? C_ACC : (mood == 2 || mood == 5) ? C_CRIT : C_MUTED;
  const char *word = mood == 6 ? "on a roll!"
                   : mood == 2 ? "running low!"
                   : mood == 5 ? "out of tokens"
                   : mood == 3 ? "resting - no usage" : "waiting for usage";
  // Caption sits under the sprite, so it follows the scaled cell size rather
  // than a design-space constant -- otherwise it overlaps on a larger panel.
  const int S = kitsuneCell();
  // Back to design space: subtract the letterbox before dividing by the
  // scale, or the margin gets counted as part of the sprite's height.
  int cy = ((mapY(40) - offY) + K_ROWS * S + mapLen(12)) * 256 / mapQ;
  drawCentered(word, cy, 2, mc);

  // Stat line: the same window the mascot is reacting to, plus its countdown.
  // These used to be picked separately -- the mascot went by the 5-hour window
  // and this line by the fullest -- so the screen could read "on a roll!" over
  // three tails with "Weekly 96% used" printed directly beneath it.
  int idx = tightestWindow();
  if (idx >= 0) {
    int u = (int)(windows[idx].utilization + 0.5f);
    int shown = showUsed ? u : 100 - u;
    char buf[40];
    snprintf(buf, sizeof(buf), "%s %d%% %s", windows[idx].label, shown,
             showUsed ? "used" : "left");
    drawCentered(buf, cy + 26, 1, C_MUTED);
    fmtCountdown(windows[idx].resets_at, buf, sizeof(buf));
    if (buf[0]) drawCentered(buf, cy + 42, 1, C_MUTED);
  }
  drawKitsuneAnim(mood, mascotFrame);

  if (mascotBuf) { gfx = real; mascotBuf->flush(); }   // blit the finished frame
}

// Timer screen: one big countdown to the soonest reset, ticking every second.
// The clock digits live in a fixed band so the per-second tick can redraw just
// that strip (drawTimerClock) instead of the whole screen.
static void drawTimerClock() {
  char b[16];
  fmtClock(timerResetAt, b, sizeof(b));
  long s = (timerResetAt && timeSynced) ? (long)(timerResetAt - time(nullptr)) : -1;
  // The ladder below is about how soon the reset lands, which only reads right
  // while you still have headroom. Once you're out it inverts: a two-day wait
  // would wear terracotta, the same colour as a full tank, and the last five
  // minutes of the wait would go crimson exactly as the news turns good. So the
  // digits go neutral — the crimson label above has already said you're out,
  // and all these have left to do is be readable.
  uint16_t c = s < 0     ? C_MUTED
             : timerOut  ? C_INK
             : s < 300   ? C_CRIT
             : s < 1800  ? C_WARN
                         : C_ACC;
  gfx->fillRect(0, mapY(150), scrW, mapLen(52), C_BG);   // clear the digits band
  drawCentered(b, 156, 4, c);
}

static void drawTimer() {
  gfx->fillScreen(C_BG);
  drawUpdateBadge(222, 20);
  // Soonest upcoming reset — except that an exhausted window wins outright.
  // When you're out of Weekly, an untouched session resetting in two hours is
  // not the number you're waiting on; pass 0 looks only at windows you've run
  // out of, and pass 1 runs at all only if none have.
  int idx = -1;
  for (int pass = 0; pass < 2 && idx < 0; pass++)
    for (int i = 0; i < nWindows; i++) {
      if (!windows[i].resets_at) continue;
      if (pass == 0 && !windowOut(windows[i])) continue;
      if (idx < 0 || windows[i].resets_at < windows[idx].resets_at) idx = i;
    }
  if (idx < 0) {
    timerResetAt = 0;
    timerOut = false;
    drawCentered("Countdown", 60, 3, C_INK);
    drawCentered("waiting for usage data", 156, 2, C_MUTED);
    return;
  }
  timerResetAt = windows[idx].resets_at;
  // "back in" rather than "resets in" when you're out: the countdown has
  // stopped being a fact about the window and started being your wait.
  timerOut = windowOut(windows[idx]);
  drawCentered(windows[idx].label, 78, 2, timerOut ? C_CRIT : C_MUTED);
  drawCentered(timerOut ? "back in" : "resets in", 112, 3, C_INK);
  drawTimerClock();
  drawCentered("H : MM : SS", 214, 1, C_MUTED);
}

// ---- Actions screen ------------------------------------------------------

// Queue the selected shortcut for the companion to pick up. Oldest is dropped
// if nobody is collecting, so the queue can't grow unbounded.
static void queueAction(const char *id) {
  if (actionQN >= ACTION_Q_MAX) {
    for (int i = 1; i < ACTION_Q_MAX; i++) {
      actionQ[i - 1] = actionQ[i];
      actionQAt[i - 1] = actionQAt[i];
    }
    actionQN = ACTION_Q_MAX - 1;
  }
  actionQ[actionQN] = id;
  actionQAt[actionQN] = millis();
  actionQN++;
}

// Drop presses the companion never collected in time.
static void expireActions() {
  int keep = 0;
  for (int i = 0; i < actionQN; i++)
    if (millis() - actionQAt[i] < ACTION_TTL_MS) {
      actionQ[keep] = actionQ[i];
      actionQAt[keep] = actionQAt[i];
      keep++;
    }
  actionQN = keep;
}

// True while a companion is actively polling, so the screen can say whether a
// press will land anywhere.
static bool actionsListening() {
  return lastActionPollMs && (millis() - lastActionPollMs) < 10000;
}

static void drawActions() {
  gfx->fillScreen(C_BG);
  drawUpdateBadge(222, 20);
  drawCentered("Actions", 34, 3, C_INK);
  drawCentered("tap to send to Claude Code", 72, 1, C_MUTED);

  int y = 108;
  for (int i = 0; i < N_ACTIONS; i++) {
    bool sel = (i == actionSel);
    if (sel) gfx->fillRoundRect(mapX(14), mapY(y - 8), mapLen(212), mapLen(46),
                                mapLen(10), C_ACC_T);
    drawCentered(ACTIONS[i].label, y, 2, sel ? C_ACC : C_MUTED);
    drawCentered(ACTIONS[i].keys, y + 22, 1, C_MUTED);
    y += 56;
  }

  drawCentered("swipe up/down to choose", 288, 1, C_MUTED);
  if (actionsListening())
    drawCentered("companion connected", 304, 1, C_ACC);
  else
    drawCentered("run companion with --actions", 304, 1, C_WARN);
}

// ---- Projects screen -----------------------------------------------------

static void drawProjects() {
  gfx->fillScreen(C_BG);
  drawUpdateBadge(222, 20);
  drawCentered("Top projects", 30, 2, C_INK);

  // Say the scope every time. These shares come from one computer's session
  // logs, so they answer "what did I spend it on here" — not "what did my
  // account spend". Left unlabelled, a missing project reads as a bug rather
  // than as work done somewhere this board can't see.
  char cap[40];
  if (projWindow[0]) snprintf(cap, sizeof(cap), "this computer - last %s", projWindow);
  else               strlcpy(cap, "this computer", sizeof(cap));
  drawCentered(cap, 54, 1, C_MUTED);

  if (nProjects == 0) {
    // Empty means two different things and they need different advice. Once a
    // push has landed (projAt set), the companion demonstrably *is* running and
    // simply had a quiet window — telling the user to start it would contradict
    // the caption directly above, which says it just reported.
    if (projAt) {
      char quiet[36];
      if (projWindow[0]) snprintf(quiet, sizeof(quiet), "no activity in the last %s", projWindow);
      else               strlcpy(quiet, "no activity yet", sizeof(quiet));
      drawCentered("all quiet", 150, 2, C_MUTED);
      drawCentered(quiet, 182, 1, C_MUTED);
    } else {
      drawCentered("nothing logged yet", 150, 2, C_MUTED);
      drawCentered("needs the companion running", 182, 1, C_MUTED);
    }
    return;
  }

  int y = 88;
  for (int i = 0; i < nProjects; i++) {
    uint16_t c = (i == 0) ? C_ACC : C_INK;   // the biggest spender stands out
    drawLeft(projects[i].name, 14, y, 1, c);

    char pct[8];
    snprintf(pct, sizeof(pct), "%d%%", (int)(projects[i].share + 0.5f));
    int16_t x1, y1; uint16_t w, h;
    gfx->setTextSize(mapSz(1));
    gfx->getTextBounds(pct, 0, 0, &x1, &y1, &w, &h);
    // Right-aligned. Positioned with the raw cursor, not drawLeft: the width
    // from getTextBounds is in real pixels, and drawLeft maps its x from
    // design space, so subtracting one from the other mixes the two.
    gfx->setTextColor(c);
    gfx->setCursor(mapX(226) - (int)w, mapY(y));
    gfx->print(pct);

    const int barX = mapX(14), barW = mapLen(212), barH = mapLen(6);
    int bw = (int)(barW * projects[i].share / 100.0f + 0.5f);
    if (bw < mapLen(2)) bw = mapLen(2);          // a sliver still reads as "some"
    gfx->fillRect(barX, mapY(y + 12), barW, barH, C_ACC_T);
    gfx->fillRect(barX, mapY(y + 12), bw, barH, c);
    y += 34;
  }

  // The shares are of the whole window, so with anything hidden they stop
  // short of 100%. Saying how many are missing keeps that from reading as a
  // rounding bug — the same reason the meters carry "+N more".
  if (projMore > 0) {
    char more[24];
    snprintf(more, sizeof(more), "+%d more", projMore);
    drawCentered(more, y + 2, 1, C_MUTED);
  }
}

// ---- Settings screen -----------------------------------------------------

static int enabledScreenCount() {
  return __builtin_popcount(screenMask & ((1 << UI_SCREENS) - 1));
}

// Refuse the three toggles that would strand you: the last screens standing,
// the power-on default, and this screen itself. The web form at /settings can
// still do all three — this guard is about not letting a stray tap on a 2"
// display hide the only thing that tells you where that form lives.
static bool toggleScreenAt(int i) {
  const char *why = nullptr;
  // Guard the *disable* direction only. Blocking both ways made a Settings row
  // that the web form had switched off impossible to switch back on: you would
  // tap the unchecked box and be told it stays on, while it stayed off.
  if (i == SCREEN_SETTINGS && screenEnabled(i)) why = "Settings stays on";
  else if (i == defaultScreen)           why = "that's the default";
  else if (screenEnabled(i) && enabledScreenCount() <= 2)
                                         why = "keep at least two";
  if (why) {
    strlcpy(settingMsg, why, sizeof(settingMsg));
    settingMsgAt = millis();
    return false;
  }
  screenMask ^= (1 << i);
  prefs.begin("headroom", false);
  prefs.putUChar("smask", screenMask);
  prefs.end();
  settingMsg[0] = 0;
  return true;
}

static void drawSettings() {
  gfx->fillScreen(C_BG);
  drawCentered("Settings", 26, 2, C_INK);

  // The whole reason this screen exists: a working board used to show its
  // address nowhere, so there was no route from the device to its own config.
  if (WiFi.status() == WL_CONNECTED) {
    // The address is the whole point of this screen, so it gets the largest
    // type on it — it was previously the same size as the title above it,
    // which read as small on the bench. Splitting the port off is what buys
    // the room: "ip:8080" at size 3 is past drawCentered's 236px ceiling for
    // any real address, and on a longer LAN prefix it exceeded it even at
    // size 2, which silently stepped the font down to caption size. The port
    // never changes, so it loses nothing by moving to the line below.
    char ip[24];
    strlcpy(ip, WiFi.localIP().toString().c_str(), sizeof(ip));
    drawCentered(ip, 46, 3, C_ACC);
    char hint[40];
    snprintf(hint, sizeof(hint), "port %d - open in a browser", API_PORT);
    drawCentered(hint, 76, 1, C_MUTED);
  } else {
    drawCentered("offline", 50, 2, C_MUTED);
    drawCentered("no address until Wi-Fi joins", 76, 1, C_MUTED);
  }
  char ver[16];
  snprintf(ver, sizeof(ver), "v%s", FW_VERSION);
  drawCentered(ver, 90, 1, C_MUTED);

  gfx->drawFastHLine(mapX(14), mapY(106), mapLen(212), C_ACC_T);
  drawLeft("Screens in rotation", 14, 114, 1, C_MUTED);

  int y = 132;
  for (int i = 0; i < UI_SCREENS; i++) {
    bool sel = (i == settingSel);
    bool on  = screenEnabled(i);
    if (sel) gfx->fillRoundRect(mapX(10), mapY(y - 4), mapLen(220), mapLen(20),
                                mapLen(5), C_ACC_T);
    drawLeft(on ? "[x]" : "[ ]", 16, y, 1, on ? C_ACC : C_MUTED);
    drawLeft(SCREEN_NAMES[i], 46, y, 1, on ? C_INK : C_MUTED);
    y += 21;
  }

  // A refusal has to say why, or the tap just looks broken. It fades so the
  // footer goes back to explaining the controls.
  if (settingMsg[0] && millis() - settingMsgAt < 2500)
    drawCentered(settingMsg, 304, 1, C_WARN);
  else
    drawCentered("tap toggles - swipe L/R exits", 304, 1, C_MUTED);
}

// Draw whichever screen is active (data updates / ticks call this).
static bool pairingActive();   // defined with the pairing handlers below
static void drawPairScreen();
static void wake();            // defined with the motion handling below

static void drawScreen() {
  // While pairing, the one-time code owns the screen — any full redraw
  // (30s tick, auto-rotate, a tap) must not paint the normal UI over it.
  if (pairingActive()) { drawPairScreen(); return; }
  if (uiScreen == 1)      drawFocus();
  else if (uiScreen == 2) drawHistory();
  else if (uiScreen == 3) drawMascot();
  else if (uiScreen == SCREEN_TIMER)    drawTimer();
  else if (uiScreen == SCREEN_ACTIONS)  drawActions();
  else if (uiScreen == SCREEN_PROJECTS) drawProjects();
  else if (uiScreen == SCREEN_SETTINGS) drawSettings();
  else                                  drawMeters();
}

// ---- running out puts the countdown up by itself --------------------------
// The moment you run out is the one moment the only useful number on the desk
// is how long until you can work again, and it's also the moment you're least
// likely to go tapping through screens to find it. So the board switches to
// the countdown on its own.
//
// It does this once per episode. Tap away and it stays away: `exhaustEpisode`
// holds the reset time of the window that triggered the switch, so the board
// won't force the screen again until that window resets and is spent afresh.
// Being out of two windows at once is still one episode, not two.
static time_t exhaustEpisode = 0;

static void checkExhaustion() {
  int idx = -1;
  for (int i = 0; i < nWindows; i++)
    if (windowOut(windows[i])) { idx = i; break; }

  if (idx < 0) { exhaustEpisode = 0; return; }   // recovered -> re-arm

  // Without a reset time there is nothing to count down to, so leave the
  // screen alone rather than switching to a countdown that can't count.
  time_t ep = windows[idx].resets_at;
  if (!ep || ep == exhaustEpisode) return;
  exhaustEpisode = ep;                           // claim it either way

  if (uiScreen == SCREEN_TIMER) return;          // already showing
  if (!screenEnabled(SCREEN_TIMER)) return;      // Timer taken out of the rotation
  if (pairingActive()) return;                   // the one-time code owns the screen
  uiScreen = SCREEN_TIMER;
}

// ------------------------------------------------------------ phone alerts
// POST to ntfy (and/or Pushover) when a window crosses a threshold. The board
// has no speaker, but a phone push reaches you anywhere. Edge-triggered with a
// recovery notice, so you get one "low" and one "recovered" per window.

static String ntfyTopic, poToken, poUser;
static int    alertPct = 90;              // notify at/above this % used
struct AlertState { char key[24]; bool over; };
static AlertState alertStates[MAX_WINDOWS] = {};

static bool alertsConfigured() {
  return ntfyTopic.length() > 0 || (poToken.length() > 0 && poUser.length() > 0);
}

static String urlEncode(const String &s) {
  String o;
  char b[4];
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~')
      o += c;
    else if (c == ' ') o += "%20";
    else { snprintf(b, sizeof(b), "%%%02X", (unsigned char)c); o += b; }
  }
  return o;
}

static void sendNtfy(const char *title, const char *body) {
  if (ntfyTopic.length() == 0) return;
  WiFiClientSecure client;
  tlsTrust(client);
  HTTPClient h;
  if (!h.begin(client, "https://ntfy.sh/" + ntfyTopic)) return;
  h.addHeader("Title", title);
  h.addHeader("Content-Type", "text/plain");
  h.POST(String(body));
  h.end();
}

static void sendPushover(const char *title, const char *body) {
  if (poToken.length() == 0 || poUser.length() == 0) return;
  WiFiClientSecure client;
  tlsTrust(client);
  HTTPClient h;
  if (!h.begin(client, "https://api.pushover.net/1/messages.json")) return;
  h.addHeader("Content-Type", "application/x-www-form-urlencoded");
  String form = "token=" + urlEncode(poToken) + "&user=" + urlEncode(poUser) +
                "&title=" + urlEncode(title) + "&message=" + urlEncode(body);
  h.POST(form);
  h.end();
}

static void sendAlert(const char *title, const char *body) {
  if (WiFi.status() != WL_CONNECTED) return;
  sendNtfy(title, body);
  sendPushover(title, body);
}

// Edge-triggered per window: fire once on crossing up, once on recovery.
static void checkAlerts() {
  if (!alertsConfigured()) return;
  for (int i = 0; i < nWindows; i++) {
    Window &w = windows[i];
    int used = (int)(w.utilization + 0.5f);
    AlertState *st = nullptr;
    for (AlertState &a : alertStates)
      if (a.key[0] && !strcmp(a.key, w.key)) { st = &a; break; }
    if (!st)
      for (AlertState &a : alertStates)
        if (!a.key[0]) { strlcpy(a.key, w.key, sizeof(a.key)); a.over = false; st = &a; break; }
    if (!st) continue;
    char body[80];
    if (used >= alertPct && !st->over) {
      st->over = true;
      snprintf(body, sizeof(body), "%s at %d%% used", w.label, used);
      sendAlert("Yoyu", body);
    } else if (used < alertPct - 10 && st->over) {
      st->over = false;
      snprintf(body, sizeof(body), "%s recovered (%d%% used)", w.label, used);
      sendAlert("Yoyu", body);
    }
  }
}

static void saveAlerts() {
  prefs.begin("headroom", false);
  prefs.putString("ntfy", ntfyTopic);
  prefs.putString("potok", poToken);
  prefs.putString("pouser", poUser);
  prefs.putInt("alpct", alertPct);
  prefs.end();
}

// ------------------------------------------------------------------ web api

static void sendJson(int code, const String &body) {
  server->send(code, "application/json", body);
}

// Constant-time compare so a token can't be guessed a character at a time.
static bool ctEqual(const String &a, const String &b) {
  if (a.length() != b.length()) return false;
  uint8_t d = 0;
  for (size_t i = 0; i < a.length(); i++) d |= (uint8_t)a[i] ^ (uint8_t)b[i];
  return d == 0;
}

// Shared-secret gate for every state-changing endpoint. Open when no push token
// is set (zero-config default); once the owner sets one in /settings, callers
// must prove it. Machines (the companion) send the X-Push-Token header; browser
// forms send a matching `token` field. Because the token itself can only be
// changed through this gate, an unauthenticated request can no longer clear it.
static bool apiAuthed() {
  if (pushToken.length() == 0) return true;
  if (ctEqual(server->header("X-Push-Token"), pushToken)) return true;
  if (server->hasArg("token") && ctEqual(server->arg("token"), pushToken)) return true;
  return false;
}

// 401 helper for the browser (form) endpoints.
static void denyUnauthed() {
  server->send(401, "text/html",
               "<h2>Locked</h2><p>This board has a device token set. Add it in "
               "the <b>device token</b> field to make changes. "
               "<a href=/>back</a></p>");
}

// Minimal HTML-attribute escaping for any user-controlled string reflected into
// a page (e.g. the ntfy topic), so it can't break out of an attribute or inject
// markup.
static String htmlEscape(const String &in) {
  String o; o.reserve(in.length() + 8);
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    if      (c == '&')  o += F("&amp;");
    else if (c == '<')  o += F("&lt;");
    else if (c == '>')  o += F("&gt;");
    else if (c == '"')  o += F("&quot;");
    else if (c == '\'') o += F("&#39;");
    else                o += c;
  }
  return o;
}

// A "device token" input, rendered into config forms only when a token is set,
// so the owner can authorize a save. Empty (no UX change) in the default,
// zero-config state. Never echoes the token value.
static String adminTokenField() {
  if (pushToken.length() == 0) return String();
  return F("<label>device token</label>"
           "<input name=token type=password placeholder='required to save'>");
}

// A name for this particular board that survives a reboot.
//
// Two of these on one network is now an ordinary situation rather than an edge
// case, and everything that has to tell them apart -- the companion's per-board
// top-up keys, the list of boards it feeds, the tray menu -- needs an identity
// that stays put. mDNS does not provide one. ESP-IDF resolves the yoyu.local
// collision by itself, renaming whichever board finished probing second to
// yoyu-2.local, so both are reachable by name; but that name is assigned in
// boot order and the two swap when they restart together. The MAC does not.
static const char *deviceId() {
  static char id[7] = "";
  if (!id[0]) {
    uint8_t m[6];
    WiFi.macAddress(m);            // set once Wi-Fi is up, which it is by here
    snprintf(id, sizeof(id), "%02x%02x%02x", m[3], m[4], m[5]);
  }
  return id;
}

static void handleStatus() {
  JsonDocument doc;
  doc["app"] = "Yoyu";        // discovery marker the companion looks for
  doc["mini"] = true;
  doc["id"] = deviceId();     // stable across reboots; "board" is only a model
  // What this board is actually running. The release checklist asks you to
  // confirm an OTA "reboots on the new version", and without this that can only
  // be done by eye or by scraping /update — neither of which works for a board
  // on a shelf, or for more than one at a time.
  doc["version"] = FW_VERSION;
  doc["self_hosted"] = selfHosted;
  doc["board"] = BOARD_SLUG;          // which panel this build drives
  doc["theme"] = THEME_NAMES[uiTheme < 0 ? 0 : uiTheme];
  doc["plan"] = plan[0] ? plan : (const char *)nullptr;
  // Why the last poll said what it said, and how the release check is faring.
  // Both were previously visible only on the board's own screen, which makes a
  // board on a shelf — or more than one at a time — undiagnosable remotely.
  // pollStatus is the string the meters print when they have no data.
  doc["poll_status"] = pollStatus[0] ? pollStatus : (const char *)nullptr;
  // Whether anything is actually visible. A board that is drawing correctly
  // onto a dark panel looks identical, over the network, to one that is fine.
  doc["screen_off"] = screenOff;
  doc["backlight"] = backlight;
  doc["pairing"] = pairingActive();
  JsonObject rc = doc["release_check"].to<JsonObject>();
  rc["ok"]          = tagFetchOk;
  rc["tries"]       = tagFetchTries;   // 0 = never run since boot
  rc["latest"]      = cachedTag[0] ? cachedTag : (const char *)nullptr;
  rc["heap_before"] = tagHeapBefore;
  rc["heap_after"]  = tagHeapAfter;
  rc["heap_free"]   = ESP.getFreeHeap();
  JsonArray arr = doc["windows"].to<JsonArray>();
  for (int i = 0; i < nWindows; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["key"] = windows[i].key;
    o["label"] = windows[i].label;
    o["utilization"] = windows[i].utilization;
    o["resets_at"] = (long)windows[i].resets_at;
  }
  // Which screen is up, so a failure to switch is diagnosable without
  // standing over the board. Reset times likewise: a countdown that never
  // counts is usually a reset time that never parsed.
  doc["screen"] = uiScreen;
  doc["screen_name"] = SCREEN_NAMES[uiScreen];
  // Project shares, and how long ago they arrived — a Projects screen stuck on
  // yesterday's ranking is a companion that stopped pushing, not a board bug.
  if (nProjects > 0) {
    JsonArray pj = doc["projects"].to<JsonArray>();
    for (int i = 0; i < nProjects; i++) {
      JsonObject o = pj.add<JsonObject>();
      o["name"] = projects[i].name;
      o["share"] = projects[i].share;
    }
    doc["projects_window"] = projWindow;
    doc["projects_more"] = projMore;
    doc["projects_age_s"] = (long)((millis() - projAt) / 1000);
  }
  doc["server_time"] = (long)time(nullptr);
  String out;
  serializeJson(doc, out);
  sendJson(200, out);
}

static void handlePush() {
  if (!apiAuthed()) {
    sendJson(401, "{\"ok\":false,\"error\":\"unauthorized\"}");
    return;
  }
  if (server->hasArg("plain") == false) {
    sendJson(400, "{\"ok\":false,\"error\":\"no body\"}");
    return;
  }
  const String &body = server->arg("plain");
  if (body.length() > 8192) {
    sendJson(413, "{\"ok\":false,\"error\":\"too large\"}");
    return;
  }
  JsonDocument doc;
  if (deserializeJson(doc, body) != DeserializationError::Ok ||
      !doc["windows"].is<JsonArray>()) {
    sendJson(400, "{\"ok\":false,\"error\":\"bad json\"}");
    return;
  }
  nWindows = 0;
  nWindowsSeen = 0;
  for (JsonObject w : doc["windows"].as<JsonArray>()) {
    if (!w["utilization"].is<float>() && !w["utilization"].is<int>()) continue;
    nWindowsSeen++;                       // count first: "+N more" must be honest
    if (nWindows >= MAX_WINDOWS) continue;
    Window &dst = windows[nWindows++];
    strlcpy(dst.key, w["key"] | "", sizeof(dst.key));
    strlcpy(dst.label, w["label"] | "Usage", sizeof(dst.label));
    // compact long labels for a 2" screen
    if (strcmp(dst.key, "five_hour") == 0) strlcpy(dst.label, "Session", sizeof(dst.label));
    else if (strcmp(dst.key, "seven_day") == 0) strlcpy(dst.label, "Weekly", sizeof(dst.label));
    else if (strcmp(dst.key, "seven_day_opus") == 0) strlcpy(dst.label, "Opus", sizeof(dst.label));
    float u = w["utilization"].as<float>();
    dst.utilization = u < 0 ? 0 : (u > 100 ? 100 : u);
    dst.resets_at = parseISO(w["resets_at"] | (const char *)nullptr);
  }
  strlcpy(plan, doc["plan"] | "", sizeof(plan));
  // Optional: per-project shares, from the companion reading Claude Code's own
  // session logs. An older companion simply omits the key — leave the last set
  // standing rather than blanking the screen on every push.
  if (doc["projects"].is<JsonArray>()) {
    nProjects = 0;
    for (JsonObject p : doc["projects"].as<JsonArray>()) {
      if (nProjects >= MAX_PROJ) break;
      const char *nm = p["name"] | "";
      if (!nm[0]) continue;
      Proj &d = projects[nProjects++];
      strlcpy(d.name, nm, sizeof(d.name));
      float s = p["share"] | 0.0f;
      d.share = s < 0 ? 0 : (s > 100 ? 100 : s);
    }
    strlcpy(projWindow, doc["projects_window"] | "", sizeof(projWindow));
    projMore = doc["projects_more"] | 0;
    if (projMore < 0) projMore = 0;
    projAt = millis();
  }
  lastPushMs = millis();
  lastCompanionPushMs = millis();
  sendJson(200, "{\"ok\":true}");
  noteUsageActivity();
  checkAlerts();
  checkExhaustion();
  drawScreen();
}

// Wi-Fi provisioning portal (AP mode). The page fetches /scan for a tappable
// list of nearby networks so nothing has to be typed by hand (manual entry
// stays as a fallback).
static const char PORTAL_HTML[] PROGMEM = R"HTML(<!DOCTYPE html>
<html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Yoyu Wi-Fi setup</title>
<style>body{font-family:system-ui;background:#f0eee6;color:#3d3929;padding:24px 18px;margin:0}
.card{background:#faf9f5;border:1px solid rgba(61,57,41,.12);border-radius:14px;padding:16px;max-width:420px;margin:0 auto}
h2{margin:.2rem 0 .6rem}p{margin:.4rem 0}
input{width:100%;padding:12px;font-size:1rem;border-radius:10px;border:1px solid rgba(61,57,41,.25);margin:6px 0 12px;box-sizing:border-box}
button{display:block;width:100%;background:#d97757;color:#fff;font-weight:600;font-size:1.05rem;padding:14px;border-radius:10px;border:none}
#list{margin:6px 0 12px}
.net{background:#fff;color:#3d3929;text-align:left;font-weight:500;font-size:1rem;padding:12px 14px;margin:6px 0;border:1px solid rgba(61,57,41,.18);border-radius:10px;display:flex;justify-content:space-between;align-items:center}
.net.sel{border-color:#d97757;background:#fbeee8}
.net .bars{color:#6b6759;font-size:.85rem;margin-left:10px}
.row{display:flex;gap:8px;align-items:center;margin:6px 0}
.row button{width:auto;padding:8px 12px;font-size:.9rem;background:#e9e6dc;color:#3d3929}
.muted{color:#6b6759;font-size:.9rem}</style>
</head><body><div class="card">
<h2>Connect Yoyu to Wi-Fi</h2>
<div class="row"><strong>Pick your network</strong>
<button type="button" id="rescan">Rescan</button></div>
<div id="list" class="muted">Scanning&hellip;</div>
<form method="POST" action="/wifi">
<input name="ssid" id="ssid" placeholder="Network name (SSID)" autocapitalize="off" autocorrect="off">
<input name="password" id="pw" type="password" placeholder="Wi-Fi password">
<button type="submit">Connect</button></form>
<p class="muted">Not listed? Type the name above.</p>
</div>
<script>
function bars(r){var n=r>=-55?4:r>=-65?3:r>=-75?2:1;return '•'.repeat(n)+'·'.repeat(4-n);}
function pick(el,ssid){document.querySelectorAll('.net').forEach(function(x){x.classList.remove('sel');});
el.classList.add('sel');document.getElementById('ssid').value=ssid;document.getElementById('pw').focus();}
function load(){var L=document.getElementById('list');L.textContent='Scanning…';L.className='muted';
fetch('/scan').then(function(r){return r.json();}).then(function(nets){
if(!nets.length){L.textContent='No networks found. Type the name below.';return;}
L.innerHTML='';L.className='';
nets.forEach(function(n){var b=document.createElement('button');b.type='button';b.className='net';
b.innerHTML='<span>'+(n.lock?'🔒 ':'')+n.ssid.replace(/</g,'&lt;')+'</span><span class="bars">'+bars(n.rssi)+'</span>';
b.onclick=function(){pick(b,n.ssid);};L.appendChild(b);});
}).catch(function(){L.textContent='Scan failed — type your network below.';});}
document.getElementById('rescan').onclick=load;load();
</script>
</body></html>)HTML";

static void handlePortal() { server->send(200, "text/html", PORTAL_HTML); }

// Return nearby networks as JSON: [{"ssid","rssi","lock"}...], strongest first,
// deduped by name. Runs in AP+STA mode so the phone stays connected mid-scan.
static void handleScan() {
  int n = WiFi.scanNetworks(false /*async*/, false /*hidden*/);
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < n && arr.size() < 24; i++) {
    String ssid = WiFi.SSID(i);
    if (ssid.length() == 0) continue;
    bool dup = false;
    for (JsonObject o : arr)
      if (ssid == (const char *)(o["ssid"] | "")) { dup = true; break; }
    if (dup) continue;
    JsonObject o = arr.add<JsonObject>();
    o["ssid"] = ssid;
    o["rssi"] = WiFi.RSSI(i);
    o["lock"] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
  }
  WiFi.scanDelete();
  String out;
  serializeJson(doc, out);
  sendJson(200, out);
}

static void handleWifiSave() {
  String ssid = server->arg("ssid");
  String pass = server->arg("password");
  if (ssid.length() == 0 || ssid.length() > 64) {
    server->send(200, "text/html",
                 "<p>Pick a network name first. <a href=/>back</a></p>");
    return;
  }
  prefs.begin("headroom", false);
  prefs.putString("ssid", ssid);
  prefs.putString("psk", pass);
  prefs.end();
  server->send(200, "text/html",
               "<h2>Saved — rebooting.</h2><p>Yoyu will join your network."
               " Watch its screen for the address.</p>");
  delay(1200);
  ESP.restart();
}

// ---------------------------------------------------- Improv Wi-Fi (serial)
// Lets the browser flasher (ESP Web Tools) provision Wi-Fi over the same USB
// cable used to flash — the browser asks for your network right after Install
// and sends it down the wire. No hotspot, no typing an address.
// Protocol: https://www.improv-wifi.com/serial/

namespace improv {
enum Type : uint8_t { T_CURRENT_STATE = 0x01, T_ERROR = 0x02,
                      T_RPC = 0x03, T_RPC_RESPONSE = 0x04 };
enum State : uint8_t { S_AUTHORIZED = 0x02, S_PROVISIONING = 0x03,
                       S_PROVISIONED = 0x04 };
enum Err : uint8_t { E_NONE = 0x00, E_INVALID_RPC = 0x01, E_UNKNOWN_RPC = 0x02,
                     E_CANNOT_CONNECT = 0x03, E_UNKNOWN = 0xFF };
enum Cmd : uint8_t { C_WIFI_SETTINGS = 0x01, C_REQUEST_STATE = 0x02,
                     C_REQUEST_INFO = 0x03, C_REQUEST_SCAN = 0x04 };
static const char HEADER[6] = {'I', 'M', 'P', 'R', 'O', 'V'};
}  // namespace improv

static uint8_t improvRx[288];
static size_t improvPos = 0;

// Frame and emit one Improv packet: IMPROV + ver + type + len + data + cksum.
static void improvSend(uint8_t type, const uint8_t *data, uint8_t len) {
  uint8_t pkt[288];
  size_t n = 0;
  memcpy(pkt, improv::HEADER, 6); n = 6;
  pkt[n++] = 1;             // protocol version
  pkt[n++] = type;
  pkt[n++] = len;
  for (uint8_t i = 0; i < len; i++) pkt[n++] = data[i];
  uint32_t sum = 0;
  for (size_t i = 0; i < n; i++) sum += pkt[i];
  pkt[n++] = (uint8_t)(sum & 0xFF);
  Serial.write(pkt, n);
  Serial.write('\n');
}

static void improvSendState(uint8_t s) { improvSend(improv::T_CURRENT_STATE, &s, 1); }
static void improvSendError(uint8_t e) { improvSend(improv::T_ERROR, &e, 1); }

// RPC response: [cmd][blobLen][ (len-prefixed string)* ].
static void improvSendResult(uint8_t cmd, const char *const *strs, uint8_t nstrs) {
  uint8_t d[256];
  size_t n = 0;
  d[n++] = cmd;
  size_t lenAt = n++;       // filled in once the strings are laid down
  size_t start = n;
  for (uint8_t i = 0; i < nstrs; i++) {
    uint8_t sl = (uint8_t)strlen(strs[i]);
    if (n + 1 + sl > sizeof(d)) break;
    d[n++] = sl;
    memcpy(d + n, strs[i], sl); n += sl;
  }
  d[lenAt] = (uint8_t)(n - start);
  improvSend(improv::T_RPC_RESPONSE, d, (uint8_t)n);
}

// The device URL the browser should open once we're online. The port is not
// optional: :80 is the captive portal, which only exists in AP mode, so a URL
// without the port sends every new arrival to a closed port on the board they
// just set up — the flasher's "Visit Device" button is the first thing they
// click after joining Wi-Fi.
static void improvSendURL() {
  char url[48];
  snprintf(url, sizeof(url), "http://%s:%d",
           WiFi.localIP().toString().c_str(), API_PORT);
  const char *urls[1] = {url};
  improvSendResult(improv::C_WIFI_SETTINGS, urls, 1);
}

// One RPC result per network, then an empty result to mark the end. Lets the
// browser show a dropdown so only the password has to be typed.
static void improvSendScan() {
  int n = WiFi.scanNetworks();
  for (int i = 0; i < n; i++) {
    char ssid[33];
    strlcpy(ssid, WiFi.SSID(i).c_str(), sizeof(ssid));
    if (ssid[0] == 0) continue;
    char rssi[8];
    snprintf(rssi, sizeof(rssi), "%d", WiFi.RSSI(i));
    const char *row[3] = {ssid, rssi,
                          WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "NO" : "YES"};
    improvSendResult(improv::C_REQUEST_SCAN, row, 3);
  }
  improvSendResult(improv::C_REQUEST_SCAN, nullptr, 0);
  WiFi.scanDelete();
}

static void improvConnect(const char *ssid, const char *pass) {
  improvSendState(improv::S_PROVISIONING);
  gfx->fillScreen(C_BG);
  drawCentered("Connecting to Wi-Fi", 120, 2, C_INK);
  drawCentered(ssid, 158, 1, C_MUTED);

  WiFi.mode(WIFI_STA);
  WiFi.setHostname("yoyu");
  WiFi.begin(ssid, pass);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) delay(200);

  if (WiFi.status() != WL_CONNECTED) {
    improvSendError(improv::E_CANNOT_CONNECT);
    drawCentered("Couldn't connect - check password", 196, 1, C_CRIT);
    return;
  }
  prefs.begin("headroom", false);
  prefs.putString("ssid", ssid);
  prefs.putString("psk", pass);
  prefs.end();
  improvSendState(improv::S_PROVISIONED);
  improvSendURL();
  Serial.flush();
  delay(600);
  ESP.restart();                 // reboot into the normal STA + API flow
}

static void improvDispatch() {
  if (improvRx[7] != improv::T_RPC) return;   // we only handle commands
  uint8_t cmd = improvRx[9];
  uint8_t blob = improvRx[10];
  const uint8_t *d = &improvRx[11];
  switch (cmd) {
    case improv::C_REQUEST_STATE: {
      bool online = WiFi.status() == WL_CONNECTED;
      improvSendState(online ? improv::S_PROVISIONED : improv::S_AUTHORIZED);
      if (online) improvSendURL();
      break;
    }
    case improv::C_REQUEST_INFO: {
      const char *info[4] = {"Yoyu", FW_VERSION, "ESP32-S3", "Yoyu"};
      improvSendResult(improv::C_REQUEST_INFO, info, 4);
      break;
    }
    case improv::C_REQUEST_SCAN:
      improvSendScan();
      break;
    case improv::C_WIFI_SETTINGS: {
      if (blob < 2) { improvSendError(improv::E_INVALID_RPC); break; }
      uint8_t sl = d[0];
      if (1 + sl + 1 > blob || sl > 64) { improvSendError(improv::E_INVALID_RPC); break; }
      uint8_t pl = d[1 + sl];
      if (2 + sl + pl > blob || pl > 64) { improvSendError(improv::E_INVALID_RPC); break; }
      char ssid[65], pass[65];
      memcpy(ssid, d + 1, sl); ssid[sl] = 0;
      memcpy(pass, d + 2 + sl, pl); pass[pl] = 0;
      improvConnect(ssid, pass);
      break;
    }
    default:
      improvSendError(improv::E_UNKNOWN_RPC);
  }
}

// Feed one serial byte through the packet parser; dispatch on a valid frame.
static void improvByte(uint8_t b) {
  if (improvPos >= sizeof(improvRx)) improvPos = 0;
  if (improvPos < 6) {                            // resync on the IMPROV header
    if (b == (uint8_t)improv::HEADER[improvPos]) improvRx[improvPos++] = b;
    else { improvPos = 0; if (b == 'I') improvRx[improvPos++] = b; }
    return;
  }
  improvRx[improvPos++] = b;
  if (improvPos < 9) return;                      // need version, type, length
  size_t total = 9 + (size_t)improvRx[8] + 1;     // +data +checksum
  if (improvPos < total) return;
  uint32_t sum = 0;
  for (size_t i = 0; i < total - 1; i++) sum += improvRx[i];
  if ((uint8_t)(sum & 0xFF) == improvRx[total - 1]) improvDispatch();
  improvPos = 0;
}

static void improvPoll() {
  while (Serial.available()) improvByte((uint8_t)Serial.read());
}

// ------------------------------------------- Phase 2: on-device usage polling

static void loadCreds() {
  // NVS namespace stays "headroom" forever. It is invisible to users, and
  // renaming it would silently drop every board's Wi-Fi, login, alerts and
  // screen settings the moment it took the update.
  prefs.begin("headroom", true);
  accessTok  = prefs.getString("atok", "");
  topupKey   = prefs.getString("tkey", "");
  tokenExpMs = prefs.getULong64("exp", 0);
  bool hadRefresh = prefs.isKey("rtok");
  strlcpy(plan, prefs.getString("plan", "").c_str(), sizeof(plan));
  showUsed   = prefs.getBool("used", false);
  applyTheme(prefs.getInt("theme", DEFAULT_THEME));
  ntfyTopic  = prefs.getString("ntfy", "");
  poToken    = prefs.getString("potok", "");
  poUser     = prefs.getString("pouser", "");
  alertPct   = prefs.getInt("alpct", 90);
  strlcpy(tzEnv, prefs.getString("tz", tzEnv).c_str(), sizeof(tzEnv));
  clock24    = prefs.getBool("clk24", false);
  nightDim   = prefs.getBool("ndim", true);
  screenMask = prefs.getUChar("smask", 0x1F);
  defaultScreen = prefs.getInt("dscr", 0);
  rotateSecs = prefs.getInt("rots", 0);
  pushToken  = prefs.getString("ptok", "");
  bool timerMigrated = prefs.getBool("tmrmig", false);
  bool actionsMigrated = prefs.getBool("actmig", false);
  bool projectsMigrated = prefs.getBool("prjmig", false);
  bool settingsMigrated = prefs.getBool("setmig", false);
  prefs.end();
  // Purge a refresh token left by firmware that used to sign itself in. Done
  // here rather than on the next saveCreds() because a board that is never
  // re-paired would otherwise keep a live, spendable credential in flash for
  // as long as it runs -- for a token this build has no way to use.
  if (hadRefresh) {
    prefs.begin("headroom", false);
    prefs.remove("rtok");
    prefs.end();
  }
  selfHosted = accessTok.length() > 0;
  if (defaultScreen < 0 || defaultScreen >= UI_SCREENS) defaultScreen = 0;
  screenMask &= (1 << UI_SCREENS) - 1;      // ignore stray high bits
  screenMask |= (1 << defaultScreen);       // the default is always in the rotation
  // One-time: reveal each screen added since the install was set up, then never
  // touch the mask again (so a later un-check sticks). Settings especially — it
  // is the only place the board prints its own address, so an upgrade that left
  // it hidden would keep the problem it was added to fix. Actions and Projects
  // are inert without a companion, so revealing them costs nothing.
  //
  // One transaction, not one per screen: each prefs.end() is a commit to
  // wear-levelled flash, and doing them separately writes three superseded
  // values of smask before the one that matters. The next new screen is one
  // more line inside the block rather than a fifth copy of it.
  if (!timerMigrated || !actionsMigrated || !projectsMigrated ||
      !settingsMigrated) {
    prefs.begin("headroom", false);
    if (!timerMigrated) {
      screenMask |= (1 << SCREEN_TIMER);    prefs.putBool("tmrmig", true);
    }
    if (!actionsMigrated) {
      screenMask |= (1 << SCREEN_ACTIONS);  prefs.putBool("actmig", true);
    }
    if (!projectsMigrated) {
      screenMask |= (1 << SCREEN_PROJECTS); prefs.putBool("prjmig", true);
    }
    if (!settingsMigrated) {
      screenMask |= (1 << SCREEN_SETTINGS); prefs.putBool("setmig", true);
    }
    prefs.putUChar("smask", screenMask);
    prefs.end();
  }
  uiScreen = defaultScreen;                 // boot on the chosen screen
}

static void applyTz() {
  setenv("TZ", tzEnv, 1);
  tzset();
}

static void saveTheme() {
  prefs.begin("headroom", false);
  prefs.putInt("theme", uiTheme);
  prefs.end();
}

static void saveCreds() {
  prefs.begin("headroom", false);
  prefs.putString("atok", accessTok);
  // A board upgrading from a build that stored one still has a refresh token
  // in flash. It is never read now, but leaving a live credential sitting
  // there earns nothing, and it can still be spent by anything that gets at
  // the flash -- so the upgrade actively removes it rather than orphaning it.
  prefs.remove("rtok");
  prefs.putString("tkey", topupKey);
  prefs.putULong64("exp", tokenExpMs);
  prefs.putString("plan", plan);
  prefs.end();
}

// Adopt an oauth object (from the companion's --pair or a pasted login) and
// persist it. Accepts either the raw oauth dict or a {claudeAiOauth:{...}}.
static bool storeOauth(JsonObject root) {
  JsonObject o = root["claudeAiOauth"].is<JsonObject>()
                     ? root["claudeAiOauth"].as<JsonObject>()
                     : root;
  const char *at = o["accessToken"].as<const char *>();
  if (!at) at = o["access_token"].as<const char *>();
  if (!at) return false;
  accessTok = at;
  // The refresh token is deliberately dropped, even when one is sent.
  //
  // Refresh tokens rotate: using one invalidates it and issues a replacement.
  // Claude Code on the owner's computer holds the same credential and refreshes
  // whenever it likes, so a board that also refreshes is in a race it wins
  // about half the time -- and every win signs the owner out of Claude Code.
  // The board's access token lasts ~8 hours, so it was winning that race
  // roughly three times a day.
  //
  // Buying a second Claude plan to avoid it costs several times what the board
  // does, so sharing one account is the normal case and has to be the safe one.
  // The board therefore holds a credential it cannot rotate, and the companion
  // tops it up (POST /api/token). The cost is that the board goes stale some
  // hours after the computer sleeps instead of running indefinitely -- which is
  // a smaller price than silently breaking the login it borrowed.
  tokenExpMs = o["expiresAt"] | (uint64_t)0;
  if (!tokenExpMs) tokenExpMs = o["expires_at"] | (uint64_t)0;
  const char *sub = o["subscriptionType"].as<const char *>();
  strlcpy(plan, sub ? sub : "", sizeof(plan));
  selfHosted = true;
  saveCreds();
  return true;
}

// There is deliberately no refresh here. Rotating a shared refresh token is
// what signed the owner out of Claude Code roughly daily; see storeOauth().
// Top-ups arrive from the companion at POST /api/token instead.

// Map an Anthropic usage window key to a short label that fits a 2" screen.
static const char *shortLabel(const char *key) {
  if (!strcmp(key, "five_hour"))            return "Session";
  if (!strcmp(key, "seven_day"))            return "Weekly";
  if (!strcmp(key, "seven_day_opus"))       return "Opus";
  if (!strcmp(key, "seven_day_sonnet"))     return "Sonnet";
  if (!strcmp(key, "seven_day_fable"))      return "Fable";
  // Any future per-model weekly window: show the model name, capitalised,
  // rather than a generic "Usage" that's indistinguishable from its siblings.
  if (!strncmp(key, "seven_day_", 10) && key[10]) {
    static char model[16];
    strlcpy(model, key + 10, sizeof(model));
    for (char *p = model; *p; p++) if (*p == '_') *p = ' ';
    if (model[0] >= 'a' && model[0] <= 'z') model[0] -= 32;
    return model;
  }
  if (!strcmp(key, "seven_day_oauth_apps")) return "Apps";
  if (!strcmp(key, "extra_usage"))          return "Extra";
  return "Usage";
}

// GET the usage endpoint, parse windows, redraw. Refreshes once on 401/403.
static bool fetchUsage(bool allowRefresh) {
  if (accessTok.length() == 0) {
    strlcpy(pollStatus, "not paired yet", sizeof(pollStatus));
    return false;
  }
  WiFiClientSecure client;
  tlsTrust(client);
  HTTPClient https;
  if (!https.begin(client, USAGE_URL)) {
    strlcpy(pollStatus, "can't reach Anthropic", sizeof(pollStatus));
    return false;
  }
  https.addHeader("Authorization", "Bearer " + accessTok);
  https.addHeader("anthropic-beta", OAUTH_BETA);
  https.addHeader("Accept", "application/json");
  https.addHeader("User-Agent", UA);
  const char *collect[] = {"Retry-After"};
  https.collectHeaders(collect, 1);
  int code = https.GET();
  if (code == 401 || code == 403) {
    // Expected roughly daily, and not a fault: the access token has run out and
    // only the companion can mint another. Deliberately not worded as an error
    // -- the previous "login expired - re-pair" sent people to re-pair, which
    // is a much bigger operation than opening an app that was going to run at
    // login anyway.
    https.end();
    // Two different situations, and they need different things done. A board
    // that has been paired since the update is simply waiting for a top-up and
    // needs nothing from anyone. A board upgraded from firmware that refreshed
    // for itself has no top-up key yet, and does need pairing once more.
    strlcpy(pollStatus,
            topupKey.length() ? "waiting for your computer"
                              : "needs pairing once more",
            sizeof(pollStatus));
    return false;
  }
  (void)allowRefresh;
  if (code != 200) {
    if (code == 429) {
      // Rate limited: back off so we stop pounding the endpoint (and prolonging
      // the cooldown). Honor Retry-After as a floor, else double each time,
      // capped at 30 min. A good read resets it (below).
      long ra = https.header("Retry-After").toInt();   // seconds, 0 if absent
      https.end();
      unsigned long raMs = ra > 0 ? (unsigned long)ra * 1000UL : 0;
      unsigned long grow = pollBackoffMs ? pollBackoffMs * 2 : POLL_INTERVAL_MS;
      unsigned long bo = raMs > grow ? raMs : grow;
      if (bo > 1800000UL) bo = 1800000UL;              // 30 min cap
      pollBackoffMs = bo;
      unsigned long mins = (POLL_INTERVAL_MS + pollBackoffMs + 59999UL) / 60000UL;
      snprintf(pollStatus, sizeof(pollStatus), "rate limited - waiting ~%lum", mins);
      return false;
    }
    https.end();
    if (code == 401 || code == 403)
      strlcpy(pollStatus, "login expired - re-pair", sizeof(pollStatus));
    else if (code < 0)
      strlcpy(pollStatus, "can't reach Anthropic", sizeof(pollStatus));
    else
      snprintf(pollStatus, sizeof(pollStatus), "Anthropic error %d", code);
    return false;
  }
  String payload = https.getString();
  https.end();

  JsonDocument doc;
  if (deserializeJson(doc, payload) || doc.as<JsonObject>().isNull()) {
    strlcpy(pollStatus, "bad response", sizeof(pollStatus));
    return false;
  }
  JsonObject root = doc.as<JsonObject>();

  // Preferred display order, mirroring the companion's ordering. Anything the
  // API reports that isn't listed here (a newly-introduced model window, say)
  // is appended afterwards rather than dropped — a fixed allow-list would make
  // a new window silently invisible on a self-hosted board.
  static const char *const ORDER[] = {
      "five_hour", "seven_day", "seven_day_sonnet", "seven_day_opus",
      "seven_day_fable", "seven_day_oauth_apps", "extra_usage"};
  nWindows = 0;
  nWindowsSeen = 0;

  // Adds one window if it looks like a usage object; counts it either way.
  auto take = [&](const char *key, JsonVariant v) {
    if (!v.is<JsonObject>() || v["utilization"].isNull()) return;
    nWindowsSeen++;
    if (nWindows >= MAX_WINDOWS) return;
    Window &w = windows[nWindows++];
    strlcpy(w.key, key, sizeof(w.key));
    strlcpy(w.label, shortLabel(key), sizeof(w.label));
    float u = v["utilization"].as<float>();
    w.utilization = u < 0 ? 0 : (u > 100 ? 100 : u);
    const char *r = v["resets_at"].as<const char *>();
    if (!r) r = v["resetsAt"].as<const char *>();
    w.resets_at = parseISO(r);
  };

  for (const char *key : ORDER) take(key, root[key]);
  for (JsonPair kv : root) {                   // then anything not listed above
    bool known = false;
    for (const char *key : ORDER)
      if (!strcmp(kv.key().c_str(), key)) { known = true; break; }
    if (!known) take(kv.key().c_str(), kv.value());
  }
  if (nWindows == 0) {
    strlcpy(pollStatus, "no usage windows", sizeof(pollStatus));
    return false;
  }
  pollStatus[0] = 0;    // success -> clear any prior error
  pollBackoffMs = 0;    // and drop back to the normal poll cadence
  lastPushMs = millis();
  noteUsageActivity();
  checkAlerts();
  checkExhaustion();
  drawScreen();
  return true;
}

static void pollUsage() {
  if (selfHosted && WiFi.status() == WL_CONNECTED) fetchUsage(true);
}

// ------------------------------------------------- OTA self-update (over Wi-Fi)

static void parseVer(const char *s, int out[3]) {
  if (*s == 'v' || *s == 'V') s++;
  out[0] = out[1] = out[2] = 0;
  sscanf(s, "%d.%d.%d", &out[0], &out[1], &out[2]);
}

static bool tagNewer(const char *latest, const char *current) {
  int L[3], C[3];
  parseVer(latest, L);
  parseVer(current, C);
  for (int i = 0; i < 3; i++)
    if (L[i] != C[i]) return L[i] > C[i];
  return false;
}

// The latest published release's tag (e.g. "v1.1.0"), or "" on failure.
// One attempt. Returns "" on any failure; the retry policy lives in the caller.
static String fetchLatestTagOnce() {
  WiFiClientSecure client;
  tlsTrust(client);
  HTTPClient https;
  https.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  if (!https.begin(client, RELEASES_API)) return "";
  https.addHeader("User-Agent", UA);
  https.addHeader("Accept", "application/vnd.github+json");
  int code = https.GET();
  if (code != 200) { https.end(); return ""; }
  // Parse the tag straight from the response stream with a filter, so the whole
  // 10-30KB release JSON never lands in one big String (heap/fragmentation).
  JsonDocument filter;
  filter["tag_name"] = true;
  JsonDocument doc;
  DeserializationError err =
      deserializeJson(doc, https.getStream(), DeserializationOption::Filter(filter));
  https.end();
  if (err) return "";
  const char *tag = doc["tag_name"].as<const char *>();
  return tag ? String(tag) : "";
}

// The latest published release's tag (e.g. "v1.1.0"), or "" once it has really
// failed.
//
// Retried, and cached for TAG_CACHE_MS. Both exist because of the same observed
// bug: the update page would offer an "Install update" button, and the POST
// behind it -- which re-checked with its own second fetch -- would come back
// "already on the latest version". A single handshake fails often enough to see
// by hand, and returning "" for a transient failure is indistinguishable from
// "definitely up to date", which is the wrong default of the two.
//
// The cache also makes one user action cost one round trip instead of two,
// which matters more than the saved traffic: the second handshake was the one
// that kept failing.
static String fetchLatestTag() {
  unsigned long now = millis();
  if (cachedTag[0] && (now - cachedTagAt) < TAG_CACHE_MS) return String(cachedTag);

  tagHeapBefore = ESP.getFreeHeap();
  String tag;
  for (tagFetchTries = 1; tagFetchTries <= TAG_FETCH_TRIES; tagFetchTries++) {
    tag = fetchLatestTagOnce();
    if (tag.length()) break;
    // Short pause between attempts. Measured on hardware, the fetch costs about
    // 1.2KB of a free 270KB, so this is not waiting for memory to come back —
    // it is giving a flaky connection a moment rather than hammering it.
    if (tagFetchTries < TAG_FETCH_TRIES) delay(TAG_RETRY_DELAY_MS);
  }
  tagHeapAfter = ESP.getFreeHeap();
  tagFetchOk = tag.length() > 0;
  if (tagFetchOk) {
    strlcpy(cachedTag, tag.c_str(), sizeof(cachedTag));
    cachedTagAt = millis();
  }
  return tag;
}

// Periodic background check: is a newer release out than what we're running?
// Sets the flag that lights the on-screen update badge. Cheap and best-effort.
static void checkForUpdate() {
  if (WiFi.status() != WL_CONNECTED) return;
  String latest = fetchLatestTag();
  if (!latest.length()) return;
  strlcpy(latestSeen, latest.c_str(), sizeof(latestSeen));
  updateAvailable = tagNewer(latest.c_str(), FW_VERSION);
}

static void drawUpdateProgress(int pct) {
  static int last = -1;
  if (pct == last) return;
  last = pct;
  gfx->fillScreen(C_BG);
  drawCentered("Updating Yoyu", 110, 2, C_INK);
  char b[8];
  snprintf(b, sizeof(b), "%d%%", pct);
  drawCentered(b, 150, 4, C_ACC);
  gfx->drawRect(mapX(30), mapY(208), mapLen(180), mapLen(16), C_MUTED);
  int w = mapLen(176) * pct / 100;
  if (w > 0) gfx->fillRect(mapX(32), mapY(210), w, mapLen(12), C_ACC);
  drawCentered("keep it powered", 244, 1, C_MUTED);
}

// Fetch the detached ECDSA signature for the app image into `out`.
static bool fetchSig(uint8_t *out, size_t cap, size_t *outLen) {
  WiFiClientSecure client;
  tlsTrust(client);
  HTTPClient https;
  https.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  if (!https.begin(client, APP_SIG_URL)) return false;
  https.addHeader("User-Agent", UA);
  int code = https.GET();
  int len = (code == 200) ? https.getSize() : -1;
  if (len <= 0 || (size_t)len > cap) { https.end(); return false; }
  int got = https.getStreamPtr()->readBytes(out, len);
  https.end();
  if (got != len) return false;
  *outLen = (size_t)len;
  return true;
}

// Verify a SHA-256 image hash against OTA_PUBKEY using its detached signature.
static bool otaSignatureValid(const uint8_t *hash, const uint8_t *sig, size_t sigLen) {
  mbedtls_pk_context pk;
  mbedtls_pk_init(&pk);
  int rc = mbedtls_pk_parse_public_key(&pk, (const uint8_t *)OTA_PUBKEY,
                                       strlen(OTA_PUBKEY) + 1);
  if (rc == 0)
    rc = mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, hash, 32, sig, sigLen);
  mbedtls_pk_free(&pk);
  return rc == 0;
}

// Download the app image into the inactive OTA slot, hashing as it streams, and
// only mark it bootable if its signature verifies against the embedded public
// key. The running firmware is never overwritten, so a failed or forged
// download just leaves us as-is. No valid signature -> refuse (fail closed).
static bool doOTA() {
  uint8_t sig[128];
  size_t sigLen = 0;
  if (!fetchSig(sig, sizeof(sig), &sigLen)) return false;   // unsigned -> refuse

  WiFiClientSecure client;
  tlsTrust(client);
  HTTPClient https;
  https.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  if (!https.begin(client, APP_BIN_URL)) return false;
  https.addHeader("User-Agent", UA);
  int code = https.GET();
  if (code != 200) { https.end(); return false; }
  int len = https.getSize();
  if (len <= 0) { https.end(); return false; }
  if (!Update.begin((size_t)len)) { https.end(); return false; }

  mbedtls_md_context_t md;
  mbedtls_md_init(&md);
  mbedtls_md_setup(&md, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 0);
  mbedtls_md_starts(&md);

  WiFiClient *stream = https.getStreamPtr();
  uint8_t buf[1024];
  int done = 0;
  unsigned long lastData = millis();
  while (done < len) {
    size_t avail = stream->available();
    if (avail) {
      int want = len - done;
      if (want > (int)sizeof(buf)) want = sizeof(buf);
      if ((int)avail < want) want = (int)avail;
      int n = stream->readBytes(buf, want);
      if (n <= 0) break;
      if (Update.write(buf, n) != (size_t)n) break;
      mbedtls_md_update(&md, buf, n);
      done += n;
      drawUpdateProgress((int)((long)done * 100 / len));
      lastData = millis();
    } else if (!https.connected() || millis() - lastData > 15000) {
      break;                                        // stalled
    } else {
      delay(2);
    }
  }
  https.end();

  uint8_t hash[32];
  mbedtls_md_finish(&md, hash);
  mbedtls_md_free(&md);

  if (done != len || !otaSignatureValid(hash, sig, sigLen)) {
    Update.abort();
    return false;
  }
  return Update.end(true);   // true = mark the new slot bootable
}

static void handleUpdatePage() {
  String latest = fetchLatestTag();
  bool newer = latest.length() && tagNewer(latest.c_str(), FW_VERSION);
  String s = F(
      "<!DOCTYPE html><html><head><meta charset=utf-8>"
      "<meta name=viewport content='width=device-width,initial-scale=1'>"
      "<title>Yoyu - update</title><style>"
      "body{font-family:system-ui;background:#f0eee6;color:#3d3929;padding:22px 16px;margin:0}"
      ".card{background:#faf9f5;border:1px solid rgba(61,57,41,.12);border-radius:14px;"
      "padding:16px;max-width:460px;margin:0 auto}h2{margin:.2rem 0 .6rem}"
      "button{background:#d97757;color:#fff;font-weight:600;font-size:1rem;padding:12px 18px;"
      "border:none;border-radius:10px}.muted{color:#6b6759;font-size:.9rem}"
      "code{background:rgba(61,57,41,.07);padding:1px 6px;border-radius:5px}"
      "</style></head><body><div class=card>"
      "<p style='margin:0 0 10px'><a href='/' style='color:#a8442a;"
      "text-decoration:none;font-weight:600'>&larr; Home</a></p>"
      "<h2>Firmware update</h2>");
  s += "<p>Installed: <code>v";
  s += FW_VERSION;
  s += "</code>";
  if (latest.length()) { s += " &middot; Latest: <code>"; s += latest; s += "</code>"; }
  s += "</p>";
  if (!latest.length())
    s += F("<p class=muted>Couldn't reach GitHub to check right now.</p>");
  else if (newer) {
    s += F("<form method=POST action=/update/install>");
    s += adminTokenField();
    s += F("<button type=submit>Install update</button></form>"
           "<p class=muted>Takes about a minute. The board keeps your Wi-Fi and "
           "login, shows progress, and reboots itself. Keep it powered.</p>");
  }
  else
    s += F("<p>&#10003; You're on the latest version.</p>");
  s += F("</div></body></html>");
  server->send(200, "text/html", s);
}

static void handleUpdateInstall() {
  if (!apiAuthed()) { denyUnauthed(); return; }
  // Re-check that the published release is actually newer before flashing, so a
  // stale or hand-crafted POST can't trigger a needless re-flash or a downgrade.
  // (This is a browser-form endpoint, so it can't carry the X-Push-Token header;
  // the version gate is what protects it.)
  //
  // This used to be a second network round trip, moments after the one that
  // rendered the button — and it was the one that failed, telling people who
  // had just been offered an update that they were already current. The check
  // stays; fetchLatestTag() now serves it from the cache the page filled.
  String latest = fetchLatestTag();
  if (!latest.length() || !tagNewer(latest.c_str(), FW_VERSION)) {
    server->send(409, "text/html",
                 "<h2>Nothing to install</h2><p>Already on the latest version "
                 "(or GitHub was unreachable). <a href=/update>back</a></p>");
    return;
  }
  server->send(200, "text/html",
               "<h2>Updating&hellip;</h2><p>The board is installing and will "
               "reboot in about a minute. Watch its screen.</p>");
  delay(150);
  drawUpdateProgress(0);
  if (doOTA()) {
    drawCentered("Done - rebooting", 268, 1, C_ACC);
    delay(800);
    ESP.restart();
  } else {
    gfx->fillScreen(C_BG);
    drawCentered("Update failed", 130, 2, C_CRIT);
    drawCentered("still on the old version", 165, 1, C_MUTED);
    delay(2500);
    drawScreen();
  }
}

// ---- /connect web page: paste a Claude login once, board polls on its own ---

static void handleConnectPage() {
  String s = F(
      "<!DOCTYPE html><html><head><meta charset=utf-8>"
      "<meta name=viewport content='width=device-width,initial-scale=1'>"
      "<title>Yoyu - connect account</title><style>"
      "body{font-family:system-ui;background:#f0eee6;color:#3d3929;padding:22px 16px;margin:0}"
      ".card{background:#faf9f5;border:1px solid rgba(61,57,41,.12);border-radius:14px;"
      "padding:16px;max-width:520px;margin:0 auto}h2{margin:.2rem 0 .6rem}"
      "textarea{width:100%;height:120px;font-family:monospace;font-size:.82rem;"
      "border-radius:10px;border:1px solid rgba(61,57,41,.25);padding:10px;box-sizing:border-box}"
      "button{background:#d97757;color:#fff;font-weight:600;font-size:1rem;padding:12px 18px;"
      "border:none;border-radius:10px;margin-top:10px}"
      ".warn{background:#fbeee8;border:1px solid rgba(217,119,87,.35);border-radius:10px;"
      "padding:10px 12px;font-size:.9rem;margin:10px 0}.ok{color:#2e7d32;font-weight:600}"
      "code{background:rgba(61,57,41,.07);padding:1px 5px;border-radius:5px}"
      "</style></head><body><div class=card><h2>Connect your Claude account</h2>");
  if (selfHosted)
    s += F("<p class=ok>&#10003; Connected - the board polls your usage on its own.</p>");
  s += F(
      "<div class=warn><b>Easier way:</b> run the companion once with "
      "<code>--pair</code> and it sends this board your login automatically - "
      "no copying. This manual page is only for when you can't run it.</div>"
      "<p>Paste the contents of your Claude Code login. The board reads your "
      "usage directly, so no companion app has to keep running.</p>"
      "<ul><li><b>macOS:</b> Keychain item <code>Claude Code-credentials</code></li>"
      "<li><b>Windows/Linux:</b> <code>~/.claude/.credentials.json</code></li></ul>"
      "<div class=warn><b>Use a separate Claude login for the board.</b> If you "
      "paste the same login your computer's Claude Code uses, the two keep "
      "logging each other out. Best is a spare account just for the display.</div>"
      "<form method=POST action=/connect>"
      "<textarea name=creds placeholder='{&quot;claudeAiOauth&quot;:{...}}'></textarea>");
  s += adminTokenField();
  s += F("<button type=submit>Connect</button></form>");
  if (selfHosted) {
    s += F("<form method=POST action=/disconnect>");
    s += adminTokenField();
    s += F("<button style='background:#8a8577'>Disconnect</button></form>");
  }
  s += F("</div></body></html>");
  server->send(200, "text/html", s);
}

static void handleConnectSave() {
  if (!apiAuthed()) { denyUnauthed(); return; }
  String raw = server->arg("creds");
  JsonDocument doc;
  if (raw.length() == 0 || deserializeJson(doc, raw) ||
      !storeOauth(doc.as<JsonObject>())) {
    server->send(200, "text/html",
                 "<p>Couldn't read a login from that. <a href=/connect>back</a></p>");
    return;
  }
  bool ok = fetchUsage(true);
  String s = F("<!DOCTYPE html><html><head><meta charset=utf-8>"
               "<meta name=viewport content='width=device-width,initial-scale=1'>"
               "</head><body style='font-family:system-ui;padding:24px'>");
  if (ok)
    s += F("<h2>Connected &#10003;</h2><p>The board is showing your live usage. "
           "You can close this - it runs on its own now.</p>");
  else
    s += F("<h2>Saved, but the first read failed</h2><p>Make sure you pasted a "
           "current, valid login. The board will keep retrying.</p>");
  s += F("<p><a href=/connect>back</a></p></body></html>");
  server->send(200, "text/html", s);
}

// ---- Pairing (C3): an out-of-band code proves the companion is talking to the
// real board before any token crosses the wire. The board shows a one-time code
// on its screen; the companion proves the endpoint knows it (HMAC challenge)
// before sending, and the board proves the companion knows it before storing.
// A discovery-race impostor never sees the code, so it never receives the token.
static String        pairCode;                 // "" when not pairing
static unsigned long pairStartMs = 0;
static const unsigned long PAIR_TTL = 180000;  // 3 min, single use

static bool pairingActive() {
  return pairCode.length() && (millis() - pairStartMs) < PAIR_TTL;
}

static String hmacSha256Hex(const String &key, const String &msg) {
  uint8_t out[32];
  const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  mbedtls_md_hmac(info, (const uint8_t *)key.c_str(), key.length(),
                  (const uint8_t *)msg.c_str(), msg.length(), out);
  char hex[65];
  for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", out[i]);
  return String(hex);
}

// Big code on screen; the loop reverts to the normal UI when it expires.
static void drawPairScreen() {
  gfx->fillScreen(C_BG);
  drawCentered("Pairing", 56, 3, C_INK);
  drawCentered("type this code into the", 100, 1, C_MUTED);
  drawCentered("companion app on your computer", 116, 1, C_MUTED);
  drawCentered(pairCode.c_str(), 150, 5, C_ACC);
  drawCentered("expires in 3 minutes", 210, 1, C_MUTED);
}

// GET /api/actions — hand over any shortcuts pressed since the last poll and
// clear them. Read-only from the network's point of view: presses are only
// ever created by a physical touch on the board.
static void handleActions() {
  if (!apiAuthed()) { denyUnauthed(); return; }
  lastActionPollMs = millis();
  expireActions();
  String out = "{\"actions\":[";
  for (int i = 0; i < actionQN; i++) {
    if (i) out += ",";
    out += "\""; out += actionQ[i]; out += "\"";
  }
  out += "]}";
  actionQN = 0;                       // delivered; don't repeat them
  sendJson(200, out);
}

static void handlePairStart() {
  // Anyone on the LAN can ask the board to enter pairing mode, but the code is
  // shown ONLY on the physical screen — an impostor board can't display it.
  static const char *AB = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";  // no 0/O/1/I
  pairCode = "";
  for (int i = 0; i < 6; i++) pairCode += AB[esp_random() % 32];
  pairStartMs = millis();
  // Wake first. A dimmed or face-down board would otherwise render the code
  // onto a dark panel and report success -- the companion waits for a number
  // nobody can read, which is indistinguishable from the board ignoring the
  // request. Pairing is the one moment the screen is the entire interface.
  wake();
  drawPairScreen();
  sendJson(200, "{\"ok\":true}");
}

static void handlePairChallenge() {
  if (!pairingActive()) { sendJson(409, "{\"ok\":false,\"error\":\"not pairing\"}"); return; }
  String nonce = server->hasArg("plain") ? server->arg("plain") : "";
  if (nonce.length() < 8) { sendJson(400, "{\"ok\":false}"); return; }
  sendJson(200, String("{\"ok\":true,\"mac\":\"") + hmacSha256Hex(pairCode, nonce) + "\"}");
}

// The token POST is authorized by EITHER a matching push token (if the owner set
// one) OR a valid pairing-code MAC — never open by default, unlike the browser
// gate. This closes the "unauthenticated device overwrites the login" hole.
static bool pairMacOk(const String &body) {
  if (!pairingActive()) return false;
  String nonce = server->header("X-Pair-Nonce");
  String mac   = server->header("X-Pair-Mac");
  if (nonce.length() < 8 || mac.length() != 64) return false;
  return ctEqual(mac, hmacSha256Hex(pairCode, nonce + body));
}

// Companion --pair posts the oauth token here so the user never handles it.
static void handlePair() {
  if (!server->hasArg("plain")) { sendJson(400, "{\"ok\":false}"); return; }
  String body = server->arg("plain");
  bool tokenOk = pushToken.length() &&
                 ctEqual(server->header("X-Push-Token"), pushToken);
  if (!tokenOk && !pairMacOk(body)) {
    sendJson(401, "{\"ok\":false,\"error\":\"unauthorized\"}");
    return;
  }
  JsonDocument doc;
  if (deserializeJson(doc, body) || !storeOauth(doc.as<JsonObject>())) {
    sendJson(400, "{\"ok\":false,\"error\":\"no login in body\"}");
    return;
  }
  pairCode = ""; pairStartMs = 0;             // consume the pairing session
  // Mint a fresh top-up key on every pair, so re-pairing a board also revokes
  // whatever the last companion held.
  // Not named HEX: Arduino's Print.h defines that as a number base.
  static const char *HEXDIGITS = "0123456789abcdef";
  topupKey = "";
  for (int i = 0; i < 32; i++) topupKey += HEXDIGITS[esp_random() & 0xF];
  saveCreds();
  bool live = fetchUsage(true);
  String out = String("{\"ok\":true,\"live\":") + (live ? "true" : "false")
             + ",\"topup_key\":\"" + topupKey + "\"}";
  sendJson(200, out.c_str());
}

// Accept a newer access token for the account this board is already signed in
// to. This is what replaces refreshing: the companion holds the refresh token
// (as Claude Code always did) and the board is handed the short-lived result.
static void handleTokenTopup() {
  if (topupKey.length() == 0 ||
      !ctEqual(server->header("X-Topup-Key"), topupKey)) {
    sendJson(401, "{\"ok\":false,\"error\":\"bad or missing top-up key\"}");
    return;
  }
  String body = server->hasArg("plain") ? server->arg("plain") : "";
  JsonDocument doc;
  if (deserializeJson(doc, body) || !storeOauth(doc.as<JsonObject>())) {
    sendJson(400, "{\"ok\":false,\"error\":\"no access token in body\"}");
    return;
  }
  bool live = fetchUsage(true);
  sendJson(200, live ? "{\"ok\":true,\"live\":true}"
                     : "{\"ok\":true,\"live\":false}");
}

static void handleDisconnect() {
  if (!apiAuthed()) { denyUnauthed(); return; }
  accessTok = ""; topupKey = ""; tokenExpMs = 0; selfHosted = false;
  plan[0] = 0;
  prefs.begin("headroom", false);
  prefs.remove("atok"); prefs.remove("rtok");
  prefs.remove("exp");  prefs.remove("plan");
  prefs.end();
  nWindows = 0; nWindowsSeen = 0;
  server->send(200, "text/html", "<p>Disconnected. <a href=/connect>back</a></p>");
}

// ---- /alerts: phone push when usage gets high (ntfy topic / Pushover keys) --

static void handleAlertsPage() {
  char pct[8];
  snprintf(pct, sizeof(pct), "%d", alertPct);
  String s = F(
      "<!DOCTYPE html><html><head><meta charset=utf-8>"
      "<meta name=viewport content='width=device-width,initial-scale=1'>"
      "<title>Yoyu - phone alerts</title><style>"
      "body{font-family:system-ui;background:#f0eee6;color:#3d3929;padding:22px 16px;margin:0}"
      ".card{background:#faf9f5;border:1px solid rgba(61,57,41,.12);border-radius:14px;"
      "padding:16px;max-width:520px;margin:0 auto}h2{margin:.2rem 0 .6rem}label{font-size:.9rem}"
      "input{width:100%;padding:11px;font-size:1rem;border-radius:10px;"
      "border:1px solid rgba(61,57,41,.25);margin:4px 0 12px;box-sizing:border-box}"
      "button{background:#d97757;color:#fff;font-weight:600;font-size:1rem;padding:12px 18px;"
      "border:none;border-radius:10px}code{background:rgba(61,57,41,.07);padding:1px 5px;border-radius:5px}"
      ".muted{color:#6b6759;font-size:.85rem}</style></head><body><div class=card>"
      "<p style='margin:0 0 10px'><a href='/' style='color:#a8442a;"
      "text-decoration:none;font-weight:600'>&larr; Home</a></p>"
      "<h2>Phone alerts</h2>"
      "<p>Get a push when a window gets high. Easiest is <b>ntfy</b>: install "
      "the free ntfy app, pick any topic name, and enter it below.</p>"
      "<form method=POST action=/alerts>"
      "<label>ntfy topic</label>"
      "<input name=ntfy value='");
  s += htmlEscape(ntfyTopic);
  s += F("' placeholder='e.g. yoyu-dave-9f3'>"
         "<label>Alert at what % used?</label>"
         "<input name=pct type=number min=50 max=100 value='");
  s += pct;
  s += F("'>"
         "<details><summary class=muted>Pushover instead (optional)</summary>"
         "<label>Pushover API token</label>"
         "<input name=potok placeholder='");
  s += poToken.length() ? F("(saved - leave blank to keep)") : F("");
  s += F("'><label>Pushover user key</label><input name=pouser placeholder='");
  s += poUser.length() ? F("(saved - leave blank to keep)") : F("");
  s += F("'></details>");
  s += adminTokenField();
  s += F("<button type=submit>Save</button></form>"
         "<form method=POST action=/alerts/test style='margin-top:10px'>");
  s += adminTokenField();
  s += F("<button style='background:#8a8577'>Send test alert</button></form>"
         "<p class=muted>Recovery notice fires when it drops ~10% below the "
         "threshold.</p></div></body></html>");
  server->send(200, "text/html", s);
}

static void handleAlertsSave() {
  if (!apiAuthed()) { denyUnauthed(); return; }
  ntfyTopic = server->arg("ntfy");
  ntfyTopic.trim();
  int p = server->arg("pct").toInt();
  if (p >= 50 && p <= 100) alertPct = p;
  String pt = server->arg("potok"); pt.trim();
  String pu = server->arg("pouser"); pu.trim();
  if (pt.length()) poToken = pt;      // blank keeps the saved value
  if (pu.length()) poUser = pu;
  saveAlerts();
  server->send(200, "text/html",
               "<p>Saved. <a href=/alerts>back</a></p>");
}

static void handleAlertsTest() {
  if (!apiAuthed()) { denyUnauthed(); return; }
  if (!alertsConfigured()) {
    server->send(200, "text/html",
                 "<p>Set a topic or keys first. <a href=/alerts>back</a></p>");
    return;
  }
  sendAlert("Yoyu", "Test alert - notifications are working.");
  server->send(200, "text/html",
               "<p>Sent - check your phone. <a href=/alerts>back</a></p>");
}

// ---- /settings: friendly timezone picker (clock only; countdowns are TZ-free)

static const char *TZ_OPTIONS[][2] = {
    {"US Eastern",          "EST5EDT,M3.2.0,M11.1.0"},
    {"US Central",          "CST6CDT,M3.2.0,M11.1.0"},
    {"US Mountain",         "MST7MDT,M3.2.0,M11.1.0"},
    {"US Arizona (no DST)", "MST7"},
    {"US Pacific",          "PST8PDT,M3.2.0,M11.1.0"},
    {"US Alaska",           "AKST9AKDT,M3.2.0,M11.1.0"},
    {"US Hawaii",           "HST10"},
    {"UK / Ireland",        "GMT0BST,M3.5.0/1,M10.5.0"},
    {"Central Europe",      "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"India",               "IST-5:30"},
    {"Japan",               "JST-9"},
    {"Sydney",              "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {"UTC",                 "UTC0"},
};
static const int N_TZ = sizeof(TZ_OPTIONS) / sizeof(TZ_OPTIONS[0]);

static void handleSettingsPage() {
  String s = F(
      "<!DOCTYPE html><html><head><meta charset=utf-8>"
      "<meta name=viewport content='width=device-width,initial-scale=1'>"
      "<title>Yoyu - settings</title><style>"
      "body{font-family:system-ui;background:#f0eee6;color:#3d3929;padding:22px 16px;margin:0}"
      ".card{background:#faf9f5;border:1px solid rgba(61,57,41,.12);border-radius:14px;"
      "padding:16px;max-width:520px;margin:0 auto}h2{margin:.2rem 0 .6rem}label{font-size:.9rem}"
      "select{width:100%;padding:11px;font-size:1rem;border-radius:10px;"
      "border:1px solid rgba(61,57,41,.25);margin:4px 0 12px;box-sizing:border-box;background:#fff}"
      "button{background:#d97757;color:#fff;font-weight:600;font-size:1rem;padding:12px 18px;"
      "border:none;border-radius:10px}.muted{color:#6b6759;font-size:.85rem}</style>"
      "</head><body><div class=card>"
      "<p style='margin:0 0 10px'><a href='/' style='color:#a8442a;"
      "text-decoration:none;font-weight:600'>&larr; Home</a></p><h2>Settings</h2>"
      "<form method=POST action=/settings><label>Time zone (for the clock)</label>"
      "<select name=tz>");
  for (int i = 0; i < N_TZ; i++) {
    s += "<option value='";
    s += TZ_OPTIONS[i][1];
    s += "'";
    if (!strcmp(tzEnv, TZ_OPTIONS[i][1])) s += " selected";
    s += ">";
    s += TZ_OPTIONS[i][0];
    s += "</option>";
  }
  s += F("</select><label>Clock format</label><select name=clock>"
         "<option value=12");
  if (!clock24) s += " selected";
  s += F(">12-hour (3:45 PM)</option><option value=24");
  if (clock24) s += " selected";
  s += F(">24-hour (15:45)</option></select>"
         "<label>Overnight dimming</label><select name=ndim>"
         "<option value=on");
  if (nightDim) s += " selected";
  s += F(">On (dim 10pm-7am)</option><option value=off");
  if (!nightDim) s += " selected";
  s += F(">Off</option></select>"
         "<label>Screens to show (tap the display to cycle these)</label>"
         "<div style='margin:4px 0 12px'>");
  for (int i = 0; i < UI_SCREENS; i++) {
    s += "<label style='display:block;font-size:1rem;padding:3px 0'>"
         "<input type=checkbox name=scr";
    s += i;
    s += " value=1";
    if (screenEnabled(i)) s += " checked";
    s += "> ";
    s += SCREEN_NAMES[i];
    s += "</label>";
  }
  s += F("</div><label>Default screen (shown at power-on)</label><select name=dscr>");
  for (int i = 0; i < UI_SCREENS; i++) {
    s += "<option value=";
    s += i;
    if (i == defaultScreen) s += " selected";
    s += ">";
    s += SCREEN_NAMES[i];
    s += "</option>";
  }
  s += F("</select><label>Auto-rotate screens</label><select name=rots>");
  static const int ROT_OPTS[] = {0, 10, 20, 30, 60};
  for (int i = 0; i < 5; i++) {
    s += "<option value=";
    s += ROT_OPTS[i];
    if (ROT_OPTS[i] == rotateSecs) s += " selected";
    s += ">";
    if (ROT_OPTS[i] == 0) s += "Off (tap only)";
    else { s += "Every "; s += ROT_OPTS[i]; s += "s"; }
    s += "</option>";
  }
  s += F("</select><label>Theme</label><select name=theme>");
  for (int i = 0; i < THEME_COUNT; i++) {
    s += "<option value="; s += i;
    if (i == uiTheme) s += " selected";
    s += ">"; s += THEME_NAMES[i]; s += "</option>";
  }
  s += F("</select>"
         "<p class=muted>Dim suits an always-on AMOLED, where an unlit pixel "
         "emits nothing and the accents otherwise run at full brightness. "
         "Paper is for a bright room. Mono drops colour entirely.</p>"
         "<label>Push token <span class=muted>(optional)</span></label>"
         "<input type=text name=ptok autocomplete=off placeholder='");
  s += pushToken.length() ? "set - leave blank to keep" : "off - any device on your Wi-Fi can feed the board";
  s += F("' style='width:100%;padding:11px;font-size:1rem;border-radius:10px;"
         "border:1px solid rgba(61,57,41,.25);margin:4px 0 4px;box-sizing:border-box'>"
         "<p class=muted style='margin:0 0 12px'>Set a secret here and put the same "
         "value in the companion (<code>--token</code>) to lock feeding/pairing to "
         "your computer. Type <b>off</b> to clear it.</p>");
  s += adminTokenField();
  s += F("<button type=submit>Save</button></form>"
         "<p class=muted>Reset countdowns are timezone-independent. The default "
         "screen is always shown even if unchecked above.</p></div></body></html>");
  server->send(200, "text/html", s);
}

static void handleSettingsSave() {
  if (!apiAuthed()) { denyUnauthed(); return; }
  prefs.begin("headroom", false);
  String tz = server->arg("tz");
  for (int i = 0; i < N_TZ; i++)
    if (tz == TZ_OPTIONS[i][1]) {              // only accept a listed value
      strlcpy(tzEnv, tz.c_str(), sizeof(tzEnv));
      prefs.putString("tz", tzEnv);
      applyTz();
      break;
    }
  if (server->hasArg("clock")) {
    clock24 = (server->arg("clock") == "24");
    prefs.putBool("clk24", clock24);
  }
  if (server->hasArg("ndim")) {
    nightDim = (server->arg("ndim") == "on");
    prefs.putBool("ndim", nightDim);
  }
  if (server->hasArg("dscr")) {              // the screen form is present
    int d = server->arg("dscr").toInt();
    if (d >= 0 && d < UI_SCREENS) defaultScreen = d;
    uint8_t m = 0;
    for (int i = 0; i < UI_SCREENS; i++)
      if (server->arg(String("scr") + i) == "1") m |= (1 << i);
    m |= (1 << defaultScreen);              // default always shown (also keeps m != 0)
    screenMask = m;
    int r = server->arg("rots").toInt();
    if (r < 0) r = 0;
    if (r > 3600) r = 3600;
    rotateSecs = r;
    prefs.putUChar("smask", screenMask);
    prefs.putInt("dscr", defaultScreen);
    prefs.putInt("rots", rotateSecs);
    if (!screenEnabled(uiScreen)) uiScreen = defaultScreen;  // current got turned off
  }
  if (server->hasArg("theme")) {
    int t = server->arg("theme").toInt();
    if (t != uiTheme) {
      applyTheme(t);
      prefs.putInt("theme", uiTheme);   // inside the open prefs transaction
    }
  }
  if (server->hasArg("ptok")) {
    String t = server->arg("ptok");
    t.trim();
    if (t == "off") { pushToken = ""; prefs.putString("ptok", ""); }
    else if (t.length()) { pushToken = t; prefs.putString("ptok", pushToken); }
    // blank => leave the existing token untouched
  }
  prefs.end();
  applyBacklight();
  drawScreen();
  server->send(200, "text/html", "<p>Saved. <a href=/settings>back</a></p>");
}

// Styled landing page: status + how to feed it (companion / pair), links.
// The kitsune as inline SVG for the web UI, from the same sprite the screen
// draws -- including the live tail count, so the page and the panel never
// disagree about how much headroom is left.
static const char *kitsuneFill(char ch) {
  switch (ch) {
    case 'K': return "#1A1816";
    case 'B': return "#C9603F";
    case 'W': return "#FAF7EF";
    case 'S': return "#9E4429";
  }
  return nullptr;
}

static String kitsuneSvg(int px) {
  // Compose onto a scratch grid rather than emitting the sprite and then the
  // tails: the tails carry background cells that punch the gaps between them,
  // and a background <rect> over a transparent page would show as a dark patch.
  char grid[K_ROWS][K_COLS];
  memset(grid, '.', sizeof(grid));
  int tails = kitsuneTails();
  for (int t = 0; t < tails; t++)
    for (int i = 0; i < KITSUNE_TAIL_N[t]; i++) {
      const TailCell &tc = KITSUNE_TAILS[t][i];
      grid[tc.r][tc.c] = tc.ch;
    }
  for (int y = 0; y < K_ROWS; y++)
    for (int x = 0; x < K_COLS; x++)
      if (KITSUNE_SPRITE[y][x] != '.') grid[y][x] = KITSUNE_SPRITE[y][x];
  for (const TailCell &f : KITSUNE_FACE) grid[f.r][f.c] = f.ch;
  String s = "<svg width="; s += px; s += " height="; s += (px * K_ROWS) / K_COLS;
  s += " viewBox='0 0 18 15' shape-rendering=crispEdges style='display:block'>";
  for (int y = 0; y < K_ROWS; y++)
    for (int x = 0; x < K_COLS; x++) {
      const char *fill = kitsuneFill(grid[y][x]);
      if (!fill) continue;
      s += "<rect x="; s += x; s += " y="; s += y;
      s += " width=1 height=1 fill='"; s += fill; s += "'/>";
    }
  s += "</svg>";
  return s;
}

static void handleRoot() {
  String ip = WiFi.localIP().toString();
  const char *st = selfHosted ? "Running self-contained"
                 : lastPushMs ? "Fed by the companion"
                              : "Not set up yet";
  String s = F(
      "<!DOCTYPE html><html><head><meta charset=utf-8>"
      "<meta name=viewport content='width=device-width,initial-scale=1'>"
      "<title>Yoyu \u3088\u3086\u3046</title><style>"
      "body{font-family:system-ui;background:#f0eee6;color:#3d3929;padding:22px 16px;margin:0}"
      ".card{background:#faf9f5;border:1px solid rgba(61,57,41,.12);border-radius:14px;"
      "padding:18px;max-width:520px;margin:0 auto 14px}h1{margin:.1rem 0}"
      ".pill{display:inline-block;background:#efe9df;color:#6b6552;border-radius:999px;"
      "padding:3px 11px;font-size:.82rem}h3{margin:.2rem 0 .5rem}p{margin:.45rem 0}"
      "ol{padding-left:1.2rem;margin:.4rem 0}li{margin:.3rem 0}"
      "code{background:rgba(61,57,41,.07);padding:2px 6px;border-radius:6px;font-size:.9em;word-break:break-all}"
      "a.btn{display:inline-block;background:#d97757;color:#fff;text-decoration:none;"
      "font-weight:600;padding:11px 17px;border-radius:10px;margin:6px 8px 0 0}"
      ".muted{color:#6b6759;font-size:.9rem}summary{cursor:pointer}"
      ".ava{background:#262624;border-radius:12px;padding:7px;flex:none}"
      // Same lockup as the setup page. Every page the board serves declares
      // utf-8, so the kana are safe here -- unlike the panel, whose bitmap
      // font is ASCII only and would draw them as blanks.
      ".jp{font-size:.6em;color:#6b6759;font-weight:400;margin-left:.3em}</style>"
      "</head><body>"
      "<div class=card style='display:flex;align-items:center;gap:14px'>"
      "<div class=ava>");
  s += kitsuneSvg(52);
  s += F("</div><div><h1 style='margin:0 0 5px'>Yoyu"
         "<span class=jp lang=ja>\u3088\u3086\u3046</span></h1><span class=pill>");
  s += st;
  s += F("</span></div></div><div class=card><h3>See your Claude usage</h3><ol>"
         "<li><b>Download the companion app</b> and open it.</li>"
         "<li>That's it &mdash; it finds this board on your network and shows "
         "your usage. It also starts with your computer so it stays live.</li>"
         "</ol><p><a class=btn href='https://daveeuson.github.io/Yoyu/'>"
         "Get the companion app</a></p>"
         "<p class=muted>No typing, no address to enter.</p></div>"
         "<div class=card><details><summary><b>Advanced:</b> run without your "
         "computer</summary>"
         "<p>Normally you just leave the companion running (above) &mdash; that's "
         "the recommended setup. If you'd rather the board keep updating with your "
         "computer <i>off</i>, pair it once and it polls Anthropic itself:</p>"
         "<p><code>YoyuCompanion --pair</code></p>"
         "<p class=muted>(finds this board automatically. From source: "
         "<code>python companion.py --pair</code>.)</p>"
         "<p class=muted style='color:#c2410c'><b>Use a spare Claude account for "
         "the board.</b> If you pair the same account you use on your computer, "
         "the two will keep rotating each other's login and log each other out.</p>"
         "</details></div>"
         "<div class=card><h3>Settings &amp; more</h3>"
         "<a class=btn href=/settings style='background:#8a8577'>Settings</a>"
         "<a class=btn href=/alerts style='background:#8a8577'>Phone alerts</a>");
  if (updateAvailable) {
    s += "<a class=btn href=/update>Update available";
    if (latestSeen[0]) { s += " ("; s += latestSeen; s += ")"; }
    s += " &uarr;</a>";
  } else {
    s += "<a class=btn href=/update style='background:#8a8577'>Check for updates</a>";
  }
  s += F("<details class=muted style='margin-top:12px'>"
         "<summary>Advanced: paste a login by hand</summary>"
         "<p><a href=/connect>Open the manual connect page</a> &mdash; only if "
         "you can't run the companion.</p></details></div>"
         "<p class=muted style='text-align:center'>");
  // Carry the port here too: this footer is what people copy down as "the
  // board's address", and without it both forms land on the closed port 80.
  s += ip;
  s += ":";
  s += API_PORT;
  s += F(" &middot; yoyu.local:8080</p>"
         "<p class=muted style='text-align:center;font-size:.78rem;margin:0'>"
         "this board is <b>");
  s += deviceId();
  s += F("</b> &middot; ");
  s += BOARD_SLUG;
  s += F(" &middot; if you run two, yoyu.local reaches whichever booted first"
         "</p>"
         "<p class=muted style='text-align:center;font-size:.78rem;margin:2px 0 8px'>"
         "Made by Dave Euson with <span style='color:#d97757'>&hearts;</span> "
         "in San Diego &middot; &copy; 2026 Dave Euson</p>"
         "</body></html>");
  server->send(200, "text/html", s);
}

// -------------------------------------------------------------- input helpers

static void toggleUsedMode() {
  showUsed = !showUsed;
  prefs.begin("headroom", false);
  prefs.putBool("used", showUsed);
  prefs.end();
  drawScreen();
}

// Wipe saved Wi-Fi + login and reboot into the setup portal.
static void factoryReset() {
  prefs.begin("headroom", false);
  prefs.clear();
  prefs.end();
  screenOff = false;
  setBacklight(255);
  gfx->fillScreen(C_BG);
  drawCentered("Reset", 130, 3, C_WARN);
  drawCentered("reconnect Wi-Fi to set up again", 175, 1, C_MUTED);
  delay(1500);
  ESP.restart();
}

// BOOT held ~5s -> factory reset. Cheap to poll every loop.
static void checkBootButton() {
  static unsigned long downSince = 0;
  if (digitalRead(BOOT_BTN) == LOW) {
    if (downSince == 0) downSince = millis();
    else if (millis() - downSince > 5000) factoryReset();
  } else {
    downSince = 0;
  }
}

// ------------------------------------------------------------- touch + motion
// Shared I2C bus, 400 kHz: the touch controller and a QMI8658 6-axis IMU @ 0x6B.
// Pins and the touch address are per-board (boards.h). The CST816D register map
// was verified against the Waveshare ESP-IDF demo + community drivers; the
// CST9220 on the AMOLED board is UNVERIFIED -- see touchRead().
//
// Both chips degrade gracefully: if one isn't found, its feature is simply
// disabled. That is worth remembering when bringing up a new board, because it
// means wrong pins or a wrong address look exactly like working hardware with
// the feature switched off, rather than like a failure.

static const int     I2C_SDA    = TOUCH_SDA;
static const int     I2C_SCL    = TOUCH_SCL;
static const uint8_t IMU_ADDR_A = 0x6B;   // Waveshare default (SA0 high)
static const uint8_t IMU_ADDR_B = 0x6A;   // fallback
static uint8_t imuAddr  = 0;
static bool    touchOk  = false;
static bool    imuOk    = false;

static bool i2cRead(uint8_t addr, uint8_t reg, uint8_t *buf, uint8_t n) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;   // repeated start
  if (Wire.requestFrom((int)addr, (int)n) != n) return false;
  for (uint8_t i = 0; i < n; i++) buf[i] = Wire.read();
  return true;
}

// CST92xx parts address their registers with 16 bits, unlike the CST816D's 8.
// That difference is most of why the existing read does not port to them.
static bool i2cRead16(uint8_t addr, uint16_t reg, uint8_t *buf, uint8_t n) {
  Wire.beginTransmission(addr);
  Wire.write((uint8_t)(reg >> 8));
  Wire.write((uint8_t)(reg & 0xFF));
  if (Wire.endTransmission(false) != 0) return false;   // repeated start
  if (Wire.requestFrom((int)addr, (int)n) != n) return false;
  for (uint8_t i = 0; i < n; i++) buf[i] = Wire.read();
  return true;
}

static bool i2cWrite16(uint8_t addr, uint16_t reg, const uint8_t *data,
                       uint8_t n) {
  Wire.beginTransmission(addr);
  Wire.write((uint8_t)(reg >> 8));
  Wire.write((uint8_t)(reg & 0xFF));
  for (uint8_t i = 0; i < n; i++) Wire.write(data[i]);
  return Wire.endTransmission() == 0;
}

static void dispatchGesture(uint8_t g);   // defined with the touch UI below

#if !TOUCH_IS_CST816
// ---- CST9220 (Hynitron CST92xx) -----------------------------------------
//
// Register map and framing taken from ESPHome's cst9220 component, which is
// the only open implementation of this part -- Hynitron do not publish a
// datasheet and Waveshare's wiki does not document the protocol.
//
// Two things make it unlike the CST816D the other board uses: registers are
// addressed with 16 bits, and the chip reports raw coordinates with no gesture
// engine at all. Everything the UI reacts to -- tap, long press, the four
// swipes -- is worked out here from where a finger landed and left.
static const uint16_t CST_REG_TOUCH = 0xD000;
// 0xD101 is DEBUG mode, not a generic "command mode". The distinction is the
// whole ballgame: the info registers are only readable from it, and a chip
// left sitting in it answers every touch read with a frozen idle frame and
// never raises its interrupt. Normal reporting has to be asked for by name.
static const uint16_t CST_REG_DEBUG  = 0xD101;
static const uint16_t CST_REG_NORMAL = 0xD109;
static const uint16_t CST_REG_MODE_RB = 0x0002;   // echoes the mode just set
// Unlocks mode setting. The chip will not act on a work-mode write that has
// not been preceded by this, and -- this is the trap -- it echoes the mode you
// asked for at 0x0002 either way. So writing 0xD109 cold looks exactly like
// success: the read-back says 0x09, and the part carries on not scanning.
static const uint16_t CST_REG_MODESET = 0xD11E;
static const uint16_t CST_REG_CHECK = 0xD1FC;
// Firmware version and its checksum. The vendor driver refuses the part
// outright when the version reads back as 0xA5A5A5A5 -- that is what a CST92xx
// says when it has no firmware loaded at all. Such a chip still answers its
// address, still reports the resolution held in ROM and still accepts a mode
// write; it simply never scans the panel. Every symptom we have.
static const uint16_t CST_REG_FWVER = 0xD208;
static const uint16_t CST_REG_RES   = 0xD1F8;
static const uint16_t CST_REG_INFO  = 0xD204;
static const uint8_t  CST_ACK       = 0xAB;   // written back to release a report
// 2 points, not 5. ESPHome assumes five fingers and reads 30 bytes; the
// vendor's own driver declares MAX_FINGER_NUM as 2 and reads 15. Asking for
// more than the chip has to give can spoil the transaction, and this panel
// is not a five-finger surface anyway.
static const uint8_t  CST_DATA_LEN  = 15;     // 2 points x 5 bytes + 5 header

// Wake it and confirm it is the part we think it is. Without the command-mode
// write the chip acknowledges its address and returns nothing but zeroes
// forever, which looks exactly like a screen nobody is touching.
// Ask the controller to change work mode, the way its own driver does.
//
// Entering debug mode to read the info registers is a plain write; coming back
// out to normal reporting is not. The mode has to be unlocked first with
// 0xD11E, written twice, and the chip confirms the unlock by echoing 0x1E at
// 0x0002. Only then does a work-mode write take. Skipping it is silent: the
// mode read-back still returns whatever you asked for.
static bool cstSetMode(uint16_t modeReg) {
  uint8_t buf[4];
  bool unlocked = false;
  for (int i = 0; i < 3 && !unlocked; i++) {
    // Twice, deliberately. The vendor writes this register two times in a row
    // before reading the confirmation back.
    if (!i2cWrite16(TOUCH_ADDR, CST_REG_MODESET, buf, 0) ||
        !i2cWrite16(TOUCH_ADDR, CST_REG_MODESET, buf, 0) ||
        !i2cRead16(TOUCH_ADDR, CST_REG_MODE_RB, buf, 4)) {
      delay(200);
      continue;
    }
    unlocked = (buf[1] == 0x1E);
    if (!unlocked) delay(200);
  }
  if (!unlocked) {
    // Worth saying out loud rather than pressing on quietly: every symptom of
    // a chip that never got unlocked looks like a chip that is working.
    Serial.println(F("[touch] mode unlock (0xD11E) never confirmed"));
  }
  if (!i2cWrite16(TOUCH_ADDR, modeReg, buf, 0)) return false;
  if (!i2cRead16(TOUCH_ADDR, CST_REG_MODE_RB, buf, 2)) return false;
  uint8_t want = (uint8_t)(modeReg & 0xFF);
  Serial.printf("[touch] work mode -> 0x%02X (wanted 0x%02X)%s\n",
                buf[1], want, unlocked ? "" : "  [not unlocked]");
  if (buf[1] != want) return false;
  delay(10);
  return true;
}

static bool cstBegin() {
  // The exact order the vendor's own driver walks. The two reads in the middle
  // are not curiosity: writing command mode and then jumping straight to touch
  // reports leaves the chip answering with a frozen idle frame and never
  // raising its interrupt line. Whatever the handshake is, it completes by
  // going through the whole sequence.
  uint8_t buf[4];
  if (!i2cWrite16(TOUCH_ADDR, CST_REG_DEBUG, buf, 0)) return false;
  delay(5);
  if (!i2cRead16(TOUCH_ADDR, CST_REG_CHECK, buf, 4)) return false;
  uint32_t check = (uint32_t)buf[3] << 24 | (uint32_t)buf[2] << 16 |
                   (uint32_t)buf[1] << 8 | buf[0];
  if (!i2cRead16(TOUCH_ADDR, CST_REG_RES, buf, 4)) return false;
  int rw = (int)buf[1] << 8 | buf[0];
  int rh = (int)buf[3] << 8 | buf[2];
  if (!i2cRead16(TOUCH_ADDR, CST_REG_INFO, buf, 4)) return false;
  // Chip type is the HIGH pair and the project id the low one -- not the other
  // way round. Reading the low pair and calling it the id meant the number
  // printed at boot was the project id, so the part was never actually
  // identified; it just looked as though it had been.
  uint16_t chipType  = (uint16_t)buf[3] << 8 | buf[2];
  uint16_t projectId = (uint16_t)buf[1] << 8 | buf[0];
  // Resolution is the honest check that this is a conversation rather than
  // noise: it should come back as the panel's own size.
  Serial.printf("[touch] checkcode %08lX  chip 0x%04X  project 0x%04X  "
                "reports %dx%d\n",
                (unsigned long)check, chipType, projectId, rw, rh);

  uint8_t fw[8];
  if (i2cRead16(TOUCH_ADDR, CST_REG_FWVER, fw, 8)) {
    uint32_t fwVer = (uint32_t)fw[3] << 24 | (uint32_t)fw[2] << 16 |
                     (uint32_t)fw[1] << 8 | fw[0];
    uint32_t sum   = (uint32_t)fw[7] << 24 | (uint32_t)fw[6] << 16 |
                     (uint32_t)fw[5] << 8 | fw[4];
    Serial.printf("[touch] fw %08lX  checksum %08lX\n",
                  (unsigned long)fwVer, (unsigned long)sum);
    // The three checks the vendor's own driver makes before it will use the
    // part. Each names a different failure, and none of them were being made:
    // an unprogrammed chip, a garbled info block, and the wrong part entirely
    // all presented here as "initialises fine, never reports a press".
    if (fwVer == 0xA5A5A5A5UL) {
      Serial.println(F("[touch] this controller has NO FIRMWARE loaded -- it "
                       "will answer I2C and never scan the panel"));
      return false;
    }
    if ((check & 0xFFFF0000UL) != 0xCACA0000UL) {
      Serial.printf("[touch] firmware info block looks wrong (checkcode high "
                    "half %04lX, expected CACA)\n",
                    (unsigned long)(check >> 16));
      return false;
    }
  }
  if (chipType != 0x9220 && chipType != 0x9217) {
    Serial.printf("[touch] unexpected chip type 0x%04X (want 9220 or 9217)\n",
                  chipType);
    return false;
  }

  // Leave debug mode. Everything above was read from it; nothing below works
  // until the chip is put back to reporting touches -- and that takes the
  // unlock, not just the write.
  return cstSetMode(CST_REG_NORMAL);
}

// One press, start to finish. The gesture codes match the CST816D's so that
// dispatchGesture() -- and every screen's reading of it -- stays untouched:
// 1 up, 2 down, 3 left, 4 right, 5 tap, 0x0C long press.
static void pollTouchCst9220() {
#if TOUCH_INT != GFX_NOT_DEFINED
  // The controller pulls this low when a report is waiting. Reading whenever we
  // feel like it returns a stale idle frame -- FF FF FF FF with no acknowledge
  // byte -- which is indistinguishable from a screen nobody is touching.
  static int prevInt = -1;
  int intNow = digitalRead(TOUCH_INT);
#if defined(YOYU_TOUCH_PROBE)
  if (intNow != prevInt) {
    prevInt = intNow;
    Serial.printf("[touch] INT -> %s\n", intNow ? "high (idle)" : "LOW (report)");
  }
#endif
#if !defined(YOYU_TOUCH_NOINT)
  if (intNow) return;                 // nothing waiting
#endif
#endif
  uint8_t b[CST_DATA_LEN];
  if (!i2cRead16(TOUCH_ADDR, CST_REG_TOUCH, b, CST_DATA_LEN)) return;
  bool valid = (b[6] == CST_ACK);
  uint8_t fingers = valid ? (b[5] & 0x7F) : 0;
#if defined(YOYU_TOUCH_PROBE)
  // Log anything that isn't a quiet screen, so a report that arrives but is
  // rejected can be told from no report at all.
  static uint8_t prevRaw[8];
  static unsigned long lastRaw = 0;
  // Periodically as well as on change. prevRaw starts as zeroes, so an
  // all-zero frame is silently equal to it and prints nothing -- which reads
  // exactly like a read that failed.
  if (memcmp(b, prevRaw, 8) || millis() - lastRaw > 2000) {
    memcpy(prevRaw, b, 8);
    lastRaw = millis();
    Serial.printf("[touch] raw %02X %02X %02X %02X %02X %02X %02X  "
                  "valid=%d fingers=%d x=%d y=%d\n",
                  b[0], b[1], b[2], b[3], b[4], b[5], b[6],
                  valid, fingers,
                  ((int)b[1] << 4) | (b[3] >> 4),
                  ((int)b[2] << 4) | (b[3] & 0x0F));
  }
#endif
  int x = ((int)b[1] << 4) | (b[3] >> 4);
  int y = ((int)b[2] << 4) | (b[3] & 0x0F);
  // Acknowledged unconditionally, not only for frames we liked. If the chip
  // will not arm the next report until the last one is released, then skipping
  // the write on an invalid frame deadlocks it -- the first frame after boot is
  // never valid, so it would never send a second.
  {
    uint8_t ack = CST_ACK;
    i2cWrite16(TOUCH_ADDR, CST_REG_TOUCH, &ack, 1);
  }

  static bool down = false;
  static int x0, y0;
  static unsigned long t0;

  if (fingers && !down) {           // finger arrived
    down = true; x0 = x; y0 = y; t0 = millis();
    return;
  }
  if (fingers || !down) return;     // still held, or nothing happening
  down = false;                     // finger left: decide what it was

  int dx = x - x0, dy = y - y0;
  unsigned long held = millis() - t0;
  // A swipe has to cover a real distance, so a shaky tap is not read as one.
  // Scaled to the panel rather than fixed pixels: the same gesture should mean
  // the same thing on a 240px screen and a 480px one.
  int minMove = scrW / 6;
  if (abs(dx) > minMove && abs(dx) >= abs(dy)) {
    dispatchGesture(dx < 0 ? 0x03 : 0x04);          // left / right
  } else if (abs(dy) > minMove) {
    dispatchGesture(dy < 0 ? 0x01 : 0x02);          // up / down
  } else if (held > 500) {
    dispatchGesture(0x0C);                          // long press
  } else {
    dispatchGesture(0x05);                          // tap
  }
}
#endif  // !TOUCH_IS_CST816

static void i2cWrite(uint8_t addr, uint8_t reg, uint8_t val) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

static void sensorsBegin() {
#if TOUCH_RST != GFX_NOT_DEFINED
  // High-low-high, per the vendor sequence: the part wants to see a settled
  // line before the pulse, not just a rising edge out of whatever state the
  // pin powered up in. The CST816D has no reset line brought out at all.
#if TOUCH_INT != GFX_NOT_DEFINED
  pinMode(TOUCH_INT, INPUT_PULLUP);
#endif
  pinMode(TOUCH_RST, OUTPUT);
  digitalWrite(TOUCH_RST, HIGH);
  delay(5);
  digitalWrite(TOUCH_RST, LOW);
  delay(10);
  digitalWrite(TOUCH_RST, HIGH);
  delay(30);
#endif
  Wire.begin(I2C_SDA, I2C_SCL, 400000);
  // Every address that answers, logged once at boot. Both sensors degrade
  // silently by design -- a chip that isn't found just switches its feature off
  // -- which means wrong pins and a wrong address are indistinguishable from
  // hardware working fine with the feature disabled. This is the one place that
  // can tell them apart, and it costs a single pass at startup.
  Serial.printf("[i2c] scanning SDA=%d SCL=%d\n", I2C_SDA, I2C_SCL);
  int found = 0;
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      Serial.printf("[i2c]   device at 0x%02X\n", a);
      found++;
    }
  }
  Serial.printf("[i2c] %d device(s); expecting touch 0x%02X, imu 0x%02X/0x%02X\n",
                found, TOUCH_ADDR, IMU_ADDR_A, IMU_ADDR_B);
  Wire.beginTransmission(TOUCH_ADDR);
  touchOk = (Wire.endTransmission() == 0);
#if !TOUCH_IS_CST816
  // Answering its address is not the same as being awake -- see cstBegin().
  if (touchOk) touchOk = cstBegin();
#endif
  Serial.printf("[touch] controller %s\n", touchOk ? "ready" : "not found");
  uint8_t imuCandidates[2] = {IMU_ADDR_A, IMU_ADDR_B};
  for (uint8_t a : imuCandidates) {
    uint8_t who = 0;
    if (i2cRead(a, 0x00, &who, 1) && who == 0x05) { imuAddr = a; imuOk = true; break; }
  }
  if (imuOk) {
    i2cWrite(imuAddr, 0x02, 0x60);   // CTRL1: addr auto-increment, little-endian
    i2cWrite(imuAddr, 0x03, 0x13);   // CTRL2: accel +/-4g  (8192 LSB/g)
    i2cWrite(imuAddr, 0x08, 0x01);   // CTRL7: accelerometer enable
  }
}

static void wake() { screenOff = false; applyBacklight(); }

// Next enabled screen in `dir` (+1 / -1), skipping ones the user turned off.
// Returns `from` unchanged if it's the only one enabled.
static int nextEnabled(int from, int dir) {
  int c = from;
  for (int k = 0; k < UI_SCREENS; k++) {
    c = (c + dir + UI_SCREENS) % UI_SCREENS;
    if (screenEnabled(c)) return c;
  }
  return from;
}

static void cycleScreen(int dir) {
  uiScreen = nextEnabled(uiScreen, dir);
  drawScreen();
}

static void bumpBrightness(int d) {
  int v = (int)backlight + d;
  if (v < 25) v = 25;
  if (v > 255) v = 255;
  screenOff = false;
  setBacklight((uint8_t)v);
}

// CST816 gesture codes: 1 up, 2 down, 3 left, 4 right, 5 tap, 0x0B dbl, 0x0C long
static void dispatchGesture(uint8_t g) {
  lastUserTouch = millis();                 // pause auto-rotate while you interact
  if (screenOff) { wake(); return; }        // a dimmed screen wakes on any touch

  // No double-tap shortcut to Settings, deliberately. pollTouch dispatches on
  // every finger release, so the first tap of a double-tap has already been
  // delivered as a plain tap before 0x0B ever arrives — on the Actions screen
  // that meant queueing a real keystroke to the user's computer and *then*
  // jumping. Making it correct would mean holding every tap ~250ms to see
  // whether a second one follows, which is visible lag on "next screen" for a
  // shortcut that only duplicates paging. Settings is in the rotation instead.

  // Settings rebinds up/down to move the cursor and tap to toggle the row.
  // Left/right still page away, so there is always a way out.
  if (uiScreen == SCREEN_SETTINGS && !pairingActive()) {
    switch (g) {
      case 0x01: settingSel = (settingSel + UI_SCREENS - 1) % UI_SCREENS;
                 drawSettings(); return;                  // swipe up   -> previous
      case 0x02: settingSel = (settingSel + 1) % UI_SCREENS;
                 drawSettings(); return;                  // swipe down -> next
      case 0x03: cycleScreen(-1); return;
      case 0x04: cycleScreen(+1); return;
      case 0x0C: cycleScreen(+1); return;                 // long press -> leave
      default:                                            // tap -> toggle row
        toggleScreenAt(settingSel);
        drawSettings();
        return;
    }
  }

  // The Actions screen rebinds tap and up/down: a tap has to *do* something
  // there rather than page away, and up/down picks which shortcut to send.
  // Left/right still change screens, so there's always a way out.
  if (uiScreen == SCREEN_ACTIONS && !pairingActive()) {
    switch (g) {
      case 0x01: actionSel = (actionSel + N_ACTIONS - 1) % N_ACTIONS;
                 drawActions(); return;                   // swipe up   -> previous
      case 0x02: actionSel = (actionSel + 1) % N_ACTIONS;
                 drawActions(); return;                   // swipe down -> next
      case 0x03: cycleScreen(-1); return;
      case 0x04: cycleScreen(+1); return;
      case 0x0C: cycleScreen(+1); return;                 // long press -> leave
      default:                                            // tap -> fire it
        queueAction(ACTIONS[actionSel].id);
        drawCentered("sent", 288, 1, C_ACC);
        return;
    }
  }

  switch (g) {
    case 0x0C: toggleUsedMode();   break;   // long press -> % left / % used
    case 0x01: bumpBrightness(+40); break;  // swipe up   -> brighter
    case 0x02: bumpBrightness(-40); break;  // swipe down -> dimmer
    case 0x03: cycleScreen(-1);    break;   // swipe left
    case 0x04: cycleScreen(+1);    break;   // swipe right
    default:   cycleScreen(+1);             // tap -> next screen
  }
}

// Poll the touch controller; dispatch on finger release using the strongest
// gesture seen during the press (handles tap / long-press / swipe uniformly).
static void pollTouch() {
  if (!touchOk) return;
#if !TOUCH_IS_CST816
  pollTouchCst9220();
  return;
#else
  uint8_t b[6];
  if (!i2cRead(TOUCH_ADDR, 0x01, b, 6)) return;
  uint8_t gesture = b[0], finger = b[1];
  static bool touching = false;
  static uint8_t lastG = 0;
  if (finger == 1) {
    touching = true;
    if (gesture != 0) lastG = gesture;
  } else if (finger == 0 && touching) {
    dispatchGesture(lastG);                 // lastG 0 -> default tap
    touching = false;
    lastG = 0;
  }
#endif
}

// Accelerometer: flip-to-sleep and shake-to-wake. Self-calibrating — it takes
// "normal" from how the board is sitting at the first read, so which way the
// IMU's Z axis points (mounting-dependent on this board) doesn't matter.
static void pollMotion() {
  if (!imuOk) return;
  uint8_t b[6];
  if (!i2cRead(imuAddr, 0x35, b, 6)) return;
  float gx = (int16_t)((b[1] << 8) | b[0]) / 8192.0f;
  float gy = (int16_t)((b[3] << 8) | b[2]) / 8192.0f;
  float gz = (int16_t)((b[5] << 8) | b[4]) / 8192.0f;
  float mag = sqrtf(gx * gx + gy * gy + gz * gz);

  static bool  restInit = false;
  static float restZ = 1.0f;                 // gravity Z when sitting normally
  if (!restInit) { restZ = gz; restInit = true; return; }

  if (fabsf(mag - 1.0f) > 0.8f) {            // shake -> wake
    if (screenOff) wake();
    return;
  }
  if (fabsf(restZ) < 0.5f) return;           // boots upright -> don't auto-dim
  static int downCount = 0;
  float rel = gz * restZ;                     // >0 same as rest, <0 flipped over
  if (rel < -0.4f) {                          // flipped from its resting face
    if (++downCount > 3 && !screenOff) { screenOff = true; applyBacklight(); }
  } else if (rel > 0.3f) {                    // back to normal
    downCount = 0;
    if (screenOff) wake();
  }
}

// -------------------------------------------------------------------- setup

static void startPortal() {
  apMode = true;
  // AP+STA so WiFi.scanNetworks() can list nearby networks for the setup page
  // without dropping the phone off our hotspot.
  WiFi.mode(WIFI_AP_STA);
  bool apUp = WiFi.softAP(AP_SSID, AP_PSK);
  // Reported, because a hotspot that never comes up looks exactly like a
  // hotspot you cannot find: the screen says "join Yoyu-Setup" either way.
  Serial.printf("[portal] softAP(%s) -> %s  ip=%s  mac=%s  ch=%d\n",
                AP_SSID, apUp ? "ok" : "FAILED",
                WiFi.softAPIP().toString().c_str(),
                WiFi.softAPmacAddress().c_str(), WiFi.channel());
  WiFi.disconnect();                            // STA idle, just here for scans
  dns.start(53, "*", WiFi.softAPIP());          // captive: everything -> us
  server = new WebServer(80);
  server->onNotFound(handlePortal);
  server->on("/", HTTP_GET, handlePortal);
  server->on("/scan", HTTP_GET, handleScan);
  server->on("/wifi", HTTP_POST, handleWifiSave);
  server->begin();
  gfx->fillScreen(C_BG);
  drawCentered("Let's connect", 34, 3, C_INK);
  // Primary path: the browser flasher provisions over USB (Improv).
  drawCentered("Setting up in your browser?", 84, 1, C_INK);
  drawCentered("Just enter your Wi-Fi there.", 104, 1, C_MUTED);
  // Fallback path: phone hotspot.
  drawCentered("- or from a phone -", 140, 1, C_MUTED);
  drawCentered("join Wi-Fi", 164, 1, C_MUTED);
  drawCentered(AP_SSID, 184, 2, C_ACC);
  drawCentered("password", 216, 1, C_MUTED);
  drawCentered(AP_PSK, 234, 3, C_INK);          // big so it's easy to read
  drawCentered("then open http://192.168.4.1", 282, 1, C_MUTED);
  improvSendState(improv::S_AUTHORIZED);        // announce we're ready
}

static void startApi() {
  server = new WebServer(API_PORT);
  server->on("/api/status", HTTP_GET, handleStatus);
  server->on("/api/actions", HTTP_GET, handleActions);
  server->on("/api/push", HTTP_POST, handlePush);
  server->on("/api/pair", HTTP_POST, handlePair);
  server->on("/api/pair/start", HTTP_POST, handlePairStart);
  server->on("/api/pair/challenge", HTTP_POST, handlePairChallenge);
  server->on("/api/token", HTTP_POST, handleTokenTopup);
  server->on("/connect", HTTP_GET, handleConnectPage);
  server->on("/connect", HTTP_POST, handleConnectSave);
  server->on("/disconnect", HTTP_POST, handleDisconnect);
  server->on("/alerts", HTTP_GET, handleAlertsPage);
  server->on("/alerts", HTTP_POST, handleAlertsSave);
  server->on("/alerts/test", HTTP_POST, handleAlertsTest);
  server->on("/settings", HTTP_GET, handleSettingsPage);
  server->on("/settings", HTTP_POST, handleSettingsSave);
  server->on("/update", HTTP_GET, handleUpdatePage);
  server->on("/update/install", HTTP_POST, handleUpdateInstall);
  server->on("/setup", HTTP_GET, handleRoot);      // friendly alias for the docs
  server->on("/", HTTP_GET, handleRoot);
  const char *watch[] = {"X-Push-Token", "X-Pair-Nonce", "X-Pair-Mac",
                         "X-Topup-Key"};
  server->collectHeaders(watch, 4);
  server->begin();
  // Left as "yoyu" deliberately. A second board does not break this: ESP-IDF
  // probes the name, finds it taken and comes up as yoyu-2 instead, so
  // yoyu.local keeps meaning a board for everyone who owns one and every doc
  // that prints it stays true.
  MDNS.begin("yoyu");
  MDNS.addService("http", "tcp", API_PORT);
  MDNS.addServiceTxt("http", "tcp", "id", deviceId());
  MDNS.addServiceTxt("http", "tcp", "board", BOARD_SLUG);
}

void setup() {
  Serial.begin(115200);
  pinMode(BOOT_BTN, INPUT_PULLUP);   // hold 5s -> factory reset Wi-Fi
#if PANEL_HAS_BACKLIGHT
  ledcSetup(BL_CHANNEL, 5000, 8);    // backlight PWM (active high on this board)
  ledcAttachPin(LCD_BL, BL_CHANNEL);
#endif
  applyTheme(DEFAULT_THEME);         // before any drawing; loadCreds may change it
  gfx->begin(40000000);
  displayReady = true;               // brightness may now reach the panel
  setBacklight(255);
  initLayout();                      // must precede any drawing
  drawSplash("starting...", nullptr);
  sensorsBegin();                    // touch + IMU on the shared I2C bus

  prefs.begin("headroom", true);
  String ssid = prefs.getString("ssid", "");
  String psk = prefs.getString("psk", "");
  prefs.end();

  if (ssid.length() == 0) {
    startPortal();
    return;
  }

  drawSplash("joining Wi-Fi...", ssid.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("yoyu");
  WiFi.begin(ssid.c_str(), psk.c_str());
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 30000) delay(250);

  if (WiFi.status() != WL_CONNECTED) {
    // saved network unreachable -> offer setup again (don't wipe creds; a
    // router reboot shouldn't force reprovisioning — retry after portal boot)
    startPortal();
    drawCentered("(couldn't reach saved Wi-Fi)", 230, 1, C_WARN);
    return;
  }

  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  loadCreds();
  applyTz();          // header-clock timezone (from /settings; countdowns are TZ-free)
  loadHistory();
  readBattery();
  startApi();
  drawScreen();
  improvSendState(improv::S_PROVISIONED);   // in case the browser is listening
  pollUsage();                              // first live read if self-hosted
}

// --------------------------------------------------------------------- loop

void loop() {
  improvPoll();                     // browser can provision Wi-Fi over USB
  checkBootButton();                // hold BOOT 5s -> factory reset
  if (server) server->handleClient();
  if (apMode) {
    dns.processNextRequest();
    return;
  }
  static unsigned long lastTouch = 0, lastMotion = 0;
  if (millis() - lastTouch > 50)   { lastTouch = millis();  pollTouch(); }
  if (millis() - lastMotion > 400) { lastMotion = millis(); pollMotion(); }

  static unsigned long lastTick = 0;
  if (millis() - lastTick > 30000) {   // refresh clock/countdowns + battery
    lastTick = millis();
    if (!timeSynced && time(nullptr) > 1600000000) timeSynced = true;
    applyBacklight();     // ease down / back up as night comes and goes
    readBattery();
    drawScreen();
  }
  // Timer screen ticks its countdown every second (redraws only the digits).
  static unsigned long lastSec = 0;
  if (uiScreen == SCREEN_TIMER && !screenOff && !pairingActive() && timerResetAt &&
      millis() - lastSec >= 1000) {
    lastSec = millis();
    drawTimerClock();
  }
  // Drop the pairing screen back to the normal UI once the code expires.
  static bool pairShown = false;
  if (pairingActive()) pairShown = true;
  else if (pairShown) { pairShown = false; if (!screenOff) drawScreen(); }
  // The kitsune animates while its screen is up: advance a frame and redraw the
  // whole screen into the off-screen buffer (drawMascot blits it in one pass,
  // so there's no flicker).
  static unsigned long lastAnim = 0;
  if (uiScreen == 3 && !screenOff && !pairingActive() && millis() - lastAnim >= 200) {
    lastAnim = millis();
    mascotFrame++;
    drawMascot();
  }
  // Auto-rotate: advance to the next enabled screen every rotateSecs, but only
  // when more than one screen is enabled and you haven't touched it recently.
  static unsigned long lastRotate = 0;
  // Not while pairing. The code owns the screen for three minutes, and rotating
  // through the normal screens underneath it is what made the code impossible
  // to read: it appears, then the next rotation paints over it. Every other
  // screen-owning state (Timer, Actions, Settings) already stands down here.
  if (rotateSecs > 0 && !screenOff && !pairingActive() &&
      __builtin_popcount(screenMask & ((1 << UI_SCREENS) - 1)) > 1) {
    unsigned long iv = (unsigned long)rotateSecs * 1000UL;
    if (millis() - lastRotate >= iv && millis() - lastUserTouch >= iv) {
      lastRotate = millis();
      cycleScreen(+1);
    }
  }
  static unsigned long lastPoll = 0;   // self-hosted: pull fresh usage
  // Stand down while a companion is feeding us. Polling anyway asks Anthropic
  // the same question twice for one answer, and the account's rate limit is
  // shared -- which is how a board and its owner's computer end up throttling
  // each other despite neither doing anything wrong. Self-polling resumes on
  // its own once the pushes stop, which is the case it exists for.
  bool fedByCompanion = lastCompanionPushMs &&
                        (millis() - lastCompanionPushMs) < COMPANION_FRESH_MS;
  if (selfHosted && !fedByCompanion &&
      millis() - lastPoll > POLL_INTERVAL_MS + pollBackoffMs) {
    lastPoll = millis();
    pollUsage();
  }
  static unsigned long lastSample = 0; // usage history ring buffer
  static int samplesSincePersist = 0;
  if (nWindows > 0 && (lastSample == 0 || millis() - lastSample > SAMPLE_INTERVAL_MS)) {
    lastSample = millis();
    sampleHistory();
    if (++samplesSincePersist >= 6) { samplesSincePersist = 0; saveHistory(); }
    if (uiScreen == 2) drawScreen();
  }
  // Update check: ~20s after boot, then every 6h. Lights the on-screen badge.
  static unsigned long lastUpdChk = 0;
  const unsigned long UPD_CHECK_MS = 6UL * 60 * 60 * 1000;
  if ((lastUpdChk == 0 ? millis() > 20000 : millis() - lastUpdChk > UPD_CHECK_MS) &&
      WiFi.status() == WL_CONNECTED) {
    lastUpdChk = millis();
    bool was = updateAvailable;
    checkForUpdate();
    if (updateAvailable != was) drawScreen();   // badge just appeared/cleared
  }
}
