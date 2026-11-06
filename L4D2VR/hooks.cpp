#include "hooks.h"
#include "viewmodel_alignment.h"
#include "game.h"
#include "texture.h"
#include "sdk.h"
#include "sdk_server.h"
#include "vr.h"
#include "offsets.h"
#include "logger.h"
#include "runtime_publication.h"
#include "hook_startup_rollback.h"
#include "aim_feedback.h"
#include "render_context_abi.h"
#include "reticle_telemetry.h"
#include "native_reticle.h"
#include "native_beam.h"
#include "eye_hud.h"
#include <Windows.h>
#include <intrin.h>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <optional>
#include <string>

static bool RuntimePublished()
{
    return Portal2VRRuntime::IsPublished(g_Game, Hooks::m_Game);
}

static bool ValidPlayerSlot(const Game *game, int index)
{
    return game && index > 0 && static_cast<size_t>(index) < game->m_PlayersVRInfo.size();
}

static std::optional<PortalOrientation::Rotation> ReadPortalRotation(const void* portal)
{
    if (!portal)
        return std::nullopt;
    // The existing CPortal_Base2D accessor uses this ABI-specific field.
    constexpr std::uintptr_t kMatrixOffset = 0x4C4;
    const auto base = reinterpret_cast<std::uintptr_t>(portal);
    if (base > UINTPTR_MAX - kMatrixOffset - sizeof(VMatrix))
        return std::nullopt;
    const auto matrixAddress = base + kMatrixOffset;
    VMatrix matrix;
    SIZE_T bytesRead = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(matrixAddress),
                           &matrix, sizeof(matrix), &bytesRead) || bytesRead != sizeof(matrix))
        return std::nullopt;
    return PortalOrientation::Rotation::FromVMatrix(matrix);
}

struct SourceHudTextureIdentity
{
    std::array<char, 64> shortName{};
    std::array<char, 64> textureFile{};
};

// Verified against CHudTexture::DrawSelf in the installed 32-bit client.dll.
// All sampled fields are checked against the actual material before use.
struct SourceHudTextureAtlas
{
    std::uint8_t renderUsingFont;
    std::uint8_t precached;
    char characterInFont;
    std::uint8_t padding;
    std::uint32_t font;
    int textureId;
    std::array<float, 4> uv;
    int left, right, top, bottom;
};
static_assert(sizeof(SourceHudTextureIdentity) == 128);
static_assert(sizeof(SourceHudTextureAtlas) == 44);

static std::optional<SourceHudTextureAtlas> ReadSourceHudTextureAtlas(const void *texture)
{
    if (!texture)
        return std::nullopt;
    constexpr std::size_t offset = sizeof(void *) + sizeof(SourceHudTextureIdentity);
    const auto base = reinterpret_cast<std::uintptr_t>(texture);
    if (base > UINTPTR_MAX - offset - sizeof(SourceHudTextureAtlas))
        return std::nullopt;
    SourceHudTextureAtlas atlas{};
    SIZE_T bytesRead = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void *>(base + offset),
            &atlas, sizeof(atlas), &bytesRead) || bytesRead != sizeof(atlas) ||
        atlas.renderUsingFont != 0 || atlas.textureId < 0)
        return std::nullopt;
    return atlas;
}

static std::optional<SourceHudTextureIdentity> ReadSourceHudTextureIdentity(const void *texture)
{
    if (!texture)
        return std::nullopt;
    // Valve's x86 CHudTexture starts with a virtual-destructor vptr, followed
    // by these two 64-byte names. Read defensively: Portal 2's exact ABI is
    // not guaranteed to match SDK 2013.
    const auto base = reinterpret_cast<std::uintptr_t>(texture);
    if (base > UINTPTR_MAX - sizeof(void *))
        return std::nullopt;
    SourceHudTextureIdentity identity;
    SIZE_T bytesRead = 0;
    if (!ReadProcessMemory(GetCurrentProcess(),
            reinterpret_cast<const void *>(base + sizeof(void *)),
            &identity, sizeof(identity), &bytesRead) || bytesRead != sizeof(identity) ||
        !std::memchr(identity.shortName.data(), '\0', identity.shortName.size()) ||
        !std::memchr(identity.textureFile.data(), '\0', identity.textureFile.size()))
        return std::nullopt;
    const auto printable = [](const auto &name) {
        if (name[0] == '\0')
            return false;
        for (const char *at = name.data(); *at; ++at)
            if (*at < 32 || *at > 126)
                return false;
        return true;
    };
    if (!printable(identity.shortName) || !printable(identity.textureFile))
        return std::nullopt;
    return identity;
}

static std::string DescribeHudTexture(const SourceHudTextureIdentity &identity)
{
    const auto printable = [](const auto &name) {
        for (const char *at = name.data(); *at; ++at)
            if (*at < 32 || *at > 126)
                return false;
        return true;
    };
    return std::string("name=") +
        (printable(identity.shortName) ? identity.shortName.data() : "<unreadable>") +
        " material=" +
        (printable(identity.textureFile) ? identity.textureFile.data() : "<unreadable>");
}

struct NativeReticleSurface
{
    using ClipSetter = void(__cdecl *)(int, int, int, int);
    ClipSetter setter = nullptr;
    NativeReticle::ClipRect previous{};
    std::array<int, 2> translation{};

    void SetClipRect(const NativeReticle::ClipRect &rect)
    {
        setter(rect.left, rect.top, rect.right, rect.bottom);
    }
};

static std::optional<NativeReticleSurface> PrepareNativeReticleSurface(ISurface *surface)
{
    const auto read = [](std::uintptr_t address, void *output, std::size_t size) {
        SIZE_T bytesRead = 0;
        return address && ReadProcessMemory(GetCurrentProcess(),
            reinterpret_cast<const void *>(address), output, size, &bytesRead) && bytesRead == size;
    };
    std::uintptr_t table = 0;
    if (!read(reinterpret_cast<std::uintptr_t>(surface), &table, sizeof(table)))
        return std::nullopt;
    static thread_local std::uintptr_t checkedTable = 0;
    static thread_local std::uintptr_t checkedModule = 0;
    if (table != checkedTable) {
        checkedTable = table;
        checkedModule = 0;
        const auto module = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(L"vguimatsurface.dll"));
        IMAGE_DOS_HEADER dos{};
        IMAGE_NT_HEADERS32 nt{};
        if (!read(module, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
            dos.e_lfanew <= 0 || dos.e_lfanew > 4096 ||
            !read(module + dos.e_lfanew, &nt, sizeof(nt)) || nt.Signature != IMAGE_NT_SIGNATURE ||
            nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC)
            return std::nullopt;
        NativeReticle::SurfaceProbe probe{module, nt.FileHeader.TimeDateStamp,
            nt.OptionalHeader.SizeOfImage, table, 0, 0};
        // Read slots only from the recognized table, not an arbitrary vptr.
        if (table != module + 0xC4ED4 ||
            !read(table + NativeReticle::kDrawTexturedSubRectSlot * sizeof(void *),
                &probe.drawTexturedSubRect, sizeof(probe.drawTexturedSubRect)) ||
            !read(table + NativeReticle::kGetScreenSizeSlot * sizeof(void *),
                &probe.getScreenSize, sizeof(probe.getScreenSize)) || !NativeReticle::Supported(probe))
            return std::nullopt;
        constexpr std::array<std::uint8_t, 12> clipPrefix{
            0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x08, 0x8B, 0x4D, 0x14, 0x8B, 0x55, 0x10};
        std::array<std::uint8_t, clipPrefix.size()> actual{};
        if (!read(module + NativeReticle::kClipSetterRva, actual.data(), actual.size()) || actual != clipPrefix)
            return std::nullopt;
        checkedModule = module;
    }
    if (!checkedModule)
        return std::nullopt;
    NativeReticleSurface result;
    if (!read(checkedModule + NativeReticle::kClipRectRva, &result.previous, sizeof(result.previous)) ||
        !read(reinterpret_cast<std::uintptr_t>(surface) + NativeReticle::kSurfaceTranslationOffset,
            result.translation.data(), sizeof(result.translation)))
        return std::nullopt;
    result.setter = reinterpret_cast<NativeReticleSurface::ClipSetter>(
        checkedModule + NativeReticle::kClipSetterRva);
    return result;
}

static Portal2MaterialAbi::Kind CheckRenderContextAbi(IMatRenderContext *context)
{
    if (!context)
        return Portal2MaterialAbi::Kind::Unsupported;
    const auto read = [](const void *address, void *output, std::size_t size) {
        SIZE_T bytesRead = 0;
        return ReadProcessMemory(GetCurrentProcess(), address, output, size, &bytesRead) &&
            bytesRead == size;
    };
    std::uintptr_t vtable = 0;
    if (!read(context, &vtable, sizeof(vtable)) || !vtable)
        return Portal2MaterialAbi::Kind::Unsupported;
    static thread_local std::uintptr_t checkedVtable = 0;
    static thread_local Portal2MaterialAbi::Kind checkedKind = Portal2MaterialAbi::Kind::Unsupported;
    if (vtable == checkedVtable)
        return checkedKind;
    checkedVtable = vtable;
    checkedKind = Portal2MaterialAbi::Kind::Unsupported;

    const auto module = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(L"materialsystem.dll"));
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS32 nt{};
    if (!module || !read(reinterpret_cast<const void *>(module), &dos, sizeof(dos)) ||
        dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 || dos.e_lfanew > 4096 ||
        !read(reinterpret_cast<const void *>(module + dos.e_lfanew), &nt, sizeof(nt)) ||
        nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
        nt.FileHeader.TimeDateStamp != 0x6A4466CAu ||
        nt.OptionalHeader.SizeOfImage != 0x14D000u ||
        module > UINTPTR_MAX - nt.OptionalHeader.SizeOfImage)
        return checkedKind;
    if (vtable != module + 0x9BEF4u && vtable != module + 0x9ED4Cu)
        return checkedKind;
    Portal2MaterialAbi::Probe probe{module, nt.FileHeader.TimeDateStamp,
        nt.OptionalHeader.SizeOfImage, vtable, 0, 0, 0};
    const auto slot = [&](std::size_t index, std::uintptr_t &address) {
        return read(reinterpret_cast<const void *>(vtable + index * sizeof(std::uintptr_t)),
                    &address, sizeof(address));
    };
    if (slot(Portal2MaterialAbi::kViewportSlot, probe.viewport) &&
        slot(Portal2MaterialAbi::kGetViewportSlot, probe.getViewport) &&
        slot(Portal2MaterialAbi::kDrawScreenSpaceRectangleSlot, probe.drawScreenSpaceRectangle))
        checkedKind = Portal2MaterialAbi::Classify(probe);
    return checkedKind;
}

enum class ReticleMaterialDrawResult
{
    Drawn,
    AtlasUnavailable,
    MaterialUnavailable,
    EyeTargetUnavailable,
    ViewportUnavailable,
    ColorUnavailable,
    AbiUnsupported
};

static const char *ReticleDrawFailureName(ReticleMaterialDrawResult result)
{
    switch (result) {
    case ReticleMaterialDrawResult::AbiUnsupported:
        return "materialsystem.dll build or render-context ABI mismatch";
    case ReticleMaterialDrawResult::ViewportUnavailable:
        return "invalid viewport";
    case ReticleMaterialDrawResult::EyeTargetUnavailable:
        return "eye render target unavailable";
    case ReticleMaterialDrawResult::MaterialUnavailable:
        return "Source material unavailable";
    case ReticleMaterialDrawResult::ColorUnavailable:
        return "Source color unreadable";
    case ReticleMaterialDrawResult::AtlasUnavailable:
        return "Source atlas unavailable";
    default:
        return "unexpected draw result";
    }
}

static ReticleMaterialDrawResult DrawSourceReticleInEye(
    Game *game, const SourceHudTextureIdentity &identity,
    const SourceHudTextureAtlas &atlas, ITexture *eyeTarget,
    const AimFeedback::ScreenPoint &position, int width, int height,
    int eyeWidth, int eyeHeight, const void *sourceColor, int *sourceAlpha)
{
    if (!eyeTarget)
        return ReticleMaterialDrawResult::EyeTargetUnavailable;
    IMatRenderContext *context = game->m_MaterialSystem->GetRenderContext();
    if (!context)
        return ReticleMaterialDrawResult::EyeTargetUnavailable;
    if (CheckRenderContextAbi(context) == Portal2MaterialAbi::Kind::Unsupported) {
        context->Release();
        return ReticleMaterialDrawResult::AbiUnsupported;
    }
    if (context->GetRenderTarget() != eyeTarget) {
        context->Release();
        return ReticleMaterialDrawResult::EyeTargetUnavailable;
    }
    IMaterial *material = game->m_MaterialSystem->FindMaterial(
        identity.textureFile.data(), "VGUI textures", false);
    if (!material || material->IsErrorMaterial()) {
        context->Release();
        return ReticleMaterialDrawResult::MaterialUnavailable;
    }
    const int atlasWidth = material->GetMappingWidth();
    const int atlasHeight = material->GetMappingHeight();
    const auto source = AimFeedback::SourceHudAtlasRect(atlas.uv,
        atlas.left, atlas.right, atlas.top, atlas.bottom,
        atlasWidth, atlasHeight, width, height);
    if (!source) {
        context->Release();
        return ReticleMaterialDrawResult::AtlasUnavailable;
    }
    std::array<std::uint8_t, 4> color{255, 255, 255, 255};
    SIZE_T bytesRead = 0;
    if (!sourceColor || !ReadProcessMemory(GetCurrentProcess(), sourceColor,
            color.data(), color.size(), &bytesRead) || bytesRead != color.size()) {
        context->Release();
        return ReticleMaterialDrawResult::ColorUnavailable;
    }
    if (sourceAlpha)
        *sourceAlpha = color[3];
    int oldX = 0, oldY = 0, oldWidth = 0, oldHeight = 0;
    context->GetViewport(oldX, oldY, oldWidth, oldHeight);
    if (oldWidth <= 0 || oldHeight <= 0 || eyeWidth <= 0 || eyeHeight <= 0) {
        context->Release();
        return ReticleMaterialDrawResult::ViewportUnavailable;
    }
    const bool changeViewport = oldX != 0 || oldY != 0 ||
        oldWidth != eyeWidth || oldHeight != eyeHeight;
    if (changeViewport)
        context->Viewport(0, 0, eyeWidth, eyeHeight);
    float oldRed = 1.0f, oldGreen = 1.0f, oldBlue = 1.0f;
    material->GetColorModulation(&oldRed, &oldGreen, &oldBlue);
    const float oldAlpha = material->GetAlphaModulation();
    material->ColorModulate(color[0] / 255.0f, color[1] / 255.0f, color[2] / 255.0f);
    material->AlphaModulate(color[3] / 255.0f);
    context->DrawScreenSpaceRectangle(material, position.x, position.y,
        width, height, source->x0, source->y0, source->x1, source->y1,
        atlasWidth, atlasHeight);
    material->ColorModulate(oldRed, oldGreen, oldBlue);
    material->AlphaModulate(oldAlpha);
    if (changeViewport)
        context->Viewport(oldX, oldY, oldWidth, oldHeight);
    context->Release();
    return ReticleMaterialDrawResult::Drawn;
}

