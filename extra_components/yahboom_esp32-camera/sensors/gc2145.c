// Copyright 2015-2021 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at

//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sccb.h"
#include "gc2145.h"
#include "gc2145_regs.h"
#include "gc2145_settings.h"

#if defined(ARDUINO_ARCH_ESP32) && defined(CONFIG_ARDUHAL_ESP_LOG)
#include "esp32-hal-log.h"
#else
#include "esp_log.h"
static const char *TAG = "gc2145";
#endif

#define H8(v) ((v)>>8)
#define L8(v) ((v)&0xff)

//#define REG_DEBUG_ON

static int read_reg(uint8_t slv_addr, const uint16_t reg)
{
    int ret = SCCB_Read(slv_addr, reg);
#ifdef REG_DEBUG_ON
    if (ret < 0) {
        ESP_LOGE(TAG, "READ REG 0x%04x FAILED: %d", reg, ret);
    }
#endif
    return ret;
}

static int write_reg(uint8_t slv_addr, const uint16_t reg, uint8_t value)
{
    int ret = 0;
#ifndef REG_DEBUG_ON
    ret = SCCB_Write(slv_addr, reg, value);
#else
    int old_value = read_reg(slv_addr, reg);
    if (old_value < 0) {
        return old_value;
    }
    if ((uint8_t)old_value != value) {
        ESP_LOGI(TAG, "NEW REG 0x%04x: 0x%02x to 0x%02x", reg, (uint8_t)old_value, value);
        ret = SCCB_Write(slv_addr, reg, value);
    } else {
        ESP_LOGD(TAG, "OLD REG 0x%04x: 0x%02x", reg, (uint8_t)old_value);
        ret = SCCB_Write(slv_addr, reg, value);//maybe not?
    }
    if (ret < 0) {
        ESP_LOGE(TAG, "WRITE REG 0x%04x FAILED: %d", reg, ret);
    }
#endif
    return ret;
}

static int check_reg_mask(uint8_t slv_addr, uint16_t reg, uint8_t mask)
{
    return (read_reg(slv_addr, reg) & mask) == mask;
}

static int set_reg_bits(uint8_t slv_addr, uint16_t reg, uint8_t offset, uint8_t mask, uint8_t value)
{
    int ret = 0;
    uint8_t c_value, new_value;
    ret = read_reg(slv_addr, reg);
    if (ret < 0) {
        return ret;
    }
    c_value = ret;
    new_value = (c_value & ~(mask << offset)) | ((value & mask) << offset);
    ret = write_reg(slv_addr, reg, new_value);
    return ret;
}

static int write_regs(uint8_t slv_addr, const uint16_t (*regs)[2])
{
    int i = 0, ret = 0;
    while (!ret && regs[i][0] != REGLIST_TAIL) {
        if (regs[i][0] == REG_DLY) {
            vTaskDelay(regs[i][1] / portTICK_PERIOD_MS);
        } else {
            ret = write_reg(slv_addr, regs[i][0], regs[i][1]);
        }
        i++;
    }
    return ret;
}

static void print_regs(uint8_t slv_addr)
{
#ifdef DEBUG_PRINT_REG
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGI(TAG, "REG list look ======================");
    for (size_t i = 0xf0; i <= 0xfe; i++) {
        ESP_LOGI(TAG, "reg[0x%02x] = 0x%02x", i, read_reg(slv_addr, i));
    }
    ESP_LOGI(TAG, "\npage 0 ===");
    write_reg(slv_addr, 0xfe, 0x00); // page 0
    for (size_t i = 0x03; i <= 0x24; i++) {
        ESP_LOGI(TAG, "p0 reg[0x%02x] = 0x%02x", i, read_reg(slv_addr, i));
    }
    for (size_t i = 0x80; i <= 0xa2; i++) {
        ESP_LOGI(TAG, "p0 reg[0x%02x] = 0x%02x", i, read_reg(slv_addr, i));
    }
    ESP_LOGI(TAG, "\npage 3 ===");
    write_reg(slv_addr, 0xfe, 0x03); // page 3
    for (size_t i = 0x01; i <= 0x43; i++) {
        ESP_LOGI(TAG, "p3 reg[0x%02x] = 0x%02x", i, read_reg(slv_addr, i));
    }
#endif
}

