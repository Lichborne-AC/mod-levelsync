#include "LevelSync.h"
#include "Chat.h"
#include "ChatCommand.h"
#include "CharacterCache.h"
#include "DBCEnums.h"
#include "InstanceSaveMgr.h"
#include "ObjectAccessor.h"
#include "ObjectGuid.h"
#include "Player.h"
#include "QuestDef.h"
#include "ScriptMgr.h"
#include <openssl/sha.h>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <unordered_map>

using namespace Acore::ChatCommands;

// -----------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------

// Escape a string for safe interpolation into a CharacterDatabase query.
// Defends against names/inputs containing apostrophes or other SQL special
// characters reaching the query layer (which would otherwise crash with a
// syntax error). Stock AC name validators block such inputs at character
// creation, but custom servers, GM operations, and external imports can
// still produce them.
static std::string EscChar(std::string s)
{
    CharacterDatabase.EscapeString(s);
    return s;
}

static std::string EscLogin(std::string s)
{
    LoginDatabase.EscapeString(s);
    return s;
}

static std::string CapFirst(std::string s)
{
    if (!s.empty())
        s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
    return s;
}

// Wipes every bound instance the target has across all 4 difficulty slots
// (dungeons + raids), preserving the binding for the map the target is
// currently inside. Returns the number of bindings cleared. Used by both
// the player-facing `.levelsync unbindall` and the GM `.levelsync gm
// unbindall` commands.
static uint32 UnbindAllInstancesOn(Player* target)
{
    uint32 counter = 0;
    for (uint8 i = 0; i < MAX_DIFFICULTY; ++i)
    {
        BoundInstancesMap const& binds = sInstanceSaveMgr->PlayerGetBoundInstances(target->GetGUID(), Difficulty(i));
        for (BoundInstancesMap::const_iterator itr = binds.begin(); itr != binds.end();)
        {
            if (itr->first != target->GetMapId())
            {
                sInstanceSaveMgr->PlayerUnbindInstance(target->GetGUID(), itr->first, Difficulty(i), true, target);
                // PlayerUnbindInstance mutates the map being iterated;
                // restart the inner loop from the beginning.
                itr = binds.begin();
                ++counter;
            }
            else
                ++itr;
        }
    }
    return counter;
}

static std::string FormatMoneyDisplay(uint32 copper)
{
    uint32 g = copper / 10000;
    uint32 s = (copper % 10000) / 100;
    uint32 c = copper % 100;
    std::string out;
    if (g > 0)
        out += std::to_string(g) + "g";
    if (s > 0)
    {
        if (!out.empty()) out += " ";
        out += std::to_string(s) + "s";
    }
    if (c > 0 || out.empty())
    {
        if (!out.empty()) out += " ";
        out += std::to_string(c) + "c";
    }
    return out;
}

static uint32 GetAccountIdByName(const std::string& name)
{
    QueryResult r = LoginDatabase.Query(
        "SELECT id FROM account WHERE username = '{}'", EscLogin(name));
    if (!r)
        return 0;
    return r->Fetch()[0].Get<uint32>();
}

static std::string HashKey(const std::string& key)
{
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(key.c_str()), key.size(), hash);
    std::ostringstream ss;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i)
        ss << std::hex << std::setw(2) << std::setfill('0') << (int)hash[i];
    return ss.str();
}

static bool VerifyAccountKey(uint32 accountId, const std::string& key)
{
    std::string hashed = HashKey(key);
    QueryResult r = CharacterDatabase.Query(
        "SELECT 1 FROM levelsync_account_keys WHERE account_id = {} AND security_key = '{}'",
        accountId, hashed);
    return r != nullptr;
}

static const char* ClassName(uint8 cls)
{
    switch (cls)
    {
        case 1:  return "Warrior";
        case 2:  return "Paladin";
        case 3:  return "Hunter";
        case 4:  return "Rogue";
        case 5:  return "Priest";
        case 6:  return "Death Knight";
        case 7:  return "Shaman";
        case 8:  return "Mage";
        case 9:  return "Warlock";
        case 11: return "Druid";
        default: return "Unknown";
    }
}

static const char* ClassColor(uint8 cls)
{
    switch (cls)
    {
        case 1:  return "C79C6E";
        case 2:  return "F58CBA";
        case 3:  return "ABD473";
        case 4:  return "FFF569";
        case 5:  return "FFFFFF";
        case 6:  return "C41F3B";
        case 7:  return "0070DE";
        case 8:  return "69CCF0";
        case 9:  return "9482C9";
        case 11: return "FF7D0A";
        default: return "FFFFFF";
    }
}

static const char* IPTierName(uint8 tier)
{
    switch (tier)
    {
        case 0:  return "Molten Core / Onyxia";
        case 1:  return "Molten Core / Onyxia";
        case 2:  return "Blackwing Lair";
        case 3:  return "Pre-AQ";
        case 4:  return "AQ War Effort";
        case 5:  return "Ahn'Qiraj";
        case 6:  return "Naxxramas 40";
        case 7:  return "Pre-TBC";
        case 8:  return "Karazhan / Gruul's Lair / Magtheridon's Lair";
        case 9:  return "Serpentshrine Cavern / Tempest Keep";
        case 10: return "Hyjal Summit / Black Temple";
        case 11: return "(Skipped)";
        case 12: return "Sunwell Plateau";
        case 13: return "Naxxramas / Eye of Eternity / Obsidian Sanctum";
        case 14: return "Ulduar";
        case 15: return "Trial of the Crusader";
        case 16: return "Icecrown Citadel";
        case 17: return "Ruby Sanctum";
        case 18: return "Tiers Complete";
        default: return "Unknown (IP may not be enabled)";
    }
}

static const char* IPTierColor(uint8 tier)
{
    switch (tier)
    {
        case 0:  return "808080";
        case 1:  return "de8c33";
        case 2:  return "de8c33";
        case 3:  return "d94040";
        case 4:  return "4da64d";
        case 5:  return "4d80d9";
        case 6:  return "a64dd9";
        case 7:  return "33b8b8";
        case 8:  return "c0b840";
        case 9:  return "cc4780";
        case 10: return "8c8cb3";
        case 11: return "ff8026";
        case 12: return "38cc8c";
        case 13: return "4d80ff";
        case 14: return "8cc74d";
        case 15: return "b380f2";
        case 16: return "38bfb3";
        case 17: return "e6334d";
        case 18: return "59cc59";
        default: return "808080";
    }
}

