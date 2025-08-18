#include "background_task.h"

#include <esp_log.h>
#include <esp_task_wdt.h>

#define TAG "BackgroundTask"

BackgroundTask::BackgroundTask(uint32_t stack_size, int thread_count, int priority)
    : thread_count_(thread_count) {
    background_task_handles_.resize(thread_count_);

    ESP_LOGI(TAG, "🔧 Creating %d BackgroundTask threads with priority %d", thread_count_, priority);

    for (int i = 0; i < thread_count_; i++) {
        xTaskCreate([](void* arg) {
            auto* params = static_cast<std::pair<BackgroundTask*, int>*>(arg);
            BackgroundTask* task = params->first;
            int worker_id = params->second;
            task->BackgroundTaskLoop(worker_id);
            delete params;
            vTaskDelete(NULL);
        }, ("bg_task_" + std::to_string(i)).c_str(), stack_size,
           new std::pair<BackgroundTask*, int>(this, i), priority, &background_task_handles_[i]);
    }
}

BackgroundTask::~BackgroundTask() {
    stop_flag_.store(true);
    condition_variable_.notify_all();

    for (auto handle : background_task_handles_) {
        if (handle != nullptr) {
            vTaskDelete(handle);
        }
    }
}

void BackgroundTask::Schedule(std::function<void()> callback) {
    std::unique_lock<std::mutex> lock(mutex_);

    // 🔴 流控机制：当任务堆积过多时，阻塞等待直到队列有空间
    if (active_tasks_ >= 70) {
        ESP_LOGW(TAG, "⏳ BackgroundTask queue FULL (%u tasks), waiting for space...", active_tasks_.load());
        condition_variable_.wait(lock, [this]() {
            return active_tasks_ < 70;
        });
        ESP_LOGI(TAG, "✅ BackgroundTask queue has space, resuming task creation");
    }

    if (active_tasks_ >= 30) {
        int free_sram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        if (free_sram < 10000) {
            ESP_LOGW(TAG, "active_tasks_ == %u, free_sram == %u", active_tasks_.load(), free_sram);
        }
    }

    active_tasks_++;
    background_tasks_.emplace_back([this, cb = std::move(callback)]() {
        cb();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            active_tasks_--;
            // 🔴 任务完成时通知等待的线程
            condition_variable_.notify_all();
        }
    });
    condition_variable_.notify_all();
}

void BackgroundTask::WaitForCompletion() {
    std::unique_lock<std::mutex> lock(mutex_);
    condition_variable_.wait(lock, [this]() {
        return background_tasks_.empty() && active_tasks_ == 0;
    });
}

void BackgroundTask::BackgroundTaskLoop(int worker_id) {
    ESP_LOGI(TAG, "🔧 BackgroundTask worker %d started, priority=%d", worker_id, uxTaskPriorityGet(NULL));

    while (!stop_flag_.load()) {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_variable_.wait(lock, [this]() {
            return !background_tasks_.empty() || stop_flag_.load();
        });

        if (stop_flag_.load()) {
            break;
        }

        if (background_tasks_.empty()) {
            continue;
        }

        // 每个工作线程取一个任务执行
        auto task = std::move(background_tasks_.front());
        background_tasks_.pop_front();
        lock.unlock();

        // 执行任务
        task();
    }

    ESP_LOGI(TAG, "🔧 BackgroundTask worker %d stopped", worker_id);
}
