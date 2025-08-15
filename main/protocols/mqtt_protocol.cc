#include "mqtt_protocol.h"
#include <sstream>
#include "board.h"
#include "application.h"
#include "settings.h"
#include "system_info.h"
#include <esp_log.h>
#include <ml307_mqtt.h>
#include <cstring>
#include <arpa/inet.h>
#include "assets/lang_config.h"
#include <cctype>

#define TAG "MQTT"

// 构造函数，创建事件组
MqttProtocol::MqttProtocol() {
    event_group_handle_ = xEventGroupCreate();
}

// 析构函数，清理资源
MqttProtocol::~MqttProtocol() {
    ESP_LOGI(TAG, "MqttProtocol deinit");
    if (mqtt_ != nullptr) {
        delete mqtt_;
    }
    vEventGroupDelete(event_group_handle_);
}

// 启动协议，实际上是启动MQTT客户端
bool MqttProtocol::Start() {
    return StartMqttClient(false);
}

// 启动MQTT客户端，包括连接和订阅
bool MqttProtocol::StartMqttClient(bool report_error) {
    // 如果客户端已存在，先删除再创建，确保重新连接
    if (mqtt_ != nullptr) {
        ESP_LOGW(TAG, "Mqtt client already started, reconnecting...");
        delete mqtt_;
        mqtt_ = nullptr;
    }

    // 从NVS读取MQTT配置
    Settings settings("mqtt", true);

    endpoint_ = settings.GetString("endpoint");
    client_id_ = settings.GetString("client_id");
    username_ = settings.GetString("username");
    password_ = settings.GetString("password");
    int keepalive_interval = settings.GetInt("keepalive", 90);
    subscribe_topic_ = settings.GetString("subscribe_topic");

    // 使用设备MAC地址生成唯一的设备ID和相关主题
    std::string user_id3 = SystemInfo::GetMacAddressDecimal();
    user_id3_ = user_id3;

    std::string phone_control_topic = "doll/control/" + user_id3;
    std::string languagesType_topic = "doll/set/" + user_id3;
    std::string moan_topic = "doll/control_moan/" + user_id3;

    // 从NVS加载保存的语言设置
    std::string saved_language = LoadLanguageTypeFromNVS();
    if (!saved_language.empty()) {
        ESP_LOGI(TAG, "Loaded language type from NVS: %s", saved_language.c_str());
    }

    // 根据设备ID和语言设置生成发布主题
    publish_topic_ = "stt/doll/" + user_id3 + "/" + saved_language;
    ESP_LOGI(TAG, "Publish topic: %s", publish_topic_.c_str());

    // 生成服务端VAD检测订阅主题
    vad_detection_topic_ = "speech/doll/" + user_id3;
    ESP_LOGI(TAG, "VAD detection topic: %s", vad_detection_topic_.c_str());

    // 检查MQTT服务器地址是否已配置
    if (endpoint_.empty()) {
        ESP_LOGW(TAG, "MQTT endpoint is not specified");
        if (report_error) {
            SetError(Lang::Strings::SERVER_NOT_FOUND);
        }
        return false;
    }

    // 创建并配置MQTT客户端实例
    mqtt_ = Board::GetInstance().CreateMqtt();
    mqtt_->SetKeepAlive(keepalive_interval);

    // 注册断开连接回调
    mqtt_->OnDisconnected([this]() {
        ESP_LOGI(TAG, "Disconnected from endpoint");
    });

    // 注册消息接收回调
    mqtt_->OnMessage([this, languagesType_topic, phone_control_topic, moan_topic](const std::string& topic, const std::string& payload) {
        // 仅在收到 JSON 时输出日志；其它消息降为 DEBUG，避免播放/音频相关噪声日志
        if (!payload.empty() && payload[0] == '{') {
            ESP_LOGI(TAG, "JSON: %s", payload.c_str());
        } else {
            ESP_LOGD(TAG, "Non-JSON on %s (%u bytes)", topic.c_str(), (unsigned)payload.size());
        }

        // 1) 优先处理服务端VAD检测消息
        if (topic == vad_detection_topic_) {
            HandleVadDetectionMessage(payload);
            return;
        }

        // 2) 处理原有主题
        if (topic == subscribe_topic_) {
            // 如果是JSON消息 (以'{'开头)
            if (!payload.empty() && payload[0] == '{') {
                ESP_LOGD(TAG, "Received JSON message: %s", payload.c_str());
                if (on_incoming_json_ != nullptr) {
                    cJSON* root = cJSON_Parse(payload.c_str());
                    if (root != nullptr) {
                        on_incoming_json_(root);
                        cJSON_Delete(root);
                    }
                }
            } else {
                // 否则，视为音频数据包（服务器发送纯OPUS payload）
                // 直接传递原始数据，避免AudioStreamPacket封装开销
                if (on_incoming_audio_ != nullptr) {
                    // 直接将payload转换为原始音频数据
                    std::vector<uint8_t> raw_data(reinterpret_cast<const uint8_t*>(payload.data()),
                                                 reinterpret_cast<const uint8_t*>(payload.data()) + payload.size());

                    // 详细记录每个音频包的接收情况
                    static uint32_t s_audio_log_counter = 0;
                    static auto s_last_audio_time = std::chrono::steady_clock::now();
                    auto current_time = std::chrono::steady_clock::now();
                    auto interval_ms = std::chrono::duration_cast<std::chrono::milliseconds>(current_time - s_last_audio_time).count();
                    s_last_audio_time = current_time;

                    // ESP_LOGI(TAG, "AUDIO: recv opus frame #%" PRIu32 ": bytes=%zu, interval=%lldms",
                    //          ++s_audio_log_counter, raw_data.size(), interval_ms);
                    // 直接传递原始数据给应用层，避免packet封装
                    on_incoming_audio_(std::move(raw_data));
                }
            }
        }
        else if (topic == phone_control_topic) {
            // 处理手机控制消息
            ESP_LOGD(TAG, "Received control message: %s", payload.c_str());
            cJSON* root = cJSON_Parse(payload.c_str());
            if (root != nullptr) {
                if (on_incoming_json_ != nullptr) {
                    on_incoming_json_(root);
                }
                cJSON_Delete(root);
            }
        }
        else if (topic == languagesType_topic) {
            // 处理语言设置消息
            ESP_LOGD(TAG, "Received language setting: %s", payload.c_str());
            cJSON* root = cJSON_Parse(payload.c_str());
            if (root != nullptr) {
                if (on_incoming_json_ != nullptr) {
                    on_incoming_json_(root);
                }
                cJSON_Delete(root);
            }
        } else if (topic == moan_topic) {
            // 处理呻吟声控制消息
          ESP_LOGI(TAG, "Received moan: %s", payload.c_str());
          cJSON* root = cJSON_Parse(payload.c_str());
          if (root != nullptr) {
            if (on_incoming_json_ != nullptr) {
              on_incoming_json_(root);
            }
            cJSON_Delete(root);
          }
        } else {
          ESP_LOGD(TAG, "Unhandled topic: %s", topic.c_str());
        }
    });

    // 连接到MQTT服务器
    ESP_LOGI(TAG, "Connecting to MQTT broker: %s", endpoint_.c_str());
    if (!mqtt_->Connect(endpoint_, MWTT_PORT, client_id_, username_, password_)) {
        ESP_LOGE(TAG, "Failed to connect to endpoint");
        SetError(Lang::Strings::SERVER_NOT_CONNECTED);
        return false;
    }

    ESP_LOGI(TAG, "Connected to endpoint");

    // 订阅相关主题
    if (!subscribe_topic_.empty()) {
        mqtt_->Subscribe(subscribe_topic_, 2);
        ESP_LOGI(TAG, "Subscribing to topic: %s", subscribe_topic_.c_str());
        mqtt_->Subscribe(phone_control_topic, 0);
        ESP_LOGI(TAG, "phone_control_topic: %s", phone_control_topic.c_str());
        mqtt_->Subscribe(languagesType_topic, 0);
        ESP_LOGI(TAG, "languagesType_topic: %s", languagesType_topic.c_str());
        mqtt_->Subscribe(moan_topic, 0);
        ESP_LOGI(TAG, "moan_topic: %s", moan_topic.c_str());
        // 新增订阅：服务端VAD检测主题
        mqtt_->Subscribe(vad_detection_topic_, 1);
        ESP_LOGI(TAG, "vad_detection_topic: %s", vad_detection_topic_.c_str());
    }

    return true;
}