static std::string DescribeReticleEye(const char *name,
                                     const ReticleTelemetry::EyeCounts &counts)
{
    std::string result = std::string(name) + " drawn=" + std::to_string(counts.drawn) +
        " outside=" + std::to_string(counts.outside) +
        " failed=" + std::to_string(counts.failed);
    constexpr const char *iconNames[] = {"LI", "LV", "RI", "RV"};
    for (std::size_t index = 0; index < 4; ++index) {
        const auto &icon = counts.icons[index];
        result += " " + std::string(iconNames[index]) + "(a0/mid/255)=" +
            std::to_string(icon.zeroAlpha) + "/" +
            std::to_string(icon.partialAlpha) + "/" +
            std::to_string(icon.opaqueAlpha);
    }
    return result;
}

static void RecordReticleDraw(bool verbose, int eye, ReticleTelemetry::Icon icon,
                              int alpha, ReticleTelemetry::Result result)
{
    if (!verbose)
        return;
    static thread_local ReticleTelemetry telemetry;
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    const auto summary = telemetry.Record(eye == 1 ? ReticleTelemetry::Eye::Left :
        ReticleTelemetry::Eye::Right, icon, alpha, result,
        static_cast<std::uint64_t>(milliseconds));
    if (summary)
        Logger::Write("Stereo reticle 1s: " + DescribeReticleEye("L", summary->eyes[0]) +
            "; " + DescribeReticleEye("R", summary->eyes[1]));
}

Hooks::Hooks(Game *game)
{
	m_Game = game;
	m_VR = game->m_VR;
	m_PushHUDStep = -999;
	m_PushedHud = true;
	m_HudCaptureRoute = HudCapture::RouteState{};
}

void Hooks::Initialize()
{
	if (MH_Initialize() != MH_OK)
	{
		Game::errorMsg("Failed to init MinHook");
		return;
	}
	m_MinHookInitialized = true;

	if (initSourceHooks() != 0)
		return;
	std::string failedHook;
	if (!m_RequiredHooks.CreateAll(failedHook)) {
		Logger::Write("Failed to create required hook " + failedHook);
		return;
	}
	if (!m_RequiredHooks.EnableAll(failedHook)) {
		Logger::Write("Failed to enable required hook " + failedHook);
		return;
	}
	m_CompatibilityHooks.Install([](const std::string &warning) { Logger::Write(warning); });
	// The optional particle path remains independently disabled on failure.
	if (m_Game->m_Offsets->m_LaserAvailable && hkPrecache.enableHook()) {
		m_Game->m_Offsets->m_LaserAvailable = false;
		Logger::Write("Laser pointer disabled: Precache hook enable failed.");
	}
	if (m_VR->m_Config.experimentalHudOverlay &&
		m_VR->m_HUDHandle != vr::k_ulOverlayHandleInvalid) {
		const auto *offsets = m_Game->m_Offsets;
		Logger::Write("Experimental HUD symbols: PushRenderTarget=" +
			std::string(offsets->PushRenderTargetAndViewport.address ? "OK" : "MISSING") +
			" PopRenderTarget=" +
			std::string(offsets->PopRenderTargetAndViewport.address ? "OK" : "MISSING") +
			" VGui_Paint=" + std::string(offsets->VGui_Paint.address ? "OK" : "MISSING"));
		if (offsets->PushRenderTargetAndViewport.address &&
			offsets->PopRenderTargetAndViewport.address && offsets->VGui_Paint.address) {
			const bool created =
				!hkPushRenderTargetAndViewport.createHook(
					(LPVOID)offsets->PushRenderTargetAndViewport.address, &dPushRenderTargetAndViewport) &&
				!hkPopRenderTargetAndViewport.createHook(
					(LPVOID)offsets->PopRenderTargetAndViewport.address, &dPopRenderTargetAndViewport) &&
				!hkVgui_Paint.createHook((LPVOID)offsets->VGui_Paint.address, &dVGui_Paint);
			if (created) {
				const bool pushEnabled = !hkPushRenderTargetAndViewport.enableHook();
				const bool popEnabled = pushEnabled && !hkPopRenderTargetAndViewport.enableHook();
				const bool paintEnabled = popEnabled && !hkVgui_Paint.enableHook();
				m_HudCaptureHooksReady = pushEnabled && popEnabled && paintEnabled;
				if (!m_HudCaptureHooksReady) {
					if (popEnabled && hkPopRenderTargetAndViewport.disableHook()) m_OptionalRollbackFailed = true;
					if (pushEnabled && hkPushRenderTargetAndViewport.disableHook()) m_OptionalRollbackFailed = true;
				}
			}
		}
		Logger::Write(m_HudCaptureHooksReady ?
			"Experimental HUD VGUI capture hooks enabled" :
			"Experimental HUD capture unavailable; stereo rendering remains enabled");
	}
    else if (m_VR->m_Config.hudInEyeCentered) {
        // Legacy eye HUD needs only the paint detour. Caption target-stack
        // hooks remain a separate optional group with their existing policy.
        const auto paint = m_Game->m_Offsets->VGui_Paint.address;
        m_EyeHudHookReady = paint &&
            !hkVgui_Paint.createHook(reinterpret_cast<LPVOID>(paint), &dVGui_Paint) &&
            !hkVgui_Paint.enableHook();
        Logger::Write(m_EyeHudHookReady ?
            "Legacy eye HUD paint hook enabled (pixels unverified)" :
            "Legacy eye HUD paint hook unavailable; original layout retained");
    }
    InitViewmodelAlignment();
    InitMuzzleSampling();
    InitNativeBeam();
	if (m_OptionalRollbackFailed) {
		Logger::Write("Optional hook rollback failed; runtime publication stopped");
		return;
	}
	m_Ready = true;
}

void Hooks::InitNativeBeam()
{
    if (!m_Game->m_Offsets->m_LaserAvailable)
        return;
    const auto module = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(L"client.dll"));
    const auto read = [](std::uintptr_t address, void *output, std::size_t size) {
        SIZE_T bytesRead = 0;
        return address && ReadProcessMemory(GetCurrentProcess(),
            reinterpret_cast<const void *>(address), output, size, &bytesRead) && bytesRead == size;
    };
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS32 nt{};
    const auto *offsets = m_Game->m_Offsets;
    const auto caller = offsets->CreatePingPointer.address + NativeBeam::kMuzzleLookupReturnOffset;
    // The extra hook only selects an existing branch in this audited build.
    // Validate the exact indirect call/branch, not just a plausible prologue.
    constexpr std::array<std::uint8_t, 7> branch{0xFF, 0xD0, 0x6A, 0x00, 0x83, 0xF8, 0xFF};
    std::array<std::uint8_t, branch.size()> actual{};
    constexpr std::array<std::uint8_t, 5> createCall{0xE8, 0x40, 0x2B, 0xEF, 0xFF};
    constexpr std::array<std::uint8_t, 10> createEntry{0x55, 0x8B, 0xEC, 0x56, 0x57,
        0x8B, 0x7D, 0x08, 0x8B, 0xF1};
    // Exact audited CP updater: ABSORIGIN (0) returns when !initializing.
    constexpr std::array<std::uint8_t, 17> updateRule{0x80, 0x7D, 0x10, 0x00, 0x75, 0x14,
        0x8B, 0x46, 0x04, 0x85, 0xC0, 0x0F, 0x84, 0x4F, 0x02, 0x00, 0x00};
    std::array<std::uint8_t, createCall.size()> actualCall{};
    std::array<std::uint8_t, createEntry.size()> actualEntry{};
    std::array<std::uint8_t, updateRule.size()> actualRule{};
    const bool compatible = module && offsets->LookupViewmodelAttachment.address &&
        read(module, &dos, sizeof(dos)) && dos.e_magic == IMAGE_DOS_SIGNATURE &&
        dos.e_lfanew > 0 && dos.e_lfanew <= 4096 &&
        read(module + dos.e_lfanew, &nt, sizeof(nt)) && nt.Signature == IMAGE_NT_SIGNATURE &&
        nt.OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC &&
        nt.FileHeader.TimeDateStamp == 0x6AA07473 && nt.OptionalHeader.SizeOfImage == 0xFF3000 &&
        offsets->CreatePingPointer.address == module + 0x280800 &&
        offsets->LookupViewmodelAttachment.address == module + 0x514B0 &&
        read(caller - 2, actual.data(), actual.size()) && actual == branch &&
        read(module + 0x2808FB, actualCall.data(), actualCall.size()) && actualCall == createCall &&
        read(module + 0x173440, actualEntry.data(), actualEntry.size()) && actualEntry == createEntry &&
        read(module + 0x172754, actualRule.data(), actualRule.size()) && actualRule == updateRule;
    if (!compatible) {
        Logger::Write("Native manual-origin beam route unavailable: audited client ABI mismatch; existing native creation retained");
        return;
    }
    const bool lookupCreated = !hkBeamAttachmentLookup.createHook(
        reinterpret_cast<LPVOID>(offsets->LookupViewmodelAttachment.address), &dBeamAttachmentLookup);
    const bool createCreated = lookupCreated && !hkCreateBeamParticle.createHook(
        reinterpret_cast<LPVOID>(module + 0x173440), &dCreateBeamParticle);
    const bool createEnabled = createCreated && !hkCreateBeamParticle.enableHook();
    const bool lookupEnabled = createEnabled && !hkBeamAttachmentLookup.enableHook();
    if (!lookupEnabled) {
		if (createEnabled && hkCreateBeamParticle.disableHook()) m_OptionalRollbackFailed = true;
        Logger::Write("Native manual-origin beam route unavailable: optional hook installation failed; existing native creation retained");
        return;
    }
    m_NativeBeamManualOriginReady = true;
    Logger::Write("Native robot_point_beam: player-owned ABSORIGIN creation; eyes-follow CP0 updates disabled for our beam only; visibility requires VR test");

    constexpr std::array<std::uint8_t, 10> drawEntry{0x55, 0x8B, 0xEC, 0x83, 0xEC,
        0x64, 0x53, 0x56, 0x8B, 0xF1};
    std::array<std::uint8_t, drawEntry.size()> actualDraw{};
    std::uintptr_t drawVtableEntry = 0;
    // CNewParticleEffect renderable = primary+8, slot9, DrawModel(flags, instance).
    m_NativeBeamDiagnosticsReady = read(module + 0x77E464 + 9 * 4, &drawVtableEntry, 4) &&
        drawVtableEntry == module + 0x17C950 &&
        read(drawVtableEntry, actualDraw.data(), actualDraw.size()) && actualDraw == drawEntry &&
        !hkDrawBeamParticle.createHook(reinterpret_cast<LPVOID>(drawVtableEntry), &dDrawBeamParticle) &&
        !hkDrawBeamParticle.enableHook();
    Logger::Write(m_NativeBeamDiagnosticsReady ?
        "Native beam DrawModel observation enabled (one record per eye scope + one bounded summary; no visibility claim)" :
        "Native beam DrawModel observation unavailable; beam creation remains enabled");
}

void Hooks::CreateNativeAimPointer(void *player, const Vector &target)
{
    if (!player || !CreatePingPointer)
        return;
    NativeBeam::CreationScope scope(m_NativeBeamLookupCaller, m_NativeBeamManualOriginReady ?
        m_Game->m_Offsets->CreatePingPointer.address + NativeBeam::kMuzzleLookupReturnOffset : 0);
    CreatePingPointer(player, target);
}

int __fastcall Hooks::dBeamAttachmentLookup(void *ecx, void *, const char *name)
{
    if (!RuntimePublished()) return hkBeamAttachmentLookup.fOriginal(ecx, name);
    if (NativeBeam::UsePlayerOwnedFallback(m_NativeBeamLookupCaller,
            reinterpret_cast<std::uintptr_t>(_ReturnAddress()), name)) {
        if (m_VR->m_RenderDiagnostics.First(RenderDiagnosticEvent::NativeBeamPlayerOwned))
            Logger::Write("Native beam creation selected Source's player-owned fallback; model/glow attachments unchanged");
        return -1;
    }
    return hkBeamAttachmentLookup.fOriginal(ecx, name);
}

