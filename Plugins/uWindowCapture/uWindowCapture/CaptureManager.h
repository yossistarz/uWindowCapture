#pragma once

#include <Windows.h>
#include <deque>
#include <mutex>

#include "WindowQueue.h"
#include "Thread.h"


enum class CapturePriority
{
    High = 0,
    Middle = 1,
    Low  = 2,
};


class CaptureManager
{
friend class Window;
#ifdef UWC_NATIVE_TESTS
friend struct CaptureThroughputTests;
#endif
public:
    CaptureManager();
    ~CaptureManager();
    void RequestCapture(int id, CapturePriority priority);
    void RequestCaptureIcon(int id);

private:
    void QueueCapture(int id, CapturePriority priority);
    int DequeueNextWindow();
    unsigned nextPriority_ = 0;
    ThreadLoop windowCaptureThreadLoop_ = { L"uWindowCapture - Window Capture Thread" };
    ThreadLoop iconCaptureThreadLoop_ = { L"uWindowCapture - Icon Capture Thread" };
    WindowQueue highPriorityQueue_;
    WindowQueue middlePriorityQueue_;
    WindowQueue lowPriorityQueue_;
    WindowQueue iconQueue_;
};