// 更新语言设置
void MqttProtocol::UpdateLanguage(const std::string& language) {
    languagesType_ = language;
    std::string user_id = SystemInfo::GetMacAddressDecimal();
    publish_topic_ = "stt/doll/" + user_id + "/" + language;
    ESP_LOGI(TAG, "Updated publish topic to: %s, language: %s", publish_topic_.c_str(), language.c_str());
}

// 发送唤醒消息
void MqttProtocol::WakeupCall() {
    std::string user_id = SystemInfo::GetMacAddressDecimal();
    std::string wakeup_topic = "stt/audio/text";

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "device_id", user_id.c_str());
    cJSON_AddStringToObject(root, "device_type", "doll");
    cJSON_AddStringToObject(root, "stt_text", "Device is ready#");
    cJSON_AddStringToObject(root, "modal_type", "audio");
    char* json_string = cJSON_PrintUnformatted(root);
    mqtt_->Publish(wakeup_topic, json_string);

    free(json_string);
    cJSON_Delete(root);

    ESP_LOGI(TAG, "Published wakeup call to %s", wakeup_topic.c_str());
}

// 从NVS加载语言类型
std::string MqttProtocol::LoadLanguageTypeFromNVS() {
    std::string saved_language;
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("config", NVS_READWRITE, &nvs_handle);
    if (err == ESP_OK) {
        size_t required_size;
        err = nvs_get_str(nvs_handle, "languagesType", NULL, &required_size);
        if (err == ESP_OK) {
            char* lang = new char[required_size];
            err = nvs_get_str(nvs_handle, "languagesType", lang, &required_size);
            if (err == ESP_OK) {
                saved_language = std::string(lang);
            }
            delete[] lang;
        }
        nvs_close(nvs_handle);
    }
    return saved_language;
}

