# ASA-Plugins

> Custom C++ plugins for **ARK: Survival Ascended** (ASA) built on the [AsaApi / DevAPI](https://github.com/ServersHub/ServerAPI) framework.
> Author: **Kxrse**. Non-Commercial, attribution required. See [LICENSE.txt](LICENSE.txt).

---

## Plugins

| Plugin | Status | Description |
|---|---|---|
| [AutoAdmin](AutoAdmin/) | Released | Persists admin by EOS ID, granting configured players admin a set delay after spawn. Blocks non-whitelisted cheat commands for restricted admins, runs ForceOnSpawn commands, and revokes then kicks downgraded admins from the Permissions group, with live config hot-reload. |
| [Relay](Relay/) | Released | Two-way chat bridge between in-game chat and a Discord channel. Relays out via webhook using the map display name, polls back in via bot token, tags cross-map messages, and never relays slash commands. No database. |
| [StatCap](StatCap/) | Released | Caps applied level-up points per stat for survivors and tamed dinos, plus a total tamed-points cap, with per-species overrides. Denies over-cap spends live and optionally destroys over-cap tames on world entry. |
| [StructureStats](StructureStats/) | Released | Tracks per-player and per-tribe structure placements and destructions to a database across a cluster. |
| [SurvivorTracker](SurvivorTracker/) | Released | Tracks survivor identity and tribe membership per map to a database across a cluster. |
| [TurretFiller](TurretFiller/) | Released | Refills nearby turrets with ammo pulled from a player's inventory via a chat command. |
| [TurretSlotCap](TurretSlotCap/) | Released | Caps the usable ammo slots on turrets by blueprint path. Config hot-reloads live. No database. |

---

## License

[Kxrse ASA Plugins Non-Commercial License](LICENSE.txt)
Free to use, modify, and redistribute **with attribution**.
Commercial use or resale requires explicit written permission.
