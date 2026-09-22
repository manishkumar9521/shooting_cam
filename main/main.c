#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_camera.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

static const char *TAG = "SHOOTING_CAM";

// Wi-Fi Configuration
#define WIFI_SSID       "ShootingCam_AP"
#define WIFI_PASSWORD   "12345678"
#define WIFI_MAX_CONN   4

// MJPEG Stream Configuration
#define PART_BOUNDARY "123456789000000000000987654321"
static const char *STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char *STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %zu\r\n\r\n";

// HTML UI Dashboard
static const char *INDEX_HTML =
    "<!doctype html><html lang=\"en\"><head>"
    "<meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1,viewport-fit=cover\">"
    "<meta name=\"theme-color\" content=\"#111827\">"
    "<title>Shooting Target Camera</title>"
    "<style>"
    "*{box-sizing:border-box}"
    "body{margin:0;background:#0b1220;color:#e5e7eb;font-family:system-ui,-apple-system,Segoe UI,Roboto,sans-serif}"
    ".app{width:min(960px,100%);margin:auto;padding:12px}"
    ".card{background:#111827;border:1px solid #263244;border-radius:14px;box-shadow:0 8px 28px rgba(0,0,0,.25)}"
    "header{padding:14px 16px;display:flex;align-items:center;justify-content:space-between;gap:12px}"
    "h1{font-size:20px;margin:0;font-weight:700}"
    ".sub{font-size:12px;color:#9ca3af;margin-top:3px}"
    ".dot{width:10px;height:10px;border-radius:50%;background:#6b7280;display:inline-block;box-shadow:0 0 0 3px rgba(107,114,128,.15)}"
    ".dot.on{background:#22c55e;box-shadow:0 0 0 3px rgba(34,197,94,.15)}"
    ".viewer{margin-top:12px;overflow:hidden;position:relative;background:#000;aspect-ratio:4/3;min-height:240px;display:flex;align-items:center;justify-content:center}"
    "#img-view{width:100%;height:100%;object-fit:contain;display:none;background:#000}"
    ".placeholder{position:absolute;inset:0;display:grid;place-items:center;color:#9ca3af;text-align:center;padding:20px}"
    ".placeholder b{display:block;color:#e5e7eb;margin-bottom:4px}"
    ".controls{display:grid;grid-template-columns:repeat(4,1fr);gap:8px;padding:12px}"
    "button{border:0;border-radius:10px;padding:11px 10px;font:inherit;font-weight:650;color:#fff;background:#2563eb;cursor:pointer}"
    "button:active{transform:translateY(1px)}"
    "button.capture{background:#dc2626}"
    "button.secondary{background:#374151}"
    "button:disabled{opacity:.5;cursor:not-allowed}"
    ".status{margin-top:12px;padding:12px 14px}"
    ".status-head{display:flex;align-items:center;justify-content:space-between;gap:8px;margin-bottom:8px}"
    ".status-head strong{font-size:14px}"
    "#status{margin:0;white-space:pre-wrap;word-break:break-word;font:12px/1.5 ui-monospace,SFMono-Regular,Menlo,monospace;color:#cbd5e1}"
    ".hint{padding:0 14px 14px;color:#6b7280;font-size:11px}"
    "@media(max-width:640px){.controls{grid-template-columns:repeat(2,1fr)}h1{font-size:18px}.viewer{aspect-ratio:4/3;min-height:210px}}"
    "</style></head><body>"
    "<main class=\"app\">"
    "<section class=\"card\">"
    "<header><div><h1>Shooting Target Camera</h1><div class=\"sub\">ESP32-S3 + OV5640</div></div>"
    "<span id=\"live-dot\" class=\"dot\" title=\"Stream status\"></span></header>"
    "<div class=\"viewer\">"
    "<img id=\"img-view\" alt=\"Camera feed\" decoding=\"async\"/>"
    "<div id=\"ph\" class=\"placeholder\"><div><b>No feed active</b>Press Live Stream to start the camera preview.</div></div>"
    "</div>"
    "<div class=\"controls\">"
    "<button id=\"stream-btn\" onclick=\"startStream()\">Live Stream</button>"
    "<button class=\"capture\" onclick=\"captureImage()\">High-Res Capture</button>"
    "<button class=\"secondary\" onclick=\"stopFeed()\">Stop</button>"
    "<button class=\"secondary\" onclick=\"refreshStatus()\">Refresh Status</button>"
    "</div>"
    "</section>"
    "<section class=\"card status\">"
    "<div class=\"status-head\"><strong>Device Status</strong><span id=\"status-time\" class=\"sub\"></span></div>"
    "<pre id=\"status\">Loading status...</pre>"
    "</section>"
    "<div class=\"hint\">All UI resources are embedded locally; no internet connection is required.</div>"
    "</main>"
    "<script>"
    "const img=document.getElementById('img-view');"
    "const ph=document.getElementById('ph');"
    "const dot=document.getElementById('live-dot');"
    "const btn=document.getElementById('stream-btn');"
    "let feed='';"
    "function showImage(){img.style.display='block';ph.style.display='none';}"
    "function startStream(){"
    "stopFeed();feed='stream';img.src='/stream?ts='+Date.now();showImage();"
    "dot.classList.add('on');btn.textContent='Streaming...';btn.disabled=true;"
    "}"
    "function captureImage(){"
    "stopFeed();feed='capture';img.src='/capture?ts='+Date.now();showImage();"
    "}"
    "function stopFeed(){"
    "if(feed){img.src='';feed='';}"
    "dot.classList.remove('on');btn.textContent='Live Stream';btn.disabled=false;"
    "}"
    "async function refreshStatus(){"
    "const box=document.getElementById('status');"
    "try{"
    "const r=await fetch('/status?ts='+Date.now(),{cache:'no-store'});"
    "if(!r.ok)throw new Error('HTTP '+r.status);"
    "const d=await r.json();box.textContent=JSON.stringify(d,null,2);"
    "document.getElementById('status-time').textContent=new Date().toLocaleTimeString();"
    "}catch(e){box.textContent='Status error: '+e.message;}"
    "}"
    "img.onerror=()=>{"
    "if(feed==='stream'){stopFeed();document.getElementById('status').textContent='Stream disconnected';}"
    "};"
    "window.addEventListener('beforeunload',stopFeed);"
    "refreshStatus();"
    "</script></body></html>";


