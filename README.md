# ASA-Plugins
> Custom C++ plugins for **ARK: Survival Ascended** (ASA) built on the [AsaApi / DevAPI](https://github.com/ServersHub/ServerAPI) framework.  
> Author: **Kxrse** - Non-Commercial, attribution required. See [LICENSE.txt](LICENSE.txt).
---
## Plugins
| Plugin | Status | Description |
|---|---|---|
| [EnhancedTeleporting](EnhancedTeleporting/) | Released | Home teleport system, per-tier permissions, foundation checks, combat blocking, and raid zone blocking. Commands: `/sethome`, `/home`, `/delhome`, `/listhome`, `/tpr`, `/tpa`, `/tpc`. |
| [AutoAdmin](AutoAdmin/) | Released | Per-admin config with enable/disable, command whitelisting, ForceOnSpawn commands, cheat blocking, and live config hot-reload with automatic kick on permission downgrade. |
| [DinoPainter](DinoPainter/) | Released | Paint tribe dinos by region or in bulk using saved presets. Permission-gated with per-group radius limits. Commands: `/paint`, `/paintall`, `/savepaint`, `/paintpresets`, `/paintdel`. |
| [AutoDoors](AutoDoors/) | Released | Automatically closes doors, hatches, and trapdoors after a configurable delay. Per-player toggle and custom delay with DB persistence. Commands: `/ad`, `/ad {seconds}`. |
| [TurretFiller](TurretFiller/) | Released | Fills nearby auto, heavy, and tek turrets with ammo from player inventory using balanced distribution. Per-player DB-persistent fill range, per-turret combat cooldown, mounted support, and permission-gated group tiers. Commands: `/fill`, `/fillrange`. |
| [TurretFPS](TurretFPS/) | Released | Suppresses tek turret projectile and impact effects to reduce client particle load. Damage is unaffected; auto and heavy turrets are not changed. |
| [TidyDams](TidyDams/) | Released | Automatically destroys beaver dams when a player closes the inventory and anything apart from Cementing Paste remains inside. |
| [SurvivorTracker](SurvivorTracker/) | Released | Tracks player identity and tribe membership per map to a database across a cluster. |
| [TribeWarden](TribeWarden/) | Released | Forces all players to always be in a tribe and automatically creates a solo tribe for any tribeless player. |
| [SurvivorStats](SurvivorStats/) | Released | Tracks per-character stats including level, kills, and deaths to a database. |
| [HarvestStats](ResourceStats/) | Released | Tracks per-character harvested resource totals to a database. |
| [StructureStats](StructureStats/) | Released | Tracks per-character and per-tribe structure placements and destructions to a database. |
| [LinkInChat](LinkInChat/) | Released | Allows admins to configure custom `/commands` that respond with a configured message in chat, with cooldown support. |
| [HarvestScale](HarvestScale/) | Released | Scales harvest rates based on tribe size per map using config-driven rate tiers. |
| [UnlockAll](UnlockAll/) | Released | Automatically unlocks engrams, tek engrams, Lost Colony skills, and explorer notes on spawn. Permission-gated with per-group toggles and Lost Colony DLC ownership check. |
| [PlayerRename](PlayerRename/) | Released | Allows players to rename their survivor via chat command with configurable length limits and a blocked names filter. Command: `/rename`. |
| [PrivateMessages](PrivateMessages/) | Released | Private messaging between survivors with duplicate name disambiguation and reply support. Commands: `/pm`, `/r`. |
| [FoundationParity](FoundationParity/) | Released | Gives walls and ceilings parity with foundations under the enemy-foundation proximity rule, blocking placement within the enemy-foundation prevention radius of an enemy foundation, wall, or ceiling. |
---
## License
[Kxrse ASA Plugins Non-Commercial License](LICENSE.txt)  
Free to use, modify, and redistribute **with attribution**.  
Commercial use or resale requires explicit written permission.
