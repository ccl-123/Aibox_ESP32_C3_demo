#include "application.h"
#include "board.h"
#include "display.h"
#include "system_info.h"
#include "ml307_ssl_transport.h"
#include "audio_codec.h"
#include "mqtt_protocol.h"
#include "websocket_protocol.h"
#include "font_awesome_symbols.h"
#include "iot/thing_manager.h"
#include "assets/lang_config.h"
#include "mcp_server.h"
#include "audio_debugger.h"

#if CONFIG_USE_AUDIO_PROCESSOR
#include "afe_audio_processor.h"
#else
#include "no_audio_processor.h"
#endif

#if CONFIG_USE_AFE_WAKE_WORD
#include "afe_wake_word.h"
#elif CONFIG_USE_ESP_WAKE_WORD
#include "esp_wake_word.h"
#else
#include "no_wake_word.h"
#endif

#include <cstring>
#include <esp_log.h>
#include <esp_system.h>
#include <cJSON.h>
#include <driver/gpio.h>
#include <arpa/inet.h>

#define TAG "Application"


static const char* const STATE_STRINGS[] = {
    "unknown",
    "starting",
    "configuring",
    "idle",
    "connecting",
    "listening",
    "speaking",
    "upgrading",
    "activating",
    "audio_testing",
    "fatal_error",
    "invalid_state"
};

Application::Application() {
    event_group_ = xEventGroupCreate();
    // 创建2个高优先级BackgroundTask线程，专门处理音频解码等实时任务
    // 优先级6：高于默认任务(1-2)，但低于关键系统任务(7+)
    // 栈大小28KB，足够处理OPUS编码等复杂任务
    background_task_ = new BackgroundTask(4096 * 7, 2, 6);

    ////初始化OTA相关参数
    ota_.SetCheckVersionUrl(CONFIG_OTA_URL);
    ota_.SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());

#if CONFIG_USE_DEVICE_AEC
    aec_mode_ = kAecOnDeviceSide;
#elif CONFIG_USE_SERVER_AEC
    aec_mode_ = kAecOnServerSide;
#else
    aec_mode_ = kAecOff;
#endif

#if CONFIG_USE_AUDIO_PROCESSOR
    audio_processor_ = std::make_unique<AfeAudioProcessor>();
#else
    audio_processor_ = std::make_unique<NoAudioProcessor>();
#endif

#if CONFIG_USE_AFE_WAKE_WORD
    wake_word_ = std::make_unique<AfeWakeWord>();
#elif CONFIG_USE_ESP_WAKE_WORD
    wake_word_ = std::make_unique<EspWakeWord>();
#else
    wake_word_ = std::make_unique<NoWakeWord>();
#endif

    esp_timer_create_args_t clock_timer_args = {
        .callback = [](void* arg) {
            Application* app = (Application*)arg;
            app->OnClockTimer();
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "clock_timer",
        .skip_unhandled_events = true
    };
    esp_timer_create(&clock_timer_args, &clock_timer_handle_);
}

Application::~Application() {
    if (clock_timer_handle_ != nullptr) {
        esp_timer_stop(clock_timer_handle_);
        esp_timer_delete(clock_timer_handle_);
    }
    if (background_task_ != nullptr) {
        delete background_task_;
    }
    vEventGroupDelete(event_group_);
}

void Application::CheckNewVersion() {
  auto& board = Board::GetInstance();
  auto display = board.GetDisplay();

  // 设置设备信息作为POST数据
  ota_.SetPostData(board.GetJson());

  while (true) {
    if (ota_.CheckVersion()) {
      if (ota_.HasNewVersion()) {
        ESP_LOGI(TAG, "New firmware version detected: %s", ota_.GetFirmwareVersion().c_str());

        // 设置检查完成事件，让主线程继续执行并设置设备状态为空闲
        xEventGroupSetBits(event_group_, CHECK_NEW_VERSION_DONE_EVENT);

        // 等待设备进入空闲状态，确保升级安全
        ESP_LOGI(TAG, "Waiting for device to enter idle state before upgrade...");
        do {
          vTaskDelay(pdMS_TO_TICKS(1000)); // 减少等待时间，提高响应性
        } while (GetDeviceState() != kDeviceStateIdle);

        ESP_LOGI(TAG, "Device is now idle, scheduling upgrade...");
        // 使用主任务执行升级，不可取消
        Schedule([this, &board, display]() {
          ESP_LOGI(TAG, "Executing upgrade task in main thread...");
          SetDeviceState(kDeviceStateUpgrading);

          // 显示升级状态
          if (display != nullptr) {
            display->SetIcon(FONT_AWESOME_DOWNLOAD);
            display->SetStatus(("新版本 " + ota_.GetFirmwareVersion()).c_str());
          }

          auto codec = board.GetAudioCodec();
          codec->EnableOutput(true);

          // 播放升级音频提示
          PlaySound(Lang::Sounds::P3_UPGRADE);
          ESP_LOGI(TAG, "Starting firmware upgrade...");
          vTaskDelay(pdMS_TO_TICKS(2500));

          // 预先关闭音频输入输出，避免升级过程中的音频操作干扰
          codec->EnableInput(false);  // 关闭麦克风输入

          // 清空音频队列并通知等待的线程
          {
            std::lock_guard<std::mutex> lock(mutex_);
            audio_decode_queue_.clear();
            audio_send_queue_.clear();
            audio_decode_cv_.notify_all();
          }

          // 停止正在进行的背景任务
          background_task_->WaitForCompletion();

          // 停止音频处理器和唤醒词检测
          audio_processor_->Stop();
          wake_word_->StopDetection();

          // 关闭协议连接，释放网络资源
          if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
          }

          // 等待任何正在进行的音频操作完成
          vTaskDelay(pdMS_TO_TICKS(1000));

          // 最后关闭音频输出，确保升级提示音播放完成
          codec->EnableOutput(false);

          // 开始升级，带进度回调
          ota_.StartUpgrade([this, display](int progress, size_t speed) {
            char buffer[64];
            snprintf(buffer, sizeof(buffer), "%d%% %zuKB/s", progress, speed / 1024);
            ESP_LOGI(TAG, "Upgrade progress: %s", buffer);

            // 更新显示状态，避免频繁更新
            static int last_displayed_progress = -1;
            if (progress != last_displayed_progress && (progress % 5 == 0 || progress >= 95)) {
              if (display != nullptr) {
                display->SetStatus(buffer);
              }
              last_displayed_progress = progress;
            }

            // 在关键进度点提供额外日志
            if (progress == 50) {
              ESP_LOGI(TAG, "Upgrade halfway complete...");
            } else if (progress >= 90) {
              ESP_LOGI(TAG, "Upgrade nearly complete, preparing to reboot...");
            }
          });

          // 如果升级成功，设备会重启，不会执行到这里
          // 如果执行到这里，说明升级失败
          ESP_LOGE(TAG, "Firmware upgrade failed!");

          // 显示升级失败状态
          display->SetStatus("升级失败");
          display->SetEmotion("sad");

          // 尝试恢复音频系统
          try {
            codec->EnableOutput(true);
            codec->EnableInput(true);

            // 重新启动音频处理器和唤醒词检测
            audio_processor_->Start();
            wake_word_->StartDetection();

            ESP_LOGI(TAG, "Audio system recovery attempted");
          } catch (...) {
            ESP_LOGE(TAG, "Failed to recover audio system");
          }

          // 等待用户看到错误信息
          vTaskDelay(pdMS_TO_TICKS(5000));

          // 升级失败后重启设备
          ESP_LOGI(TAG, "Restarting device after upgrade failure...");
          esp_restart();
        });
        return; // 升级任务已调度，退出版本检查循环
      } else {
        // 没有新版本，标记当前版本为有效
        ota_.MarkCurrentVersionValid();
        ESP_LOGI(TAG, "Current version is up to date: %s", ota_.GetCurrentVersion().c_str());
      }

      // 设置检查完成事件
      xEventGroupSetBits(event_group_, CHECK_NEW_VERSION_DONE_EVENT);
      return;
    }

    // 版本检查失败，记录错误并重试
    ESP_LOGW(TAG, "Version check failed, retrying in 60 seconds...");

    // 如果是网络问题，可能需要更长的重试间隔
    static int retry_count = 0;
    retry_count++;

    if (retry_count >= 5) {
      ESP_LOGE(TAG, "Version check failed %d times, extending retry interval", retry_count);
      vTaskDelay(pdMS_TO_TICKS(300000)); // 5分钟后重试
      retry_count = 0; // 重置计数器
    } else {
      vTaskDelay(pdMS_TO_TICKS(60000)); // 60秒后重试
    }
  }
}