// Global State
static int connected_devices = 0;
static SemaphoreHandle_t camera_mutex = NULL;

// ============================================================
// Camera Configuration & Helpers
// ============================================================

// OV5640 AEC/AGC tuning requested for this project.
#define CAMERA_AE_LEVEL                 1
#define CAMERA_GAIN_CEILING             GAINCEILING_8X
#define CAMERA_AEC2_ENABLED             0
#define CAMERA_AEC_ENABLED              1
#define CAMERA_AGC_ENABLED              1

// Fixed 50 Hz anti-flicker target. The exposure alignment period is 10 ms.
#define CAMERA_ANTIFLICKER_HZ           50
#define CAMERA_ANTIFLICKER_MAX_BANDS    4

static const char *framesize_to_string(framesize_t framesize)
{
    switch (framesize) {
        case FRAMESIZE_VGA:  return "VGA";
        case FRAMESIZE_UXGA: return "UXGA";
        default:             return "OTHER";
    }
}

static const char *gainceiling_to_string(gainceiling_t ceiling)
{
    switch (ceiling) {
        case GAINCEILING_2X:   return "2X";
        case GAINCEILING_4X:   return "4X";
        case GAINCEILING_8X:   return "8X";
        case GAINCEILING_16X:  return "16X";
        case GAINCEILING_32X:  return "32X";
        case GAINCEILING_64X:  return "64X";
        case GAINCEILING_128X: return "128X";
        default:               return "UNKNOWN";
    }
}

