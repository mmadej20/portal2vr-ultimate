#pragma once

#include <cstdint>
#include <utility>
#include "render_target_readiness.h"

inline bool CompleteVRTextureCreation(RenderTargetRetryState& retry,
                                     std::uint64_t nowMilliseconds, bool drainSucceeded)
{
    if (drainSucceeded) retry.RecordSuccess();
    else retry.RecordFailure(nowMilliseconds);
    return drainSucceeded;
}

template<typename Invalidate, typename ReleaseHeld, typename ReleaseMouse>
void SuspendVRInputAfterRenderFailure(Invalidate invalidate, ReleaseHeld releaseHeld, ReleaseMouse releaseMouse)
{
    invalidate();
    releaseHeld();
    releaseMouse();
}

struct VRTextureSubmissionReadiness
{
    bool handle = false;
    std::uint64_t image = 0;
    std::uint32_t width = 0, height = 0;
    bool device = false, physicalDevice = false, instance = false, queue = false;
    std::uint32_t samples = 0;
    bool Ready() const
    {
        return handle && image && width && height && device && physicalDevice && instance && queue && samples;
    }
};

template<typename Hide, typename Clear>
bool DetachVRMenuTexture(bool hideOverlay, Hide hide, Clear clear)
{
    if (hideOverlay) hide();
    return clear();
}

// Menu-refresh idea adapted from native-vr 0094269795aa29d2b0477af00e082b8b686aa68c.
// Scene transitions request refreshes. Allocation success and device resets
// remain independent, so a failed attempt in a menu still gets delayed retries.
class VRResourceLifecycle
{
public:
    bool ObserveGameplay(bool inGame)
    {
        const bool leavingGameplay = m_WasInGame && !inGame;
        m_WasInGame = inGame;
        return leavingGameplay;
    }
    void OnDeviceReset() { ++m_DeviceGeneration; }
    std::uint64_t DeviceGeneration() const { return m_DeviceGeneration; }

private:
    bool m_WasInGame = false;
    std::uint64_t m_DeviceGeneration = 0;
};

// Detachment is not a compositor fence. This ordering only establishes that
// producer-queue work has drained before the application's owners are dropped.
template<typename Detach, typename Drain, typename Release>
bool RetireVRResources(Detach detach, Drain drain, Release release)
{
    if (!detach()) return false;
    if (!drain()) return false;
    release();
    return true;
}

// Present rotates backbuffers before its existing device-idle wait. Keep at
// most the current and previous image until that wait completes.
template<typename Owner>
class CapturedImageRetention
{
public:
    bool Replace(Owner image)
    {
        if (m_Retired != nullptr) return false;
        m_Retired = std::move(m_Current);
        m_Current = std::move(image);
        return true;
    }
    void Drained() { m_Retired = nullptr; }
    void ClearAfterDrain() { m_Retired = nullptr; m_Current = nullptr; }
    const Owner& Current() const { return m_Current; }

private:
    Owner m_Current;
    Owner m_Retired;
};
