#include "esp_log.h"
#include <stdio.h>
#include <stdint.h>
#include "driver/i2s_pdm.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stddef.h>
#include <math.h>
#include "driver/spi_master.h"
#include "driver/sdspi_host.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include <stdbool.h>
#include <sys/stat.h>
#include <errno.h>

#define SAMPLE_RATE 16000
#define CHANNELS 1
#define BITS_PER_SAMPLE 16
#define RECORDING_DURATION 30 // seconds
#define BUFFER_SAMPLES 256

#define MIC_CLK GPIO_NUM_42
#define MIC_DATA GPIO_NUM_41

#define SD_CS GPIO_NUM_21
#define SD_SCK GPIO_NUM_7
#define SD_MISO GPIO_NUM_8
#define SD_MOSI GPIO_NUM_9

#define MOUNT_POINT "/sdcard"

#define MAX_RECORDINGS 100

static const char *TAG = "microphone_config";

static i2s_chan_handle_t rx_channel;

typedef struct __attribute__((packed))
{
    char riff[4];
    uint32_t chunk_size;
    char wave[4];

    char fmt[4];
    uint32_t subchunk1_size;
    uint16_t audio_format;
    uint16_t num_channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;

    char data[4];
    uint32_t data_size;

} wav_header_t;

static bool generate_filename(char *filename, size_t size);
static bool write_wav_header(FILE *file, uint32_t data_size);

static bool generate_filename(char *filename, size_t size)
{
    struct stat file_info;

    for (int i = 1; i <= MAX_RECORDINGS; i++)
    {
        int length = snprintf(
            filename,
            size,
            "/sdcard/recording_%03d.wav",
            i
        );

        if (length < 0 || (size_t)length >= size)
        {
            return false;
        }

        if (stat(filename, &file_info) != 0)
        {
            if (errno == ENOENT)
            {
                return true;
            }

            // Unexpected filesystem error
            ESP_LOGE(TAG, "File check failed: errno=%d", errno);
            return false;
        }
    }

    return false;
}

static bool write_wav_header(FILE *file, uint32_t data_size)
{
    wav_header_t header = {
        .riff = {'R', 'I', 'F', 'F'},
        .chunk_size = 36 + data_size,
        .wave = {'W', 'A', 'V', 'E'},

        .fmt = {'f', 'm', 't', ' '},
        .subchunk1_size = 16,
        .audio_format = 1,
        .num_channels = CHANNELS,
        .sample_rate = SAMPLE_RATE,

        .byte_rate = SAMPLE_RATE * CHANNELS *
                     (BITS_PER_SAMPLE / 8),

        .block_align = CHANNELS * (BITS_PER_SAMPLE / 8),
        .bits_per_sample = BITS_PER_SAMPLE,

        .data = {'d', 'a', 't', 'a'},
        .data_size = data_size};

    return fwrite(&header, sizeof(header), 1, file) == 1;
}