// Decode the PLL registers using the same relationships used by the OV5640
// driver. This lets the 50 Hz banding step be recalculated after a resolution
// change instead of assuming a single hard-coded timing value.
static bool ov5640_get_pclk_hz(sensor_t *s, uint32_t *pclk_hz)
{
    if (!s || !pclk_hz) return false;

    const int reg3035 = s->get_reg(s, 0x3035, 0xFF);
    const int reg3036 = s->get_reg(s, 0x3036, 0xFF);
    const int reg3037 = s->get_reg(s, 0x3037, 0xFF);
    const int reg3039 = s->get_reg(s, 0x3039, 0xFF);
    const int reg3108 = s->get_reg(s, 0x3108, 0xFF);
    const int reg3824 = s->get_reg(s, 0x3824, 0xFF);
    const int reg460c = s->get_reg(s, 0x460C, 0xFF);

    if (reg3035 < 0 || reg3036 < 0 || reg3037 < 0 || reg3039 < 0 ||
        reg3108 < 0 || reg3824 < 0 || reg460c < 0) {
        return false;
    }

    const uint32_t xclk_hz = (uint32_t)s->xclk_freq_hz;
    uint32_t multiplier = (uint32_t)reg3036;
    uint32_t sys_div = (uint32_t)((reg3035 >> 4) & 0x0F);
    uint32_t pre_div = (uint32_t)(reg3037 & 0x0F);
    bool root_2x = (reg3037 & 0x10) != 0;
    bool bypass = (reg3039 & 0x80) != 0;
    uint32_t pclk_root_sel = (uint32_t)((reg3108 >> 4) & 0x03);
    uint32_t pclk_div = (uint32_t)(reg3824 & 0x1F);
    bool pclk_manual = (reg460c & 0x02) != 0;

    if (sys_div == 0) sys_div = 1;
    if (multiplier < 4 || multiplier > 252) return false;

    // OV5640 pre-divider encodings used by the esp32-camera driver.
    uint32_t pre_num = 1;
    uint32_t pre_den = 1;
    switch (pre_div) {
        case 0: pre_num = 1; pre_den = 1; break;
        case 1: pre_num = 1; pre_den = 1; break;
        case 2: pre_num = 2; pre_den = 1; break;
        case 3: pre_num = 3; pre_den = 1; break;
        case 4: pre_num = 4; pre_den = 1; break;
        case 5: pre_num = 3; pre_den = 2; break;
        case 6: pre_num = 6; pre_den = 1; break;
        case 7: pre_num = 5; pre_den = 2; break;
        case 8: pre_num = 8; pre_den = 1; break;
        default: return false;
    }

    const uint64_t refin_hz = ((uint64_t)xclk_hz * pre_den) / pre_num;
    const uint64_t root_div = root_2x ? 2ULL : 1ULL;

    uint64_t pll_clk_hz;
    if (bypass) {
        pll_clk_hz = xclk_hz;
    } else {
        const uint64_t vco_hz = (refin_hz * multiplier) / root_div;
        // 5 follows the current esp32-camera OV5640 PLL calculation for 10-bit mode.
        pll_clk_hz = ((vco_hz / sys_div) * 2ULL) / 5ULL;
    }

    const uint32_t pclk_root_div = 1U << pclk_root_sel;
    const uint32_t final_pclk_div = (pclk_manual && pclk_div) ? pclk_div : 2U;
    const uint64_t calculated_pclk = pll_clk_hz / pclk_root_div / final_pclk_div;

    if (calculated_pclk == 0 || calculated_pclk > UINT32_MAX) return false;

    *pclk_hz = (uint32_t)calculated_pclk;
    return true;
}