void *__fastcall Hooks::dCreateBeamParticle(void *ecx, void *, const char *name,
    int attachType, int attachment, Vector offset, int flags)
{
    if (!RuntimePublished())
        return hkCreateBeamParticle.fOriginal(ecx, name, attachType, attachment, offset, flags);
    const int controlled = NativeBeam::FactoryAttachment(m_NativeBeamLookupCaller,
        reinterpret_cast<std::uintptr_t>(_ReturnAddress()), name, attachType, attachment);
    if (controlled != attachType &&
        m_VR->m_RenderDiagnostics.First(RenderDiagnosticEvent::NativeBeamManualOrigin))
        Logger::Write("Native beam CP0 binding changed: EYES_FOLLOW(6) -> ABSORIGIN(0); subsequent native eye-position updates skipped; VR maintains muzzle CP0");
    return hkCreateBeamParticle.fOriginal(ecx, name, controlled, attachment, offset, flags);
}

int __fastcall Hooks::dDrawBeamParticle(void *ecx, void *, int flags, const void *instance)
{
    // Preserve every native render/cull decision. A nonzero return is still
    // not proof of pixels reaching the headset (batching/materials may differ).
    const int result = hkDrawBeamParticle.fOriginal(ecx, flags, instance);
    if (Portal2VRRuntime::IsPublished(g_Game, m_Game) &&
        m_NativeBeamRenderProbe.ObserveDraw(reinterpret_cast<std::uintptr_t>(ecx), m_ActiveAimEye))
        Logger::Write("Native beam DrawModel observed: eyeScope=" + std::to_string(m_ActiveAimEye) +
            " flags=" + std::to_string(flags) + " nativeReturn=" + std::to_string(result) +
            " (0=outside stereo scope; native return is not pixel/visibility proof)");
    return result;
}

void Hooks::InitViewmodelAlignment()
{
    if (!m_VR->m_Config.experimentalViewmodelAlignment)
        return;
    const auto *offsets = m_Game->m_Offsets;
    const struct Symbol { const char *name; const Offset *offset; } symbols[] = {
        {"DrawViewModels", &offsets->DrawViewModels},
        {"ViewmodelCalcView", &offsets->ViewmodelCalcView},
        {"FormatViewModelAttachment", &offsets->FormatViewModelAttachment},
        {"SetViewmodelLocalOrigin", &offsets->SetViewmodelLocalOrigin},
        {"SetViewmodelLocalAngles", &offsets->SetViewmodelLocalAngles},
        {"ViewmodelScreenAspect", &offsets->ViewmodelScreenAspect}
    };
    bool available = true;
    for (const auto &symbol : symbols) {
        Logger::Write(std::string(symbol.name) + " ........ " +
            (symbol.offset->address ? "OK" : "MISSING") + " [optional viewmodel alignment]");
        available = available && symbol.offset->address != 0;
    }
    if (available) {
        const bool created =
            !hkDrawViewModels.createHook((LPVOID)offsets->DrawViewModels.address, &dDrawViewModels) &&
            !hkViewmodelCalcView.createHook((LPVOID)offsets->ViewmodelCalcView.address, &dViewmodelCalcView) &&
            !hkFormatViewModelAttachment.createHook((LPVOID)offsets->FormatViewModelAttachment.address, &dFormatViewModelAttachment) &&
            !hkViewmodelScreenAspect.createHook((LPVOID)offsets->ViewmodelScreenAspect.address, &dViewmodelScreenAspect);
        if (created) {
            SetViewmodelLocalOrigin = reinterpret_cast<tSetViewmodelLocalOrigin>(offsets->SetViewmodelLocalOrigin.address);
            SetViewmodelLocalAngles = reinterpret_cast<tSetViewmodelLocalAngles>(offsets->SetViewmodelLocalAngles.address);
            m_ViewmodelAlignmentReady = !hkDrawViewModels.enableHook() &&
                !hkViewmodelCalcView.enableHook() && !hkFormatViewModelAttachment.enableHook() &&
                !hkViewmodelScreenAspect.enableHook();
        }
    }
    if (!m_ViewmodelAlignmentReady) {
		if (hkDrawViewModels.isEnabled && hkDrawViewModels.disableHook()) m_OptionalRollbackFailed = true;
		if (hkViewmodelCalcView.isEnabled && hkViewmodelCalcView.disableHook()) m_OptionalRollbackFailed = true;
		if (hkFormatViewModelAttachment.isEnabled && hkFormatViewModelAttachment.disableHook()) m_OptionalRollbackFailed = true;
		if (hkViewmodelScreenAspect.isEnabled && hkViewmodelScreenAspect.disableHook()) m_OptionalRollbackFailed = true;
        SetViewmodelLocalOrigin = nullptr;
        SetViewmodelLocalAngles = nullptr;
    }
    Logger::Write(m_ViewmodelAlignmentReady ?
        "Experimental viewmodel alignment enabled: controller-locked native model; VR aspect; no player-view FOV attachment warp (hardware unverified)" :
        "Experimental viewmodel alignment unavailable; entire optional group disabled, legacy viewmodel retained");
}

bool Hooks::CanAlignViewmodel()
{
    if (!Portal2VRRuntime::IsPublished(g_Game, m_Game))
        return false;
    return ViewmodelAlignment::Eligibility{
        true, m_VR->m_IsVREnabled && m_VR->m_Config.experimentalViewmodelAlignment,
        m_Game->m_Hooks->m_ViewmodelAlignmentReady, m_VR->m_TrackingOutputValid,
        m_VR->m_RightControllerPose.valid, m_Game->m_EngineClient->IsInGame(),
        m_Game->m_VguiSurface->IsCursorVisible()}.Allowed();
}

void Hooks::InitMuzzleSampling()
{
    if (!m_VR->m_Config.aimFromViewmodelMuzzle)
        return;
    const auto *offsets = m_Game->m_Offsets;
    const struct Symbol { const char *name; const Offset *offset; } symbols[] = {
        {"ViewmodelFormatAttachment", &offsets->ViewmodelFormatAttachment},
        {"LookupViewmodelAttachment", &offsets->LookupViewmodelAttachment},
        {"GetViewmodelOwner", &offsets->GetViewmodelOwner},
        {"GetViewmodelAbsOrigin", &offsets->GetViewmodelAbsOrigin},
        {"GetViewmodelAbsAngles", &offsets->GetViewmodelAbsAngles}
    };
    bool available = m_ViewmodelAlignmentReady;
    for (const auto &symbol : symbols) {
        Logger::Write(std::string(symbol.name) + " ........ " +
            (symbol.offset->address ? "OK" : "MISSING") + " [optional muzzle sampling]");
        available = available && symbol.offset->address != 0;
    }
    if (available && !hkViewmodelFormatAttachment.createHook(
            reinterpret_cast<LPVOID>(offsets->ViewmodelFormatAttachment.address),
            &dViewmodelFormatAttachment)) {
        LookupViewmodelAttachment = reinterpret_cast<tLookupViewmodelAttachment>(offsets->LookupViewmodelAttachment.address);
        GetViewmodelOwner = reinterpret_cast<tGetViewmodelOwner>(offsets->GetViewmodelOwner.address);
        GetViewmodelAbsOrigin = reinterpret_cast<tGetViewmodelAbsOrigin>(offsets->GetViewmodelAbsOrigin.address);
        GetViewmodelAbsAngles = reinterpret_cast<tGetViewmodelAbsAngles>(offsets->GetViewmodelAbsAngles.address);
        m_MuzzleSamplingReady = !hkViewmodelFormatAttachment.enableHook();
    }
    if (!m_MuzzleSamplingReady) {
        LookupViewmodelAttachment = nullptr;
        GetViewmodelOwner = nullptr;
        GetViewmodelAbsOrigin = nullptr;
        GetViewmodelAbsAngles = nullptr;
    }
    Logger::Write(m_MuzzleSamplingReady ?
        "Muzzle sampling enabled: passive native animated attachment; beam uses current model pose" :
        "Muzzle sampling unavailable: requested muzzle beam suppressed; existing model alignment unchanged");
    Logger::Write(std::string("Aim beam: ") +
        (m_VR->m_Config.experimentalWorldAimMarker ? "experimental muzzle line" : "native robot_point_beam") +
        "; reticle: " + (m_VR->m_Config.experimentalStereoReticle ?
            "rollback stereo Source atlas" : "original per-eye Source DrawSelf"));
}

void __fastcall Hooks::dViewmodelFormatAttachment(void *ecx, void *, int index, matrix3x4_t &matrix)
{
    hkViewmodelFormatAttachment.fOriginal(ecx, index, matrix);
    if (!CanAlignViewmodel() || !m_Game->m_Hooks->m_MuzzleSamplingReady)
        return;
    const auto local = m_Game->GetClientEntity(m_Game->m_EngineClient->GetLocalPlayer());
    if (!local || GetViewmodelOwner(ecx) != local)
        return;
    // Native SetupBones_AttachmentHelper passes zero-based i here and stores
    // i+1 afterwards (client 0x5D55C). Lookup uses the renderable subobject;
    // unlike GetAttachment it does not force a recursive bone setup.
    const int muzzle = LookupViewmodelAttachment(static_cast<char *>(ecx) + 4, "muzzle");
    if (muzzle <= 0 || index != muzzle - 1)
        return;
    m_VR->CaptureViewmodelMuzzle({matrix[0][3], matrix[1][3], matrix[2][3]},
                                GetViewmodelAbsOrigin(ecx), GetViewmodelAbsAngles(ecx));
}

void __fastcall Hooks::dDrawViewModels(void *ecx, void *, const CViewSetup &view, bool draw)
{
    // Portal 2 replaces this view's aspect with the engine's width/height-based
    // aspect (or convar override) inside DrawViewModels, client RVA 0x1F216D.
    // That is not the symmetric VR projection aspect. Override this draw only.
    const float aspect = CanAlignViewmodel() && m_ActiveAimEyeView ? view.m_flAspectRatio : 0.0f;
    ViewmodelAlignment::ProjectionScope scope(m_ViewmodelDrawAspect, aspect);
    hkDrawViewModels.fOriginal(ecx, view, draw);
}

float __fastcall Hooks::dViewmodelScreenAspect(void *ecx, void *, int width, int height)
{
    const float nativeAspect = hkViewmodelScreenAspect.fOriginal(ecx, width, height);
    if (!RuntimePublished()) return nativeAspect;
    static bool logged = false;
    if (!logged && std::isfinite(m_ViewmodelDrawAspect) && m_ViewmodelDrawAspect > 0.0f) {
        logged = true;
        Logger::Write("Experimental viewmodel projection: nativeAspect=" + std::to_string(nativeAspect) +
            " eyeAspect=" + std::to_string(m_ViewmodelDrawAspect) +
            " dimensions=" + std::to_string(width) + "x" + std::to_string(height));
    }
    return ViewmodelAlignment::AspectOr(m_ViewmodelDrawAspect, nativeAspect);
}

void __fastcall Hooks::dViewmodelCalcView(void *ecx, void *, void *owner,
                                        const Vector &origin, const QAngle &angles)
{
    hkViewmodelCalcView.fOriginal(ecx, owner, origin, angles);
    if (!m_ControllerViewmodelUpdate || !CanAlignViewmodel())
        return;
    // The native function adds viewmodel offsets, bob and lag after our player
    // hook. Retain its housekeeping, then use Source setters (which invalidate
    // transforms) instead of writing guessed entity fields.
    SetViewmodelLocalOrigin(ecx, origin);
    SetViewmodelLocalAngles(ecx, angles);
    static bool logged = false;
    if (!logged) {
        logged = true;
        Logger::Write("Experimental viewmodel: native local pose set after bob/offset calculation; model pivot still requires VR calibration");
    }
}

void __cdecl Hooks::dFormatViewModelAttachment(void *owner, Vector &origin, bool inverse)
{
    if (CanAlignViewmodel()) {
        const auto local = m_Game->GetClientEntity(m_Game->m_EngineClient->GetLocalPlayer());
        // With identical model/world projection the forward and inverse FOV
        // conversions are identities. The native helper uses the cached player
        // view, so it cannot be used for either conversion in this mode.
        if (local && (!owner || owner == local))
            return;
    }
    hkFormatViewModelAttachment.fOriginal(owner, origin, inverse);
}

Hooks::~Hooks()
{
	if (m_MinHookInitialized) {
		if (MH_DisableHook(MH_ALL_HOOKS) != MH_OK)
			Logger::Write("Failed to disable all MinHook detours during teardown");
		if (MH_Uninitialize() != MH_OK)
			Logger::Write("Failed to uninitialize MinHook");
	}
}

bool Hooks::RollbackFailedInitialization()
{
	if (!m_MinHookInitialized) return true;
	const MH_STATUS disable = DisableUnpublishedHooks();
	// A callback may have entered its detour before the entries were disabled
	// and call fOriginal later. Uninitialize would free that trampoline. Keep
	// MinHook and the unpublished Game alive until process exit in dllmain.
	Logger::Write("MinHook startup rollback: disable=" + std::to_string(disable) +
		"; trampoline storage retained for in-flight callbacks");
	return disable == MH_OK;
}