// 发送文本消息
bool MqttProtocol::SendText(const std::string& text) {
    if (publish_topic_.empty()) {
        return false;
    }
    if (!mqtt_->Publish(publish_topic_, text)) {
        ESP_LOGE(TAG, "Failed to publish message: %s", text.c_str());
        SetError(Lang::Strings::SERVER_ERROR);
        return false;
    }
    return true;
}

// 发送音频数据 - 支持分片传输
bool MqttProtocol::SendAudio(const AudioStreamPacket& packet) {
    // 降低日志频率，并修正格式化规约：使用 %u 搭配显式转换，避免某些平台下 %zu 导致变参错位
    ESP_LOGD(TAG, "SendAudio: payload_size=%u, sample_rate=%d, frame_duration=%d",
             (unsigned)packet.payload.size(), packet.sample_rate, packet.frame_duration);

    if (publish_topic_.empty() || mqtt_ == nullptr || !mqtt_->IsConnected()) {
        ESP_LOGE(TAG, "MQTT client not connected or topic empty");
        audio_stats_.failed_packets++;
        return false;
    }

    // 更新统计信息
    audio_stats_.total_packets++;
    audio_stats_.total_bytes += packet.payload.size();
    audio_stats_.last_transmission = std::chrono::steady_clock::now();

    // 直接使用payload数据，不处理时间戳（参考S3项目）
    std::string mqtt_payload(reinterpret_cast<const char*>(packet.payload.data()),
                            packet.payload.size());

    // 分片发送大的音频数据（参考S3项目实现）
    const size_t MAX_CHUNK_SIZE = 1024;  // 每片最大1KB

    if (mqtt_payload.size() <= MAX_CHUNK_SIZE) {
        // 小数据包直接发送
        if (!mqtt_->Publish(publish_topic_, std::move(mqtt_payload))) {
            ESP_LOGE(TAG, "Failed to publish audio message");
            audio_stats_.failed_packets++;
            SetError(Lang::Strings::SERVER_ERROR);
            return false;
        }
        audio_stats_.total_chunks++;
        // 成功发送小包（单帧），在 DEBUG 级别记录一次成功日志
        ESP_LOGD(TAG, "Audio packet published: bytes=%u", (unsigned)mqtt_payload.size());

    } else {
        // 大数据包分片发送
        size_t remaining = mqtt_payload.size();
        size_t offset = 0;
        size_t total_chunks = (mqtt_payload.size() + MAX_CHUNK_SIZE - 1) / MAX_CHUNK_SIZE;

        // 修正 size_t 打印格式以避免潜在不兼容
        ESP_LOGI(TAG, "Sending large audio packet in chunks: total_size=%u, chunks=%u",
                 (unsigned)mqtt_payload.size(), (unsigned)total_chunks);

        while (remaining > 0) {
            size_t chunk_size = std::min(remaining, MAX_CHUNK_SIZE);
            std::string chunk_payload(mqtt_payload.data() + offset, chunk_size);

            if (!mqtt_->Publish(publish_topic_, chunk_payload, 0)) {  // QoS 0，低延迟
                ESP_LOGE(TAG, "Failed to publish audio chunk at offset %u", (unsigned)offset);
                audio_stats_.failed_packets++;
                SetError(Lang::Strings::SERVER_ERROR);
                return false;
            }

            audio_stats_.total_chunks++;
            remaining -= chunk_size;
            offset += chunk_size;
        }

        ESP_LOGI(TAG, "Successfully sent audio packet in %u chunks", (unsigned)total_chunks);
    }

    // 周期性输出统计信息，避免日志过于频繁（每100包一次，约每3秒）
    // if ((audio_stats_.total_packets % 100) == 0) {
    //     LogAudioStats();
    // }

    return true;
}