static esp_err_t configure_50hz_antiflicker(sensor_t *s)
{
    if (!s || !s->get_reg || !s->set_reg) return ESP_ERR_INVALID_ARG;

    // HTS = total horizontal timing in pixel clocks; VTS = total frame timing.
    const int hts = s->get_reg(s, 0x380C, 0xFFFF);
    const int vts = s->get_reg(s, 0x380E, 0xFFFF);
    if (hts <= 0 || vts <= 0) {
        ESP_LOGE(TAG, "50Hz anti-flicker: failed to read HTS/VTS (HTS=%d VTS=%d)", hts, vts);
        return ESP_FAIL;
    }

    uint32_t pclk_hz = 0;
    if (!ov5640_get_pclk_hz(s, &pclk_hz)) {
        ESP_LOGE(TAG, "50Hz anti-flicker: unable to calculate current pixel clock");
        return ESP_FAIL;
    }

    // 50 Hz mains produces 100 Hz light-intensity modulation. We therefore
    // align the AEC banding step to 10 ms (1/100 s).
    const uint64_t modulation_hz = (uint64_t)CAMERA_ANTIFLICKER_HZ * 2ULL;
    uint32_t b50_step = (uint32_t)(((uint64_t)pclk_hz + ((modulation_hz / 2ULL) * hts)) /
                                   (modulation_hz * hts));
    if (b50_step < 1) b50_step = 1;
    if (b50_step > 0x3FF) b50_step = 0x3FF;

    // Force 50 Hz selection and disable automatic 50/60 Hz switching.
    // 0x3C00[2] = 0 selects 50 Hz; 0x3C01[7] = 1 disables auto detection.
    int ret = 0;
    ret |= s->set_reg(s, 0x3C00, 0x04, 0x00);
    ret |= s->set_reg(s, 0x3C01, 0x80, 0x80);

    // 0x3A00[5] enables the AEC banding function. AEC2/night mode is handled
    // separately by set_aec2(0).
    ret |= s->set_reg(s, 0x3A00, 0x20, 0x20);

    // Program the 50 Hz banding width and maximum number of bands.
    ret |= s->set_reg(s, 0x3A08, 0x03, (int)((b50_step >> 8) & 0x03));
    ret |= s->set_reg(s, 0x3A09, 0xFF, (int)(b50_step & 0xFF));
    ret |= s->set_reg(s, 0x3A0E, 0x3F, CAMERA_ANTIFLICKER_MAX_BANDS);

    // Limit AEC to a frame-valid exposure and no more than four 10 ms bands.
    uint32_t max_exposure = (uint32_t)vts;
    if (max_exposure > 4) max_exposure -= 4;
    const uint32_t band_limited_exposure = b50_step * CAMERA_ANTIFLICKER_MAX_BANDS;
    if (band_limited_exposure < max_exposure) max_exposure = band_limited_exposure;
    if (max_exposure > 0xFFFF) max_exposure = 0xFFFF;

    ret |= s->set_reg(s, 0x3A14, 0x0F, (int)((max_exposure >> 8) & 0x0F));
    ret |= s->set_reg(s, 0x3A15, 0xFF, (int)(max_exposure & 0xFF));

    if (ret != 0) {
        ESP_LOGE(TAG, "50Hz anti-flicker register programming failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG,
             "50Hz anti-flicker: PCLK=%lu Hz HTS=%d VTS=%d B50_STEP=%lu (~10ms) MAX_EXPO=%lu MAX_BANDS=%d",
             (unsigned long)pclk_hz, hts, vts, (unsigned long)b50_step,
             (unsigned long)max_exposure, CAMERA_ANTIFLICKER_MAX_BANDS);

    return ESP_OK;
}

static esp_err_t configure_exposure_pipeline(sensor_t *s)
{
    if (!s) return ESP_ERR_INVALID_STATE;

    // Keep auto exposure enabled.
    int ret = s->set_exposure_ctrl(s, CAMERA_AEC_ENABLED);
    if (ret != 0) return ESP_FAIL;

    // Disable AEC2/night mode.
    ret = s->set_aec2(s, CAMERA_AEC2_ENABLED);
    if (ret != 0) return ESP_FAIL;

    // Keep auto gain enabled.
    ret = s->set_gain_ctrl(s, CAMERA_AGC_ENABLED);
    if (ret != 0) return ESP_FAIL;

    // Shift the AE target one step brighter.
    ret = s->set_ae_level(s, CAMERA_AE_LEVEL);
    if (ret != 0) return ESP_FAIL;

    // Limit automatic gain to 8X.
    ret = s->set_gainceiling(s, CAMERA_GAIN_CEILING);
    if (ret != 0) return ESP_FAIL;

    return configure_50hz_antiflicker(s);
}

static void log_camera_state(const char *reason)
{
    sensor_t *s = esp_camera_sensor_get();
    if (!s) {
        ESP_LOGE(TAG, "Camera state [%s]: sensor handle unavailable", reason);
        return;
    }

    const int reg3a00 = s->get_reg(s, 0x3A00, 0xFF);
    const int reg3c00 = s->get_reg(s, 0x3C00, 0xFF);
    const int reg3c01 = s->get_reg(s, 0x3C01, 0xFF);
    const int b50_msb = s->get_reg(s, 0x3A08, 0x03);
    const int b50_lsb = s->get_reg(s, 0x3A09, 0xFF);
    const int max_bands = s->get_reg(s, 0x3A0E, 0x3F);
    const int max_exposure_hi = s->get_reg(s, 0x3A14, 0x0F);
    const int max_exposure_lo = s->get_reg(s, 0x3A15, 0xFF);
    const int b50_step = (b50_msb << 8) | b50_lsb;
    const int max_exposure = (max_exposure_hi << 8) | max_exposure_lo;

    uint16_t width = 0;
    uint16_t height = 0;
    switch (s->status.framesize) {
        case FRAMESIZE_VGA:  width = 640;  height = 480;  break;
        case FRAMESIZE_UXGA: width = 1600; height = 1200; break;
        default: break;
    }

    ESP_LOGI(TAG,
             "Camera state [%s]: %s %ux%u Q%d | AEC=%d AGC=%d AEC2=%d AE=%d GainCeil=%s | "
             "50Hz=%d AutoDetect=%d BandFn=%d B50=%d MaxBands=%d MaxExpo=%d | HMirror=%d VFlip=%d",
             reason, framesize_to_string(s->status.framesize), width, height, s->status.quality,
             s->status.aec, s->status.agc, s->status.aec2, s->status.ae_level,
             gainceiling_to_string((gainceiling_t)s->status.gainceiling),
             (reg3c00 >= 0) ? ((reg3c00 & 0x04) == 0) : -1,
             (reg3c01 >= 0) ? ((reg3c01 & 0x80) == 0) : -1,
             (reg3a00 >= 0) ? ((reg3a00 & 0x20) != 0) : -1,
             b50_step, max_bands, max_exposure, s->status.hmirror, s->status.vflip);
}

static esp_err_t apply_camera_settings(framesize_t framesize, int quality)
{
    sensor_t *s = esp_camera_sensor_get();
    if (!s) return ESP_ERR_INVALID_STATE;

    const bool resolution_changed = (s->status.framesize != framesize);
    const bool quality_changed = (s->status.quality != quality);

    if (!resolution_changed && !quality_changed) {
        // Profile already active: do not flush and do not disturb AEC/AGC.
        log_camera_state("profile-unchanged");
        return ESP_OK;
    }

    if (resolution_changed) {
        ESP_LOGI(TAG, "Changing camera resolution: %s -> %s",
                 framesize_to_string(s->status.framesize), framesize_to_string(framesize));
        if (s->set_framesize(s, framesize) != 0) {
            ESP_LOGE(TAG, "Failed to set camera framesize=%d", framesize);
            return ESP_FAIL;
        }
    }

    if (quality_changed) {
        ESP_LOGI(TAG, "Changing JPEG quality: %d -> %d", s->status.quality, quality);
        if (s->set_quality(s, quality) != 0) {
            ESP_LOGE(TAG, "Failed to set JPEG quality=%d", quality);
            return ESP_FAIL;
        }
    }

    // Resolution changes alter PLL/timing, so recalculate 50 Hz anti-flicker
    // after the new sensor timing is active.
    if (resolution_changed) {
        if (configure_exposure_pipeline(s) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to reconfigure AEC/AGC/50Hz anti-flicker");
            return ESP_FAIL;
        }
    }

    ESP_LOGI(TAG, "Camera profile active: %s / JPEG Q%d",
             framesize_to_string(framesize), quality);

    // Flush exactly ONE frame after a resolution change. Do not flush for
    // JPEG-quality-only changes.
    if (resolution_changed) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb) {
            ESP_LOGI(TAG, "Flushed one frame after resolution change: %ux%u",
                     fb->width, fb->height);
            esp_camera_fb_return(fb);
        } else {
            ESP_LOGW(TAG, "Resolution changed but first flush frame was unavailable");
        }
    }

    log_camera_state("profile-applied");
    return ESP_OK;
}

