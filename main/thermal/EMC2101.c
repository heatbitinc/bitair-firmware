#include "esp_log.h"
#include "esp_check.h"

#include "i2c_bitaxe.h"
#include "EMC2101.h"

static const char * TAG = "EMC2101";

#ifndef CONFIG_FAN_PWM_FREQUENCY_HZ_EFFECTIVE
#define CONFIG_FAN_PWM_FREQUENCY_HZ_EFFECTIVE 1400
#endif

static i2c_master_dev_handle_t emc2101_dev_handle;

static int temp_offset;
static const uint8_t EMC2101_SPINUP_DISABLED = 0x00;

// Requested fan percentages are scaled to 75% of the EMC2101 setting range.
// This matches the fan drive circuit on Bitair; keep it as is.
static const float FAN_DUTY_SCALE = 0.75f;

static uint8_t emc2101_pwm_range_from_hz(uint32_t pwm_hz)
{
    switch (pwm_hz) {
        case 22500:
            return 0;
        case 11250:
            return 1;
        case 5600:
            return 2;
        case 1400:
            return 3;
        default:
            ESP_LOGW(TAG, "Unsupported PWM frequency %lu Hz, fallback to 1400 Hz", (unsigned long)pwm_hz);
            return 3;
    }
}

esp_err_t EMC2101_set_pwm_frequency_hz(uint32_t pwm_hz)
{
    uint8_t range_bits = emc2101_pwm_range_from_hz(pwm_hz);

    // Base fan configuration keeps existing behavior and updates only RANGE bits.
    uint8_t fan_cfg = 0b00100011;
    fan_cfg = (fan_cfg & (uint8_t)~0x03U) | (range_bits & 0x03U);

    ESP_RETURN_ON_ERROR(i2c_bitaxe_register_write_byte(emc2101_dev_handle, EMC2101_FAN_CONFIG, fan_cfg), TAG, "Failed to set EMC2101 PWM frequency");

    const uint32_t applied_hz[] = {22500, 11250, 5600, 1400};
    ESP_LOGI(TAG,
             "EMC2101 PWM frequency request=%lu Hz, applied=%lu Hz (range=%u)",
             (unsigned long)pwm_hz,
             (unsigned long)applied_hz[range_bits],
             (unsigned)range_bits);

    return ESP_OK;
}

/**
 * @brief Initialize the EMC2101 sensor.
 *
 * @return esp_err_t ESP_OK on success, or an error code on failure.
 */
esp_err_t EMC2101_init(int temp_offset_param)
{
    ESP_LOGI(TAG, "Initializing EMC2101 (Temperature offset: %d° C)", temp_offset_param);

    temp_offset = temp_offset_param;

    if (i2c_bitaxe_add_device(EMC2101_I2CADDR_DEFAULT, &emc2101_dev_handle, TAG) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add device");
        return ESP_FAIL;
    }

    // set the TACH input
    ESP_RETURN_ON_ERROR(i2c_bitaxe_register_write_byte(emc2101_dev_handle, EMC2101_REG_CONFIG, 0x04), TAG, "Failed to set TACH input");


    //! deprecated bit was never set successfully
    // --- Configure Fan Settings (Register 0x4A) ---
    // Start with a base configuration:
    // Bit 7 (MASK) = 0: Tach Alert Enabled
    // Bit 6 (EN)   = 1: *** FAN DRIVER ENABLED ***
    // Bit 5 (LEVEL)= 1: Direct Setting Mode (use PWM Duty Cycle Reg 0x4C)
    // Bit 4 (POL)  = 0: Normal Polarity (placeholder, set below)
    // Bit 3:2(EDGES)=00: 2 Tach pulses per revolution (standard)
    // Bit 1:0(RANGE)=PWM base frequency (set below from sdkconfig)
    ESP_RETURN_ON_ERROR(EMC2101_set_pwm_frequency_hz((uint32_t)CONFIG_FAN_PWM_FREQUENCY_HZ_EFFECTIVE), TAG, "Failed to apply PWM frequency setting");

    // Disable hardware spin-up assist to avoid audible 100% boost when duty goes 0% -> target.
    // Fan speed transitions are controlled by firmware policy in fan_controller_task.
    ESP_RETURN_ON_ERROR(i2c_bitaxe_register_write_byte(emc2101_dev_handle, EMC2101_FAN_SPINUP, EMC2101_SPINUP_DISABLED),
                        TAG,
                        "Failed to disable EMC2101 fan spin-up profile");
    



    // We're using default filtering and conversion, no need to set them again.
    // //set filtering
    // ESP_ERROR_CHECK(i2c_bitaxe_register_write_byte(emc2101_dev_handle, EMC2101_TEMP_FILTER, EMC2101_DEFAULT_FILTER));

    // //set conversion rate
    // ESP_ERROR_CHECK(i2c_bitaxe_register_write_byte(emc2101_dev_handle, EMC2101_REG_DATA_RATE, EMC2101_DEFAULT_DATARATE));

    return ESP_OK;
}

