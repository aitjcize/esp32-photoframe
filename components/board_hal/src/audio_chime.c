#include "board_hal.h"
#include "esp_err.h"
#include "esp_log.h"
#include "sdkconfig.h"

#ifndef CONFIG_BOARD_DRIVER_WAVESHARE_PHOTOPAINTER_73

bool board_hal_has_speaker(void)
{
    return false;
}

esp_err_t board_hal_play_chime(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t board_hal_play_chime_preset(const char *preset)
{
    (void) preset;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t board_hal_play_wav_file(const char *path)
{
    (void) path;
    return ESP_ERR_NOT_SUPPORTED;
}

#else

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "axp2101.h"
#include "chime_presets.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "wav_pcm.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const char *TAG = "board_audio";

#define CHIME_SAMPLE_RATE 16000
#define CHIME_AMPLITUDE 7000
#define ES8311_REG_RESET 0x00
#define ES8311_REG_CLK_MANAGER1 0x01
#define ES8311_REG_CLK_MANAGER2 0x02
#define ES8311_REG_CLK_MANAGER3 0x03
#define ES8311_REG_CLK_MANAGER4 0x04
#define ES8311_REG_CLK_MANAGER5 0x05
#define ES8311_REG_SDP_IN 0x09
#define ES8311_REG_SDP_OUT 0x0A
#define ES8311_REG_SYSTEM1 0x0D
#define ES8311_REG_SYSTEM3 0x12
#define ES8311_REG_SYSTEM4 0x13
#define ES8311_REG_SYSTEM7 0x1B
#define ES8311_REG_SYSTEM8 0x1C
#define ES8311_REG_DAC_VOL 0x32
#define ES8311_REG_GPIO 0x37
#define ES8311_REG_CLK_DIV 0x16
#define ES8311_REG_CHIP_ID1 0xFD
#define ES8311_CHIP_ID 0x83

static SemaphoreHandle_t s_chime_mutex;

static void chime_mutex_init(void)
{
    if (!s_chime_mutex) {
        s_chime_mutex = xSemaphoreCreateMutex();
    }
}

static esp_err_t es8311_write(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(dev, buf, sizeof(buf), pdMS_TO_TICKS(100));
}

static esp_err_t es8311_read(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(dev, &reg, 1, val, 1, pdMS_TO_TICKS(100));
}

static void pa_set(bool enable)
{
    // NS4150B CTRL is active-high. Waveshare's codec_board config is
    // `out: {codec: ES8311, pa: 7, ...}` — PA GPIO 7 must be high for sound.
    gpio_set_level(BOARD_HAL_AUDIO_PA_PIN, enable ? 1 : 0);
}

// ES8311 slave + MCLK, 16-bit Philips I2S, DAC only. Sequence follows the
// common Espressif ES8311 DAC bring-up used by the Waveshare audio test
// (esp_codec_dev ES8311, use_mclk=1). MCLK tracks I2S Fs (256x), so the same
// clock-manager values work for 8–22.05 kHz chime WAVs.
static esp_err_t es8311_dac_init(i2c_master_dev_handle_t dev)
{
    uint8_t chip_id = 0;
    esp_err_t err = es8311_read(dev, ES8311_REG_CHIP_ID1, &chip_id);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ES8311 not responding on I2C 0x%02x", BOARD_HAL_AUDIO_ES8311_ADDR);
        return err;
    }
    if (chip_id != ES8311_CHIP_ID) {
        ESP_LOGW(TAG, "Unexpected ES8311 chip ID 0x%02x (expected 0x%02x)", chip_id,
                 ES8311_CHIP_ID);
    }

    ESP_ERROR_CHECK_WITHOUT_ABORT(es8311_write(dev, ES8311_REG_RESET, 0x1F));
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_ERROR_CHECK_WITHOUT_ABORT(es8311_write(dev, ES8311_REG_RESET, 0x00));
    ESP_ERROR_CHECK_WITHOUT_ABORT(es8311_write(dev, ES8311_REG_RESET, 0x80));  // CSM on

    ESP_ERROR_CHECK_WITHOUT_ABORT(es8311_write(dev, ES8311_REG_CLK_MANAGER1, 0x3F));
    ESP_ERROR_CHECK_WITHOUT_ABORT(es8311_write(dev, ES8311_REG_CLK_MANAGER2, 0x00));
    ESP_ERROR_CHECK_WITHOUT_ABORT(es8311_write(dev, ES8311_REG_CLK_MANAGER3, 0x10));
    ESP_ERROR_CHECK_WITHOUT_ABORT(es8311_write(dev, ES8311_REG_CLK_DIV, 0x24));
    ESP_ERROR_CHECK_WITHOUT_ABORT(es8311_write(dev, ES8311_REG_CLK_MANAGER4, 0x20));
    ESP_ERROR_CHECK_WITHOUT_ABORT(es8311_write(dev, ES8311_REG_CLK_MANAGER5, 0x00));

    // SDP_IN/OUT word length 16-bit (WL=011), Philips I2S (FMT=00) => 0x0C
    ESP_ERROR_CHECK_WITHOUT_ABORT(es8311_write(dev, ES8311_REG_SDP_IN, 0x0C));
    ESP_ERROR_CHECK_WITHOUT_ABORT(es8311_write(dev, ES8311_REG_SDP_OUT, 0x0C));

    ESP_ERROR_CHECK_WITHOUT_ABORT(es8311_write(dev, ES8311_REG_SYSTEM1, 0x01));
    ESP_ERROR_CHECK_WITHOUT_ABORT(es8311_write(dev, ES8311_REG_SYSTEM3, 0x00));
    ESP_ERROR_CHECK_WITHOUT_ABORT(es8311_write(dev, ES8311_REG_SYSTEM4, 0x10));
    ESP_ERROR_CHECK_WITHOUT_ABORT(es8311_write(dev, ES8311_REG_SYSTEM7, 0x0A));
    ESP_ERROR_CHECK_WITHOUT_ABORT(es8311_write(dev, ES8311_REG_SYSTEM8, 0x6A));
    ESP_ERROR_CHECK_WITHOUT_ABORT(es8311_write(dev, ES8311_REG_GPIO, 0x08));
    ESP_ERROR_CHECK_WITHOUT_ABORT(es8311_write(dev, ES8311_REG_DAC_VOL, 0xBF));  // 0 dB

    return ESP_OK;
}

static void es8311_standby(i2c_master_dev_handle_t dev)
{
    es8311_write(dev, ES8311_REG_DAC_VOL, 0x00);
    es8311_write(dev, ES8311_REG_RESET, 0x1F);
}

static void i2s_write_silence(i2s_chan_handle_t tx, int frames)
{
    int16_t zeros[128] = {0};
    while (frames > 0) {
        int n = frames > 64 ? 64 : frames;
        size_t written = 0;
        i2s_channel_write(tx, zeros, (size_t) n * 4, &written, pdMS_TO_TICKS(200));
        frames -= n;
    }
}

static void play_tone(i2s_chan_handle_t tx, float freq_hz, int duration_ms, int amplitude)
{
    const int n = CHIME_SAMPLE_RATE * duration_ms / 1000;
    const int edge = CHIME_SAMPLE_RATE * 8 / 1000;  // 8 ms attack / release
    int16_t buf[256];
    float phase = 0.0f;
    const float phase_inc = 2.0f * (float) M_PI * freq_hz / (float) CHIME_SAMPLE_RATE;
    int produced = 0;

    while (produced < n) {
        int frames = n - produced;
        if (frames > 128) {
            frames = 128;
        }
        for (int i = 0; i < frames; i++) {
            int idx = produced + i;
            float env = 1.0f;
            if (idx < edge) {
                env = (float) idx / (float) edge;
            } else if (idx > n - edge) {
                env = (float) (n - idx) / (float) edge;
            }
            int16_t sample = (int16_t) (sinf(phase) * (float) amplitude * env);
            buf[i * 2] = sample;
            buf[i * 2 + 1] = sample;
            phase += phase_inc;
            if (phase > 2.0f * (float) M_PI) {
                phase -= 2.0f * (float) M_PI;
            }
        }
        size_t written = 0;
        i2s_channel_write(tx, buf, (size_t) frames * 4, &written, pdMS_TO_TICKS(500));
        produced += frames;
    }
}

static void play_preset_tones(i2s_chan_handle_t tx, const char *preset)
{
    const chime_note_t *notes = NULL;
    size_t count = 0;
    if (!chime_preset_lookup(preset, &notes, &count)) {
        chime_preset_lookup(CHIME_PRESET_FALLBACK, &notes, &count);
    }
    if (!notes || count == 0) {
        return;
    }

    const char *name = (preset && preset[0]) ? preset : CHIME_PRESET_DEFAULT;
    const int amplitude = (strcmp(name, "softping") == 0) ? (CHIME_AMPLITUDE / 2) : CHIME_AMPLITUDE;

    int played_ms = 0;
    for (size_t i = 0; i < count && played_ms < CHIME_PRESET_MAX_MS; i++) {
        int dur = notes[i].duration_ms;
        if (dur <= 0) {
            continue;
        }
        if (played_ms + dur > CHIME_PRESET_MAX_MS) {
            dur = CHIME_PRESET_MAX_MS - played_ms;
        }
        if (notes[i].freq_hz <= 0.0f) {
            i2s_write_silence(tx, CHIME_SAMPLE_RATE * dur / 1000);
        } else {
            play_tone(tx, notes[i].freq_hz, dur, amplitude);
        }
        played_ms += dur;
    }
}

typedef struct {
    i2s_chan_handle_t tx;
    i2c_master_dev_handle_t es8311;
} audio_session_t;

static void audio_session_close(audio_session_t *s)
{
    pa_set(false);
    if (s->es8311) {
        es8311_standby(s->es8311);
    }
    if (s->tx) {
        i2s_channel_disable(s->tx);
        i2s_del_channel(s->tx);
        s->tx = NULL;
    }
    if (s->es8311) {
        i2c_master_bus_rm_device(s->es8311);
        s->es8311 = NULL;
    }
}

static esp_err_t audio_session_open(audio_session_t *s, uint32_t sample_rate)
{
    memset(s, 0, sizeof(*s));

    axp2101_prepare_audio_rails();
    vTaskDelay(pdMS_TO_TICKS(50));

    gpio_config_t pa_conf = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << BOARD_HAL_AUDIO_PA_PIN),
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&pa_conf);
    pa_set(false);

    i2c_master_bus_handle_t bus = board_hal_get_i2c_bus();
    if (!bus) {
        return ESP_ERR_INVALID_STATE;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BOARD_HAL_AUDIO_ES8311_ADDR,
        .scl_speed_hz = 100000,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &dev_cfg, &s->es8311);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add ES8311 I2C device: %s", esp_err_to_name(err));
        return err;
    }

    // Start I2S (and MCLK) before codec register writes — Waveshare
    // codec_init enables the I2S channel first so the ES8311 has a clock.
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    err = i2s_new_channel(&chan_cfg, &s->tx, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(err));
        audio_session_close(s);
        return err;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg =
            I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg =
            {
                .mclk = BOARD_HAL_AUDIO_I2S_MCLK_PIN,
                .bclk = BOARD_HAL_AUDIO_I2S_BCLK_PIN,
                .ws = BOARD_HAL_AUDIO_I2S_WS_PIN,
                .dout = BOARD_HAL_AUDIO_I2S_DOUT_PIN,
                .din = I2S_GPIO_UNUSED,
                .invert_flags =
                    {
                        .mclk_inv = false,
                        .bclk_inv = false,
                        .ws_inv = false,
                    },
            },
    };
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

    err = i2s_channel_init_std_mode(s->tx, &std_cfg);
    if (err == ESP_OK) {
        err = i2s_channel_enable(s->tx);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S init failed: %s", esp_err_to_name(err));
        audio_session_close(s);
        return err;
    }

    err = es8311_dac_init(s->es8311);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ES8311 DAC init failed: %s", esp_err_to_name(err));
        audio_session_close(s);
        return err;
    }

    i2s_write_silence(s->tx, 128);
    pa_set(true);
    vTaskDelay(pdMS_TO_TICKS(30));
    return ESP_OK;
}