void Application::ShowActivationCode(const std::string& code, const std::string& message) {
    struct digit_sound {
        char digit;
        const std::string_view& sound;
    };
    static const std::array<digit_sound, 10> digit_sounds{{
        digit_sound{'0', Lang::Sounds::P3_0},
        digit_sound{'1', Lang::Sounds::P3_1}, 
        digit_sound{'2', Lang::Sounds::P3_2},
        digit_sound{'3', Lang::Sounds::P3_3},
        digit_sound{'4', Lang::Sounds::P3_4},
        digit_sound{'5', Lang::Sounds::P3_5},
        digit_sound{'6', Lang::Sounds::P3_6},
        digit_sound{'7', Lang::Sounds::P3_7},
        digit_sound{'8', Lang::Sounds::P3_8},
        digit_sound{'9', Lang::Sounds::P3_9}
    }};

    // This sentence uses 9KB of SRAM, so we need to wait for it to finish
    Alert(Lang::Strings::ACTIVATION, message.c_str(), "happy", Lang::Sounds::P3_ACTIVATION);

    for (const auto& digit : code) {
        auto it = std::find_if(digit_sounds.begin(), digit_sounds.end(),
            [digit](const digit_sound& ds) { return ds.digit == digit; });
        if (it != digit_sounds.end()) {
            PlaySound(it->sound);
        }
    }
}

void Application::Alert(const char* status, const char* message, const char* emotion, const std::string_view& sound) {
    ESP_LOGW(TAG, "Alert %s: %s [%s]", status, message, emotion);
    auto display = Board::GetInstance().GetDisplay();
    if (display != nullptr) {
        display->SetStatus(status);
        display->SetEmotion(emotion);
        display->SetChatMessage("system", message);
    }
    if (!sound.empty()) {
        ResetDecoder();
        PlaySound(sound);
    }
}

void Application::DismissAlert() {
    if (device_state_ == kDeviceStateIdle) {
        auto display = Board::GetInstance().GetDisplay();
        if (display != nullptr) {
            display->SetStatus(Lang::Strings::STANDBY);
            display->SetEmotion("neutral");
            display->SetChatMessage("system", "");
        }
    }
}

void Application::PlaySound(const std::string_view& sound) {
    // Wait for the previous sound to finish
    {
        std::unique_lock<std::mutex> lock(mutex_);
        audio_decode_cv_.wait(lock, [this]() {
            return audio_decode_queue_.empty();
        });
    }

    background_task_->WaitForCompletion();

    const char* data = sound.data();
    size_t size = sound.size();
    for (const char* p = data; p < data + size; ) {
        auto p3 = (BinaryProtocol3*)p;
        p += sizeof(BinaryProtocol3);

        auto payload_size = ntohs(p3->payload_size);
        // 优化：直接提取原始音频数据，避免AudioStreamPacket封装
        std::vector<uint8_t> raw_data(payload_size);
        memcpy(raw_data.data(), p3->payload, payload_size);
        p += payload_size;

        std::lock_guard<std::mutex> lock(mutex_);
        audio_decode_queue_.emplace_back(std::move(raw_data));
    }
}

void Application::EnterAudioTestingMode() {
    ESP_LOGI(TAG, "Entering audio testing mode");
    ResetDecoder();
    SetDeviceState(kDeviceStateAudioTesting);
}

void Application::ExitAudioTestingMode() {
    ESP_LOGI(TAG, "Exiting audio testing mode");
    SetDeviceState(kDeviceStateWifiConfiguring);
    // 优化：将audio_testing_queue_转换为原始数据格式
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& packet : audio_testing_queue_) {
        audio_decode_queue_.emplace_back(std::move(packet.payload));
    }
    audio_testing_queue_.clear();
    audio_decode_cv_.notify_all();
}

