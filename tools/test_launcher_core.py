"""Pure configuration and file operations for the local test launcher."""

from dataclasses import dataclass
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import tempfile
import uuid

if os.name == "nt":
    import msvcrt
else:
    import fcntl


@dataclass(frozen=True)
class Profile:
    label: str
    changes: dict[str, str]
    experimental: bool = False


@dataclass(frozen=True)
class Check:
    group: str
    id: str
    label: str
    profile: str


BASE_VALUES = {
    "VerboseDiagnostics": "false",
    "TrackingMode": "Seated",
    "RoomscaleMode": "Off",
    "PortalOrientationMode": "LegacyYaw",
    "ExperimentalHUDOverlay": "false",
    "HUDInEyeCentered": "false",
    "HUDInEyeScale": "0.45",
    "HUDInEyeVerticalOffset": "0.05",
    "ShowSourceCrosshair": "true",
    "ExperimentalWorldAimMarker": "false",
    "ExperimentalStereoReticle": "false",
    "ReticleDistanceScaling": "true",
    "ExperimentalViewmodelAlignment": "false",
    "AimFromViewmodelMuzzle": "false",
    "ExperimentalPortalShotHaptics": "false",
    "AimMode": "2",
    "RenderWindow": "0",
    "HUDDistanceMeters": "1.3",
    "HUDWidthMeters": "1.4",
    "HUDVerticalOffsetMeters": "-0.15",
    "ViewmodelPosCustomOffsetX": "0.0",
    "ViewmodelPosCustomOffsetY": "0.0",
    "ViewmodelPosCustomOffsetZ": "0.0",
}

PROFILES = {
    "baseline": Profile("Baseline (M0/M1)", {}),
    "combined": Profile("Confirmed setup + native reticle test",
                        {"ExperimentalHUDOverlay": "true",
                         "ExperimentalWorldAimMarker": "true",
                         "ExperimentalPortalShotHaptics": "true"}, True),
    "standing": Profile("M2 Standing", {"TrackingMode": "Standing"}),
    "observe": Profile("M3 Observe (no player movement)", {"RoomscaleMode": "Observe"}),
    "roomscale_observe_combined": Profile("M3 Observe + current VR setup (no player movement)",
                                          {"RoomscaleMode": "Observe",
                                           "ExperimentalHUDOverlay": "true",
                                           "ExperimentalWorldAimMarker": "true",
                                           "ExperimentalPortalShotHaptics": "true",
                                           "ViewmodelPosCustomOffsetX": "-4.5",
                                           "ViewmodelPosCustomOffsetY": "1.0",
                                           "ViewmodelPosCustomOffsetZ": "-1.5"}, True),
    "roomscale_active_experimental": Profile("Native reticle + muzzle line (fallback)",
                                             {"RoomscaleMode": "ActiveExperimental",
                                              "6DOF": "true",
                                              "ExperimentalViewmodelAlignment": "true",
                                              "AimFromViewmodelMuzzle": "true",
                                              "HUDDistanceMeters": "1.25",
                                              "HUDWidthMeters": "1.65",
                                              "HUDVerticalOffsetMeters": "0.10",
                                              "ExperimentalHUDOverlay": "true",
                                              "ExperimentalWorldAimMarker": "true",
                                              "ExperimentalPortalShotHaptics": "true",
                                              "ViewmodelPosCustomOffsetX": "-4.5",
                                              "ViewmodelPosCustomOffsetY": "1.0",
                                              "ViewmodelPosCustomOffsetZ": "-1.5"}, True),
    "full_rotation": Profile("M4 FullRotation", {"PortalOrientationMode": "FullRotation"}, True),
    "yaw_only": Profile("M4 YawOnly", {"PortalOrientationMode": "YawOnly"}, True),
    "preserve_horizon": Profile("M4 PreserveHorizon", {"PortalOrientationMode": "PreserveHorizon"}, True),
    "hud": Profile("M5 HUD", {"ExperimentalHUDOverlay": "true"}, True),
    "aim_marker": Profile("Controller aim marker (experimental)",
                          {"ExperimentalWorldAimMarker": "true"}, True),
    "aim_model_alignment": Profile("Native reticle + portal-gun alignment (experimental)",
                                   {"ExperimentalHUDOverlay": "true",
                                    "ExperimentalViewmodelAlignment": "true",
                                    "ExperimentalWorldAimMarker": "true",
                                    "ExperimentalPortalShotHaptics": "true",
                                    "ViewmodelPosCustomOffsetX": "-4.5",
                                    "ViewmodelPosCustomOffsetY": "1.0",
                                    "ViewmodelPosCustomOffsetZ": "-1.5"}, True),
    "haptics": Profile("M6 Haptics", {"ExperimentalPortalShotHaptics": "true"}, True),
    "mirror": Profile("M6 Mirror ON", {"RenderWindow": "1"}, True),
}

