#include "LevelSync.h"
#include "Config.h"
#include "ObjectAccessor.h"
#include "CharacterCache.h"
#include "Chat.h"
#include "ObjectGuid.h"
#include "ObjectMgr.h"
#include "QuestDef.h"
#include "ScriptMgr.h"
#include "UpdateFields.h"
#include <algorithm>
#include <string>

LevelSyncMgr* LevelSyncMgr::instance()
{
    static LevelSyncMgr inst;
    return &inst;
}

void LevelSyncMgr::LoadConfig()
{
    _enabled              = sConfigMgr->GetOption<bool>("LevelSync.Enable",               true);
    _allowLevelSync       = sConfigMgr->GetOption<bool>("LevelSync.AllowLevelSync",       true);
    _allowProgressionSync = sConfigMgr->GetOption<bool>("LevelSync.AllowProgressionSync", false);
    _dkException          = sConfigMgr->GetOption<bool>("LevelSync.DeathKnightException",   false);
    _dkIPException        = sConfigMgr->GetOption<bool>("LevelSync.DeathKnightIPException", false);
    _maxLinkedAccounts    = sConfigMgr->GetOption<uint32>("LevelSync.MaxLinkedAccounts",  6);

    if (_maxLinkedAccounts < 1)  _maxLinkedAccounts = 1;
    if (_maxLinkedAccounts > 10) _maxLinkedAccounts = 10;
}

// -----------------------------------------------------------------------
// Group queries
// -----------------------------------------------------------------------

uint32 LevelSyncMgr::GetGroupId(uint32 charGuid) const
{
    QueryResult r = CharacterDatabase.Query(
        "SELECT group_id FROM levelsync_members WHERE char_guid = {}", charGuid);
    if (!r)
        return 0;
    return r->Fetch()[0].Get<uint32>();
}

uint32 LevelSyncMgr::GetGroupAccountCount(uint32 groupId) const
{
    QueryResult r = CharacterDatabase.Query(
        "SELECT COUNT(DISTINCT account_id) FROM levelsync_members WHERE group_id = {}",
        groupId);
    if (!r)
        return 0;
    return static_cast<uint32>(r->Fetch()[0].Get<uint64>());
}

bool LevelSyncMgr::IsGroupLevelSyncEnabled(uint32 groupId) const
{
    QueryResult r = CharacterDatabase.Query(
        "SELECT level_sync_enabled FROM levelsync_groups WHERE group_id = {}", groupId);
    if (!r)
        return false;
    return r->Fetch()[0].Get<uint8>() == 1;
}

bool LevelSyncMgr::IsGroupProgressionSyncEnabled(uint32 groupId) const
{
    QueryResult r = CharacterDatabase.Query(
        "SELECT sync_progression FROM levelsync_groups WHERE group_id = {}", groupId);
    if (!r)
        return false;
    return r->Fetch()[0].Get<uint8>() == 1;
}

std::vector<uint32> LevelSyncMgr::GetGroupMemberGuids(uint32 groupId, uint32 excludeGuid) const
{
    std::vector<uint32> guids;
    QueryResult r = CharacterDatabase.Query(
        "SELECT char_guid FROM levelsync_members WHERE group_id = {}", groupId);
    if (!r)
        return guids;
    do
    {
        uint32 g = r->Fetch()[0].Get<uint32>();
        if (g != excludeGuid)
            guids.push_back(g);
    } while (r->NextRow());
    return guids;
}

// -----------------------------------------------------------------------
// Level sync helpers
// -----------------------------------------------------------------------

bool LevelSyncMgr::IsDKPushBlocked(uint8 sourceClass, uint8 targetLevel) const
{
    return !_dkException && sourceClass == LEVELSYNC_CLASS_DEATH_KNIGHT && targetLevel < LEVELSYNC_DK_MIN_LEVEL;
}

