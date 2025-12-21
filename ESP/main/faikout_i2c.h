// Manage I2C bus with both Daikin motherboard and temp/hum inlet sensor
// Use Master I2C to request temp/hum from inlet sensor (HSHCAL101B)
// Use Slave I2C to provide temp/hum to Daikin motherboard

#include "revk.h"
#if CONFIG_SOC_I2C_NUM > 1 // we need 2 I2C bus for master and slave
#define FAIKOUT_I2C_SENSOR


/// @brief Init the I2C bus to communicate with both Daikin motherboard and temp/hum sensor
/// @param i2cslsda GPIO to use for I2C slave with Daikin board - SDA
/// @param i2cslscl GPIO to use for I2C slave with Daikin board - SCL
/// @param i2cmasda GPIO to use for I2C master with inlet sensor - SDA
/// @param i2cmassl GPIO to use for I2C master with inlet sensor - SCL
/// @return ESP_OK if no error, ESP_ERR_INVALID_ARG if GPIO are not declared as IN/OUT (gpio_ok() check), ESP_ERR_NOT_ALLOWED if init already done, ESP_FAIL otherwise
esp_err_t faikout_i2c_init(revk_gpio_t i2cslsda, revk_gpio_t i2cslscl, revk_gpio_t i2cmasda, revk_gpio_t i2cmassl);

/// @brief Get init status
/// @return true if started, false otherwise
bool faikout_i2c_is_init_done(void);

/// @brief Start to respond to Daikin motherboard queries
/// @return ESP_OK if no error, ESP_ERR_NOT_ALLOWED if init has not been done or failed or if already started, ESP_FAIL otherwise
esp_err_t faikout_i2c_start(void);

/// @brief Get start status
/// @return true if started, false otherwise
bool faikout_i2c_is_started(void);

/// @brief Set temp/hum sensor values to provide to Daikin motherboard
/// @param temp Temp °C
/// @param hum Hum %
/// @return ESP_OK
esp_err_t faikout_i2c_set_external_sensor_values(float temp, float hum);

/// @brief Get temp/hum from inlet sensor
/// @param temp pointer to receive Temp °C
/// @param hum pointer to receive Hum %
/// @return ESP_OK if no error, ESP_ERR_NOT_ALLOWED if not already started, ESP_FAIL otherwise
esp_err_t faikout_i2c_get_inlet_sensor_values(float *temp, float *hum);

#endif