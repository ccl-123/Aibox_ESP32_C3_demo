#ifndef BACKGROUND_TASK_H
#define BACKGROUND_TASK_H

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mutex>
#include <list>
#include <vector>
#include <condition_variable>
#include <atomic>

class BackgroundTask {
public:
    BackgroundTask(uint32_t stack_size = 4096 * 2, int thread_count = 3, int priority = 5);
    ~BackgroundTask();

    void Schedule(std::function<void()> callback);
    void WaitForCompletion();

private:
    std::mutex mutex_;
    std::list<std::function<void()>> background_tasks_;
    std::condition_variable condition_variable_;
    std::vector<TaskHandle_t> background_task_handles_;
    std::atomic<size_t> active_tasks_{0};
    std::atomic<bool> stop_flag_{false};
    int thread_count_;

    void BackgroundTaskLoop(int worker_id);
};

#endif