# A/B changes only the beam selector. Preserve the confirmed native reticle,
# model, captions and roomscale setup without duplicating its calibration.
PROFILES["roomscale_active_native_aim"] = Profile(
    "Legacy beam + HUD reticle (muzzle test)",
    {**PROFILES["roomscale_active_experimental"].changes,
     "ExperimentalWorldAimMarker": "false"}, True)
PROFILES["roomscale_active_stereo_aim"] = Profile(
    "Stereo atlas + muzzle line (rollback)",
    {**PROFILES["roomscale_active_experimental"].changes,
     "ExperimentalStereoReticle": "true", "ReticleDistanceScaling": "false"}, True)

CHECKLIST = (
    Check("M0/M1", "m1_launch", "Launch with -insecure; no crash; resolved hooks appear in log", "baseline"),
    Check("M0/M1", "m1_menu", "Menu and game image are visible in the HMD", "baseline"),
    Check("M0/M1", "m1_controller_loss", "Controller disconnect/reconnect does not crash", "baseline"),
    Check("M0/M1", "m1_shutdown", "Game exits cleanly", "baseline"),
    Check("M2", "m2_seated", "Seated: head and both hands track correctly", "baseline"),
    Check("M2", "m2_standing", "Standing: level loads; tracked-height anchor and recenter feel correct", "standing"),
    Check("M2", "m2_turn", "Snap/smooth turning and movement direction", "baseline"),
    Check("M2", "m2_aim", "3D laser line follows the right-controller shot", "aim_marker"),
    Check("M2", "m2_portal_status", "Native blue/orange portal status appears at the laser endpoint", "combined"),
    Check("M2", "m2_model_alignment", "Compare portal-gun body/glow against aim-marker profile", "aim_model_alignment"),
    Check("Aim A/B", "aim_native_muzzle", "DIAGNOSTIC: robot_point_beam starts at the gun muzzle and follows it", "roomscale_active_native_aim"),
    Check("Aim A/B", "aim_native_hud", "Original HUD crosshair: visible, stable and dynamic blue/orange status", "roomscale_active_experimental"),
    Check("Aim A/B", "aim_stereo_muzzle", "RECOMMENDED: original HUD reticle + world line starts at the gun muzzle", "roomscale_active_experimental"),
    Check("Aim A/B", "aim_reticle_distance", "Native reticle: gently smaller on distant walls; portal status remains readable", "roomscale_active_experimental"),
    Check("M3", "m3_observe", "Observe logs steps without moving the player", "roomscale_observe_combined"),
    Check("M3", "m3_active_walk", "Small physical steps: body follows head; returning restores alignment", "roomscale_active_experimental"),
    Check("M3", "m3_active_wall", "Wall: view remains bounded; stepping back does not drift the body", "roomscale_active_experimental"),
    Check("M3", "m3_active_input", "Stick, pause and recenter: no jumps or lingering physical movement", "roomscale_active_experimental"),
    Check("M3", "m3_active_portal", "After walk tests: wall portal resets old target; head/hand stay aligned", "roomscale_active_experimental"),
    Check("M4", "m4_legacy", "LegacyYaw: wall-to-wall portal traversal", "baseline"),
    Check("M4", "m4_full", "FullRotation: head/hand orientation across wall portals", "full_rotation"),
    Check("M4", "m4_floor", "Floor/ceiling portals: comfort and stereo", "preserve_horizon"),
    Check("M4", "m4_recenter", "Recenter around portal traversal; no duplicate event", "full_rotation"),
    Check("M5", "m5_subtitles", "Subtitles: visible, readable, and follow the view", "hud"),
    Check("M5", "m5_menu", "Pause/menu: hover, click, back; no stuck click", "hud"),
    Check("M5", "m5_roomscale_menu", "After physical walking: pause menu is readable without returning to start", "roomscale_active_experimental"),
    Check("M5", "m5_loss", "Keyboard still operates menu after controller loss", "hud"),
    Check("M6", "m6_shot", "Portal shot: one pulse on the correct hand", "haptics"),
    Check("M6", "m6_loss", "Controller loss: no haptic spam or crash", "haptics"),
    Check("M6", "m6_mirror", "Mirror off/on: image and UI; no performance claim", "mirror"),
    Check("M6", "m6_perf", "Record median and high-percentile CPU/GPU timing", "mirror"),
)


def visible_checks(completed: set[str], show_completed: bool = False) -> tuple[Check, ...]:
    """Keep the local record while presenting only outstanding checks by default."""
    return tuple(item for item in CHECKLIST if show_completed or item.id not in completed)