uint8 LevelSyncMgr::GetEffectiveHighestLevel(uint32 groupId, uint8 targetCurrentLevel) const
{
    QueryResult r = CharacterDatabase.Query(
        "SELECT c.level, c.`class` "
        "FROM levelsync_members m "
        "JOIN characters c ON m.char_guid = c.guid "
        "WHERE m.group_id = {}",
        groupId);

    if (!r)
        return targetCurrentLevel;

    uint8 highest = targetCurrentLevel;
    do
    {
        uint8 memberLevel = r->Fetch()[0].Get<uint8>();
        uint8 memberClass = r->Fetch()[1].Get<uint8>();

        if (!_dkException && memberClass == LEVELSYNC_CLASS_DEATH_KNIGHT && targetCurrentLevel < LEVELSYNC_DK_MIN_LEVEL)
            continue;

        if (memberLevel > highest)
            highest = memberLevel;
    } while (r->NextRow());

    return highest;
}

void LevelSyncMgr::ApplyLevelToOnline(Player* target, uint8 newLevel)
{
    if (target->GetLevel() >= newLevel)
        return;

    _syncing = true;
    target->GiveLevel(newLevel);
    _syncing = false;

    ChatHandler(target->GetSession()).PSendSysMessage(
        "|cff00ff00[LevelSync]|r Your level has been updated to {} by the group sync.",
        newLevel);
}

void LevelSyncMgr::BatchUpdateOfflineLevel(const std::vector<uint32>& guids, uint8 newLevel, uint8 minCurrentLevel)
{
    if (guids.empty())
        return;

    std::string inList;
    for (size_t i = 0; i < guids.size(); ++i)
    {
        if (i > 0) inList += ',';
        inList += std::to_string(guids[i]);
    }

    CharacterDatabase.Execute(
        "UPDATE characters SET level = {}, xp = 0 WHERE guid IN ({}) AND level < {} AND level >= {}",
        newLevel, inList, newLevel, minCurrentLevel);

    for (uint32 g : guids)
        sCharacterCache->UpdateCharacterLevel(
            ObjectGuid::Create<HighGuid::Player>(g), newLevel);
}

void LevelSyncMgr::BatchUpdateOfflineXP(const std::vector<uint32>& guids, uint8 level, uint32 xp)
{
    if (guids.empty())
        return;

    std::string inList;
    for (size_t i = 0; i < guids.size(); ++i)
    {
        if (i > 0) inList += ',';
        inList += std::to_string(guids[i]);
    }

    CharacterDatabase.Execute(
        "UPDATE characters SET xp = {} WHERE guid IN ({}) AND level = {} AND xp < {}",
        xp, inList, level, xp);
}

// -----------------------------------------------------------------------
// Level sync entry points
// -----------------------------------------------------------------------

void LevelSyncMgr::SyncGroupOnLogin(Player* player)
{
    if (!_enabled || !_allowLevelSync)
        return;

    uint32 groupId = GetGroupId(player->GetGUID().GetCounter());
    if (!groupId || !IsGroupLevelSyncEnabled(groupId))
        return;

    uint8 highest = GetEffectiveHighestLevel(groupId, player->GetLevel());

    if (highest > player->GetLevel())
        ApplyLevelToOnline(player, highest);

    uint8 myLevel = player->GetLevel();
    uint8 myClass = player->getClass();

    for (uint32 g : GetGroupMemberGuids(groupId, player->GetGUID().GetCounter()))
    {
        if (Player* member = ObjectAccessor::FindPlayerByLowGUID(g))
        {
            if (member->GetLevel() < myLevel)
            {
                if (IsDKPushBlocked(myClass, member->GetLevel()))
                    continue;
                ApplyLevelToOnline(member, myLevel);
            }
        }
    }
}

