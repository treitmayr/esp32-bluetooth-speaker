/*
 * https://dsp.stackexchange.com/questions/2990/how-to-change-volume-of-a-pcm-16-bit-signed-audio
 * 
 * In order to adjust the volume, the input signal is multiplied by some constant in dB.
 * constant = min_dB + (volume_level * (max_dB - min_dB)) / volume_level_max;
 * constant = (pow(10.0, constant / 20.0);
 * In order to avoit the floating point calculation, the constant is multiplied by some value 
 * and then during input signal proccessing, the constant is restored back. 
 * So the last equation will look like below:
 * constant = (pow(10.0, constant / 20.0) * VOLUME_SCALE_VAL;
 *
 * input_signal = (input_signal * constant) >> VOLUME_SCALE_VAL
 */

#include "sdkconfig.h"
#ifndef CONFIG_EXAMPLE_BUILD_FACTORY_IMAGE

#include <stdint.h>
#include <limits.h>
#include <math.h>
#include "bt_app_av.h"
#include "bt_app_volume_control.h"
#include "esp_log.h"
#include "esp_system.h"


#define VOLUME_LEVELS 128
#define VOLUME_LEVEL_MAX (VOLUME_LEVELS - 1)
#define INITIAL_VOLUME VOLUME_LEVEL_MAX

#define VOLUME_SCALE_BITS 15
#define VOLUME_SCALE_VAL (1 << VOLUME_SCALE_BITS)

#define NOISE_SAMPLE_COUNT 2000

static const char *TAG = "VOLCTL";

static uint16_t gain_presets[VOLUME_LEVELS];
static int16_t noise[NOISE_SAMPLE_COUNT];

/* if the volume is not set by host, use this volume. */
int32_t volume = INITIAL_VOLUME;

extern esp_log_level_t esp_log_level_get(const char *tag);

void generate_triangular_pdf_noise()
{
    ESP_LOGD(TAG, "Generating %d samples of triangular PDF noise", NOISE_SAMPLE_COUNT);

    esp_fill_random((void *) noise, sizeof(noise));
    const int16_t first = noise[0];
    int16_t s2 = noise[0] / 2;
    const int16_t int16_min = -2 * ((int16_t) 1 << (sizeof(int16_t) * 8 - 2));
    for (unsigned int idx = 0; idx < NOISE_SAMPLE_COUNT; idx++)
    {
        const int16_t s1 = s2;
        s2 = (idx < (NOISE_SAMPLE_COUNT - 1)) ? noise[idx + 1] : first;
        s2 /= 2;
        int16_t diff = s2 - s1;
        /* do NOT include the lowest value of int16_t */
        if (diff == int16_min)
        {
            diff += 1;
        }
        /* scale down random value to match volume resolution */
        if (VOLUME_SCALE_BITS < 15)
        {
            diff /= 1 << (15 - VOLUME_SCALE_BITS);
        }
        noise[idx] = diff;
    }
    /* DEBUG: Check the distribution of noise values:
    uint16_t count[16] = {0};
    const int slots = sizeof(count)/sizeof(count[0]);
    for (unsigned int idx = 0; idx < NOISE_SAMPLE_COUNT; idx++)
    {
        int slot = ((int32_t) noise[idx]) * (slots / 2) / VOLUME_SCALE_VAL + (slots / 2);
        if (slot < 0 || slot >= slots) printf("Warning: slot = %d\n", slot);
        count[slot]++;
    }
    for (unsigned int idx = 0; idx < slots; idx++)
    {
        putchar(':');
        for (unsigned int i = 0; i < count[idx] / (NOISE_SAMPLE_COUNT / 400); i++)
        {
            putchar('*');
        }
        putchar('\n');
    }
    */
}

void bt_app_vc_initialize(float min_db, float max_db, bool level0_mute)
{
    const float diff_db = max_db - min_db;
    for (unsigned int level = 0; level < VOLUME_LEVELS; level++)
    {
        float constant = min_db + (level * diff_db) / VOLUME_LEVEL_MAX;
        constant = pow(10.0, constant / 20.0) * VOLUME_SCALE_VAL;
        gain_presets[level] = (uint16_t) constant;
        ESP_LOGD(TAG, "gain[%d] = %x\n", level, gain_presets[level]);
    }
    if (level0_mute)
    {
        gain_presets[0] = 0;
        ESP_LOGD(TAG, "gain[%d] = %x\n", 0, gain_presets[0]);
    }

    /* create Triangular PDF noise for dither */
    generate_triangular_pdf_noise();
}

void bt_app_set_initial_volume()
{
    bt_app_set_volume(INITIAL_VOLUME);
}

void bt_app_set_volume(uint32_t level)
{
    volume = (level <= VOLUME_LEVEL_MAX) ? level : VOLUME_LEVEL_MAX;
    ESP_LOGD(TAG, "volume: level=%d/127, mult=%d/%d",
             level, gain_presets[volume], VOLUME_SCALE_VAL);
}

uint32_t bt_app_get_volume(void)
{
    return volume;
}

void bt_app_adjust_volume(uint8_t *data, size_t size)
{
    static unsigned int noise_idx = 0;
    const uint16_t gain = gain_presets[volume];
    const bool apply_dither = (gain <= (VOLUME_SCALE_VAL / 2));
    if (gain < VOLUME_SCALE_VAL)
    {
        size_t items = size / (BT_SBC_BITS_PER_SAMPLE / 8); /* 8 is bits per byte */
        int16_t* ptr = (int16_t*) data;
        while (items--)
        {
            int32_t fraction = (int32_t)*ptr;
            fraction *= gain;
            if (apply_dither)
            {
                fraction += noise[noise_idx++];
            }
            fraction /= VOLUME_SCALE_VAL;
            *ptr = (int16_t)fraction;
            ptr += 1;
            if (noise_idx == NOISE_SAMPLE_COUNT)
            {
                noise_idx = 0;
            }
        }
    }
}

#endif /* CONFIG_EXAMPLE_BUILD_FACTORY_IMAGE */