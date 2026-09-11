#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>

#define AUDIO_INPUT_SAMPLE_RATE 24000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000

#define AUDIO_INPUT_REFERENCE true
#if CONFIG_BOARD_TYPE_WAVESHARE_ESP32_S3_TOUCH_AMOLED_1_75
#define AUDIO_I2S_GPIO_MCLK GPIO_NUM_42
#define AUDIO_I2S_GPIO_WS GPIO_NUM_45
#define AUDIO_I2S_GPIO_BCLK GPIO_NUM_9
#define AUDIO_I2S_GPIO_DIN GPIO_NUM_10
#define AUDIO_I2S_GPIO_DOUT GPIO_NUM_8
#define EXAMPLE_PIN_NUM_LCD_RST GPIO_NUM_39
#define PIN_NUM_TOUCH_RST GPIO_NUM_40
#define PIN_NUM_TOUCH_INT GPIO_NUM_11
#elif CONFIG_BOARD_TYPE_WAVESHARE_ESP32_S3_TOUCH_AMOLED_1_75C
#define AUDIO_I2S_GPIO_MCLK GPIO_NUM_16
#define AUDIO_I2S_GPIO_WS GPIO_NUM_45
#define AUDIO_I2S_GPIO_BCLK GPIO_NUM_9
#define AUDIO_I2S_GPIO_DIN GPIO_NUM_10
#define AUDIO_I2S_GPIO_DOUT GPIO_NUM_8
#define EXAMPLE_PIN_NUM_LCD_RST GPIO_NUM_1
#define PIN_NUM_TOUCH_RST GPIO_NUM_2
#define PIN_NUM_TOUCH_INT GPIO_NUM_11
#endif

#define AUDIO_CODEC_PA_PIN GPIO_NUM_46
#define AUDIO_CODEC_I2C_SDA_PIN GPIO_NUM_15
#define AUDIO_CODEC_I2C_SCL_PIN GPIO_NUM_14
#define AUDIO_CODEC_ES8311_ADDR ES8311_CODEC_DEFAULT_ADDR
#define AUDIO_CODEC_ES7210_ADDR ES7210_CODEC_DEFAULT_ADDR

#define I2C_ADDRESS ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_000
#define BOOT_BUTTON_GPIO GPIO_NUM_0

#define EXAMPLE_PIN_NUM_LCD_CS GPIO_NUM_12
#define EXAMPLE_PIN_NUM_LCD_PCLK GPIO_NUM_38
#define EXAMPLE_PIN_NUM_LCD_DATA0 GPIO_NUM_4
#define EXAMPLE_PIN_NUM_LCD_DATA1 GPIO_NUM_5
#define EXAMPLE_PIN_NUM_LCD_DATA2 GPIO_NUM_6
#define EXAMPLE_PIN_NUM_LCD_DATA3 GPIO_NUM_7

#define DISPLAY_WIDTH 466
#define DISPLAY_HEIGHT 466
#define DISPLAY_MIRROR_X false
#define DISPLAY_MIRROR_Y false
#define DISPLAY_SWAP_XY false

#define DISPLAY_OFFSET_X 0
#define DISPLAY_OFFSET_Y 0

#define DISPLAY_BACKLIGHT_PIN GPIO_NUM_NC
#define DISPLAY_BACKLIGHT_OUTPUT_INVERT false

// External I2C servo controller (slave 0x24), 12 channels (0~11).
// Consumed via I2cServoController::Config by the shared module
// main/servo/i2c_servo_controller.
#define I2C_SERVO_SLAVE_ADDR 0x24
#define I2C_SERVO_CLK_HZ (100 * 1000)
#define I2C_SERVO_CH_MAX 11
// 1: share the codec I2C bus (GPIO14/15); 0: use a dedicated I2C1 bus on
// I2C_SERVO_SDA_PIN/I2C_SERVO_SCL_PIN (free pins routed to test points)
#define I2C_SERVO_USE_MAIN_BUS 1
#if !I2C_SERVO_USE_MAIN_BUS
#define I2C_SERVO_SDA_PIN GPIO_NUM_17
#define I2C_SERVO_SCL_PIN GPIO_NUM_18
#endif

// Quadruped gait engine (4 of the 12 channels used, one servo per leg).
// Legs swing sinusoidally around QUAD_CENTER_X10 with trot phasing (diagonal
// pairs in phase). Tune these to the mechanical build:
//   - swap a leg channel if a leg is mirrored on the chassis
//   - increase QUAD_SWING_X10 for longer strides, QUAD_PERIOD_MS for slower
//     steps; keep ticks deep enough for the slave frame queue (depth 8)
#define QUAD_LEG_LF_CH 0     // left front leg
#define QUAD_LEG_RF_CH 1     // right front leg
#define QUAD_LEG_LB_CH 2     // left back leg
#define QUAD_LEG_RB_CH 3     // right back leg
#define QUAD_CENTER_X10 900  // neutral standing angle (90.0 deg)
#define QUAD_SWING_X10 300   // swing amplitude (+/- 30.0 deg)
#define QUAD_PERIOD_MS 600   // one full swing cycle
#define QUAD_TICK_MS 50      // frame interval for continuous SET_MULTI bursts
#endif // _BOARD_CONFIG_H_
