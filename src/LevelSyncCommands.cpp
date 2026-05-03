#include "LevelSync.h"
#include "Chat.h"
#include "ChatCommand.h"
#include "CharacterCache.h"
#include "ObjectAccessor.h"
#include "ObjectGuid.h"
#include "Player.h"
#include "ScriptMgr.h"
#include <openssl/sha.h>
#include <iomanip>
#include <sstream>

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
        case 0:  return "Starting Point";
        case 1:  return "Molten Core";
        case 2:  return "Onyxia";
        case 3:  return "Blackwing Lair";
        case 4:  return "Pre-AQ";
        case 5:  return "AQ War Effort";
        case 6:  return "Ahn'Qiraj";
        case 7:  return "Naxxramas 40";
        case 8:  return "Pre-TBC";
        case 9:  return "Karazhan / Gruul / Magtheridon";
        case 10: return "Serpentshrine Cavern / Tempest Keep";
        case 11: return "Hyjal Summit / Black Temple";
        case 12: return "Zul'Aman";
        case 13: return "Sunwell Plateau";
        case 14: return "Naxxramas / Eye of Eternity / Obsidian Sanctum";
        case 15: return "Ulduar";
        case 16: return "Trial of the Crusader";
        case 17: return "Icecrown Citadel";
        case 18: return "Ruby Sanctum";
        default: return "Unknown (IP may not be enabled)";
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

        uint8 tier = sLevelSync->GetCharacterIPTierPublic(guid);
        if (tier == 0)
        {
            handler->PSendSysMessage("    |cff{}{}|r (lvl {}) ({}) IP Tier: None",
                ClassColor(cls), name, static_cast<uint32>(level), ClassName(cls));
        }
        else
        {
            handler->PSendSysMessage("    |cff{}{}|r (lvl {}) ({}) IP Tier: {} - {}",
                ClassColor(cls), name, static_cast<uint32>(level), ClassName(cls),
                static_cast<uint32>(tier), IPTierName(tier));
        }
    } while (r->NextRow());
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
        };

        static ChatCommandTable commandTable =
        {
            { "setkey",         HandleSetKeyCommand,         SEC_PLAYER,     Console::No },
            { "addaccount",     HandleAddAccountCommand,     SEC_PLAYER,     Console::No },
            { "addchar",        HandleAddCharCommand,        SEC_PLAYER,     Console::No },
            { "removeaccount",  HandleRemoveAccountCommand,  SEC_PLAYER,     Console::No },
            { "removechar",     HandleRemoveCharCommand,     SEC_PLAYER,     Console::No },
            { "removeall",      HandleRemoveAllCommand,      SEC_PLAYER,     Console::No },
            { "listaccount",    HandleListAccountCommand,    SEC_PLAYER,     Console::No },
            { "status",         HandleStatusCommand,         SEC_PLAYER,     Console::No },
            { "level",          HandleLevelCommand,          SEC_PLAYER,     Console::No },
            { "IP",               HandleIndivProgressionCommand, SEC_PLAYER, Console::No },
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
            handler->PSendSysMessage("[LevelSync] Module is disabled.");
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

        handler->PSendSysMessage("[LevelSync] Account key set.");
        return true;
    }

    // -------------------------------------------------------------------
    // .levelsync addaccount <account> [key]
    // -------------------------------------------------------------------
    static bool HandleAddAccountCommand(ChatHandler* handler, char const* args)
    {
        if (!sLevelSync->IsEnabled())
        {
            handler->PSendSysMessage("[LevelSync] Module is disabled.");
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
            handler->PSendSysMessage("[LevelSync] Account '{}' not found.", accountArg);
            return true;
        }

        bool ownAccount = (targetAccountId == myAccountId);

        if (!ownAccount)
        {
            if (!keyArg)
            {
                handler->PSendSysMessage("[LevelSync] A key is required to link another account.");
                return true;
            }

            if (!VerifyAccountKey(targetAccountId, std::string(keyArg)))
            {
                handler->PSendSysMessage("[LevelSync] Invalid key.");
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
                    handler->PSendSysMessage("[LevelSync] That account is already in a different sync group.");
                    return true;
                }
            }
        }

        // Get or create this player's group
        uint32 groupId = GetOrCreateGroup(myGuid, myAccountId);
        if (!groupId)
        {
            handler->PSendSysMessage("[LevelSync] Failed to create sync group.");
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
                handler->PSendSysMessage("[LevelSync] Group is full ({}/{} accounts).",
                    sLevelSync->GetGroupAccountCount(groupId),
                    sLevelSync->GetMaxLinkedAccounts());
                return true;
            }
        }

        QueryResult chars = CharacterDatabase.Query(
            "SELECT guid, name FROM characters WHERE account = {}", targetAccountId);

        if (!chars)
        {
            handler->PSendSysMessage("[LevelSync] No characters found on account '{}'.", accountArg);
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
                handler->PSendSysMessage("[LevelSync] Skipped {} — already in another group.", charName);
                continue;
            }

            CharacterDatabase.Execute(
                "INSERT IGNORE INTO levelsync_members (group_id, char_guid, account_id) VALUES ({}, {}, {})",
                groupId, charGuid, targetAccountId);
            ++linked;
        } while (chars->NextRow());

        if (linked > 0)
        {
            bool levelWasOn = sLevelSync->IsGroupLevelSyncEnabled(groupId);
            bool ipWasOn    = sLevelSync->IsGroupProgressionSyncEnabled(groupId);

            if (levelWasOn)
                CharacterDatabase.Execute(
                    "UPDATE levelsync_groups SET level_sync_enabled = 0 WHERE group_id = {}", groupId);
            if (ipWasOn)
                CharacterDatabase.Execute(
                    "UPDATE levelsync_groups SET sync_progression = 0 WHERE group_id = {}", groupId);

            handler->PSendSysMessage("[LevelSync] Linked {} character(s) from account '{}'.", linked, accountArg);
            if (levelWasOn)
                handler->PSendSysMessage("[LevelSync] Level sync turned OFF — use .levelsync level on to sync.");
            if (ipWasOn)
                handler->PSendSysMessage("[LevelSync] IP sync turned OFF — use .levelsync IP on to sync.");
        }
        else
            handler->PSendSysMessage("[LevelSync] No new characters were linked from account '{}'.", accountArg);

        return true;
    }

    // -------------------------------------------------------------------
    // .levelsync addchar <charname> [key]
    // -------------------------------------------------------------------
    static bool HandleAddCharCommand(ChatHandler* handler, char const* args)
    {
        if (!sLevelSync->IsEnabled())
        {
            handler->PSendSysMessage("[LevelSync] Module is disabled.");
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
            handler->PSendSysMessage("[LevelSync] Character '{}' not found.", charName);
            return true;
        }

        uint32 targetGuid      = charResult->Fetch()[0].Get<uint32>();
        uint32 targetAccountId = charResult->Fetch()[1].Get<uint32>();
        bool   ownAccount      = (targetAccountId == myAccountId);

        if (!ownAccount)
        {
            if (!keyArg)
            {
                handler->PSendSysMessage("[LevelSync] A key is required to link a character from another account.");
                return true;
            }

            if (!VerifyAccountKey(targetAccountId, std::string(keyArg)))
            {
                handler->PSendSysMessage("[LevelSync] Invalid key.");
                return true;
            }
        }

        // Check target not already in a different group
        uint32 targetGroup = sLevelSync->GetGroupId(targetGuid);
        uint32 myGroup     = sLevelSync->GetGroupId(myGuid);

        if (targetGroup && targetGroup != myGroup)
        {
            handler->PSendSysMessage("[LevelSync] {} is already in a different sync group.", charName);
            return true;
        }

        if (targetGroup && targetGroup == myGroup)
        {
            handler->PSendSysMessage("[LevelSync] {} is already in your sync group.", charName);
            return true;
        }

        uint32 groupId = GetOrCreateGroup(myGuid, myAccountId);
        if (!groupId)
        {
            handler->PSendSysMessage("[LevelSync] Failed to create sync group.");
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
                handler->PSendSysMessage("[LevelSync] Group is full ({}/{} accounts).",
                    sLevelSync->GetGroupAccountCount(groupId),
                    sLevelSync->GetMaxLinkedAccounts());
                return true;
            }
        }

        CharacterDatabase.Execute(
            "INSERT IGNORE INTO levelsync_members (group_id, char_guid, account_id) VALUES ({}, {}, {})",
            groupId, targetGuid, targetAccountId);

        bool levelWasOn = sLevelSync->IsGroupLevelSyncEnabled(groupId);
        bool ipWasOn    = sLevelSync->IsGroupProgressionSyncEnabled(groupId);

        if (levelWasOn)
            CharacterDatabase.Execute(
                "UPDATE levelsync_groups SET level_sync_enabled = 0 WHERE group_id = {}", groupId);
        if (ipWasOn)
            CharacterDatabase.Execute(
                "UPDATE levelsync_groups SET sync_progression = 0 WHERE group_id = {}", groupId);

        handler->PSendSysMessage("[LevelSync] {} added to sync group.", charName);
        if (levelWasOn)
            handler->PSendSysMessage("[LevelSync] Level sync turned OFF — use .levelsync level on to sync.");
        if (ipWasOn)
            handler->PSendSysMessage("[LevelSync] IP sync turned OFF — use .levelsync IP on to sync.");

        return true;
    }

    // -------------------------------------------------------------------
    // .levelsync removeaccount <account>
    // -------------------------------------------------------------------
    static bool HandleRemoveAccountCommand(ChatHandler* handler, char const* args)
    {
        if (!sLevelSync->IsEnabled())
        {
            handler->PSendSysMessage("[LevelSync] Module is disabled.");
            return true;
        }

        char* accountArg = strtok(const_cast<char*>(args), " ");
        if (!accountArg)
        {
            handler->PSendSysMessage("Usage: .levelsync removeaccount <account>");
            return true;
        }

        Player* player = handler->GetSession()->GetPlayer();
        uint32  myGuid = player->GetGUID().GetCounter();

        uint32 groupId = sLevelSync->GetGroupId(myGuid);
        if (!groupId)
        {
            handler->PSendSysMessage("[LevelSync] You are not in a sync group.");
            return true;
        }

        uint32 targetAccountId = GetAccountIdByName(std::string(accountArg));
        if (!targetAccountId)
        {
            handler->PSendSysMessage("[LevelSync] Account '{}' not found.", accountArg);
            return true;
        }

        // Notify online members being removed
        QueryResult toRemove = CharacterDatabase.Query(
            "SELECT m.char_guid FROM levelsync_members m "
            "JOIN characters c ON m.char_guid = c.guid "
            "WHERE m.group_id = {} AND c.account = {}",
            groupId, targetAccountId);

        if (!toRemove)
        {
            handler->PSendSysMessage("[LevelSync] Account '{}' is not in your sync group.", accountArg);
            return true;
        }

        do
        {
            uint32 g = toRemove->Fetch()[0].Get<uint32>();
            if (Player* member = ObjectAccessor::FindPlayerByLowGUID(g))
                ChatHandler(member->GetSession()).PSendSysMessage(
                    "|cff00ff00[LevelSync]|r Your character has been removed from the sync group.");
        } while (toRemove->NextRow());

        CharacterDatabase.Execute(
            "DELETE m FROM levelsync_members m "
            "JOIN characters c ON m.char_guid = c.guid "
            "WHERE m.group_id = {} AND c.account = {}",
            groupId, targetAccountId);

        handler->PSendSysMessage("[LevelSync] Account '{}' removed from sync group.", accountArg);
        return true;
    }

    // -------------------------------------------------------------------
    // .levelsync removechar <charname>
    // -------------------------------------------------------------------
    static bool HandleRemoveCharCommand(ChatHandler* handler, char const* args)
    {
        if (!sLevelSync->IsEnabled())
        {
            handler->PSendSysMessage("[LevelSync] Module is disabled.");
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
            handler->PSendSysMessage("[LevelSync] You are not in a sync group.");
            return true;
        }

        std::string charName(nameArg);
        QueryResult charResult = CharacterDatabase.Query(
            "SELECT guid FROM characters WHERE LOWER(name) = LOWER('{}')", EscChar(charName));
        if (!charResult)
        {
            handler->PSendSysMessage("[LevelSync] Character '{}' not found.", charName);
            return true;
        }

        uint32 targetGuid  = charResult->Fetch()[0].Get<uint32>();
        uint32 targetGroup = sLevelSync->GetGroupId(targetGuid);

        if (targetGroup != groupId)
        {
            handler->PSendSysMessage("[LevelSync] {} is not in your sync group.", charName);
            return true;
        }

        // Notify if online
        if (Player* member = ObjectAccessor::FindPlayerByLowGUID(targetGuid))
            ChatHandler(member->GetSession()).PSendSysMessage(
                "|cff00ff00[LevelSync]|r Your character has been removed from the sync group.");

        CharacterDatabase.Execute(
            "DELETE FROM levelsync_members WHERE char_guid = {}", targetGuid);

        handler->PSendSysMessage("[LevelSync] {} removed from sync group.", charName);
        return true;
    }

    // -------------------------------------------------------------------
    // .levelsync removeall  — full nuke
    // -------------------------------------------------------------------
    static bool HandleRemoveAllCommand(ChatHandler* handler, char const* /*args*/)
    {
        if (!sLevelSync->IsEnabled())
        {
            handler->PSendSysMessage("[LevelSync] Module is disabled.");
            return true;
        }

        Player* player      = handler->GetSession()->GetPlayer();
        uint32  myGuid      = player->GetGUID().GetCounter();
        uint32  myAccountId = player->GetSession()->GetAccountId();

        uint32 groupId = sLevelSync->GetGroupId(myGuid);
        if (!groupId)
        {
            CharacterDatabase.Execute(
                "DELETE FROM levelsync_account_keys WHERE account_id = {}", myAccountId);
            handler->PSendSysMessage("[LevelSync] You are not in a sync group. Account key removed.");
            return true;
        }

        // Notify all online members
        for (uint32 g : sLevelSync->GetGroupMemberGuids(groupId, myGuid))
        {
            if (Player* member = ObjectAccessor::FindPlayerByLowGUID(g))
                ChatHandler(member->GetSession()).PSendSysMessage(
                    "|cff00ff00[LevelSync]|r Your sync group has been disbanded.");
        }

        // Wipe account keys for all accounts in the group
        CharacterDatabase.DirectExecute(
            "DELETE ak FROM levelsync_account_keys ak "
            "JOIN levelsync_members m ON ak.account_id = m.account_id "
            "WHERE m.group_id = {}",
            groupId);

        // Wipe members then group
        CharacterDatabase.Execute(
            "DELETE FROM levelsync_members WHERE group_id = {}", groupId);

        CharacterDatabase.Execute(
            "DELETE FROM levelsync_groups WHERE group_id = {}", groupId);

        handler->PSendSysMessage("[LevelSync] Sync group disbanded. All keys removed.");
        return true;
    }

    // -------------------------------------------------------------------
    // .levelsync listaccount <account>
    // -------------------------------------------------------------------
    static bool HandleListAccountCommand(ChatHandler* handler, char const* args)
    {
        if (!sLevelSync->IsEnabled())
        {
            handler->PSendSysMessage("[LevelSync] Module is disabled.");
            return true;
        }

        char* accountArg = strtok(const_cast<char*>(args), " ");
        if (!accountArg)
        {
            handler->PSendSysMessage("Usage: .levelsync listaccount <account>");
            return true;
        }

        uint32 targetAccountId = GetAccountIdByName(std::string(accountArg));
        if (!targetAccountId)
        {
            handler->PSendSysMessage("[LevelSync] Account '{}' not found.", accountArg);
            return true;
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
            handler->PSendSysMessage("[LevelSync] No characters on account '{}'.", accountArg);
            return true;
        }

        handler->PSendSysMessage("[LevelSync] Characters on account '{}':", accountArg);
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
        if (!sLevelSync->IsEnabled())
        {
            handler->PSendSysMessage("[LevelSync] Module is disabled.");
            return true;
        }

        Player* player = handler->GetSession()->GetPlayer();
        uint32  myGuid = player->GetGUID().GetCounter();

        uint32 groupId = sLevelSync->GetGroupId(myGuid);
        if (!groupId)
        {
            handler->PSendSysMessage("[LevelSync] You are not in a sync group.");
            return true;
        }

        uint32 accounts   = sLevelSync->GetGroupAccountCount(groupId);
        bool   levelSync  = sLevelSync->IsGroupLevelSyncEnabled(groupId);
        bool   progSync   = sLevelSync->IsGroupProgressionSyncEnabled(groupId);

        QueryResult countResult = CharacterDatabase.Query(
            "SELECT COUNT(*) FROM levelsync_members WHERE group_id = {}", groupId);
        uint64 totalChars = countResult ? countResult->Fetch()[0].Get<uint64>() : 0;

        handler->PSendSysMessage("[LevelSync] Sync Group #{}", groupId);
        handler->PSendSysMessage("  Accounts: {}/{}", accounts, sLevelSync->GetMaxLinkedAccounts());
        handler->PSendSysMessage("  Total Characters: {}", totalChars);
        handler->PSendSysMessage("  Level sync: {}", levelSync ? "ON" : "OFF");
        handler->PSendSysMessage("  Progression sync: {}", progSync ? "ON" : "OFF");
        handler->PSendSysMessage("[LevelSync] Group members:");
        DisplayGroupMembers(handler, groupId);

        return true;
    }

    // -------------------------------------------------------------------
    // .levelsync level on|off
    // -------------------------------------------------------------------
    static bool HandleLevelCommand(ChatHandler* handler, char const* args)
    {
        if (!sLevelSync->IsEnabled())
        {
            handler->PSendSysMessage("[LevelSync] Module is disabled.");
            return true;
        }

        if (!sLevelSync->IsLevelSyncAllowed())
        {
            handler->PSendSysMessage("[LevelSync] Level sync is disabled by the server.");
            return true;
        }

        char* arg = strtok(const_cast<char*>(args), " ");
        if (!arg)
        {
            handler->PSendSysMessage("Usage: .levelsync level on|off");
            return true;
        }

        bool enable = (std::string(arg) == "on");

        Player* player = handler->GetSession()->GetPlayer();
        uint32  myGuid = player->GetGUID().GetCounter();

        uint32 groupId = sLevelSync->GetGroupId(myGuid);
        if (!groupId)
        {
            handler->PSendSysMessage("[LevelSync] You are not in a sync group.");
            return true;
        }

        CharacterDatabase.Execute(
            "UPDATE levelsync_groups SET level_sync_enabled = {} WHERE group_id = {}",
            enable ? 1 : 0, groupId);

        handler->PSendSysMessage("[LevelSync] Level sync {}.", enable ? "enabled" : "disabled");

        if (enable)
        {
            handler->PSendSysMessage("[LevelSync] Syncing group to highest level...");
            sLevelSync->SyncGroupOnLevelToggle(groupId);
        }

        return true;
    }

    // -------------------------------------------------------------------
    // .levelsync IP on|off
    // -------------------------------------------------------------------
    static bool HandleIndivProgressionCommand(ChatHandler* handler, char const* args)
    {
        if (!sLevelSync->IsEnabled())
        {
            handler->PSendSysMessage("[LevelSync] Module is disabled.");
            return true;
        }

        if (!sLevelSync->IsProgressionAllowed())
        {
            handler->PSendSysMessage("[LevelSync] Progression sync is disabled by the server.");
            return true;
        }

        char* arg = strtok(const_cast<char*>(args), " ");
        if (!arg)
        {
            handler->PSendSysMessage("Usage: .levelsync IP on|off");
            return true;
        }

        bool enable = (std::string(arg) == "on");

        Player* player = handler->GetSession()->GetPlayer();
        uint32  myGuid = player->GetGUID().GetCounter();

        uint32 groupId = sLevelSync->GetGroupId(myGuid);
        if (!groupId)
        {
            handler->PSendSysMessage("[LevelSync] You are not in a sync group.");
            return true;
        }

        CharacterDatabase.Execute(
            "UPDATE levelsync_groups SET sync_progression = {} WHERE group_id = {}",
            enable ? 1 : 0, groupId);

        handler->PSendSysMessage("[LevelSync] Progression sync {}.", enable ? "enabled" : "disabled");

        if (enable)
        {
            handler->PSendSysMessage("[LevelSync] Syncing group progression...");
            sLevelSync->SyncIPOnToggle(groupId);
        }

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
            handler->PSendSysMessage("[LevelSync] Character '{}' not found.", target);
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
            handler->PSendSysMessage("[LevelSync] '{}' is not in a sync group. Account key removed.", target);
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

        handler->PSendSysMessage("[LevelSync] Group #{} fully disbanded. All keys removed.", groupId);
        return true;
    }
};

void AddSC_LevelSyncCommands()
{
    new LevelSyncCommandScript();
}
