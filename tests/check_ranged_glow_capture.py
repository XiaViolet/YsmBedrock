"""Check a single Bedrock play session with Eri, one bow shot and one crossbow shot.

Pass animation-status.json and target-render-probe-status.json captured while
both arrows were in view. Use the original pack and keep the Arke overlay off.
"""
import argparse
import json
from pathlib import Path


def check(animation, records):
    actions = set(animation["hands"]["animations_seen"])
    for required in ("use_mainhand:bow", "use_mainhand:crossbow", "hold_mainhand:charged_crossbow"):
        if required not in actions:
            raise ValueError(f"missing hand animation: {required}")
    visual = animation["bone_visual_render"]
    if not visual["surface_route_supported"] or not visual["individual_actor_requests"]:
        raise ValueError("world renderer did not route an individual actor")
    if not visual["glow_passes"] or not visual["light_overrides"] or visual["arke_glow_layers"]:
        raise ValueError("expected authored bone light with the extra Arke layer disabled")
    by_actor = {}
    for entry in records:
        if entry.get("actor") == "minecraft:arrow" and entry["stage"] == "native_frame_prepared":
            by_actor.setdefault(entry["actor_id"], []).append(entry)
    verified = {}
    for actor, frames in by_actor.items():
        items = {frame["animation_input"]["shoot_item_id"] for frame in frames}
        stages = {frame["animation_input"]["in_ground"] for frame in frames}
        drew_model = any(frame["material"]["mesh_draws"] > 0 for frame in frames)
        if len(items) == 1 and stages == {False, True} and drew_model:
            verified.setdefault(items.pop(), []).append(actor)
    for item in ("minecraft:bow", "minecraft:crossbow"):
        if not verified.get(item):
            raise ValueError(f"missing one rendered replacement arrow tracked from flight to impact: {item}")
    return verified


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("animation", type=Path)
    parser.add_argument("targets", type=Path)
    args = parser.parse_args()
    try:
        print(check(json.loads(args.animation.read_text("utf-8-sig")),
                    json.loads(args.targets.read_text("utf-8-sig"))))
    except (KeyError, ValueError) as err:
        parser.exit(1, f"FAIL: {err}\n")
