/* FastEPD glue for the M5Stack PaperS3 (third_party/FastEPD).
 *
 * FastEPD is a C++ library; this is the only C++ translation unit in the app.
 * It owns the single FASTEPD instance and exposes plain extern-"C" entry points
 * (see eink_fastepd.h) so the rest of the (C) project can drive the panel.
 *
 * The PaperS3 panel is selected by BB_PANEL_M5PAPERS3 — FastEPD brings up the
 * i80 bus + bit-banged gate driver + DC/DC boost itself. NOTE: FastEPD's stock
 * M5PaperS3 def uses GPIO47 as the i80 dummy-DC pin, which collides with this
 * board's SD chip-select (BOARD_SD_CS=47); the vendored def is locally patched
 * to use GPIO49 instead (see third_party/FastEPD/src/FastEPD.inl). */

#include "FastEPD.h"
#include "esp_log.h"
#include "eink_fastepd.h"

static const char *TAG = "fastepd";
static FASTEPD s_epd;

extern "C" bool eink_fastepd_bringup(void)
{
    int rc = s_epd.initPanel(BB_PANEL_M5PAPERS3, 20000000);
    ESP_LOGI(TAG, "initPanel(BB_PANEL_M5PAPERS3) rc=%d (0 = BBEP_SUCCESS)", rc);
    if (rc != BBEP_SUCCESS) {
        ESP_LOGE(TAG, "FastEPD init failed");
        return false;
    }
    s_epd.setRotation(0);
    ESP_LOGI(TAG, "logical panel = %d x %d", (int)s_epd.width(), (int)s_epd.height());

    /* Clear the panel to white (a flickering full update). */
    s_epd.fillScreen(BBEP_WHITE);
    s_epd.fullUpdate(CLEAR_SLOW, /*bKeepOn=*/true, NULL);

    /* Draw a visible test image so the bring-up is obvious on the panel. */
    s_epd.fillRect(40, 40, 320, 140, BBEP_BLACK);
    s_epd.setFont(FONT_12x16);
    s_epd.setTextColor(BBEP_BLACK);
    s_epd.drawString("FastEPD PaperS3 bring-up OK", 40, 220);
    s_epd.fullUpdate(CLEAR_SLOW, /*bKeepOn=*/true, NULL);

    ESP_LOGI(TAG, "bring-up complete");
    return true;
}
