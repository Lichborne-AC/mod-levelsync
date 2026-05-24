# mod-levelsync

An AzerothCore module that syncs characters across multiple accounts to the same level, XP, and Individual Progression tier. Provides a traditional leveling experience (1-5 man) while keeping your alts in lockstep with your main — without manual edits. Built for private servers running large altbot setups (mod-playerbots).

---

## Features

- **Level Sync** — Members of a sync group are pulled to the same level. The sync runs only when a player runs `.levelsync level on`. Online characters get in-memory updates with a chat notification; offline characters get DB writes via bulk transactions. Upward-only — sync never lowers a character's level.
- **XP Sync** — Same-level XP is propagated as part of the level sync. The highest XP at the group's top level is pushed to all members at that level (online + offline) when the command fires.
- **IP Sync** — Individual Progression tiers (mod-individual-progression) are aligned across the group when a player runs `.levelsync IP on`. The highest tier in the group is pushed to everyone below it. Tier-up quests no longer auto-propagate — run the command after a tier advance.
- **Multi-account groups** — Up to 10 accounts (configurable, default 6) per sync group. Each account can have up to 10 characters.
- **Upward-only** — Sync never lowers a character's level or tier. If the group highest is level 10 / tier 5, no one gets demoted below that.
- **Player-driven** — Nothing is automatic. Group members run `.levelsync level on` or `.levelsync IP on` when they want the group reconciled. There is no persistent on/off state and no per-event hook.
- **Rate-limit** — A 10-second per-group cooldown prevents spamming the sync commands.
- **Death Knight exception (level)** — Optional, default OFF. When OFF (`0`), DKs are excluded as sync sources for sub-55 targets — prevents a fresh DK from forcing all sub-55 characters to level 55. When ON (`1`), DKs participate normally.
- **Death Knight exception (IP)** — Optional, default OFF. Same logic for IP tiers using tier 13 (Sunwell Plateau) as the threshold.
- **Secure key system** — SHA-256 hashed keys are required to link accounts. Keys persist until overwritten or a GM clears them.
- **Auto orphan cleanup** — On every server startup, levelsync data referencing deleted characters is pruned. Empty groups are removed silently.
- **Gold Pool** — `.levelsync money` drains every other group member's gold (online + offline) into the caller's wallet in a single operation.
- **Self raid/dungeon unbind (opt-in)** — `.levelsync unbindall` is a non-GM equivalent of `.instance unbind all`. Default OFF; intended for personal/private servers where the operator wants to give every player the convenience of clearing their own lockouts.

---

## Sync model — important reading

mod-levelsync syncs **only when a player runs the toggle command**. There is no automatic sync at login, logout, or quest reward, and no per-group on/off state stored across sessions.

- `.levelsync level on` — pushes the group's highest level (and XP at that level) to every member. Runs once and exits.
- `.levelsync IP on` — pushes the group's highest IP tier to every member. Runs once and exits.

Both commands honour a 10-second per-group cooldown.

Drift between sessions is **expected** — if your main grinds 5 levels in a play session, your alts won't see those levels until somebody runs `.levelsync level on`. This is the deliberate design: it avoids cascading false levels from mod-playerbots' `SyncQuestWithPlayer` mirroring, and it puts every state change behind an explicit player action.

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
| `levelsync_groups` | One row per sync group. The `level_sync_enabled` and `sync_progression` columns are retained from earlier versions but the current runtime no longer reads them — sync state is driven entirely by the toggle commands and server conf. |
| `levelsync_members` | One row per character in a group. Links `char_guid` and `account_id` to a `group_id`. |
| `levelsync_account_keys` | Stores the SHA-256 hashed security key per account. Required to link accounts. One row per account maximum. |

