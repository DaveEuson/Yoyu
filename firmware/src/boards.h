// Board definitions. One of these is selected at build time by platformio.ini;
// everything below is the only place a raw GPIO number or panel dimension is
// allowed to appear, so adding a third board never means hunting through
// main.cpp for constants.
//
// Which board a build targets also decides which release asset it downloads for
// OTA. Getting that wrong is not cosmetic: both images are validly signed, so
// the signature check would happily accept LCD firmware onto an AMOLED board
// and leave it with a driver that cannot talk to its panel. The names are set
// here, next to the panel they belong to, for exactly that reason.

#pragma once

// ---------------------------------------------------------------- selection

#if !defined(YOYU_BOARD_LCD2) && !defined(YOYU_BOARD_AMOLED216) && \
    !defined(YOYU_BOARD_C6LCD147)
#define YOYU_BOARD_LCD2 1        // default: the board this project shipped on
#endif

// -------------------------------------------- Waveshare ESP32-S3-Touch-LCD-2
#if defined(YOYU_BOARD_LCD2)

#define BOARD_NAME        "ESP32-S3-Touch-LCD-2"
#define BOARD_SLUG        "lcd2"

// 2" ST7789 240x320 IPS over 4-wire SPI. This is the reference panel: the
// design space is 1:1 with it, so every map* call is the identity here.
#define PANEL_W           240
#define PANEL_H           320
#define PANEL_ROTATION    2      // portrait, USB-C on top
#define PANEL_IS_QSPI     0
#define PANEL_HAS_BACKLIGHT 1    // an IPS panel needs one; an AMOLED does not
// IPS panels drive their pixels inverted, so the controller has to invert
// back. Passed to the driver's `ips` argument, which is really just "send
// INVON" -- get it wrong and every colour comes out as its complement.
#define PANEL_INVERT      1

#define LCD_SCLK          39
#define LCD_MOSI          38
#define LCD_MISO          40
#define LCD_DC            42
#define LCD_CS            45
#define LCD_RST           GFX_NOT_DEFINED    // soft reset only
#define LCD_BL            1

#define TOUCH_SDA         48
#define TOUCH_SCL         47
#define TOUCH_ADDR        0x15   // CST816D
#define TOUCH_IS_CST816   1      // 8-bit registers, gestures decoded in hardware
// Whether a person can change screens with their hands. Not the same as a
// touch chip answering on the bus: the AMOLED has one that does, and never
// reports a press. Auto-rotate leans on this, because "off" on a board nobody
// can tap is a screen there is no way to leave.
#define HAS_TOUCH_INPUT   1
// BOOT differs by chip family: GPIO0 on the S3, GPIO9 on the C6.
#define BOOT_PIN          0
#define HAS_BUTTONS       0      // touch does the work here
#define TOUCH_INT         GFX_NOT_DEFINED
#define TOUCH_RST         GFX_NOT_DEFINED

#define HAS_BATTERY_ADC   1
#define VBAT_PIN          5      // via the onboard 200K/100K divider

// This controller is a 240x320 part driving a 240x320 panel, so no offset.
#define PANEL_COL_OFFSET  0
#define BACKLIGHT_MAX     255    // nothing on this panel objects to full
#define HAS_I2C_PERIPHERALS 1    // touch, and an IMU
#define HAS_RGB_LED       0
// Meters, Focus, History, Yoyu, Timer. What a fresh board starts with.
#define DEFAULT_SCREEN_MASK 0x1F
#define DEFAULT_SCREEN    0      // Meters

// The other board's fingerprint, for the wrong-firmware check in main.cpp.
// Looking for the AXP2101 on the AMOLED's own I2C pins is a positive
// identification rather than a guess: nothing on this board answers there,
// so finding it means this firmware is running on the wrong hardware.
#define OTHER_BOARD_SLUG  "amoled216"
#define OTHER_I2C_SDA     15
#define OTHER_I2C_SCL     14
#define OTHER_I2C_ANCHOR  0x34   // AXP2101, on every AMOLED board
#define HAS_SIBLING_CHECK 1

// Frozen on purpose. Boards in the field fetch these exact names for OTA, so
// renaming them to match the product would strand every one of them. A rename
// needs its own transition (publish both names for several releases), not a
// tidy-up. See PRODUCT.md, "Identifiers that deliberately did not follow the
// rename".
// A backlit IPS panel has a floor of emitted light anyway, so the original
// palette reads as intended on it.
#define DEFAULT_THEME     0      // Night

#define OTA_ASSET_PREFIX  "headroom-mini"

// --------------------------------------- Waveshare ESP32-S3-Touch-AMOLED-2.16
#elif defined(YOYU_BOARD_AMOLED216)