// Displays all members of a group grouped by account, with class color, class name, and IP tier.
static void DisplayGroupMembers(ChatHandler* handler, uint32 groupId)
{
    QueryResult r = CharacterDatabase.Query(
        "SELECT c.name, c.level, c.`class`, m.account_id, c.guid "
        "FROM levelsync_members m "
        "JOIN characters c ON m.char_guid = c.guid "
        "WHERE m.group_id = {} "
        "ORDER BY m.account_id ASC, c.level DESC",
        groupId);

    if (!r)
    {
        handler->PSendSysMessage("  (no members)");
        return;
    }

    uint32 currentAccount = 0;
    do
    {
        std::string name  = r->Fetch()[0].Get<std::string>();
        uint8  level      = r->Fetch()[1].Get<uint8>();
        uint8  cls        = r->Fetch()[2].Get<uint8>();
        uint32 accountId  = r->Fetch()[3].Get<uint32>();
        uint32 guid       = r->Fetch()[4].Get<uint32>();

        if (accountId != currentAccount)
        {
            QueryResult acctCount = CharacterDatabase.Query(
                "SELECT COUNT(*) FROM levelsync_members WHERE group_id = {} AND account_id = {}",
                groupId, accountId);
            uint64 charCount = acctCount ? acctCount->Fetch()[0].Get<uint64>() : 0;
            handler->PSendSysMessage("  Account {}: Characters: {}", accountId, charCount);
            currentAccount = accountId;
        }

        // Prefer the in-memory Player state when the character is online or
        // bot-loaded. The DB row only persists on the next save tick or
        // logout, so reading c.level / character_queststatus_rewarded
        // directly can lag behind reality (mod-levelsync's own sync writes
        // go through GiveLevel / RewardQuest, which update memory first).
        uint8 displayLevel = level;
        uint8 displayTier  = sLevelSync->GetCharacterIPTierPublic(guid);

        if (Player* p = ObjectAccessor::FindPlayerByLowGUID(guid))
        {
            displayLevel = p->GetLevel();
            displayTier  = 0;
            for (uint8 i = 1; i <= LEVELSYNC_IP_MAX_TIER; ++i)
            {
                if (p->GetQuestStatus(LEVELSYNC_IP_QUEST_BASE + i) == QUEST_STATUS_REWARDED)
                    displayTier = i;
            }
        }

        handler->PSendSysMessage("    |cff{}{}|r (lvl {}) (|cffffff00{}|r) IP Tier: |cff{}{} - {}|r",
            ClassColor(cls), CapFirst(name), static_cast<uint32>(displayLevel),
            ClassName(cls),
            IPTierColor(displayTier), static_cast<uint32>(displayTier), IPTierName(displayTier));
    } while (r->NextRow());
}

static void DisplayGroupStatus(ChatHandler* handler, uint32 groupId)
{
    uint32 accounts  = sLevelSync->GetGroupAccountCount(groupId);

    // Fire-only model: sync only runs when the player invokes
    //   .levelsync level on   or   .levelsync IP on
    // Status reflects whether the server conf permits the sync, NOT a
    // persistent per-group toggle. "Ready" = conf-enabled and the
    // command will fire when invoked; "Off" = conf-disabled.
    bool levelReady = sLevelSync->IsLevelSyncAllowed();
    bool progReady  = sLevelSync->IsProgressionAllowed();

    // OLD (per-group persistent toggle, kept for reference):
    // bool levelSync = sLevelSync->IsGroupLevelSyncEnabled(groupId);
    // bool progSync  = sLevelSync->IsGroupProgressionSyncEnabled(groupId);

    QueryResult countResult = CharacterDatabase.Query(
        "SELECT COUNT(*) FROM levelsync_members WHERE group_id = {}", groupId);
    uint64 totalChars = countResult ? countResult->Fetch()[0].Get<uint64>() : 0;

    handler->PSendSysMessage("|cff00ff00[LevelSync]|r Sync Group #{}", groupId);
    handler->PSendSysMessage("  Accounts: {}/{}", accounts, sLevelSync->GetMaxLinkedAccounts());
    handler->PSendSysMessage("  Total Characters: {}", totalChars);
    handler->PSendSysMessage("  Level sync: {}", levelReady ? "|cff00ff00Available|r" : "|cffff0000Disabled|r");
    handler->PSendSysMessage("  Progression sync: {}", progReady ? "|cff00ff00Available|r" : "|cffff0000Disabled|r");
    handler->PSendSysMessage("|cff00ff00[LevelSync]|r Group members:");
    DisplayGroupMembers(handler, groupId);
    handler->PSendSysMessage("|cff00ff00[LevelSync]|r For a graphical interface use the addon: |cff3399ffhttps://github.com/Lichborne-AC/LevelsyncUI|r");
}

// Creates a new group and returns its group_id.
//
// founderGuid is written into the row so the SELECT-after-INSERT can find it
// without relying on LAST_INSERT_ID(), which is per-connection and unsafe
// under AzerothCore's round-robin connection pool. The SELECT picks the most
// recent group_id for this founder so a player who has left a previous group
// gets a fresh one rather than silently rejoining the old one.
static uint32 CreateGroup(uint32 founderGuid)
{
    CharacterDatabase.DirectExecute(
        "INSERT INTO levelsync_groups (founder_guid, level_sync_enabled, sync_progression) VALUES ({}, 0, 0)",
        founderGuid);

    QueryResult r = CharacterDatabase.Query(
        "SELECT group_id FROM levelsync_groups WHERE founder_guid = {} ORDER BY group_id DESC LIMIT 1",
        founderGuid);
    if (!r)
        return 0;
    return r->Fetch()[0].Get<uint32>();
}

// Returns the group_id the character is in, or creates+joins a new one.
static uint32 GetOrCreateGroup(uint32 charGuid, uint32 accountId)
{
    uint32 groupId = sLevelSync->GetGroupId(charGuid);
    if (groupId)
        return groupId;

    groupId = CreateGroup(charGuid);
    if (!groupId)
        return 0;

    CharacterDatabase.Execute(
        "INSERT IGNORE INTO levelsync_members (group_id, char_guid, account_id) VALUES ({}, {}, {})",
        groupId, charGuid, accountId);

    return groupId;
}

// -----------------------------------------------------------------------
// Command class
// -----------------------------------------------------------------------

class LevelSyncCommandScript : public CommandScript
{
public:
    LevelSyncCommandScript() : CommandScript("LevelSyncCommandScript") {}

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable gmTable =
        {
            { "removeall",  HandleGmRemoveAllCommand,  SEC_GAMEMASTER, Console::No },
            { "xp",         HandleGmXpCommand,         SEC_GAMEMASTER, Console::No },
            { "unbindall",  HandleGmUnbindAllCommand,  SEC_GAMEMASTER, Console::No },
        };

        static ChatCommandTable commandTable =
        {
            { "setkey",         HandleSetKeyCommand,         SEC_PLAYER,     Console::No },
            { "addaccount",     HandleAddAccountCommand,     SEC_PLAYER,     Console::No },
            { "addchar",        HandleAddCharCommand,        SEC_PLAYER,     Console::No },
            { "removeaccount",  HandleRemoveAccountCommand,  SEC_PLAYER,     Console::No },
            { "removechar",     HandleRemoveCharCommand,     SEC_PLAYER,     Console::No },
            { "removeall",      HandleRemoveAllCommand,      SEC_PLAYER,     Console::No },
            { "disbandaccount", HandleDisbandAccountCommand, SEC_PLAYER,     Console::No },
            { "listaccount",    HandleListAccountCommand,    SEC_PLAYER,     Console::No },
            { "status",         HandleStatusCommand,         SEC_PLAYER,     Console::No },
            { "level",          HandleLevelCommand,          SEC_PLAYER,     Console::No },
            { "IP",               HandleIndivProgressionCommand, SEC_PLAYER, Console::No },
            { "money",          HandleMoneyCommand,          SEC_PLAYER,     Console::No },
            { "unbindall",      HandleUnbindAllCommand,      SEC_PLAYER,     Console::No },
            { "gm",             gmTable },
        };

