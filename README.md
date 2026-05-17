# mod-levelsync

An AzerothCore module that syncs characters across multiple accounts to the same level, XP, and Individual Progression tier. Provides a traditional leveling experience (1-5 man) while keeping your alts in lockstep with your main — without manual edits. Built for private servers running large altbot setups (mod-playerbots).

---

## Features

- **Level Sync** — Members of a sync group are kept at the same level. Reconciliation happens at session boundaries (login, logout) and on explicit toggle. Online characters get in-memory updates with a chat notification; offline characters get DB writes via bulk transactions. Upward-only — sync never lowers a character's level.
- **XP Sync** — Same-level XP is also propagated. When a member logs in or out, the highest XP at the group's top level is pushed to all members at that level (online + offline).
- **IP Sync** — Individual Progression tiers (mod-individual-progression) are kept in lockstep across the group. Tier changes propagate immediately — completing a tier-advance quest (or running `.ip set`) syncs the rest of the group on the spot.
- **Multi-account groups** — Up to 10 accounts (configurable, default 6) per sync group. Each account can have up to 10 characters.
- **Upward-only** — Sync never lowers a character's level or tier. If the group highest is level 10 / tier 5, no one gets demoted below that.
- **Player-controlled toggles** — Level sync and IP sync are each toggled ON/OFF by group members. Adding a new character automatically turns the relevant flag OFF (with a red `OFF` chat notice) so a low-level alt isn't suddenly boosted before the player is ready.
- **Toggle rate-limit** — A 10-second per-group cooldown prevents toggle-spam.
- **Death Knight exception (level)** — Optional, default OFF. When OFF (`0`), DKs are excluded as sync sources for sub-55 targets — prevents a fresh DK from forcing all sub-55 characters to level 55. When ON (`1`), DKs participate normally.
- **Death Knight exception (IP)** — Optional, default OFF. Same logic for IP tiers using tier 13 (Sunwell Plateau) as the threshold.
- **Secure key system** — SHA-256 hashed keys are required to link accounts. Keys persist until overwritten or a GM clears them.
- **Auto orphan cleanup** — On every server startup, levelsync data referencing deleted characters is pruned. Empty groups are removed silently.
- **Gold Pool** — `.levelsync money` drains every other group member's gold (online + offline) into the caller's wallet in a single operation.
- **Self raid/dungeon unbind (opt-in)** — `.levelsync unbindall` is a non-GM equivalent of `.instance unbind all`. Default OFF; intended for personal/private servers where the operator wants to give every player the convenience of clearing their own lockouts.

---

## Sync model — important reading

mod-levelsync uses a **lazy-sync model** for level and XP, and a **per-event model** for IP tier. Knowing the difference helps you understand what the mod will and won't do mid-session.

### Level / XP — lazy-sync (session boundaries)

Level and XP changes during a play session do **not** automatically propagate to other group members. Sync fires only at:

- **Login** — when any group member logs in, the group reconciles around the new highest level/XP.
- **Logout** — when a member logs out, their level/XP push to everyone else.
- **Toggle** — `.levelsync level on` / `off` forces a full resync at the moment of execution.

This means: if your main grinds 5 levels during a play session, your offline alts won't see those levels until somebody logs in/out or you toggle. Drift mid-session is **expected and intended** — it avoids cascading false levels from mod-playerbots' `SyncQuestWithPlayer` mirroring.

### IP tier — per-event

IP tier advancement is a discrete milestone (e.g., beating Naxx 40 unlocks tier 7). When mod-individual-progression rewards a tier-up quest, mod-levelsync propagates the new tier to the entire group immediately — online and offline. No need to log out or toggle.

The asymmetry is deliberate. Tier-ups are intentional progression events that the whole group should advance through together; level/XP gain is continuous and prone to cascade issues, so it's lazy by design.

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
| `LevelSync.AllowMoneyCommands` | `1` | Allow players to use `.levelsync money` to pool group gold into the caller's wallet |
| `LevelSync.AllowRaidUnbind` | `0` | Allow players to use `.levelsync unbindall` (non-GM `.instance unbind all`). Off by default; recommended only for private servers |
| `LevelSync.MaxLinkedAccounts` | `6` | Maximum accounts per sync group (1–10) |
| `LevelSync.DeathKnightException` | `0` | Allow DKs to boost non-DK characters below level 55 (`0` = excluded, `1` = participates normally) |
| `LevelSync.DeathKnightIPException` | `0` | Allow DKs to boost non-DK characters below IP tier 13 (`0` = excluded, `1` = participates normally) |

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

Use `.levelsync addaccount <account>` or `.levelsync addchar <name>` to add a new member. **Both level sync and IP sync are auto-disabled when adding** (red `OFF` chat notice) to prevent a low-level alt from being instantly boosted. Re-enable with `.levelsync level on` and/or `.levelsync IP on` after all members are in.