void Application::ToggleChatState() {
    if (device_state_ == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (device_state_ == kDeviceStateWifiConfiguring) {
        EnterAudioTestingMode();
        return;
    } else if (device_state_ == kDeviceStateAudioTesting) {
        ExitAudioTestingMode();
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }

    if (device_state_ == kDeviceStateIdle) {
        Schedule([this]() {
            if (!protocol_->IsAudioChannelOpened()) {
                SetDeviceState(kDeviceStateConnecting);
                if (!protocol_->OpenAudioChannel()) {
                    return;
                }
            }

            SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
        });
    } else if (device_state_ == kDeviceStateSpeaking) {
        Schedule([this]() {
            AbortSpeaking(kAbortReasonNone);
        });
    } else if (device_state_ == kDeviceStateListening) {
        Schedule([this]() {
            protocol_->CloseAudioChannel();
        });
    }
}

void Application::StartListening() {
    if (device_state_ == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (device_state_ == kDeviceStateWifiConfiguring) {
        EnterAudioTestingMode();
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }
    
    if (device_state_ == kDeviceStateIdle) {
        Schedule([this]() {
            if (!protocol_->IsAudioChannelOpened()) {
                SetDeviceState(kDeviceStateConnecting);
                if (!protocol_->OpenAudioChannel()) {
                    return;
                }
            }

            SetListeningMode(kListeningModeManualStop);
        });
    } else if (device_state_ == kDeviceStateSpeaking) {
        Schedule([this]() {
            AbortSpeaking(kAbortReasonNone);
            SetListeningMode(kListeningModeManualStop);
        });
    }
}

void Application::StopListening() {
    if (device_state_ == kDeviceStateAudioTesting) {
        ExitAudioTestingMode();
        return;
    }

    const std::array<int, 3> valid_states = {
        kDeviceStateListening,
        kDeviceStateSpeaking,
        kDeviceStateIdle,
    };
    // If not valid, do nothing
    if (std::find(valid_states.begin(), valid_states.end(), device_state_) == valid_states.end()) {
        return;
    }

    Schedule([this]() {
        if (device_state_ == kDeviceStateListening) {
            protocol_->SendStopListening();
            SetDeviceState(kDeviceStateIdle);
        }
    });
}

void Application::Start() {
    auto& board = Board::GetInstance();
    SetDeviceState(kDeviceStateStarting);

    /* Setup the display */
    auto display = board.GetDisplay();

    /* Setup the audio codec */
    auto codec = board.GetAudioCodec();
    opus_decoder_ = std::make_unique<OpusDecoderWrapper>(codec->output_sample_rate(), 1, OPUS_FRAME_DURATION_MS);
    opus_encoder_ = std::make_unique<OpusEncoderWrapper>(16000, 1, OPUS_FRAME_DURATION_MS);
    if (aec_mode_ != kAecOff) {
        ESP_LOGI(TAG, "AEC mode: %d, setting opus encoder complexity to 0", aec_mode_);
        opus_encoder_->SetComplexity(0);
    } else if (board.GetBoardType() == "ml307") {
        ESP_LOGI(TAG, "ML307 board detected, setting opus encoder complexity to 5");
        opus_encoder_->SetComplexity(5);
    } else {
        ESP_LOGI(TAG, "WiFi board detected, setting opus encoder complexity to 0");
        opus_encoder_->SetComplexity(0);
    }

    if (codec->input_sample_rate() != 16000) {
        input_resampler_.Configure(codec->input_sample_rate(), 16000);
        reference_resampler_.Configure(codec->input_sample_rate(), 16000);
    }
    codec->Start();

#if CONFIG_USE_AUDIO_PROCESSOR
    xTaskCreatePinnedToCore([](void* arg) {
        Application* app = (Application*)arg;
        app->AudioLoop();
        vTaskDelete(NULL);
    }, "audio_loop", 4096 * 2, this, 8, &audio_loop_task_handle_, 1);
#else
    xTaskCreate([](void* arg) {
        Application* app = (Application*)arg;
        app->AudioLoop();
        vTaskDelete(NULL);
    }, "audio_loop", 4096 * 2, this, 8, &audio_loop_task_handle_);
#endif

    /* Start the clock timer to update the status bar */
    esp_timer_start_periodic(clock_timer_handle_, 1000000);

    /* Wait for the network to be ready */
    board.StartNetwork();

    // Update the status bar immediately to show the network state
    display->UpdateStatusBar(true);

    // Check for new firmware version or get the MQTT broker address
    xTaskCreate(
        [](void* arg) {
          Application* app = (Application*)arg;
          app->CheckNewVersion();
          vTaskDelete(NULL);
        },
        "check_new_version", 6800, this, 1, nullptr);

    // Initialize the protocol
    display->SetStatus(Lang::Strings::LOADING_PROTOCOL);

    // Add MCP common tools before initializing the protocol
#if CONFIG_IOT_PROTOCOL_MCP
    McpServer::GetInstance().AddCommonTools();
#endif

    if (ota_.HasMqttConfig()) {
        protocol_ = std::make_unique<MqttProtocol>();
    } else if (ota_.HasWebsocketConfig()) {
        protocol_ = std::make_unique<WebsocketProtocol>();
    } else {
        ESP_LOGW(TAG, "No protocol specified in the OTA config, using MQTT");
        protocol_ = std::make_unique<MqttProtocol>();
    }

    protocol_->OnNetworkError([this](const std::string& message) {
        SetDeviceState(kDeviceStateIdle);
        Alert(Lang::Strings::ERROR, message.c_str(), "sad", Lang::Sounds::P3_EXCLAMATION);
    });

    // 新增：服务端VAD检测回调 - 直接转Speaking状态
    protocol_->OnServerVadDetected([this]() {
        Schedule([this]() {

            // 仅在监听状态下处理
            if (device_state_ != kDeviceStateListening) {
                ESP_LOGW(TAG, "[Server-VAD] device not in listening state: %d", device_state_);
                return;
            }

            ESP_LOGI(TAG, "[Server-VAD] END received, transitioning to Speaking state");

            // 直接转换到Speaking状态
            SetDeviceState(kDeviceStateSpeaking);

        });
    });


    protocol_->OnIncomingAudio([this](std::vector<uint8_t>&& raw_data) {
        static uint32_t packet_counter = 0;
        static auto last_packet_time = std::chrono::steady_clock::now();
        auto current_time = std::chrono::steady_clock::now();
        auto interval_ms = std::chrono::duration_cast<std::chrono::milliseconds>(current_time - last_packet_time).count();
        last_packet_time = current_time;

        std::lock_guard<std::mutex> lock(mutex_);

        // 详细日志：每个包都记录，便于分析服务端发送间隔
        // ESP_LOGI(TAG, "[AUDIO-RX] Packet #%" PRIu32 ": size=%u bytes, interval=%dms, state=%d, 📦QUEUE=[%u/%d], 🔧TASKS=%d",
        //          ++packet_counter, (unsigned)raw_data.size(), (int)interval_ms, device_state_,
        //          (unsigned)audio_decode_queue_.size(), MAX_AUDIO_PACKETS_IN_QUEUE, active_decode_tasks_.load());

        // 检查是否应该接收音频数据
        if (!aborted_ && device_state_ == kDeviceStateSpeaking && audio_decode_queue_.size() < MAX_AUDIO_PACKETS_IN_QUEUE) {
            audio_decode_queue_.emplace_back(std::move(raw_data));
            // ESP_LOGI(TAG, "[AUDIO-RX] 🔊 Added packet to queue, 📦NEW_SIZE=[%u/%d]",
            //          (unsigned)audio_decode_queue_.size(), MAX_AUDIO_PACKETS_IN_QUEUE);
        } else {
            // 详细记录丢包原因
            const char* drop_reason = "unknown";
            if (aborted_) drop_reason = "aborted";
            else if (device_state_ != kDeviceStateSpeaking) drop_reason = "wrong_state";
            else if (audio_decode_queue_.size() >= MAX_AUDIO_PACKETS_IN_QUEUE) drop_reason = "queue_full";

            // ESP_LOGW(TAG, "[AUDIO-RX] ❌ DROPPED packet - reason:%s, aborted:%d state:%d 📦QUEUE=[%u/%d] 🔧TASKS=%d",
            //          drop_reason, aborted_ ? 1 : 0, device_state_, (unsigned)audio_decode_queue_.size(),
            //          MAX_AUDIO_PACKETS_IN_QUEUE, active_decode_tasks_.load());
        }
    });


    protocol_->OnAudioChannelOpened([this, codec, &board]() {
        board.SetPowerSaveMode(false);
        if (protocol_->server_sample_rate() != codec->output_sample_rate()) {
            ESP_LOGW(TAG, "Server sample rate %d does not match device output sample rate %d, resampling may cause distortion",
                protocol_->server_sample_rate(), codec->output_sample_rate());
        }

#if CONFIG_IOT_PROTOCOL_XIAOZHI
        auto& thing_manager = iot::ThingManager::GetInstance();
        protocol_->SendIotDescriptors(thing_manager.GetDescriptorsJson());
        std::string states;
        if (thing_manager.GetStatesJson(states, false)) {
            protocol_->SendIotStates(states);
        }
#endif
    });
    protocol_->OnAudioChannelClosed([this, &board]() {
        board.SetPowerSaveMode(true);
        Schedule([this]() {
            auto display = Board::GetInstance().GetDisplay();
            if (display != nullptr) {
                display->SetChatMessage("system", "");
            }
            SetDeviceState(kDeviceStateIdle);
        });
    });
    protocol_->OnIncomingJson([this, display](const cJSON* root) {
        // Parse JSON data
        auto type = cJSON_GetObjectItem(root, "type");
        if (strcmp(type->valuestring, "tts") == 0) {
            auto state = cJSON_GetObjectItem(root, "state");
            if (strcmp(state->valuestring, "start") == 0) {
                ESP_LOGW(TAG, "--------------------GET START----------------------");
                // 立即切换状态，避免音频数据在状态切换前被丢弃
                aborted_ = false;
                if (device_state_ == kDeviceStateIdle || device_state_ == kDeviceStateListening || device_state_ == kDeviceStateSpeaking) {
                    ESP_LOGI(TAG, "[TTS-START] Immediately switching to speaking state to avoid packet drops");
                    SetDeviceState(kDeviceStateSpeaking);
                }
            } else if (strcmp(state->valuestring, "stop") == 0) {
                ESP_LOGW(TAG, "--------------------GET STOP----------------------");
                Schedule([this]() {
                    background_task_->WaitForCompletion();
                    // Always honor stop even if speaking flag was not set due to ordering
                    aborted_ = false; // clear abort flag to allow next round
                    if (listening_mode_ == kListeningModeManualStop) {
                        SetDeviceState(kDeviceStateIdle);
                    } else {
                        SetDeviceState(kDeviceStateListening);
                    }
                });
            } else if (strcmp(state->valuestring, "sentence_start") == 0) {
                auto text = cJSON_GetObjectItem(root, "text");
                if (cJSON_IsString(text)) {
                    ESP_LOGI(TAG, "<< %s", text->valuestring);
                    Schedule([this, display, message = std::string(text->valuestring)]() {
                        if (display != nullptr) {
                            display->SetChatMessage("assistant", message.c_str());
                        }
                    });
                }
            }
        } else if (strcmp(type->valuestring, "stt") == 0) {
            auto text = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(text)) {
                ESP_LOGI(TAG, ">> %s", text->valuestring);
                Schedule([this, display, message = std::string(text->valuestring)]() {
                    if (display != nullptr) {
                        display->SetChatMessage("user", message.c_str());
                    }
                });
            }
        } else if (strcmp(type->valuestring, "llm") == 0) {
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(emotion)) {
                Schedule([this, display, emotion_str = std::string(emotion->valuestring)]() {
                    if (display != nullptr) {
                        display->SetEmotion(emotion_str.c_str());
                    }
                });
            }
#if CONFIG_IOT_PROTOCOL_MCP
        } else if (strcmp(type->valuestring, "mcp") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            if (cJSON_IsObject(payload)) {
                McpServer::GetInstance().ParseMessage(payload);
            }
#endif
#if CONFIG_IOT_PROTOCOL_XIAOZHI
        } else if (strcmp(type->valuestring, "iot") == 0) {
            auto commands = cJSON_GetObjectItem(root, "commands");
            if (cJSON_IsArray(commands)) {
                auto& thing_manager = iot::ThingManager::GetInstance();
                for (int i = 0; i < cJSON_GetArraySize(commands); ++i) {
                    auto command = cJSON_GetArrayItem(commands, i);
                    thing_manager.Invoke(command);
                }
            }
#endif
        } else if (strcmp(type->valuestring, "system") == 0) {
            auto command = cJSON_GetObjectItem(root, "command");
            if (cJSON_IsString(command)) {
                ESP_LOGI(TAG, "System command: %s", command->valuestring);
                if (strcmp(command->valuestring, "reboot") == 0) {
                    // Do a reboot if user requests a OTA update
                    Schedule([this]() {
                        Reboot();
                    });
                } else {
                    ESP_LOGW(TAG, "Unknown system command: %s", command->valuestring);
                }
            }
        } else if (strcmp(type->valuestring, "alert") == 0) {
            auto status = cJSON_GetObjectItem(root, "status");
            auto message = cJSON_GetObjectItem(root, "message");
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(status) && cJSON_IsString(message) && cJSON_IsString(emotion)) {
                Alert(status->valuestring, message->valuestring, emotion->valuestring, Lang::Sounds::P3_VIBRATION);
            } else {
                ESP_LOGW(TAG, "Alert command requires status, message and emotion");
            }
        } else {
            ESP_LOGW(TAG, "Unknown message type: %s", type->valuestring);
        }
    });
    bool protocol_started = protocol_->Start();

    audio_debugger_ = std::make_unique<AudioDebugger>();
    audio_processor_->Initialize(codec);
    audio_processor_->OnOutput([this](std::vector<int16_t>&& data) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (audio_send_queue_.size() >= MAX_AUDIO_PACKETS_IN_QUEUE) {
                ESP_LOGW(TAG, "Too many audio packets in queue, drop the newest packet");
                return;
            }
        }
        background_task_->Schedule([this, data = std::move(data)]() mutable {
            opus_encoder_->Encode(std::move(data), [this](std::vector<uint8_t>&& opus) {
                AudioStreamPacket packet;
                packet.payload = std::move(opus);
#ifdef CONFIG_USE_SERVER_AEC
                {
                    std::lock_guard<std::mutex> lock(timestamp_mutex_);
                    if (!timestamp_queue_.empty()) {
                        packet.timestamp = timestamp_queue_.front();
                        timestamp_queue_.pop_front();
                    } else {
                        packet.timestamp = 0;
                    }

                    if (timestamp_queue_.size() > 3) { // 限制队列长度3
                        timestamp_queue_.pop_front(); // 该包发送前先出队保持队列长度
                        return;
                    }
                }
#endif
                std::lock_guard<std::mutex> lock(mutex_);
                if (audio_send_queue_.size() >= MAX_AUDIO_PACKETS_IN_QUEUE) {
                    ESP_LOGW(TAG, "Too many audio packets in queue, drop the oldest packet");
                    audio_send_queue_.pop_front();
                }
                audio_send_queue_.emplace_back(std::move(packet));
                xEventGroupSetBits(event_group_, SEND_AUDIO_EVENT);
            });
        });
    });
    audio_processor_->OnVadStateChange([this](bool speaking) {
        if (device_state_ == kDeviceStateListening) {
            Schedule([this, speaking]() {
                if (speaking) {
                    voice_detected_ = true;
                } else {
                    voice_detected_ = false;
                }
                auto led = Board::GetInstance().GetLed();
                led->OnStateChanged();
            });
        }
    });

    wake_word_->Initialize(codec);
    wake_word_->OnWakeWordDetected([this](const std::string& wake_word) {
        //ESP_LOGW(TAG, "=====================WAKE WORD======================");
        Schedule([this, &wake_word]() {
            if (!protocol_) {
                return;
            }

            if (device_state_ == kDeviceStateIdle) {
                wake_word_->EncodeWakeWordData();
                
                if (!protocol_->IsAudioChannelOpened()) {
                    SetDeviceState(kDeviceStateConnecting);
                    if (!protocol_->OpenAudioChannel()) {
                        wake_word_->StartDetection();
                        return;
                    }
                }

                ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
#if CONFIG_USE_AFE_WAKE_WORD
                AudioStreamPacket packet;
                // Encode and send the wake word data to the server
                while (wake_word_->GetWakeWordOpus(packet.payload)) {
                    protocol_->SendAudio(packet);
                }
                // Set the chat state to wake word detected
                protocol_->SendWakeWordDetected(wake_word);
#else
                // Play the pop up sound to indicate the wake word is detected
                // And wait 60ms to make sure the queue has been processed by audio task
                ResetDecoder();
                PlaySound(Lang::Sounds::P3_POPUP);
                vTaskDelay(pdMS_TO_TICKS(60));
#endif
                SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
            } else if (device_state_ == kDeviceStateSpeaking) {
                AbortSpeaking(kAbortReasonWakeWordDetected);
            } else if (device_state_ == kDeviceStateActivating) {
                SetDeviceState(kDeviceStateIdle);
            }
        });
    });
    wake_word_->StartDetection();

    // Wait for the new version check to finish
    xEventGroupWaitBits(event_group_, CHECK_NEW_VERSION_DONE_EVENT, pdTRUE, pdFALSE, portMAX_DELAY);
    SetDeviceState(kDeviceStateIdle);

    has_server_time_ = ota_.HasServerTime();
    if (protocol_started) {
        std::string message = std::string(Lang::Strings::VERSION) + ota_.GetCurrentVersion();
        if (display != nullptr) {
            display->ShowNotification(message.c_str());
            display->SetChatMessage("system", "");
        }
        // Play the success sound to indicate the device is ready
        ResetDecoder();
        PlaySound(Lang::Sounds::P3_SUCCESS);
    }

    // Print heap stats
    SystemInfo::PrintHeapStats();

    // Enter the main event loop
    MainEventLoop();
}

