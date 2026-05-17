#include "LevelSync.h"
#include "AchievementMgr.h"
#include "Config.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "CharacterCache.h"
#include "Chat.h"
#include "ObjectGuid.h"
#include "ObjectMgr.h"
#include "QuestDef.h"
#include "ScriptMgr.h"
#include "UpdateFields.h"
#include <string>
#include <unordered_map>

static std::string FormatMoneyString(uint32 copper)
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
    _allowMoneyCommands   = sConfigMgr->GetOption<bool>("LevelSync.AllowMoneyCommands",  true);
    _allowRaidUnbind      = sConfigMgr->GetOption<bool>("LevelSync.AllowRaidUnbind",     false);
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
// Toggle rate-limit
// -----------------------------------------------------------------------

bool LevelSyncMgr::TryConsumeToggleCooldown(uint32 groupId, uint32& secondsRemaining)
{
    constexpr time_t COOLDOWN_SECONDS = 10;

    time_t now = std::time(nullptr);
    auto it = _lastToggle.find(groupId);
    if (it != _lastToggle.end())
    {
        time_t elapsed = now - it->second;
        if (elapsed >= 0 && elapsed < COOLDOWN_SECONDS)
        {
            secondsRemaining = static_cast<uint32>(COOLDOWN_SECONDS - elapsed);
            return false;
        }
    }

    _lastToggle[groupId] = now;
    secondsRemaining = 0;
    return true;
}

// -----------------------------------------------------------------------
// Login-path helper
// -----------------------------------------------------------------------

bool LevelSyncMgr::LoadGroupLoginInfo(uint32 charGuid, GroupLoginInfo& out) const
{
    out = GroupLoginInfo{};

    QueryResult r = CharacterDatabase.Query(
        "SELECT g.group_id, g.level_sync_enabled, g.sync_progression "
        "FROM levelsync_members m "
        "JOIN levelsync_groups g ON m.group_id = g.group_id "
        "WHERE m.char_guid = {}",
        charGuid);

    if (!r)
        return false;

    Field* f = r->Fetch();
    out.groupId                = f[0].Get<uint32>();
    out.levelSyncEnabled       = f[1].Get<uint8>() == 1;
    out.progressionSyncEnabled = f[2].Get<uint8>() == 1;
    return true;
}

// -----------------------------------------------------------------------
// Level sync helpers
// -----------------------------------------------------------------------

std::vector<LevelSyncMgr::GroupLevelMember> LevelSyncMgr::LoadGroupLevelMembers(uint32 groupId) const
{
    std::vector<GroupLevelMember> members;

    QueryResult r = CharacterDatabase.Query(
        "SELECT m.char_guid, c.level, c.`class`, c.xp "
        "FROM levelsync_members m "
        "JOIN characters c ON m.char_guid = c.guid "
        "WHERE m.group_id = {}",
        groupId);

    if (!r)
        return members;

    do
    {
        GroupLevelMember gm;
        gm.guid  = r->Fetch()[0].Get<uint32>();
        gm.level = r->Fetch()[1].Get<uint8>();
        gm.cls   = r->Fetch()[2].Get<uint8>();
        gm.xp    = r->Fetch()[3].Get<uint32>();

        // Overlay live in-memory state for online characters. The DB row
        // only persists on the next save tick or logout, so reading c.level
        // / c.xp directly can lag behind reality (e.g. a GM bumps a bot to
        // level 10 via .character level — DB still shows the old level
        // until next save). Sync paths that consume this snapshot need to
        // see the truth, otherwise they compute "highest" against stale
        // data and skip propagation.
        if (Player* online = ObjectAccessor::FindPlayerByLowGUID(gm.guid))
        {
            gm.level = online->GetLevel();
            gm.xp    = online->GetUInt32Value(PLAYER_XP);
        }

        members.push_back(gm);
    } while (r->NextRow());

    return members;
}