static esp_err_t play_locked(esp_err_t (*fn)(audio_session_t *s, void *ctx), void *ctx)
{
    chime_mutex_init();
    if (!s_chime_mutex || xSemaphoreTake(s_chime_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    audio_session_t session;
    esp_err_t err = fn(&session, ctx);
    xSemaphoreGive(s_chime_mutex);
    return err;
}

static esp_err_t play_preset_session(audio_session_t *s, void *ctx)
{
    const char *preset = (const char *) ctx;
    ESP_LOGI(TAG, "Playing local ES8311 speaker chime preset '%s'",
             (preset && preset[0]) ? preset : CHIME_PRESET_DEFAULT);
    esp_err_t err = audio_session_open(s, CHIME_SAMPLE_RATE);
    if (err != ESP_OK) {
        return err;
    }
    play_preset_tones(s->tx, preset);
    i2s_write_silence(s->tx, 128);
    audio_session_close(s);
    return ESP_OK;
}

static esp_err_t play_wav_session(audio_session_t *s, void *ctx)
{
    const char *path = (const char *) ctx;
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGW(TAG, "Chime WAV not found: %s", path);
        return ESP_ERR_NOT_FOUND;
    }

    wav_pcm_info_t info;
    if (wav_pcm_parse_file(f, &info) != 0 || !wav_pcm_is_supported(&info)) {
        ESP_LOGW(TAG, "Unsupported or invalid chime WAV: %s", path);
        fclose(f);
        return ESP_ERR_NOT_SUPPORTED;
    }

    uint32_t play_bytes = wav_pcm_max_play_bytes(&info);
    ESP_LOGI(TAG, "Playing chime WAV %s (%lu Hz, %u ch, %u-bit, %lu bytes)", path,
             (unsigned long) info.sample_rate, info.channels, info.bits_per_sample,
             (unsigned long) play_bytes);

    esp_err_t err = audio_session_open(s, info.sample_rate);
    if (err != ESP_OK) {
        fclose(f);
        return err;
    }

    if (fseek(f, (long) info.data_offset, SEEK_SET) != 0) {
        audio_session_close(s);
        fclose(f);
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t src[256];
    int16_t dst[256];
    const size_t src_frame = (size_t) info.channels * (info.bits_per_sample / 8u);
    uint32_t remaining = play_bytes;
    while (remaining > 0 && src_frame > 0) {
        size_t want = remaining > sizeof(src) ? sizeof(src) : remaining;
        want -= want % src_frame;
        if (want == 0) {
            break;
        }
        size_t got = fread(src, 1, want, f);
        if (got == 0) {
            break;
        }
        size_t consumed = 0;
        size_t frames = wav_pcm_expand_s16_stereo(&info, src, got, dst, 128, &consumed);
        if (frames == 0) {
            break;
        }
        size_t written = 0;
        i2s_channel_write(s->tx, dst, frames * 4, &written, pdMS_TO_TICKS(500));
        if (consumed > remaining) {
            consumed = remaining;
        }
        remaining -= (uint32_t) consumed;
        // Long bulletins (up to WAV_PCM_MAX_SECONDS) must yield so IDLE can
        // feed the task watchdog.
        if ((remaining >> 15) != ((remaining + consumed) >> 15)) {
            vTaskDelay(1);
        }
    }

    i2s_write_silence(s->tx, 128);
    audio_session_close(s);
    fclose(f);
    return ESP_OK;
}

bool board_hal_has_speaker(void)
{
    return true;
}

esp_err_t board_hal_play_chime(void)
{
    return board_hal_play_chime_preset(CHIME_PRESET_DEFAULT);
}

esp_err_t board_hal_play_chime_preset(const char *preset)
{
    return play_locked(play_preset_session, (void *) preset);
}

esp_err_t board_hal_play_wav_file(const char *path)
{
    if (!path || !path[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    return play_locked(play_wav_session, (void *) path);
}

#endif