void Application::OnClockTimer() {
    clock_ticks_++;

    auto display = Board::GetInstance().GetDisplay();
    if (display != nullptr) {
        display->UpdateStatusBar();
    }

    // Print the debug info every 10 seconds
    if (clock_ticks_ % 10 == 0) {
        // SystemInfo::PrintTaskCpuUsage(pdMS_TO_TICKS(1000));
        // SystemInfo::PrintTaskList();
        SystemInfo::PrintHeapStats();

        // If we have synchronized server time, set the status to clock "HH:MM" if the device is idle
        if (has_server_time_) {
            if (device_state_ == kDeviceStateIdle) {
                Schedule([this]() {
                    // Set status to clock "HH:MM"
                    time_t now = time(NULL);
                    char time_str[64];
                    strftime(time_str, sizeof(time_str), "%H:%M  ", localtime(&now));
                    Board::GetInstance().GetDisplay()->SetStatus(time_str);
                });
            }
        }
    }
}

// Add a async task to MainLoop
void Application::Schedule(std::function<void()> callback) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        main_tasks_.push_back(std::move(callback));
    }
    xEventGroupSetBits(event_group_, SCHEDULE_EVENT);
}

// The Main Event Loop controls the chat state and websocket connection
// If other tasks need to access the websocket or chat state,
// they should use Schedule to call this function
void Application::MainEventLoop() {
    // Raise the priority of the main event loop to avoid being interrupted by background tasks (which has priority 2)
    vTaskPrioritySet(NULL, 3);

    while (true) {
        auto bits = xEventGroupWaitBits(event_group_, SCHEDULE_EVENT | SEND_AUDIO_EVENT, pdTRUE, pdFALSE, portMAX_DELAY);

        if (bits & SEND_AUDIO_EVENT) {
            std::unique_lock<std::mutex> lock(mutex_);
            auto packets = std::move(audio_send_queue_);
            lock.unlock();
            for (auto& packet : packets) {
                if (!protocol_->SendAudio(packet)) {
                    break;
                }
            }
        }

        if (bits & SCHEDULE_EVENT) {
            std::unique_lock<std::mutex> lock(mutex_);
            auto tasks = std::move(main_tasks_);
            lock.unlock();
            for (auto& task : tasks) {
                task();
            }
        }
    }
}

