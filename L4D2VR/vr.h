#pragma once
#include "openvr.h"
#include "vector.h"
#include <chrono>
#include <atomic>
#include "digital_input.h"
#include "tracked_device.h"
#include "config.h"
#include "roomscale_motor.h"
#include "ui_input.h"
#include "haptics.h"
#include "render_diagnostics.h"
#include "render_target_readiness.h"
#include "vr_resource_lifecycle.h"
#include "render_condition_diagnostics.h"
#include "menu_overlay_placement.h"
#include "muzzle_origin.h"
#include "openvr_session.h"
#include <filesystem>

#define MAX_STR_LEN 256

class Game;
class C_BasePlayer;
struct IDirect3DTexture9;
struct IDirect3DSurface9;
struct IDirect3DDevice9;
struct IDirect3DVR9;
class ITexture;


struct TrackedDevicePoseData 
{
	bool valid = false;
	std::string TrackedDeviceName;
	Vector TrackedDevicePos;
	Vector TrackedDeviceVel;
	QAngle TrackedDeviceAng;
	QAngle TrackedDeviceAngVel;
};

class SharedTextureHolder
{
public:
	vr::VRVulkanTextureData_t m_VulkanData{};
	vr::Texture_t m_VRTexture{};
};

class VR
{
public:
	Game *m_Game = nullptr;
    IDirect3DDevice9 *m_D3DDevice = nullptr;
    IDirect3DVR9 *m_D3DVR = nullptr;
    bool OwnsD3DDevice(const IDirect3DDevice9 *device) const { return m_D3DDevice == device; }
    bool InvalidateD3DResources(bool deviceReset = true);
    bool DetachBackBufferOverlay(bool hide = true);
    bool RefreshBackBuffer(SharedTextureHolder& holder);
    void InvalidateTrackingOutput();
    void SuspendInputForRenderFailure();

	vr::IVRSystem *m_System = nullptr;
	vr::IVRInput *m_Input = nullptr;
	vr::IVROverlay *m_Overlay = nullptr;
	vr::IVRRenderModels *m_RenderModels = nullptr;

	vr::VROverlayHandle_t m_MainMenuHandle = vr::k_ulOverlayHandleInvalid;
	vr::VROverlayHandle_t m_HUDHandle = vr::k_ulOverlayHandleInvalid;
	bool m_HUDBoundsReady = false;
	bool m_WorldAimMarkerLogged = false;
	bool m_WorldAimMarkerCadenceLogged = false;
	std::chrono::steady_clock::time_point m_LastWorldAimMarkerUpdate{};
	bool m_HUDCaptureLogged = false;
	unsigned m_HUDMissingCaptureFrames = 0;
	UiInput::MenuPointerState m_MenuPointerState;
	std::chrono::steady_clock::time_point m_NextMenuInputErrorLog{};
	std::chrono::steady_clock::time_point m_NextMenuPlacementLog{};
	std::chrono::steady_clock::time_point m_NextHapticErrorLog{};
	Haptics::ShotGate m_PortalShotHapticGate;
	bool m_HapticOutputsAvailable = false;
	bool m_OpenVRStarted = false;
    Portal2VROpenVR::SessionLease m_OpenVRSession;
	int m_LastPoseError = 0;
	int m_LastInputError = 0;

	float m_HorizontalOffsetLeft;
	float m_VerticalOffsetLeft;
	float m_HorizontalOffsetRight;
	float m_VerticalOffsetRight;

	uint32_t m_RenderWidth;
	uint32_t m_RenderHeight;
	uint32_t m_AntiAliasing;
	uint32_t m_RenderWindow;
	float m_Aspect;
	float m_Fov;

	vr::VRTextureBounds_t m_TextureBounds[2];
	vr::TrackedDevicePose_t m_Poses[vr::k_unMaxTrackedDeviceCount];

	Vector m_EyeToHeadTransformPosLeft = { 0,0,0 };
	Vector m_EyeToHeadTransformPosRight = { 0,0,0 };

	Vector m_HmdForward;
	Vector m_HmdRight;
	Vector m_HmdUp;

	Vector m_HmdPosLocalInWorld = { 0,0,0 };