int Hooks::initSourceHooks()
{
#define REQUIRE_HOOK(hook, target, detour) do { \
	LPVOID requiredTarget = (LPVOID)(target); \
	m_RequiredHooks.Add(#hook, [requiredTarget]() { return hook.createHook(requiredTarget, &detour); }, \
		[]() { return hook.enableHook(); }); \
} while (false)
#define REGISTER_IF_RESOLVED(hook, target, detour) do { \
	LPVOID resolvedTarget = (LPVOID)(target); \
	m_CompatibilityHooks.AddIfResolved(#hook, resolvedTarget, \
		[resolvedTarget]() { return hook.createHook(resolvedTarget, &detour); }, \
		[]() { return hook.enableHook(); }); \
} while (false)
	/*LPVOID pGetRenderTargetVFunc = (LPVOID)(m_Game->m_Offsets->GetRenderTarget.address);
	hkGetRenderTarget.createHook(pGetRenderTargetVFunc, &dGetRenderTarget);*/

	LPVOID pRenderViewVFunc = (LPVOID)(m_Game->m_Offsets->RenderView.address);
	REQUIRE_HOOK(hkRenderView, pRenderViewVFunc, dRenderView);

	LPVOID calcViewModelViewAddr = (LPVOID)(m_Game->m_Offsets->CalcViewModelView.address);
	REQUIRE_HOOK(hkCalcViewModelView, calcViewModelViewAddr, dCalcViewModelView);

	LPVOID ProcessUsercmdsAddr = (LPVOID)(m_Game->m_Offsets->ProcessUsercmds.address);
	REQUIRE_HOOK(hkProcessUsercmds, ProcessUsercmdsAddr, dProcessUsercmds);

	LPVOID ReadUserCmdAddr = (LPVOID)(m_Game->m_Offsets->ReadUserCmd.address);
	REQUIRE_HOOK(hkReadUsercmd, ReadUserCmdAddr, dReadUsercmd);

	/*LPVOID WriteUsercmdDeltaToBufferAddr = (LPVOID)(m_Game->m_Offsets->WriteUsercmdDeltaToBuffer.address);
	hkWriteUsercmdDeltaToBuffer.createHook(WriteUsercmdDeltaToBufferAddr, &dWriteUsercmdDeltaToBuffer);*/

	LPVOID WriteUsercmdAddr = (LPVOID)(m_Game->m_Offsets->WriteUsercmd.address);
	REQUIRE_HOOK(hkWriteUsercmd, WriteUsercmdAddr, dWriteUsercmd);

	/*LPVOID AdjustEngineViewportAddr = (LPVOID)(m_Game->m_Offsets->AdjustEngineViewport.address);
	hkAdjustEngineViewport.createHook(AdjustEngineViewportAddr, &dAdjustEngineViewport);

	LPVOID ViewportAddr = (LPVOID)(m_Game->m_Offsets->Viewport.address);
	hkViewport.createHook(ViewportAddr, &dViewport);

	LPVOID GetViewportAddr = (LPVOID)(m_Game->m_Offsets->GetViewport.address);
	hkGetViewport.createHook(GetViewportAddr, &dGetViewport);*/

	LPVOID EyePositionAddr = (LPVOID)(m_Game->m_Offsets->EyePosition.address);
	REGISTER_IF_RESOLVED(hkEyePosition, EyePositionAddr, dEyePosition);

	/*LPVOID DrawModelExecuteAddr = (LPVOID)(m_Game->m_Offsets->DrawModelExecute.address);
	hkDrawModelExecute.createHook(DrawModelExecuteAddr, &dDrawModelExecute);*/


	/*LPVOID IsSplitScreenAddr = (LPVOID)(m_Game->m_Offsets->IsSplitScreen.address);
	hkIsSplitScreen.createHook(IsSplitScreenAddr, &dIsSplitScreen);*/


	/*LPVOID GetFullScreenTextureAddr = (LPVOID)(m_Game->m_Offsets->GetFullScreenTexture.address);
	hkGetFullScreenTexture.createHook(GetFullScreenTextureAddr, &dGetFullScreenTexture);*/

	LPVOID Weapon_ShootPositionAddr = (LPVOID)(m_Game->m_Offsets->Weapon_ShootPosition.address);
	REGISTER_IF_RESOLVED(hkWeapon_ShootPosition, Weapon_ShootPositionAddr, dWeapon_ShootPosition);
	
	LPVOID TraceFirePortalAddr = (LPVOID)(m_Game->m_Offsets->TraceFirePortalServer.address);
	REQUIRE_HOOK(hkTraceFirePortal, TraceFirePortalAddr, dTraceFirePortal);

	REQUIRE_HOOK(hkCWeaponPortalgun_FirePortal, m_Game->m_Offsets->CWeaponPortalgun_FirePortal.address, dCWeaponPortalgun_FirePortal);

	LPVOID DrawSelfAddr = (LPVOID)(m_Game->m_Offsets->DrawSelf.address);
	REQUIRE_HOOK(hkDrawSelf, DrawSelfAddr, dDrawSelf);
	// Projection is called by our HUD hook, but does not need its own detour.
	ClipTransform = reinterpret_cast<tClipTransform>(m_Game->m_Offsets->ClipTransform.address);
	

	// Portalling
	LPVOID PlayerPortalledAddr = (LPVOID)(m_Game->m_Offsets->PlayerPortalled.address);
	REQUIRE_HOOK(hkPlayerPortalled, PlayerPortalledAddr, dPlayerPortalled);

	UTIL_Portal_FirstAlongRay = (tUTIL_Portal_FirstAlongRay)m_Game->m_Offsets->UTIL_Portal_FirstAlongRay.address;
	UTIL_IntersectRayWithPortal = (tUTIL_IntersectRayWithPortal)m_Game->m_Offsets->UTIL_IntersectRayWithPortal.address;
	UTIL_Portal_AngleTransform = (tUTIL_Portal_AngleTransform)m_Game->m_Offsets->UTIL_Portal_AngleTransform.address;

	LPVOID CreateMoveAddr = (LPVOID)(m_Game->m_Offsets->CreateMove.address);
	REQUIRE_HOOK(hkCreateMove, CreateMoveAddr, dCreateMove);

	// Grababbles
	REQUIRE_HOOK(hkUpdateObject, m_Game->m_Offsets->UpdateObject.address, dUpdateObject);
	REQUIRE_HOOK(hkUpdateObjectVM, m_Game->m_Offsets->UpdateObjectVM.address, dUpdateObjectVM);
	REQUIRE_HOOK(hkEyeAngles, m_Game->m_Offsets->EyeAngles.address, dEyeAngles);

	// Portal Gun VFX
	REQUIRE_HOOK(hkGetDefaultFOV, m_Game->m_Offsets->GetDefaultFOV.address, dGetDefaultFOV);
	REQUIRE_HOOK(hkGetFOV, m_Game->m_Offsets->GetFOV.address, dGetFOV);
	REGISTER_IF_RESOLVED(hkGetViewModelFOV, m_Game->m_Offsets->GetViewModelFOV.address, dGetViewModelFOV);
	
	// Laser Pointer
	GetPortalPlayer = (tGetPortalPlayer)m_Game->m_Offsets->GetPortalPlayer.address;
	CreatePingPointer = (tCreatePingPointer)m_Game->m_Offsets->CreatePingPointer.address;
	PrecacheParticleSystem = (tPrecacheParticleSystem)m_Game->m_Offsets->PrecacheParticleSystem.address;
	if (m_Game->m_Offsets->m_LaserAvailable &&
		hkPrecache.createHook((LPVOID)(m_Game->m_Offsets->Precache.address), &dPrecache)) {
		m_Game->m_Offsets->m_LaserAvailable = false;
		Logger::Write("Laser pointer disabled: Precache hook creation failed.");
	}
	REQUIRE_HOOK(hkSetDrawOnlyForSplitScreenUser, m_Game->m_Offsets->SetDrawOnlyForSplitScreenUser.address, dSetDrawOnlyForSplitScreenUser);
	REQUIRE_HOOK(hkCHudCrosshair_ShouldDraw, m_Game->m_Offsets->CHudCrosshair_ShouldDraw.address, dCHudCrosshair_ShouldDraw);

	//
	EntityIndex = (tEntindex)m_Game->m_Offsets->CBaseEntity_entindex.address;
	GetOwner = (tGetOwner)m_Game->m_Offsets->GetOwner.address;
	GetFullScreenTexture = (tGetFullScreenTexture)m_Game->m_Offsets->GetFullScreenTexture.address;
#undef REQUIRE_HOOK
#undef REGISTER_IF_RESOLVED
	return 0;
} 

bool __fastcall Hooks::dCHudCrosshair_ShouldDraw(void* ecx, void* edx) {
	bool shouldDraw = hkCHudCrosshair_ShouldDraw.fOriginal(ecx);
	if (Portal2VRRuntime::IsPublished(g_Game, m_Game) && m_VR->m_IsVREnabled &&
		m_Game->m_EngineClient->IsInGame() &&
		m_VR->m_RenderDiagnostics.First(shouldDraw ? RenderDiagnosticEvent::CrosshairShouldDrawTrue :
			RenderDiagnosticEvent::CrosshairShouldDrawFalse))
		Logger::Write(std::string("Crosshair ShouldDraw: Source returned ") +
			(shouldDraw ? "true" : "false") + " (actual pixels unverified)");

	// Keep the Source crosshair as a fallback until the optional laser is
	// confirmed visible by a real VR test. Symbol resolution alone is not proof.
	return shouldDraw;
}

void __fastcall Hooks::dPrecache(void* ecx, void* edx) {
	hkPrecache.fOriginal(ecx);
	if (!RuntimePublished()) return;
	PrecacheParticleSystem("robot_point_beam");
}

void __fastcall Hooks::dClientThink(void* ecx, void* edx) {
	hkClientThink.fOriginal(ecx);
}

void __fastcall Hooks::dSetDrawOnlyForSplitScreenUser(void* ecx, void* edx, int nSlot) {
	hkSetDrawOnlyForSplitScreenUser.fOriginal(ecx, RuntimePublished() ? -1 : nSlot);
}

ITexture *__fastcall Hooks::dGetFullScreenTexture()
{
	ITexture *result = hkGetFullScreenTexture.fOriginal();
	return result;
}

ITexture* __fastcall Hooks::dGetRenderTarget(void* ecx, void* edx)
{
	ITexture* result = hkGetRenderTarget.fOriginal(ecx);
	return result;
}