void LevelSyncMgr::SyncGroupOnLogout(Player* player)
{
    if (!_enabled)
        return;

    uint32 groupId = GetGroupId(player->GetGUID().GetCounter());
    if (!groupId)
        return;

    uint8  myLevel = player->GetLevel();
    uint32 myXP    = player->GetUInt32Value(PLAYER_XP);
    uint8  myClass = player->getClass();
    uint8  minLevel = IsDKPushBlocked(myClass, 0) ? LEVELSYNC_DK_MIN_LEVEL : 0;

    if (_allowLevelSync && IsGroupLevelSyncEnabled(groupId))
    {
        std::vector<uint32> offlineGuids;

        for (uint32 g : GetGroupMemberGuids(groupId, player->GetGUID().GetCounter()))
        {
            if (Player* member = ObjectAccessor::FindPlayerByLowGUID(g))
            {
                if (!IsDKPushBlocked(myClass, member->GetLevel()) && member->GetLevel() < myLevel)
                    ApplyLevelToOnline(member, myLevel);

                if (member->GetLevel() == myLevel && member->GetUInt32Value(PLAYER_XP) < myXP)
                    member->SetUInt32Value(PLAYER_XP, myXP);
            }
            else
                offlineGuids.push_back(g);
        }

        BatchUpdateOfflineLevel(offlineGuids, myLevel, minLevel);
        BatchUpdateOfflineXP(offlineGuids, myLevel, myXP);
    }
}

void LevelSyncMgr::SyncGroupOnLevelUp(Player* player)
{
    if (!_enabled || !_allowLevelSync || _syncing)
        return;

    uint32 groupId = GetGroupId(player->GetGUID().GetCounter());
    if (!groupId || !IsGroupLevelSyncEnabled(groupId))
        return;

    uint8 newLevel = player->GetLevel();
    uint8 myClass  = player->getClass();
    uint8 minLevel = IsDKPushBlocked(myClass, 0) ? LEVELSYNC_DK_MIN_LEVEL : 0;
    std::vector<uint32> offlineGuids;

    for (uint32 g : GetGroupMemberGuids(groupId, player->GetGUID().GetCounter()))
    {
        if (Player* member = ObjectAccessor::FindPlayerByLowGUID(g))
        {
            if (IsDKPushBlocked(myClass, member->GetLevel()))
                continue;
            ApplyLevelToOnline(member, newLevel);
        }
        else
            offlineGuids.push_back(g);
    }

    BatchUpdateOfflineLevel(offlineGuids, newLevel, minLevel);
}

void LevelSyncMgr::SyncGroupOnLevelToggle(uint32 groupId)
{
    if (!_enabled || !_allowLevelSync)
        return;

    QueryResult r = CharacterDatabase.Query(
        "SELECT c.level, c.`class`, m.char_guid "
        "FROM levelsync_members m "
        "JOIN characters c ON m.char_guid = c.guid "
        "WHERE m.group_id = {} "
        "ORDER BY c.level DESC",
        groupId);

    if (!r)
        return;

    struct MemberInfo { uint32 guid; uint8 level; uint8 cls; };
    std::vector<MemberInfo> members;
    do
    {
        MemberInfo mi;
        mi.level = r->Fetch()[0].Get<uint8>();
        mi.cls   = r->Fetch()[1].Get<uint8>();
        mi.guid  = r->Fetch()[2].Get<uint32>();
        members.push_back(mi);
    } while (r->NextRow());

    std::vector<uint32> offlineGuids;

    for (auto& mi : members)
    {
        uint8 target = GetEffectiveHighestLevel(groupId, mi.level);
        if (target <= mi.level)
            continue;

        if (Player* p = ObjectAccessor::FindPlayerByLowGUID(mi.guid))
            ApplyLevelToOnline(p, target);
        else
            offlineGuids.push_back(mi.guid);
    }

    uint8 highest = 0;
    for (auto& mi : members)
    {
        if (!_dkException && mi.cls == LEVELSYNC_CLASS_DEATH_KNIGHT)
            continue;
        if (mi.level > highest)
            highest = mi.level;
    }

    if (highest > 0)
        BatchUpdateOfflineLevel(offlineGuids, highest);
}

// -----------------------------------------------------------------------
// IP sync helpers
// -----------------------------------------------------------------------