// 打印音频传输统计信息（用于调试）
void MqttProtocol::LogAudioStats() {
    auto now = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(
        now - audio_stats_.last_transmission).count();

    ESP_LOGI(TAG, "=== Audio Transmission Stats ===");
    ESP_LOGI(TAG, "Total packets: %u", (unsigned)audio_stats_.total_packets);
    ESP_LOGI(TAG, "Total chunks: %u", (unsigned)audio_stats_.total_chunks);
    ESP_LOGI(TAG, "Failed packets: %u", (unsigned)audio_stats_.failed_packets);
    ESP_LOGI(TAG, "Total bytes: %u", (unsigned)audio_stats_.total_bytes);
    ESP_LOGI(TAG, "Success rate: %.2f%%",
             audio_stats_.total_packets > 0 ?
             (100.0 * (audio_stats_.total_packets - audio_stats_.failed_packets) / audio_stats_.total_packets) : 0.0);
    ESP_LOGI(TAG, "Avg chunks per packet: %.2f",
             audio_stats_.total_packets > 0 ?
             (float)audio_stats_.total_chunks / audio_stats_.total_packets : 0.0);
    ESP_LOGI(TAG, "Last transmission: %d seconds ago", (int)duration);
    ESP_LOGI(TAG, "================================");
}

// 发送IMU（陀螺仪）数据
void MqttProtocol::SendImuStatesAndValue(const t_sQMI8658& imu_data, int touch_value) {
  if (mqtt_ == nullptr || !mqtt_->IsConnected()) {
    ESP_LOGE(TAG, "MQTT client not connected");
    return;
  }

  if (user_id3_.empty()) {
    ESP_LOGE(TAG, "User ID is empty");
    return;
  }

  // 构建JSON消息
  cJSON* root = cJSON_CreateObject();
  if (root == NULL) {
    ESP_LOGE(TAG, "Failed to create JSON object");
    return;
  }

  cJSON_AddNumberToObject(root, "imu_type", imu_data.motion);
  cJSON_AddNumberToObject(root, "gx", imu_data.gyr_x);
  cJSON_AddNumberToObject(root, "gy", imu_data.gyr_y);
  cJSON_AddNumberToObject(root, "gz", imu_data.gyr_z);
  cJSON_AddNumberToObject(root, "ax", imu_data.acc_x);
  cJSON_AddNumberToObject(root, "ay", imu_data.acc_y);
  cJSON_AddNumberToObject(root, "az", imu_data.acc_z);
  cJSON_AddNumberToObject(root, "touch_value", touch_value);
  cJSON_AddStringToObject(root, "device_id", user_id3_.c_str());

  char* message_str = cJSON_PrintUnformatted(root);
  if (message_str == NULL) {
    ESP_LOGE(TAG, "Failed to print JSON");
    cJSON_Delete(root);
    return;
  }

  std::string message(message_str);
  std::string imu_topic = "doll/imu_status";

  ESP_LOGI(TAG, "Sending IMU data: %s to topic: %s", message_str, imu_topic.c_str());

  mqtt_->Publish(imu_topic, message);

  cJSON_free(message_str);
  cJSON_Delete(root);
}

// 关闭音频通道（在纯MQTT模式下，这通常只是一个逻辑上的关闭）
void MqttProtocol::CloseAudioChannel() {
    ESP_LOGI(TAG, "Closing audio channel");
    if (mqtt_ && !publish_topic_.empty()) {
        // 发送"END"消息，服务器可以此作为音频流结束的标志
        mqtt_->Publish(publish_topic_, "END", 1);
    }

    if (on_audio_channel_closed_ != nullptr) {
        on_audio_channel_closed_();
    }
}