void __fastcall Hooks::dRenderView(void *ecx, void *edx, CViewSetup &setup, CViewSetup &hudViewSetup, int nClearFlags, int whatToDraw)
{
	// MinHook may dispatch this while Game::Initialize is still enabling hooks.
	if (!Portal2VRRuntime::IsPublished(g_Game, m_Game))
		return hkRenderView.fOriginal(ecx, setup, hudViewSetup, nClearFlags, whatToDraw);
	m_VR->ApplyPendingPortalOrientation(setup.origin);
    if (!m_VR->m_TrackingOutputValid) {
        if (m_VR->m_RenderDiagnostics.First(RenderDiagnosticEvent::TrackingBypass))
            Logger::Write("RenderView: stereo bypassed because tracking output is invalid");
        return hkRenderView.fOriginal(ecx, setup, hudViewSetup, nClearFlags, whatToDraw);
	}
	if (!m_VR->m_CreatedVRTextures) {
		m_VR->CreateVRTextures();
	}
	if (!m_VR->m_CreatedVRTextures) {
        if (m_VR->m_RenderDiagnostics.First(RenderDiagnosticEvent::RenderTargetBypass))
            Logger::Write("RenderView: stereo bypassed because VR render targets are unavailable");
        return hkRenderView.fOriginal(ecx, setup, hudViewSetup, nClearFlags, whatToDraw);
    }

	if (m_Game->m_VguiSurface->IsCursorVisible()) {
        if (m_VR->m_RenderDiagnostics.First(RenderDiagnosticEvent::CursorBypass))
            Logger::Write("RenderView: stereo bypassed while VGUI cursor is visible");
		return hkRenderView.fOriginal(ecx, setup, hudViewSetup, nClearFlags, whatToDraw);
	}

	//VPanel* g_pFullscreenRootPanel = *(VPanel**)(m_Game->m_Offsets->g_pFullscreenRootPanel.address);

	IMaterialSystem* matSystem = m_Game->m_MaterialSystem;

	hudViewSetup.width = m_VR->m_RenderWidth;
	hudViewSetup.height = m_VR->m_RenderHeight;
	hudViewSetup.fov = m_VR->m_Fov;
	//hudViewSetup.fovViewmodel = m_VR->m_Fov;
	hudViewSetup.m_flAspectRatio = m_VR->m_Aspect;

	hudViewSetup.m_nUnscaledWidth = m_VR->m_RenderWidth;
	hudViewSetup.m_nUnscaledHeight = m_VR->m_RenderHeight;

	Vector position = setup.origin;

    if (!m_VR->ExperimentalPortalOrientation() && m_VR->m_ApplyPortalRotationOffset) {
		Vector vec = position - m_VR->m_SetupOrigin;
		float distance = sqrt(vec.x * vec.x + vec.y * vec.y + vec.z * vec.z);

		// Rudimentary portalling detection
		if (distance > 35) {
			//m_VR->m_RotationOffset.x += m_VR->m_PortalRotationOffset.x;
			m_VR->m_RotationOffset.y += m_VR->m_PortalRotationOffset.y;
			//m_VR->m_RotationOffset.z += m_VR->m_PortalRotationOffset.z;

			m_VR->UpdateHMDAngles();

			m_VR->m_ApplyPortalRotationOffset = false;
		}
	}

	m_VR->m_SetupOrigin = position;
	m_VR->UpdateRoomscaleRenderAnchor(position);

	Vector hmdAngle = m_VR->GetViewAngle();
	QAngle inGameAngle(hmdAngle.x, hmdAngle.y, hmdAngle.z);
	m_Game->m_EngineClient->SetViewAngles(inGameAngle);

	float aspect = setup.m_flAspectRatio;

	setup.x = 0;
	setup.y = 0;
	setup.width = m_VR->m_RenderWidth;
	setup.height = m_VR->m_RenderHeight;
	setup.m_nUnscaledWidth = m_VR->m_RenderWidth;
	setup.m_nUnscaledHeight = m_VR->m_RenderHeight;
	setup.fov = m_VR->m_Fov;
	setup.fovViewmodel = m_VR->m_Fov;
	setup.m_flAspectRatio = m_VR->m_Aspect;
	setup.zNear = 6;
	setup.zNearViewmodel = 2;
	setup.angles = hmdAngle;

	CViewSetup leftEyeView = setup;
	CViewSetup rightEyeView = setup;

	int playerIndex = m_Game->m_EngineClient->GetLocalPlayer();
	C_BasePlayer* localPlayer = (C_BasePlayer*)m_Game->GetClientEntity(playerIndex);

	// Left eye CViewSetup
	QAngle tempAngle = QAngle(setup.angles.x, setup.angles.y, setup.angles.z);
	leftEyeView.origin = m_VR->TraceEye((uint32_t*)localPlayer, position, m_VR->GetViewOriginLeft(position), tempAngle);
	if (m_VR->ExperimentalPortalOrientation())
		leftEyeView.angles = Vector(tempAngle.x, tempAngle.y, tempAngle.z);
	else
		leftEyeView.angles.y = tempAngle.y;

	//std::cout << "dRenderView - Left Start\n";
	IMatRenderContext* rndrContext = matSystem->GetRenderContext();
	rndrContext->SetRenderTarget(m_VR->m_LeftEyeTexture);
	rndrContext->Release();
	const CViewSetup *previousAimEyeView = m_ActiveAimEyeView;
	const int previousAimEye = m_ActiveAimEye;
	const float previousReticleScale = m_ActiveReticleScale;
	// One distance/size for the entire stereo pair and all portal-status layers.
	const float reticleScale = NativeReticle::DistanceScale(m_VR->m_Config.reticleDistanceScaling,
		VectorLength(m_VR->m_AimPos - (position + m_VR->GetHmdViewOffset())), m_VR->m_VRScale);
	m_ActiveAimEyeView = &leftEyeView;
	m_ActiveAimEye = 1;
	m_ActiveReticleScale = reticleScale;
	hkRenderView.fOriginal(ecx, leftEyeView, hudViewSetup, nClearFlags, whatToDraw);
	m_ActiveAimEyeView = previousAimEyeView;
	m_ActiveAimEye = previousAimEye;
	m_ActiveReticleScale = previousReticleScale;
	
	// Right eye CViewSetup
	tempAngle = QAngle(setup.angles.x, setup.angles.y, setup.angles.z);
	rightEyeView.origin = m_VR->TraceEye((uint32_t*)localPlayer, position, m_VR->GetViewOriginRight(position), tempAngle);
	if (m_VR->ExperimentalPortalOrientation())
		rightEyeView.angles = Vector(tempAngle.x, tempAngle.y, tempAngle.z);
	else
		rightEyeView.angles.y = tempAngle.y;

	//std::cout << "dRenderView - Right Start\n";
	rndrContext = matSystem->GetRenderContext();
	rndrContext->SetRenderTarget(m_VR->m_RightEyeTexture);
	rndrContext->Release();
	m_ActiveAimEyeView = &rightEyeView;
	m_ActiveAimEye = 2;
	m_ActiveReticleScale = reticleScale;
	hkRenderView.fOriginal(ecx, rightEyeView, hudViewSetup, nClearFlags, whatToDraw);
	m_ActiveAimEyeView = previousAimEyeView;
	m_ActiveAimEye = previousAimEye;
	m_ActiveReticleScale = previousReticleScale;

	if (!previousAimEyeView && m_Game->m_Hooks->m_NativeBeamDiagnosticsReady) {
		if (const auto scopes = m_NativeBeamRenderProbe.StereoPairSummary())
			Logger::Write("Native beam DrawModel observation after 120 stereo pairs: leftScope=" +
				std::to_string((*scopes & 1) != 0) + " rightScope=" +
				std::to_string((*scopes & 2) != 0) + " outsideScope=" +
				std::to_string((*scopes & 4) != 0) + " (all zero means no draw call observed; not visibility proof)");
	}

	m_PushedHud = false;



	rndrContext = matSystem->GetRenderContext();
	rndrContext->SetRenderTarget(NULL);
	rndrContext->Release();

	/*rndrContext = matSystem->GetRenderContext();

	ITexture* fullscreenTxt = rndrContext->GetRenderTarget();

	Rect_t srcRect;
	srcRect.x = setup.x;
	srcRect.y = setup.y;
	srcRect.width = 1920;
	srcRect.height = 1080;

	rndrContext->SetRenderTarget(m_VR->m_RightEyeTexture);
	rndrContext->CopyRenderTargetToTextureEx(fullscreenTxt, 0, &srcRect, &srcRect);

	rndrContext->SetRenderTarget(NULL);
	rndrContext->Release();*/

	if (m_VR->m_RenderWindow) {
		setup.m_flAspectRatio = aspect;

		//setup.width, setup.height
		hkRenderView.fOriginal(ecx, setup, hudViewSetup, nClearFlags, whatToDraw);
	}


	m_VR->m_RenderedNewFrame = true;
    if (m_VR->m_RenderDiagnostics.First(RenderDiagnosticEvent::StereoRendered))
        Logger::Write("RenderView: first stereo pair rendered");
}

bool __fastcall Hooks::dCreateMove(void *ecx, void *edx, float flInputSampleTime, CUserCmd *cmd)
{
	if (!RuntimePublished()) return hkCreateMove.fOriginal(ecx, flInputSampleTime, cmd);
	if (!cmd->command_number)
		return hkCreateMove.fOriginal(ecx, flInputSampleTime, cmd);
	const int manualButtons = cmd->buttons & (IN_FORWARD | IN_BACK | IN_MOVELEFT | IN_MOVERIGHT | IN_JUMP);

	if (m_VR->m_IsVREnabled && m_VR->m_TrackingOutputValid)
	{
		cmd->viewangles = m_VR->m_HmdAngAbs;

		vr::InputAnalogActionData_t analogActionData;
		if (m_VR->GetAnalogActionData(m_VR->m_ActionWalk, analogActionData)) {
			// Run toward other guy
			cmd->buttons &= ~(IN_FORWARD | IN_BACK | IN_MOVELEFT | IN_MOVERIGHT);

			const auto movement = TrackingSpace::RebaseAnalogToView(
				analogActionData.x, analogActionData.y, m_VR->GetMovementForward(), m_VR->m_HmdForward);
			cmd->forwardmove += movement.forward * MAX_LINEAR_SPEED;
			cmd->sidemove += movement.side * MAX_LINEAR_SPEED;

			// We'll only be moving fwd or sideways
			cmd->upmove = 0.0f;

			if (cmd->forwardmove > 0.0f)
			{
				cmd->buttons |= IN_FORWARD;
			}
			else if (cmd->forwardmove < 0.0f)
			{
				cmd->buttons |= IN_BACK;
			}

			if (cmd->sidemove > 0.0f)
			{
				cmd->buttons |= IN_MOVELEFT;
			}
			else if (cmd->sidemove < 0.0f)
			{
				cmd->buttons |= IN_MOVERIGHT;
			}

		}

	}
	m_VR->ObserveRoomscaleCommand(cmd->command_number); // diagnostic only; never changes CUserCmd
	const bool manualMovement = RoomscaleMotion::HasManualInput(
		cmd->forwardmove, cmd->sidemove, cmd->upmove, manualButtons != 0);
	if (const auto movement = m_VR->GetRoomscaleCommand(cmd->command_number, manualMovement)) {
		cmd->forwardmove += movement->forward;
		cmd->sidemove += movement->side;
		cmd->buttons &= ~(IN_FORWARD | IN_BACK | IN_MOVELEFT | IN_MOVERIGHT);
		if (cmd->forwardmove > 0.0f) cmd->buttons |= IN_FORWARD;
		else if (cmd->forwardmove < 0.0f) cmd->buttons |= IN_BACK;
		// Source ComputeSideMove uses positive sidemove for +moveright.
		if (cmd->sidemove > 0.0f) cmd->buttons |= IN_MOVERIGHT;
		else if (cmd->sidemove < 0.0f) cmd->buttons |= IN_MOVELEFT;
	}

	return false;
}

void __fastcall Hooks::dEndFrame(void *ecx, void *edx)
{
	return hkEndFrame.fOriginal(ecx);
}

void __fastcall Hooks::dCalcViewModelView(void *ecx, void *edx, const Vector &eyePosition, const QAngle &eyeAngles)
{
	if (!RuntimePublished()) return hkCalcViewModelView.fOriginal(ecx, eyePosition, eyeAngles);
	Vector vecNewOrigin = eyePosition;
	QAngle vecNewAngles = eyeAngles;

	//std::cout << "dCalcViewModelView: (" << m_VR->m_IsVREnabled << ")\n";

	if (m_VR->m_IsVREnabled && m_VR->m_TrackingOutputValid && m_VR->m_RightControllerPose.valid)
	{
		vecNewOrigin = m_VR->GetRecommendedViewmodelAbsPos();
		vecNewAngles = m_VR->GetRecommendedViewmodelAbsAngle();
	}


    const bool previousControllerUpdate = m_ControllerViewmodelUpdate;
    m_ControllerViewmodelUpdate = CanAlignViewmodel() && ecx ==
        m_Game->GetClientEntity(m_Game->m_EngineClient->GetLocalPlayer());
    hkCalcViewModelView.fOriginal(ecx, vecNewOrigin, vecNewAngles);
    m_ControllerViewmodelUpdate = previousControllerUpdate;
}

float __fastcall Hooks::dProcessUsercmds(void *ecx, void *edx, edict_t *player, void *buf, int numcmds, int totalcmds, int dropped_packets, bool ignore, bool paused)
{
	if (!RuntimePublished())
		return hkProcessUsercmds.fOriginal(ecx, player, buf, numcmds, totalcmds, dropped_packets, ignore, paused);
	Server_BaseEntity *pPlayer = (Server_BaseEntity*)player->m_pUnk->GetBaseEntity();

	int index = EntityIndex(pPlayer);
	const int priorIndex = m_Game->m_CurrentUsercmdID;
	m_Game->m_CurrentUsercmdID = index;

	const auto result = hkProcessUsercmds.fOriginal(ecx, player, buf, numcmds, totalcmds, dropped_packets, ignore, paused);
	m_Game->m_CurrentUsercmdID = priorIndex;
	return result;
}

int Hooks::dWriteUsercmd(bf_write *buf, CUserCmd *to, CUserCmd *from)
{
	auto result =  hkWriteUsercmd.fOriginal(buf, to, from);
	if (!RuntimePublished()) return result;

	// Let's write our stuff into the buffer
	if (m_VR->m_IsVREnabled && m_VR->m_TrackingOutputValid && m_VR->m_RightControllerPose.valid)
	{
		Vector controllerPos = m_VR->GetRightControllerAbsPos();
		QAngle controllerAngles = m_VR->GetRightControllerAbsAngle();

		buf->WriteChar(-2);
		buf->WriteBitVec3Coord(controllerPos);
		buf->WriteBitAngles(controllerAngles);
	}

	return result;
}

int Hooks::dReadUsercmd(bf_read *buf, CUserCmd* move, CUserCmd* from)
{
	auto result = hkReadUsercmd.fOriginal(buf, move, from);
	if (!RuntimePublished()) return result;

	int i = m_Game->m_CurrentUsercmdID;
	if (!ValidPlayerSlot(m_Game, i)) return result;
	auto& vrPlayer = m_Game->m_PlayersVRInfo[i];

	auto pos = buf->Tell();
	int res = buf->ReadChar();

	// This means we got a VR player on the other side
	if (res == -2)
	{
		vrPlayer.isUsingVR = true;
		buf->ReadBitVec3Coord(vrPlayer.controllerPos);
		buf->ReadBitAngles(vrPlayer.controllerAngle);
	}
	else {
		vrPlayer.isUsingVR = false;
		buf->Seek(pos);
	}

	return result;
}


void Hooks::dAdjustEngineViewport(int &x, int &y, int &width, int &height)
{
	width = m_VR->m_RenderWidth;
	height = m_VR->m_RenderHeight;

	hkAdjustEngineViewport.fOriginal(x, y, width, height);
}

void Hooks::dGetViewport(void *ecx, void *edx, int &x, int &y, int &width, int &height)
{
	hkGetViewport.fOriginal(ecx, x, y, width, height);

	width = m_VR->m_RenderWidth;
	height = m_VR->m_RenderHeight;
}

int Hooks::dGetPrimaryAttackActivity(void *ecx, void *edx, void *meleeInfo)
{
	return hkGetPrimaryAttackActivity.fOriginal(ecx, meleeInfo);
}

Vector *Hooks::dEyePosition(void *ecx, void *edx, Vector *eyePos)
{
	Vector *result = hkEyePosition.fOriginal(ecx, eyePos);
	return result;
}

// We'll keep this for... future reference!
void Hooks::dDrawModelExecute(void *ecx, void *edx, void *state, const ModelRenderInfo_t &info, void *pCustomBoneToWorld)
{
	if (info.pModel)
	{
		std::string modelName = m_Game->m_ModelInfo->GetModelName(info.pModel);
		if (modelName.find("/arms/") != std::string::npos)
		{
			m_Game->m_ArmsMaterial = m_Game->m_MaterialSystem->FindMaterial(modelName.c_str(), "Model textures");
			m_Game->m_ArmsModel = info.pModel;
			m_Game->m_CachedArmsModel = true;
		}
	}

	if (info.pModel && info.pModel == m_Game->m_ArmsModel)
	{
		m_Game->m_ArmsMaterial->SetMaterialVarFlag(MATERIAL_VAR_NO_DRAW, true);
		m_Game->m_ModelRender->ForcedMaterialOverride(m_Game->m_ArmsMaterial);
		hkDrawModelExecute.fOriginal(ecx, state, info, pCustomBoneToWorld);
		m_Game->m_ModelRender->ForcedMaterialOverride(NULL);
		return;
	}

	hkDrawModelExecute.fOriginal(ecx, state, info, pCustomBoneToWorld);
}