esp_err_t EMC2101_set_ideality_factor(uint8_t ideality){
    //set Ideality Factor
    ESP_RETURN_ON_ERROR(i2c_bitaxe_register_write_byte(emc2101_dev_handle, EMC2101_IDEALITY_FACTOR, ideality), TAG, "Failed to set ideality factor");
    return ESP_OK;
}

esp_err_t EMC2101_set_beta_compensation(uint8_t beta){
    //set Beta Compensation
    ESP_RETURN_ON_ERROR(i2c_bitaxe_register_write_byte(emc2101_dev_handle, EMC2101_BETA_COMPENSATION, beta), TAG, "Failed to set beta compensation");
    return ESP_OK;
}

// takes a fan speed percent
esp_err_t EMC2101_set_fan_speed(float percent)
{
    uint8_t speed;

    speed = (uint8_t) (63.0 * percent * FAN_DUTY_SCALE);
    ESP_RETURN_ON_ERROR(i2c_bitaxe_register_write_byte(emc2101_dev_handle, EMC2101_REG_FAN_SETTING, speed), TAG, "Failed to set fan speed");
    return ESP_OK;
}

// RPM = 5400000/reading
uint16_t EMC2101_get_fan_speed(void)
{
    uint8_t tach_lsb = 0, tach_msb = 0;
    uint16_t reading;
    uint16_t RPM;
    esp_err_t err;

    err = i2c_bitaxe_register_read(emc2101_dev_handle, EMC2101_TACH_LSB, &tach_lsb, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read fan speed LSB: %s", esp_err_to_name(err));
        return 0;
    }
    
    err = i2c_bitaxe_register_read(emc2101_dev_handle, EMC2101_TACH_MSB, &tach_msb, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read fan speed MSB: %s", esp_err_to_name(err));
        return 0;
    }

    // ESP_LOGI(TAG, "Raw Fan Speed = %02X %02X", tach_msb, tach_lsb);

    reading = tach_lsb | (tach_msb << 8);
    RPM = 5400000 / reading;

    // ESP_LOGI(TAG, "Fan Speed = %d RPM", RPM);
    if (RPM == 82) {
        return 0;
    }
    return RPM;
}

float EMC2101_get_external_temp(void)
{
    uint8_t temp_msb = 0, temp_lsb = 0;
    uint16_t reading;
    esp_err_t err;

    err = i2c_bitaxe_register_read(emc2101_dev_handle, EMC2101_EXTERNAL_TEMP_MSB, &temp_msb, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read external temperature MSB: %s", esp_err_to_name(err));
        return -1;
    }
    
    err = i2c_bitaxe_register_read(emc2101_dev_handle, EMC2101_EXTERNAL_TEMP_LSB, &temp_lsb, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read external temperature LSB: %s", esp_err_to_name(err));
        return -1;
    }
    
    // Combine MSB and LSB, and then right shift to get 11 bits
    reading = (temp_msb << 8) | temp_lsb;
    reading >>= 5;  // Now, `reading` contains an 11-bit signed value

    // Cast `reading` to a signed 16-bit integer
    int16_t signed_reading = (int16_t)reading;

    // If the 11th bit (sign bit in 11-bit data) is set, extend the sign
    if (signed_reading & 0x0400) {
        signed_reading |= 0xF800;  // Set upper bits to extend the sign
    }

    if (signed_reading == EMC2101_TEMP_FAULT_OPEN_CIRCUIT) {
        ESP_LOGE(TAG, "EMC2101 TEMP_FAULT_OPEN_CIRCUIT: %04X", signed_reading);
    }
    if (signed_reading == EMC2101_TEMP_FAULT_SHORT) {
        ESP_LOGE(TAG, "EMC2101 TEMP_FAULT_SHORT: %04X", signed_reading);
    }

    // Convert the signed reading to temperature in Celsius
    float result = (float)signed_reading / 8.0;

    return result + temp_offset;
}

float EMC2101_get_internal_temp(void)
{
    uint8_t temp = 0;
    esp_err_t err;
    
    err = i2c_bitaxe_register_read(emc2101_dev_handle, EMC2101_INTERNAL_TEMP, &temp, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read internal temperature: %s", esp_err_to_name(err));
        return -1;
    }
    return (float) temp + temp_offset;
}
