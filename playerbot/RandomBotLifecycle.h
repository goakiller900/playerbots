#pragma once

#include "Common.h"

#include <mutex>
#include <ctime>
#include <string>
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include <future>
class Player;

#include "RandomBotLifecycleMath.h"

class RandomBotLifecycleMgr
{
public:
    static RandomBotLifecycleMgr& instance()
    {
        static RandomBotLifecycleMgr instance;
        return instance;
    }

    void Initialize();
    void Update(uint32 maxOnlineBots);
    // No configuration, command, or SQL value can bypass this development gate.
    // Change only in a separate review after all documented acceptance gates.
    static bool ExecutionAllowed() { return false; }

    bool IsSchemaAvailable() const { return schemaAvailable; }
    bool IsLoginEligible(uint32 guid) const;
    bool IsRetiringOrRetired(uint32 guid) const;
    uint64 LoginRevision(uint32 guid) const;
    bool CanLoadCharacter(uint32 guid, uint32 account, uint64 revision) const;
    bool AcquireOfflineLease(uint32 guid);
    void ReleaseOfflineLease(uint32 guid);

private:
    RandomBotLifecycleMgr() = default;

    void RefreshExcludedBots();
    void ScheduleWork();

private:
    mutable std::mutex exclusionMutex;
    std::unordered_set<uint32> excludedBots;
    std::unordered_set<uint32> offlineLeases;
    std::unordered_map<uint32, uint64> loginRevisions;
    bool initialized = false;
    bool schemaAvailable = false;
    bool exclusionsAvailable = false;
    bool exclusionsFailed = false;
    bool workPending = false;
    bool workReloadLoginPool = false;
    uint64 workCompletedEstateId = 0;
    std::string workSuccessMessage;
    uint32 workLeaseGuid = 0;
    uint32 workLifecycleGuid = 0;
    std::future<bool> workResult;
    time_t nextWorkAt = 0;
    time_t nextEligibilityAt = 0;
};

#define sRandomBotLifecycleMgr RandomBotLifecycleMgr::instance()

#ifdef GenerateBotTests
bool RunRandomBotLifecycleMathTests(std::string& error);
#endif
