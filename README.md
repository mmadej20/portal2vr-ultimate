# Portal 2 VR

An incremental modernization fork of [Gistix/portal2vr](https://github.com/Gistix/portal2vr), for the original Windows version of Portal 2. This remains a 32-bit Source/DX9 runtime-hooking mod, not a replacement engine or a rewrite. SteamVR / OpenVR and the modified DXVK rendering path are retained.

## Getting started

Requirements: Portal 2 for Windows, Steam, working SteamVR and a VR headset. The launcher requires Python **3.10+** with Tcl/Tk (included in the standard Windows Python installer). It does not install Python, SteamVR or drivers automatically.

1. Build the mod as described below. This source checkout does not contain a prebuilt `Release/d3d9.dll`.
2. Double-click **`Portal2VR Launcher.cmd`** in the repository root. Alternatively run `python -m tools.launcher` or open `Portal2VR Launcher.pyw`.
3. Select the folder containing `portal2.exe` and your `steam.exe`. Choose the Portal 2 copy registered in that Steam client: launch requests AppID 620, not the selected executable directly. Paths are checked separately; multiple independent Steam installations are not automatically matched (a library on another drive is fine). Close Portal 2 before installing or changing its files.
4. Start SteamVR and connect the headset/controllers, then click **Launch Portal 2 in VR**. The launcher applies the config, backs up originals and launches Steam app 620 with `-insecure` and the retained VR-compatible video arguments.
5. Enable subtitles in Portal 2's own options if you want captions.

The launcher starts with the **recommended setup**. Settings survive restarts. `Save settings` changes only local preferences; `Apply without launching` installs the mod/config without starting the game. Apply changes with the game closed; restart to use the new settings.

### Recommended setup

The launcher default includes features confirmed during the maintainer's headset sessions:

- Seated tracking with positional head movement; Standing is available in Settings.
- Physical roomscale walking with player/body feedback, rather than moving only the camera away from the collision body. `RoomscaleMode=ActiveExperimental` uses guarded `CUserCmd` movement; `Off` and read-only `Observe` remain available.
- LegacyYaw portal traversal, HMD-relative stick movement and smooth turning. Snap turning is selectable.
- Controller-locked portal-gun model and glow, with current calibration offsets. The working **experimental world aim line** starts at the animated muzzle.
- The **original Portal reticle and dynamic blue/orange status** at the aim endpoint, not custom replacement artwork.
- Readable subtitle overlay and pause/menu placement corrected after physical walking.
- Portal-shot haptics, with bounded amplitude/duration and duplicate-event suppression.
- Quiet diagnostics: startup, changed render geometry and failures are logged. Repeated allocation/MSAA details and periodic summaries are off; enable **Verbose diagnostics** in Advanced when investigating them. The extra desktop render is off.

These observations concern the tested installation, not every headset, game build or portal orientation. The versioned `L4D2VR/config.txt` retains conservative default-off experimental flags; the launcher supplies the recommended profile.

### Configuration and recovery

Settings provides tracking, movement, roomscale, turning and desktop-render options. Advanced exposes all current mod parameters with descriptions/bounds: caption placement, gun calibration, IPD/world scale, controller pitch, haptics, portal orientation, anti-aliasing, aiming and diagnostics. Invalid numbers and incompatible roomscale/muzzle settings are rejected before deployment. Preview config shows what will be written.

`Restore recommended` resets the form without changing installation paths or silently applying anything. Turning off **World aim line** selects the native particle beam for diagnosis; it is **not confirmed visible** and is not recommended.

Before first installation the shared installer takes a hash-verified snapshot of any existing files it will replace. Later updates retain that first snapshot. **Restore original game files** restores pre-existing files and removes only files created by the launcher; backups remain available. Externally edited managed files, corrupted backups and concurrent installer operations are refused rather than overwritten.

Keep these local files together when moving the checkout or recovering an installation:

- `tools/.launcher-state.json`: shared install ownership and developer checklist.
- `tools/.launcher-backups/`: original snapshots and manifests.
- `tools/.user-launcher-settings.json`: user settings, separate from install ownership.

Do not delete install state/backups to fix an error. If only user preferences are malformed, copy that preferences file aside before replacing it; startup does not silently reset it. Open mod log accesses `bin/portal2vr.log` in the selected game folder. The older `python -m tools.test_launcher` remains available for manual tests using the same backups/transaction lock. Changing settings in one launcher does not silently synchronize the other launcher's form.

## Changes from the original mod

- Digital inputs send press/release commands on state transitions instead of repeating release commands every frame. One-shot actions trigger only on press transitions.
- HMD/controller indices and pose validity are checked; disconnected/untracked controllers do not become unchecked pose-array accesses or current-looking stale hand poses.
- OpenVR subsystem/action startup errors and required/optional hook resolution have explicit failure handling and persistent Release diagnostics. Missing `-insecure` is explained instead of silently terminating the game.
- Config parsing has bounds and valid-value fallback. Reloading uses a managed watcher lifecycle and logs errors instead of repeated background dialogs.
- Shared tracking-space conversion, independent hand offsets, recenter/turn coherence, Standing height handling and selectable locomotion direction were added.
- Experimental roomscale feedback includes replay guards, manual-input priority and portal/reset handling. Experimental 3D portal orientation modes are separate from recommended LegacyYaw.
- VR startup/render-target readiness and menu placement were stabilized across the mod and modified DXVK.
- Stereo aiming/HUD projection and native reticle clipping were corrected. Gun/effect alignment and animated muzzle sampling serve both beam paths; the world line is the launcher fallback for the still-invisible native particle beam.
- Subtitle capture/placement, roomscale pause-menu anchoring and menu click releases were improved; local portal-shot haptics were added.
- DXVK and MinHook are ordinary pinned source files instead of submodules, retaining licenses/provenance in `THIRD_PARTY_SOURCES.txt`. The VR DXVK fork is intentionally retained, not replaced with generic latest DXVK.
- A user-facing launcher provides validated advanced settings, original backups, transactional staging/restore, config preview and diagnostic shortcuts. Pure-component C++ and offline Python installer/GUI tests cover reproducible behavior without the Source runtime.

## Known limitations

- The original `robot_point_beam` particle laser remains invisible on the tested setup despite creation/control-point/draw fixes. Use the recommended world line and original Portal reticle.
- FullRotation, YawOnly and PreserveHorizon are manual experiments. FullRotation has limited wall-portal evidence; floor/ceiling comfort, recenter edge cases and broader traversal need testing. Active roomscale requires LegacyYaw and 6DOF.
- Reticle distance scaling is modest (100% at 1 m to 80% at 10 m); a dedicated far-distance readability comparison remains outstanding.
- Signatures, offsets, vtables and the 32-bit engine ABI remain compatibility risks. No universal compatibility, co-op, performance or 120 Hz frame-rate claim is made.
- `EyePosition`, `Weapon_ShootPosition` and `GetViewModelFOV` use audited RVAs only after the DLL's SHA-256 and its loaded code/table references match the supported build. The shoot-origin override applies only to the verified `FirePortal` call, preserving other calls to the shared function. An unsupported DLL, failed verification or failed hook installation produces a warning in `bin/portal2vr.log` and skips that hook without blocking startup; Source's original shoot position or viewmodel FOV may be used. The user reported successful hardware testing on one current setup; other game builds and setups require separate validation.
- No OpenXR backend, haptic redesign, optimized desktop mirror, Portal Reloaded ThirdAttack support or complete HUD rewrite. Haptics cover local portal shots, not every interaction.

## Selective PCVR improvements

The `native-vr` fork was reviewed at commit [`0094269`](https://github.com/iFeelLikeChicken2Nite/portal2vr-ultimate/commit/0094269795aa29d2b0477af00e082b8b686aa68c). The changes here adapt its menu-refresh and HUD ideas independently of Sixense. No fork commits were cherry-picked, and no Sixense/Hydra proxy, bindings, module aliases, calibration or alternate engine ABI were added.

Render targets retain their existing ownership and bounded retry policy. A return from gameplay to the menu/loading state requests one compatibility refresh instead of recreating every menu frame. A real D3D9 reset invalidates resources independently, including while already in a menu. Failed creation keeps stereo disabled and retains the existing one-second backoff and three-attempt budget, including failures of the final allocation drain. Missing bridge/backbuffer data waits for recovery without consuming allocation attempts. A failed Present queue drain clears tracking validity and releases held actions/menu clicks, preventing stale controller input while submission is unavailable. Menu/pause cursor handling remains on the existing overlay path.

Graphics diagnostics report initial failures, changed failure details and recovery. Quiet mode suppresses unchanged repeated conditions per instance; **Verbose diagnostics** repeats unchanged details at most once every five seconds per condition. New failures, changed details and recovery remain immediate. Logs distinguish missing bridge/capture/image/dimensions/context, non-owned devices, recreation/reset and submission failures. A successful API call or allocation does not establish correct headset pixels.

Advanced offers these optional legacy HUD controls:

| Setting | Default | Effect |
| --- | --- | --- |
| `HUDInEyeCentered` | `false` | Centers in-game HUD panels within each eye. Requires `AimMode=0` or `1` and `ExperimentalHUDOverlay=false`. |
| `HUDInEyeScale` | `0.45` | Width as a fraction of the eye target; allowed range `0.2` to `1`. Aspect is retained and the rectangle must fit inside the eye. |
| `HUDInEyeVerticalOffset` | `0.05` | Vertical offset as a fraction of eye height; allowed range `-0.4` to `0.4`. Positive moves down. |
| `ShowSourceCrosshair` | `true` | `false` hides recognized flat Source crosshair sprites in legacy VR eye rendering. It preserves native blue/orange Portal status, menus, desktop drawing, controller aiming and world aim lines. |

HUD centering changes only a validated eye viewport and restores the previous viewport on scope exit. It does not bind a different depth buffer. Mixed paint passes containing UI/cursor layers retain the original viewport. Incompatible centering settings are rejected by the launcher; the native parser falls back safely. The recommended native reticle/caption profile stays unchanged. Previously saved complete launcher preferences gain the new defaults without resetting existing values or paths; malformed/incomplete preferences remain errors.

Native controller grabbing and hold-distance adjustment are **deferred**. The fork's hand-origin override covers `UpdateObject` but misses directly invoked `UpdateObjectVM`, detects holding without a verified local-player check, and writes ConVar fields through assumed offsets. Its distance mapping can bypass native bounds, and disabling the feature can skip restoration. These fail the safety gate for standard Portal 2.

A future grab experiment needs exact-build verified coverage of both update paths and their `EyePosition`/`EyeAngles` calls; a local-player, current-controller-pose and confirmed-held-object gate; scoped nested restoration; and a verified `ICvar`/ConVar accessor to snapshot, set and restore all affected values on drop, disable, level change and shutdown. Distance adjustment should use a dedicated input action while holding. No grab settings, memory writes or input changes are shipped here.

These changes require a fresh game/headset session. Test startup, repeated menu idling and returns, pause/resume, campaign/workshop map loads, a graphics reset while in a menu and in gameplay, controller loss/recovery, exit, and log recovery after a transient failure. Test optional HUD controls in both eyes with legacy aiming; confirm that the recommended native portal-status reticle, captions, menu pointer and aim line behave as before. A map switch that never exposes a non-game state needs separate observation; scene transitions alone do not prove every Source resource event.

OpenVR Vulkan submissions use the application's queue, so consumer detachment and producer queue draining are resource-ordering safeguards, not a compositor fence or proof across drivers. The modified DXVK/OpenVR resource lifetime remains a runtime compatibility risk. See [Valve's Vulkan integration guidance](https://github.com/ValveSoftware/openvr/wiki/Vulkan). No new hardware validation is claimed by this backport.

## Building and verification

Use Visual Studio 2022 / Build Tools with the C++ desktop workload, MSVC v143 and a Windows SDK. Build **Release / x86**; the game-facing DLL must remain 32-bit. DXVK, OpenVR and MinHook sources are included; no submodule initialization is required.

From a Developer Command Prompt in the repository root:

```bat
msbuild l4d2vr.sln /t:Build /p:Configuration=Release /p:Platform=x86 /p:PORTAL2_DIR=
msbuild tests\stabilization_tests.vcxproj /t:Build /p:Configuration=Release /p:Platform=Win32
tests\bin\stabilization_tests.exe
msbuild tests\pcvr_render_tests.vcxproj /t:Build /p:Configuration=Release /p:Platform=Win32
tests\bin\pcvr_render_tests.exe
msbuild tests\pcvr_hud_tests.vcxproj /t:Build /p:Configuration=Release /p:Platform=Win32
tests\bin\pcvr_hud_tests.exe
python -m unittest discover -s tests -p test_*.py
```

Output: `Release/d3d9.dll`. Building does not deploy unless `PORTAL2_DIR` is set; leave it empty when using the launcher so it owns backup/staging. Optional direct MSBuild deployment copies the DLL to `%PORTAL2_DIR%\bin` and **does not create a launcher backup**. No machine-specific Steam deployment path is embedded in the build.

C++/Python suites validate isolated logic and fixture installations. GUI tests use temporary game files and intercept external Steam process invocation; they do not launch Portal 2 or validate a headset session. Existing Source SDK/DXVK compiler warnings remain; a successful build is not runtime VR proof.

## Credits

Based on Gistix/portal2vr, existing OpenVR bindings and the VR-modified DXVK pipeline. Upstream PRs/community forks were research references, not blindly merged branches. Third-party licenses/source pins are retained.