static esp_err_t camera_init(void)
{
    camera_config_t config = {
        .pin_pwdn     = -1,
        .pin_reset    = -1,
        .pin_xclk     = 38,
        .pin_sccb_sda = 8,
        .pin_sccb_scl = 7,
        .pin_d7       = 21,
        .pin_d6       = 39,
        .pin_d5       = 40,
        .pin_d4       = 42,
        .pin_d3       = 46,
        .pin_d2       = 48,
        .pin_d1       = 47,
        .pin_d0       = 45,
        .pin_vsync    = 17,
        .pin_href     = 18,
        .pin_pclk     = 41,

        .xclk_freq_hz = 20000000,
        .ledc_timer   = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,
        .pixel_format = PIXFORMAT_JPEG,
        
        // UXGA (1600x1200) keeps still capture at 2MP instead of 5MP/QSXGA.
        .frame_size   = FRAMESIZE_UXGA,
        .jpeg_quality = 5,
        
        .fb_count     = 2,
        .fb_location  = CAMERA_FB_IN_PSRAM,
        .grab_mode    = CAMERA_GRAB_LATEST,
    };

    ESP_LOGI(TAG, "Initializing OV5640 camera...");
    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera initialization failed: 0x%x", err);
        return err;
    }

    // Mirror the sensor horizontally so the image is flipped left <-> right.
    // This affects both /stream and /capture without any extra JPEG processing.
    sensor_t *s = esp_camera_sensor_get();
    if (!s) {
        ESP_LOGE(TAG, "Sensor handle unavailable after camera init");
        return ESP_ERR_INVALID_STATE;
    }

    s->set_hmirror(s, 1);
    s->set_vflip(s, 0);
    ESP_LOGI(TAG, "Camera orientation: horizontal mirror=ON, vertical flip=OFF");

    if (configure_exposure_pipeline(s) != ESP_OK) {
        ESP_LOGE(TAG, "Camera exposure/anti-flicker configuration failed");
        esp_camera_deinit();
        return ESP_FAIL;
    }

    log_camera_state("init");
    return ESP_OK;
}