	Vector m_LeftControllerForward;
	Vector m_LeftControllerRight;
	Vector m_LeftControllerUp;

	Vector m_RightControllerForward;
	Vector m_RightControllerRight;
	Vector m_RightControllerUp;

	Vector m_ViewmodelForward;
	Vector m_ViewmodelRight;
	Vector m_ViewmodelUp;

	QAngle m_HmdAngAbs;

	Vector m_HmdPosRelativeRaw = { 0,0,0 };

	Vector m_HmdPosRelative = { 0,0,0 };

	Vector m_AimPos = { 0, 0, 0 };
    MuzzleOrigin::State m_MuzzleSample;
    std::atomic<bool> m_MuzzleSampleLogged{false};
    bool m_MuzzleWaitingLogged = false;
	bool m_Traced = false;

	Vector m_Center = { 0,0,0 };
	Vector m_SetupOrigin = { 0,0,0 };
	TrackingSpace::PlayspaceState m_Playspace;
	RoomscaleMotion::Observer m_RoomscaleObserver;
	RoomscaleMotion::Motor m_RoomscaleMotor;
	std::optional<bool> m_LastRoomscaleEligibility;
	std::chrono::steady_clock::time_point m_NextRoomscaleEligibilityLog{};
	PortalOrientation::Coordinator m_PortalCoordinator;
	PortalOrientation::RigAnchor m_PortalRigAnchor;
	PortalOrientation::Rotation m_PortalEffectiveRotation;
	PortalOrientation::Mode m_ActivePortalMode = PortalOrientation::Mode::LegacyYaw;
	std::chrono::steady_clock::time_point m_NextPortalEventLog{};
	std::uint64_t m_PoseFetchSequence = 0; // increments only after a successful WaitGetPoses
	std::chrono::steady_clock::time_point m_NextRoomscaleSummary{};
	std::chrono::steady_clock::time_point m_NextRoomscaleAnomalyLog{};
	std::chrono::steady_clock::time_point m_NextRoomscaleTrackingLog{};
	bool m_TrackingOutputValid = false;
	bool m_HmdLostSinceLastValid = false;
	bool m_HasLastHmdOffset = false;
	Vector m_LastHmdOffsetUnits{0.0f, 0.0f, 0.0f};
	bool m_MovementFallbackActive = false;
	bool m_StandingHeightInactiveLogged = false;
	bool m_HasEyeHeight = false;
	bool m_EyeHeightWasInvalid = false;
	int m_EyeHeightPlayerIndex = -1;
	C_BasePlayer* m_EyeHeightPlayerEntity = nullptr;
	float m_LastEyeHeightUnits = 0.0f;

	float m_HeightOffset = 0.0;

	Vector m_LeftControllerPosAbs;											
	Vector m_LeftControllerPosRel{0.0f, 0.0f, 0.0f};
	bool m_LeftControllerOutputValid = false;
	QAngle m_LeftControllerAngAbs;
	Vector m_RightControllerPosRel;											
	QAngle m_RightControllerAngAbs;

	Vector m_ViewmodelPosOffset;
	QAngle m_ViewmodelAngOffset;

	Vector m_ViewmodelPosCustomOffset; // Custom (from config) viewmodel position offset applied on top of hardcoded ones
    QAngle m_ViewmodelAngCustomOffset; // Custom (from config) viewmodel angle offset applied on top of hardcoded ones

	float m_Ipd;																	
	float m_EyeZ;

	Vector m_IntendedPositionOffset = { 0,0,0 };

	enum TextureID
	{
		Texture_None = -1,
		Texture_LeftEye,
		Texture_RightEye,
		Texture_HUD,
		Texture_Blank
	};

	ITexture *m_LeftEyeTexture = nullptr;
	ITexture *m_RightEyeTexture = nullptr;
	ITexture *m_HUDTexture = nullptr;
	ITexture *m_BlankTexture = nullptr;

	IDirect3DSurface9 *m_D9LeftEyeSurface = nullptr;
	IDirect3DSurface9 *m_D9RightEyeSurface = nullptr;
	IDirect3DSurface9 *m_D9HUDSurface = nullptr;
	IDirect3DSurface9 *m_D9BlankSurface = nullptr;

