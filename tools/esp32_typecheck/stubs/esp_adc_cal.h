#pragma once
#include <cstdint>
#include "esp_err.h"

typedef enum { ADC_UNIT_1 = 1, ADC_UNIT_2 = 2 } adc_unit_t;
typedef enum { ADC_ATTEN_DB_0 = 0, ADC_ATTEN_DB_2_5, ADC_ATTEN_DB_6, ADC_ATTEN_DB_11 } adc_atten_t;
typedef enum { ADC_WIDTH_BIT_9 = 0, ADC_WIDTH_BIT_10, ADC_WIDTH_BIT_11, ADC_WIDTH_BIT_12 } adc_bits_width_t;

typedef enum {
  ESP_ADC_CAL_VAL_EFUSE_VREF = 0,
  ESP_ADC_CAL_VAL_EFUSE_TP = 1,
  ESP_ADC_CAL_VAL_DEFAULT_VREF = 2,
  ESP_ADC_CAL_VAL_EFUSE_TP_FIT = 3,
  ESP_ADC_CAL_VAL_NOT_SUPPORTED
} esp_adc_cal_value_t;

typedef struct {
  adc_unit_t adc_num;
  adc_atten_t atten;
  adc_bits_width_t bit_width;
  std::uint32_t coeff_a;
  std::uint32_t coeff_b;
  std::uint32_t vref;
} esp_adc_cal_characteristics_t;

extern "C" {
esp_adc_cal_value_t esp_adc_cal_characterize(adc_unit_t adc_num, adc_atten_t atten,
                                             adc_bits_width_t bit_width,
                                             std::uint32_t default_vref,
                                             esp_adc_cal_characteristics_t* chars);
std::uint32_t esp_adc_cal_raw_to_voltage(std::uint32_t adc_reading,
                                         const esp_adc_cal_characteristics_t* chars);
}