SOURCE_TO_TARGET = (
    ("Release/d3d9.dll", "bin/d3d9.dll"),
    ("thirdparty/openvr/bin/win32/openvr_api.dll", "bin/openvr_api.dll"),
    ("L4D2VR/config.txt", "VR/config.txt"),
    ("L4D2VR/manifest.vrmanifest", "VR/manifest.vrmanifest"),
    ("L4D2VR/portal2vr_capsule_main.png", "VR/portal2vr_capsule_main.png"),
    ("L4D2VR/SteamVRActionManifest/action_manifest.json",
     "VR/SteamVRActionManifest/action_manifest.json"),
    ("L4D2VR/SteamVRActionManifest/bindings_knuckles.json",
     "VR/SteamVRActionManifest/bindings_knuckles.json"),
    ("L4D2VR/SteamVRActionManifest/bindings_oculus_touch.json",
     "VR/SteamVRActionManifest/bindings_oculus_touch.json"),
    ("L4D2VR/SteamVRActionManifest/bindings_vive_cosmos_controller.json",
     "VR/SteamVRActionManifest/bindings_vive_cosmos_controller.json"),
)


def render_config(template: str, profile: str) -> str:
    """Apply one known test profile to the versioned config template."""
    if profile not in PROFILES:
        raise ValueError(f"Unknown test profile: {profile}")
    return apply_config_values(template, {**BASE_VALUES, **PROFILES[profile].changes})


def apply_config_values(template: str, values: dict[str, str]) -> str:
    """Replace supplied keys, retaining template comments and line endings."""
    seen: set[str] = set()
    result: list[str] = []
    for line in template.splitlines(keepends=True):
        newline = "\r\n" if line.endswith("\r\n") else "\n" if line.endswith("\n") else ""
        body = line[: -len(newline)] if newline else line
        if "=" not in body:
            result.append(line)
            continue
        key_part, old_value = body.split("=", 1)
        key = key_part.strip()
        if key not in values:
            result.append(line)
            continue
        if key in seen:
            raise ValueError(f"Duplicate config key: {key}")
        seen.add(key)
        before_comment, marker, comment = old_value.partition("#")
        spacing = before_comment[len(before_comment.rstrip()):] if marker else ""
        result.append(f"{key_part}={values[key]}{spacing}{marker}{comment}{newline}")
    missing = values.keys() - seen
    if missing:
        raise ValueError("Missing config keys: " + ", ".join(sorted(missing)))
    return "".join(result)


def load_state(path: Path) -> dict:
    """Read local checklist/install state, without silently discarding corruption."""
    state = {
        "checks": [],
        "notes": "",
        "profile": "baseline",
        "game_dir": r"D:\SteamLibrary\steamapps\common\Portal 2",
        "steam_exe": r"C:\Program Files (x86)\Steam\steam.exe",
        "managed_files": {},
        "original_files": {},
        "backup_id": "",
        "installed_game_dir": "",
        "unmanaged_checkpoint": False,
    }
    if not path.exists():
        return state
    try:
        loaded = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise ValueError(f"Cannot read launcher state: {error}") from error
    if not isinstance(loaded, dict):
        raise ValueError("Launcher state must be a JSON object")
    ownership_fields = {"managed_files", "original_files", "backup_id",
                        "installed_game_dir"}
    if not ownership_fields <= loaded.keys():
        raise ValueError("Incomplete launcher state ownership metadata")
    for key, expected in (("checks", list), ("notes", str), ("profile", str),
                          ("game_dir", str), ("steam_exe", str), ("managed_files", dict),
                          ("original_files", dict), ("backup_id", str),
                          ("installed_game_dir", str)):
        if key in loaded:
            if not isinstance(loaded[key], expected):
                raise ValueError(f"Invalid launcher state field: {key}")
            state[key] = loaded[key]
    if "unmanaged_checkpoint" in loaded:
        if type(loaded["unmanaged_checkpoint"]) is not bool:
            raise ValueError("Invalid launcher state field: unmanaged_checkpoint")
        state["unmanaged_checkpoint"] = loaded["unmanaged_checkpoint"]
    if any(not isinstance(item, str) for item in state["checks"]):
        raise ValueError("Invalid launcher checklist entry")
    for field in ("managed_files", "original_files"):
        if any(not isinstance(key, str) or not isinstance(value, str)
               for key, value in state[field].items()):
            raise ValueError(f"Invalid launcher {field} entry")
    if not state["managed_files"] and (state["original_files"] or
                                        state["backup_id"] or state["installed_game_dir"]):
        raise ValueError("Launcher state has inconsistent ownership metadata")
    if state["managed_files"] and state["unmanaged_checkpoint"]:
        raise ValueError("Managed launcher state cannot have an unmanaged checkpoint")
    return state


def save_state(path: Path, state: dict) -> None:
    """Atomically persist human observations and exact-file ownership."""
    _write_json_durable(path, state)


