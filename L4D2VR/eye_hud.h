#pragma once
#include <optional>
#include <string_view>
#include <algorithm>
#include <cmath>

namespace EyeHud {
// Viewport-only centering adapted from iFeelLikeChicken2Nite/portal2vr-ultimate
// native-vr (https://github.com/iFeelLikeChicken2Nite/portal2vr-ultimate) at
// 0094269795aa29d2b0477af00e082b8b686aa68c. Local guards preserve the native
// Portal status reticle and caption capture; no target/depth rebinding.
struct Rect { int x, y, width, height; };
struct GameplayScope {
    bool published, vrEnabled, trackingValid, texturesReady, inGame, cursorVisible;
    bool activeEye, matchingEyeTarget, verifiedContext;
    bool Allowed() const {
        return published && vrEnabled && trackingValid && texturesReady && inGame &&
            !cursorVisible && activeEye && matchingEyeTarget && verifiedContext;
    }
};
inline bool CanCenter(const GameplayScope &scope, bool enabled, bool captionOverlay, int aimMode) {
    return scope.Allowed() && enabled && !captionOverlay && (aimMode == 0 || aimMode == 1);
}
inline bool IsGameplayOnlyPaint(int mode, int inGamePanels) {
    // A paint call forwards its entire mask. Never transform UI/cursor or
    // unknown mixed passes, and do not split paint calls with unverified order.
    return inGamePanels > 0 && mode == inGamePanels;
}
inline bool ContainsInsensitive(std::string_view text, std::string_view needle) {
    const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c; };
    return std::search(text.begin(), text.end(), needle.begin(), needle.end(),
        [&](char a, char b) { return lower(a) == lower(b); }) != text.end();
}
inline bool ShouldHideFlatCrosshair(const GameplayScope &scope, bool show, int aimMode,
    std::string_view name, std::string_view texture) {
    if (!scope.Allowed() || show || (aimMode != 0 && aimMode != 1)) return false;
    // Source's blue/orange validity layers and center artwork are gameplay state.
    if (ContainsInsensitive(name, "portal_crosshair") || ContainsInsensitive(texture, "portal_crosshair") ||
        ContainsInsensitive(name, "qi_center") || ContainsInsensitive(texture, "qi_center")) return false;
    return ContainsInsensitive(name, "crosshair") || ContainsInsensitive(texture, "crosshair");
}
inline std::optional<Rect> CenteredRect(int eyeWidth, int eyeHeight, int windowWidth, int windowHeight,
    float scale, float offset) {
    if (eyeWidth <= 0 || eyeHeight <= 0 || windowWidth <= 0 || windowHeight <= 0 ||
        !std::isfinite(scale) || !std::isfinite(offset) || scale < .2f || scale > 1.f ||
        offset < -.4f || offset > .4f) return std::nullopt;
    const double width = std::floor(static_cast<double>(eyeWidth) * scale);
    const double height = std::floor(width * windowHeight / windowWidth);
    if (width < 1 || height < 1 || width > eyeWidth || height > eyeHeight) return std::nullopt;
    const int w = static_cast<int>(width), h = static_cast<int>(height);
    const auto x = (eyeWidth - w) / 2;
    const double y = (eyeHeight - h) / 2 + std::trunc(static_cast<double>(eyeHeight) * offset);
    if (y < 0 || y > eyeHeight - h) return std::nullopt;
    return Rect{x, static_cast<int>(y), w, h};
}
template<class Context> class ViewportScope {
public:
    ViewportScope(Context &context, const Rect &previous, const Rect &desired)
        : m_Context(context), m_Previous(previous) { m_Context.Viewport(desired.x, desired.y, desired.width, desired.height); }
    ~ViewportScope() { m_Context.Viewport(m_Previous.x, m_Previous.y, m_Previous.width, m_Previous.height); }
    ViewportScope(const ViewportScope &) = delete;
    ViewportScope &operator=(const ViewportScope &) = delete;
private:
    Context &m_Context;
    Rect m_Previous;
};
}