void app_main(void)
{
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = SD_MOSI,
        .miso_io_num = SD_MISO,
        .sclk_io_num = SD_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    ESP_ERROR_CHECK(
        spi_bus_initialize(host.slot, &bus_cfg, SDSPI_DEFAULT_DMA));

    // 3. Configure the card's SPI connection
    sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_cfg.gpio_cs = SD_CS;
    slot_cfg.host_id = host.slot;

    // 4. Configure the filesystem
    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };
    sdmmc_card_t *card = NULL;

    esp_err_t ret = esp_vfs_fat_sdspi_mount(
        MOUNT_POINT,
        &host,
        &slot_cfg,
        &mount_cfg,
        &card);

    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "SD mount failed: %s",
                 esp_err_to_name(ret));

        spi_bus_free(host.slot);
        return;
    }

    ESP_LOGI(TAG, "SD card mounted successfully!");

    ESP_LOGI(TAG, "Audio configuration");
    ESP_LOGI(TAG, "Sample Rate:     %d Hz", SAMPLE_RATE);
    ESP_LOGI(TAG, "Channels:        %d", CHANNELS);
    ESP_LOGI(TAG, "Bits per Sample: %d\n", BITS_PER_SAMPLE);

    uint32_t total_samples = SAMPLE_RATE * RECORDING_DURATION;

    uint32_t total_bytes = total_samples * CHANNELS * (BITS_PER_SAMPLE / 8); // Calculate total bytes for the recording . conversion from bits to bytes
    ESP_LOGI(TAG, " Total samples per channel: %lu", total_samples);
    ESP_LOGI(TAG, " Total bytes for recording: %lu", total_bytes);

    // FILE *file = fopen("/sdcard/recording.wav", "wb");
    char filename[64];
    if(!generate_filename(filename, sizeof(filename)))
    {
        ESP_LOGE(TAG, "Failed to generate a filename");
        esp_vfs_fat_sdcard_unmount(MOUNT_POINT, card);
        spi_bus_free(host.slot);
        return;
    }
    ESP_LOGI(TAG, "Recording will be saved to: %s", filename);
    FILE *file = fopen(filename, "wb");

    if (!write_wav_header(file, 0))
    {
        ESP_LOGE(TAG, "Failed to write WAV header");
        fclose(file);
        esp_vfs_fat_sdcard_unmount(MOUNT_POINT, card);
        spi_bus_free(host.slot);
        return;
    }

    if (file == NULL)
    {
        ESP_LOGE(TAG, "Failed to open audio file");
        // Clean up the mounted SD card before returning
        esp_vfs_fat_sdcard_unmount(MOUNT_POINT, card);
        spi_bus_free(host.slot);
        return;
    }

    ESP_LOGI(TAG, "Initializing microphone.............");
    vTaskDelay(pdMS_TO_TICKS(1000));
    // creating a new I2S channel for receiving data from the microphone
    // i2s_chan_config_t channel_config = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    // ESP_ERROR_CHECK(i2s_new_channel(&channel_config, NULL, &rx_channel)); // asks the driver to allocate a new I2s channel. NULL = no need for TX (transmission) channel.
    // configuring PDM receive settings for the microphone
    ESP_LOGI(TAG, "Checkpoint 1: Before I2S channel creation");

    i2s_chan_config_t channel_config =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);

    esp_err_t i2s_err = i2s_new_channel(
        &channel_config,
        NULL,
        &rx_channel);

    ESP_LOGI(TAG, "Checkpoint 2: i2s_new_channel returned %s",
             esp_err_to_name(i2s_err));

    if (i2s_err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to create I2S channel");
        fclose(file);
        esp_vfs_fat_sdcard_unmount(MOUNT_POINT, card);
        spi_bus_free(host.slot);
        return;
    }
    i2s_pdm_rx_config_t pdm_config = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_PDM_RX_SLOT_PCM_FMT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                           I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .clk = MIC_CLK,
            .din = MIC_DATA,
            .invert_flags = {
                .clk_inv = 0},
        },
    };
    ESP_ERROR_CHECK(
        i2s_channel_init_pdm_rx_mode(
            rx_channel,
            &pdm_config));
    // enabling the microphone channel to start receiving data
    ESP_ERROR_CHECK(i2s_channel_enable(rx_channel));
    ESP_LOGI(TAG, "Microphone initialized successfully. ");

    ESP_LOGI(TAG, "Starting audio recording for %d seconds...\n", RECORDING_DURATION);

    int16_t audio_buffer[BUFFER_SAMPLES];

    uint32_t target_samples = SAMPLE_RATE * RECORDING_DURATION;
    uint32_t samples_recorded = 0;

    ESP_LOGI(TAG, "Recording audio...");

    while (samples_recorded < target_samples)
    {
        size_t bytes_read = 0;

        // Read a chunk from the microphone
        esp_err_t ret = i2s_channel_read(
            rx_channel,
            audio_buffer,
            sizeof(audio_buffer),
            &bytes_read,
            1000);

        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "I2S read failed: %s",
                     esp_err_to_name(ret));
            break;
        }

        size_t samples_read = bytes_read / sizeof(int16_t);

        if (samples_read == 0)
        {
            ESP_LOGE(TAG, "No audio samples received");
            break;
        }

        // Don't write beyond the requested 3 seconds
        uint32_t remaining = target_samples - samples_recorded;

        if (samples_read > remaining)
        {
            samples_read = remaining;
        }

        // Write the received PCM samples to the SD card
        size_t samples_written = fwrite(
            audio_buffer,
            sizeof(int16_t),
            samples_read,
            file);

        if (samples_written != samples_read)
        {
            ESP_LOGE(TAG, "Failed to write all audio samples");
            break;
        }

        samples_recorded += samples_written;
    }
    uint32_t data_bytes = samples_recorded * sizeof(int16_t);

    if (fseek(file, 0, SEEK_SET) != 0) // This moves the current file position back to byte zero, the beginning of the WAV file.
    {
        ESP_LOGE(TAG, "Failed to seek to WAV header");
    }
    else if (!write_wav_header(file, data_bytes))
    {
        ESP_LOGE(TAG, "Failed to finalize WAV header");
    }

    if (fclose(file) != 0)
    {
        ESP_LOGE(TAG, "Failed to close recording file");
    }

    ESP_LOGI(TAG, "Saved %lu samples (%lu bytes)",
             (unsigned long)samples_recorded,
             (unsigned long)(samples_recorded * sizeof(int16_t)));

    // Release driver resources after the test
    ESP_ERROR_CHECK(i2s_channel_disable(rx_channel));
    ESP_ERROR_CHECK(i2s_del_channel(rx_channel));

    esp_vfs_fat_sdcard_unmount(MOUNT_POINT, card);
    ESP_ERROR_CHECK(spi_bus_free(host.slot));
}