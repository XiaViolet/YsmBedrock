# Java animation input references

The generated lookup contains registry identifiers and item, entity and block tag membership from Minecraft Java 1.20.1, Forge 47.4.3, and YSM commit 15c2c25f5b50f54ee099bc474483dc4b588d956d. Versions and input digests are recorded in java-animation-tags-source.json. Game and Forge binaries are verification inputs in vendor/minecraft-1.20.1 and are not included in the release archive.

Minecraft download metadata: https://piston-meta.mojang.com/mc/game/version_manifest_v2.json

Forge artifact: https://maven.minecraftforge.net/net/minecraftforge/forge/1.20.1-47.4.3/forge-1.20.1-47.4.3-universal.jar

YSM tags and classification: https://github.com/YesSteveModel/YesSteveModel/tree/15c2c25f5b50f54ee099bc474483dc4b588d956d/src/main/resources/data/ysm/tags/items

The wing-roll calculation is independently expressed in C++ from the behavior of the matching official Java client ElytraModel, verified against the SHA1-checked client bytecode. No game class or decompiled implementation is distributed.

The long-spear and ordinary boat memberships use `data/minecraft/tags/item/spears.json`
and `data/minecraft/tags/entity_type/boat.json` from Minecraft Java 26.1.2.
The Bedrock `minecraft:is_spear` tag and shared `minecraft:boat` entity ID are
mapped to those groups. The verified client SHA-1 is
`4e618f09a0c649dde3fdf829df443ce0b8831e65`.