bool LevelSyncMgr::IsDKIPPushBlocked(uint8 sourceClass, uint8 targetTier) const
{
    return !_dkIPException && sourceClass == LEVELSYNC_CLASS_DEATH_KNIGHT && targetTier < LEVELSYNC_DK_IP_MIN_TIER;
}

uint8 LevelSyncMgr::GetCharacterIPTier(uint32 guid) const
{
    QueryResult r = CharacterDatabase.Query(
        "SELECT MAX(quest) FROM character_queststatus_rewarded "
        "WHERE guid = {} AND quest BETWEEN {} AND {} AND active = 1",
        guid,
        LEVELSYNC_IP_QUEST_BASE + 1,
        LEVELSYNC_IP_QUEST_BASE + LEVELSYNC_IP_MAX_TIER);

    if (!r || r->Fetch()[0].IsNull())
        return 0;
    return uint8(r->Fetch()[0].Get<uint32>() - LEVELSYNC_IP_QUEST_BASE);
}

uint8 LevelSyncMgr::GetEffectiveHighestIPTier(uint32 groupId, uint8 targetCurrentTier) const
{
    QueryResult r = CharacterDatabase.Query(
        "SELECT m.char_guid, c.`class` "
        "FROM levelsync_members m "
        "JOIN characters c ON m.char_guid = c.guid "
        "WHERE m.group_id = {}",
        groupId);

    if (!r)
        return targetCurrentTier;

    uint8 highest = targetCurrentTier;
    do
    {
        uint32 guid       = r->Fetch()[0].Get<uint32>();
        uint8  memberClass = r->Fetch()[1].Get<uint8>();
        uint8  memberTier  = GetCharacterIPTier(guid);

        if (IsDKIPPushBlocked(memberClass, targetCurrentTier))
            continue;

        if (memberTier > highest)
            highest = memberTier;
    } while (r->NextRow());

    return highest;
}

void LevelSyncMgr::ApplyIPTierToOnline(Player* target, uint8 newTier)
{
    uint8 currentTier = 0;
    for (uint8 i = 1; i <= LEVELSYNC_IP_MAX_TIER; ++i)
    {
        if (target->GetQuestStatus(LEVELSYNC_IP_QUEST_BASE + i) == QUEST_STATUS_REWARDED)
            currentTier = i;
    }

    if (currentTier >= newTier)
        return;

    _syncingIP = true;

    for (uint8 i = 1; i <= LEVELSYNC_IP_MAX_TIER; ++i)
    {
        uint32 qId = LEVELSYNC_IP_QUEST_BASE + i;
        if (target->GetQuestStatus(qId) == QUEST_STATUS_REWARDED)
            target->RemoveRewardedQuest(qId);
    }

    uint32 questId = LEVELSYNC_IP_QUEST_BASE + newTier;
    if (Quest const* quest = sObjectMgr->GetQuestTemplate(questId))
    {
        target->AddQuest(quest, nullptr);
        target->CompleteQuest(questId);
        target->RewardQuest(quest, 0, target, false, false);
    }

    _syncingIP = false;

    ChatHandler(target->GetSession()).PSendSysMessage(
        "|cff00ff00[LevelSync]|r Your progression tier has been updated to {} by the group sync.",
        newTier);
}

void LevelSyncMgr::BatchUpdateOfflineIPTier(const std::vector<uint32>& guids, uint8 newTier, uint8 minTier)
{
    if (guids.empty())
        return;

    for (uint32 g : guids)
    {
        uint8 currentTier = GetCharacterIPTier(g);
        if (currentTier >= newTier || currentTier < minTier)
            continue;

        CharacterDatabase.Execute(
            "DELETE FROM character_queststatus_rewarded WHERE guid = {} AND quest BETWEEN {} AND {}",
            g, LEVELSYNC_IP_QUEST_BASE + 1, LEVELSYNC_IP_QUEST_BASE + LEVELSYNC_IP_MAX_TIER);

        CharacterDatabase.Execute(
            "INSERT IGNORE INTO character_queststatus_rewarded (guid, quest, active) VALUES ({}, {}, 1)",
            g, uint32(LEVELSYNC_IP_QUEST_BASE + newTier));
    }
}

