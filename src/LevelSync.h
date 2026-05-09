#pragma once

#include "Player.h"
#include "DatabaseEnv.h"
#include <ctime>
#include <unordered_map>
#include <vector>

#define sLevelSync LevelSyncMgr::instance()

constexpr uint8  LEVELSYNC_DK_MIN_LEVEL       = 55;
constexpr uint8  LEVELSYNC_CLASS_DEATH_KNIGHT = 6;
constexpr uint32 LEVELSYNC_IP_QUEST_BASE      = 66000;
constexpr uint8  LEVELSYNC_IP_MAX_TIER        = 18;
constexpr uint8  LEVELSYNC_DK_IP_MIN_TIER     = 13;

class LevelSyncMgr
{
public:
    static LevelSyncMgr* instance();

    void LoadConfig();

    bool     IsEnabled()              const { return _enabled; }
    bool     IsLevelSyncAllowed()     const { return _allowLevelSync; }
    bool     IsProgressionAllowed()   const { return _allowProgressionSync; }
    bool     IsDKExceptionEnabled()   const { return _dkException; }
    bool     IsDKIPExceptionEnabled() const { return _dkIPException; }
    uint32   GetMaxLinkedAccounts()   const { return _maxLinkedAccounts; }

    // Group queries
    uint32 GetGroupId(uint32 charGuid) const;
    uint32 GetGroupAccountCount(uint32 groupId) const;
    bool   IsGroupLevelSyncEnabled(uint32 groupId) const;
    bool   IsGroupProgressionSyncEnabled(uint32 groupId) const;
    std::vector<uint32> GetGroupMemberGuids(uint32 groupId, uint32 excludeGuid = 0) const;

    // Level sync
    void SyncGroupOnLogin(Player* player);
    void SyncGroupOnLogout(Player* player);
    void SyncGroupOnLevelToggle(uint32 groupId);

    // IP sync
    void  SyncIPOnLogin(Player* player);
    void  SyncIPOnLogout(Player* player);
    void  SyncIPOnTierUp(Player* player, uint8 newTier);
    void  SyncIPOnToggle(uint32 groupId);
    uint8 GetCharacterIPTierPublic(uint32 guid) const { return GetCharacterIPTier(guid); }

    // Toggle rate-limit. Returns true (and stamps the timer) if the toggle is
    // allowed; returns false and writes the seconds-until-next-attempt into
    // secondsRemaining if the group is still inside its cooldown window.
    bool TryConsumeToggleCooldown(uint32 groupId, uint32& secondsRemaining);

private:
    bool   _enabled              = true;
    bool   _allowLevelSync       = true;
    bool   _allowProgressionSync = false;
    bool   _dkException          = false;
    bool   _dkIPException        = false;
    uint32 _maxLinkedAccounts    = 6;

    // Re-entrance guards. Set true while ApplyLevelToOnline / ApplyIPTierToOnline
    // is in progress so the resulting OnPlayerLevelChanged / OnPlayerCompleteQuest
    // hooks for the receiver don't recursively trigger another sync pass.
    // Safe as a singleton because AzerothCore runs PlayerScript hooks on the
    // world thread; if AC ever multi-threads player ticks, these need to become
    // thread-local or per-player.
    bool _syncing   = false;
    bool _syncingIP = false;

    // Last accepted toggle time (epoch seconds) per group_id. Used by
    // TryConsumeToggleCooldown to throttle .levelsync level/IP on/off.
    std::unordered_map<uint32, time_t> _lastToggle;

    // Login-path helper: combines the per-character group lookup and the
    // group-level/progression-sync flag lookup into one query.
    struct GroupLoginInfo
    {
        uint32 groupId                = 0;
        bool   levelSyncEnabled       = false;
        bool   progressionSyncEnabled = false;
    };

    bool LoadGroupLoginInfo(uint32 charGuid, GroupLoginInfo& out) const;

    // Level sync helpers
    struct GroupLevelMember
    {
        uint32 guid;
        uint8  level;
        uint8  cls;
        uint32 xp;
    };

    std::vector<GroupLevelMember> LoadGroupLevelMembers(uint32 groupId) const;

    bool  IsDKPushBlocked(uint8 sourceClass, uint8 targetLevel) const;
    void  ApplyLevelToOnline(Player* target, uint8 newLevel);
    void  BatchUpdateOfflineLevel(const std::vector<uint32>& guids, uint8 newLevel, uint8 minCurrentLevel = 0);
    void  BatchUpdateOfflineXP(const std::vector<uint32>& guids, uint8 level, uint32 xp);

    // IP sync helpers
    struct GroupIPMember
    {
        uint32 guid;
        uint8  cls;
        uint8  tier;
    };

    std::vector<GroupIPMember> LoadGroupIPMembers(uint32 groupId) const;
    uint8 ComputeHighestIPTierInGroup(const std::vector<GroupIPMember>& members, uint8 targetCurrentTier) const;

    uint8 GetCharacterIPTier(uint32 guid) const;
    bool  IsDKIPPushBlocked(uint8 sourceClass, uint8 targetTier) const;
    void  ApplyIPTierToOnline(Player* target, uint8 newTier);
    void  BatchUpdateOfflineIPTier(const std::vector<uint32>& guids, uint8 newTier, uint8 minTier = 0);
};
