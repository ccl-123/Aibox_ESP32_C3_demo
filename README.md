## 项目概述

Aibox_ESP32_C3_NEW 是一个基于 ESP32-C3 的智能音频设备项目，面向低功耗、低内存场景，提供稳定的语音交互能力。项目特性包括：
- 语音交互：语音上行、TTS 下行播放
- 实时播放：解码与播放任务分离，保障解码性能
- 播放水位控制：背压策略防止内存溢出
- OTA 升级：基于 ESP-IDF OTA，支持在线升级
- 协议通信：MQTT（默认）；WebSocket 代码保留但默认不启用

目标硬件：立创 ESP32-C3 开发板（lichuang c3 dev）。

---

## 系统架构

### 模块关系与数据流（Mermaid）

```mermaid
flowchart LR
    MIC[Mic/Codec In] -->|PCM 16kHz| AP[AudioProcessor/WW]
    AP --> AIIN[Application::OnAudioInput]
    AIIN -->|Opus Encode/Send| PROTO[Protocol(MQTT)]
    PROTO -->|Downlink: Opus Frames/JSON| AIOUT[Application::OnAudioOutput]
    AIOUT -->|Opus Decode| DEC[OpusDecoder]
    DEC -->|PCM| PLAYQ[[Playback Queue (PCM)]]
    PLAYQ -->|I2S Write| I2S[I2S/Codec Out]

    subgraph Application
      AIOUT
      AIIN
      DEC
      PLAYQ
    end

    subgraph Protocol
      PROTO
    end

    subgraph Board/HAL
      I2S
    end

    OTA[OTA Manager] -. control .-> Application
```

说明：
- 上行：麦克风 PCM 经过（可选）本地预处理后编码并通过协议发送
- 下行：协议接收 Opus 帧 → 解码为 PCM → 写入播放队列 → 独立播放任务输出到 I2S
- 背压：当播放队列达到阈值，暂停新的解码任务，让积压留在小内存的 Opus 队列

---

## 核心模块详解

### Application 模块（main/application.cc, .h）
- 职责：设备状态机（starting/listening/speaking/…）、主事件循环、音频上/下行协调
- 解耦设计：
  - OnAudioOutput 只负责“调度解码任务 + 入播放队列”，不阻塞 I2S 输出
  - 独立播放任务消费播放队列，持续写 I2S，避免阻塞解码
- 背压控制（默认配置）：
  - 播放队列上限 MAX_PLAYBACK_TASKS_IN_QUEUE=3
  - 高水位=2（暂停新的解码调度），低水位=1（恢复解码）
  - 背压状态日志：[BACKPRESSURE] ENTER/EXIT，便于观察水位切换
- STOP 行为：收到 STOP 后等待“解码完成 + 播放队列清空（无超时）”，再切状态，避免突然截断

### Protocol 模块（main/protocols/*）
- 抽象接口：protocol.h
  - SendAudio/SendStartListening/SendStopListening/SendAbortSpeaking/OnIncomingAudio/OnIncomingJson
  - 虚接口 SendCancelTTS（默认空实现），MQTT 实现用于通知 TTS finish/stop
- MQTT 实现：mqtt_protocol.{h,cc}
  - 音频与控制消息的收发、Server VAD 处理
  - 项目默认仅使用 MQTT；已在 Application 中通过静态转换调用 SendCancelTTS
- WebSocket：保留代码，不作为本项目默认路径

### Audio 模块（Opus/Resampler/I2S/背压）
- 编解码：
  - OpusDecoderWrapper/OpusEncoderWrapper（managed_components/esp-opus）
  - 常用配置：16kHz, 单声道, 60ms 帧
- 重采样：根据板载 Codec 采样率与服务端配置，自适应重采样
- 播放：
  - 独立播放任务（audio_playback），任务栈 8192，优先级 6
  - I2S 写入耗时≈48–60ms/帧（16kHz/60ms），与理论一致
- 背压：
  - 当播放队列≥2：暂停新的解码调度（不丢帧）
  - 当播放队列≤1：恢复解码调度
  - 目的：让积压留在 Opus 队列（体积小），避免 PCM 队列撑爆内存