void Hooks::dPushRenderTargetAndViewport(void *ecx, void *edx, ITexture *pTexture, ITexture *pDepthTexture, int nViewX, int nViewY, int nViewW, int nViewH)
{
	if (!RuntimePublished())
		return hkPushRenderTargetAndViewport.fOriginal(ecx, pTexture, pDepthTexture, nViewX, nViewY, nViewW, nViewH);
	const bool inPaint = m_VguiPaintActive;
	const bool published = Portal2VRRuntime::IsPublished(g_Game, m_Game);
	const bool redirect = published && m_HudCaptureRoute.AllowsRedirect() &&
		HudCapture::ShouldRedirectTarget(m_VR->m_Config.experimentalHudOverlay,
			m_Game->m_Hooks->m_HudCaptureHooksReady, m_VR->m_CreatedVRTextures,
			inPaint, m_Game->m_VguiSurface->IsCursorVisible(), m_PushedHud) &&
		m_VR->m_HUDTexture && m_VR->m_VKHUD.m_VRTexture.handle;
	if (inPaint) {
		++m_HudPushDepth;
		m_HudPushSeenDuringPaint = true;
	}
	if (published && m_VR->m_Config.experimentalHudOverlay &&
		m_Game->m_EngineClient->IsInGame()) {
		const auto event = inPaint ? RenderDiagnosticEvent::HudPushInPaint :
			RenderDiagnosticEvent::HudPushOutsidePaint;
		if (m_VR->m_RenderDiagnostics.First(event))
			Logger::Write(std::string("Experimental HUD render-target push: inEligiblePaint=") +
				std::to_string(inPaint) + " redirect=" + std::to_string(redirect) +
				" alreadyRedirected=" + std::to_string(m_PushedHud) +
				" hudTargetReady=" + std::to_string(m_VR->m_HUDTexture != nullptr &&
					m_VR->m_VKHUD.m_VRTexture.handle != nullptr) +
				" sourceTarget=" + std::to_string(pTexture != nullptr) +
				" viewport=" + std::to_string(nViewW) + "x" + std::to_string(nViewH));
	}
	if (redirect)
	{
		m_HudRedirectSeenDuringPaint = true;
		if (m_VR->m_RenderDiagnostics.First(RenderDiagnosticEvent::HudRedirected))
			Logger::Write("Experimental HUD render-target redirect reached (painted pixels unverified)");
		pTexture = m_VR->m_HUDTexture;
		hkPushRenderTargetAndViewport.fOriginal(ecx, pTexture, pDepthTexture, nViewX, nViewY, nViewW, nViewH);
		IMatRenderContext *renderContext = m_Game->m_MaterialSystem->GetRenderContext();
		renderContext->OverrideAlphaWriteEnable(true, true);
		renderContext->ClearColor4ub(0, 0, 0, 0);
		renderContext->ClearBuffers(true, false);
		renderContext->Release();

		m_VR->m_RenderedHud = true;
		m_PushedHud = true;
		m_HudTargetActive = true;
	}
	else
	{
		hkPushRenderTargetAndViewport.fOriginal(ecx, pTexture, pDepthTexture, nViewX, nViewY, nViewW, nViewH);
	}
}

void Hooks::dPopRenderTargetAndViewport(void *ecx, void *edx)
{
	if (!RuntimePublished()) return hkPopRenderTargetAndViewport.fOriginal(ecx);
	if (m_VguiPaintActive && !HudCapture::ShouldForwardPaintPop(
		m_ExplicitHudCaptureActive, m_HudPushDepth)) {
		m_HudUnexpectedPopDuringPaint = true;
		return;
	}
	if (m_VguiPaintActive && m_HudPushDepth > 0)
		--m_HudPushDepth;
	if (m_HudTargetActive && m_HudPushDepth == 0)
	{
		IMatRenderContext* renderContext = m_Game->m_MaterialSystem->GetRenderContext();
		renderContext->OverrideAlphaWriteEnable(false, false);
		renderContext->ClearColor4ub(0, 0, 0, 255);
		renderContext->Release();
		m_HudTargetActive = false;
	}

	hkPopRenderTargetAndViewport.fOriginal(ecx);
}

void Hooks::dVGui_Paint(void *ecx, void *edx, int mode)
{
    if (RuntimePublished() && m_Game->m_Hooks->m_EyeHudHookReady &&
        m_VR->m_Config.hudInEyeCentered && !m_VR->m_Config.experimentalHudOverlay &&
        m_VR->m_AimMode != 2 && EyeHud::IsGameplayOnlyPaint(mode, PAINT_INGAMEPANELS) && m_ActiveAimEyeView &&
        m_VR->m_IsVREnabled && m_VR->m_TrackingOutputValid && m_VR->m_CreatedVRTextures &&
        m_Game->m_EngineClient->IsInGame() && !m_Game->m_VguiSurface->IsCursorVisible()) {
        const auto release = [](IMatRenderContext *context) { if (context) context->Release(); };
        std::unique_ptr<IMatRenderContext, decltype(release)> context(
            m_Game->m_MaterialSystem->GetRenderContext(), release);
        if (context && CheckRenderContextAbi(context.get()) != Portal2MaterialAbi::Kind::Unsupported) {
            ITexture *eyeTarget = m_ActiveAimEye == 1 ? m_VR->m_LeftEyeTexture :
                m_ActiveAimEye == 2 ? m_VR->m_RightEyeTexture : nullptr;
            const EyeHud::GameplayScope scope{true, true, true, true, true, false,
                true, eyeTarget && context->GetRenderTarget() == eyeTarget, true};
            if (EyeHud::CanCenter(scope, m_VR->m_Config.hudInEyeCentered,
                    m_VR->m_Config.experimentalHudOverlay, m_VR->m_AimMode)) {
                int windowWidth = 0, windowHeight = 0;
                context->GetWindowSize(windowWidth, windowHeight);
                const auto &eye = *m_ActiveAimEyeView;
                const auto rect = EyeHud::CenteredRect(eye.width, eye.height, windowWidth, windowHeight,
                    m_VR->m_Config.hudInEyeScale, m_VR->m_Config.hudInEyeVerticalOffset);
                EyeHud::Rect previous{};
                context->GetViewport(previous.x, previous.y, previous.width, previous.height);
                if (rect && previous.width > 0 && previous.height > 0) {
                    // VGUI initializes its orthographic canvas from this viewport.
                    // Never rebind the eye target or alter its depth attachment.
                    EyeHud::ViewportScope<IMatRenderContext> viewport(*context, previous, *rect);
                    return hkVgui_Paint.fOriginal(ecx, mode);
                }
            }
        }
    }
	if (!Portal2VRRuntime::IsPublished(g_Game, m_Game) ||
		!m_VR->m_Config.experimentalHudOverlay)
		return hkVgui_Paint.fOriginal(ecx, mode);

	const bool inGame = m_Game->m_EngineClient->IsInGame();
	const bool cursorVisible = m_Game->m_VguiSurface->IsCursorVisible();
	const bool targetReady = m_VR->m_HUDTexture && m_VR->m_VKHUD.m_VRTexture.handle;
	const bool capture = HudCapture::CanCapturePaint(true, m_Game->m_Hooks->m_HudCaptureHooksReady,
		m_VR->m_CreatedVRTextures, targetReady, m_VR->m_RenderedNewFrame,
		inGame, cursorVisible);
	auto logPaintState = [&](const char *phase) {
		Logger::Write(std::string("Experimental HUD VGui_Paint ") + phase +
			": mode=" + std::to_string(mode) + " inGame=" + std::to_string(inGame) +
			" cursor=" + std::to_string(cursorVisible) +
			" hooks=" + std::to_string(m_Game->m_Hooks->m_HudCaptureHooksReady) +
			" textures=" + std::to_string(m_VR->m_CreatedVRTextures) +
			" target=" + std::to_string(targetReady) +
			" stereoFrame=" + std::to_string(m_VR->m_RenderedNewFrame));
	};
	if (m_VR->m_RenderDiagnostics.First(RenderDiagnosticEvent::HudPaintEntered))
		logPaintState("first call");
	if (inGame && m_VR->m_RenderDiagnostics.First(RenderDiagnosticEvent::HudPaintInGame))
		logPaintState("first in-game call");
	if (!capture)
		return hkVgui_Paint.fOriginal(ecx, mode);
	if (m_VR->m_RenderDiagnostics.First(RenderDiagnosticEvent::HudPaintEligible))
		logPaintState("first eligible call");
	const bool explicitCapture = m_HudCaptureRoute.ShouldCaptureExplicitly(
		capture, (mode & PAINT_UIPANELS) != 0, m_VR->m_RenderedHud);
	IMatRenderContext *captureContext = nullptr;
	if (explicitCapture) {
		captureContext = m_Game->m_MaterialSystem->GetRenderContext();
		if (!captureContext) {
			if (m_VR->m_RenderDiagnostics.First(RenderDiagnosticEvent::HudExplicitContextUnavailable))
				Logger::Write("Experimental HUD: explicit capture skipped; render context unavailable");
			return hkVgui_Paint.fOriginal(ecx, mode | PAINT_UIPANELS | PAINT_INGAMEPANELS);
		}
	}

	m_HudPushSeenDuringPaint = false;
	m_HudRedirectSeenDuringPaint = false;
	m_HudUnexpectedPopDuringPaint = false;
	m_ExplicitHudCaptureActive = captureContext != nullptr;
	m_VguiPaintActive = true;
	if (captureContext) {
		// The observed post-stereo UI paint has no nested target push. Bracket
		// that paint using the already-resolved six-argument Source ABI.
		hkPushRenderTargetAndViewport.fOriginal(captureContext, m_VR->m_HUDTexture,
			nullptr, 0, 0, m_VR->m_RenderWidth, m_VR->m_RenderHeight);
		captureContext->OverrideAlphaWriteEnable(true, true);
		captureContext->ClearColor4ub(0, 0, 0, 0);
		captureContext->ClearBuffers(true, false);
	}
	mode |= PAINT_UIPANELS | PAINT_INGAMEPANELS;
	hkVgui_Paint.fOriginal(ecx, mode);
	if (captureContext) {
		const unsigned unmatchedNestedPushes = m_HudPushDepth;
		HudCapture::UnwindNestedTargets(m_HudPushDepth, [&] {
			hkPopRenderTargetAndViewport.fOriginal(captureContext);
		});
		captureContext->OverrideAlphaWriteEnable(false, false);
		captureContext->ClearColor4ub(0, 0, 0, 255);
		hkPopRenderTargetAndViewport.fOriginal(captureContext);
		m_ExplicitHudCaptureActive = false;
		captureContext->Release();
		if (m_HudCaptureRoute.ObserveExplicitPaint(
			m_HudPushSeenDuringPaint || m_HudUnexpectedPopDuringPaint)) {
			m_VR->m_RenderedHud = false;
			if (m_VR->m_RenderDiagnostics.First(RenderDiagnosticEvent::HudExplicitUnexpectedPush))
				Logger::Write("Experimental HUD: unexpected target stack operation in explicit paint; capture disabled until restart (unmatched pushes unwound=" +
					std::to_string(unmatchedNestedPushes) + ", unmatched pop blocked=" +
					std::to_string(m_HudUnexpectedPopDuringPaint) + ")");
		} else {
			m_VR->m_RenderedHud = true;
			if (m_VR->m_RenderDiagnostics.First(RenderDiagnosticEvent::HudExplicitCapture))
				Logger::Write("Experimental HUD: explicit UI paint captured to vrHUD (pixels/alpha unverified)");
		}
	}
	m_VguiPaintActive = false;
	if (!captureContext && !m_HudRedirectSeenDuringPaint &&
		m_VR->m_RenderDiagnostics.First(RenderDiagnosticEvent::HudPaintNoRedirect))
		Logger::Write("Experimental HUD: eligible VGui_Paint returned without redirect; "
			"render-target push during paint=" + std::to_string(m_HudPushSeenDuringPaint));
	if (!captureContext && m_HudCaptureRoute.ObserveRedirectPaint(
		m_HudPushSeenDuringPaint, m_HudRedirectSeenDuringPaint))
		Logger::Write("Experimental HUD: no nested VGUI target push; explicit UI capture armed");
	if (m_HudPushDepth || m_HudTargetActive) {
		if (m_HudTargetActive) {
			IMatRenderContext* context = m_Game->m_MaterialSystem->GetRenderContext();
			context->OverrideAlphaWriteEnable(false, false);
			context->ClearColor4ub(0, 0, 0, 255);
			context->Release();
		}
		Logger::Write("Experimental HUD: VGUI render target stack was unbalanced; capture disabled until restart");
		m_Game->m_Hooks->m_HudCaptureHooksReady = false;
		m_HudPushDepth = 0;
		m_HudTargetActive = false;
		m_PushedHud = false;
		m_VR->m_RenderedHud = false;
	}
}

int Hooks::dIsSplitScreen()
{
	//std::cout << "dIsSplitScreen: " << m_PushHUDStep << "\n";

	if (m_PushHUDStep == 0)
		++m_PushHUDStep;
	else
		m_PushHUDStep = -999;

	return hkIsSplitScreen.fOriginal();
}