// -----------------------------------------------------------------------
// IP sync entry points
// -----------------------------------------------------------------------

void LevelSyncMgr::SyncIPOnLogin(Player* player)
{
    if (!_enabled || !_allowProgressionSync)
        return;

    uint32 groupId = GetGroupId(player->GetGUID().GetCounter());
    if (!groupId || !IsGroupProgressionSyncEnabled(groupId))
        return;

    uint8 myTier  = GetCharacterIPTier(player->GetGUID().GetCounter());
    uint8 highest = GetEffectiveHighestIPTier(groupId, myTier);

    if (highest > myTier)
        ApplyIPTierToOnline(player, highest);

    uint8 effectiveTier = (highest > myTier) ? highest : myTier;
    uint8 myClass       = player->getClass();

    for (uint32 g : GetGroupMemberGuids(groupId, player->GetGUID().GetCounter()))
    {
        if (Player* member = ObjectAccessor::FindPlayerByLowGUID(g))
        {
            uint8 memberTier = GetCharacterIPTier(member->GetGUID().GetCounter());
            if (memberTier < effectiveTier)
            {
                if (IsDKIPPushBlocked(myClass, memberTier))
                    continue;
                ApplyIPTierToOnline(member, effectiveTier);
            }
        }
    }
}

void LevelSyncMgr::SyncIPOnLogout(Player* player)
{
    if (!_enabled || !_allowProgressionSync)
        return;

    uint32 groupId = GetGroupId(player->GetGUID().GetCounter());
    if (!groupId || !IsGroupProgressionSyncEnabled(groupId))
        return;

    uint8 myTier   = GetCharacterIPTier(player->GetGUID().GetCounter());
    uint8 myClass  = player->getClass();
    uint8 minTier  = IsDKIPPushBlocked(myClass, 0) ? LEVELSYNC_DK_IP_MIN_TIER : 0;

    std::vector<uint32> offlineGuids;

    for (uint32 g : GetGroupMemberGuids(groupId, player->GetGUID().GetCounter()))
    {
        if (Player* member = ObjectAccessor::FindPlayerByLowGUID(g))
        {
            uint8 memberTier = GetCharacterIPTier(member->GetGUID().GetCounter());
            if (memberTier < myTier)
            {
                if (IsDKIPPushBlocked(myClass, memberTier))
                    continue;
                ApplyIPTierToOnline(member, myTier);
            }
        }
        else
            offlineGuids.push_back(g);
    }

    BatchUpdateOfflineIPTier(offlineGuids, myTier, minTier);
}

void LevelSyncMgr::SyncIPOnTierUp(Player* player, uint8 newTier)
{
    if (!_enabled || !_allowProgressionSync || _syncingIP)
        return;

    uint32 groupId = GetGroupId(player->GetGUID().GetCounter());
    if (!groupId || !IsGroupProgressionSyncEnabled(groupId))
        return;

    uint8 myClass  = player->getClass();
    uint8 minTier  = IsDKIPPushBlocked(myClass, 0) ? LEVELSYNC_DK_IP_MIN_TIER : 0;
    std::vector<uint32> offlineGuids;

    for (uint32 g : GetGroupMemberGuids(groupId, player->GetGUID().GetCounter()))
    {
        if (Player* member = ObjectAccessor::FindPlayerByLowGUID(g))
        {
            uint8 memberTier = GetCharacterIPTier(member->GetGUID().GetCounter());
            if (IsDKIPPushBlocked(myClass, memberTier))
                continue;
            if (memberTier < newTier)
                ApplyIPTierToOnline(member, newTier);
        }
        else
            offlineGuids.push_back(g);
    }

    BatchUpdateOfflineIPTier(offlineGuids, newTier, minTier);
}

