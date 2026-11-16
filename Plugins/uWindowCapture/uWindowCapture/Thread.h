#pragma once

#include <functional>
#include <string>
#include <chrono>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <mutex>


class ThreadLoop
{
public:
    using ThreadFunc = std::function<void()>;
    using microseconds = std::chrono::microseconds;
    enum class WaitMode { Periodic, OnRequest };

    ThreadLoop(const std::wstring& name);
    ~ThreadLoop();
    void Start(
        const ThreadFunc& func,
        const microseconds& interval = microseconds(1'000'000 / 60),
        WaitMode waitMode = WaitMode::Periodic);
    void Restart();
    void Stop();
    void Wake();
    void SetInitializer(const ThreadFunc& func) { initializerFunc_ = func; }
    void SetFinalizer(const ThreadFunc& func) { finalizerFunc_ = func; }
    bool IsRunning() const;
    bool HasFunction() const;

private:
    const std::wstring name_;
    std::thread thread_;
    std::atomic<bool> isRunning_ = false;
    microseconds interval_ = microseconds::zero();
    ThreadFunc loopFunc_ = nullptr;
    ThreadFunc finalizerFunc_ = nullptr;
    ThreadFunc initializerFunc_ = nullptr;
    WaitMode waitMode_ = WaitMode::Periodic;
    std::mutex wakeMutex_;
    std::condition_variable wakeCondition_;
    bool wakeRequested_ = false;
};
