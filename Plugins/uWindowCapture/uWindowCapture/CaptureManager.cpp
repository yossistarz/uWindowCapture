#include <algorithm>
#include "CaptureManager.h"
#include "WindowManager.h"
#include "Window.h"
#include "WindowTexture.h"
#include "Debug.h"
#include "Util.h"

using namespace Microsoft::WRL;



namespace
{
    constexpr auto kMaintenanceInterval = std::chrono::milliseconds(250);
    constexpr int kBatchLimit = 32;
}


// ---


CaptureManager::CaptureManager()
{
    windowCaptureThreadLoop_.SetFinalizer([]
    {
        if (const auto& wgcManager = WindowManager::GetWindowsGraphicsCaptureManager())
        {
            wgcManager->StopAllInstances();
        }
    });

    windowCaptureThreadLoop_.Start([this] 
    {
        for (int processed = 0; processed < kBatchLimit; ++processed)
        {
            const int id = DequeueNextWindow();
            if (id < 0) break;
            if (auto window = WindowManager::Get().GetWindow(id))
            {
                window->windowTexture_->KeepCaptureAlive();
                window->Capture();
            }
        }

        if (const auto& wgcManager = WindowManager::GetWindowsGraphicsCaptureManager())
        {
            wgcManager->UpdateFromCaptureThread();
        }
        if (!highPriorityQueue_.Empty() || !middlePriorityQueue_.Empty() || !lowPriorityQueue_.Empty())
            windowCaptureThreadLoop_.Wake();
    }, kMaintenanceInterval, ThreadLoop::WaitMode::OnRequest);

    iconCaptureThreadLoop_.Start([this] 
    {
        for (int processed = 0; processed < kBatchLimit; ++processed)
        {
            const int id = iconQueue_.Dequeue();
            if (id < 0) break;
            if (auto window = WindowManager::Get().GetWindow(id))
            {
                window->CaptureIcon();
            }
        }
        if (!iconQueue_.Empty()) iconCaptureThreadLoop_.Wake();
    }, ThreadLoop::microseconds::zero(), ThreadLoop::WaitMode::OnRequest);
}


CaptureManager::~CaptureManager()
{
    iconCaptureThreadLoop_.Stop();
    windowCaptureThreadLoop_.Stop();
}


void CaptureManager::RequestCapture(int id, CapturePriority priority)
{
    if (auto window = WindowManager::Get().GetWindow(id))
    {
        ++window->captureRequestCount_;
        int current = window->pendingCapturePriority_;
        const int requested = static_cast<int>(priority);
        while ((current < 0 || requested < current) &&
            !window->pendingCapturePriority_.compare_exchange_weak(current, requested)) {}
        QueueCapture(id, priority);
    }
}


int CaptureManager::DequeueNextWindow()
{
    WindowQueue* priorities[] = { &highPriorityQueue_, &highPriorityQueue_, &highPriorityQueue_,
        &highPriorityQueue_, &middlePriorityQueue_, &middlePriorityQueue_, &lowPriorityQueue_ };
    for (unsigned checked = 0; checked < std::size(priorities); ++checked)
    {
        auto queue = priorities[nextPriority_];
        nextPriority_ = (nextPriority_ + 1) % std::size(priorities);
        const int id = queue->Dequeue();
        if (id >= 0) return id;
    }
    return -1;
}


void CaptureManager::QueueCapture(int id, CapturePriority priority)
{
    switch (priority)
    {
        case CapturePriority::High:
        {
            highPriorityQueue_.Enqueue(id);
            break;
        }
        case CapturePriority::Middle:
        {
            middlePriorityQueue_.Enqueue(id);
            break;
        }
        case CapturePriority::Low:
        {
            lowPriorityQueue_.Enqueue(id);
            break;
        }
    }
    windowCaptureThreadLoop_.Wake();
}


void CaptureManager::RequestCaptureIcon(int id)
{
    iconQueue_.Enqueue(id);
    iconCaptureThreadLoop_.Wake();
}