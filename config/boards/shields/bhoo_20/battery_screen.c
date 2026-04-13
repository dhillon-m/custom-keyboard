/*
 * BHOO-20 OLED battery display
 * 128×64 SSD1306, mounted upside-down (rotated via segment-remap + com-invdir).
 *
 * Layout (coordinates as seen on-screen after hardware rotation):
 *
 *   Bar 1 (BHOO-75, top):    y=1..26   outer rect  x=0..102
 *   Bar 2 (BHOO-20, bottom): y=37..62  outer rect  x=0..102
 *   Inner fill area: x=2, width=<pct>px (1px per %), height=22px
 *   Number zone:  x=106..127 — two 5×9 glyphs or centred 7×9 lightning bolt
 *
 * BHOO-20 battery level comes from zmk_battery_state_changed (local device).
 * BHOO-75 battery level is received via a custom split BLE service defined in
 * central_battery_service.c on the BHOO-75 side (future work).  For now the
 * top bar initialises at 0 and updates via zmk_split_central_battery_level_changed
 * if that event is ever wired up; otherwise it stays at whatever was last set.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <lvgl.h>
#include <zmk/display.h>
#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* ── Layout constants ─────────────────────────────────────────────────── */
#define DISP_W        128
#define DISP_H         64

#define BAR_OUTER_X     0
#define BAR_OUTER_W   103   /* x=0..102: 1px border + 100px fill + 1px border + 1px right */
#define BAR_INNER_X     2   /* fill left edge inside border */
#define BAR_MAX_W     100   /* 1 pixel per 1% */

#define BAR1_Y          1
#define BAR2_Y         37
#define BAR_OUTER_H    26
#define BAR_INNER_Y_OFF 2
#define BAR_INNER_H    22

#define NUM_X         106   /* number zone left edge */
#define GLYPH_W         5
#define GLYPH_H         9
#define GLYPH_GAP       1

/* Bolt is 7 wide, centred in the 22px number zone (x=106..127):
 * centre = 106 + 22/2 = 117; bolt_x = 117 - 7/2 = 113 */
#define BOLT_W          7
#define BOLT_X        113

/* ── Bitmap font: 5 wide × 9 tall ─────────────────────────────────────── */
/* Each byte is a row; bit 4 = leftmost column. */
static const uint8_t FONT_5X9[10][GLYPH_H] = {
    /* 0 */ { 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E },
    /* 1 */ { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E },
    /* 2 */ { 0x0E, 0x11, 0x01, 0x01, 0x06, 0x08, 0x10, 0x10, 0x1F },
    /* 3 */ { 0x0E, 0x11, 0x01, 0x01, 0x06, 0x01, 0x01, 0x11, 0x0E },
    /* 4 */ { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02, 0x02, 0x02 },
    /* 5 */ { 0x1F, 0x10, 0x10, 0x1E, 0x01, 0x01, 0x01, 0x11, 0x0E },
    /* 6 */ { 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x11, 0x11, 0x0E },
    /* 7 */ { 0x1F, 0x01, 0x01, 0x02, 0x04, 0x04, 0x08, 0x08, 0x08 },
    /* 8 */ { 0x0E, 0x11, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x11, 0x0E },
    /* 9 */ { 0x0E, 0x11, 0x11, 0x11, 0x0F, 0x01, 0x01, 0x02, 0x0C },
};

/* Lightning bolt: 7 wide × 9 tall; bit 6 = leftmost column */
static const uint8_t BOLT_BITMAP[GLYPH_H] = {
    0x1C, /* ..###.. */
    0x18, /* ..##... */
    0x10, /* ..#.... */
    0x3F, /* .###### */
    0x0C, /* ...##.. */
    0x06, /* ....##. */
    0x03, /* .....## */
    0x01, /* ......# */
    0x00,
};

/* ── Canvas buffer (1-bit monochrome) ───────────────────────────────── */
#define CANVAS_BUF_SIZE LV_CANVAS_BUF_SIZE_INDEXED_1BIT(DISP_W, DISP_H)
static lv_color_t canvas_buf[CANVAS_BUF_SIZE];

static lv_obj_t *canvas;
static uint8_t   batt_75 = 0;
static uint8_t   batt_20 = 0;

static lv_color_t col_white;
static lv_color_t col_black;

/* ── Drawing helpers ─────────────────────────────────────────────────── */
static void px(int x, int y, lv_color_t c)
{
    if (x >= 0 && x < DISP_W && y >= 0 && y < DISP_H) {
        lv_canvas_set_px_color(canvas, x, y, c);
    }
}