	SharedTextureHolder m_VKLeftEye;
	SharedTextureHolder m_VKRightEye;
	SharedTextureHolder m_VKBackBuffer;
	SharedTextureHolder m_VKHUD;
	SharedTextureHolder m_VKBlankTexture;

	bool m_IsVREnabled = false;
	RenderDiagnosticGate m_RenderDiagnostics;
	RenderTargetDiagnosticGate m_RenderTargetDiagnostics;
	RenderConditionDiagnostics m_RenderConditions;
	VRResourceLifecycle m_ResourceLifecycle;
	MenuOverlayPlacement m_MenuOverlayPlacement;
	bool m_IsInitialized = false;
	bool m_RenderedNewFrame = false;
	bool m_RenderedHud = false;
	bool m_CreatedVRTextures = false;
    RenderTargetRetryState m_RenderTargetRetry;
	bool m_LaserRequestLogged = false;
	bool m_LaserParticleObserved = false;
	TextureID m_CreatingTextureID = Texture_None;

	bool m_PressedTurn = false;
	bool m_PushingThumbstick = false;
	bool m_PointerCreated = false;
	DigitalButtonState m_PrimaryAttackState;
	DigitalButtonState m_SecondaryAttackState;
	DigitalButtonState m_JumpState;
	DigitalButtonState m_CrouchState;
	DigitalButtonState m_UseState;
	DigitalButtonState m_ReloadState;

	// action set
	vr::VRActionSetHandle_t m_ActionSet;
	vr::VRActiveActionSet_t m_ActiveActionSet;
	vr::VRActionSetHandle_t m_HapticActionSet = vr::k_ulInvalidActionSetHandle;
	vr::VRActiveActionSet_t m_ActiveHapticActionSet{};
	vr::VRActionHandle_t m_HapticLeft = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_HapticRight = vr::k_ulInvalidActionHandle;

	// actions
	vr::VRActionHandle_t m_ActionJump;
	vr::VRActionHandle_t m_ActionPrimaryAttack;
	vr::VRActionHandle_t m_ActionSecondaryAttack;
	vr::VRActionHandle_t m_ActionReload;
	vr::VRActionHandle_t m_ActionWalk;
	vr::VRActionHandle_t m_ActionTurn;
	vr::VRActionHandle_t m_ActionUse;
	vr::VRActionHandle_t m_ActionNextItem;
	vr::VRActionHandle_t m_ActionPrevItem;
	vr::VRActionHandle_t m_ActionResetPosition;
	vr::VRActionHandle_t m_ActionCrouch;
	vr::VRActionHandle_t m_ActionFlashlight;
	vr::VRActionHandle_t m_ActionActivateVR;
	vr::VRActionHandle_t m_MenuSelect;
	vr::VRActionHandle_t m_MenuBack;
	vr::VRActionHandle_t m_MenuUp;
	vr::VRActionHandle_t m_MenuDown;
	vr::VRActionHandle_t m_MenuLeft;
	vr::VRActionHandle_t m_MenuRight;
	vr::VRActionHandle_t m_Spray; 
	vr::VRActionHandle_t m_Scoreboard;
	vr::VRActionHandle_t m_ShowHUD;
	vr::VRActionHandle_t m_Pause;

	TrackedDevicePoseData m_HmdPose;
	TrackedDevicePoseData m_LeftControllerPose;
	TrackedDevicePoseData m_RightControllerPose;

	bool m_ApplyPortalRotationOffset = false;
	QAngle m_PortalRotationOffset = {0, 0, 0};
	QAngle m_RotationOffset = { 0, 0, 0 };
	bool m_OverrideEyeAngles = false;
	std::chrono::steady_clock::time_point m_PrevFrameTime;

	float m_TurnSpeed = 0.15f;
	bool m_SnapTurning = false;
	float m_SnapTurnAngle = 45.0f;
	bool m_LeftHanded = false;
	float m_VRScale = 43.2f;
	float m_IpdScale = 1.0f;
	bool m_6DOF = true;
	float m_HudDistance = 1.3f;
	float m_HudSize = 4.0f;
	bool m_HudAlwaysVisible = false;
	int m_AimMode = 2;
	ConfigSnapshot m_Config;
	std::filesystem::file_time_type m_ConfigLastModified{};
	std::chrono::steady_clock::time_point m_NextConfigCheck{};

