#include "esp_log.h"
#include <stdio.h>
#include <stdint.h>
#include "driver/i2s_pdm.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stddef.h>

#define SAMPLE_RATE 16000
#define CHANNELS 1
#define BITS_PER_SAMPLE 16
#define RECORDING_DURATION 3 // seconds
#define BUFFER_SAMPLES 256

#define MIC_CLK GPIO_NUM_42
#define MIC_DATA GPIO_NUM_41

static const char *TAG = "microphome_config";

static i2s_chan_handle_t rx_channel;

void app_main(void)
{
    ESP_LOGI(TAG, "Audio configuration");
    ESP_LOGI(TAG, "Sample Rate:     %d Hz", SAMPLE_RATE);
    ESP_LOGI(TAG, "Channels:        %d", CHANNELS);
    ESP_LOGI(TAG, "Bits per Sample: %d\n", BITS_PER_SAMPLE);

    uint32_t total_samples = SAMPLE_RATE * RECORDING_DURATION;

    uint32_t total_bytes = total_samples * CHANNELS * (BITS_PER_SAMPLE / 8); // Calculate total bytes for the recording . conversion from bits to bytes
    ESP_LOGI(TAG, " Total samples per channel: %lu", total_samples);
    ESP_LOGI(TAG, " Total bytes for recording: %lu", total_bytes);

    ESP_LOGI(TAG, "Initializing microphone...");
    vTaskDelay(pdMS_TO_TICKS(1000));
    // creating a new I2S channel for receiving data from the microphone
    i2s_chan_config_t channel_config = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&channel_config, NULL, &rx_channel)); // asks the driver to allocate a new I2s channel. NULL = no need for TX (transmission) channel.
    // configuring PDM receive settings for the microphone
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
    vTaskDelay(pdMS_TO_TICKS(1000));
    // enabling the microphone channel to start receiving data
    ESP_ERROR_CHECK(i2s_channel_enable(rx_channel));
    ESP_LOGI(TAG, "Microphone initialized successfully. ");
    vTaskDelay(pdMS_TO_TICKS(1000));

    ESP_LOGI(TAG, "Starting audio recording for %d seconds...", RECORDING_DURATION);
    int16_t audio_buffer[BUFFER_SAMPLES]; // int16_t occupies 2 bytes, our buffer is 512 bytes long, which is enough to hold 256 samples of 16-bit audio data.

    for (int i = 0; i < 5; i++)
    {
        size_t bytes_read = 0;

        esp_err_t ret = i2s_channel_read(
            rx_channel,
            audio_buffer,         // memory where samples will be copied
            sizeof(audio_buffer), // buffer capacity in bytes
            &bytes_read,          // where the driver will write how many bytes were actually read
            1000);                // timeout in milliseconds
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "Error reading from microphone: %s", esp_err_to_name(ret));
            break;
        }
        size_t samples_read = bytes_read / (BITS_PER_SAMPLE / 8); // convert bytes to samples
        ESP_LOGI(TAG, "Read %d samples from microphone", (unsigned)samples_read);
        size_t samples_to_print = samples_read < 10 ? samples_read : 10; // print only the first 10 samples
        for (size_t j = 0; j < samples_to_print; j++)
        {
            printf("%d ", (int)audio_buffer[j]);
        }
        printf("\n");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    // Release driver resources after the test
    ESP_ERROR_CHECK(i2s_channel_disable(rx_channel));
    ESP_ERROR_CHECK(i2s_del_channel(rx_channel));
}