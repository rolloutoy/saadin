// Manage I2C bus with both Daikin motherboard and temp/hum inlet sensor
// Use Master I2C to request temp/hum from inlet sensor (HSHCAL101B)
// Use Slave I2C to provide temp/hum to Daikin motherboard

#include "revk.h"
#if CONFIG_SOC_I2C_NUM > 1 // we need 2 I2C bus for master and slave

#include "driver/i2c_master.h"
#include "driver/i2c_slave.h"
#include "faikout_i2c.h"

#define MASTER_READ_TIMEOUT_MS 1000
#define SLAVE_READ_TIMEOUT_MS 1000
#define INLET_SENSOR_ADDR 0x18
#define I2C_BUFFER_LENGTH 16
#define SENSOR_COMMAND (0x10)

static const char TAG[] = "Faikout_i2c";

typedef struct
{
    // Events and tasks
    QueueHandle_t event_queue; // Queue to handle I2C requests from Daikin motherboard
    TaskHandle_t i2cTask;      // Task to handle I2C requests from Daikin motherboard
    // Data
    uint8_t command_data;       // To store command from I2C requests from Daikin motherboard
    float temp;                 // To store temp value to reply to I2C requests from Daikin motherboard
    float hum;                  // To store hum value to reply to I2C requests from Daikin motherboard
    // Handles
    i2c_master_bus_handle_t master_bus_handle;
    i2c_master_dev_handle_t master_dev_handle;
    i2c_slave_dev_handle_t slave_handle;
    // Flags
    bool is_init_done;
    bool is_started;
} faikout_i2c_context_t;

static faikout_i2c_context_t faikout_i2c_context = {
    .event_queue = NULL,
    .i2cTask = NULL,
    .is_init_done = false,
    .is_started = false,
};

typedef enum
{
    I2C_SLAVE_EVT_RX,
    I2C_SLAVE_EVT_TX
} i2c_slave_event_t;

static void faikout_i2c_slave_task(void *p);