static int reset(sensor_t *sensor)
{
    int ret = 0;
    // Software Reset: clear all registers and reset them to their default values
    ret = write_reg(sensor->slv_addr, RESET_RELATED, 0xe0);
    if (ret) {
        ESP_LOGE(TAG, "Software Reset FAILED!");
        return ret;
    }
    vTaskDelay(100 / portTICK_PERIOD_MS);
    ret = write_regs(sensor->slv_addr, gc2145_default_init_regs);
    if (ret == 0) {
        ESP_LOGD(TAG, "Camera defaults loaded");
        vTaskDelay(100 / portTICK_PERIOD_MS);
#ifdef CONFIG_IDF_TARGET_ESP32
        write_reg(sensor->slv_addr, 0xfe, 0x00);
        //ensure pclk <= 15MHz for esp32
        set_reg_bits(sensor->slv_addr, 0xf8, 0, 0x3f, 2); // divx4
        set_reg_bits(sensor->slv_addr, 0xfa, 4, 0x0f, 2); // divide_by
#endif

    }
    return ret;
}

static int set_pixformat(sensor_t *sensor, pixformat_t pixformat)
{
    int ret = 0;

    switch (pixformat) {
    case PIXFORMAT_RGB565:
        write_reg(sensor->slv_addr, 0xfe, 0x00);
        ret = set_reg_bits(sensor->slv_addr, P0_OUTPUT_FORMAT, 0, 0x1f, 6);  //RGB565
        break;

    case PIXFORMAT_YUV422:
        write_reg(sensor->slv_addr, 0xfe, 0x00);
        ret = set_reg_bits(sensor->slv_addr, P0_OUTPUT_FORMAT, 0, 0x1f, 2); //yuv422
        break;
    default:
        ESP_LOGW(TAG, "unsupport format");
        ret = -1;
        break;
    }

    if (ret == 0) {
        sensor->pixformat = pixformat;
        ESP_LOGD(TAG, "Set pixformat to: %u", pixformat);
    }
    return ret;
}