DWORD *Hooks::dPrePushRenderTarget(void *ecx, void *edx, int a2)
{
	//std::cout << "dPrePushRenderTarget: " << m_PushHUDStep << "\n";

	if (m_PushHUDStep == 1)
		++m_PushHUDStep;
	else
		m_PushHUDStep = -999;

	return hkPrePushRenderTarget.fOriginal(ecx, a2);
}

Vector* __fastcall Hooks::dWeapon_ShootPosition(void* ecx, void* edx, Vector* eyePos)
{
	// This address is also used by another virtual method. Only the audited
	// FirePortal call is allowed to replace its hidden Vector return value.
	const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
	Vector* result = hkWeapon_ShootPosition.fOriginal(ecx, eyePos);
	if (!RuntimePublished() || !result || !CompatibilityHooks::IsVerifiedShootCaller(
		caller, m_Game->m_Offsets->Weapon_ShootPosition.shootCaller)) return result;

	int localIndex = m_Game->m_EngineClient->GetLocalPlayer();
	int index = EntityIndex(ecx);
	if (!ValidPlayerSlot(m_Game, index)) return result;

	auto vrPlayer = m_Game->m_PlayersVRInfo[index];

	if (m_VR->m_IsVREnabled && m_VR->m_TrackingOutputValid && m_VR->m_RightControllerPose.valid && localIndex == index) {
		*result = m_VR->GetRightControllerAbsPos();	
	}
	else if (vrPlayer.isUsingVR)
	{
		*result = vrPlayer.controllerPos;
	}

	return result;
}

void* Hooks::dCWeaponPortalgun_FirePortal(void* ecx, void* edx, bool bPortal2, Vector* pVector) {
	if (!RuntimePublished()) return hkCWeaponPortalgun_FirePortal.fOriginal(ecx, bPortal2, pVector);
	bool wasTrue = m_VR->m_OverrideEyeAngles;
	const int localIndex = m_Game->m_EngineClient ?
		m_Game->m_EngineClient->GetLocalPlayer() : -1;
	void *owner = GetOwner(ecx);
	const bool localShot = Haptics::IsLocalShot(localIndex, owner ? EntityIndex(owner) : -1);

	m_VR->m_OverrideEyeAngles = true;

	auto result = hkCWeaponPortalgun_FirePortal.fOriginal(ecx, bPortal2, pVector);
	// This is a candidate fire event, not proof of successful portal placement.
	// Queue on the local weapon owner only; VR API calls stay on the update thread.
	if (localShot)
		m_VR->QueuePortalShotHaptic();

	if (!wasTrue)
		m_VR->m_OverrideEyeAngles = false;

	return result;
}

bool __fastcall Hooks::dTraceFirePortal(void* ecx, void* edx, const Vector& vTraceStart, const Vector& vDirection, bool bPortal2, int iPlacedBy, void* tr) //trace_tx& tr, Vector& vFinalPosition //  , Vector& vFinalPosition, QAngle& qFinalAngles, int iPlacedBy, bool bTest /*= false*/
{
	if (!RuntimePublished())
		return hkTraceFirePortal.fOriginal(ecx, vTraceStart, vDirection, bPortal2, iPlacedBy, tr);
	Vector vNewTraceStart = vTraceStart;
	Vector vNewDirection = vDirection;

	if (iPlacedBy == 2) {
		int localIndex = m_Game->m_EngineClient->GetLocalPlayer();

		auto owner = GetOwner(ecx);

		if (owner) {
			int index = EntityIndex(owner);
			if (!ValidPlayerSlot(m_Game, index))
				return hkTraceFirePortal.fOriginal(ecx, vTraceStart, vDirection, bPortal2, iPlacedBy, tr);

			auto vrPlayer = m_Game->m_PlayersVRInfo[index];

			if (m_VR->m_IsVREnabled && m_VR->m_TrackingOutputValid && m_VR->m_RightControllerPose.valid && localIndex == index) {
				vNewTraceStart = m_VR->GetRightControllerAbsPos();
				vNewDirection = m_VR->m_RightControllerForward;
			}
			else if (vrPlayer.isUsingVR)
			{
				vNewTraceStart = vrPlayer.controllerPos;
				Vector fwd, rt, up;
				QAngle::AngleVectors(vrPlayer.controllerAngle, &fwd, &rt, &up);
				vNewDirection = fwd;
			}
		}
	}

	return hkTraceFirePortal.fOriginal(ecx, vNewTraceStart, vNewDirection, bPortal2, iPlacedBy, tr);
}

void __fastcall Hooks::dPlayerPortalled(void* ecx, void* edx, void* a2, __int64 a3)
{
	if (!RuntimePublished()) return hkPlayerPortalled.fOriginal(ecx, a2, a3);
	CBaseEntity* pBaseEntity = (CBaseEntity*)ecx;
	const int playerIndex = m_Game->m_EngineClient->GetLocalPlayer();
	const bool localPlayer = playerIndex > 0 &&
		m_Game->GetClientEntity(playerIndex) == pBaseEntity;
	const bool experimental = m_VR->ExperimentalPortalOrientation();
	const auto portalRotation = experimental && localPlayer ?
		ReadPortalRotation(a2) : std::nullopt;

	QAngle angAbsRotationBefore;
	m_Game->m_EngineClient->GetViewAngles(angAbsRotationBefore);

	hkPlayerPortalled.fOriginal(ecx, a2, a3);
	if (localPlayer)
		m_VR->ResetRoomscale();

	QAngle angAbsRotationAfter;
	m_Game->m_EngineClient->GetViewAngles(angAbsRotationAfter);

	if (experimental && localPlayer) {
		m_VR->QueuePortalTraversal(reinterpret_cast<std::uintptr_t>(pBaseEntity),
			reinterpret_cast<std::uintptr_t>(a2), portalRotation);
	} else if (!experimental && angAbsRotationBefore != angAbsRotationAfter) {
		m_VR->m_PortalRotationOffset = angAbsRotationAfter - angAbsRotationBefore;
		m_VR->m_ApplyPortalRotationOffset = true;
	}

	return;
}

int Hooks::dGetModeHeight(void* ecx, void* edx) {
	//std::cout << "dGetModeHeight\n";
	return m_VR->m_RenderHeight;
}

bool Hooks::ScreenTransform(const Vector& point, Vector* pScreen, int width, int height)
{
	bool retval = ClipTransform(point, pScreen);

	pScreen->x = 0.5f * (pScreen->x + 1.0f) * width;
	pScreen->y = 0.5f * (-pScreen->y + 1.0f) * height;

	return retval;
}

int __fastcall Hooks::dDrawSelf(void* ecx, void* edx, int x, int y, int w, int h, const void* clr, float flApparentZ) {
	if (!RuntimePublished()) return hkDrawSelf.fOriginal(ecx, x, y, w, h, clr, flApparentZ);
    if (!m_VR->m_Config.showSourceCrosshair && (m_VR->m_AimMode == 0 || m_VR->m_AimMode == 1) &&
        m_ActiveAimEyeView && m_VR->m_IsVREnabled && m_VR->m_TrackingOutputValid &&
        m_VR->m_CreatedVRTextures && m_Game->m_EngineClient->IsInGame() &&
        !m_Game->m_VguiSurface->IsCursorVisible() && !m_ExplicitHudCaptureActive && !m_HudTargetActive) {
        // Keep Source ShouldDraw and every Portal status sprite. This option
        // controls only recognized generic flat HUD artwork in legacy eyes.
        const auto identity = ReadSourceHudTextureIdentity(ecx);
        if (identity) {
            IMatRenderContext *context = m_Game->m_MaterialSystem->GetRenderContext();
            if (context) {
                const bool verified = CheckRenderContextAbi(context) != Portal2MaterialAbi::Kind::Unsupported;
                ITexture *eyeTarget = m_ActiveAimEye == 1 ? m_VR->m_LeftEyeTexture :
                    m_ActiveAimEye == 2 ? m_VR->m_RightEyeTexture : nullptr;
                const bool matches = verified && eyeTarget && context->GetRenderTarget() == eyeTarget;
                context->Release();
                const EyeHud::GameplayScope scope{true, true, true, true, true, false, true, matches, verified};
                if (EyeHud::ShouldHideFlatCrosshair(scope, false, m_VR->m_AimMode,
                        identity->shortName.data(), identity->textureFile.data()))
                    return 0;
            }
        }
    }
	//std::cout << "dDrawSelf - X: " << x << ", Y: " << y << ", W: " << w << ", H: " << h << ", Z: " << flApparentZ << "\n";

	//int playerIndex = m_Game->m_EngineClient->GetLocalPlayer();

	//auto viewport = m_Game->m_ClientMode->GetViewport();

	int newX = x;
	int	newY = y;

	if (m_VR->m_IsVREnabled && m_VR->m_TrackingOutputValid &&
		m_VR->m_RightControllerPose.valid && m_VR->m_AimMode == 2 &&
		m_Game->m_EngineClient->IsInGame() && !m_Game->m_VguiSurface->IsCursorVisible())
	{
		int windowWidth, windowHeight;
		IMatRenderContext* context = m_Game->m_MaterialSystem->GetRenderContext();
		if (!context)
			return hkDrawSelf.fOriginal(ecx, x, y, w, h, clr, flApparentZ);
		if (CheckRenderContextAbi(context) == Portal2MaterialAbi::Kind::Unsupported) {
			context->Release();
			return hkDrawSelf.fOriginal(ecx, x, y, w, h, clr, flApparentZ);
		}
		context->GetWindowSize(windowWidth, windowHeight);
		int viewportX = 0, viewportY = 0, viewportWidth = 0, viewportHeight = 0;
		context->GetViewport(viewportX, viewportY, viewportWidth, viewportHeight);
		ITexture *currentTarget = context->GetRenderTarget();
		context->Release();
		const auto sourceIdentity = ReadSourceHudTextureIdentity(ecx);
		if (!sourceIdentity || !AimFeedback::IsReticleIconName(
			sourceIdentity->shortName.data(), sourceIdentity->textureFile.data()))
			return hkDrawSelf.fOriginal(ecx, x, y, w, h, clr, flApparentZ);
		// Captions are a separate HMD overlay, not the reticle's world-hit plane.
		// Do not duplicate portal icons into that crop after drawing them per eye.
		if (m_ExplicitHudCaptureActive || m_HudTargetActive) {
			if (m_VR->m_RenderDiagnostics.First(RenderDiagnosticEvent::NativeReticleCaptureSuppressed))
				Logger::Write("Portal reticle excluded from caption overlay; native status is drawn per eye");
			return 0;
		}
		if (!m_ActiveAimEyeView)
			return hkDrawSelf.fOriginal(ecx, x, y, w, h, clr, flApparentZ);

		if (m_VR->m_Config.experimentalStereoReticle) {
			// Source selects the actual portal status icon and color. Draw the same
			// atlas subrectangle into the active eye only when its engine ABI matches.
			if (!AimFeedback::IsCenteredReticleSprite(x, y, w, h, windowWidth, windowHeight) &&
				!AimFeedback::IsCenteredReticleSprite(x, y, w, h,
					m_ActiveAimEyeView->width, m_ActiveAimEyeView->height))
				return hkDrawSelf.fOriginal(ecx, x, y, w, h, clr, flApparentZ);
			const auto identity = ReadSourceHudTextureIdentity(ecx);
			const bool isReticle = identity && AimFeedback::IsReticleIconName(
				identity->shortName.data(), identity->textureFile.data());
			if (!isReticle) {
				if (m_VR->m_RenderDiagnostics.First(
					RenderDiagnosticEvent::CrosshairWorldUnknownIcon)) {
					Logger::Write(std::string("Experimental stereo reticle: centered Source icon left unchanged, ") +
						(identity ? DescribeHudTexture(*identity) : "identity unreadable"));
				}
				return hkDrawSelf.fOriginal(ecx, x, y, w, h, clr, flApparentZ);
			}
			const bool leftPortalIcon = AimFeedback::ContainsAsciiInsensitive(
				identity->shortName.data(), "portal_crosshair_left");
			const bool rightPortalIcon = AimFeedback::ContainsAsciiInsensitive(
				identity->shortName.data(), "portal_crosshair_right");
			const bool invalidPortalIcon = AimFeedback::ContainsAsciiInsensitive(
				identity->shortName.data(), "_invalid");
			const auto icon = leftPortalIcon ?
				(invalidPortalIcon ? ReticleTelemetry::Icon::LeftInvalid :
				 ReticleTelemetry::Icon::LeftValid) :
				rightPortalIcon ?
				(invalidPortalIcon ? ReticleTelemetry::Icon::RightInvalid :
				 ReticleTelemetry::Icon::RightValid) : ReticleTelemetry::Icon::Other;
			Vector forward, right, up;
			const Vector &viewAngles = m_ActiveAimEyeView->angles;
			const QAngle eyeAngles(viewAngles.x, viewAngles.y, viewAngles.z);
			QAngle::AngleVectors(eyeAngles, &forward, &right, &up);
			const auto projected = AimFeedback::ProjectReticleSpriteToEye(
				m_VR->m_AimPos, m_ActiveAimEyeView->origin, forward, right, up,
				m_ActiveAimEyeView->fov, m_ActiveAimEyeView->m_flAspectRatio,
				m_ActiveAimEyeView->width, m_ActiveAimEyeView->height,
				x, y, w, h, windowWidth, windowHeight);
			if (!projected) {
                RecordReticleDraw(m_VR->m_Config.verboseDiagnostics, m_ActiveAimEye, icon, -1,
					ReticleTelemetry::Result::ProjectedOutside);
				if (m_VR->m_RenderDiagnostics.First(
					RenderDiagnosticEvent::CrosshairWorldProjectionSkipped))
					Logger::Write("Experimental stereo reticle: center sprite outside eye viewport; skipped");
				return 0;
			}
			const auto atlas = ReadSourceHudTextureAtlas(ecx);
			ReticleMaterialDrawResult result = ReticleMaterialDrawResult::AtlasUnavailable;
			int sourceAlpha = -1;
			if (atlas) {
				ITexture *eyeTarget = m_ActiveAimEye == 1 ?
					m_VR->m_LeftEyeTexture : m_VR->m_RightEyeTexture;
				result = DrawSourceReticleInEye(m_Game, *identity, *atlas, eyeTarget,
					*projected, w, h, m_ActiveAimEyeView->width,
					m_ActiveAimEyeView->height, clr, &sourceAlpha);
			}
            RecordReticleDraw(m_VR->m_Config.verboseDiagnostics, m_ActiveAimEye, icon, sourceAlpha,
				result == ReticleMaterialDrawResult::Drawn ?
					ReticleTelemetry::Result::Drawn : ReticleTelemetry::Result::Failed);
			const auto event = m_ActiveAimEye == 1 ?
				RenderDiagnosticEvent::CrosshairWorldLeftEye :
				RenderDiagnosticEvent::CrosshairWorldRightEye;
			if (result == ReticleMaterialDrawResult::Drawn) {
				if (m_VR->m_RenderDiagnostics.First(event))
					Logger::Write(std::string("Experimental stereo reticle: original Source atlas submitted in ") +
						(m_ActiveAimEye == 1 ? "left" : "right") + " eye at " +
						std::to_string(projected->x) + "," + std::to_string(projected->y) +
						" " + DescribeHudTexture(*identity) +
						"; actual VR visibility remains unverified");
				return 0;
			}
			RenderDiagnosticEvent failureEvent = RenderDiagnosticEvent::CrosshairDirectAtlasUnavailable;
			switch (result) {
			case ReticleMaterialDrawResult::MaterialUnavailable:
				failureEvent = RenderDiagnosticEvent::CrosshairDirectMaterialUnavailable;
				break;
			case ReticleMaterialDrawResult::EyeTargetUnavailable:
				failureEvent = RenderDiagnosticEvent::CrosshairDirectEyeTargetUnavailable;
				break;
			case ReticleMaterialDrawResult::ViewportUnavailable:
				failureEvent = RenderDiagnosticEvent::CrosshairDirectViewportUnavailable;
				break;
			case ReticleMaterialDrawResult::ColorUnavailable:
				failureEvent = RenderDiagnosticEvent::CrosshairDirectColorUnavailable;
				break;
			case ReticleMaterialDrawResult::AbiUnsupported:
				failureEvent = RenderDiagnosticEvent::CrosshairDirectAbiUnsupported;
				break;
			default:
				break;
			}
			if (m_VR->m_RenderDiagnostics.First(failureEvent))
				Logger::Write("Experimental stereo reticle: direct Source atlas draw unavailable (" +
					std::string(ReticleDrawFailureName(result)) +
					"); native desktop draw retained; " + DescribeHudTexture(*identity));
			return hkDrawSelf.fOriginal(ecx, x, y, w, h, clr, flApparentZ);
		}

		// Native VGUI StartDrawing uses the active eye's ortho/viewport, but
		// PushMakeCurrent still clips to the cached desktop HUD panel. Use the
		// actual eye view, not engine ClipTransform's cached desktop/HUD view.
		Vector forward, right, up;
		const auto &eye = *m_ActiveAimEyeView;
		QAngle::AngleVectors(QAngle(eye.angles.x, eye.angles.y, eye.angles.z), &forward, &right, &up);
		const auto projected = AimFeedback::ProjectReticleSpriteToEye(
			m_VR->m_AimPos, eye.origin, forward, right, up, eye.fov, eye.m_flAspectRatio,
			eye.width, eye.height, x, y, w, h, windowWidth, windowHeight);
		if (!projected)
			return 0;
		const auto hit = AimFeedback::ProjectWorldToEye(m_VR->m_AimPos, eye.origin, forward, right, up,
			eye.fov, eye.m_flAspectRatio, eye.width, eye.height);
		const auto layout = hit ? NativeReticle::ScaleLayout(*projected, *hit, w, h,
			m_ActiveReticleScale) : std::nullopt;
		if (!layout)
			return 0;
		auto surface = PrepareNativeReticleSurface(m_Game->m_VguiSurface);
		const auto position = surface ? NativeReticle::SurfacePosition(layout->position,
			surface->translation[0], surface->translation[1]) : std::nullopt;
		ITexture *eyeTarget = m_ActiveAimEye == 1 ? m_VR->m_LeftEyeTexture : m_VR->m_RightEyeTexture;
		if (!position || currentTarget != eyeTarget || viewportX != 0 || viewportY != 0 ||
			viewportWidth != eye.width || viewportHeight != eye.height) {
			if (m_VR->m_RenderDiagnostics.First(RenderDiagnosticEvent::NativeReticleUnavailable))
				Logger::Write("Native Portal reticle correction unavailable: surface ABI or active eye viewport mismatch; native draw retained");
			return hkDrawSelf.fOriginal(ecx, x, y, w, h, clr, flApparentZ);
		}
		if (m_VR->m_RenderDiagnostics.First(RenderDiagnosticEvent::NativeReticleDraw))
			Logger::Write("Native Portal reticle: original DrawSelf with explicit eye projection; clip=" +
				std::to_string(surface->previous.left) + "," + std::to_string(surface->previous.top) + "," +
				std::to_string(surface->previous.right) + "," + std::to_string(surface->previous.bottom) +
				" -> " + std::to_string(eye.width) + "x" + std::to_string(eye.height) +
				"; Source artwork/status/alpha retained");
		NativeReticle::ClipScope<NativeReticleSurface> clip(*surface, surface->previous,
			{0, 0, eye.width, eye.height});
		return hkDrawSelf.fOriginal(ecx, position->x, position->y, layout->width, layout->height, clr, flApparentZ);
	}

	return hkDrawSelf.fOriginal(ecx, newX, newY, w, h, clr, flApparentZ);
}