// The Audio Loop is used to input and output audio data
void Application::AudioLoop() {
    //ESP_LOGW(TAG, "=====================  AudioLoop  ======================");
    auto codec = Board::GetInstance().GetAudioCodec();
    while (true) {
        OnAudioInput();
        if (codec->output_enabled()) {
            OnAudioOutput();
        }
    }
}

void Application::OnAudioOutput() {
    // 修复：检查并发解码任务数，但允许一定的队列积压处理
    int current_tasks = active_decode_tasks_.load();
    if (current_tasks >= MAX_CONCURRENT_DECODE_TASKS) {
        // ESP_LOGW(TAG, "[AUDIO-OUT] ⏸️ Max concurrent tasks reached 🔧[%d/%d], skipping - QUEUE_SIZE=%u",
        //          current_tasks, MAX_CONCURRENT_DECODE_TASKS, (unsigned)audio_decode_queue_.size());
        return;
    }

    auto now = std::chrono::steady_clock::now();
    auto codec = Board::GetInstance().GetAudioCodec();
    const int max_silence_seconds = 10;

    std::unique_lock<std::mutex> lock(mutex_);
    if (audio_decode_queue_.empty()) {
        // Disable the output if there is no audio data for a long time
        if (device_state_ == kDeviceStateIdle) {
            auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - last_output_time_).count();
            if (duration > max_silence_seconds) {
                codec->EnableOutput(false);
            }
        }
        return;
    }

    // 优化：直接处理原始音频数据，避免packet解包开销
    auto raw_data = std::move(audio_decode_queue_.front());
    audio_decode_queue_.pop_front();
    size_t remaining_queue_size = audio_decode_queue_.size();
    lock.unlock();
    audio_decode_cv_.notify_all();

    // ESP_LOGI(TAG, "[AUDIO-OUT] 🎵 Processing packet: size=%u bytes, 📦REMAINING=[%u], 🔧TASKS=%d",
    //          (unsigned)raw_data.size(), (unsigned)remaining_queue_size, active_decode_tasks_.load());

    auto decode_start_time = std::chrono::steady_clock::now();
    // ESP_LOGI(TAG, "[AUDIO-OUT] 🚀 Starting decode task, 📦QUEUE=[%u]",
    //          (unsigned)remaining_queue_size);

    background_task_->Schedule([this, codec, raw_data = std::move(raw_data), decode_start_time]() mutable {
        auto decode_task_start = std::chrono::steady_clock::now();
        auto schedule_delay_ms = std::chrono::duration_cast<std::chrono::milliseconds>(decode_task_start - decode_start_time).count();

        // 修复：在任务真正开始执行时才管理计数器
        active_decode_tasks_.fetch_add(1);
        int current_tasks = active_decode_tasks_.load();
        if (aborted_) {
            active_decode_tasks_.fetch_sub(1);
            ESP_LOGW(TAG, "[AUDIO-OUT] Decode task aborted, remaining tasks: %d", current_tasks - 1);
            return;
        }

        auto opus_decode_start = std::chrono::steady_clock::now();
        std::vector<int16_t> pcm;
        // 直接解码原始数据，固定参数：16000Hz, 60ms, 1通道
        if (!opus_decoder_->Decode(std::move(raw_data), pcm)) {
            ESP_LOGE(TAG, "[AUDIO-OUT] OPUS decode failed");
            active_decode_tasks_.fetch_sub(1);
            return;
        }
        auto opus_decode_end = std::chrono::steady_clock::now();
        auto opus_decode_ms = std::chrono::duration_cast<std::chrono::milliseconds>(opus_decode_end - opus_decode_start).count();

        // Resample if the sample rate is different
        auto resample_start = std::chrono::steady_clock::now();
        if (opus_decoder_->sample_rate() != codec->output_sample_rate()) {
            int target_size = output_resampler_.GetOutputSamples(pcm.size());
            std::vector<int16_t> resampled(target_size);
            output_resampler_.Process(pcm.data(), pcm.size(), resampled.data());
            pcm = std::move(resampled);
        }
        auto resample_end = std::chrono::steady_clock::now();
        auto resample_ms = std::chrono::duration_cast<std::chrono::milliseconds>(resample_end - resample_start).count();

        auto output_start = std::chrono::steady_clock::now();
        codec->OutputData(pcm);
        auto output_end = std::chrono::steady_clock::now();
        auto output_ms = std::chrono::duration_cast<std::chrono::milliseconds>(output_end - output_start).count();

        auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(output_end - decode_start_time).count();

        // 任务完成，减少计数器
        int remaining_tasks = active_decode_tasks_.fetch_sub(1) - 1;
        // ESP_LOGI(TAG, "[AUDIO-OUT] ✅ Decode complete: schedule_delay=%dms, opus=%dms, resample=%dms, output=%dms, total=%dms, pcm_samples=%u, 🔧REMAINING_TASKS=[%d]",
        //          (int)schedule_delay_ms, (int)opus_decode_ms, (int)resample_ms, (int)output_ms, (int)total_ms, (unsigned)pcm.size(), remaining_tasks);

#ifdef CONFIG_USE_SERVER_AEC
        // 原始数据没有时间戳，使用当前时间
        std::lock_guard<std::mutex> lock(timestamp_mutex_);
        timestamp_queue_.push_back(0);
#endif
        last_output_time_ = std::chrono::steady_clock::now();
    });
}

