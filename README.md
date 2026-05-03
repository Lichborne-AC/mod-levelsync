# mod-levelsync

An AzerothCore module that keeps a group of characters across one or more
linked accounts at the same character level and Individual Progression tier.
Designed for private servers running large altbot setups (mod-playerbots) where
the operator wants alts to follow a "main" automatically without manual
edits.

Sync is **upward-only** — characters never lose levels or tiers because
another member is lower.

---

## Features

- **Level Sync** — When any member levels up, all other members in the sync
  group are brought to the same level. Online members are updated in real time
  via `Player::GiveLevel`; offline members are updated directly in the
  `characters` table.
- **XP Sync** — On logout, the logging-out player's XP is pushed to all other
  members at the same level (online and offline). Keeps "current bar"
  progress consistent without hammering the DB during normal play.
- **IP Sync** — Optional. Syncs Individual Progression tiers
  (mod-individual-progression) across the group using the same upward-only
  rule as level sync.
- **Multi-account groups** — Up to 6 accounts (configurable, 1–10) can be
  linked into a single sync group. There is no fixed cap on characters per
  account beyond what AzerothCore allows.
- **Per-group toggles** — Level sync and IP sync are each toggled ON/OFF per
  group by any member. Adding a new member automatically turns BOTH syncs OFF
  so the new character isn't immediately boosted before the player is ready;
  re-enable manually with `.levelsync level on` and/or `.levelsync IP on`.
- **Death Knight exceptions (optional, default OFF)** — Two independent
  switches let you choose whether DKs participate as a sync source for
  members below the DK starting threshold (level 55 / IP tier 13).
- **Secure account linking** — Linking another player's account requires a
  SHA-256 hashed key set by that account's owner. (This is a SEPARATE key
  system from mod-playerbots — see "Compatibility notes" below.)
- **GM control** — Server admins can fully disband any sync group and wipe
  its keys, by character name or by account name.
- **Auto orphan cleanup** — On every server startup, members referencing
  deleted characters are pruned, empty groups are removed, and account keys
  no longer attached to any group are cleaned up. No manual DB maintenance
  required.

---

## Requirements

- AzerothCore (WotLK 3.3.5a), built with `MODULES=static` (default) or
  `dynamic`.