static int set_framesize(sensor_t *sensor, framesize_t framesize)
{
    int ret = 0;
    if (framesize > FRAMESIZE_VGA) {
        ESP_LOGW(TAG, "Invalid framesize: %u", framesize);
        framesize = FRAMESIZE_VGA;
    }
    sensor->status.framesize = framesize;
    uint16_t w = resolution[framesize].width;
    uint16_t h = resolution[framesize].height;
    uint16_t row_s =(resolution[FRAMESIZE_VGA].height - h) / 2;
    uint16_t col_s = (resolution[FRAMESIZE_VGA].width - w) / 2;
    (void)row_s;
    (void)col_s;

#if CONFIG_GC_SENSOR_SUBSAMPLE_MODE
    struct subsample_cfg {
        uint16_t ratio_numerator;
        uint16_t ratio_denominator;
        uint8_t reg0x99;
        uint8_t reg0x9b;
        uint8_t reg0x9c;
        uint8_t reg0x9d;
        uint8_t reg0x9e;
        uint8_t reg0x9f;
        uint8_t reg0xa0;
        uint8_t reg0xa1;
        uint8_t reg0xa2;
    };
    const struct subsample_cfg subsample_cfgs[] = { // define some subsample ratio
        // {60, 420, 0x77, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, //1/7 // A smaller ratio brings a larger view, but it reduces the frame rate
        // {84, 420, 0x55, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, //1/5
        // {105, 420, 0x44, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},//1/4
        {140, 420, 0x33, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},//1/3
        {210, 420, 0x22, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},//1/2
        {240, 420, 0x77, 0x02, 0x46, 0x02, 0x46, 0x02, 0x46, 0x02, 0x46},//4/7
        {252, 420, 0x55, 0x02, 0x04, 0x02, 0x04, 0x02, 0x04, 0x02, 0x04},//3/5
        {280, 420, 0x33, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00},//2/3
        {420, 420, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},//1/1
    };
    uint16_t win_w = resolution[FRAMESIZE_VGA].width;
    uint16_t win_h = resolution[FRAMESIZE_VGA].height;
    const struct subsample_cfg *cfg = NULL;
    /**
     * Strategy: try to keep the maximum perspective
     */
    uint8_t i = 0;
    if (framesize >= FRAMESIZE_VGA) {
        i = 1;
    }
    for (; i < sizeof(subsample_cfgs) / sizeof(struct subsample_cfg); i++) {
        cfg = &subsample_cfgs[i];
        if ((win_w * cfg->ratio_numerator / cfg->ratio_denominator >= w) && (win_h * cfg->ratio_numerator / cfg->ratio_denominator >= h)) {
            win_w = w * cfg->ratio_denominator / cfg->ratio_numerator;
            win_h = h * cfg->ratio_denominator / cfg->ratio_numerator;
            row_s =(resolution[FRAMESIZE_VGA].height - win_h) / 2;
            col_s =(resolution[FRAMESIZE_VGA].width - win_w) / 2;
            ESP_LOGI(TAG, "subsample win:%dx%d, ratio:%f", win_w, win_h, (float)cfg->ratio_numerator / (float)cfg->ratio_denominator);
            break;
        }
    }

/*
    write_reg(sensor->slv_addr, 0xfe, 0x00);
    write_reg(sensor->slv_addr, P0_CROP_ENABLE, 0x01);
    write_reg(sensor->slv_addr, 0x09, H8(row_s));
    write_reg(sensor->slv_addr, 0x0a, L8(row_s));
    write_reg(sensor->slv_addr, 0x0b, H8(col_s));
    write_reg(sensor->slv_addr, 0x0c, L8(col_s));
    write_reg(sensor->slv_addr, 0x0d, H8(win_h + 8));
    write_reg(sensor->slv_addr, 0x0e, L8(win_h + 8));
    write_reg(sensor->slv_addr, 0x0f, H8(win_w + 16));
    write_reg(sensor->slv_addr, 0x10, L8(win_w + 16));

    write_reg(sensor->slv_addr, 0x99, cfg->reg0x99);
    write_reg(sensor->slv_addr, 0x9b, cfg->reg0x9b);
    write_reg(sensor->slv_addr, 0x9c, cfg->reg0x9c);
    write_reg(sensor->slv_addr, 0x9d, cfg->reg0x9d);
    write_reg(sensor->slv_addr, 0x9e, cfg->reg0x9e);
    write_reg(sensor->slv_addr, 0x9f, cfg->reg0x9f);
    write_reg(sensor->slv_addr, 0xa0, cfg->reg0xa0);
    write_reg(sensor->slv_addr, 0xa1, cfg->reg0xa1);
    write_reg(sensor->slv_addr, 0xa2, cfg->reg0xa2);

    write_reg(sensor->slv_addr, 0x95, H8(h));
    write_reg(sensor->slv_addr, 0x96, L8(h));
    write_reg(sensor->slv_addr, 0x97, H8(w));
    write_reg(sensor->slv_addr, 0x98, L8(w));
*/

    //my test 固定使用320*240 摄像头画面调节中间
    write_reg(sensor->slv_addr, 0xfe, 0x00);
    write_reg(sensor->slv_addr, 0xfd, 0x01);
    write_reg(sensor->slv_addr, 0xfa, 0x00);

    write_reg(sensor->slv_addr, 0xfe, 0x00);
    write_reg(sensor->slv_addr, 0x99, 0x22);
    write_reg(sensor->slv_addr, 0x90, 0x01);
    write_reg(sensor->slv_addr, 0x91, 0x00);
    write_reg(sensor->slv_addr, 0x92, 0x1e);

    write_reg(sensor->slv_addr, 0x93, 0x00);
    write_reg(sensor->slv_addr, 0x94, 0x28);
    write_reg(sensor->slv_addr, 0x95, 0x00);
    write_reg(sensor->slv_addr, 0x96, 0xf0);
    write_reg(sensor->slv_addr, 0x97, 0x01);
    write_reg(sensor->slv_addr, 0x98, 0x40);
    write_reg(sensor->slv_addr, 0x9a, 0x06);
    write_reg(sensor->slv_addr, 0x00, 0x00);


#elif CONFIG_GC_SENSOR_WINDOWING_MODE
    write_reg(sensor->slv_addr, 0xfe, 0x00);

    write_reg(sensor->slv_addr, P0_CROP_ENABLE, 0x01);
    // write_reg(sensor->slv_addr, 0xec, col_s / 8); //measure window
    // write_reg(sensor->slv_addr, 0xed, row_s / 8);
    // write_reg(sensor->slv_addr, 0xee, (col_s + h) / 8);
    // write_reg(sensor->slv_addr, 0xef, (row_s + w) / 8);

    write_reg(sensor->slv_addr, 0x09, H8(row_s));
    write_reg(sensor->slv_addr, 0x0a, L8(row_s));
    write_reg(sensor->slv_addr, 0x0b, H8(col_s));
    write_reg(sensor->slv_addr, 0x0c, L8(col_s));
    write_reg(sensor->slv_addr, 0x0d, H8(h + 8));
    write_reg(sensor->slv_addr, 0x0e, L8(h + 8));
    write_reg(sensor->slv_addr, 0x0f, H8(w + 8));
    write_reg(sensor->slv_addr, 0x10, L8(w + 8));

    write_reg(sensor->slv_addr, 0x95, H8(h));
    write_reg(sensor->slv_addr, 0x96, L8(h));
    write_reg(sensor->slv_addr, 0x97, H8(w));
    write_reg(sensor->slv_addr, 0x98, L8(w));

#endif

    if (ret == 0) {
        ESP_LOGD(TAG, "Set framesize to: %ux%u", w, h);
    }
    return ret;

}