void __cdecl Hooks::dVGui_GetHudBounds(int slot, int& x, int& y, int& w, int& h) {
	if (m_VR->m_IsVREnabled && !m_Game->m_VguiSurface->IsCursorVisible())
	{
		x = y = 0;
		w = m_VR->m_RenderWidth;
		h = m_VR->m_RenderHeight;
	} else {
		hkVGui_GetHudBounds.fOriginal(slot, x, y, w, h);
	}

	//std::cout << "dVGui_GetHudBounds - X: " << x << ", Y: " << y << ", W: " << w << ", H: " << h << "\n";
}

void __cdecl Hooks::dVGui_GetPanelBounds(int slot, int& x, int& y, int& w, int& h) {
	if (m_VR->m_IsVREnabled && !m_Game->m_VguiSurface->IsCursorVisible())
	{
		x = y = 0;
		w = m_VR->m_RenderWidth;
		h = m_VR->m_RenderHeight;
	}
	else {
		hkVGui_GetPanelBounds.fOriginal(slot, x, y, w, h);
	}

	//std::cout << "dVGui_GetPanelBounds - X: " << x << ", Y: " << y << ", W: " << w << ", H: " << h << "\n";
}

void __cdecl Hooks::dVGUI_UpdateScreenSpaceBounds(int nNumSplits, int sx, int sy, int sw, int sh) {
	hkVGUI_UpdateScreenSpaceBounds.fOriginal(nNumSplits, sx, sy, m_VR->m_RenderWidth, m_VR->m_RenderHeight);
}

void __cdecl Hooks::dVGui_GetTrueScreenSize(int &w, int &h) {
	w = m_VR->m_RenderWidth;
	h = m_VR->m_RenderHeight;
}

void __fastcall Hooks::dGetScreenSize(void* ecx, void* edx, int& wide, int& tall) {
	//hkGetScreenSize.fOriginal(ecx, wide, tall);
	wide = m_VR->m_RenderWidth;
	tall = m_VR->m_RenderHeight;
}

void __cdecl Hooks::dGetHudSize(int& w, int& h) {
	w = m_VR->m_RenderWidth;
	h = m_VR->m_RenderHeight;
}

void __fastcall Hooks::dPush2DView(void* ecx, void* edx, IMatRenderContext* pRenderContext, const CViewSetup& view, int nFlags, ITexture* pRenderTarget, void* frustumPlanes) {
	m_PushedHud = false;

	return hkPush2DView.fOriginal(ecx, pRenderContext, view, nFlags, pRenderTarget, frustumPlanes);
}

void __fastcall Hooks::dRender(void* ecx, void* edx, vrect_t* rect) {
	//std::cout << "dRender - X: " << rect->x << ", Y: " << rect->y << ", W: " << rect->width << ", H: " << rect->height  << "\n";

	return hkRender.fOriginal(ecx, rect);
}

void __fastcall Hooks::dSetBounds(void* ecx, void* edx, int x, int y, int w, int h) {
	std::cout << "dSetBounds - X: " << x << ", Y: " << y << ", W: " << w << ", H: " << h << "\n";

	hkSetBounds.fOriginal(ecx, x, y, m_VR->m_RenderWidth, m_VR->m_RenderHeight);
}

void __fastcall Hooks::dSetSize(void* ecx, void* edx, int wide, int tall) {
	hkSetSize.fOriginal(ecx, wide, tall);

	//std::cout << "dSetSize - Wide: " << wide << ", Tall: " << tall  << "\n";
}

void __fastcall Hooks::dGetClipRect(void* ecx, void* edx, int& x0, int& y0, int& x1, int& y1) {
	hkGetClipRect.fOriginal(ecx, x0, y0, x1, y1);

	//std::cout << "dGetClipRect - X: " << x0 << ", Y: " << y0 << ", W: " << x1 << ", H: " << y1  << "\n";
}

double __fastcall Hooks::dComputeError(void* ecx, void* edx) {
	bool wasTrue = m_VR->m_OverrideEyeAngles;

	m_VR->m_OverrideEyeAngles = true;

	double computedError = hkComputeError.fOriginal(edx);

	if (!wasTrue)
		m_VR->m_OverrideEyeAngles = false;

	return computedError;
}

bool __fastcall Hooks::dUpdateObject(void* ecx, void* edx, void* pPlayer, float flError, bool bIsTeleport) {
	if (!RuntimePublished()) return hkUpdateObject.fOriginal(ecx, pPlayer, flError, bIsTeleport);
	bool wasTrue = m_VR->m_OverrideEyeAngles;

	m_VR->m_OverrideEyeAngles = true;

	bool value = hkUpdateObject.fOriginal(ecx, pPlayer, flError, bIsTeleport);

	if (!wasTrue)
		m_VR->m_OverrideEyeAngles = false;

	return value;
}

bool __fastcall Hooks::dUpdateObjectVM(void* ecx, void* edx, void* pPlayer, float flError) {
	if (!RuntimePublished()) return hkUpdateObjectVM.fOriginal(ecx, pPlayer, flError);
	bool wasTrue = m_VR->m_OverrideEyeAngles;

	m_VR->m_OverrideEyeAngles = true;

	bool value = hkUpdateObjectVM.fOriginal(ecx, pPlayer, flError);

	if (!wasTrue)
		m_VR->m_OverrideEyeAngles = false;

	return value;
}

// This function is apparently not used by Portal 2, remove?
void __fastcall Hooks::dRotateObject(void* ecx, void* edx, void* pPlayer, float fRotAboutUp, float fRotAboutRight, bool bUseWorldUpInsteadOfPlayerUp) {
	bool wasTrue = m_VR->m_OverrideEyeAngles;

	m_VR->m_OverrideEyeAngles = true;

	hkRotateObject.fOriginal(ecx, pPlayer, fRotAboutUp, fRotAboutRight, bUseWorldUpInsteadOfPlayerUp);

	if (!wasTrue)
		m_VR->m_OverrideEyeAngles = false;
}

// This is CPlayerBase, do we also need to hook CPortalPlayer? can the same function be used by both?
// This works for release, but why was it crashing before??? TODO: buy a c++ book...
QAngle& __fastcall Hooks::dEyeAngles(void* ecx, void* edx) {
	if (!RuntimePublished()) return hkEyeAngles.fOriginal(ecx);
	if (m_VR->m_OverrideEyeAngles) {
		int localIndex = m_Game->m_EngineClient->GetLocalPlayer();
		int index = EntityIndex(ecx);
		if (!ValidPlayerSlot(m_Game, index)) return hkEyeAngles.fOriginal(ecx);

		auto& vrPlayer = m_Game->m_PlayersVRInfo[index];

		if (m_VR->m_IsVREnabled && m_VR->m_TrackingOutputValid && m_VR->m_RightControllerPose.valid && localIndex == index) {
			return m_VR->GetRightControllerAbsAngleConst();
		}
		else if (vrPlayer.isUsingVR)
		{
			return vrPlayer.controllerAngle;
		}
	}

	return hkEyeAngles.fOriginal(ecx);
}

int __fastcall Hooks::dGetDefaultFOV(void* ecx, void* edx) {
	if (!RuntimePublished()) return hkGetDefaultFOV.fOriginal(ecx);
	return m_VR->m_Fov;
}

double __fastcall Hooks::dGetFOV(void* ecx, void* edx) {
	if (!RuntimePublished()) return hkGetFOV.fOriginal(ecx);
	return m_VR->m_Fov;
}

float __fastcall Hooks::dGetViewModelFOV(void* ecx, void* edx) {
	if (!RuntimePublished()) return hkGetViewModelFOV.fOriginal(ecx);
	return static_cast<float>(m_VR->m_Fov);
}