#define BOARD_NAME        "ESP32-S3-Touch-AMOLED-2.16"
#define BOARD_SLUG        "amoled216"

// 2.16" CO5300 480x480 AMOLED over QSPI. Square, which the 240x320 design space
// is not -- see the note at the bottom of this file before drawing anything.
#define PANEL_W           480
#define PANEL_H           480
#define PANEL_ROTATION    0
#define PANEL_IS_QSPI     1
#define PANEL_HAS_BACKLIGHT 0    // self-emissive; brightness is a panel command
// An AMOLED emits directly and needs no inversion. Setting this the way the
// IPS board needs it renders the whole UI as its own negative -- a white
// background with black text and blue meters, which reads as a broken
// layout rather than as a wrong flag.
#define PANEL_INVERT      0

// Arduino_ESP32QSPI takes its four data lines in the argument order
// (mosi, miso, quadwp, quadhd), which are D0..D3 on the datasheet.
#define QSPI_CS           12
#define QSPI_CLK          38
#define QSPI_D0           4
#define QSPI_D1           5
#define QSPI_D2           6
#define QSPI_D3           7
#define PANEL_RST         39

#define TOUCH_SDA         15
#define TOUCH_SCL         14
#define TOUCH_INT         11
#define TOUCH_RST         40
#define TOUCH_ADDR        0x5A   // confirmed by bus scan, see below
// CONFIRMED on hardware: an I2C scan of this board answers at 0x5A, alongside
// the IMU at 0x6B, an RTC at 0x51 and an audio codec at 0x18.
//
// The CST9220 is a CST92xx-family multi-touch controller: 16-bit registers and
// no hardware gesture engine, so the CST816D read does not port to it and taps,
// long presses and swipes are derived from coordinates. Register map from
// ESPHome's cst9220 component -- Hynitron publish no datasheet.
#define TOUCH_IS_CST816   0
// Whether a person can change screens with their hands. Not the same as a
// touch chip answering on the bus: the AMOLED has one that does, and never
// reports a press. Auto-rotate leans on this, because "off" on a board nobody
// can tap is a screen there is no way to leave.
#define HAS_TOUCH_INPUT   0    // CST9220 answers, never reports a press
#define BOOT_PIN          0
// Three keys along the edge. IO18 on the left goes back a screen and BOOT on
// the right goes forward. PWR in the middle is also the AXP2101's power-on
// gate -- holding it four seconds cuts the power in hardware -- so nothing is
// bound to it. Pins from a third-party board file for this exact board, then
// confirmed on the hardware by reading them in /api/status before anything
// was bound to them.
#define HAS_BUTTONS       1
#define BTN_PREV_PIN      18     // IO18, a key to ground
#define BTN_PWR_PIN       16     // PWR, active high through a BSS138 inverter

// This board has no battery divider on an ADC pin, and looking for one was
// the wrong question: it carries an AXP2101 power-management chip that already
// measures the cell and keeps a fuel gauge. Confirmed on the bus at 0x34, and
// it is what Waveshare's own 03_LVGL_AXP2101_ADC_Data example talks to.
//
// So the ADC path stays off and the PMIC path answers instead -- a percentage
// straight from the gauge, rather than a voltage curve fitted by hand.
#define HAS_BATTERY_ADC   0
#define HAS_BATTERY_PMIC  1
#define PMIC_ADDR         0x34   // AXP2101

#define PANEL_COL_OFFSET  0
#define BACKLIGHT_MAX     255
#define HAS_I2C_PERIPHERALS 1    // touch, IMU, PMIC, RTC, codec
#define HAS_RGB_LED       0
#define DEFAULT_SCREEN_MASK 0x1F
#define DEFAULT_SCREEN    0      // Meters

// The other board's fingerprint, for the wrong-firmware check in main.cpp.
// The CST816D sits on pins this board does not use for I2C at all, so an
// answer there is proof of the wrong hardware rather than an inference from
// an empty bus.
#define OTHER_BOARD_SLUG  "lcd2"
#define OTHER_I2C_SDA     48
#define OTHER_I2C_SCL     47
#define OTHER_I2C_ANCHOR  0x15   // CST816D, the LCD board's only I2C device
#define HAS_SIBLING_CHECK 1

// An AMOLED has no such floor: an unlit pixel emits nothing, so the accents
// run at full blast against true black and the first impression of the panel
// is glare. It opens on Dim, and Night is still one setting away.
#define DEFAULT_THEME     1      // Dim

#define OTA_ASSET_PREFIX  "yoyu-amoled"

// ------------------------------------------- Waveshare ESP32-C6-LCD-1.47
#elif defined(YOYU_BOARD_C6LCD147)

