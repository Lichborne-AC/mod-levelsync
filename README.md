# mod-levelsync

An AzerothCore module that syncs characters across multiple accounts to the same level and Individual Progression tier. Provides a traditional leveling experience (1-5 man) while automatically syncing your alts to your current progress (without manual edits).  Built for private servers running large altbot setups (mod-playerbots).  

---

## Features

- **Level Sync** — When members levels up, all other members are instantly brought to the same level. Works for online characters in real-time and offline characters via direct DB update. (Characters do not sync down)
- **XP Sync** — On logout, character's current XP is written to all offline group members at the same level, keeping progress consistent.
- **IP Sync** — Syncs Individual Progression tiers (mod-individual-progression) across the group using the same upward-only logic as level sync. (will not sync down)
- **Multi-account groups** — Up to 10 accounts (configurable)(default 6) can be linked into a single sync group. Each account can have up to 10 characters.
- **Upward-only** — Sync never lowers Level/IP levels. If the group highest is level 10, no one gets set below 10.
- **Player-controlled toggles** — Level sync and IP sync are each toggled ON/OFF by players in the module group. Adding a new character automatically turns syncs OFF. The group manually re-enables after everyone is added. (prevents accidental edits)
- **Auto-status response** — Every add, remove, and sync toggle command automatically prints the full group status followed by a confirmation line, so you always see the current state of the group after any change.
- **Death Knight exception (level)** — Optional: Default OFF. When disabled (0), a DK is excluded as a sync source for characters below level 55 — prevents a fresh DK from forcing all sub-55 characters to level 55. When enabled (1), DKs participate normally and can boost sub-55 characters.
- **Death Knight exception (IP)** — Optional: Default OFF. Same logic for IP tiers using tier 13 (Sunwell Plateau) as the threshold.
- **GM commands** — Server admins can fully disband any sync group by character name, including clearing all associated account keys. (full reset)
- **Secure key system** — SHA-256 hashed keys are required to link accounts, preventing unauthorized linking. Keys persist until overwritten or a GM clears them — no need to re-run setkey after removing characters from a group.
- **Auto orphan cleanup** — On every server startup, any levelsync data referencing deleted characters is automatically pruned. Empty groups are removed silently — no manual DB maintenance required.

---

## Requirements