	VR() {};
	VR(Game *game);
	~VR();
	bool SetActionManifest(const char *fileName);
	bool InstallApplicationManifest(const char *fileName);
	void Update();
	void SetScreenSizeOverride(bool bState);
	void CreateVRTextures();
	void SubmitVRTextures();
	bool RepositionOverlays();
	void CreateExperimentalHUDOverlay();
	void SubmitExperimentalHUDOverlay();
	void GetPoses();
	bool UpdatePosesAndActions();
	void GetViewParameters();
	void ProcessMenuInput();
	void SendMenuMouse(UiInput::MouseTransition transition);
	void ReleaseMenuMouse();
	void ProcessInput();
	void QueuePortalShotHaptic();
	void DispatchPortalShotHaptic(bool actionsReady);
	void ProcessViewActions();
	Vector GetMovementForward();
	void ProcessHeldAction(vr::VRActionHandle_t actionHandle, DigitalButtonState &state,
	                       const char *pressCommand, const char *releaseCommand);
	void ReleaseHeldActions();
	VMatrix VMatrixFromHmdMatrix(const vr::HmdMatrix34_t &hmdMat);
	vr::HmdMatrix34_t VMatrixToHmdMatrix(const VMatrix &vMat);
	vr::HmdMatrix34_t GetControllerTipMatrix(vr::ETrackedControllerRole controllerRole);
	bool CheckOverlayIntersectionForController(vr::VROverlayHandle_t overlayHandle, vr::ETrackedControllerRole controllerRole);
	QAngle GetRightControllerAbsAngle();
	QAngle& GetRightControllerAbsAngleConst();
	Vector GetRightControllerAbsPos();
	Vector GetRecommendedViewmodelAbsPos();
	QAngle GetRecommendedViewmodelAbsAngle();
	void UpdateHMDAngles();
	void UpdateTracking();
	bool ExperimentalPortalOrientation() const;
	void QueuePortalTraversal(std::uintptr_t playerKey, std::uintptr_t portalKey,
	                          const std::optional<PortalOrientation::Rotation> &rotation);
	void ApplyPendingPortalOrientation(const Vector &renderOrigin);
	void ApplyPortalRigToDerivedPose();
	void ResetPortalOrientation();
	void ObserveRoomscaleCommand(int commandNumber);
	bool RoomscaleEnabled() const;
	bool RoomscaleEligible() const;
	void ResetRoomscale(bool recenter = false, bool newCommandStream = false);
	void UpdateRoomscaleRenderAnchor(const Vector &sourceAnchor);
	std::optional<TrackingSpace::MoveAxes> GetRoomscaleCommand(int commandNumber, bool manualMovement);
	Vector GetHmdViewOffset();
	void UpdateAimFeedback(C_BasePlayer *localPlayer);
    void ResetMuzzleSample();
    void CaptureViewmodelMuzzle(const Vector &, const Vector &, const QAngle &);
    std::optional<Vector> GetAimBeamOrigin();
	Vector GetViewAngle();
	Vector GetViewOrigin(Vector setupOrigin);
	Vector GetViewOriginLeft(Vector setupOrigin);
	Vector GetViewOriginRight(Vector setupOrigin);
	bool PressedDigitalAction(vr::VRActionHandle_t &actionHandle, bool checkIfActionChanged = false);
	bool GetAnalogActionData(vr::VRActionHandle_t &actionHandle, vr::InputAnalogActionData_t &analogDataOut);
	void ResetPosition();
	void GetPoseData(vr::TrackedDevicePose_t &poseRaw, TrackedDevicePoseData &poseOut);
	void ParseConfigFile();
	Vector Trace(uint32_t* localPlayer, bool &didHit);
	Vector TraceEye(uint32_t* localPlayer, Vector cameraPos, Vector eyePos, QAngle& eyeAngle);
};