        static ChatCommandTable rootTable =
        {
            { "levelsync", commandTable },
        };

        return rootTable;
    }

    // -------------------------------------------------------------------
    // .levelsync setkey <key>
    // -------------------------------------------------------------------
    static bool HandleSetKeyCommand(ChatHandler* handler, char const* args)
    {
        if (!sLevelSync->IsEnabled())
        {
            handler->PSendSysMessage("|cff00ff00[LevelSync]|r Module is disabled.");
            return true;
        }

        char* keyArg = strtok(const_cast<char*>(args), " ");
        if (!keyArg)
        {
            handler->PSendSysMessage("Usage: .levelsync setkey <key>");
            return true;
        }

        uint32 accountId = handler->GetSession()->GetAccountId();
        std::string hashed = HashKey(std::string(keyArg));

        CharacterDatabase.Execute(
            "INSERT INTO levelsync_account_keys (account_id, security_key) VALUES ({}, '{}') "
            "ON DUPLICATE KEY UPDATE security_key = '{}'",
            accountId, hashed, hashed);

        handler->PSendSysMessage("|cff00ff00[LevelSync]|r Account key set.");
        return true;
    }

    // -------------------------------------------------------------------
    // .levelsync addaccount <account> [key]
    // -------------------------------------------------------------------
    static bool HandleAddAccountCommand(ChatHandler* handler, char const* args)
    {
        if (!sLevelSync->IsEnabled())
        {
            handler->PSendSysMessage("|cff00ff00[LevelSync]|r Module is disabled.");
            return true;
        }

        char* accountArg = strtok(const_cast<char*>(args), " ");
        char* keyArg     = strtok(nullptr, " ");

        if (!accountArg)
        {
            handler->PSendSysMessage("Usage: .levelsync addaccount <account> [key]");
            return true;
        }

        Player* player        = handler->GetSession()->GetPlayer();
        uint32  myGuid        = player->GetGUID().GetCounter();
        uint32  myAccountId   = player->GetSession()->GetAccountId();

        uint32 targetAccountId = GetAccountIdByName(std::string(accountArg));
        if (!targetAccountId)
        {
            handler->PSendSysMessage("|cff00ff00[LevelSync]|r Account '{}' not found.", accountArg);
            return true;
        }

        bool ownAccount = (targetAccountId == myAccountId);

        if (!ownAccount)
        {
            if (!keyArg)
            {
                handler->PSendSysMessage("|cff00ff00[LevelSync]|r A key is required to link another account.");
                return true;
            }

            if (!VerifyAccountKey(targetAccountId, std::string(keyArg)))
            {
                handler->PSendSysMessage("|cff00ff00[LevelSync]|r Invalid key.");
                return true;
            }

            // Block if any char on target account is in a DIFFERENT group
            QueryResult conflict = CharacterDatabase.Query(
                "SELECT m.group_id FROM levelsync_members m "
                "JOIN characters c ON m.char_guid = c.guid "
                "WHERE c.account = {} LIMIT 1",
                targetAccountId);

            if (conflict)
            {
                uint32 theirGroup = conflict->Fetch()[0].Get<uint32>();
                uint32 myGroup    = sLevelSync->GetGroupId(myGuid);

                if (theirGroup != myGroup)
                {
                    handler->PSendSysMessage("|cff00ff00[LevelSync]|r That account is already in a different sync group.");
                    return true;
                }
            }
        }

        // Get or create this player's group
        uint32 groupId = GetOrCreateGroup(myGuid, myAccountId);
        if (!groupId)
        {
            handler->PSendSysMessage("|cff00ff00[LevelSync]|r Failed to create sync group.");
            return true;
        }

        // Slot check — only if target account isn't already represented in this group
        QueryResult acctInGroup = CharacterDatabase.Query(
            "SELECT 1 FROM levelsync_members m "
            "JOIN characters c ON m.char_guid = c.guid "
            "WHERE m.group_id = {} AND c.account = {} LIMIT 1",
            groupId, targetAccountId);

        if (!acctInGroup)
        {
            if (sLevelSync->GetGroupAccountCount(groupId) >= sLevelSync->GetMaxLinkedAccounts())
            {
                handler->PSendSysMessage("|cff00ff00[LevelSync]|r Group is full ({}/{} accounts).",
                    sLevelSync->GetGroupAccountCount(groupId),
                    sLevelSync->GetMaxLinkedAccounts());
                return true;
            }
        }

        QueryResult chars = CharacterDatabase.Query(
            "SELECT guid, name FROM characters WHERE account = {}", targetAccountId);

        if (!chars)
        {
            handler->PSendSysMessage("|cff00ff00[LevelSync]|r No characters found on account '{}'.", accountArg);
            return true;
        }

        // Insert all chars from target account
        uint32 linked = 0;
        do
        {
            uint32 charGuid      = chars->Fetch()[0].Get<uint32>();
            std::string charName = chars->Fetch()[1].Get<std::string>();

            // Skip if already in this group
            QueryResult alreadyHere = CharacterDatabase.Query(
                "SELECT 1 FROM levelsync_members WHERE char_guid = {} AND group_id = {}",
                charGuid, groupId);
            if (alreadyHere)
                continue;

            // Skip if in a different group
            QueryResult otherGroup = CharacterDatabase.Query(
                "SELECT group_id FROM levelsync_members WHERE char_guid = {}", charGuid);
            if (otherGroup)
            {
                handler->PSendSysMessage("|cff00ff00[LevelSync]|r Skipped {} — already in another group.", charName);
                continue;
            }

            CharacterDatabase.DirectExecute(
                "INSERT IGNORE INTO levelsync_members (group_id, char_guid, account_id) VALUES ({}, {}, {})",
                groupId, charGuid, targetAccountId);
            ++linked;
        } while (chars->NextRow());

        if (linked > 0)
        {
            bool levelWasOn = sLevelSync->IsGroupLevelSyncEnabled(groupId);
            bool ipWasOn    = sLevelSync->IsGroupProgressionSyncEnabled(groupId);

            if (levelWasOn)
                CharacterDatabase.DirectExecute(
                    "UPDATE levelsync_groups SET level_sync_enabled = 0 WHERE group_id = {}", groupId);
            if (ipWasOn)
                CharacterDatabase.DirectExecute(
                    "UPDATE levelsync_groups SET sync_progression = 0 WHERE group_id = {}", groupId);

            DisplayGroupStatus(handler, groupId);
            handler->PSendSysMessage("|cff00ff00[LevelSync]|r Linked {} character(s) from account '{}'.", linked, accountArg);
            if (levelWasOn)
                handler->PSendSysMessage("|cff00ff00[LevelSync]|r Level sync turned |cffff0000OFF|r — use .levelsync level on to sync.");
            if (ipWasOn)
                handler->PSendSysMessage("|cff00ff00[LevelSync]|r IP sync turned |cffff0000OFF|r — use .levelsync IP on to sync.");
        }
        else
            handler->PSendSysMessage("|cff00ff00[LevelSync]|r No new characters were linked from account '{}'.", accountArg);

        return true;
    }

    // -------------------------------------------------------------------
    // .levelsync addchar <charname> [key]
    // -------------------------------------------------------------------
    static bool HandleAddCharCommand(ChatHandler* handler, char const* args)
    {
        if (!sLevelSync->IsEnabled())
        {
            handler->PSendSysMessage("|cff00ff00[LevelSync]|r Module is disabled.");
            return true;
        }

        char* nameArg = strtok(const_cast<char*>(args), " ");
        char* keyArg  = strtok(nullptr, " ");

        if (!nameArg)
        {
            handler->PSendSysMessage("Usage: .levelsync addchar <charname> [key]");
            return true;
        }

        Player* player      = handler->GetSession()->GetPlayer();
        uint32  myGuid      = player->GetGUID().GetCounter();
        uint32  myAccountId = player->GetSession()->GetAccountId();

        std::string charName(nameArg);

        QueryResult charResult = CharacterDatabase.Query(
            "SELECT guid, account FROM characters WHERE LOWER(name) = LOWER('{}')", EscChar(charName));
        if (!charResult)
        {
            handler->PSendSysMessage("|cff00ff00[LevelSync]|r Character '{}' not found.", charName);
            return true;
        }

        uint32 targetGuid      = charResult->Fetch()[0].Get<uint32>();
        uint32 targetAccountId = charResult->Fetch()[1].Get<uint32>();
        bool   ownAccount      = (targetAccountId == myAccountId);

        if (!ownAccount)
        {
            if (!keyArg)
            {
                handler->PSendSysMessage("|cff00ff00[LevelSync]|r A key is required to link a character from another account.");
                return true;
            }

            if (!VerifyAccountKey(targetAccountId, std::string(keyArg)))
            {
                handler->PSendSysMessage("|cff00ff00[LevelSync]|r Invalid key.");
                return true;
            }
        }

        // Check target not already in a different group
        uint32 targetGroup = sLevelSync->GetGroupId(targetGuid);
        uint32 myGroup     = sLevelSync->GetGroupId(myGuid);

        if (targetGroup && targetGroup != myGroup)
        {
            handler->PSendSysMessage("|cff00ff00[LevelSync]|r {} is already in a different sync group.", charName);
            return true;
        }

        if (targetGroup && targetGroup == myGroup)
        {
            handler->PSendSysMessage("|cff00ff00[LevelSync]|r {} is already in your sync group.", charName);
            return true;
        }

        uint32 groupId = GetOrCreateGroup(myGuid, myAccountId);
        if (!groupId)
        {
            handler->PSendSysMessage("|cff00ff00[LevelSync]|r Failed to create sync group.");
            return true;
        }

        // Slot check — only if this account isn't already represented
        QueryResult acctInGroup = CharacterDatabase.Query(
            "SELECT 1 FROM levelsync_members m "
            "JOIN characters c ON m.char_guid = c.guid "
            "WHERE m.group_id = {} AND c.account = {} LIMIT 1",
            groupId, targetAccountId);

        if (!acctInGroup)
        {
            if (sLevelSync->GetGroupAccountCount(groupId) >= sLevelSync->GetMaxLinkedAccounts())
            {
                handler->PSendSysMessage("|cff00ff00[LevelSync]|r Group is full ({}/{} accounts).",
                    sLevelSync->GetGroupAccountCount(groupId),
                    sLevelSync->GetMaxLinkedAccounts());
                return true;
            }
        }

        CharacterDatabase.DirectExecute(
            "INSERT IGNORE INTO levelsync_members (group_id, char_guid, account_id) VALUES ({}, {}, {})",
            groupId, targetGuid, targetAccountId);

        bool levelWasOn = sLevelSync->IsGroupLevelSyncEnabled(groupId);
        bool ipWasOn    = sLevelSync->IsGroupProgressionSyncEnabled(groupId);

        if (levelWasOn)
            CharacterDatabase.DirectExecute(
                "UPDATE levelsync_groups SET level_sync_enabled = 0 WHERE group_id = {}", groupId);
        if (ipWasOn)
            CharacterDatabase.DirectExecute(
                "UPDATE levelsync_groups SET sync_progression = 0 WHERE group_id = {}", groupId);

        DisplayGroupStatus(handler, groupId);
        handler->PSendSysMessage("|cff00ff00[LevelSync]|r {} added to sync group.", CapFirst(charName));
        if (levelWasOn)
            handler->PSendSysMessage("|cff00ff00[LevelSync]|r Level sync turned |cffff0000OFF|r — use .levelsync level on to sync.");
        if (ipWasOn)
            handler->PSendSysMessage("|cff00ff00[LevelSync]|r IP sync turned |cffff0000OFF|r — use .levelsync IP on to sync.");

        return true;
    }

    // -------------------------------------------------------------------
    // .levelsync removeaccount <account>
    // .levelsync removeaccount # <accountid>
    // -------------------------------------------------------------------
    static bool HandleRemoveAccountCommand(ChatHandler* handler, char const* args)
    {
        if (!sLevelSync->IsEnabled())
        {
            handler->PSendSysMessage("|cff00ff00[LevelSync]|r Module is disabled.");
            return true;
        }

        char* accountArg = strtok(const_cast<char*>(args), " ");
        if (!accountArg)
        {
            handler->PSendSysMessage("Usage: .levelsync removeaccount <account> or .levelsync removeaccount # <accountid>");
            return true;
        }

        Player* player = handler->GetSession()->GetPlayer();
        uint32  myGuid = player->GetGUID().GetCounter();

        uint32 groupId = sLevelSync->GetGroupId(myGuid);
        if (!groupId)
        {
            handler->PSendSysMessage("|cff00ff00[LevelSync]|r You are not in a sync group.");
            return true;
        }

        uint32 targetAccountId = 0;
        std::string displayName;

        if (std::string(accountArg) == "#")
        {
            char* idArg = strtok(nullptr, " ");
            if (!idArg)
            {
                handler->PSendSysMessage("Usage: .levelsync removeaccount # <accountid>");
                return true;
            }
            targetAccountId = static_cast<uint32>(std::strtoul(idArg, nullptr, 10));
            if (!targetAccountId)
            {
                handler->PSendSysMessage("|cff00ff00[LevelSync]|r Invalid account ID.");
                return true;
            }
            displayName = std::string(idArg);
        }
        else
        {
            targetAccountId = GetAccountIdByName(std::string(accountArg));
            if (!targetAccountId)
            {
                handler->PSendSysMessage("|cff00ff00[LevelSync]|r Account '{}' not found.", accountArg);
                return true;
            }
            displayName = std::string(accountArg);
        }

        // Notify online members being removed
        QueryResult toRemove = CharacterDatabase.Query(
            "SELECT m.char_guid FROM levelsync_members m "
            "JOIN characters c ON m.char_guid = c.guid "
            "WHERE m.group_id = {} AND c.account = {}",
            groupId, targetAccountId);

        if (!toRemove)
        {
            handler->PSendSysMessage("|cff00ff00[LevelSync]|r Account '{}' is not in your sync group.", displayName);
            return true;
        }

        do
        {
            uint32 g = toRemove->Fetch()[0].Get<uint32>();
            if (Player* member = ObjectAccessor::FindPlayerByLowGUID(g))
                ChatHandler(member->GetSession()).PSendSysMessage(
                    "|cff00ff00[LevelSync]|r Your character has been removed from the sync group.");
        } while (toRemove->NextRow());

        CharacterDatabase.DirectExecute(
            "DELETE m FROM levelsync_members m "
            "JOIN characters c ON m.char_guid = c.guid "
            "WHERE m.group_id = {} AND c.account = {}",
            groupId, targetAccountId);

        DisplayGroupStatus(handler, groupId);
        handler->PSendSysMessage("|cff00ff00[LevelSync]|r Account '{}' removed from sync group.", displayName);
        return true;
    }

    // -------------------------------------------------------------------
    // .levelsync removechar <charname>
    // -------------------------------------------------------------------
    static bool HandleRemoveCharCommand(ChatHandler* handler, char const* args)
    {
        if (!sLevelSync->IsEnabled())
        {
            handler->PSendSysMessage("|cff00ff00[LevelSync]|r Module is disabled.");
            return true;
        }

        char* nameArg = strtok(const_cast<char*>(args), " ");
        if (!nameArg)
        {
            handler->PSendSysMessage("Usage: .levelsync removechar <charname>");
            return true;
        }

        Player* player = handler->GetSession()->GetPlayer();
        uint32  myGuid = player->GetGUID().GetCounter();

        uint32 groupId = sLevelSync->GetGroupId(myGuid);
        if (!groupId)
        {
            handler->PSendSysMessage("|cff00ff00[LevelSync]|r You are not in a sync group.");
            return true;
        }

        std::string charName(nameArg);
        QueryResult charResult = CharacterDatabase.Query(
            "SELECT guid, account FROM characters WHERE LOWER(name) = LOWER('{}')", EscChar(charName));
        if (!charResult)
        {
            handler->PSendSysMessage("|cff00ff00[LevelSync]|r Character '{}' not found.", charName);
            return true;
        }

        uint32 targetGuid  = charResult->Fetch()[0].Get<uint32>();
        uint32 targetGroup = sLevelSync->GetGroupId(targetGuid);

        if (targetGroup != groupId)
        {
            handler->PSendSysMessage("|cff00ff00[LevelSync]|r {} is not in your sync group.", charName);
            return true;
        }

        // Notify if online
        if (Player* member = ObjectAccessor::FindPlayerByLowGUID(targetGuid))
            ChatHandler(member->GetSession()).PSendSysMessage(
                "|cff00ff00[LevelSync]|r Your character has been removed from the sync group.");

        CharacterDatabase.DirectExecute(
            "DELETE FROM levelsync_members WHERE char_guid = {}", targetGuid);

        DisplayGroupStatus(handler, groupId);
        handler->PSendSysMessage("|cff00ff00[LevelSync]|r {} removed from sync group.", CapFirst(charName));
        return true;
    }

    // -------------------------------------------------------------------
    // .levelsync removeall  — full nuke
    // -------------------------------------------------------------------
    static bool HandleRemoveAllCommand(ChatHandler* handler, char const* /*args*/)
    {
        if (!sLevelSync->IsEnabled())
        {
            handler->PSendSysMessage("|cff00ff00[LevelSync]|r Module is disabled.");
            return true;
        }

        Player* player = handler->GetSession()->GetPlayer();
        uint32  myGuid = player->GetGUID().GetCounter();

        uint32 groupId = sLevelSync->GetGroupId(myGuid);
        if (!groupId)
        {
            handler->PSendSysMessage("|cff00ff00[LevelSync]|r You are not in a sync group.");
            return true;
        }

        // Notify all online members
        for (uint32 g : sLevelSync->GetGroupMemberGuids(groupId, myGuid))
        {
            if (Player* member = ObjectAccessor::FindPlayerByLowGUID(g))
                ChatHandler(member->GetSession()).PSendSysMessage(
                    "|cff00ff00[LevelSync]|r Your sync group has been disbanded.");
        }

        // Wipe members then group
        CharacterDatabase.Execute(
            "DELETE FROM levelsync_members WHERE group_id = {}", groupId);

        CharacterDatabase.Execute(
            "DELETE FROM levelsync_groups WHERE group_id = {}", groupId);

        handler->PSendSysMessage("|cff00ff00[LevelSync]|r Sync group disbanded.");
        return true;
    }

    // -------------------------------------------------------------------
    // .levelsync disbandaccount
    // -------------------------------------------------------------------
    static bool HandleDisbandAccountCommand(ChatHandler* handler, char const* /*args*/)
    {
        if (!sLevelSync->IsEnabled())
        {
            handler->PSendSysMessage("|cff00ff00[LevelSync]|r Module is disabled.");
            return true;
        }

        Player* player      = handler->GetSession()->GetPlayer();
        uint32  myAccountId = player->GetSession()->GetAccountId();

        QueryResult r = CharacterDatabase.Query(
            "SELECT DISTINCT m.group_id FROM levelsync_members m "
            "JOIN characters c ON m.char_guid = c.guid "
            "WHERE c.account = {}",
            myAccountId);

        if (!r)
        {
            handler->PSendSysMessage("|cff00ff00[LevelSync]|r No sync groups found for your account.");
            return true;
        }

        std::vector<uint32> groups;
        do {
            groups.push_back(r->Fetch()[0].Get<uint32>());
        } while (r->NextRow());

        for (uint32 groupId : groups)
        {
            for (uint32 g : sLevelSync->GetGroupMemberGuids(groupId))
            {
                if (Player* member = ObjectAccessor::FindPlayerByLowGUID(g))
                    ChatHandler(member->GetSession()).PSendSysMessage(
                        "|cff00ff00[LevelSync]|r Your sync group has been disbanded.");
            }

            CharacterDatabase.Execute(
                "DELETE FROM levelsync_members WHERE group_id = {}", groupId);

            CharacterDatabase.Execute(
                "DELETE FROM levelsync_groups WHERE group_id = {}", groupId);
        }

        handler->PSendSysMessage("|cff00ff00[LevelSync]|r Disbanded {} sync group(s). Keys preserved.",
            static_cast<uint32>(groups.size()));
        return true;
    }

    // -------------------------------------------------------------------
    // .levelsync listaccount <account>
    // -------------------------------------------------------------------
    static bool HandleListAccountCommand(ChatHandler* handler, char const* args)
    {
        if (!sLevelSync->IsEnabled())
        {
            handler->PSendSysMessage("|cff00ff00[LevelSync]|r Module is disabled.");
            return true;
        }

        char* accountArg = strtok(const_cast<char*>(args), " ");
        char* keyArg     = strtok(nullptr, " ");
        if (!accountArg)
        {
            handler->PSendSysMessage("Usage: .levelsync listaccount <account> [key]");
            return true;
        }

        Player* player      = handler->GetSession()->GetPlayer();
        uint32  myAccountId = player->GetSession()->GetAccountId();

        uint32 targetAccountId = GetAccountIdByName(std::string(accountArg));
        if (!targetAccountId)
        {
            handler->PSendSysMessage("|cff00ff00[LevelSync]|r Account '{}' not found.", accountArg);
            return true;
        }

        bool ownAccount = (targetAccountId == myAccountId);
        if (!ownAccount)
        {
            if (!keyArg)
            {
                handler->PSendSysMessage("|cff00ff00[LevelSync]|r A key is required to view another account's characters.");
                return true;
            }
            if (!VerifyAccountKey(targetAccountId, std::string(keyArg)))
            {
                handler->PSendSysMessage("|cff00ff00[LevelSync]|r Invalid key.");
                return true;
            }
        }

        QueryResult chars = CharacterDatabase.Query(
            "SELECT c.name, c.level, c.`class`, m.group_id "
            "FROM characters c "
            "LEFT JOIN levelsync_members m ON m.char_guid = c.guid "
            "WHERE c.account = {} "
            "ORDER BY c.level DESC",
            targetAccountId);

        if (!chars)
        {
            handler->PSendSysMessage("|cff00ff00[LevelSync]|r No characters on account '{}'.", accountArg);
            return true;
        }

        handler->PSendSysMessage("|cff00ff00[LevelSync]|r Characters on account '{}':", accountArg);
        do
        {
            std::string name  = chars->Fetch()[0].Get<std::string>();
            uint8 level       = chars->Fetch()[1].Get<uint8>();
            uint8 cls         = chars->Fetch()[2].Get<uint8>();
            uint32 grp        = chars->Fetch()[3].IsNull() ? 0 : chars->Fetch()[3].Get<uint32>();

            std::string status = grp ? "[Group " + std::to_string(grp) + "]" : "[No Group]";

            handler->PSendSysMessage("  |cff{}{}|r (lv {}) ({}) {}",
                ClassColor(cls), name, static_cast<uint32>(level), ClassName(cls), status);
        } while (chars->NextRow());

        return true;
    }

    // -------------------------------------------------------------------
    // .levelsync status
    // -------------------------------------------------------------------
    static bool HandleStatusCommand(ChatHandler* handler, char const* /*args*/)
    {
        Player* player = handler->GetSession()->GetPlayer();
        uint32  myGuid = player->GetGUID().GetCounter();

        // Per-character 1.5s cooldown. Silent fail on spam (no chat output).
        // Timer stamps only on accepted calls so a spam burst can't slide
        // the next allowed call forward.
        using clock = std::chrono::steady_clock;
        static std::unordered_map<uint32, clock::time_point> lastStatus;
        constexpr auto COOLDOWN = std::chrono::milliseconds(1500);
        auto now = clock::now();
        auto it = lastStatus.find(myGuid);
        if (it != lastStatus.end() && now - it->second < COOLDOWN)
            return true;
        lastStatus[myGuid] = now;

        if (!sLevelSync->IsEnabled())
        {
            handler->PSendSysMessage("|cff00ff00[LevelSync]|r Module is disabled.");
            return true;
        }

        uint32 groupId = sLevelSync->GetGroupId(myGuid);
        if (!groupId)
        {
            handler->PSendSysMessage("|cff00ff00[LevelSync]|r You are not in a sync group.");
            return true;
        }

        DisplayGroupStatus(handler, groupId);
        return true;
    }

    // -------------------------------------------------------------------
    // .levelsync level on
    //
    // Fire-only: runs a single level sync (highest member level
    // propagated to the rest of the group), then status returns to
    // "Ready". No persistent on/off state.
    // -------------------------------------------------------------------
    static bool HandleLevelCommand(ChatHandler* handler, char const* args)
    {
        if (!sLevelSync->IsEnabled())
        {
            handler->PSendSysMessage("|cff00ff00[LevelSync]|r Module is disabled.");
            return true;
        }

        if (!sLevelSync->IsLevelSyncAllowed())
        {
            handler->PSendSysMessage("|cff00ff00[LevelSync]|r Level sync is disabled by the server.");
            return true;
        }

        char* arg = strtok(const_cast<char*>(args), " ");
        if (!arg)
        {
            handler->PSendSysMessage("Usage: .levelsync level on");
            return true;
        }

        std::string argStr(arg);
        if (argStr == "off")
        {
            handler->PSendSysMessage("|cff00ff00[LevelSync]|r Level sync is fire-only — nothing to turn off. Use .levelsync level on to fire a sync.");
            return true;
        }
        if (argStr != "on")
        {
            handler->PSendSysMessage("Usage: .levelsync level on");
            return true;
        }

        Player* player = handler->GetSession()->GetPlayer();
        uint32  myGuid = player->GetGUID().GetCounter();

        uint32 groupId = sLevelSync->GetGroupId(myGuid);
        if (!groupId)
        {
            handler->PSendSysMessage("|cff00ff00[LevelSync]|r You are not in a sync group.");
            return true;
        }

        // 10s per-group cooldown for players. GMs bypass.
        bool isGM = handler->GetSession() && handler->GetSession()->GetSecurity() > SEC_PLAYER;
        if (!isGM)
        {
            uint32 secondsRemaining = 0;
            if (!sLevelSync->TryConsumeToggleCooldown(groupId, secondsRemaining))
            {
                handler->PSendSysMessage(
                    "|cff00ff00[LevelSync]|r Must wait |cffff0000{}|r second(s) before resync.",
                    secondsRemaining);
                return true;
            }
        }

        handler->PSendSysMessage("|cff00ff00[LevelSync]|r Syncing group to highest level...");
        sLevelSync->SyncGroupOnLevelToggle(groupId);

        DisplayGroupStatus(handler, groupId);
        handler->PSendSysMessage("|cff00ff00[LevelSync]|r Level sync fired. Status is back to |cff00ff00Available|r (10s cooldown).");

        return true;

        // OLD (persistent on/off toggle, kept for reference):
        // bool enable = (std::string(arg) == "on");
        // CharacterDatabase.DirectExecute(
        //     "UPDATE levelsync_groups SET level_sync_enabled = {} WHERE group_id = {}",
        //     enable ? 1 : 0, groupId);
        // if (enable)
        // {
        //     handler->PSendSysMessage("|cff00ff00[LevelSync]|r Syncing group to highest level...");
        //     sLevelSync->SyncGroupOnLevelToggle(groupId);
        // }
        // DisplayGroupStatus(handler, groupId);
        // handler->PSendSysMessage("|cff00ff00[LevelSync]|r Level sync {}.", enable ? "|cff00ff00enabled|r" : "|cffff0000disabled|r");
    }

    // -------------------------------------------------------------------
    // .levelsync IP on
    //
    // Fire-only: runs a single IP-tier sync (highest tier in the group
    // propagated to the rest), then status returns to "Ready". No
    // persistent on/off state.
    // -------------------------------------------------------------------
    static bool HandleIndivProgressionCommand(ChatHandler* handler, char const* args)
    {
        if (!sLevelSync->IsEnabled())
        {
            handler->PSendSysMessage("|cff00ff00[LevelSync]|r Module is disabled.");
            return true;
        }

        if (!sLevelSync->IsProgressionAllowed())
        {
            handler->PSendSysMessage("|cff00ff00[LevelSync]|r Progression sync is disabled by the server.");
            return true;
        }

        char* arg = strtok(const_cast<char*>(args), " ");
        if (!arg)
        {
            handler->PSendSysMessage("Usage: .levelsync IP on");
            return true;
        }

        std::string argStr(arg);
        if (argStr == "off")
        {
            handler->PSendSysMessage("|cff00ff00[LevelSync]|r IP sync is fire-only — nothing to turn off. Use .levelsync IP on to fire a sync.");
            return true;
        }
        if (argStr != "on")
        {
            handler->PSendSysMessage("Usage: .levelsync IP on");
            return true;
        }

        Player* player = handler->GetSession()->GetPlayer();
        uint32  myGuid = player->GetGUID().GetCounter();

        uint32 groupId = sLevelSync->GetGroupId(myGuid);
        if (!groupId)
        {
            handler->PSendSysMessage("|cff00ff00[LevelSync]|r You are not in a sync group.");
            return true;
        }

        // 10s per-group cooldown for players. GMs bypass.
        bool isGM = handler->GetSession() && handler->GetSession()->GetSecurity() > SEC_PLAYER;
        if (!isGM)
        {
            uint32 secondsRemaining = 0;
            if (!sLevelSync->TryConsumeToggleCooldown(groupId, secondsRemaining))
            {
                handler->PSendSysMessage(
                    "|cff00ff00[LevelSync]|r Must wait |cffff0000{}|r second(s) before resync.",
                    secondsRemaining);
                return true;
            }
        }

        handler->PSendSysMessage("|cff00ff00[LevelSync]|r Syncing group progression...");
        sLevelSync->SyncIPOnToggle(groupId);

        DisplayGroupStatus(handler, groupId);
        handler->PSendSysMessage("|cff00ff00[LevelSync]|r IP sync fired. Status is back to |cff00ff00Available|r (10s cooldown).");

        return true;

        // OLD (persistent on/off toggle, kept for reference):
        // bool enable = (std::string(arg) == "on");
        // CharacterDatabase.DirectExecute(
        //     "UPDATE levelsync_groups SET sync_progression = {} WHERE group_id = {}",
        //     enable ? 1 : 0, groupId);
        // if (enable)
        // {
        //     handler->PSendSysMessage("|cff00ff00[LevelSync]|r Syncing group progression...");
        //     sLevelSync->SyncIPOnToggle(groupId);
        // }
        // DisplayGroupStatus(handler, groupId);
        // handler->PSendSysMessage("|cff00ff00[LevelSync]|r Progression sync {}.", enable ? "|cff00ff00enabled|r" : "|cffff0000disabled|r");
    }

    // -------------------------------------------------------------------
    // .levelsync money
    //
    // Pools every other group member's gold (online + offline) into the
    // caller's wallet. One-shot — no toggle, no persistent state. Shares
    // the 10-second per-group toggle cooldown so it can't be spammed.
    // Online drains use Player::SetMoney(0) so PLAYER_FIELD_COINAGE marks
    // dirty and the client wallet refreshes within a tick; offline drains
    // are a direct UPDATE on characters.money. Caller's pre-pool money is
    // untouched and the credit goes through Player::ModifyMoney.
    // -------------------------------------------------------------------
    static bool HandleMoneyCommand(ChatHandler* handler, char const* /*args*/)
    {
        if (!sLevelSync->IsEnabled())
        {
            handler->PSendSysMessage("|cff00ff00[LevelSync]|r Module is disabled.");
            return true;
        }

        if (!sLevelSync->IsMoneyCommandsAllowed())
        {
            handler->PSendSysMessage("|cff00ff00[LevelSync]|r Money commands are disabled by the server.");
            return true;
        }

        Player* player = handler->GetSession()->GetPlayer();
        uint32  myGuid = player->GetGUID().GetCounter();

        uint32 groupId = sLevelSync->GetGroupId(myGuid);
        if (!groupId)
        {
            handler->PSendSysMessage("|cff00ff00[LevelSync]|r You are not in a sync group.");
            return true;
        }

        bool isGM = handler->GetSession() && handler->GetSession()->GetSecurity() > SEC_PLAYER;
        if (!isGM)
        {
            uint32 secondsRemaining = 0;
            if (!sLevelSync->TryConsumeToggleCooldown(groupId, secondsRemaining))
            {
                handler->PSendSysMessage(
                    "|cff00ff00[LevelSync]|r Must wait |cffff0000{}|r second(s) before resync.",
                    secondsRemaining);
                return true;
            }
        }

        auto result = sLevelSync->PoolGroupMoney(player, groupId);

        switch (result.status)
        {
            case LevelSyncMgr::PoolStatus::AloneInGroup:
                handler->PSendSysMessage(
                    "|cff00ff00[LevelSync]|r No other members in your sync group.");
                return true;

            case LevelSyncMgr::PoolStatus::NoGold:
                handler->PSendSysMessage(
                    "|cff00ff00[LevelSync]|r Group has no gold to pool.");
                return true;

            case LevelSyncMgr::PoolStatus::CapExceeded:
            {
                std::string total = FormatMoneyDisplay(
                    result.totalDrained > 0xFFFFFFFFull
                        ? 0xFFFFFFFFu
                        : uint32(result.totalDrained));
                handler->PSendSysMessage(
                    "|cff00ff00[LevelSync]|r Total |cffffd700{}|r would exceed "
                    "the gold cap. Withdraw manually first.",
                    total);
                return true;
            }

            case LevelSyncMgr::PoolStatus::Ok:
            {
                std::string total = FormatMoneyDisplay(
                    result.totalDrained > 0xFFFFFFFFull
                        ? 0xFFFFFFFFu
                        : uint32(result.totalDrained));
                handler->PSendSysMessage(
                    "|cff00ff00[LevelSync]|r Pooled |cffffd700{}|r from {} member(s) "
                    "into your wallet.",
                    total, result.contributors);
                return true;
            }
        }

        return true;
    }

    // -------------------------------------------------------------------
    // .levelsync unbindall [name]
    //
    // Non-GM equivalent of the core `.instance unbind all` command. Wipes
    // every bound instance across all 4 difficulty slots (dungeons +
    // raids), preserving the binding for the map the target is currently
    // inside.
    //
    // Target resolution (overloaded):
    //   - No arg → clicked/tab-selected player, falls back to self.
    //   - Name arg → online player matching that name (case-insensitive).
    //                Refused if the named player is offline / not found.
    //
    // Gated by LevelSync.AllowRaidUnbind (default 0). No cooldown.
    // -------------------------------------------------------------------
    static bool HandleUnbindAllCommand(ChatHandler* handler, char const* args)
    {
        if (!sLevelSync->IsEnabled())
        {
            handler->PSendSysMessage("|cff00ff00[LevelSync]|r Module is disabled.");
            return true;
        }

        if (!sLevelSync->IsRaidUnbindAllowed())
        {
            handler->PSendSysMessage("|cff00ff00[LevelSync]|r Unbind is disabled by the server.");
            return true;
        }

        Player* target = nullptr;
        char* nameArg  = strtok(const_cast<char*>(args), " ");

        if (nameArg && *nameArg)
        {
            // Name lookup is case-insensitive; FindPlayerByName does the
            // normalisation internally and returns nullptr if the player
            // is offline or doesn't exist.
            target = ObjectAccessor::FindPlayerByName(std::string(nameArg));
            if (!target)
            {
                handler->PSendSysMessage(
                    "|cff00ff00[LevelSync]|r '{}' not found or offline.", nameArg);
                return true;
            }
        }
        else
        {
            target = handler->getSelectedPlayer();
            if (!target)
                target = handler->GetSession()->GetPlayer();
        }

        if (!target)
            return true;

        uint32 counter = UnbindAllInstancesOn(target);

        if (target == handler->GetSession()->GetPlayer())
            handler->PSendSysMessage("|cff00ff00[LevelSync]|r Unbound |cffffff00{}|r instance(s).", counter);
        else
            handler->PSendSysMessage("|cff00ff00[LevelSync]|r Unbound |cffffff00{}|r instance(s) on {}.",
                counter, target->GetName());
        return true;
    }

    // -------------------------------------------------------------------
    // .levelsync gm unbindall [name]
    //
    // GM-gated counterpart to .levelsync unbindall. Same overloaded
    // target resolution: clicked/tab-selected player with self fallback
    // if no arg, or online named player (case-insensitive) if given.
    // Always available to GMs regardless of LevelSync.AllowRaidUnbind —
    // the conf flag only controls the SEC_PLAYER form.
    // -------------------------------------------------------------------
    static bool HandleGmUnbindAllCommand(ChatHandler* handler, char const* args)
    {
        Player* target = nullptr;
        char* nameArg  = strtok(const_cast<char*>(args), " ");

        if (nameArg && *nameArg)
        {
            target = ObjectAccessor::FindPlayerByName(std::string(nameArg));
            if (!target)
            {
                handler->PSendSysMessage(
                    "|cff00ff00[LevelSync]|r '{}' not found or offline.", nameArg);
                return true;
            }
        }
        else
        {
            target = handler->getSelectedPlayer();
            if (!target)
                target = handler->GetSession()->GetPlayer();
        }

        if (!target)
            return true;

        uint32 counter = UnbindAllInstancesOn(target);

        if (target == handler->GetSession()->GetPlayer())
            handler->PSendSysMessage("|cff00ff00[LevelSync]|r Unbound |cffffff00{}|r instance(s).", counter);
        else
            handler->PSendSysMessage("|cff00ff00[LevelSync]|r Unbound |cffffff00{}|r instance(s) on {}.",
                counter, target->GetName());
        return true;
    }

    // -------------------------------------------------------------------
    // .levelsync gm removeall <account|charname>
    // -------------------------------------------------------------------
    static bool HandleGmRemoveAllCommand(ChatHandler* handler, char const* args)
    {
        char* arg = strtok(const_cast<char*>(args), " ");
        if (!arg)
        {
            handler->PSendSysMessage("Usage: .levelsync gm removeall <charname>");
            return true;
        }

        std::string target(arg);

        QueryResult charResult = CharacterDatabase.Query(
            "SELECT account FROM characters WHERE LOWER(name) = LOWER('{}')", EscChar(target));

        if (!charResult)
        {
            handler->PSendSysMessage("|cff00ff00[LevelSync]|r Character '{}' not found.", target);
            return true;
        }

        uint32 accountId = charResult->Fetch()[0].Get<uint32>();

        // Find their group via any character on this account
        QueryResult r = CharacterDatabase.Query(
            "SELECT DISTINCT m.group_id FROM levelsync_members m "
            "JOIN characters c ON m.char_guid = c.guid "
            "WHERE c.account = {} LIMIT 1",
            accountId);

        if (!r)
        {
            // Not in a group — just nuke their key
            CharacterDatabase.Execute(
                "DELETE FROM levelsync_account_keys WHERE account_id = {}", accountId);
            handler->PSendSysMessage("|cff00ff00[LevelSync]|r '{}' is not in a sync group. Account key removed.", target);
            return true;
        }

        uint32 groupId = r->Fetch()[0].Get<uint32>();

        // Notify all online members
        for (uint32 g : sLevelSync->GetGroupMemberGuids(groupId))
        {
            if (Player* member = ObjectAccessor::FindPlayerByLowGUID(g))
                ChatHandler(member->GetSession()).PSendSysMessage(
                    "|cff00ff00[LevelSync]|r Your sync group has been disbanded by a GM.");
        }

        // Wipe keys, members, group
        CharacterDatabase.DirectExecute(
            "DELETE ak FROM levelsync_account_keys ak "
            "JOIN levelsync_members m ON ak.account_id = m.account_id "
            "WHERE m.group_id = {}",
            groupId);

        CharacterDatabase.Execute(
            "DELETE FROM levelsync_members WHERE group_id = {}", groupId);

        CharacterDatabase.Execute(
            "DELETE FROM levelsync_groups WHERE group_id = {}", groupId);

        handler->PSendSysMessage("|cff00ff00[LevelSync]|r Group #{} fully disbanded. All keys removed.", groupId);
        return true;
    }

    // -------------------------------------------------------------------
    // .levelsync gm xp <amount>
    //
    // Adds XP to the GM's current target (or self if no target). Uses the
    // engine's normal Player::GiveXP path so any resulting level-up is
    // handled correctly. Required because AC has no built-in xp modify
    // command and direct DB writes are invisible to online characters.
    // -------------------------------------------------------------------
    static bool HandleGmXpCommand(ChatHandler* handler, char const* args)
    {
        char* arg = strtok(const_cast<char*>(args), " ");
        if (!arg)
        {
            handler->PSendSysMessage("Usage: .levelsync gm xp <amount>");
            return true;
        }

        int32 amount = atoi(arg);
        if (amount <= 0)
        {
            handler->PSendSysMessage("|cff00ff00[LevelSync]|r XP amount must be a positive integer.");
            return true;
        }

        Player* target = handler->getSelectedPlayer();
        if (!target)
            target = handler->GetSession()->GetPlayer();

        if (!target)
        {
            handler->PSendSysMessage("|cff00ff00[LevelSync]|r No valid target.");
            return true;
        }

        target->GiveXP(static_cast<uint32>(amount), nullptr);

        handler->PSendSysMessage(
            "|cff00ff00[LevelSync]|r Granted {} XP to {} (now level {}, xp {}).",
            amount, target->GetName(),
            static_cast<uint32>(target->GetLevel()),
            target->GetUInt32Value(PLAYER_XP));
        return true;
    }
};

void AddSC_LevelSyncCommands()
{
    new LevelSyncCommandScript();
}
