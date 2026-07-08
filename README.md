# ASA-Plugins
> Custom C++ plugins for **ARK: Survival Ascended** (ASA) built on the [AsaApi / DevAPI](https://github.com/ServersHub/ServerAPI) framework.  
> Author: **Kxrse** - Non-Commercial, attribution required. See [LICENSE.txt](LICENSE.txt).
---
## Plugins
| Plugin | Status | Description |
|---|---|---|
| [EnhancedTeleporting](EnhancedTeleporting/) | Released | Home teleport system with per-tier permissions and foundation checks. Reads cluster combat and raid block state from Blockade, gated by independent config toggles. Commands: `/sethome`, `/home`, `/delhome`, `/listhome`, `/tpr`, `/tpa`, `/tpc`. |
| [AutoAdmin](AutoAdmin/) | Released | Per-admin config with enable/disable, command whitelisting, ForceOnSpawn commands, cheat blocking, and live config hot-reload with automatic kick on permission downgrade. |
| [DinoPainter](DinoPainter/) | Released | Paint tribe dinos by region or in bulk using saved presets. Permission-gated with per-group radius limits. Commands: `/paint`, `/paintall`, `/savepaint`, `/paintpresets`, `/paintdel`. |
| [AutoDoors](AutoDoors/) | Released | Automatically closes doors, hatches, and trapdoors after a configurable delay. Per-player toggle and custom delay with DB persistence. Commands: `/ad`, `/ad {seconds}`. |
| [TurretFiller](TurretFiller/) | Released | Fills nearby auto, heavy, and tek turrets with ammo from player inventory using balanced distribution. Per-player DB-persistent fill range, per-turret combat cooldown, mounted support, and permission-gated group tiers. Commands: `/fill`, `/fillrange`. |
| [TurretFPS](TurretFPS/) | Released | Suppresses tek turret projectile and impact effects to reduce client particle load. Damage is unaffected; auto and heavy turrets are not changed. |
| [TidyDams](TidyDams/) | Released | Automatically destroys beaver dams when a player closes the inventory and anything apart from Cementing Paste remains inside. |
| [SurvivorTracker](SurvivorTracker/) | Released | Tracks player identity and tribe membership per map to a database across a cluster. |
| [TribeWarden](TribeWarden/) | Released | Forces all players to always be in a tribe and automatically creates a solo tribe for any tribeless player. |
| [SurvivorStats](SurvivorStats/) | Released | Tracks per-character stats including level, kills, and deaths to a database. |
| [ResourceStats](ResourceStats/) | Released | Tracks per-character harvested resource totals to a database. |
| [StructureStats](StructureStats/) | Released | Tracks per-character and per-tribe structure placements and destructions to a database. |
| [LinkInChat](LinkInChat/) | Released | Allows admins to configure custom `/commands` that respond with a configured message in chat, with cooldown support. |
| [HarvestScale](HarvestScale/) | Released | Scales harvest rates based on tribe size per map using config-driven rate tiers. |
| [UnlockAll](UnlockAll/) | Released | Automatically unlocks engrams, tek engrams, Lost Colony skills, and explorer notes on spawn. Permission-gated with per-group toggles and Lost Colony DLC ownership check. |
| [PlayerRename](PlayerRename/) | Released | Allows players to rename their survivor via chat command with configurable length limits and a blocked names filter. Command: `/rename`. |
| [PrivateMessages](PrivateMessages/) | Released | Private messaging between survivors with duplicate name disambiguation and reply support. Commands: `/pm`, `/r`. |
| [FoundationParity](FoundationParity/) | Released | Gives walls and ceilings parity with foundations under the enemy-foundation proximity rule, blocking placement within the enemy-foundation prevention radius of an enemy foundation, wall, or ceiling. |
| [SneakyFoundyFinder](SneakyFoundyFinder/) | Released | Helps players locate meshed enemy foundations when raiding or rebuilding by team-pinging the nearest enemy foundation, floor, or pillar in range. Skips own tribe. A scan radius of 5100 matches the range where ARK's "too close to enemy foundation" placement block is seen. Command: `/sff`. |
| [Census](Census/) | Released | Posts a live per-map online roster to Discord as a single edit-in-place embed, showing each player's EOS ID, survivor name, implant ID, tribe name, and tribe ID. Configurable update cadence with per-map display name and color overrides for modded map support. |
| [YutyAutoRoar](YutyAutoRoar/) | Released | Makes an aimed tribe-owned Yuty fear or courage roar on a configurable interval until stopped, with optional stamina refill and DB persistence that survives reboots and cryo. Tracked Yutys are cleared on death. Commands: `/yar fear`, `/yar courage`, `/yar off`. |
| [DropDropper](DropDropper/) | Released | Whitelisted admin tool for spawning supply beacons on the caller. Single and cave drops by colour keyword, per-map mass drops with config-driven blueprint paths for modded map support, and a random party spread across the higher tiers. Commands: `/drop`, `/dropc`, `/massdrop`, `/dropparty`. |
| [Kits](Kits/) | Released | Permission-gated kit redemption granting configured items and dino cryopods. Per-rank cooldowns and max uses with DB persistence, vanilla or Pelayori cryopod support, and config hot-reload. Commands: `/kits`, `/kit {name}`. |
| [CloudStorage](CloudStorage/) | Released | Permission-gated cross-map item bank backed by a shared cluster database. Stores stackable items, gear with full stat and durability fidelity, and blueprints with quality preserved (config-gated). Per-group slot allowances, name abbreviations, and a blueprint blacklist. Eggs and cryopods are refused. Bulk store and retrieve, with a `bp` keyword to separate blueprints from their crafted form. Commands: `/upload`, `/download`, `/ulist`, `/uploadall`, `/downloadall`. |
| [Blockade](Blockade/) | Released | Cluster-wide combat and raid block state owned by one plugin and shared via database. Combat block on real PvP damage from players, enemy tames, or hostile dinos; positional raid zones from enemy structure damage including C4 and turret fire, covering attackers and defenders alike. EOS immunity list and fully config-driven messages and timings. |
| [WildLimiter](WildLimiter/) | Released | Prevents or limits wild dino spawns by blueprint match. Hard block despawns listed species, per-species live caps, and per-map overrides that replace the defaults. Admin and GMSummon spawns are left untouched via deferred judgement, and a sweep clears existing matches on a config change. |
| [ShadowBan](ShadowBan/) | Released | Cluster-wide shadowban that pins a player to a fixed location and teleports them back when they move beyond a set leash distance. Bans are shared across the cluster via database with per-map pins, so a transfer re-pins the player on the new map at the next sync. RCON ONLY. Commands: `shadowban {eos_id}`, `unshadowban {eos_id}`. |
| [InstaKits](InstaKits/) | Released | Permission-gated kits granted automatically on spawn, highest rank winning when a player holds several. Per-kit recurring or one-time delivery with DB persistence, per-item quality tier, hotbar slot placement, and auto-equip, plus an optional tek engram unlock. Config hot-reload. See [Help.txt](InstaKits/Help.txt) for config setup. |
| [FiberScriber](FiberScriber/) | Released | Admin generator tool that rewrites all crafting costs to fiber. On command it enumerates every craftable item on the server via the master item list and engram lists, then writes `ConfigOverrideItemCraftingCosts` lines into Game.ini, covering base game and modded items and skipping anything already overridden. Atomic write with a backup, and overrides apply on next server boot. Console command: `Cheat GenFiberCosts`. |
| [TurretSlotCap](TurretSlotCap/) | Released | Caps the usable ammo slots on turrets per type using a blueprint-substring config. Writes MaxInventoryItems and AbsoluteMaxInventoryItems on placement and on server load, so the cap is enforced for both `/fill` and hand placement with no ghost slots that swallow ammo. Turrets matching no key are left untouched. Config hot-reload. |
---
## License
[Kxrse ASA Plugins Non-Commercial License](LICENSE.txt)  
Free to use, modify, and redistribute **with attribution**.  
Commercial use or resale requires explicit written permission.
