"""Legacy HUD settings and strict compatibility for saved user preferences."""
import json
from pathlib import Path
from tempfile import TemporaryDirectory
import unittest

from tools import launcher_settings as settings
from tools import test_launcher_core as core

NEW_DEFAULTS = {"HUDInEyeCentered": "false", "HUDInEyeScale": "0.45",
                "HUDInEyeVerticalOffset": "0.05", "ShowSourceCrosshair": "true"}


class EyeHudSettingsTests(unittest.TestCase):
    def test_new_settings_preserve_recommended_profile(self):
        values = settings.recommended_settings()
        for key, value in NEW_DEFAULTS.items():
            self.assertEqual(values.get(key), value, key)
        self.assertEqual(values["AimMode"], "2")
        self.assertEqual(values["ExperimentalHUDOverlay"], "true")

    def test_centering_dependencies_are_explicit(self):
        values = settings.recommended_settings()
        values.update(NEW_DEFAULTS, HUDInEyeCentered="true")
        with self.assertRaisesRegex(ValueError, "HUDInEyeCentered requires"):
            settings.validate_settings(values)
        values["AimMode"] = "1"
        with self.assertRaisesRegex(ValueError, "HUDInEyeCentered requires"):
            settings.validate_settings(values)
        values["ExperimentalHUDOverlay"] = "false"
        self.assertEqual(settings.validate_settings(values)["HUDInEyeCentered"], "true")

    def test_old_complete_preferences_preserve_paths_values_and_file(self):
        old_values = {k: v for k, v in settings.recommended_settings().items() if k not in NEW_DEFAULTS}
        old_values.update(SnapTurning="true", TurnSpeed="0.27", RenderWindow="1")
        old = {"version": 1, "game_dir": "D:/My Game", "steam_exe": "C:/Steam/steam.exe", "values": old_values}
        with TemporaryDirectory() as temporary:
            path = Path(temporary) / "preferences.json"
            original = json.dumps(old).encode()
            path.write_bytes(original)
            migrated = settings.load_preferences(path, {})
            self.assertEqual(migrated["values"], {**old_values, **NEW_DEFAULTS})
            self.assertEqual(migrated["game_dir"], old["game_dir"])
            self.assertEqual(migrated["steam_exe"], old["steam_exe"])
            self.assertEqual(path.read_bytes(), original)

    def test_unknown_incomplete_and_partial_old_schemas_are_rejected(self):
        old_values = {k: v for k, v in settings.recommended_settings().items() if k not in NEW_DEFAULTS}
        variants = [dict(old_values, Unexpected="true"), dict(old_values, HUDInEyeCentered="false")]
        incomplete = dict(old_values)
        del incomplete["AimMode"]
        variants.append(incomplete)
        with TemporaryDirectory() as temporary:
            path = Path(temporary) / "preferences.json"
            for values in variants:
                original = json.dumps({"version": 1, "game_dir": "D:/Game", "steam_exe": "C:/Steam.exe", "values": values}).encode()
                path.write_bytes(original)
                with self.assertRaises(ValueError):
                    settings.load_preferences(path, {})
                self.assertEqual(path.read_bytes(), original)

    def test_hud_range_boundaries_and_invalid_values(self):
        values = settings.recommended_settings()
        for scale in ("0.2", "1"):
            for offset in ("-0.4", "0.4"):
                updated = {**values, "HUDInEyeScale": scale, "HUDInEyeVerticalOffset": offset}
                self.assertEqual(settings.validate_settings(updated)["HUDInEyeScale"], scale)
        for key, invalid in {"HUDInEyeScale": ("0.19", "1.01", "nan", "inf"),
                             "HUDInEyeVerticalOffset": ("-0.41", "0.41", "nan", "inf"),
                             "HUDInEyeCentered": ("yes",), "ShowSourceCrosshair": ("0",)}.items():
            for value in invalid:
                with self.subTest(key=key, value=value), self.assertRaisesRegex(ValueError, key):
                    settings.validate_settings({**values, key: value})

    def test_migrated_preferences_save_reload_and_generate_custom_preview(self):
        old_values = {k: v for k, v in settings.recommended_settings().items() if k not in NEW_DEFAULTS}
        old_values.update(AimMode="1", ExperimentalHUDOverlay="false", TurnSpeed="0.27")
        old = {"version": 1, "game_dir": "D:/My Game", "steam_exe": "C:/Steam/steam.exe", "values": old_values}
        with TemporaryDirectory() as temporary:
            path = Path(temporary) / "preferences.json"
            path.write_text(json.dumps(old), encoding="utf-8")
            migrated = settings.load_preferences(path, {})
            migrated["values"].update(HUDInEyeCentered="true", HUDInEyeScale="0.6",
                                      HUDInEyeVerticalOffset="-0.2", ShowSourceCrosshair="false")
            settings.save_preferences(path, migrated)
            self.assertEqual(settings.load_preferences(path, {}), migrated)
            template = (Path(__file__).resolve().parent.parent / "L4D2VR/config.txt").read_text(encoding="utf-8")
            preview = core.apply_config_values(template, migrated["values"])
            rendered = dict(line.split("#", 1)[0].strip().split("=", 1) for line in preview.splitlines()
                            if "=" in line.split("#", 1)[0])
            self.assertEqual(rendered, migrated["values"])
            self.assertEqual(json.loads(path.read_text())["game_dir"], old["game_dir"])
            self.assertEqual(json.loads(path.read_text())["steam_exe"], old["steam_exe"])

    def test_old_complete_schema_still_rejects_invalid_old_value(self):
        old_values = {k: v for k, v in settings.recommended_settings().items() if k not in NEW_DEFAULTS}
        old_values["TurnSpeed"] = "nan"
        with TemporaryDirectory() as temporary:
            path = Path(temporary) / "preferences.json"
            original = json.dumps({"version": 1, "game_dir": "D:/Game", "steam_exe": "C:/Steam.exe", "values": old_values})
            path.write_text(original, encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "TurnSpeed"):
                settings.load_preferences(path, {})
            self.assertEqual(path.read_text(encoding="utf-8"), original)


if __name__ == "__main__":
    unittest.main()