- AzerothCore (WotLK 3.3.5a)
- **Optional:** [mod-individual-progression](https://github.com/azerothcore/mod-individual-progression) — required only for IP Sync. Not required for level sync.

---

## Installation

1. Clone or copy this module into your `modules/` directory:
   ```
   modules/mod-levelsync/
   ```

2. Rebuild the server:
   ```bash
   cd build
   cmake ..
   make -j$(nproc)
   make install
   ```

3. Apply the SQL schema to `acore_characters`:
   ```bash
   mysql -u acore -pacore acore_characters < modules/mod-levelsync/data/sql/characters/base/mod_levelsync_tables.sql
   ```

4. Copy and edit the config:
   ```
   env/dist/etc/modules/mod_levelsync.conf
   ```

5. Restart the worldserver.

---

## Configuration

| Option | Default | Description |
|--------|---------|-------------|
| `LevelSync.Enable` | `1` | Enable or disable the module entirely |
| `LevelSync.AllowLevelSync` | `1` | Allow players to use level sync |
| `LevelSync.AllowProgressionSync` | `1` | Allow players to use IP sync (requires mod-individual-progression) |
| `LevelSync.MaxLinkedAccounts` | `6` | Maximum accounts per sync group (1–10) |
| `LevelSync.DeathKnightException` | `0` | Allow DKs to boost non-DK characters below level 55 (0 = excluded, 1 = participates normally) | 
| `LevelSync.DeathKnightIPException` | `0` | Allow DKs to boost non-DK characters below IP tier 13 (0 = excluded, 1 = participates normally) |

---

## Database Tables

All tables are added to `acore_characters`. No core tables are modified.

| Table | Purpose |
|-------|---------|
| `levelsync_groups` | One row per sync group. Stores `level_sync_enabled` and `sync_progression` flags. |
| `levelsync_members` | One row per character in a group. Links `char_guid` and `account_id` to a `group_id`. |
| `levelsync_account_keys` | Stores the SHA-256 hashed security key per account. Required to link accounts. One row per account maximum. |

IP tier data is stored in the existing `character_queststatus_rewarded` table using hidden quest IDs 66001–66018 (mod-individual-progression's format). mod-levelsync does not add any IP-specific tables.

---

## How Sync Works

### Adding Characters

Use `.levelsync addaccount <account>` or `.levelsync addchar <name>` to add a new member/account to the module group. **Both level sync and IP sync are turned OFF by default**. This prevents the new character from being immediately boosted before the player is ready. Enable with `.levelsync level on` and/or `.levelsync IP on` after all members have been added.

### Triggers

| Event | Level Sync | IP Sync |
|-------|-----------|---------|
| Turn Sync | Syncs the player up to the group's highest level. Syncs all online and offline members to the highest level in the group. | Same logic for IP tier. |
| Player logs out | Pushes the logging-out player's level and XP to all other group members (online in-memory, offline via DB). Never reduces levels. | Pushes the logging-out player's IP tier to all other group members. Never reduces IP tiers. |
| Player levels up | Immediately pushes new level to all online members. Offline members are updated in DB. | N/A (IP has no equivalent automatic event; tier changes use `OnPlayerCompleteQuest`). |
| IP tier advances (quest reward) | N/A | `OnPlayerCompleteQuest` fires for quest IDs 66001–66018. Pushes new tier to all group members. |
| `.levelsync level on` | Syncs all group members to the current highest level at the moment of toggle. | — |
| `.levelsync IP on` | — | Syncs all group members to the current highest IP tier at the moment of toggle. |

### Death Knight Exception

When `LevelSync.DeathKnightException = 0` (default), a DK is excluded as a sync *source* for characters below level 55 — it will not push its level 55 to sub-55 group members. The DK can still be synced *up* by non-DK characters. Once all non-DK characters in the group reach 55+, the DK participates normally as a sync source. When set to `1`, DKs participate immediately and can boost sub-55 characters. The same logic applies to `LevelSync.DeathKnightIPException` using tier 13 as the threshold.

---

## Player Commands

All commands begin with `.levelsync`.

### Setup

| Command | Description |
|---------|-------------|
| `.levelsync setkey <key>` | Set a security key for your account. Other players need this key to link your account to their group. Keys persist until overwritten. |
| `.levelsync addaccount <account> [key]` | Link all characters from another account into your sync group. |
| `.levelsync addchar <charname> [key]` | Link a single character into your sync group. Key is required if the character is on a different account. |
| `.levelsync removeaccount <account>` | Remove all characters from an account from your sync group by account name. |
| `.levelsync removeaccount # <accountid>` | Remove all characters from an account by numeric account ID (e.g. `# 105`). |
| `.levelsync removechar <charname>` | Remove a single character from your sync group. |
| `.levelsync removeall` | Disband your entire sync group. |
| `.levelsync disbandaccount` | Disband every sync group associated with any character on your account. Works even if the character you are logged in on is not personally in a group. |
| `.levelsync listaccount <account> [key]` | Show all characters on an account with their level, class, and group status. Key is required when viewing another account — not required for your own. |

### Status & Toggles

| Command | Description |
|---------|-------------|
| `.levelsync status` | Show your sync group summary: group ID, account count, sync states, and all members with level, class, and IP tier. |
| `.levelsync level on\|off` | Enable or disable level sync for your group. Enabling immediately syncs all members to the current highest level. |
| `.levelsync IP on\|off` | Enable or disable IP sync for your group. Enabling immediately syncs all members to the current highest IP tier. |

### Status Output Example

In-game the output appears with color coding: `[LevelSync]` in green, ON in green, OFF in red, character names in class color, class names in gold, and IP tier labels in tier-specific colors. Shown here in plain text:

```
[LevelSync] Sync Group #1
  Accounts: 3/6
  Total Characters: 9
  Level sync: ON
  Progression sync: ON
[LevelSync] Group members:
  Account 105: Characters: 3
    Aone (lvl 60) (Warrior) IP Tier: 7 - Naxxramas 40
    Atwo (lvl 60) (Paladin) IP Tier: 7 - Naxxramas 40
    Athree (lvl 60) (Death Knight) IP Tier: 13 - Sunwell Plateau
  Account 106: Characters: 3
    Bone (lvl 60) (Hunter) IP Tier: 7 - Naxxramas 40
    ...
```

---

## GM Commands

| Command | Description |
|---------|-------------|
| `.levelsync gm removeall <charname>` | Fully disband the sync group that the named character belongs to and remove all associated account keys. If the character is not in a group, removes their account key only. |

---

## IP Tier Reference

Used with mod-individual-progression. Tiers are stored as hidden quest IDs in `character_queststatus_rewarded`.

| Tier | Quest ID | Name |
|------|----------|------|
| 0 | — | None (Starting Point) |
| 1 | 66001 | Molten Core |
| 2 | 66002 | Onyxia |
| 3 | 66003 | Blackwing Lair |
| 4 | 66004 | Pre-AQ |
| 5 | 66005 | AQ War Effort |
| 6 | 66006 | Ahn'Qiraj |
| 7 | 66007 | Naxxramas 40 |
| 8 | 66008 | Pre-TBC |
| 9 | 66009 | Karazhan / Gruul / Magtheridon |
| 10 | 66010 | Serpentshrine Cavern / Tempest Keep |
| 11 | 66011 | Hyjal Summit / Black Temple |
| 12 | 66012 | Zul'Aman |
| 13 | 66013 | Sunwell Plateau |
| 14 | 66014 | Naxxramas / Eye of Eternity / Obsidian Sanctum |
| 15 | 66015 | Ulduar |
| 16 | 66016 | Trial of the Crusader |
| 17 | 66017 | Icecrown Citadel |
| 18 | 66018 | Ruby Sanctum |

---

## UI Addon

[LevelsyncUI](https://github.com/Lichborne-AC/LevelsyncUI) — A World of Warcraft addon (WotLK 3.3.5a, AzerothCore) that provides a graphical UI for mod-levelsync. Recommended but not required — all functionality is available via dot commands without the addon.

---

## License

GPL v2 
