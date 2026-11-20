"""Run existing YsmBedrock regression suites for the affected features."""

import argparse
from pathlib import Path
import subprocess
import sys


PROJECT = Path(__file__).resolve().parents[1]
TESTS = PROJECT / "tests"

# A suite can cover more than one feature. Full discovery also picks up new suites.
SUITES = {
    "import": (
        "import", "model_library", "model_textures", "content_identity",
        "image_codec", "avif_codec", "webp_codec", "animation_compatibility",
    ),
    "animation": (
        "animation_compatibility", "coded_playback", "controllers", "source_controllers",
        "rotation_transitions", "bone_reset", "first_person", "armor_animations", "hand_animations",
    ),
    "script": (
        "molang", "molang_objects", "source_math", "physics", "model_functions",
        "source_actions", "deferred_events", "bone_query_phases", "sync_events",
    ),
    "inputs": (
        "entity_queries", "host_input_queries", "java_vanilla_inputs", "weather_events",
        "replaced_controllers",
    ),
    "render": (
        "bone_identity", "bone_visuals", "client_geometry", "render_space",
        "render_observation", "first_person", "replaced_geometry",
    ),
    "entities": (
        "replaced_targets", "replaced_assignments", "replaced_controllers", "replaced_geometry",
    ),
    "effects": ("model_audio", "model_particles", "source_actions", "deferred_events"),
    "ui": ("ysm_ui", "key_bindings", "model_library"),
    "sync": ("roaming", "roaming_session", "sync_events", "replaced_assignments"),
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("features", nargs="*", help="Feature names, or all for full discovery")
    parser.add_argument("--list", action="store_true", help="List features and test modules")
    args = parser.parse_args()
    if args.list:
        for feature, suites in SUITES.items():
            print(f"{feature}: {', '.join('test_' + suite for suite in suites)}")
        return 0
    if not args.features:
        parser.error("Select a feature or all. Use --list to see the suites.")
    command = [sys.executable, "-B", "-m", "unittest"]
    if args.features == ["all"]:
        return subprocess.run(command + ["discover", "-s", "tests"], cwd=PROJECT).returncode
    for feature in args.features:
        if feature not in SUITES:
            parser.error(f"Unknown feature or invalid combination: {feature}. Use --list.")
    modules = dict.fromkeys("test_" + suite for feature in args.features for suite in SUITES[feature])
    return subprocess.run(command + list(modules), cwd=TESTS).returncode


if __name__ == "__main__":
    sys.exit(main())