IP tier data is stored in the existing `character_queststatus_rewarded` table using hidden quest IDs 66001–66018 (mod-individual-progression's format). mod-levelsync does not add any IP-specific tables.

---

## How Sync Works

### Adding Characters

Use `.levelsync addaccount <account>` or `.levelsync addchar <name>` to add a new member. Newly added characters keep their existing level and tier until somebody runs the toggle command — so a low-level alt won't be boosted to the group ceiling until the group is ready.

### Triggers

| Event | Level Sync | IP Sync |
|-------|-----------|---------|
| Player logs in | No effect. | No effect. |
| Player logs out | No effect. | No effect. |
| Player levels up | No effect. Drift is reconciled at the next `.levelsync level on`. | N/A |
| IP tier advances (quest reward or `.ip set`) | N/A | No effect. Drift is reconciled at the next `.levelsync IP on`. |
| `.levelsync level on` | Full resync: every member pulled to highest level, with XP push at each effective ceiling (handles DK / non-DK ceilings independently). | — |
| `.levelsync IP on` | — | Full resync: every member pulled to highest tier. |

All previous auto-sync paths (`OnPlayerLogin`, `OnPlayerLogout`, `OnPlayerCompleteQuest`) are no-ops in the current version. The underlying Mgr methods (`SyncGroupOnLogin`, `SyncIPOnLogin`, `SyncIPOnTierUp`, etc.) are still present in the codebase so the old behavior can be restored by un-commenting the hook bodies in `LevelSyncPlayerScript`.

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
| `.levelsync status` | Show your sync group summary: group ID, account count, conf-permission state for level and IP sync, and all members with live level, class, and IP tier. Ends with a link to the LevelsyncUI addon. |
| `.levelsync level on` | Fires a single full level + XP resync (multi-ceiling DK rules applied). Returns to `Available` afterwards. Subject to a 10-second per-group cooldown. `.levelsync level off` is a no-op — there is no persistent state to disable. |
| `.levelsync IP on` | Fires a single full IP tier resync. Returns to `Available` afterwards. Same 10-second cooldown. `.levelsync IP off` is a no-op. |
| `.levelsync money` | One-shot. Drains every other group member's gold (online + offline) into your wallet. Refused if the resulting wallet would exceed the gold cap (`MAX_MONEY_AMOUNT`) — withdraw manually first if so. Refused if the group has only you or no one has any gold. Online drained members get a chat notice. Subject to the same 10-second cooldown as the toggles. |
| `.levelsync unbindall [name]` | One-shot. Wipes every instance binding the target has across all 4 difficulty slots (dungeons + raids), preserving the binding for the map the target is currently inside. **No arg** → clicked/tab-selected player, falls back to you (same semantics as stock `.instance unbind all`). **With name** → online player matching that name (case-insensitive); refused if offline. Requires `LevelSync.AllowRaidUnbind = 1` on the server; refuses with a "disabled by server" message otherwise. No cooldown. |

### Cooldown rejection message

If you fire the sync too fast, you'll see:

```
[LevelSync] Must wait N second(s) before resync.
```

### Status Output Example

In-game the output appears with color coding: `[LevelSync]` in green, `Available` in green, `Disabled` in red, character names in class color, class names in gold, and IP tier labels in tier-specific colors. The level/progression status reflects what the server conf permits (whether `.levelsync level on` / `.levelsync IP on` will fire when invoked), not a persistent toggle. Shown here in plain text:

```
[LevelSync] Sync Group #1
  Accounts: 3/6
  Total Characters: 9
  Level sync: Available
  Progression sync: Available
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

`.ip set` is provided by mod-individual-progression, not by mod-levelsync. It's the recommended way to advance a character to a specific IP tier. After using it on one member, run `.levelsync IP on` to push the new tier to the rest of the group.

```
.ip set <player> <tier>     # e.g. .ip set Aone 5
```

---

## UI Addons

- [PlayerbotManager](https://github.com/Lichborne-AC/PlayerbotManager) — Companion addon for mod-playerbots. Useful alongside mod-levelsync for managing the altbot roster that a sync group is built around.
- [LevelsyncUI](https://github.com/Lichborne-AC/LevelsyncUI) — A World of Warcraft addon (WotLK 3.3.5a, AzerothCore) that provides a graphical UI for mod-levelsync. Recommended but not required — all functionality is available via dot commands without the addon. The link is also printed at the end of every `.levelsync status` output.

---

## License

GPL v2
