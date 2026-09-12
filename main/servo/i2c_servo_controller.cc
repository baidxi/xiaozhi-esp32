#include "i2c_servo_controller.h"

#include <cJSON.h>
#include <cmath>

#include <esp_log.h>

#include "mcp_server.h"

#define TAG "I2cServo"

I2cServoController::I2cServoController(i2c_master_bus_handle_t i2c_bus)
    : I2cServoController(i2c_bus, Config()) {}

I2cServoController::I2cServoController(i2c_master_bus_handle_t i2c_bus, const Config& config)
    : cfg_(config) {
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = cfg_.slave_addr,
        .scl_speed_hz = cfg_.scl_speed_hz,
    };
    esp_err_t ret = i2c_master_bus_add_device(i2c_bus, &dev_cfg, &dev_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add I2C device 0x%02X: %s", cfg_.slave_addr, esp_err_to_name(ret));
        dev_ = nullptr;
    }
}

I2cServoController::~I2cServoController() {
    if (gait_task_ != nullptr) {
        gait_ = GAIT_STOP;
    }
    if (dev_ != nullptr) {
        i2c_master_bus_rm_device(dev_);
    }
}

bool I2cServoController::SetAngleX10(int channel, int angle_x10) {
    if (channel < 0 || channel > cfg_.ch_max || angle_x10 < 0 || angle_x10 > 1800) {
        return false;
    }
    return WriteFrame(CMD_SET_ANGLE, static_cast<uint8_t>(channel),
                      static_cast<uint16_t>(angle_x10));
}

bool I2cServoController::SetEnable(int channel, bool enable) {
    if (channel < 0 || channel > cfg_.ch_max) {
        return false;
    }
    return WriteFrame(CMD_SET_ENABLE, static_cast<uint8_t>(channel), enable ? 1 : 0);
}

bool I2cServoController::CenterAll() { return WriteFrame(CMD_CENTER_ALL, 0, 0); }

bool I2cServoController::WriteFrame(uint8_t cmd, uint8_t ch, uint16_t val) {
    uint8_t frame[4] = {
        cmd,
        ch,
        static_cast<uint8_t>(val >> 8),
        static_cast<uint8_t>(val & 0xFF),
    };
    return WriteRaw(frame, sizeof(frame));
}

bool I2cServoController::WriteRaw(const uint8_t* data, size_t len) {
    if (dev_ == nullptr) {
        return false;
    }
    esp_err_t ret = i2c_master_transmit(dev_, data, len, kI2cTimeoutMs);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "I2C transmit failed: %s", esp_err_to_name(ret));
        return false;
    }
    return true;
}

bool I2cServoController::ReadWord(uint8_t cmd, uint8_t ch, uint16_t& val) {
    if (dev_ == nullptr) {
        return false;
    }
    if (!WriteFrame(cmd, ch, 0)) {
        return false;
    }
    uint8_t buf[2] = {0, 0};
    esp_err_t ret = i2c_master_receive(dev_, buf, sizeof(buf), kI2cTimeoutMs);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "I2C receive failed: %s", esp_err_to_name(ret));
        return false;
    }
    val = (static_cast<uint16_t>(buf[0]) << 8) | buf[1];
    return true;
}

bool I2cServoController::WriteLegAngles(const int (&angles_x10)[4]) {
    uint8_t frame[2 + 3 * 4] = {0};
    frame[0] = CMD_SET_MULTI;
    frame[1] = 4;
    for (int i = 0; i < 4; ++i) {
        int v = angles_x10[i];
        if (v < 0)
            v = 0;
        if (v > 1800)
            v = 1800;
        frame[2 + 3 * i] = static_cast<uint8_t>(cfg_.leg_ch[i]);
        frame[3 + 3 * i] = static_cast<uint8_t>(v >> 8);
        frame[4 + 3 * i] = static_cast<uint8_t>(v & 0xFF);
    }
    return WriteRaw(frame, sizeof(frame));
}

void I2cServoController::SetGait(Gait gait) {
    if (!cfg_.enable_gait) {
        return;
    }
    gait_ = gait;
    if (gait != GAIT_STOP && gait_task_ == nullptr) {
        BaseType_t ok = xTaskCreate(GaitTaskFunc, "servo_gait", 3072, this, 5, &gait_task_);
        if (ok != pdPASS) {
            ESP_LOGE(TAG, "Failed to create gait task");
            gait_task_ = nullptr;
            gait_ = GAIT_STOP;
        }
    }
}

void I2cServoController::GaitTaskFunc(void* arg) {
    static_cast<I2cServoController*>(arg)->GaitLoop();
}