def _write_json_durable(path: Path, value: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    payload = (json.dumps(value, ensure_ascii=False, indent=2) + "\n").encode("utf-8")
    with tempfile.NamedTemporaryFile(prefix=path.name + ".", suffix=".tmp",
                                     dir=path.parent, delete=False) as stream:
        temporary = Path(stream.name)
    try:
        with temporary.open("wb") as stream:
            stream.write(payload)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


@contextmanager
def _transaction_lock(state_path: Path):
    """Serialize state and game-file operations across launcher processes."""
    lock_path = state_path.with_name(state_path.name + ".lock")
    lock_path.parent.mkdir(parents=True, exist_ok=True)
    with lock_path.open("a+b") as handle:
        if lock_path.stat().st_size == 0:
            handle.write(b"0")
            handle.flush()
        handle.seek(0)
        try:
            if os.name == "nt":
                msvcrt.locking(handle.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                fcntl.flock(handle.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as error:
            raise RuntimeError("Another launcher operation is in progress") from error
        try:
            yield
        finally:
            handle.seek(0)
            if os.name == "nt":
                msvcrt.locking(handle.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                fcntl.flock(handle.fileno(), fcntl.LOCK_UN)


def save_preferences(state_path: Path, ui_state: dict) -> dict:
    """Save human-entered fields without replacing install ownership metadata."""
    if ui_state["profile"] not in PROFILES:
        raise ValueError("Unknown test profile")
    with _transaction_lock(state_path):
        if _journal_path(state_path).exists() or _journal_path(state_path).is_symlink():
            raise RuntimeError("Interrupted launcher operation requires recovery before saving state")
        current = load_state(state_path)
        if not current["managed_files"] and not current["unmanaged_checkpoint"]:
            _check_unowned_backups(state_path)
            current["unmanaged_checkpoint"] = True
        for key in ("checks", "notes", "profile", "game_dir", "steam_exe"):
            current[key] = ui_state[key]
        save_state(state_path, current)
        return current


def _digest(contents: bytes) -> str:
    return hashlib.sha256(contents).hexdigest()


def _safe_target(game: Path, relative: str) -> Path:
    target = game / relative
    if target.is_symlink() or not target.resolve().is_relative_to(game.resolve()):
        raise ValueError(f"Unsafe game target: {target}")
    return target


def plan_install(repo: Path, game: Path, profile: str, *,
                 config_values: dict[str, str] | None = None) -> dict[Path, bytes]:
    """Read exact source payloads without writing to the game."""
    if config_values is not None:
        from .launcher_settings import validate_settings
        config_values = validate_settings(config_values)
    if not (game / "portal2.exe").is_file() or not (game / "bin").is_dir():
        raise FileNotFoundError(f"Not a Portal 2 installation: {game}")
    planned: dict[Path, bytes] = {}
    for source_relative, target_relative in SOURCE_TO_TARGET:
        source = repo / source_relative
        if not source.is_file():
            raise FileNotFoundError(f"Missing launcher source: {source}")
        payload = source.read_bytes()
        if target_relative == "VR/config.txt":
            template = payload.decode("utf-8")
            payload = (render_config(template, profile) if config_values is None else
                       apply_config_values(template, config_values)).encode("utf-8")
        planned[_safe_target(game, target_relative)] = payload
    return planned


def validate_launch_paths(repo: Path, game: Path, steam: Path) -> None:
    if not steam.is_file() or steam.name.lower() != "steam.exe":
        raise FileNotFoundError(f"Steam executable not found: {steam}")
    plan_install(repo, game, "baseline")


def build_steam_command(steam: Path) -> list[str]:
    return [str(steam), "-applaunch", "620", "-insecure", "-window", "-novid",
            "+mat_motion_blur_percent_of_screen_max", "0", "+mat_queue_mode", "0",
            "+mat_vsync", "0", "+mat_antialias", "0",
            "+mat_grain_scale_override", "0", "-width", "1280", "-height", "720"]


def _write_payload(target: Path, payload: bytes) -> None:
    target.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(prefix=target.name + ".portal2vr-", suffix=".tmp",
                                     dir=target.parent, delete=False) as stream:
        temporary = Path(stream.name)
    try:
        with temporary.open("wb") as stream:
            stream.write(payload)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, target)
    finally:
        temporary.unlink(missing_ok=True)


def _remove_empty_vr_directories(game: Path) -> None:
    for directory in (game / "VR/SteamVRActionManifest", game / "VR"):
        try:
            directory.rmdir()
        except (FileNotFoundError, OSError):
            pass


def _validate_managed(state: dict, game: Path) -> dict[str, str]:
    managed = state["managed_files"]
    allowed = {target for _, target in SOURCE_TO_TARGET}
    if any(relative not in allowed for relative in managed | state["original_files"]):
        raise ValueError("Launcher state contains an unexpected file path")
    if managed and managed.keys() != allowed:
        raise ValueError("Launcher state has an incomplete managed file set")
    if managed and state["installed_game_dir"] != str(game.resolve()):
        raise RuntimeError("Test files belong to another game directory; restore there first")
    if managed and not re.fullmatch(r"[0-9a-f]{32}", state["backup_id"]):
        raise ValueError("Launcher state has no valid backup identifier")
    if not managed and (state["original_files"] or state["backup_id"]):
        raise ValueError("Launcher state has inconsistent backup information")
    return managed


def _backup_dir(state_path: Path, backup_id: str) -> Path:
    if not re.fullmatch(r"[0-9a-f]{32}", backup_id):
        raise ValueError("Invalid backup identifier")
    root = state_path.parent / ".launcher-backups"
    if root.is_symlink():
        raise ValueError("Backup root must not be a symlink")
    snapshot = root / backup_id
    if snapshot.is_symlink():
        raise ValueError("Backup snapshot must not be a symlink")
    return snapshot


def _create_backup(game: Path, state_path: Path,
                   previous: dict[Path, bytes | None]) -> tuple[str, dict[str, str]]:
    backup_id = uuid.uuid4().hex
    snapshot = _backup_dir(state_path, backup_id)
    snapshot.mkdir(parents=True, exist_ok=False)
    originals: dict[str, str] = {}
    for target, contents in previous.items():
        if contents is None:
            continue
        relative = target.relative_to(game).as_posix()
        backup = snapshot / relative
        backup.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(target, backup)
        with backup.open("r+b") as stream:
            stream.flush()
            os.fsync(stream.fileno())
        shutil.copystat(target, backup)
        digest = _digest(contents)
        if _digest(backup.read_bytes()) != digest:
            raise RuntimeError(f"Backup verification failed: {backup}")
        originals[relative] = digest
    save_state(snapshot / "manifest.json", {
        "game_dir": str(game.resolve()), "original_files": originals,
    })
    return backup_id, originals


def _verified_backups(game: Path, state_path: Path, state: dict) -> dict[str, Path]:
    snapshot = _backup_dir(state_path, state["backup_id"])
    manifest = snapshot / "manifest.json"
    if manifest.is_symlink() or not manifest.is_file():
        raise RuntimeError(f"Backup manifest missing or unsafe: {manifest}")
    try:
        recorded = json.loads(manifest.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise RuntimeError(f"Cannot read backup manifest: {error}") from error
    originals = state["original_files"]
    if recorded != {"game_dir": str(game.resolve()), "original_files": originals}:
        raise RuntimeError("Backup manifest does not match staged game state")
    verified: dict[str, Path] = {}
    for relative, digest in originals.items():
        backup = snapshot / relative
        if (backup.is_symlink() or not backup.resolve().is_relative_to(snapshot.resolve())
                or not backup.is_file() or _digest(backup.read_bytes()) != digest):
            raise RuntimeError(f"Backup file changed or missing: {backup}")
        verified[relative] = backup
    return verified


def _retained_backup_manifests(state_path: Path):
    """Read and verify complete backup snapshots once per ownership check."""
    root = state_path.parent / ".launcher-backups"
    if not root.exists() and not root.is_symlink():
        return
    if root.is_symlink() or not root.is_dir():
        raise RuntimeError(f"Unsafe launcher backup root: {root}")
    allowed = {relative for _, relative in SOURCE_TO_TARGET}
    for snapshot in root.iterdir():
        if not re.fullmatch(r"[0-9a-f]{32}", snapshot.name):
            continue
        if snapshot.is_symlink() or not snapshot.is_dir():
            raise RuntimeError(f"Unsafe launcher backup snapshot: {snapshot}")
        manifest = snapshot / "manifest.json"
        if not manifest.exists() and not manifest.is_symlink():
            continue  # A process may have exited before backup completion.
        if manifest.is_symlink() or not manifest.is_file():
            raise RuntimeError(f"Unsafe launcher backup manifest: {manifest}")
        try:
            recorded = json.loads(manifest.read_text(encoding="utf-8"))
        except (OSError, UnicodeError, json.JSONDecodeError) as error:
            raise RuntimeError(f"Cannot read retained backup manifest: {error}") from error
        if (not isinstance(recorded, dict) or
                recorded.keys() != {"game_dir", "original_files"} or
                not isinstance(recorded["game_dir"], str) or
                not isinstance(recorded["original_files"], dict)):
            raise RuntimeError(f"Invalid retained backup manifest: {manifest}")
        game = Path(recorded["game_dir"])
        if str(game.resolve()) != recorded["game_dir"]:
            raise RuntimeError(f"Invalid retained backup game path: {manifest}")
        originals = recorded["original_files"]
        if any(not isinstance(relative, str) or relative not in allowed or
               not isinstance(digest, str) or not re.fullmatch(r"[0-9a-f]{64}", digest)
               for relative, digest in originals.items()):
            raise RuntimeError(f"Invalid retained backup manifest: {manifest}")
        _verified_backups(game, state_path,
                          {"backup_id": snapshot.name, "original_files": originals})
        yield game, originals


def _check_unowned_backups(state_path: Path) -> None:
    """An unchecked empty state may conceal an active install."""
    for recorded_game, originals in _retained_backup_manifests(state_path):
        for _, relative in SOURCE_TO_TARGET:
            target = _safe_target(recorded_game, relative)
            if _file_digest(target) != originals.get(relative):
                raise RuntimeError("Retained backup conflicts with missing launcher ownership: "
                                   f"{target}")


def _copy_backup(backup: Path, target: Path) -> None:
    target.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(prefix=target.name + ".portal2vr-", suffix=".tmp",
                                     dir=target.parent, delete=False) as stream:
        temporary = Path(stream.name)
    try:
        shutil.copyfile(backup, temporary)
        with temporary.open("r+b") as stream:
            stream.flush()
            os.fsync(stream.fileno())
        shutil.copystat(backup, temporary)
        os.replace(temporary, target)
    finally:
        temporary.unlink(missing_ok=True)


_OWNERSHIP_FIELDS = ("managed_files", "original_files", "backup_id", "installed_game_dir")


def _ownership(state: dict) -> dict:
    return {key: state[key].copy() if isinstance(state[key], dict) else state[key]
            for key in _OWNERSHIP_FIELDS}


def _journal_path(state_path: Path) -> Path:
    return state_path.with_name(state_path.name + ".transaction.json")


def _transaction_dir(state_path: Path, transaction_id: str) -> Path:
    if not re.fullmatch(r"[0-9a-f]{32}", transaction_id):
        raise ValueError("Invalid transaction identifier")
    root = state_path.parent / ".launcher-backups"
    transactions = root / ".transactions"
    snapshot = transactions / transaction_id
    if any(path.is_symlink() for path in (root, transactions, snapshot)):
        raise ValueError("Transaction backup path must not be a symlink")
    return snapshot


def _file_digest(target: Path) -> str | None:
    if target.is_symlink() or (target.exists() and not target.is_file()):
        raise RuntimeError(f"Unsafe transaction target: {target}")
    return _digest(target.read_bytes()) if target.is_file() else None


def _prepare_transaction(game: Path, state_path: Path, before_state: dict,
                         after_state: dict, before: dict[Path, bytes | None],
                         after: dict[Path, bytes | None]) -> dict:
    """Publish recoverable preimages before changing even one game file."""
    journal_path = _journal_path(state_path)
    if journal_path.exists() or journal_path.is_symlink():
        raise RuntimeError("Interrupted launcher operation requires recovery")
    transaction_id = uuid.uuid4().hex
    snapshot = _transaction_dir(state_path, transaction_id)
    snapshot.mkdir(parents=True, exist_ok=False)
    files = {}
    for target, contents in before.items():
        relative = target.relative_to(game).as_posix()
        prior = _digest(contents) if contents is not None else None
        desired = after[target]
        files[relative] = {"before": prior,
                           "after": _digest(desired) if desired is not None else None}
        if contents is not None:
            backup = snapshot / relative
            backup.parent.mkdir(parents=True, exist_ok=True)
            with backup.open("xb") as stream:
                stream.write(contents)
                stream.flush()
                os.fsync(stream.fileno())
            if _digest(backup.read_bytes()) != prior:
                raise RuntimeError(f"Transaction preimage verification failed: {backup}")
    if any(_file_digest(target) != files[target.relative_to(game).as_posix()]["before"]
           for target in before):
        raise RuntimeError("Game files changed while preparing launcher transaction")
    journal = {"version": 1, "id": transaction_id, "game_dir": str(game.resolve()),
               "before_state": _ownership(before_state),
               "after_state": _ownership(after_state), "files": files}
    _write_json_durable(journal_path, journal)
    return journal


def _verify_transaction_output(game: Path, journal: dict) -> None:
    for relative, expected in journal["files"].items():
        target = _safe_target(game, relative)
        if _file_digest(target) != expected["after"]:
            raise RuntimeError(f"Launcher transaction output verification failed: {target}")


def _read_transaction(state_path: Path) -> dict | None:
    path = _journal_path(state_path)
    if path.is_symlink():
        raise RuntimeError(f"Unsafe launcher transaction journal: {path}")
    if not path.exists():
        return None
    try:
        journal = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise RuntimeError(f"Cannot read launcher transaction journal: {error}") from error
    if (not isinstance(journal, dict) or journal.keys() !=
            {"version", "id", "game_dir", "before_state", "after_state", "files"}
            or type(journal["version"]) is not int or journal["version"] != 1
            or not isinstance(journal["game_dir"], str)
            or not isinstance(journal["before_state"], dict)
            or not isinstance(journal["after_state"], dict)
            or not isinstance(journal["files"], dict)):
        raise RuntimeError("Invalid launcher transaction journal")
    if not isinstance(journal["id"], str):
        raise RuntimeError("Invalid launcher transaction identifier")
    _transaction_dir(state_path, journal["id"])
    allowed = {target for _, target in SOURCE_TO_TARGET}
    if journal["files"].keys() != allowed:
        raise RuntimeError("Invalid launcher transaction file set")
    for ownership in (journal["before_state"], journal["after_state"]):
        if ownership.keys() != set(_OWNERSHIP_FIELDS):
            raise RuntimeError("Invalid launcher transaction ownership")
        if (not isinstance(ownership["managed_files"], dict)
                or not isinstance(ownership["original_files"], dict)
                or not isinstance(ownership["backup_id"], str)
                or not isinstance(ownership["installed_game_dir"], str)):
            raise RuntimeError("Invalid launcher transaction ownership")
        if any(not isinstance(key, str) or not isinstance(value, str)
               for field in ("managed_files", "original_files")
               for key, value in ownership[field].items()):
            raise RuntimeError("Invalid launcher transaction ownership")
    for entry in journal["files"].values():
        if (not isinstance(entry, dict) or entry.keys() != {"before", "after"}
                or any(value is not None and
                       (not isinstance(value, str) or not re.fullmatch(r"[0-9a-f]{64}", value))
                       for value in entry.values())):
            raise RuntimeError("Invalid launcher transaction file digest")
    before, after = journal["before_state"], journal["after_state"]
    if not before["managed_files"] and not after["managed_files"]:
        raise RuntimeError("Invalid launcher transaction ownership transition")
    if (before["managed_files"] and after["managed_files"]
            and (before["backup_id"] != after["backup_id"]
                 or before["original_files"] != after["original_files"]
                 or before["installed_game_dir"] != after["installed_game_dir"])):
        raise RuntimeError("Launcher transaction changed original backup ownership")
    for relative, entry in journal["files"].items():
        expected_before = (before["managed_files"].get(relative)
                           if before["managed_files"] else after["original_files"].get(relative))
        expected_after = (after["managed_files"].get(relative)
                          if after["managed_files"] else before["original_files"].get(relative))
        if entry != {"before": expected_before, "after": expected_after}:
            raise RuntimeError("Launcher transaction file digest disagrees with ownership")
    return journal


def _finish_transaction(state_path: Path, journal: dict) -> None:
    _journal_path(state_path).unlink()
    shutil.rmtree(_transaction_dir(state_path, journal["id"]))


def _recover_transaction_unlocked(state_path: Path) -> bool:
    journal = _read_transaction(state_path)
    if journal is None:
        return False
    game = Path(journal["game_dir"])
    if str(game.resolve()) != journal["game_dir"]:
        raise RuntimeError("Launcher transaction game path changed")
    state = load_state(state_path)
    ownership = _ownership(state)
    before_state, after_state = journal["before_state"], journal["after_state"]
    if ownership != before_state and ownership != after_state:
        raise RuntimeError("Launcher state changed during interrupted operation")
    for recorded in (before_state, after_state):
        _validate_managed({**state, **recorded}, game)
        if recorded["managed_files"]:
            _verified_backups(game, state_path, recorded)
    snapshot = _transaction_dir(state_path, journal["id"])
    if not snapshot.is_dir():
        raise RuntimeError("Launcher transaction preimages are missing")
    targets = {}
    for relative, expected in journal["files"].items():
        target = _safe_target(game, relative)
        backup = snapshot / relative
        if expected["before"] is not None:
            if (backup.is_symlink() or not backup.resolve().is_relative_to(snapshot.resolve())
                    or not backup.is_file()
                    or _digest(backup.read_bytes()) != expected["before"]):
                raise RuntimeError(f"Launcher transaction preimage changed or missing: {backup}")
        current = _file_digest(target)
        if current not in (expected["before"], expected["after"]):
            raise RuntimeError(f"Game file changed outside interrupted launcher operation: {target}")
        if ownership == after_state and current != expected["after"]:
            raise RuntimeError(f"Completed launcher operation has changed file: {target}")
        targets[target] = (current, expected, backup)
    if ownership == before_state and ownership != after_state:
        for target, (current, expected, backup) in targets.items():
            if current == expected["before"]:
                continue
            if expected["before"] is None:
                target.unlink()
            else:
                _write_payload(target, backup.read_bytes())
        _remove_empty_vr_directories(game)
    if any(_file_digest(target) !=
           data[1]["after" if ownership == after_state else "before"]
           for target, data in targets.items()):
        raise RuntimeError("Launcher transaction recovery verification failed")
    _finish_transaction(state_path, journal)
    return True


def recover_pending_transaction(state_path: Path) -> bool:
    """Restore the pre-operation files after an interrupted launcher process."""
    with _transaction_lock(state_path):
        return _recover_transaction_unlocked(state_path)


def _stage_install_unlocked(repo: Path, game: Path, state_path: Path, profile: str, *,
                            config_values: dict[str, str] | None = None) -> list[Path]:
    """Stage exact mod files after snapshotting originals; roll back failures."""
    planned = plan_install(repo, game, profile, config_values=config_values)
    state = load_state(state_path)
    managed = _validate_managed(state, game)
    planned_paths = {target.relative_to(game).as_posix() for target in planned}
    if managed and managed.keys() != planned_paths:
        raise RuntimeError("Launcher version changed its file set; restore first")
    previous: dict[Path, bytes | None] = {}
    if managed:
        _verified_backups(game, state_path, state)
    for target in planned:
        relative = target.relative_to(game).as_posix()
        if target.is_symlink():
            raise ValueError(f"Refusing symlink: {target}")
        if target.exists():
            contents = target.read_bytes()
            if managed and _digest(contents) != managed[relative]:
                raise RuntimeError(f"Managed file changed outside launcher: {target}")
            previous[target] = contents
        else:
            if managed:
                raise RuntimeError(f"Managed file disappeared outside launcher: {target}")
            previous[target] = None
    backup_id, originals = (state["backup_id"], state["original_files"])
    if not managed:
        if not state["unmanaged_checkpoint"]:
            _check_unowned_backups(state_path)
        backup_id, originals = _create_backup(game, state_path, previous)
    next_state = {**state, "managed_files": {
        target.relative_to(game).as_posix(): _digest(payload)
        for target, payload in planned.items()},
        "original_files": originals, "backup_id": backup_id,
        "installed_game_dir": str(game.resolve()), "game_dir": str(game),
        "profile": profile, "unmanaged_checkpoint": False}
    journal = _prepare_transaction(game, state_path, state, next_state, previous,
                                   {target: payload for target, payload in planned.items()})
    try:
        for target, payload in planned.items():
            if previous[target] == payload:
                continue
            _write_payload(target, payload)
        _verify_transaction_output(game, journal)
        save_state(state_path, next_state)
    except Exception as error:
        try:
            _recover_transaction_unlocked(state_path)
        except Exception as rollback_error:
            raise RuntimeError(f"Staging failed and rollback is incomplete: {rollback_error}") from error
        raise
    _finish_transaction(state_path, journal)
    return list(planned)


def stage_install(repo: Path, game: Path, state_path: Path, profile: str, *,
                  config_values: dict[str, str] | None = None) -> list[Path]:
    with _transaction_lock(state_path):
        _recover_transaction_unlocked(state_path)
        return _stage_install_unlocked(repo, game, state_path, profile, config_values=config_values)


def _restore_install_unlocked(game: Path, state_path: Path) -> list[Path]:
    """Restore pre-existing files, remove created ones, and retain backups."""
    state = load_state(state_path)
    managed = _validate_managed(state, game)
    if not managed:
        if not state["unmanaged_checkpoint"]:
            _check_unowned_backups(state_path)
        return []
    backups = _verified_backups(game, state_path, state)
    targets: list[Path] = []
    for relative, digest in managed.items():
        target = _safe_target(game, relative)
        if target.is_symlink() or not target.is_file() or _digest(target.read_bytes()) != digest:
            raise RuntimeError(f"Managed file changed outside launcher: {target}")
        targets.append(target)
    staged_payloads = {target: target.read_bytes() for target in targets}
    next_state = {**state, "managed_files": {}, "original_files": {},
                  "backup_id": "", "installed_game_dir": "",
                  "unmanaged_checkpoint": True}
    desired = {target: backups[target.relative_to(game).as_posix()].read_bytes()
               if target.relative_to(game).as_posix() in backups else None
               for target in targets}
    journal = _prepare_transaction(game, state_path, state, next_state,
                                   staged_payloads, desired)
    try:
        for target in targets:
            relative = target.relative_to(game).as_posix()
            if relative in backups:
                _copy_backup(backups[relative], target)
            else:
                target.unlink()
        _verify_transaction_output(game, journal)
        save_state(state_path, next_state)
    except Exception as error:
        try:
            _recover_transaction_unlocked(state_path)
        except Exception as rollback_error:
            raise RuntimeError(f"Restore failed ({error}) and rollback is incomplete: {rollback_error}") from error
        raise
    _finish_transaction(state_path, journal)
    _remove_empty_vr_directories(game)
    return targets


def restore_install(game: Path, state_path: Path) -> list[Path]:
    with _transaction_lock(state_path):
        _recover_transaction_unlocked(state_path)
        return _restore_install_unlocked(game, state_path)