void LevelSyncMgr::SyncIPOnToggle(uint32 groupId)
{
    if (!_enabled || !_allowProgressionSync)
        return;

    QueryResult r = CharacterDatabase.Query(
        "SELECT m.char_guid, c.`class` "
        "FROM levelsync_members m "
        "JOIN characters c ON m.char_guid = c.guid "
        "WHERE m.group_id = {}",
        groupId);

    if (!r)
        return;

    struct MemberInfo { uint32 guid; uint8 cls; };
    std::vector<MemberInfo> members;
    do
    {
        MemberInfo mi;
        mi.guid = r->Fetch()[0].Get<uint32>();
        mi.cls  = r->Fetch()[1].Get<uint8>();
        members.push_back(mi);
    } while (r->NextRow());

    std::vector<uint32> offlineGuids;

    for (auto& mi : members)
    {
        uint8 currentTier = GetCharacterIPTier(mi.guid);
        uint8 target      = GetEffectiveHighestIPTier(groupId, currentTier);
        if (target <= currentTier)
            continue;

        if (Player* p = ObjectAccessor::FindPlayerByLowGUID(mi.guid))
            ApplyIPTierToOnline(p, target);
        else
            offlineGuids.push_back(mi.guid);
    }

    uint8 highest = 0;
    for (auto& mi : members)
    {
        if (!_dkIPException && mi.cls == LEVELSYNC_CLASS_DEATH_KNIGHT)
            continue;
        uint8 tier = GetCharacterIPTier(mi.guid);
        if (tier > highest)
            highest = tier;
    }

    if (highest > 0)
        BatchUpdateOfflineIPTier(offlineGuids, highest);
}

// -----------------------------------------------------------------------
// Player script
// -----------------------------------------------------------------------

class LevelSyncPlayerScript : public PlayerScript
{
public:
    LevelSyncPlayerScript() : PlayerScript("LevelSyncPlayerScript") {}

    void OnPlayerLogin(Player* player) override
    {
        sLevelSync->SyncGroupOnLogin(player);
        sLevelSync->SyncIPOnLogin(player);
    }

    void OnPlayerLevelChanged(Player* player, uint8 /*oldLevel*/) override
    {
        sLevelSync->SyncGroupOnLevelUp(player);
    }

    void OnPlayerLogout(Player* player) override
    {
        sLevelSync->SyncGroupOnLogout(player);
        sLevelSync->SyncIPOnLogout(player);
    }

    void OnPlayerCompleteQuest(Player* player, Quest const* quest) override
    {
        uint32 questId = quest->GetQuestId();
        if (questId <= LEVELSYNC_IP_QUEST_BASE || questId > LEVELSYNC_IP_QUEST_BASE + LEVELSYNC_IP_MAX_TIER)
            return;
        uint8 newTier = uint8(questId - LEVELSYNC_IP_QUEST_BASE);
        sLevelSync->SyncIPOnTierUp(player, newTier);
    }
};

// -----------------------------------------------------------------------
// World script
// -----------------------------------------------------------------------

class LevelSyncWorldScript : public WorldScript
{
public:
    LevelSyncWorldScript() : WorldScript("LevelSyncWorldScript") {}

    void OnBeforeConfigLoad(bool /*reload*/) override
    {
        sLevelSync->LoadConfig();
    }

    void OnStartup() override
    {
        // Remove members whose character no longer exists
        CharacterDatabase.Execute(
            "DELETE m FROM levelsync_members m "
            "LEFT JOIN characters c ON m.char_guid = c.guid "
            "WHERE c.guid IS NULL");

        // Remove groups that have no remaining members
        CharacterDatabase.Execute(
            "DELETE g FROM levelsync_groups g "
            "LEFT JOIN levelsync_members m ON g.group_id = m.group_id "
            "WHERE m.group_id IS NULL");

    }
};

void AddSC_LevelSync()
{
    new LevelSyncPlayerScript();
    new LevelSyncWorldScript();
}