bool LevelSyncMgr::IsDKPushBlocked(uint8 sourceClass, uint8 targetLevel) const
{
    return !_dkException && sourceClass == LEVELSYNC_CLASS_DEATH_KNIGHT && targetLevel < LEVELSYNC_DK_MIN_LEVEL;
}

void LevelSyncMgr::ApplyLevelToOnline(Player* target, uint8 newLevel)
{
    if (target->GetLevel() >= newLevel)
        return;

    _syncing = true;
    target->GiveLevel(newLevel);
    // GiveLevel does not zero PLAYER_XP. Without this, the receiver retains
    // whatever XP they had toward their previous level, which collapses to
    // an extra ding on the very next quest turn-in.
    target->SetUInt32Value(PLAYER_XP, 0);
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

    // Pre-filter: ask the DB which guids will actually qualify so the
    // post-update cache and achievement-queue writes only fire for chars
    // whose level genuinely changed. Without this, callers that pass a
    // mixed batch (e.g. an offline DK at 55 in a level-10 push) would see
    // their cache row drift from the DB row for non-qualifying guids and
    // accumulate pointless achievement-queue rows that are discarded at
    // login. Costs one extra SELECT per call; group sizes are small.
    QueryResult r = CharacterDatabase.Query(
        "SELECT guid FROM characters WHERE guid IN ({}) AND level < {} AND level >= {}",
        inList, newLevel, minCurrentLevel);
    if (!r)
        return;

    std::vector<uint32> actual;
    do { actual.push_back(r->Fetch()[0].Get<uint32>()); }
    while (r->NextRow());

    std::string actualInList;
    for (size_t i = 0; i < actual.size(); ++i)
    {
        if (i > 0) actualInList += ',';
        actualInList += std::to_string(actual[i]);
    }

    CharacterDatabase.Execute(
        "UPDATE characters SET level = {}, xp = 0 WHERE guid IN ({})",
        newLevel, actualInList);

    for (uint32 g : actual)
    {
        sCharacterCache->UpdateCharacterLevel(
            ObjectGuid::Create<HighGuid::Player>(g), newLevel);
        // Mirror what cs_character.cpp's offline branch does so a char who
        // gets pushed past a reach-level threshold while offline is credited
        // for the achievement on next login.
        sAchievementMgr->UpdateAchievementCriteriaForOfflinePlayer(
            g, ACHIEVEMENT_CRITERIA_TYPE_REACH_LEVEL);
    }
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

    uint32 myGuid = player->GetGUID().GetCounter();

    GroupLoginInfo info;
    if (!LoadGroupLoginInfo(myGuid, info))
        return;
    if (!info.groupId || !info.levelSyncEnabled)
        return;

    auto members = LoadGroupLevelMembers(info.groupId);
    if (members.empty())
        return;

    uint8 myLevel = player->GetLevel();

    // Skip DK members when the target (logging-in player) is below the DK
    // floor and the exception config is disabled.
    uint8 highest = myLevel;
    for (auto const& m : members)
    {
        if (!_dkException && m.cls == LEVELSYNC_CLASS_DEATH_KNIGHT && myLevel < LEVELSYNC_DK_MIN_LEVEL)
            continue;
        if (m.level > highest)
            highest = m.level;
    }

    if (highest > myLevel)
    {
        ApplyLevelToOnline(player, highest);
        myLevel = highest;
    }

    // After any pull-up, find the highest XP at the player's current level
    // and bump the player up to match. Online members' XP is read live;
    // offline members' XP is read from the snapshot. Same-level only —
    // cross-level comparison is moot because the player's at the highest
    // level after the pull-up step.
    uint32 myXP = player->GetUInt32Value(PLAYER_XP);
    uint32 highestXP = myXP;
    for (auto const& m : members)
    {
        if (m.guid == myGuid)
            continue;
        if (m.level != myLevel)
            continue;
        uint32 memberXP = m.xp;
        if (Player* online = ObjectAccessor::FindPlayerByLowGUID(m.guid))
            memberXP = online->GetUInt32Value(PLAYER_XP);
        if (memberXP > highestXP)
            highestXP = memberXP;
    }

    if (highestXP > myXP)
    {
        player->SetUInt32Value(PLAYER_XP, highestXP);
        myXP = highestXP;
    }

    uint8 myClass = player->getClass();
    uint8 minLevel = IsDKPushBlocked(myClass, 0) ? LEVELSYNC_DK_MIN_LEVEL : 0;
    std::vector<uint32> offlineGuids;

    for (auto const& m : members)
    {
        if (m.guid == myGuid)
            continue;
        if (IsDKPushBlocked(myClass, m.level))
            continue;

        if (Player* member = ObjectAccessor::FindPlayerByLowGUID(m.guid))
        {
            if (m.level < myLevel)
            {
                ApplyLevelToOnline(member, myLevel);
                // ApplyLevelToOnline zeros the receiver's XP. Bump it up to
                // myXP so online pulled-up members match the offline branch
                // (BatchUpdateOfflineLevel + BatchUpdateOfflineXP would put
                // an offline pulled-up char at myLevel/myXP). Without this
                // bump, online and offline members would drift apart after
                // login sync.
                if (myXP > 0)
                    member->SetUInt32Value(PLAYER_XP, myXP);
            }
            else if (m.level == myLevel && member->GetUInt32Value(PLAYER_XP) < myXP)
            {
                member->SetUInt32Value(PLAYER_XP, myXP);
            }
        }
        else
        {
            offlineGuids.push_back(m.guid);
        }
    }

    BatchUpdateOfflineLevel(offlineGuids, myLevel, minLevel);
    BatchUpdateOfflineXP(offlineGuids, myLevel, myXP);
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

void LevelSyncMgr::SyncGroupOnLevelToggle(uint32 groupId)
{
    if (!_enabled || !_allowLevelSync)
        return;

    auto members = LoadGroupLevelMembers(groupId);
    if (members.empty())
        return;

    // Precompute the two ceilings the DK rule can produce, in one pass over
    // the in-memory member list. highestAll wins when the target sits at or
    // above the DK floor (or the exception config is on); highestNonDK wins
    // when a sub-floor target would otherwise be pulled up by a DK.
    uint8 highestAll   = 0;
    uint8 highestNonDK = 0;
    for (auto const& m : members)
    {
        if (m.level > highestAll)
            highestAll = m.level;
        if (m.cls != LEVELSYNC_CLASS_DEATH_KNIGHT && m.level > highestNonDK)
            highestNonDK = m.level;
    }

    std::vector<uint32> offlineGuids;

    for (auto const& mi : members)
    {
        uint8 target = (_dkException || mi.level >= LEVELSYNC_DK_MIN_LEVEL)
                       ? highestAll
                       : highestNonDK;
        if (target <= mi.level)
            continue;

        if (Player* p = ObjectAccessor::FindPlayerByLowGUID(mi.guid))
            ApplyLevelToOnline(p, target);
        else
            offlineGuids.push_back(mi.guid);
    }

    // BatchUpdateOfflineLevel takes a single ceiling for the whole offline
    // set; mirror the original behavior of "max of non-DK levels unless the
    // exception is on, then max of all levels".
    uint8 offlineHighest = _dkException ? highestAll : highestNonDK;
    if (offlineHighest > 0)
        BatchUpdateOfflineLevel(offlineGuids, offlineHighest);

    // Sync XP at each effective ceiling. With DK exception OFF, non-DKs cap
    // at highestNonDK while DKs cap at highestAll — the two ceilings can
    // differ (e.g. non-DKs at level 2 alongside level-55 DKs). A single XP
    // search at highestAll would miss the non-DK tier entirely. So run the
    // XP push at each ceiling level present in the group, deduplicating if
    // they're equal.
    auto syncXPAtLevel = [&](uint8 ceiling) {
        if (ceiling == 0)
            return;
        uint32 topXP = 0;
        for (auto const& m : members)
        {
            if (m.level != ceiling)
                continue;
            uint32 memberXP = m.xp;
            if (Player* online = ObjectAccessor::FindPlayerByLowGUID(m.guid))
                memberXP = online->GetUInt32Value(PLAYER_XP);
            if (memberXP > topXP)
                topXP = memberXP;
        }
        if (topXP == 0)
            return;

        std::vector<uint32> xpOfflineGuids;
        for (auto const& m : members)
        {
            if (Player* online = ObjectAccessor::FindPlayerByLowGUID(m.guid))
            {
                if (online->GetLevel() == ceiling &&
                    online->GetUInt32Value(PLAYER_XP) < topXP)
                    online->SetUInt32Value(PLAYER_XP, topXP);
            }
            else
            {
                xpOfflineGuids.push_back(m.guid);
            }
        }
        // BatchUpdateOfflineXP filters server-side by level == ceiling AND
        // xp < topXP, so passing all offline guids is safe — only the ones
        // actually at this ceiling with less XP will be touched.
        BatchUpdateOfflineXP(xpOfflineGuids, ceiling, topXP);
    };

    syncXPAtLevel(highestNonDK);
    if (highestAll != highestNonDK)
        syncXPAtLevel(highestAll);
}

// -----------------------------------------------------------------------
// IP sync helpers
// -----------------------------------------------------------------------

bool LevelSyncMgr::IsDKIPPushBlocked(uint8 sourceClass, uint8 targetTier) const
{
    return !_dkIPException && sourceClass == LEVELSYNC_CLASS_DEATH_KNIGHT && targetTier < LEVELSYNC_DK_IP_MIN_TIER;
}

std::vector<LevelSyncMgr::GroupIPMember> LevelSyncMgr::LoadGroupIPMembers(uint32 groupId) const
{
    std::vector<GroupIPMember> members;

    QueryResult r = CharacterDatabase.Query(
        "SELECT m.char_guid, c.`class`, MAX(q.quest) "
        "FROM levelsync_members m "
        "JOIN characters c ON m.char_guid = c.guid "
        "LEFT JOIN character_queststatus_rewarded q "
        "  ON q.guid = m.char_guid "
        "  AND q.quest BETWEEN {} AND {} "
        "  AND q.active = 1 "
        "WHERE m.group_id = {} "
        "GROUP BY m.char_guid, c.`class`",
        LEVELSYNC_IP_QUEST_BASE + 1,
        LEVELSYNC_IP_QUEST_BASE + LEVELSYNC_IP_MAX_TIER,
        groupId);

    if (!r)
        return members;

    do
    {
        GroupIPMember gm;
        gm.guid = r->Fetch()[0].Get<uint32>();
        gm.cls  = r->Fetch()[1].Get<uint8>();
        gm.tier = r->Fetch()[2].IsNull()
                  ? 0
                  : uint8(r->Fetch()[2].Get<uint32>() - LEVELSYNC_IP_QUEST_BASE);

        // Overlay live in-memory tier for online characters. ApplyIPTierToOnline
        // flips quest status in memory before the DB row commits, so reading
        // character_queststatus_rewarded directly can lag. Walk the player's
        // rewarded-quest status for the IP range and pick the highest.
        if (Player* online = ObjectAccessor::FindPlayerByLowGUID(gm.guid))
        {
            uint8 liveTier = 0;
            for (uint8 i = 1; i <= LEVELSYNC_IP_MAX_TIER; ++i)
            {
                if (online->GetQuestStatus(LEVELSYNC_IP_QUEST_BASE + i) == QUEST_STATUS_REWARDED)
                    liveTier = i;
            }
            gm.tier = liveTier;
        }

        members.push_back(gm);
    } while (r->NextRow());

    return members;
}

uint8 LevelSyncMgr::ComputeHighestIPTierInGroup(const std::vector<GroupIPMember>& members, uint8 targetCurrentTier) const
{
    uint8 highest = targetCurrentTier;
    for (auto const& m : members)
    {
        if (IsDKIPPushBlocked(m.cls, targetCurrentTier))
            continue;
        if (m.tier > highest)
            highest = m.tier;
    }
    return highest;
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

    // CSV of all candidate guids for the tier-lookup query.
    std::string candidateList;
    for (size_t i = 0; i < guids.size(); ++i)
    {
        if (i > 0) candidateList += ',';
        candidateList += std::to_string(guids[i]);
    }

    // One query to find each candidate's current tier (missing rows = tier 0).
    QueryResult r = CharacterDatabase.Query(
        "SELECT guid, MAX(quest) FROM character_queststatus_rewarded "
        "WHERE guid IN ({}) AND quest BETWEEN {} AND {} AND active = 1 "
        "GROUP BY guid",
        candidateList,
        LEVELSYNC_IP_QUEST_BASE + 1,
        LEVELSYNC_IP_QUEST_BASE + LEVELSYNC_IP_MAX_TIER);

    std::unordered_map<uint32, uint8> currentTierByGuid;
    if (r)
    {
        do
        {
            uint32 g = r->Fetch()[0].Get<uint32>();
            uint8  t = r->Fetch()[1].IsNull()
                       ? 0
                       : uint8(r->Fetch()[1].Get<uint32>() - LEVELSYNC_IP_QUEST_BASE);
            currentTierByGuid[g] = t;
        } while (r->NextRow());
    }

    // Keep only guids that actually need to move up to newTier.
    std::vector<uint32> toUpdate;
    toUpdate.reserve(guids.size());
    for (uint32 g : guids)
    {
        auto it = currentTierByGuid.find(g);
        uint8 curr = (it != currentTierByGuid.end()) ? it->second : 0;
        if (curr >= newTier || curr < minTier)
            continue;
        toUpdate.push_back(g);
    }

    if (toUpdate.empty())
        return;

    // Build the IN-list and the multi-row VALUES list in one pass.
    std::string updateList;
    std::string valuesList;
    uint32 newQuestId = LEVELSYNC_IP_QUEST_BASE + newTier;
    for (size_t i = 0; i < toUpdate.size(); ++i)
    {
        if (i > 0)
        {
            updateList += ',';
            valuesList += ',';
        }
        updateList += std::to_string(toUpdate[i]);
        valuesList += '(';
        valuesList += std::to_string(toUpdate[i]);
        valuesList += ',';
        valuesList += std::to_string(newQuestId);
        valuesList += ",1)";
    }

    // Atomically clear the old IP rows and insert the new tier row for every
    // qualifying guid. If either statement fails the whole pair rolls back,
    // so a partial failure cannot strand a character at tier 0.
    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();

    trans->Append(
        "DELETE FROM character_queststatus_rewarded "
        "WHERE guid IN ({}) AND quest BETWEEN {} AND {}",
        updateList,
        LEVELSYNC_IP_QUEST_BASE + 1,
        LEVELSYNC_IP_QUEST_BASE + LEVELSYNC_IP_MAX_TIER);

    trans->Append(
        "INSERT IGNORE INTO character_queststatus_rewarded "
        "(guid, quest, active) VALUES {}",
        valuesList);

    CharacterDatabase.CommitTransaction(trans);
}

// -----------------------------------------------------------------------
// IP sync entry points
// -----------------------------------------------------------------------

void LevelSyncMgr::SyncIPOnLogin(Player* player)
{
    if (!_enabled || !_allowProgressionSync)
        return;

    uint32 myGuid = player->GetGUID().GetCounter();

    GroupLoginInfo info;
    if (!LoadGroupLoginInfo(myGuid, info))
        return;
    if (!info.groupId || !info.progressionSyncEnabled)
        return;

    auto members = LoadGroupIPMembers(info.groupId);
    if (members.empty())
        return;

    uint8 myTier = 0;
    for (auto const& m : members)
    {
        if (m.guid == myGuid)
        {
            myTier = m.tier;
            break;
        }
    }

    uint8 highest = ComputeHighestIPTierInGroup(members, myTier);

    if (highest > myTier)
        ApplyIPTierToOnline(player, highest);

    uint8 effectiveTier = (highest > myTier) ? highest : myTier;
    uint8 myClass       = player->getClass();

    for (auto const& m : members)
    {
        if (m.guid == myGuid)
            continue;
        if (m.tier >= effectiveTier)
            continue;
        if (IsDKIPPushBlocked(myClass, m.tier))
            continue;

        if (Player* member = ObjectAccessor::FindPlayerByLowGUID(m.guid))
            ApplyIPTierToOnline(member, effectiveTier);
    }
}

void LevelSyncMgr::SyncIPOnLogout(Player* player)
{
    if (!_enabled || !_allowProgressionSync)
        return;

    uint32 myGuid = player->GetGUID().GetCounter();

    GroupLoginInfo info;
    if (!LoadGroupLoginInfo(myGuid, info))
        return;
    if (!info.groupId || !info.progressionSyncEnabled)
        return;

    auto members = LoadGroupIPMembers(info.groupId);
    if (members.empty())
        return;

    uint8 myTier = 0;
    for (auto const& m : members)
    {
        if (m.guid == myGuid)
        {
            myTier = m.tier;
            break;
        }
    }

    uint8 myClass = player->getClass();
    uint8 minTier = IsDKIPPushBlocked(myClass, 0) ? LEVELSYNC_DK_IP_MIN_TIER : 0;

    std::vector<uint32> offlineGuids;

    for (auto const& m : members)
    {
        if (m.guid == myGuid)
            continue;

        if (Player* member = ObjectAccessor::FindPlayerByLowGUID(m.guid))
        {
            if (m.tier < myTier)
            {
                if (IsDKIPPushBlocked(myClass, m.tier))
                    continue;
                ApplyIPTierToOnline(member, myTier);
            }
        }
        else
            offlineGuids.push_back(m.guid);
    }

    BatchUpdateOfflineIPTier(offlineGuids, myTier, minTier);
}

void LevelSyncMgr::SyncIPOnTierUp(Player* player, uint8 newTier)
{
    if (!_enabled || !_allowProgressionSync || _syncingIP)
        return;

    uint32 myGuid = player->GetGUID().GetCounter();

    GroupLoginInfo info;
    if (!LoadGroupLoginInfo(myGuid, info))
        return;
    if (!info.groupId || !info.progressionSyncEnabled)
        return;

    auto members = LoadGroupIPMembers(info.groupId);
    if (members.empty())
        return;

    uint8 myClass = player->getClass();
    uint8 minTier = IsDKIPPushBlocked(myClass, 0) ? LEVELSYNC_DK_IP_MIN_TIER : 0;
    std::vector<uint32> offlineGuids;

    for (auto const& m : members)
    {
        if (m.guid == myGuid)
            continue;

        if (Player* member = ObjectAccessor::FindPlayerByLowGUID(m.guid))
        {
            if (IsDKIPPushBlocked(myClass, m.tier))
                continue;
            if (m.tier < newTier)
                ApplyIPTierToOnline(member, newTier);
        }
        else
            offlineGuids.push_back(m.guid);
    }

    BatchUpdateOfflineIPTier(offlineGuids, newTier, minTier);
}

void LevelSyncMgr::SyncIPOnToggle(uint32 groupId)
{
    if (!_enabled || !_allowProgressionSync)
        return;

    auto members = LoadGroupIPMembers(groupId);
    if (members.empty())
        return;

    std::vector<uint32> offlineGuids;

    for (auto const& mi : members)
    {
        uint8 target = ComputeHighestIPTierInGroup(members, mi.tier);
        if (target <= mi.tier)
            continue;

        if (Player* p = ObjectAccessor::FindPlayerByLowGUID(mi.guid))
            ApplyIPTierToOnline(p, target);
        else
            offlineGuids.push_back(mi.guid);
    }

    uint8 highest = 0;
    for (auto const& mi : members)
    {
        if (!_dkIPException && mi.cls == LEVELSYNC_CLASS_DEATH_KNIGHT)
            continue;
        if (mi.tier > highest)
            highest = mi.tier;
    }

    if (highest > 0)
        BatchUpdateOfflineIPTier(offlineGuids, highest);
}

// -----------------------------------------------------------------------
// Gold pool
// -----------------------------------------------------------------------

LevelSyncMgr::PoolResult LevelSyncMgr::PoolGroupMoney(Player* caller, uint32 groupId)
{
    PoolResult out;

    uint32 callerGuid = caller->GetGUID().GetCounter();

    // Snapshot every other member's money. For online members, overlay the
    // live in-memory value because the DB `money` column only persists on
    // the next player save and can lag behind reality (e.g. the member just
    // sold to a vendor seconds ago). Stale DB values would short-change the
    // cap check and the credit math.
    QueryResult q = CharacterDatabase.Query(
        "SELECT m.char_guid, c.money "
        "FROM levelsync_members m "
        "JOIN characters c ON m.char_guid = c.guid "
        "WHERE m.group_id = {} AND m.char_guid != {}",
        groupId, callerGuid);

    if (!q)
    {
        out.status = PoolStatus::AloneInGroup;
        return out;
    }

    struct MoneyMember { uint32 guid; uint32 money; };
    std::vector<MoneyMember> members;
    do
    {
        MoneyMember mm;
        mm.guid  = q->Fetch()[0].Get<uint32>();
        mm.money = q->Fetch()[1].Get<uint32>();
        if (Player* online = ObjectAccessor::FindPlayerByLowGUID(mm.guid))
            mm.money = online->GetMoney();
        members.push_back(mm);
    } while (q->NextRow());

    uint64 snapshotTotal = 0;
    for (auto const& m : members)
        snapshotTotal += m.money;

    if (snapshotTotal == 0)
    {
        out.status = PoolStatus::NoGold;
        return out;
    }

    // Cap check up front. If caller's wallet + the snapshot total would
    // exceed MAX_MONEY_AMOUNT, refuse cleanly before touching anyone's
    // gold. Use uint64 arithmetic so the addition can't silently wrap.
    uint64 callerMoney = caller->GetMoney();
    uint64 projected   = callerMoney + snapshotTotal;
    if (projected > uint64(MAX_MONEY_AMOUNT))
    {
        out.status       = PoolStatus::CapExceeded;
        out.totalDrained = snapshotTotal;
        out.projectedSum = projected > 0xFFFFFFFFull ? 0xFFFFFFFFu : uint32(projected);
        return out;
    }

    // Drain online members first via SetMoney(0). PLAYER_FIELD_COINAGE is a
    // dirty-flagged UpdateField, so the client wallet refreshes within a
    // tick — same path vendors and mail use. Re-read GetMoney() inside the
    // loop in case the live value shifted between snapshot and drain.
    uint64 actualDrained = 0;
    std::vector<uint32> offlineGuids;
    offlineGuids.reserve(members.size());

    for (auto const& m : members)
    {
        Player* p = ObjectAccessor::FindPlayerByLowGUID(m.guid);
        if (!p)
        {
            offlineGuids.push_back(m.guid);
            continue;
        }

        uint32 amt = p->GetMoney();
        if (amt == 0)
            continue;

        p->SetMoney(0);
        actualDrained += amt;
        ++out.contributors;

        ChatHandler(p->GetSession()).PSendSysMessage(
            "|cff00ff00[LevelSync]|r Your wallet was pooled to |cffffff00{}|r "
            "(|cffffd700{}|r).",
            caller->GetName(),
            FormatMoneyString(amt));
    }

    // Drain offline members. Read-then-zero is two queries rather than a
    // single SELECT FOR UPDATE in a transaction because AC's connection
    // pool doesn't guarantee adjacent queries hit the same connection;
    // FOR UPDATE locks would only help if everything stayed on one. The
    // narrow race (a member logging in between the SELECT and the UPDATE)
    // is bounded by the world thread serialising both calls below this
    // point: ObjectAccessor stays consistent until this function returns.
    if (!offlineGuids.empty())
    {
        std::string inList;
        for (size_t i = 0; i < offlineGuids.size(); ++i)
        {
            if (i > 0) inList += ',';
            inList += std::to_string(offlineGuids[i]);
        }

        QueryResult offQ = CharacterDatabase.Query(
            "SELECT guid, money FROM characters WHERE guid IN ({}) AND money > 0",
            inList);

        if (offQ)
        {
            std::vector<uint32> toZero;
            do
            {
                uint32 g = offQ->Fetch()[0].Get<uint32>();
                uint32 amt = offQ->Fetch()[1].Get<uint32>();
                toZero.push_back(g);
                actualDrained += amt;
                ++out.contributors;
            } while (offQ->NextRow());

            if (!toZero.empty())
            {
                std::string zList;
                for (size_t i = 0; i < toZero.size(); ++i)
                {
                    if (i > 0) zList += ',';
                    zList += std::to_string(toZero[i]);
                }
                CharacterDatabase.Execute(
                    "UPDATE characters SET money = 0 WHERE guid IN ({})", zList);
            }
        }
    }

    // Credit caller. ModifyMoney has its own internal cap; the up-front
    // check makes overflow here practically impossible, but if the caller
    // somehow gained money mid-command (between snapshot and credit), fall
    // back to filling them to the cap and noting the orphaned amount in
    // the worldserver log so a GM can compensate by hand.
    if (actualDrained > 0)
    {
        uint64 nowMoney = caller->GetMoney();
        if (nowMoney + actualDrained > uint64(MAX_MONEY_AMOUNT))
        {
            uint32 canTake = (nowMoney >= uint64(MAX_MONEY_AMOUNT))
                ? 0u
                : uint32(uint64(MAX_MONEY_AMOUNT) - nowMoney);
            if (canTake > 0)
                caller->ModifyMoney(int32(canTake));
            LOG_ERROR("module",
                "[LevelSync] Money pool by GUID {} (group {}) drained {} copper "
                "but caller wallet only accepted {} — {} copper orphaned, "
                "GM compensation may be needed.",
                callerGuid, groupId, actualDrained, canTake,
                actualDrained - canTake);
        }
        else
        {
            caller->ModifyMoney(int32(actualDrained));
        }
    }

    LOG_INFO("module",
        "[LevelSync] Money pool: group {} caller GUID {} drained {} copper "
        "from {} contributor(s).",
        groupId, callerGuid, actualDrained, out.contributors);

    out.status       = PoolStatus::Ok;
    out.totalDrained = actualDrained;
    return out;
}

// -----------------------------------------------------------------------
// Player script
// -----------------------------------------------------------------------

class LevelSyncPlayerScript : public PlayerScript
{
public:
    LevelSyncPlayerScript() : PlayerScript("LevelSyncPlayerScript") {}

    // Sync triggers:
    //   - login / logout: full level + IP sync (lazy-sync model)
    //   - toggle command: forced full resync (.levelsync level on, .levelsync IP on)
    //   - tier-up via IP-progression quest reward: per-event IP sync only
    //
    // Per-event LEVEL sync is intentionally NOT hooked. Bot-driven XP gain
    // (e.g. mod-playerbots' SyncQuestWithPlayer mirroring) would cascade
    // levels back to the master and grant unearned dings. Level drift is
    // reconciled at session boundaries.
    //
    // Per-event IP TIER sync IS hooked. Tier-ups are discrete milestone
    // events triggered by mod-individual-progression's scripted logic when
    // a tier's content is completed; bots don't normally acquire IP-tier
    // quests through quest-mirroring, so the cascade concern is much weaker.
    // Real-time tier propagation matches user expectations for what's a
    // celebrated group milestone.
    void OnPlayerLogin(Player* player) override
    {
        sLevelSync->SyncGroupOnLogin(player);
        sLevelSync->SyncIPOnLogin(player);
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
