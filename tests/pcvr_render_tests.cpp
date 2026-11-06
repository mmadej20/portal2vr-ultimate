#include <iostream>
#include <memory>
#include <vector>
#include "../L4D2VR/render_target_readiness.h"

#include "../L4D2VR/vr_resource_lifecycle.h"
#include "../L4D2VR/render_condition_diagnostics.h"
#include "../L4D2VR/digital_input.h"
#include "../L4D2VR/ui_input.h"

int main()
{
    int failures = 0, checks = 0;
    const auto check = [&](bool passed, const char* name) {
        ++checks;
        if (!passed) { ++failures; std::cerr << "FAIL: " << name << '\n'; }
    };
    VRResourceLifecycle lifecycle;
    check(!lifecycle.ObserveGameplay(false), "startup menu uses initial unavailable resources without another refresh");
    for (int frame = 0; frame != 300; ++frame)
        check(!lifecycle.ObserveGameplay(false), "stable menu never requests repeated allocation");
    check(!lifecycle.ObserveGameplay(true), "entering gameplay retains ready targets");
    check(lifecycle.ObserveGameplay(false), "leaving gameplay requests one workshop compatibility refresh");
    check(!lifecycle.ObserveGameplay(false), "loading after menu transition does not re-request refresh");
    const auto initialGeneration = lifecycle.DeviceGeneration();
    lifecycle.OnDeviceReset();
    check(lifecycle.DeviceGeneration() == initialGeneration + 1, "reset creates an independent resource generation");
    check(!lifecycle.ObserveGameplay(false), "reset during stable menu does not manufacture another scene transition");
    lifecycle.ObserveGameplay(true);
    check(lifecycle.ObserveGameplay(false), "a second gameplay return requests a fresh compatibility refresh");

    RenderTargetRetryState retry;
    retry.RecordFailure(100);
    lifecycle.ObserveGameplay(false);
    check(!retry.CanAttempt(1099, true), "partial allocation preserves its delay across stable menu frames");
    check(retry.CanAttempt(1100, true), "partial allocation can recover after its delay");
    retry.RecordFailure(1100); retry.RecordFailure(2100);
    check(!retry.CanAttempt(10000, true), "stable menu cannot rearm exhausted allocation retries");
    lifecycle.OnDeviceReset(); retry.ResetForDevice();
    check(retry.CanAttempt(10000, true), "actual device reset rearms recovery even while menu stays open");
    check(!retry.CanAttempt(10000, false), "unavailable device cannot allocate after reset");
    RenderTargetRetryState finalDrainRetry;
    for (std::uint64_t attempt = 0; attempt < 3; ++attempt) {
        const auto now = attempt * 1000;
        check(finalDrainRetry.CanAttempt(now, true), "final-drain failure permits the next delayed attempt within budget");
        check(!CompleteVRTextureCreation(finalDrainRetry, now, false), "failed final drain cannot publish ready resources");
        check(!finalDrainRetry.CanAttempt(now + 999, true), "final-drain failure retains one-second backoff");
    }
    check(finalDrainRetry.Exhausted(), "three final-drain failures exhaust the allocation budget");
    check(!finalDrainRetry.CanAttempt(100000, true), "final-drain failures cannot repeatedly erase their retry history");
    finalDrainRetry.ResetForDevice();
    finalDrainRetry.RecordFailure(0);
    check(CompleteVRTextureCreation(finalDrainRetry, 1000, true), "successful final drain publishes ready resources");
    check(finalDrainRetry.CanAttempt(1000, true), "only completed creation resets previous failure history");

    RenderConditionDiagnostics diagnostics, anotherInstance;
    using C = RenderCondition;
    using D = RenderConditionChange;
    check(diagnostics.Observe(C::Bridge, true, {1}) == D::Failure, "first missing bridge is reported");
    check(diagnostics.Observe(C::Bridge, true, {1}) == D::None, "unchanged bridge failure is suppressed");
    check(diagnostics.Observe(C::BackBufferCapture, true, {2}) == D::Failure, "capture failure is independent of bridge failure");
    check(diagnostics.Observe(C::BackBufferImage, true, {0}) == D::Failure, "missing image does not suppress capture result");
    check(diagnostics.Observe(C::BackBufferDimensions, true, {0, 720}) == D::Failure, "dimensions are a separate condition");
    check(diagnostics.Observe(C::BackBufferCapture, true, {3}) == D::DetailsChanged, "changed HRESULT is reported without recovery first");
    check(diagnostics.Observe(C::BackBufferCapture, false, {0}) == D::Recovery, "capture success reports recovery");
    check(diagnostics.Observe(C::BackBufferCapture, true, {3}) == D::Failure, "same capture HRESULT recurs visibly after recovery");
    check(diagnostics.Observe(C::Bridge, false, {0}) == D::Recovery, "bridge recovery is reported");
    check(diagnostics.Observe(C::Bridge, true, {1}) == D::Failure, "bridge recurrence after recovery is reported");
    check(anotherInstance.Observe(C::Bridge, true, {1}) == D::Failure, "a second VR instance gets its own initial diagnostic");
    check(diagnostics.Observe(C::StereoSubmit, false, {0, 0}, true) == D::Ready, "initial successful submission is reported once");
    check(diagnostics.Observe(C::StereoSubmit, false, {0, 0}, true) == D::None, "stable successful submission never spams");
    check(diagnostics.Observe(C::StereoSubmit, true, {4, 0}, true) == D::Failure, "later submission error is not hidden by initial success");
    check(diagnostics.Observe(C::StereoSubmit, false, {0, 0}, true) == D::Recovery, "submission recovery is reported");
    check(diagnostics.Observe(C::StereoSubmit, true, {0, 7}, true) == D::Failure, "right-eye error is reported independently of healthy left-eye submit");
    check(diagnostics.Observe(C::StereoSubmit, true, {0, 8}, true) == D::DetailsChanged, "changed right-eye compositor error is reported");
    diagnostics.NewGeneration();
    check(diagnostics.Observe(C::Bridge, true, {1}) == D::Failure, "a device generation rearms first-failure diagnostics");
    diagnostics.SetVerbose(true, 100);
    check(diagnostics.Observe(C::Bridge, true, {1}) == D::DetailsChanged, "verbose diagnostics expose an unchanged failure");
    check(diagnostics.Observe(C::Bridge, false, {}) == D::Recovery, "verbose diagnostics retain recovery classification");
    check(diagnostics.Observe(C::Bridge, false, {}) == D::None, "verbose diagnostics do not repeat every frame");
    diagnostics.SetVerbose(true, 5100);
    check(diagnostics.Observe(C::Bridge, false, {}) == D::DetailsChanged, "verbose diagnostics expose periodic healthy observations");
    diagnostics.SetVerbose(true, 5101);
    check(diagnostics.Observe(C::Bridge, false, {9}) == D::DetailsChanged, "verbose changed healthy details are immediate within repeat interval");
    diagnostics.SetVerbose(false, 5200);
    check(diagnostics.Observe(C::Bridge, false, {}) == D::None, "disabling verbose diagnostics restores duplicate suppression");
    diagnostics.SetVerbose(true, 6000);
    diagnostics.NewGeneration();
    check(diagnostics.Observe(C::Bridge, true, {1}) == D::Failure, "new generation keeps first-failure classification in verbose mode");
    check(diagnostics.Observe(C::Bridge, true, {1}) == D::None, "new generation still suppresses immediate verbose duplicates");
    diagnostics.SetVerbose(true, 11000);
    check(diagnostics.Observe(C::Bridge, true, {1}) == D::DetailsChanged, "new generation preserves the configured verbose option");

    std::vector<int> events;
    check(RetireVRResources([&] { events.push_back(1); return true; }, [&] { events.push_back(2); return true; },
        [&] { events.push_back(3); }), "successful queue drain allows resource release");
    check(events == std::vector<int>({1, 2, 3}), "consumer detachment precedes drain and release");
    events.clear();
    check(!RetireVRResources([&] { events.push_back(1); return true; }, [&] { events.push_back(2); return false; },
        [&] { events.push_back(3); }), "failed drain blocks release");
    check(events == std::vector<int>({1, 2}), "failed drain preserves owners");
    events.clear();
    check(!RetireVRResources([&] { events.push_back(1); return false; }, [&] { events.push_back(2); return true; },
        [&] { events.push_back(3); }), "failed consumer detachment blocks retirement");
    check(events == std::vector<int>({1}), "failed detachment does not drain or release bound resources");

    CapturedImageRetention<std::shared_ptr<int>> images;
    auto first = std::make_shared<int>(1), second = std::make_shared<int>(2);
    std::weak_ptr<int> previous = first;
    check(images.Replace(first), "initial captured image is retained"); first.reset();
    check(images.Replace(second), "replacement preserves previous captured image");
    check(!previous.expired(), "capture cannot free previous image before producer drain");
    check(!images.Replace(std::make_shared<int>(3)), "undrained retired owner cannot be overwritten or grow a list");
    images.Drained();
    check(previous.expired(), "completed queue drain retires previous captured image");
    check(images.Current() == second, "queue drain retains the current captured image");
    images.ClearAfterDrain();
    check(!images.Current(), "explicit retirement clears current capture after drain");
    VRTextureSubmissionReadiness descriptor{true, 1, 1280, 720, true, true, true, true, 1};
    check(descriptor.Ready(), "complete texture descriptor is submit-ready");
    descriptor.handle = false;
    check(!descriptor.Ready(), "missing descriptor handle is never submitted"); descriptor.handle = true;
    descriptor.image = 0;
    check(!descriptor.Ready(), "missing image is never submitted"); descriptor.image = 1;
    descriptor.width = 0;
    check(!descriptor.Ready(), "zero width is never submitted"); descriptor.width = 1280;
    descriptor.height = 0;
    check(!descriptor.Ready(), "zero height is never submitted"); descriptor.height = 720;
    descriptor.device = false;
    check(!descriptor.Ready(), "missing device is never submitted"); descriptor.device = true;
    descriptor.physicalDevice = false;
    check(!descriptor.Ready(), "missing physical device is never submitted"); descriptor.physicalDevice = true;
    descriptor.instance = false;
    check(!descriptor.Ready(), "missing Vulkan instance is never submitted"); descriptor.instance = true;
    descriptor.queue = false;
    check(!descriptor.Ready(), "missing queue is never submitted"); descriptor.queue = true;
    descriptor.samples = 0;
    check(!descriptor.Ready(), "missing sample count is never submitted");
    bool visible = true, headingLocked = true, cursorCaptured = true;
    int clears = 0;
    const auto hide = [&] { visible = headingLocked = cursorCaptured = false; };
    const auto clear = [&] { ++clears; return true; };
    check(DetachVRMenuTexture(false, hide, clear), "capture rotation detaches the old texture");
    check(visible && headingLocked && cursorCaptured && clears == 1,
        "capture rotation retains menu visibility heading and cursor state");
    check(DetachVRMenuTexture(true, hide, clear), "actual invalidation detaches the texture");
    check(!visible && !headingLocked && !cursorCaptured && clears == 2,
        "actual invalidation hides the retired menu");
    DigitalButtonState heldAttack;
    UiInput::MenuPointerState heldMouse;
    heldAttack.HeldCommand(true, true, true, "+attack", "-attack");
    heldMouse.ConfirmSent(heldMouse.Press(), true);
    bool trackingValid = true, stereoFrame = true, hudFrame = true;
    int attackReleases = 0, mouseReleaseAttempts = 0;
    const auto invalidate = [&] { trackingValid = stereoFrame = hudFrame = false; };
    const auto releaseAttack = [&] {
        if (heldAttack.HeldCommand(false, false, false, "+attack", "-attack")) ++attackReleases;
    };
    const auto releaseMouse = [&] {
        const auto transition = heldMouse.LoseFocus();
        if (transition == UiInput::MouseTransition::Release) ++mouseReleaseAttempts;
        heldMouse.ConfirmSent(transition, false); // A failed SendInput must remain retryable.
    };
    SuspendVRInputAfterRenderFailure(invalidate, releaseAttack, releaseMouse);
    check(!trackingValid && !stereoFrame && !hudFrame, "failed render drain invalidates all published frame and tracking state");
    check(attackReleases == 1 && mouseReleaseAttempts == 1, "failed render drain releases held Source action and menu press");
    SuspendVRInputAfterRenderFailure(invalidate, releaseAttack, releaseMouse);
    check(attackReleases == 1, "repeated drain failure does not duplicate a balanced Source release");
    check(mouseReleaseAttempts == 2 && heldMouse.PendingRelease() == UiInput::MouseTransition::Release,
        "repeated drain failure retries a failed synthetic mouse release");
    heldMouse.ConfirmSent(heldMouse.PendingRelease(), true);
    check(heldMouse.PendingRelease() == UiInput::MouseTransition::None, "successful release clears the old menu press before recovery");
    check(heldAttack.HeldCommand(true, false, true, "+attack", "-attack") == nullptr,
        "recovered frame does not recreate a stale held action without a fresh edge");
    if (failures) return 1;
    std::cout << "PCVR render policy checks passed: " << checks << '\n';
    return 0;
}