### Board 模块（main/board.*）
- 硬件抽象：音频 Codec、电源控制、网络启动等
- 目标平台：立创 C3 开发板（lichuang c3 dev）

### OTA 模块（main/ota.*）
- 使用 ESP-IDF OTA API
- 支持在线检查与升级；URL 由 CONFIG_OTA_URL 指定
- 典型配置：双分区升级、版本比较、错误处理、自动重启

### Display 模块
- 项目中已移除显示功能（可能有少量状态栏刷新的调用）

### Wake Word 模块（可选）
- AFE/ESP/None 三态选择：CONFIG_USE_AFE_WAKE_WORD / CONFIG_USE_ESP_WAKE_WORD / 默认禁用
- 用于语音唤醒与会话控制

### IoT 模块
- iot/thing_manager：设备状态上报与控制接口预留

---

## 关键技术特性

- 解码/播放任务彻底分离：避免 I2S 阻塞拖慢解码，解码耗时≈3–8ms/帧
- 播放队列背压：小队列（上限=3，高=2/低=1）+ 背压暂停解码，防止 ESP32-C3 OOM
- 服务端 VAD：支持基于下行端检测的直接 Speaking 切换
- 协议：MQTT 默认；提供 TTS 取消（finish/stop）
- STOP 优雅收尾：等待播放队列清空后再切状态

---

## 编译与部署

### 依赖
- ESP-IDF（建议 5.x）环境已就绪（`idf.py` 可用）
- Python 3，CMake/Ninja（IDF 自带）

### 配置
- 进入项目目录：
  ```bash
  cd Aibox_ESP32_C3_NEW
  ```
- 目标芯片：
  ```bash
  idf.py set-target esp32c3
  ```
- 可选配置（菜单）：
  ```bash
  idf.py menuconfig
  ```
  - OTA URL：`CONFIG_OTA_URL`
  - AEC/Wake Word/协议等开关按需要配置

### 构建/烧录/监视
```bash
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
# 退出监视：Ctrl + ]
```

---

## 故障排除（Troubleshooting）

### 1) 栈溢出 / Guru Meditation（SILK 编码）
- 症状：Stack protection fault，回溯出现在 `silk_encode_frame_FIX`
- 原因：Opus（SILK）编码栈需求大
- 解决：
  - 后台解码任务栈：`BackgroundTask(4096*7)`（约 28KB）
  - 播放线程栈：8192（I2S 写/日志叠加更稳）
  - 如仍有问题，可降低 Opus 复杂度、减少日志

### 2) 内存不足 / 突发堆积
- 症状：下行突发导致播放队列/OPUS 队列快速增长，可能触发复位
- 解决：
  - 播放队列保持很小（上限=3，高=2/低=1），启用背压
  - 入口限速（服务端按帧率发包或设备端令牌桶）
  - 监控 free/min（SystemInfo 已打印），必要时降低 OPUS 队列上限

### 3) 无声或突然截断
- 检查：
  - 是否把空 PCM 入队（已在代码中保护）
  - 收到 STOP 后是否等待播放队列清空（已改为无超时等待）

### 4) 日志乱码/串行中断
- 长/多字节 Emoji 日志在复位前可能出现乱码（打印被打断）
- 建议在压力测试中使用 ASCII 日志，降低串口干扰

### 5) 协议相关
- 仅用 MQTT：Application 中已直接调用 `MqttProtocol::SendCancelTTS`
- 若切换协议，建议在 Protocol 抽象层覆写 `SendCancelTTS`

---

## 日志参考
- 解码完成：
  - `[AUDIO-OUT] Decode complete: schedule_delay=1ms, opus=5ms, resample=0ms, enq_play=6ms, PLAY_Q=[2], REMAINING_TASKS=[0]`
- 播放输出：
  - `[AUDIO-PLAYBACK] output=59ms, queue=2`
- 背压切换：
  - 进入：`[BACKPRESSURE] ENTER backpressure: PLAY_Q=[2/3], HIGH=2, LOW=1`
  - 退出：`[BACKPRESSURE] EXIT backpressure: PLAY_Q=[1/3], HIGH=2, LOW=1`

---



