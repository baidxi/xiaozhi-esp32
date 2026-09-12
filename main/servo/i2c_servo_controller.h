#ifndef I2C_SERVO_CONTROLLER_H
#define I2C_SERVO_CONTROLLER_H

#include <driver/i2c_master.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstddef>
#include <cstdint>
#include <string>

/**
 * @brief External I2C servo controller (default slave address 0x24)
 *
 * Wire protocol, fixed 4-byte frame [CMD][CH][VAL_H][VAL_L], VAL big endian:
 *   0x01 SET_ANGLE   set angle, VAL = angle * 10 (0..1800)
 *   0x02 SET_ENABLE  channel enable (VAL=1) / free torque (VAL=0)
 *   0x03 CENTER_ALL  center all channels to 90 degrees, CH/VAL ignored
 *   0x04 SEL_READ    select channel; a following read returns its angle * 10
 *   0x05 FW_VERSION  read returns firmware version (big endian word)
 *   0x06 DROP_COUNT  read returns count of frames dropped when queue full
 *   0x10 SET_MULTI   [0x10][N][ch][v_h][v_l]..., N = 1..12
 *
 * Boards mount this controller inside their InitializeTools():
 *   static I2cServoController servo(i2c_bus, cfg);
 *   servo.RegisterTools();
 *
 * Quadruped gait engine (opt-in via Config::enable_gait): a background task
 * streams 4-leg SET_MULTI frames at a fixed tick so the slave's frame queue
 * executes them smoothly. A gait keeps running until SetGait(GAIT_STOP) is
 * requested (voice "stop").
 */
class I2cServoController {
public:
    struct Config {
        // I2C device
        uint8_t slave_addr = 0x24;       // 7-bit slave address
        uint32_t scl_speed_hz = 100000;  // bus speed for this device
        int ch_max = 11;                 // highest channel index (0-based)

        // Quadruped gait engine (opt-in). enable_gait=false registers only
        // the six self.servo.* tools.
        bool enable_gait = false;
        int leg_ch[4] = {0, 1, 2, 3};  // LF, RF, LB, RB channel map
        int center_x10 = 900;          // neutral standing angle (90.0 deg)
        int swing_x10 = 300;           // swing amplitude (+/- 30.0 deg)
        int period_ms = 600;           // one full swing cycle
        int tick_ms = 50;              // SET_MULTI burst interval; keep the
                                       // slave frame queue (depth 8) in mind
    };

    // Note: the Config default argument cannot be spelled "= Config()" here
    // because the nested struct's default member initializers are not yet
    // complete at default-argument parse time (GCC rejects it). The default
    // is provided by a delegating constructor in the .cc file instead.
    explicit I2cServoController(i2c_master_bus_handle_t i2c_bus);
    I2cServoController(i2c_master_bus_handle_t i2c_bus, const Config& config);
    ~I2cServoController();
    // Registers self.servo.* (and self.robot.move when enable_gait) tools on
    // the McpServer singleton.
    void RegisterTools();

    // Bridge entry points (MicroPython xiaozhi.servo). Angles use the same
    // 0.1-degree units as the self.servo.* MCP tools (90 deg -> 900).
    bool SetAngleX10(int channel, int angle_x10);
    bool SetEnable(int channel, bool enable);
    bool CenterAll();

    // Gait modes for the quadruped engine (0 = stopped)
    enum Gait : int {
        GAIT_STOP = 0,
        GAIT_FORWARD = 1,
        GAIT_BACKWARD = 2,
        GAIT_LEFT = 3,
        GAIT_RIGHT = 4,
    };

    // Switches the background gait; non-stop gaits run continuously.
    // No-op when Config::enable_gait is false.
    void SetGait(Gait gait);

private:
    enum Command : uint8_t {
        CMD_SET_ANGLE = 0x01,
        CMD_SET_ENABLE = 0x02,
        CMD_CENTER_ALL = 0x03,
        CMD_SEL_READ = 0x04,
        CMD_FW_VERSION = 0x05,
        CMD_DROP_COUNT = 0x06,
        CMD_SET_MULTI = 0x10,
    };

    static constexpr size_t kMultiMaxChannels = 12;  // SET_MULTI limit (2 + 3*12 bytes)
    static constexpr int kI2cTimeoutMs = 100;

    bool WriteFrame(uint8_t cmd, uint8_t ch, uint16_t val);
    bool WriteRaw(const uint8_t* data, size_t len);
    bool ReadWord(uint8_t cmd, uint8_t ch, uint16_t& val);
    // Sends one SET_MULTI transaction with the four leg angles (x10)
    bool WriteLegAngles(const int (&angles_x10)[4]);

    static void GaitTaskFunc(void* arg);
    void GaitLoop();

    i2c_master_dev_handle_t dev_ = nullptr;
    Config cfg_;
    TaskHandle_t gait_task_ = nullptr;
    volatile int gait_ = GAIT_STOP;
};

#endif  // I2C_SERVO_CONTROLLER_H