void Application::OnAudioInput() {
    //ESP_LOGW(TAG, "=====================  OnAudioInput  ======================");
    
    if (device_state_ == kDeviceStateAudioTesting) {
        if (audio_testing_queue_.size() >= AUDIO_TESTING_MAX_DURATION_MS / OPUS_FRAME_DURATION_MS) {
            ExitAudioTestingMode();
            return;
        }
        std::vector<int16_t> data;
        int samples = OPUS_FRAME_DURATION_MS * 16000 / 1000;
        if (ReadAudio(data, 16000, samples)) {
            background_task_->Schedule([this, data = std::move(data)]() mutable {
                opus_encoder_->Encode(std::move(data), [this](std::vector<uint8_t>&& opus) {
                    AudioStreamPacket packet;
                    packet.payload = std::move(opus);
                    packet.frame_duration = OPUS_FRAME_DURATION_MS;
                    packet.sample_rate = 16000;
                    std::lock_guard<std::mutex> lock(mutex_);
                    audio_testing_queue_.push_back(std::move(packet));
                });
            });
            return;
        }
    }

    if (wake_word_->IsDetectionRunning()) {
        std::vector<int16_t> data;
        int mono_samples = wake_word_->GetFeedSize();
        if (mono_samples > 0) {
            // Ensure input is enabled in case some board/power path disabled it
            auto codec = Board::GetInstance().GetAudioCodec();
            if (!codec->input_enabled()) {
                codec->EnableInput(true);
            }

            int input_channels = codec->input_channels();
            int capture_samples = mono_samples * (input_channels > 1 ? input_channels : 1);
            if (ReadAudio(data, 16000, capture_samples)) {
                if (input_channels > 1) {
                    // Down-mix to mono: pick MIC channel from interleaved data
                    std::vector<int16_t> mono;
                    mono.reserve(mono_samples);
                    for (int i = 0; i < mono_samples; ++i) {
                        mono.push_back(data[i * input_channels]);
                    }
                    wake_word_->Feed(mono);
                } else {
                    wake_word_->Feed(data);
                }
                return;
            }
        }
    }

    if (audio_processor_->IsRunning()) {
        std::vector<int16_t> data;
        int samples = audio_processor_->GetFeedSize();
        if (samples > 0) {
            if (ReadAudio(data, 16000, samples)) {
                audio_processor_->Feed(data);
                return;
            }
        }
    }

    vTaskDelay(pdMS_TO_TICKS(OPUS_FRAME_DURATION_MS / 2));
}

