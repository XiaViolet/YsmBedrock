`src/DefaultAnimations.inc` uses the default template in the official Java
YSM 2.6.5 NeoForge 26.1.2 hotfix release. The archive path is
`assets/yes_steve_model/builtin/default/`. [Official release](https://modrinth.com/mod/yes-steve-model/version/2.6.5-neoforge%2Bmc26.1-hotfix)

The authoritative asset manifest, ysm.json, declares metadata.license.type as
"CC 0". These assets are separate from YSM's Apache-2.0 source code license.

[CC0 1.0 Universal](https://creativecommons.org/publicdomain/zero/1.0/)

The generator verifies the release archive's SHA-512. It preserves the clip
data and embeds 341 body, 8 first-person and 58 vehicle/projectile clips as
minified JSON. The generated include records each source file's SHA-256.

Seven vehicle/projectile domains follow YSM's `DefaultAnimationKey` matching.
The official `#minecraft:boat` domain also supplies the defaults to the older
single `minecraft:boat` selector.
