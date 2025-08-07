#ifndef MQTT_PROTOCOL_H
#define MQTT_PROTOCOL_H


#include "protocol.h"
#include <mqtt.h>
#include <udp.h>
#include <cJSON.h>
#include <mbedtls/aes.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>

#include <functional>
#include <string>
#include <map>
#include <mutex>
#include "esp32_s3_szp.h"

#define MQTT_PING_INTERVAL_SECONDS 90
#define MQTT_RECONNECT_INTERVAL_MS 10000

#define MQTT_PROTOCOL_SERVER_HELLO_EVENT (1 << 0)
#define MWTT_PORT 1883


class MqttProtocol : public Protocol {
public:
    MqttProtocol();
    ~MqttProtocol();

    bool Start() override;
    bool SendAudio(const AudioStreamPacket& packet) override;
    bool OpenAudioChannel() override;
    void CloseAudioChannel() override;
    bool IsAudioChannelOpened() const override;

    //F移植 添加  ---使用C3原版 注释掉
    // void SetOnIncomingAudio(std::function<void(std::vector<uint8_t>&&)> callback) {
    //     on_incoming_audio_ = std::move(callback);
    // }
    //F移植 添加
    void SendCancelTTS(bool f=false );//发送取消tts消息
    //F移植 添加
    void SendImuStatesAndValue( const t_sQMI8658& imu_data,
        int touch_value);//发送陀螺仪数据

    //F移植 添加
    // 获取音量控制值并重置标志
    bool GetVolumeControl(std::string& value) {
        if (has_volume_control_) {
            value = volume_control_value_;
            has_volume_control_ = false;
            return true;
        }
        return false;
    }

    //F移植 添加     更新语言
    void UpdateLanguage(const std::string& language);
    void WakeupCall();


private:
    EventGroupHandle_t event_group_handle_;

    // std::string publish_topic_;

    
    std::string endpoint_;//F移植 添加
    std::string client_id_;//F移植 添加
    std::string username_;//F移植 添加
    std::string password_;//F移植 添加
    std::string subscribe_topic_;//F移植 添加
    std::string publish_topic_;//F移植 添加
    std::string languagesType_;//F移植 添加  // 用于保存 languagesType 值
    std::string user_id3_;//F移植 添加

    std::mutex channel_mutex_;
    Mqtt* mqtt_ = nullptr;
    Udp* udp_ = nullptr;
    mbedtls_aes_context aes_ctx_;
    std::string aes_nonce_;
    std::string udp_server_;
    int udp_port_;
    uint32_t local_sequence_;
    uint32_t remote_sequence_;

    //使用C3原版
    //std::function<void(std::vector<uint8_t>&&)> on_incoming_audio_;//F移植 添加

    // 音量控制相关
    std::string volume_control_value_;//F移植 添加
    bool has_volume_control_ = false;//F移植 添加
    // 关机控制
    bool shutdown_requested_ = false;//F移植 添加


    bool StartMqttClient(bool report_error=false);
    void ParseServerHello(const cJSON* root);
    std::string DecodeHexString(const std::string& hex_string);

    bool SendText(const std::string& text) override;

    // 从 NVS 读取语言类型 
    std::string LoadLanguageTypeFromNVS();//F移植 添加


    std::string GetHelloMessage();
};


#endif // MQTT_PROTOCOL_H