bool Application::ReadAudio(std::vector<int16_t>& data, int sample_rate, int samples) {
    auto codec = Board::GetInstance().GetAudioCodec();
    if (!codec->input_enabled()) {
        return false;
    }

    if (codec->input_sample_rate() != sample_rate) {
        data.resize(samples * codec->input_sample_rate() / sample_rate);
        if (!codec->InputData(data)) {
            return false;
        }
        if (codec->input_channels() == 2) {
            auto mic_channel = std::vector<int16_t>(data.size() / 2);
            auto reference_channel = std::vector<int16_t>(data.size() / 2);
            for (size_t i = 0, j = 0; i < mic_channel.size(); ++i, j += 2) {
                mic_channel[i] = data[j];
                reference_channel[i] = data[j + 1];
            }
            auto resampled_mic = std::vector<int16_t>(input_resampler_.GetOutputSamples(mic_channel.size()));
            auto resampled_reference = std::vector<int16_t>(reference_resampler_.GetOutputSamples(reference_channel.size()));
            input_resampler_.Process(mic_channel.data(), mic_channel.size(), resampled_mic.data());
            reference_resampler_.Process(reference_channel.data(), reference_channel.size(), resampled_reference.data());
            data.resize(resampled_mic.size() + resampled_reference.size());
            for (size_t i = 0, j = 0; i < resampled_mic.size(); ++i, j += 2) {
                data[j] = resampled_mic[i];
                data[j + 1] = resampled_reference[i];
            }
        } else {
            auto resampled = std::vector<int16_t>(input_resampler_.GetOutputSamples(data.size()));
            input_resampler_.Process(data.data(), data.size(), resampled.data());
            data = std::move(resampled);
        }
    } else {
        data.resize(samples);
        if (!codec->InputData(data)) {
            return false;
        }
    }
    
    // 音频调试：发送原始音频数据
    if (audio_debugger_) {
        audio_debugger_->Feed(data);
    }
    
    return true;
}

void Application::AbortSpeaking(AbortReason reason) {
    ESP_LOGW(TAG, "=========================Abort speaking=========================");
    aborted_ = true;
    protocol_->SendAbortSpeaking(reason);
    // Immediately stop playback and clear queues; switch state out of speaking
    ResetDecoder();
    if (listening_mode_ == kListeningModeManualStop) {
        SetDeviceState(kDeviceStateIdle);
    } else {
        SetDeviceState(kDeviceStateListening);
    }
}

void Application::SetListeningMode(ListeningMode mode) {
    listening_mode_ = mode;
    SetDeviceState(kDeviceStateListening);
}