// ============================================================
// HTTP Endpoint Handlers
// ============================================================

// GET /
static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

// GET /status
static esp_err_t status_get_handler(httpd_req_t *req)
{
    char response[768];
    sensor_t *s = esp_camera_sensor_get();

    if (!s) {
        snprintf(response, sizeof(response),
                 "{\"device\":\"ESP32-S3\",\"project\":\"Shooting Target Camera\",\"wifi_mode\":\"AP\",\"connected_devices\":%d,\"camera_status\":\"error\"}",
                 connected_devices);
    } else {
        const int reg3c00 = s->get_reg(s, 0x3C00, 0xFF);
        const int reg3c01 = s->get_reg(s, 0x3C01, 0xFF);
        const int b50_step = (s->get_reg(s, 0x3A08, 0x03) << 8) |
                             s->get_reg(s, 0x3A09, 0xFF);

        snprintf(response, sizeof(response),
            "{"
            "\"device\":\"ESP32-S3\","
            "\"project\":\"Shooting Target Camera\","
            "\"wifi_mode\":\"AP\","
            "\"connected_devices\":%d,"
            "\"camera_status\":\"ready\","
            "\"framesize\":\"%s\","
            "\"jpeg_quality\":%d,"
            "\"aec\":%d,"
            "\"agc\":%d,"
            "\"aec2\":%d,"
            "\"ae_level\":%d,"
            "\"gain_ceiling\":\"%s\","
            "\"anti_flicker\":\"50Hz\","
            "\"banding_50hz_step\":%d,"
            "\"banding_50hz_manual\":%s,"
            "\"horizontal_mirror\":%s,"
            "\"stream_profile\":\"VGA / JPEG Q20\","
            "\"capture_profile\":\"UXGA / JPEG Q5\""
            "}",
            connected_devices,
            framesize_to_string(s->status.framesize),
            s->status.quality,
            s->status.aec,
            s->status.agc,
            s->status.aec2,
            s->status.ae_level,
            gainceiling_to_string((gainceiling_t)s->status.gainceiling),
            b50_step,
            (reg3c00 >= 0 && reg3c01 >= 0 && ((reg3c00 & 0x04) == 0) && ((reg3c01 & 0x80) != 0)) ? "true" : "false",
            s->status.hmirror ? "true" : "false"
        );
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
}

// GET /stream (Basic Quality, Basic Size)
static esp_err_t stream_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "GET /stream requested");

    esp_err_t res = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
    if (res != ESP_OK) return res;

    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    // Configure the streaming profile once, not on every frame.
    xSemaphoreTake(camera_mutex, portMAX_DELAY);
    esp_err_t profile_err = apply_camera_settings(FRAMESIZE_VGA, 20);
    xSemaphoreGive(camera_mutex);
    if (profile_err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to configure stream profile");
        return ESP_FAIL;
    }

    while (true) {
        xSemaphoreTake(camera_mutex, portMAX_DELAY);
        camera_fb_t *fb = esp_camera_fb_get();
        xSemaphoreGive(camera_mutex);

        if (!fb) {
            ESP_LOGE(TAG, "Camera capture failed during stream");
            res = ESP_FAIL;
            break;
        }

        // Do not keep the camera mutex while sending the JPEG over Wi-Fi.
        // This avoids blocking other camera operations on a slow HTTP client.
        res = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
        if (res == ESP_OK) {
            char part_buf[128];
            int hlen = snprintf(part_buf, sizeof(part_buf), STREAM_PART, fb->len);
            res = httpd_resp_send_chunk(req, part_buf, hlen);
        }
        if (res == ESP_OK) {
            res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
        }

        esp_camera_fb_return(fb);

        if (res != ESP_OK) {
            ESP_LOGI(TAG, "Stream client disconnected");
            break;
        }
    }
    return res;
}