void I2cServoController::GaitLoop() {
    // Trot phases: diagonal leg pairs (LF+RB, RF+LB) swing in phase
    static const float kLegPhase[4] = {0.0f, (float)M_PI, (float)M_PI, 0.0f};
    const int ticks_per_period = cfg_.period_ms / cfg_.tick_ms;
    int tick = 0;
    bool was_running = false;
    while (true) {
        int gait = gait_;
        if (gait == GAIT_STOP) {
            if (was_running) {
                // Return legs to the neutral standing pose once
                int neutral[4] = {cfg_.center_x10, cfg_.center_x10, cfg_.center_x10,
                                  cfg_.center_x10};
                WriteLegAngles(neutral);
                was_running = false;
            }
            tick = 0;
            vTaskDelay(pdMS_TO_TICKS(cfg_.tick_ms));
            continue;
        }
        was_running = true;
        float phase = 2.0f * (float)M_PI * tick / ticks_per_period;
        int angles[4];
        for (int i = 0; i < 4; ++i) {
            int amp = cfg_.swing_x10;
            if (gait == GAIT_LEFT && (i == 0 || i == 2)) {
                amp = 0;  // hold left legs, right legs drive -> pivot left
            } else if (gait == GAIT_RIGHT && (i == 1 || i == 3)) {
                amp = 0;  // hold right legs, left legs drive -> pivot right
            }
            float p = phase + kLegPhase[i];
            if (gait == GAIT_BACKWARD) {
                p += (float)M_PI;  // reverse the swing direction
            }
            angles[i] = cfg_.center_x10 + static_cast<int>(amp * sinf(p));
        }
        WriteLegAngles(angles);
        tick = (tick + 1) % ticks_per_period;
        vTaskDelay(pdMS_TO_TICKS(cfg_.tick_ms));
    }
}