void Application::SetDeviceState(DeviceState state) {
    if (device_state_ == state) {
        return;
    }

    clock_ticks_ = 0;
    auto previous_state = device_state_;
    device_state_ = state;
    ESP_LOGI(TAG, "STATE CHANGE: %s -> %s", STATE_STRINGS[previous_state], STATE_STRINGS[device_state_]);
    // The state is changed, wait for all background tasks to finish
    background_task_->WaitForCompletion();

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto led = board.GetLed();
    led->OnStateChanged();
    switch (state) {
        case kDeviceStateUnknown:
        case kDeviceStateIdle:
        ESP_LOGW(TAG, "=====================  Idle  ======================");
            //led->SetColor(255, 255, 0); // 黄灯
            if (display != nullptr) {
                display->SetStatus(Lang::Strings::STANDBY);
                display->SetEmotion("neutral");
            }
            audio_processor_->Stop();
            ESP_LOGW(TAG, "==------ audio_processor_->Stop  -----====");
            wake_word_->StartDetection();
            ESP_LOGW(TAG, "====----- wake_word_->StartDetection -----=====");

            break;
        case kDeviceStateConnecting:
            if (display != nullptr) {
                display->SetStatus(Lang::Strings::CONNECTING);
                display->SetEmotion("neutral");
                display->SetChatMessage("system", "");
            }
            timestamp_queue_.clear();
            break;
        case kDeviceStateListening:
        ESP_LOGW(TAG, "=====================  Listening  ======================");
            //led->SetColor(255, 0, 0); // 红灯
            if (display != nullptr) {
                display->SetStatus(Lang::Strings::LISTENING);
                display->SetEmotion("neutral");
            }
            
            // Update the IoT states before sending the start listening command
#if CONFIG_IOT_PROTOCOL_XIAOZHI
            UpdateIotStates();
#endif

            // Make sure the audio processor is running
            if (!audio_processor_->IsRunning()) {
                // Send the start listening command
                //protocol_->SendStartListening(listening_mode_);
                if (previous_state == kDeviceStateSpeaking) {
                    audio_decode_queue_.clear();
                    audio_decode_cv_.notify_all();
                    // FIXME: Wait for the speaker to empty the buffer
                    vTaskDelay(pdMS_TO_TICKS(120));
                }
                opus_encoder_->ResetState();
                audio_processor_->Start();
                wake_word_->StopDetection();
            }
            break;
        case kDeviceStateSpeaking:
        ESP_LOGW(TAG, "=====================  Speaking  ======================");
            //led->SetColor(0, 255, 0); // 绿灯
            if (display != nullptr) {
                display->SetStatus(Lang::Strings::SPEAKING);
            }

            if (listening_mode_ != kListeningModeRealtime) {
                audio_processor_->Stop();
                // 启用唤醒词检测（AFE和ESP都支持在播放时唤醒）
#if CONFIG_USE_AFE_WAKE_WORD || CONFIG_USE_ESP_WAKE_WORD
                wake_word_->StartDetection();
#else
                wake_word_->StopDetection();
#endif
            }
            ResetDecoder();
            break;
        default:
            // Do nothing
            break;
    }
}

void Application::ResetDecoder() {
    std::lock_guard<std::mutex> lock(mutex_);
    opus_decoder_->ResetState();
    // 优化：清理原始音频数据队列
    size_t cleared_packets = audio_decode_queue_.size();
    audio_decode_queue_.clear();
    audio_decode_cv_.notify_all();
    last_output_time_ = std::chrono::steady_clock::now();
    auto codec = Board::GetInstance().GetAudioCodec();
    codec->EnableOutput(true);

    ESP_LOGI(TAG, "[AUDIO-RESET] 🔄 Decoder reset, 📦CLEARED=[%u] packets, output enabled", (unsigned)cleared_packets);
}

void Application::SetDecodeSampleRate(int sample_rate, int frame_duration) {
    if (opus_decoder_->sample_rate() == sample_rate && opus_decoder_->duration_ms() == frame_duration) {
        return;
    }

    opus_decoder_.reset();
    opus_decoder_ = std::make_unique<OpusDecoderWrapper>(sample_rate, 1, frame_duration);

    auto codec = Board::GetInstance().GetAudioCodec();
    if (opus_decoder_->sample_rate() != codec->output_sample_rate()) {
        ESP_LOGI(TAG, "Resampling audio from %d to %d", opus_decoder_->sample_rate(), codec->output_sample_rate());
        output_resampler_.Configure(opus_decoder_->sample_rate(), codec->output_sample_rate());
    }
}

void Application::UpdateIotStates() {
#if CONFIG_IOT_PROTOCOL_XIAOZHI
    auto& thing_manager = iot::ThingManager::GetInstance();
    std::string states;
    if (thing_manager.GetStatesJson(states, true)) {
        protocol_->SendIotStates(states);
    }
#endif
}

void Application::Reboot() {
    ESP_LOGI(TAG, "Rebooting...");
    esp_restart();
}

void Application::WakeWordInvoke(const std::string& wake_word) {
    if (device_state_ == kDeviceStateIdle) {
        ToggleChatState();
        Schedule([this, wake_word]() {
            if (protocol_) {
                protocol_->SendWakeWordDetected(wake_word); 
            }
        }); 
    } else if (device_state_ == kDeviceStateSpeaking) {
        Schedule([this]() {
            AbortSpeaking(kAbortReasonNone);
        });
    } else if (device_state_ == kDeviceStateListening) {   
        Schedule([this]() {
            if (protocol_) {
                protocol_->CloseAudioChannel();
            }
        });
    }
}

bool Application::CanEnterSleepMode() {
    if (device_state_ != kDeviceStateIdle) {
        return false;
    }

    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        return false;
    }

    // Now it is safe to enter sleep mode
    return true;
}

void Application::SendMcpMessage(const std::string& payload) {
    Schedule([this, payload]() {
        if (protocol_) {
            protocol_->SendMcpMessage(payload);
        }
    });
}

void Application::SetAecMode(AecMode mode) {
    aec_mode_ = mode;
    Schedule([this]() {
        auto& board = Board::GetInstance();
        auto display = board.GetDisplay();
        switch (aec_mode_) {
        case kAecOff:
            audio_processor_->EnableDeviceAec(false);
            if (display != nullptr) {
                display->ShowNotification(Lang::Strings::RTC_MODE_OFF);
            }
            break;
        case kAecOnServerSide:
            audio_processor_->EnableDeviceAec(false);
            if (display != nullptr) {
                display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            }
            break;
        case kAecOnDeviceSide:
            audio_processor_->EnableDeviceAec(true);
            if (display != nullptr) {
                display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            }
            break;
        }

        // If the AEC mode is changed, close the audio channel
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
    });
}