### Triggers

| Event | Level Sync | IP Sync |
|-------|-----------|---------|
| Player logs in | Reconciles group: pulls self up to highest level/XP, pushes self's level/XP to all members below. Online + offline. | Reconciles group: pulls self up to highest tier, pushes self's tier to all members below. Online + offline. |
| Player logs out | Pushes the player's level + XP to all group members (online in-memory, offline via bulk DB UPDATE). | Pushes the player's IP tier to all group members. Online + offline (via atomic transactional bulk write). |
| Player levels up (mid-session) | **No automatic propagation.** Lazy-sync model — drift is reconciled at next login/logout/toggle. | N/A |
| IP tier advances (quest reward, e.g. via `.ip set` or normal IP-progression mechanics) | N/A | **Immediately propagates** to all group members. `OnPlayerCompleteQuest` hook fires on quest 66001–66018 reward. |
| `.levelsync level on` | Full resync: every member to highest level, with XP push at each effective ceiling (handles DK / non-DK ceilings independently). | — |
| `.levelsync IP on` | — | Full resync: every member to highest tier. |

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
| `.levelsync status` | Show your sync group summary: group ID, account count, sync states, and all members with live level, class, and IP tier. Ends with a link to the LevelsyncUI addon. |
| `.levelsync level on\|off` | Enable or disable level sync for your group. Enabling fires a full resync at the moment of toggle (level + XP, multi-ceiling DK rules). Subject to a 10-second cooldown per group. |
| `.levelsync IP on\|off` | Enable or disable IP sync for your group. Enabling fires a full tier resync. Same 10-second cooldown applies. |
| `.levelsync money` | One-shot. Drains every other group member's gold (online + offline) into your wallet. Refused if the resulting wallet would exceed the gold cap (`MAX_MONEY_AMOUNT`) — withdraw manually first if so. Refused if the group has only you or no one has any gold. Online drained members get a chat notice. Subject to the same 10-second cooldown as the toggles. |
| `.levelsync unbindall [name]` | One-shot. Wipes every instance binding the target has across all 4 difficulty slots (dungeons + raids), preserving the binding for the map the target is currently inside. **No arg** → clicked/tab-selected player, falls back to you (same semantics as stock `.instance unbind all`). **With name** → online player matching that name (case-insensitive); refused if offline. Requires `LevelSync.AllowRaidUnbind = 1` on the server; refuses with a "disabled by server" message otherwise. No cooldown. |

### Cooldown rejection message

If you toggle too fast, you'll see:

```
[LevelSync] Must wait N second(s) before resync.
```

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
    Aone (lvl 60) (Druid) IP Tier: 7 - Naxxramas 40
    Atwo (lvl 60) (Paladin) IP Tier: 7 - Naxxramas 40
    Athree (lvl 60) (Death Knight) IP Tier: 13 - Sunwell Plateau
  Account 106: Characters: 3
    Bone (lvl 60) (Hunter) IP Tier: 7 - Naxxramas 40
    ...
[LevelSync] For a graphical interface use the addon: https://github.com/Lichborne-AC/LevelsyncUI
```

---

## GM Commands

| Command | Description |
|---------|-------------|
| `.levelsync gm removeall <charname>` | Fully disband the sync group that the named character belongs to and remove all associated account keys. If the character is not in a group, removes their account key only. |
| `.levelsync gm xp <amount>` | Grant XP to your current target (or self if no target). Uses `Player::GiveXP` so it goes through AC's normal level-up pipeline — including mod-playerbots' XP-rate multiplier when targeted at a bot. Useful for testing the XP propagation paths. |
| `.levelsync gm unbindall [name]` | GM-gated counterpart to `.levelsync unbindall`. Same overloaded target resolution: clicked/tab-selected player with self fallback if no arg, or online named player (case-insensitive) if given. Always available to GMs regardless of `LevelSync.AllowRaidUnbind` — that flag only controls the player form. Online targets only. |

### Working with `.ip set`

`.ip set` is provided by mod-individual-progression, not by mod-levelsync. It's the recommended way to manually advance a character to a specific IP tier. mod-levelsync's `OnPlayerCompleteQuest` hook fires when `.ip set` rewards the underlying progression quest, so the rest of the group syncs automatically.

```
.ip set <player> <tier>     # e.g. .ip set Aone 5
```

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

[LevelsyncUI](https://github.com/Lichborne-AC/LevelsyncUI) — A World of Warcraft addon (WotLK 3.3.5a, AzerothCore) that provides a graphical UI for mod-levelsync. Recommended but not required — all functionality is available via dot commands without the addon. The link is also printed at the end of every `.levelsync status` output.

---

## License

GPL v2