void I2cServoController::RegisterTools() {
    auto& mcp_server = McpServer::GetInstance();

    if (cfg_.enable_gait) {
        mcp_server.AddTool(
            "self.robot.move",
            "控制四足机器人移动(连续动作: 非停止指令会持续执行, 直到用户喊停止)。"
            "action: forward=前进, backward=后退, left=左转, right=右转, stop=停止并回到站立姿态。"
            "用户说前进/后退/左转/右转/停止时调用此工具。",
            PropertyList({
                Property("action", kPropertyTypeString),
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                std::string action = properties["action"].value<std::string>();
                Gait gait = GAIT_STOP;
                if (action == "forward") {
                    gait = GAIT_FORWARD;
                } else if (action == "backward") {
                    gait = GAIT_BACKWARD;
                } else if (action == "left") {
                    gait = GAIT_LEFT;
                } else if (action == "right") {
                    gait = GAIT_RIGHT;
                } else if (action != "stop") {
                    return std::string("error: action 必须是 forward/backward/left/right/stop");
                }
                SetGait(gait);
                return true;
            });
    }

    char desc[192];
    snprintf(desc, sizeof(desc),
             "设置舵机角度。ch: 舵机通道号(0~%d); angle: 角度值,单位 0.1 度(0~1800),"
             "如 90 度传 900, 45.5 度传 455。",
             cfg_.ch_max);
    mcp_server.AddTool("self.servo.set_angle", desc,
                       PropertyList({
                           Property("ch", kPropertyTypeInteger, 0, cfg_.ch_max),
                           Property("angle", kPropertyTypeInteger, 0, 1800),
                       }),
                       [this](const PropertyList& properties) -> ReturnValue {
                           int ch = properties["ch"].value<int>();
                           int angle = properties["angle"].value<int>();
                           if (!WriteFrame(CMD_SET_ANGLE, ch, angle)) {
                               return std::string("error: 舵机控制器无响应");
                           }
                           return true;
                       });

    mcp_server.AddTool("self.servo.set_multi",
                       "批量设置多个舵机角度(单次同步下发)。channels: JSON 数组字符串,"
                       "每项 {\"ch\":通道号, \"angle\":角度x10}, 1~12 项, 如 "
                       "[{\"ch\":0,\"angle\":900},{\"ch\":1,\"angle\":450}]。",
                       PropertyList({
                           Property("channels", kPropertyTypeString),
                       }),
                       [this](const PropertyList& properties) -> ReturnValue {
                           std::string channels = properties["channels"].value<std::string>();
                           cJSON* root = cJSON_Parse(channels.c_str());
                           if (root == nullptr || !cJSON_IsArray(root)) {
                               if (root != nullptr)
                                   cJSON_Delete(root);
                               return std::string("error: channels 不是合法的 JSON 数组");
                           }
                           int count = cJSON_GetArraySize(root);
                           if (count < 1 || count > static_cast<int>(kMultiMaxChannels)) {
                               cJSON_Delete(root);
                               return std::string("error: 通道数量必须在 1~12 之间");
                           }
                           uint8_t frame[2 + 3 * kMultiMaxChannels] = {0};
                           frame[0] = CMD_SET_MULTI;
                           frame[1] = static_cast<uint8_t>(count);
                           for (int i = 0; i < count; ++i) {
                               cJSON* item = cJSON_GetArrayItem(root, i);
                               cJSON* jch = cJSON_GetObjectItem(item, "ch");
                               cJSON* jangle = cJSON_GetObjectItem(item, "angle");
                               if (jch == nullptr || jangle == nullptr || !cJSON_IsNumber(jch) ||
                                   !cJSON_IsNumber(jangle)) {
                                   cJSON_Delete(root);
                                   return std::string("error: 每项必须包含数值型 ch 和 angle 字段");
                               }
                               int ch = static_cast<int>(jch->valuedouble);
                               int angle = static_cast<int>(jangle->valuedouble);
                               if (ch < 0 || ch > cfg_.ch_max || angle < 0 || angle > 1800) {
                                   cJSON_Delete(root);
                                   char err[64];
                                   snprintf(err, sizeof(err),
                                            "error: ch 必须在 0~%d, angle 必须在 0~1800",
                                            cfg_.ch_max);
                                   return std::string(err);
                               }
                               frame[2 + 3 * i] = static_cast<uint8_t>(ch);
                               frame[3 + 3 * i] = static_cast<uint8_t>(angle >> 8);
                               frame[4 + 3 * i] = static_cast<uint8_t>(angle & 0xFF);
                           }
                           cJSON_Delete(root);
                           if (!WriteRaw(frame, 2 + 3 * count)) {
                               return std::string("error: 舵机控制器无响应");
                           }
                           return true;
                       });

    snprintf(
        desc, sizeof(desc),
        "使能舵机通道(上力)或卸力(自由转动)。ch: 通道号(0~%d); enable: true 使能, false 卸力。",
        cfg_.ch_max);
    mcp_server.AddTool("self.servo.set_enable", desc,
                       PropertyList({
                           Property("ch", kPropertyTypeInteger, 0, cfg_.ch_max),
                           Property("enable", kPropertyTypeBoolean),
                       }),
                       [this](const PropertyList& properties) -> ReturnValue {
                           int ch = properties["ch"].value<int>();
                           bool enable = properties["enable"].value<bool>();
                           if (!WriteFrame(CMD_SET_ENABLE, ch, enable ? 1 : 0)) {
                               return std::string("error: 舵机控制器无响应");
                           }
                           return true;
                       });

    mcp_server.AddTool("self.servo.center_all", "全部舵机归中到 90 度(站立姿态)。", PropertyList(),
                       [this](const PropertyList& properties) -> ReturnValue {
                           if (!WriteFrame(CMD_CENTER_ALL, 0, 0)) {
                               return std::string("error: 舵机控制器无响应");
                           }
                           return true;
                       });

    snprintf(desc, sizeof(desc),
             "读取指定通道舵机的当前角度。ch: 通道号(0~%d)。返回角度(度,一位小数)。", cfg_.ch_max);
    mcp_server.AddTool("self.servo.get_angle", desc,
                       PropertyList({
                           Property("ch", kPropertyTypeInteger, 0, cfg_.ch_max),
                       }),
                       [this](const PropertyList& properties) -> ReturnValue {
                           int ch = properties["ch"].value<int>();
                           uint16_t angle_x10 = 0;
                           if (!ReadWord(CMD_SEL_READ, ch, angle_x10)) {
                               return std::string("error: 舵机控制器无响应");
                           }
                           char buf[32];
                           snprintf(buf, sizeof(buf), "%.1f 度", angle_x10 / 10.0);
                           return std::string(buf);
                       });

    mcp_server.AddTool(
        "self.servo.get_info", "读取舵机控制器固件版本和丢帧计数(用于诊断通信健康度)。",
        PropertyList(), [this](const PropertyList& properties) -> ReturnValue {
            uint16_t version = 0;
            uint16_t dropped = 0;
            if (!ReadWord(CMD_FW_VERSION, 0, version)) {
                return std::string("error: 舵机控制器无响应");
            }
            if (!ReadWord(CMD_DROP_COUNT, 0, dropped)) {
                return std::string("error: 舵机控制器无响应");
            }
            char buf[64];
            snprintf(buf, sizeof(buf), "固件版本: 0x%04X, 丢帧计数: %u", version, dropped);
            return std::string(buf);
        });
}
