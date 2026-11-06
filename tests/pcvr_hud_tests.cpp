#include "../L4D2VR/config.h"
#include "../L4D2VR/eye_hud.h"
#include <iostream>
#include <sstream>
#include <cstdlib>
#include <climits>

static void expect(bool value, const char *message) {
    if (!value) { std::cerr << "FAIL: " << message << '\n'; std::exit(EXIT_FAILURE); }
}
struct Canvas {
    EyeHud::Rect rect{0, 0, 2528, 2704};
    void Viewport(int x, int y, int width, int height) { rect = {x, y, width, height}; }
};
static void nestedPaint(Canvas &canvas) {
    EyeHud::ViewportScope<Canvas> outer(canvas, canvas.rect, {10, 20, 1000, 900});
    {
        EyeHud::ViewportScope<Canvas> inner(canvas, canvas.rect, {30, 40, 500, 400});
        expect(canvas.rect.x == 30 && canvas.rect.width == 500, "inner paint uses its viewport");
    }
    expect(canvas.rect.x == 10 && canvas.rect.width == 1000, "nested paint restores outer viewport");
    return; // Early returns also restore the caller's viewport.
}

int main()
{
    std::istringstream invalid("HUDInEyeScale=nan\nHUDInEyeVerticalOffset=0.5\nShowSourceCrosshair=invalid\n");
    const auto parsed = ParseConfig(invalid, ConfigSnapshot{});
    if (parsed.errors.size() != 3) {
        std::cerr << "FAIL: malformed eye HUD and crosshair settings must be rejected\n";
        return EXIT_FAILURE;
    }
    const ConfigSnapshot defaults;
    expect(!defaults.hudInEyeCentered && defaults.showSourceCrosshair, "optional centering off and crosshair visible");
    expect(defaults.hudInEyeScale == .45f && defaults.hudInEyeVerticalOffset == .05f, "HUD geometry defaults");
    std::istringstream conflict("HUDInEyeCentered=true\nAimMode=2\n");
    const auto safe = ParseConfig(conflict, defaults);
    expect(!safe.value.hudInEyeCentered && !safe.notes.empty(), "incompatible startup centering safely disabled and reported");
    std::istringstream valid("HUDInEyeCentered=true\nAimMode=1\nHUDInEyeScale=0.2\nHUDInEyeVerticalOffset=-0.4\nShowSourceCrosshair=false\n");
    auto centered = ParseConfig(valid, defaults);
    expect(centered.errors.empty() && centered.value.hudInEyeCentered && !centered.value.showSourceCrosshair, "valid legacy configuration accepted");
    const auto reloaded = ApplyRuntimeConfig(defaults, centered, true);
    expect(!reloaded.value.hudInEyeCentered && !reloaded.notes.empty(), "enabling HUD hook requires restart");
    std::istringstream geometry("HUDInEyeScale=1\nHUDInEyeVerticalOffset=0.4\n");
    const auto active = ApplyRuntimeConfig(centered.value, ParseConfig(geometry, centered.value), true);
    expect(active.value.hudInEyeScale == 1 && active.value.hudInEyeVerticalOffset == .4f, "installed HUD geometry can reload");
    std::istringstream captionConflict("HUDInEyeCentered=true\nAimMode=1\nExperimentalHUDOverlay=true\n");
    const auto captionSafe = ParseConfig(captionConflict, defaults);
    expect(!captionSafe.value.hudInEyeCentered && captionSafe.value.experimentalHudOverlay, "startup fallback retains caption settings");
    std::istringstream modeConflict("AimMode=2\n");
    const auto modeSafe = ApplyRuntimeConfig(centered.value, ParseConfig(modeConflict, centered.value), true);
    expect(modeSafe.value.hudInEyeCentered && modeSafe.value.aimMode == 1 && !modeSafe.errors.empty(), "incompatible active reload preserves all prior settings");
    for (const char *text : {"HUDInEyeScale=0.19", "HUDInEyeScale=1.01", "HUDInEyeScale=inf",
        "HUDInEyeVerticalOffset=-0.41", "HUDInEyeVerticalOffset=0.41", "HUDInEyeVerticalOffset=nan"}) {
        std::istringstream malformed(text);
        const auto rejected = ApplyRuntimeConfig(centered.value, ParseConfig(malformed, centered.value), true);
        expect(!rejected.errors.empty() && rejected.value.hudInEyeScale == centered.value.hudInEyeScale &&
            rejected.value.hudInEyeVerticalOffset == centered.value.hudInEyeVerticalOffset, "malformed geometry reload preserves active config");
    }

    EyeHud::GameplayScope scope{true, true, true, true, true, false, true, true, true};
    expect(EyeHud::CanCenter(scope, true, false, 1), "verified legacy eye can center");
    expect(!EyeHud::CanCenter(scope, false, false, 1), "centering is opt in");
    expect(!EyeHud::CanCenter(scope, true, true, 1), "caption overlay keeps its layout");
    expect(!EyeHud::CanCenter(scope, true, false, 2), "native reticle keeps full eye viewport");
    constexpr int uiPanels = 1, inGamePanels = 2, cursor = 4;
    expect(EyeHud::IsGameplayOnlyPaint(inGamePanels, inGamePanels), "gameplay-only paint remains eligible");
    expect(!EyeHud::IsGameplayOnlyPaint(uiPanels | inGamePanels, inGamePanels), "mixed UI/gameplay paint retains original viewport");
    expect(!EyeHud::IsGameplayOnlyPaint(cursor | inGamePanels, inGamePanels), "mixed cursor/gameplay paint retains original viewport");
    expect(!EyeHud::IsGameplayOnlyPaint(uiPanels | cursor | inGamePanels, inGamePanels), "all mixed paint flags retain original viewport");
    expect(!EyeHud::IsGameplayOnlyPaint(uiPanels, inGamePanels), "UI-only paint bypasses centering");
    expect(!EyeHud::IsGameplayOnlyPaint(cursor, inGamePanels), "cursor-only paint bypasses centering");
    expect(!EyeHud::IsGameplayOnlyPaint(0, inGamePanels), "empty paint bypasses centering");
    expect(!EyeHud::IsGameplayOnlyPaint(8 | inGamePanels, inGamePanels), "unknown paint bits bypass centering");
    for (int field = 0; field < 9; ++field) {
        auto bypass = scope;
        bool *fields[] = {&bypass.published, &bypass.vrEnabled, &bypass.trackingValid,
            &bypass.texturesReady, &bypass.inGame, &bypass.cursorVisible, &bypass.activeEye,
            &bypass.matchingEyeTarget, &bypass.verifiedContext};
        *fields[field] = field == 5;
        expect(!EyeHud::CanCenter(bypass, true, false, 1), "every unsupported state bypasses centering");
        expect(!EyeHud::ShouldHideFlatCrosshair(bypass, false, 1, "crosshair", "hud/crosshair"), "every unsupported state retains crosshair");
    }
    expect(EyeHud::ShouldHideFlatCrosshair(scope, false, 1, "CrossHair", "hud/flat"), "generic flat crosshair can hide");
    expect(!EyeHud::ShouldHideFlatCrosshair(scope, true, 1, "crosshair", ""), "visible crosshair default preserved");
    expect(!EyeHud::ShouldHideFlatCrosshair(scope, false, 2, "crosshair", ""), "native mode never filters artwork");
    expect(!EyeHud::ShouldHideFlatCrosshair(scope, false, 1, "portal_crosshair_left_invalid", ""), "portal status always retained");
    expect(!EyeHud::ShouldHideFlatCrosshair(scope, false, 0, "crosshair", "hud/portal_crosshair"), "portal status texture always retained");
    expect(!EyeHud::ShouldHideFlatCrosshair(scope, false, 1, "crosshair", "hud/qi_center"), "Portal center status retained");
    expect(!EyeHud::ShouldHideFlatCrosshair(scope, false, 1, "subtitle", "hud/caption"), "unrecognized HUD artwork retained");
    const auto rect = EyeHud::CenteredRect(2528, 2704, 1280, 720, .45f, .05f);
    expect(rect && rect->x == 695 && rect->y == 1167 && rect->width == 1137 && rect->height == 639, "centered viewport preserves window aspect and offset");
    expect(!EyeHud::CenteredRect(0, 2704, 1280, 720, .45f, .05f), "zero eye rejected");
    expect(!EyeHud::CenteredRect(2528, 2704, 0, 720, .45f, .05f), "zero window rejected");
    expect(!EyeHud::CenteredRect(1000, 1000, 1000, 1000, 1.f, .4f), "out of eye offset rejected");
    expect(!EyeHud::CenteredRect(INT_MAX, 1000, 1, INT_MAX, 1.f, 0.f), "height overflow rejected");
    expect(!EyeHud::CenteredRect(1000, 1000, 1000, 1000, std::numeric_limits<float>::quiet_NaN(), 0), "NaN rejected");
    expect(!EyeHud::CenteredRect(1000, 1000, 1000, 1000, .19f, 0), "scale outside contract rejected");
    const auto full = EyeHud::CenteredRect(1000, 1000, 1000, 1000, 1, 0);
    expect(full && full->x == 0 && full->y == 0 && full->width == 1000 && full->height == 1000, "maximum scale fits identical aspect");
    const auto lower = EyeHud::CenteredRect(1000, 1000, 1000, 1000, .2f, -.4f);
    expect(lower && lower->y == 0, "minimum scale and negative offset boundary fits");
    expect(!EyeHud::CenteredRect(1000, 1000, 1000, 1000, std::numeric_limits<float>::infinity(), 0), "infinite scale rejected");
    expect(!EyeHud::CenteredRect(1000, 1000, 1000, 1000, .45f, std::numeric_limits<float>::infinity()), "infinite offset rejected");
    Canvas canvas;
    nestedPaint(canvas);
    expect(canvas.rect.x == 0 && canvas.rect.y == 0 && canvas.rect.width == 2528 && canvas.rect.height == 2704, "early return restores full caller viewport");
    try {
        EyeHud::ViewportScope<Canvas> scope(canvas, canvas.rect, {10, 20, 1000, 900});
        throw 1;
    } catch (int) {}
    expect(canvas.rect.width == 2528 && canvas.rect.height == 2704, "exception unwind restores caller viewport");
    std::cout << "PCVR HUD tests passed\n";
}