// 打开音频通道（在纯MQTT模式下，只要MQTT连接着，通道就是打开的）
bool MqttProtocol::OpenAudioChannel() {
    if (mqtt_ == nullptr || !mqtt_->IsConnected()) {
        ESP_LOGI(TAG, "MQTT is not connected, trying to connect now");
        if (!StartMqttClient(true)) {
            return false;
        }
    }

    if (on_audio_channel_opened_ != nullptr) {
        on_audio_channel_opened_();
    }
    return true;
}

// 检查音频通道是否打开
bool MqttProtocol::IsAudioChannelOpened() const {
    // 音频通道的状态等同于MQTT客户端的连接状态
    return mqtt_ != nullptr && mqtt_->IsConnected() && !error_occurred_;
}

// 解析服务端VAD检测消息（支持纯文本 "END" 或 JSON）
void MqttProtocol::HandleVadDetectionMessage(const std::string& payload) {
    // 先兼容纯文本 END：去除首尾空白后匹配（大小写不敏感）
    std::string trimmed = payload;
    size_t i = 0, j = trimmed.size();
    while (i < j && std::isspace((unsigned char)trimmed[i])) ++i;
    while (j > i && std::isspace((unsigned char)trimmed[j - 1])) --j;
    trimmed = trimmed.substr(i, j - i);
    if (trimmed.size() == 3 &&
        (trimmed[0] == 'E' || trimmed[0] == 'e') &&
        (trimmed[1] == 'N' || trimmed[1] == 'n') &&
        (trimmed[2] == 'D' || trimmed[2] == 'd')) {
        ESP_LOGI(TAG, "VAD detection: ----------------------------plain END received----------------------------");
        HandleServerVadDetection();
        return;
    }

    // 再按 JSON 解析
    cJSON* root = cJSON_Parse(payload.c_str());
    if (!root) {
        ESP_LOGW(TAG, "VAD detection: invalid payload (neither plain END nor JSON)");
        return;
    }

    // 兼容 JSON 字符串形式："END"
    if (cJSON_IsString(root) && root->valuestring && strcasecmp(root->valuestring, "END") == 0) {
        HandleServerVadDetection();
        cJSON_Delete(root);
        return;
    }

    auto cleanup = [&]() { cJSON_Delete(root); };

    cJSON* type = cJSON_GetObjectItem(root, "type");
    cJSON* trigger = cJSON_GetObjectItem(root, "trigger");
    cJSON* message = cJSON_GetObjectItem(root, "message");
    cJSON* language = cJSON_GetObjectItem(root, "language");
    cJSON* ts = cJSON_GetObjectItem(root, "timestamp");

    bool is_end = type && cJSON_IsString(type) && strcmp(type->valuestring,"speech_end")==0;
    bool trig_ok=false;
    if (trigger && cJSON_IsString(trigger)) {
      trig_ok = strcmp(trigger->valuestring,"vad_detection")==0 ||
                strcmp(trigger->valuestring,"valid_speech_confirmed")==0;
    }
    bool msg_ok = message && cJSON_IsString(message) && strcasecmp(message->valuestring,"END")==0;
    if (!(is_end && trig_ok && msg_ok)) { 
        ESP_LOGW(TAG,"VAD detection: unexpected"); 
        cleanup(); 
        return; 
    }
    HandleServerVadDetection(); 
    cleanup();
}

// 服务端VAD检测处理 通知APP层（不做本地防抖）
void MqttProtocol::HandleServerVadDetection() {
    ESP_LOGI(TAG, "Server VAD detected speech end, notify application");
    if (on_server_vad_detected_) {
        on_server_vad_detected_();
    } else {
        ESP_LOGW(TAG, "on_server_vad_detected_ callback not set");
    }
}


// 发送取消TTS（文本转语音）的请求
void MqttProtocol::SendCancelTTS(bool f) {
    std::string device_id = SystemInfo::GetMacAddressDecimal();
    std::stringstream ss;
    ss << "{\"user_id\":\"" << device_id << "\",\"action\":\"" << (f ? "finish" : "stop") << "\"}";
    std::string message = ss.str();

    ESP_LOGI(TAG, "Sending CancelTTS message: %s", message.c_str());
    mqtt_->Publish("tts/cancel", message, 2);
    ESP_LOGI(TAG, "CancelTTS message sent to topic: tts/cancel");
}
