# ASA-Plugins
> Custom C++ plugins for **ARK: Survival Ascended** (ASA) built on the [AsaApi / DevAPI](https://github.com/ServersHub/ServerAPI) framework.
> Author: **Kxrse**. Non-Commercial, attribution required. See [LICENSE.txt](LICENSE.txt).
---
## Plugins
| Plugin | Status | Description |
|---|---|---|
| [AutoAdmin](AutoAdmin/) | Released | Persists admin by EOS ID, granting configured players admin a set delay after spawn. Blocks non-whitelisted cheat commands for restricted admins, runs ForceOnSpawn commands, and revokes then kicks downgraded admins from the Permissions group, with live config hot-reload. |
| [AutoTribe](AutoTribe/) | Released | Forces any tribeless player into a solo tribe with a generated four word name. Fires on spawn, respawn, transfer arrival, and on leaving or being kicked from a tribe, retrying until the tribe takes. Names are drawn from an editable word list and capped to the in-game name limit. Word lists and config hot-reload live. No database. |
| [Blockade](Blockade/) | Released | Owns combat and raid block state for the cluster. Combat fires on real damage from an enemy player or tamed dino. Raid zones form on real enemy structure damage, merge within a configured radius, and expire after their last hit, blocking any player standing inside. Both show an on-screen marker and mirror to a database for other plugins to read, scoped per map, with an EOS immunity list and live config hot-reload. |
| [Census](Census/) | Released | Posts a per-map online roster to Discord as a single edit-in-place embed, listing EOS ID, survivor name, implant ID, and tribe for every player online. Each map owns its own message and reposts it if deleted. Per-map embed titles and colors, and the embed is only edited when the roster actually changes. No database. |
| [Relay](Relay/) | Released | Two-way chat bridge between in-game chat and a Discord channel. Relays out via webhook using the map display name, polls back in via bot token, tags cross-map messages, and never relays slash commands. No database. |
| [StatCap](StatCap/) | Released | Caps applied level-up points per stat for survivors and tamed dinos, plus a total tamed-points cap, with per-species overrides. Denies over-cap spends live and optionally destroys over-cap tames on world entry. |
| [StructureStats](StructureStats/) | Released | Tracks per-player and per-tribe structure placements and destructions to a database across a cluster. |
| [SurvivorTracker](SurvivorTracker/) | Released | Tracks survivor identity and tribe membership per map to a database across a cluster. |
| [TurretFiller](TurretFiller/) | Released | Refills nearby turrets with ammo pulled from a player's inventory via a chat command. |
| [TurretSlotCap](TurretSlotCap/) | Released | Caps the usable ammo slots on turrets by blueprint path. Config hot-reloads live. No database. |
| [UnlockAll](UnlockAll/) | Released | Grants engrams, explorer notes, and skills on join, gated per Permissions group by a priority ranked tier system with a Default tier for everyone else. Waits until the character exists before granting, so it covers first spawn, reconnects, and transfer arrivals, and re-grants after a mindwipe or on new character creation. Engrams are per tier, either all or tek only, and the Lost Colony notes unlock for players who own it. Config hot-reloads live. No database. |
---
## License
[Kxrse ASA Plugins Non-Commercial License](LICENSE.txt)
Free to use, modify, and redistribute **with attribution**.
Commercial use or resale requires explicit written permission.