static void fill_rect(int x, int y, int w, int h, lv_color_t c)
{
    for (int row = y; row < y + h; row++) {
        for (int col = x; col < x + w; col++) {
            px(col, row, c);
        }
    }
}

static void draw_outline(int x, int y, int w, int h, lv_color_t c)
{
    for (int i = x; i < x + w; i++) {
        px(i, y,         c);
        px(i, y + h - 1, c);
    }
    for (int j = y + 1; j < y + h - 1; j++) {
        px(x,         j, c);
        px(x + w - 1, j, c);
    }
}

static void draw_digit(int x, int y, uint8_t d)
{
    if (d > 9) return;
    for (int row = 0; row < GLYPH_H; row++) {
        for (int col = 0; col < GLYPH_W; col++) {
            if (FONT_5X9[d][row] & (1u << (GLYPH_W - 1 - col))) {
                px(x + col, y + row, col_white);
            }
        }
    }
}

static void draw_bolt(int x, int y)
{
    for (int row = 0; row < GLYPH_H; row++) {
        for (int col = 0; col < BOLT_W; col++) {
            if (BOLT_BITMAP[row] & (1u << (BOLT_W - 1 - col))) {
                px(x + col, y + row, col_white);
            }
        }
    }
}

static void draw_number_zone(uint8_t pct, int bar_y)
{
    int text_y = bar_y + BAR_INNER_Y_OFF + (BAR_INNER_H - GLYPH_H) / 2;

    fill_rect(NUM_X, bar_y, DISP_W - NUM_X, BAR_OUTER_H, col_black);

    if (pct >= 100) {
        draw_bolt(BOLT_X, text_y);
    } else {
        uint8_t tens = pct / 10;
        uint8_t ones = pct % 10;
        if (tens > 0) {
            draw_digit(NUM_X, text_y, tens);
        }
        draw_digit(NUM_X + GLYPH_W + GLYPH_GAP, text_y, ones);
    }
}

static void redraw(void)
{
    fill_rect(0, 0, DISP_W, DISP_H, col_black);

    /* Bar 1: BHOO-75 */
    draw_outline(BAR_OUTER_X, BAR1_Y, BAR_OUTER_W, BAR_OUTER_H, col_white);
    if (batt_75 > 0) {
        uint8_t fill = batt_75 > 100 ? 100 : batt_75;
        fill_rect(BAR_INNER_X, BAR1_Y + BAR_INNER_Y_OFF, fill, BAR_INNER_H, col_white);
    }
    draw_number_zone(batt_75, BAR1_Y);

    /* Bar 2: BHOO-20 */
    draw_outline(BAR_OUTER_X, BAR2_Y, BAR_OUTER_W, BAR_OUTER_H, col_white);
    if (batt_20 > 0) {
        uint8_t fill = batt_20 > 100 ? 100 : batt_20;
        fill_rect(BAR_INNER_X, BAR2_Y + BAR_INNER_Y_OFF, fill, BAR_INNER_H, col_white);
    }
    draw_number_zone(batt_20, BAR2_Y);
}

/* ── Battery event listener ──────────────────────────────────────────── */
/*
 * zmk_battery_state_changed fires for the local device's battery.
 * This updates the BHOO-20 (bottom) bar.
 * BHOO-75 top bar updates via zmk_split_central_battery_changed defined below.
 */
static int local_battery_cb(const zmk_event_t *eh)
{
    const struct zmk_battery_state_changed *ev = as_zmk_battery_state_changed(eh);
    if (!ev) return 0;
    batt_20 = ev->state_of_charge;
    redraw();
    return 0;
}

ZMK_LISTENER(batt_local, local_battery_cb);
ZMK_SUBSCRIPTION(batt_local, zmk_battery_state_changed);

/* ── Public API: central can call this to push BHOO-75 battery level ── */
void bhoo_battery_set_central(uint8_t pct)
{
    batt_75 = pct;
    redraw();
}

/* ── Custom screen entry point ───────────────────────────────────────── */
lv_obj_t *zmk_display_status_screen(void)
{
    lv_obj_t *screen = lv_obj_create(NULL);

    col_white = lv_color_white();
    col_black = lv_color_black();

    canvas = lv_canvas_create(screen);
    lv_canvas_set_buffer(canvas, canvas_buf, DISP_W, DISP_H,
                         LV_IMG_CF_INDEXED_1BIT);
    lv_canvas_set_palette(canvas, 0, col_black);
    lv_canvas_set_palette(canvas, 1, col_white);
    lv_obj_set_pos(canvas, 0, 0);

    redraw();
    return screen;
}