- **Optional:** [mod-individual-progression](https://github.com/azerothcore/mod-individual-progression)
  if you want IP Sync. mod-levelsync runs fine without it; IP-related
  commands and hooks become no-ops.

---

## Installation

1. Place the module in your modules directory:

       modules/mod-levelsync/

2. Rebuild AzerothCore:

       cd build
       cmake ..
       make -j$(nproc)
       make install

3. Apply the schema to `acore_characters`:

       mysql -u acore -pacore acore_characters \
         < modules/mod-levelsync/data/sql/characters/base/mod_levelsync_tables.sql

   (Or let AzerothCore's SQL auto-importer pick it up at next worldserver
   start — works either way.)

4. The `.conf.dist` is installed to `env/dist/etc/modules/mod_levelsync.conf.dist`.
   Copy it (or rename it) to `mod_levelsync.conf` and edit:

       cd env/dist/etc/modules/
       cp mod_levelsync.conf.dist mod_levelsync.conf
       # edit mod_levelsync.conf

5. Restart the worldserver.

---

## Configuration

| Option | Default | Description |
|--------|---------|-------------|
| `LevelSync.Enable` | `1` | Master switch. `0` disables the entire module (commands return "module disabled"). |
| `LevelSync.AllowLevelSync` | `1` | Allow players to use level sync. `0` disables `.levelsync level on`. |
| `LevelSync.AllowProgressionSync` | `0` | Allow players to use IP sync. `0` disables `.levelsync IP on`. Requires mod-individual-progression. |
| `LevelSync.MaxLinkedAccounts` | `6` | Max accounts per sync group. Clamped to `[1, 10]`. |
| `LevelSync.DeathKnightException` | `0` | `0` = block DKs from boosting members below level 55. `1` = allow. |
| `LevelSync.DeathKnightIPException` | `0` | `0` = block DKs from boosting members below IP tier 13. `1` = allow. |

**Death Knight exception semantics, restated** (because it's easy to misread):
- The default (`0`, "exception OFF") is the *more conservative* setting:
  the DK does NOT push other members up just by existing in the group.
- Setting it to `1` ("exception ON") tells the mod to TREAT the DK as a
  normal sync source even though they start at 55 / tier 13. With this
  enabled, adding a fresh DK to a group of level-30 alts will pull all the
  level-30 alts up to 55.

---

## Database Tables

All tables are added to `acore_characters`. No core tables are modified.

| Table | Purpose |
|-------|---------|
| `levelsync_groups` | One row per sync group. Stores `level_sync_enabled` and `sync_progression` flags. |
| `levelsync_members` | One row per character in a group. Maps `char_guid` and `account_id` to `group_id`. PK is `char_guid` so a character can only be in one group. |
| `levelsync_account_keys` | SHA-256 hashed security key per account. Required to link OTHER accounts into your group. |

IP tier data is NOT stored in any new table. mod-individual-progression
already uses hidden quest IDs `66001–66018` in `character_queststatus_rewarded`
to represent tiers 1–18; mod-levelsync reads and writes the same rows.

---

## How sync works

### Adding members

Use `.levelsync addaccount <account>` (links every character on that account)
or `.levelsync addchar <name>` (one character at a time). When you add ANY
new member, both level sync and IP sync are turned OFF for the group. Add
everyone first, then re-enable with `.levelsync level on` and/or
`.levelsync IP on`. This is intentional — it prevents a freshly-linked level-1
alt from being instantly boosted to 60 before you've finished setting things
up.

### Sync triggers

| Event | Level sync behavior | IP sync behavior |
|-------|--------------------|------------------|
| Player logs in | Player is brought up to the group's highest level (subject to DK rules); player is then used as a source to push any members below their level. | Same logic for IP tier. |
| Player logs out | Player's level + XP pushed to all other members. Online members updated in memory; offline via DB. Never decreases. | Player's IP tier pushed to all other members. Never decreases. |
| Player levels up | New level pushed immediately to all other online members; offline members updated via DB. | N/A — IP tier doesn't change on level-up. |
| Player completes IP tier quest (66001–66018) | N/A | New tier pushed to all other members (online via in-memory quest re-award, offline via direct DB row insert). |
| `.levelsync level on` | Group is synced to the current highest level immediately. | — |
| `.levelsync IP on` | — | Group is synced to the current highest IP tier immediately. |

### Death Knight exception

When `LevelSync.DeathKnightException = 0` (default), a DK is excluded as a
sync **source** for any member currently below level 55. The DK can still be
synced **upward** by non-DK members. Once every non-DK in the group reaches
55+, the DK's level participates normally.

`LevelSync.DeathKnightIPException` works the same way using IP tier 13 as
the threshold.

> **Server-specific note:** if you've set
> `IndividualProgression.DeathKnightStartingProgression = 0` in your
> `individualProgression.conf` (so DKs start at IP tier 0 like everyone
> else), the DK IP exception is largely dormant — DKs won't naturally hold a
> tier-13 head-start over groupmates. The logic is still correct and stays
> in for safety; it just doesn't do anything in normal play on that config.

---

## Player commands

All commands are subcommands of `.levelsync`. SEC level: `SEC_PLAYER`.

### Setup

| Command | Description |
|---------|-------------|
| `.levelsync setkey <key>` | Set (or replace) the security key for your account. Other players need this exact string to link your account into their sync group. |
| `.levelsync addaccount <account> [key]` | Link every character on `<account>` into your sync group. `[key]` is required unless you're linking your own account. |
| `.levelsync addchar <charname> [key]` | Link a single character. `[key]` is required if the character is on a different account. |
| `.levelsync removeaccount <account>` | Remove every character belonging to `<account>` from your group. |
| `.levelsync removechar <charname>` | Remove a single character from your group. |
| `.levelsync removeall` | Disband your sync group entirely and remove all linked account keys. If you are not in a group, this just removes your own account key. |
| `.levelsync listaccount <account>` | List every character on `<account>`, with class, level, and which sync group (if any) they're in. |

### Status / toggles

| Command | Description |
|---------|-------------|
| `.levelsync status` | Show your group's ID, account count, total characters, sync ON/OFF state, and a per-account member list with class, level, and IP tier. |
| `.levelsync level on\|off` | Enable or disable level sync for your group. Enabling immediately syncs everyone to the current highest level. |
| `.levelsync IP on\|off` | Enable or disable IP sync for your group. Enabling immediately syncs everyone to the current highest IP tier. Requires `LevelSync.AllowProgressionSync = 1` server-side. |

### Status output example

```
[LevelSync] Sync Group #1
  Accounts: 3/6
  Total Characters: 9
  Level sync: ON
  Progression sync: ON
[LevelSync] Group members:
  Account 105: Characters: 3
    Test (lvl 60) (Warrior) IP Tier: 7 - Naxxramas 40
    Testtwo (lvl 60) (Paladin) IP Tier: 7 - Naxxramas 40
    Testthree (lvl 60) (Death Knight) IP Tier: 7 - Naxxramas 40
  Account 106: Characters: 3
    Testfour (lvl 60) (Shaman) IP Tier: 7 - Naxxramas 40
    ...
```

---

## GM commands

All GM commands require `SEC_GAMEMASTER` and run in-game (`Console::No`).

| Command | Description |
|---------|-------------|
| `.levelsync gm removeall <charname>` | Look up the character's account, find the sync group, and fully disband it (members, group row, and all linked account keys). If the character is not in any group, only that character's account key is removed. |

---

## IP tier reference

For use with mod-individual-progression. Tiers are stored as hidden quest IDs
in `character_queststatus_rewarded`.

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

## Compatibility notes

- **mod-individual-progression**: required for IP sync; fully optional for
  level sync. mod-levelsync reads/writes the SAME hidden quest rows mod-IP
  uses, so the two are tightly coupled when both are enabled. Removing
  mod-IP later will leave hidden quest rows behind in
  `character_queststatus_rewarded` (the rows are harmless but you may want
  to clean them).

- **mod-playerbots account linking**: mod-playerbots provides its own
  account-link key system at `.playerbots account setKey/link/unlink/...`.
  mod-levelsync's keys are SEPARATE — different table, different commands,
  different purpose. Both modules can coexist without conflict, but a player
  who wants their alts both bot-controllable and level-synced will need to
  set up keys in BOTH systems.

- **Other modules audited**: no command-name, table-name, hook-signature, or
  global-symbol collisions with mod-ah-bot-plus, mod-appreciation,
  mod-no-hearthstone-cooldown, mod-starter-guild.

---

## UI Addon

A companion in-game UI addon is in development. It will provide a graphical
interface for managing your sync group, viewing member status, and toggling
syncs without typing chat commands. See `LevelSync_Addon_Prompt.txt` for the
full addon specification if you want to build or customize one yourself.

The UI addon is not required — every feature is available via chat commands.

---

## License

GPL v2 — same as AzerothCore.