static int set_hmirror(sensor_t *sensor, int enable)
{
    int ret = 0;
    sensor->status.hmirror = enable;
    ret = write_reg(sensor->slv_addr, 0xfe, 0x00);
    ret |= set_reg_bits(sensor->slv_addr, P0_ANALOG_MODE1, 0, 0x01, enable != 0);
    if (ret == 0) {
        ESP_LOGD(TAG, "Set h-mirror to: %d", enable);
    }
    return ret;
}

static int set_vflip(sensor_t *sensor, int enable)
{
    int ret = 0;
    sensor->status.vflip = enable;
    ret = write_reg(sensor->slv_addr, 0xfe, 0x00);
    ret |= set_reg_bits(sensor->slv_addr, P0_ANALOG_MODE1, 1, 0x01, enable != 0);
    if (ret == 0) {
        ESP_LOGD(TAG, "Set v-flip to: %d", enable);
    }
    return ret;
}

static int set_colorbar(sensor_t *sensor, int enable)
{
    int ret = 0;
    // ret = write_reg(sensor->slv_addr, 0xfe, 0x00);
    // ret |= set_reg_bits(sensor->slv_addr, P0_DEBUG_MODE3, 3, 0x01, enable);
    if (ret == 0) {
        sensor->status.colorbar = enable;
        ESP_LOGD(TAG, "Set colorbar to: %d", enable);
    }
    return ret;
}

static int get_reg(sensor_t *sensor, int reg, int mask)
{
    int ret = 0;
    if (mask > 0xFF) {
        ESP_LOGE(TAG, "mask should not more than 0xff");
    } else {
        ret = read_reg(sensor->slv_addr, reg);
    }
    if (ret > 0) {
        ret &= mask;
    }
    return ret;
}

static int set_reg(sensor_t *sensor, int reg, int mask, int value)
{
    int ret = 0;
    if (mask > 0xFF) {
        ESP_LOGE(TAG, "mask should not more than 0xff");
    } else {
        ret = read_reg(sensor->slv_addr, reg);
    }
    if (ret < 0) {
        return ret;
    }
    value = (ret & ~mask) | (value & mask);

    if (mask > 0xFF) {

    } else {
        ret = write_reg(sensor->slv_addr, reg, value);
    }
    return ret;
}