esp_err_t faikout_i2c_init(revk_gpio_t i2cslsda, revk_gpio_t i2cslscl, revk_gpio_t i2cmasda, revk_gpio_t i2cmassl)
{
    if (faikout_i2c_context.is_init_done)
    {
        ESP_LOGE(TAG, "Init already done!");
        return ESP_ERR_NOT_ALLOWED;
    }
    // Check GPIO are valid (both input and output)
    if (!(((gpio_ok (i2cslsda.num) & 3) == 3)
        && ((gpio_ok (i2cslscl.num) & 3) == 3)
        && ((gpio_ok (i2cmasda.num) & 3) == 3)
        && ((gpio_ok (i2cmassl.num) & 3) == 3)))
    {
        ESP_LOGE(TAG, "Init with invalid GPIO!");
        return ESP_ERR_INVALID_ARG;
    }
    // Init Master for inlet sensor management
    i2c_master_bus_config_t configM = {
#if CONFIG_SOC_HP_I2C_NUM > 1
        .i2c_port = I2C_NUM_1,
        .clk_source = I2C_CLK_SRC_DEFAULT,
#else
        .i2c_port = LP_I2C_NUM_0,
        .clk_source = LP_I2C_SCLK_DEFAULT,
#endif
        .sda_io_num = i2cmasda.num,
        .scl_io_num = i2cmassl.num,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = (i2cmasda.nopull || i2cmassl.nopull) ? false : true,
    };
    if (i2c_new_master_bus(&configM, &faikout_i2c_context.master_bus_handle) != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to init master bus");
        return ESP_FAIL;
    }

    i2c_device_config_t master_dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = INLET_SENSOR_ADDR,
        .scl_speed_hz = 20000,
    };
    if (i2c_master_bus_add_device(faikout_i2c_context.master_bus_handle, &master_dev_config, &faikout_i2c_context.master_dev_handle) != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to init device bus");
        i2c_del_master_bus(faikout_i2c_context.master_bus_handle);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Master init OK");

    // Init Slave for communication with Daikin motherboard
    i2c_slave_config_t i2c_slv_config = {
        .i2c_port = I2C_NUM_0,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .scl_io_num = i2cslscl.num,
        .sda_io_num = i2cslsda.num,
        .slave_addr = INLET_SENSOR_ADDR,
        .send_buf_depth = 8 * I2C_BUFFER_LENGTH,
        .receive_buf_depth = 8 * I2C_BUFFER_LENGTH,
        .addr_bit_len = I2C_ADDR_BIT_LEN_7,
        .flags.enable_internal_pullup = (i2cslsda.nopull || i2cslscl.nopull) ? false : true,
    };
    if (i2c_new_slave_device(&i2c_slv_config, &faikout_i2c_context.slave_handle) != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to init slave");
        i2c_master_bus_rm_device(faikout_i2c_context.master_dev_handle);
        i2c_del_master_bus(faikout_i2c_context.master_bus_handle);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Slave init OK");
    faikout_i2c_context.is_init_done = true;
    return ESP_OK;
}

bool faikout_i2c_is_init_done(void)
{
    return faikout_i2c_context.is_init_done;
}

static bool i2c_slave_request_cb(i2c_slave_dev_handle_t i2c_slave, const i2c_slave_request_event_data_t *evt_data, void *arg)
{
    faikout_i2c_context_t *context = (faikout_i2c_context_t *)arg;
    i2c_slave_event_t evt = I2C_SLAVE_EVT_TX;
    BaseType_t xTaskWoken = 0;
    xQueueSendFromISR(context->event_queue, &evt, &xTaskWoken);
    return xTaskWoken;
}

static bool i2c_slave_receive_cb(i2c_slave_dev_handle_t i2c_slave, const i2c_slave_rx_done_event_data_t *evt_data, void *arg)
{
    faikout_i2c_context_t *context = (faikout_i2c_context_t *)arg;
    i2c_slave_event_t evt = I2C_SLAVE_EVT_RX;
    BaseType_t xTaskWoken = 0;
    // Command only contains one byte, so just save one bytes here.
    context->command_data = *evt_data->buffer;
    xQueueSendFromISR(context->event_queue, &evt, &xTaskWoken);
    return xTaskWoken;
}

esp_err_t faikout_i2c_start(void)
{
    float temp;
    float hum;
    if (!faikout_i2c_context.is_init_done)
    {
        ESP_LOGE(TAG, "Init not done - not allowed to start!");
        return ESP_ERR_NOT_ALLOWED;
    }
    if (faikout_i2c_context.is_started)
    {
        ESP_LOGE(TAG, "Already started!");
        return ESP_ERR_NOT_ALLOWED;
    }
    if (!faikout_i2c_get_inlet_sensor_values(&temp, &hum))
    {
        faikout_i2c_set_external_sensor_values(temp, hum);
    }
    else
    {
        ESP_LOGE(TAG, "Start: unable to get inlet sensor values!");
        return ESP_FAIL;
    }
    if (!faikout_i2c_context.event_queue)
    {
        faikout_i2c_context.event_queue = xQueueCreate(16, sizeof(i2c_slave_event_t));
        if (!faikout_i2c_context.event_queue)
        {
            ESP_LOGE(TAG, "Start: Creating queue failed!");
            return ESP_FAIL;
        }
    }
    faikout_i2c_context.is_started = true;
    faikout_i2c_context.i2cTask = revk_task(TAG, faikout_i2c_slave_task, &faikout_i2c_context, 4);
    if (!faikout_i2c_context.i2cTask)
    {
        ESP_LOGE(TAG, "Start: Creating task failed!");
        faikout_i2c_context.is_started = false;
        return ESP_FAIL;
    }
    i2c_slave_event_callbacks_t cbs = {
        .on_receive = i2c_slave_receive_cb,
        .on_request = i2c_slave_request_cb,
    };
    if (i2c_slave_register_event_callbacks(faikout_i2c_context.slave_handle, &cbs, &faikout_i2c_context))
    {
        faikout_i2c_context.is_started = false;
        ESP_LOGE(TAG, "Start: Creating callbacks failed!");
        return ESP_FAIL;
    }
    return ESP_OK;
}

bool faikout_i2c_is_started(void)
{
    return faikout_i2c_context.is_started;
}

esp_err_t faikout_i2c_set_external_sensor_values(float temp, float hum)
{
    faikout_i2c_context.temp = temp;
    faikout_i2c_context.hum = hum;

    return ESP_OK;
}

esp_err_t faikout_i2c_get_inlet_sensor_values(float *temp, float *hum)
{
    if (!faikout_i2c_context.is_init_done)
    {
        ESP_LOGE(TAG, "Init not done - not allowed to get inlet sensor values!");
        return ESP_ERR_NOT_ALLOWED;
    }
    size_t len = 4;
    uint8_t reg_addr = SENSOR_COMMAND;
    uint8_t data[4];
    uint16_t tmp;
    if (i2c_master_transmit_receive(faikout_i2c_context.master_dev_handle, &reg_addr, 1, data, len, MASTER_READ_TIMEOUT_MS / portTICK_PERIOD_MS))
    {
        ESP_LOGE(TAG, "Failed to read from inlet sensor!");
        return ESP_FAIL;
    }
    tmp = data[2] | (data[3] << 8);
    if (tmp)
        *temp = (tmp - 2096) / 50.0f;

    tmp = data[0] | (data[1] << 8);
    if (tmp)
        *hum = (tmp - 1280) / 64.0f;
    return ESP_OK;
}

static void faikout_i2c_slave_task(void *arg)
{
    faikout_i2c_context_t *context = (faikout_i2c_context_t *)arg;
    i2c_slave_dev_handle_t handle = (i2c_slave_dev_handle_t)context->slave_handle;

    uint8_t zero_buffer[32] = {}; // Use this buffer to clear the fifo.
    uint32_t write_len, total_written;
    uint32_t buffer_size = 0;

    uint8_t tmp_buffer_slave_sensor[4];
    uint16_t tmp;

    while (context->is_started)
    {
        i2c_slave_event_t evt;
        if (xQueueReceive(context->event_queue, &evt, SLAVE_READ_TIMEOUT_MS / portTICK_PERIOD_MS) == pdTRUE)
        {
            if (evt == I2C_SLAVE_EVT_TX)
            {
                uint8_t *data_buffer;
                switch (context->command_data)
                {
                case SENSOR_COMMAND:
                    // Update buffer
                    tmp = (uint16_t)((faikout_i2c_context.temp + 41.92f) * 50.0f);
                    tmp_buffer_slave_sensor[2] = tmp & 0xFF;
                    tmp_buffer_slave_sensor[3] = (tmp >> 8) & 0x0F;

                    tmp = (uint16_t)((faikout_i2c_context.hum + 20.0f) * 64.0f);
                    tmp_buffer_slave_sensor[0] = tmp & 0xFF;
                    tmp_buffer_slave_sensor[1] = (tmp >> 8) & 0x1F;
                    // Assign
                    data_buffer = tmp_buffer_slave_sensor;
                    buffer_size = sizeof(tmp_buffer_slave_sensor);
                    break;
                default:
                    ESP_LOGE(TAG, "faikout_i2c_slave: Invalid command");
                    data_buffer = zero_buffer;
                    buffer_size = sizeof(zero_buffer);
                    break;
                }

                total_written = 0;
                while (total_written < buffer_size)
                {
                    ESP_ERROR_CHECK(i2c_slave_write(handle, data_buffer + total_written, buffer_size - total_written, &write_len, 1000));
                    if (write_len == 0)
                    {
                        ESP_LOGE(TAG, "faikout_i2c_slave: Write error or timeout");
                        break;
                    }
                    total_written += write_len;
                }
            }
        }
    }
    vTaskDelete(NULL);
    context->i2cTask = NULL;
}
#endif