#define BOARD_NAME        "ESP32-C6-LCD-1.47"
#define BOARD_SLUG        "c6lcd147"

// 1.47" ST7789 172x320 IPS over 4-wire SPI. The controller is a 240-wide part
// driving a 172-wide panel, so the visible columns start 34 in: (240-172)/2.
// Without that offset everything lands shifted and clipped, which looks like a
// broken layout rather than a missing constant.
#define PANEL_W           172
#define PANEL_H           320
#define PANEL_COL_OFFSET  34
#define PANEL_ROTATION    0
#define PANEL_IS_QSPI     0
#define PANEL_HAS_BACKLIGHT 1
#define PANEL_INVERT      1      // IPS, same as the 2" board

#define LCD_SCLK          7
#define LCD_MOSI          6
#define LCD_MISO          GFX_NOT_DEFINED   // not wired to the panel
#define LCD_DC            15
#define LCD_CS            14
#define LCD_RST           21
#define LCD_BL            22

// Waveshare's documentation says twice, unprompted, to keep this panel at 50%
// brightness or less: it heats at full brightness and the heat leaves
// permanent dark patches on the glass. A cap in the firmware is worth more
// than a warning in a page nobody reads, so full brightness here is half.
#define BACKLIGHT_MAX     128

// No touch and no IMU. BOOT and RESET are the only inputs on the board.
// I2C still comes up, on two pins that are broken out and connected to
// nothing, so the existing scan finds nothing and both features switch
// themselves off exactly as they already do when a chip is missing.
#define HAS_I2C_PERIPHERALS 0
#define TOUCH_SDA         18
#define TOUCH_SCL         19
#define TOUCH_ADDR        0x15
#define TOUCH_IS_CST816   1
#define TOUCH_INT         GFX_NOT_DEFINED
// Whether a person can change screens with their hands. Not the same as a
// touch chip answering on the bus: the AMOLED has one that does, and never
// reports a press. Auto-rotate leans on this, because "off" on a board nobody
// can tap is a screen there is no way to leave.
#define HAS_TOUCH_INPUT   0    // no touch hardware at all
// GPIO9 is the C6's BOOT strap. The firmware read GPIO0 here for as long as
// the C6 has existed, so the five-second factory reset never worked on it.
#define BOOT_PIN          9
#define HAS_BUTTONS       0      // BOOT and RESET only, under the acrylic
#define TOUCH_RST         GFX_NOT_DEFINED

// The onboard WS2812, under the acrylic. Core 3.x drives one with
// rgbLedWrite(), so this costs no library.
#define HAS_RGB_LED       1
#define RGB_LED_PIN       8

#define HAS_BATTERY_ADC   0

// Two screens, not ten. 172px of width is not the place for the full
// rotation, and these are the two that answer the only questions the board
// exists for: how much is left, and when does it run out.
//
// Micro rather than Meters, because Meters spends its top third on a clock and
// a plan name. On this panel that third is the difference between a bar you
// can read from a doorway and one you have to walk up to.
#define DEFAULT_SCREEN_MASK ((1 << 9) | (1 << 8))   // Micro, Pace
#define DEFAULT_SCREEN    9      // Micro

#define DEFAULT_THEME     0      // Night
#define OTA_ASSET_PREFIX  "yoyu-c6lcd147"

// No sibling to look for: the check works by finding the other board's I2C
// chip, and this board has no I2C devices of its own to be missing.
#define HAS_SIBLING_CHECK 0

#endif

// ------------------------------------------------------------ design space
//
// Every screen is authored against a fixed 240x320 reference and mapped to the
// real panel at draw time. On the LCD board that mapping is the identity. On a
// 480x480 panel it is NOT uniform:
//
//     mapX = 480/240 = 2.00        mapY = 480/320 = 1.50
//
// Applied naively that stretches everything horizontally by a third relative to
// its height -- meters get fat, the kitsune gets stretched, and the bitmap font
// (which can only step in whole multiples) drifts out of register with the
// boxes it sits in. A square panel is not a bigger portrait panel.
//
// The intended fix is to scale uniformly by the tighter axis and centre the
// result, which on this panel means x1.50, a 360x480 live area, and 60px of
// unused margin down each side -- 75% of the glass. That is deliberate: correct
// proportions on three quarters of the panel beats a distorted layout on all of
// it, and it is the same choice the mascot already makes when it fits itself to
// the glass on both axes.
//
// Reclaiming those margins means designing square variants of each screen, not
// stretching the portrait ones. That is a design job, not a scaling constant.

#define DESIGN_W          240
#define DESIGN_H          320