static int init_status(sensor_t *sensor)
{
    write_reg(sensor->slv_addr, 0xfe, 0x00);
    sensor->status.brightness = 0;
    sensor->status.contrast = 0;
    sensor->status.saturation = 0;
    sensor->status.sharpness = 0;
    sensor->status.denoise = 0;
    sensor->status.ae_level = 0;
    sensor->status.gainceiling = 0;
    sensor->status.awb = 0;
    sensor->status.dcw = 0;
    sensor->status.agc = 0;
    sensor->status.aec = 0;
    sensor->status.hmirror = check_reg_mask(sensor->slv_addr, P0_ANALOG_MODE1, 0x01);
    sensor->status.vflip = check_reg_mask(sensor->slv_addr, P0_ANALOG_MODE1, 0x02);
    sensor->status.colorbar = 0;
    sensor->status.bpc = 0;
    sensor->status.wpc = 0;
    sensor->status.raw_gma = 0;
    sensor->status.lenc = 0;
    sensor->status.quality = 0;
    sensor->status.special_effect = 0;
    sensor->status.wb_mode = 0;
    sensor->status.awb_gain = 0;
    sensor->status.agc_gain = 0;
    sensor->status.aec_value = 0;
    sensor->status.aec2 = 0;

    print_regs(sensor->slv_addr);
    return 0;
}

static int set_dummy(sensor_t *sensor, int val)
{
    ESP_LOGW(TAG, "Unsupported");
    return -1;
}
static int set_gainceiling_dummy(sensor_t *sensor, gainceiling_t val)
{
    ESP_LOGW(TAG, "Unsupported");
    return -1;
}

int gc2145_detect(int slv_addr, sensor_id_t *id)
{
    if (GC2145_SCCB_ADDR == slv_addr) {
        uint8_t MIDL = SCCB_Read(slv_addr, CHIP_ID_LOW);
        uint8_t MIDH = SCCB_Read(slv_addr, CHIP_ID_HIGH);
        uint16_t PID = MIDH << 8 | MIDL;
        if (GC2145_PID == PID) {
            id->PID = PID;
            return PID;
        } else {
            ESP_LOGI(TAG, "Mismatch PID=0x%x", PID);
        }
    }
    return 0;
}

int gc2145_init(sensor_t *sensor)
{
    sensor->init_status = init_status;
    sensor->reset = reset;
    sensor->set_pixformat = set_pixformat;
    sensor->set_framesize = set_framesize;
    sensor->set_contrast = set_dummy;
    sensor->set_brightness = set_dummy;
    sensor->set_saturation = set_dummy;
    sensor->set_sharpness = set_dummy;
    sensor->set_denoise = set_dummy;
    sensor->set_gainceiling = set_gainceiling_dummy;
    sensor->set_quality = set_dummy;
    sensor->set_colorbar = set_colorbar;
    sensor->set_whitebal = set_dummy;
    sensor->set_gain_ctrl = set_dummy;
    sensor->set_exposure_ctrl = set_dummy;
    sensor->set_hmirror = set_hmirror;
    sensor->set_vflip = set_vflip;

    sensor->set_aec2 = set_dummy;
    sensor->set_awb_gain = set_dummy;
    sensor->set_agc_gain = set_dummy;
    sensor->set_aec_value = set_dummy;

    sensor->set_special_effect = set_dummy;
    sensor->set_wb_mode = set_dummy;
    sensor->set_ae_level = set_dummy;

    sensor->set_dcw = set_dummy;
    sensor->set_bpc = set_dummy;
    sensor->set_wpc = set_dummy;

    sensor->set_raw_gma = set_dummy;
    sensor->set_lenc = set_dummy;

    sensor->get_reg = get_reg;
    sensor->set_reg = set_reg;
    sensor->set_res_raw = NULL;
    sensor->set_pll = NULL;
    sensor->set_xclk = NULL;

    ESP_LOGI(TAG, "GC2145 Attached");
    return 0;
}