// GET /capture (High Quality, Highest Size)
static esp_err_t capture_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "GET /capture requested");

    xSemaphoreTake(camera_mutex, portMAX_DELAY);
    esp_err_t profile_err = apply_camera_settings(FRAMESIZE_UXGA, 5);
    if (profile_err != ESP_OK) {
        xSemaphoreGive(camera_mutex);
        ESP_LOGE(TAG, "Failed to configure capture profile");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to configure capture profile");
        return ESP_FAIL;
    }

    camera_fb_t *fb = esp_camera_fb_get();
    xSemaphoreGive(camera_mutex);

    if (!fb) {
        ESP_LOGE(TAG, "Camera capture failed");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Camera capture failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Captured image: %ux%u, %u bytes", fb->width, fb->height, (unsigned)fb->len);

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

    esp_err_t ret = httpd_resp_send(req, (const char *)fb->buf, fb->len);
    esp_camera_fb_return(fb);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send image: %s", esp_err_to_name(ret));
        return ret;
    }

    return ESP_OK;
}

// ============================================================
// Wi-Fi Initialization & Events
// ============================================================

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base != WIFI_EVENT) return;

    if (event_id == WIFI_EVENT_AP_START) {
        ESP_LOGI(TAG, "Wi-Fi AP started. SSID: %s | IP: 192.168.4.1", WIFI_SSID);
    } 
    else if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        connected_devices++;
    } 
    else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        if (connected_devices > 0) connected_devices--;
    }
}

static void wifi_init_softap(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = WIFI_SSID,
            .ssid_len = strlen(WIFI_SSID),
            .channel = 1,
            .password = WIFI_PASSWORD,
            .max_connection = WIFI_MAX_CONN,
            .authmode = WIFI_AUTH_WPA2_PSK
        }
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
}

// ============================================================
// HTTP Server Initialization
// ============================================================

static httpd_handle_t start_webserver(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;

    ESP_LOGI(TAG, "Starting HTTP server...");
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return NULL;
    }

    httpd_uri_t root_uri    = { .uri = "/",        .method = HTTP_GET, .handler = root_get_handler,   .user_ctx = NULL };
    httpd_uri_t status_uri  = { .uri = "/status",  .method = HTTP_GET, .handler = status_get_handler, .user_ctx = NULL };
    httpd_uri_t stream_uri  = { .uri = "/stream",  .method = HTTP_GET, .handler = stream_handler,     .user_ctx = NULL };
    httpd_uri_t capture_uri = { .uri = "/capture", .method = HTTP_GET, .handler = capture_handler,    .user_ctx = NULL };

    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &root_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &status_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &stream_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &capture_uri));

    ESP_LOGI(TAG, "HTTP server started (Endpoints: /, /status, /stream, /capture)");
    return server;
}

// ============================================================
// Application Entry Point
// ============================================================
void app_main(void)
{
    ESP_LOGI(TAG, "====================================");
    ESP_LOGI(TAG, "     Shooting Target Camera");
    ESP_LOGI(TAG, "     ESP32-S3 + OV5640");
    ESP_LOGI(TAG, "====================================");

    camera_mutex = xSemaphoreCreateMutex();

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    if (camera_init() != ESP_OK) return;

    wifi_init_softap();
    start_webserver();

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}