#include "playerbot/playerbot.h"
#include "RandomBotLifecycle.h"
#include "RandomBotEstateService.h"
#include "PlayerbotAIConfig.h"
#include "Entities/Player.h"

void RandomBotLifecycleMgr::Initialize()
{
    if (initialized)
        return;
    initialized = true;
    sRandomBotEstateService.Initialize();

    // Always honor persisted exclusions, even when new retirement is disabled.
    // COUNT distinguishes a genuinely absent table from a failed schema query.
    auto tables = CharacterDatabase.Query(
        "SELECT COUNT(*) FROM information_schema.tables WHERE table_schema=DATABASE() "
        "AND table_name='ai_playerbot_lifecycle'");
    if (!tables)
    {
        exclusionsFailed = true;
        sLog.outError("Random bot lifecycle: schema lookup failed; random-bot selection is blocked.");
        return;
    }
    exclusionsAvailable = tables->Fetch()[0].GetUInt32() != 0;
    if (!exclusionsAvailable)
        return;

    RefreshExcludedBots();
    auto auxiliary = CharacterDatabase.Query(
        "SELECT COUNT(*) FROM information_schema.tables WHERE table_schema=DATABASE() "
        "AND table_name IN ('ai_playerbot_lifecycle_inheritance','ai_playerbot_lifecycle_item',"
        "'ai_playerbot_lifecycle_operation','ai_playerbot_lifecycle_auction','ai_playerbot_lifecycle_auction_return')");
    schemaAvailable = auxiliary && auxiliary->Fetch()[0].GetUInt32() == 5 && !exclusionsFailed;
}

void RandomBotLifecycleMgr::RefreshExcludedBots()
{
    // LEFT JOIN supplies one NULL row when no exclusions exist. A null result
    // therefore means a database failure, not permission to log retired bots in.
    auto result = CharacterDatabase.Query(
        "SELECT l.guid,c.account FROM (SELECT 1) seed "
        "LEFT JOIN ai_playerbot_lifecycle l ON l.status>=2 "
        "LEFT JOIN characters c ON c.guid=l.guid AND c.account=l.account");
    if (!result)
    {
        exclusionsFailed = true;
        sLog.outError("Random bot lifecycle: exclusion snapshot failed; random-bot selection is blocked.");
        return;
    }
    std::unordered_set<uint32> refreshed;
    do
    {
        if (!result->Fetch()[0].IsNULL() && !result->Fetch()[1].IsNULL() &&
            sPlayerbotAIConfig.IsInRandomAccountList(result->Fetch()[1].GetUInt32()))
            refreshed.insert(result->Fetch()[0].GetUInt32());
    } while (result->NextRow());
    std::lock_guard<std::mutex> guard(exclusionMutex);
    excludedBots.swap(refreshed);
    exclusionsFailed = false;
}

bool RandomBotLifecycleMgr::IsLoginEligible(uint32 guid) const
{
    if (sRandomBotEstateService.IsServiceCharacter(guid))
        return false;
    if (exclusionsFailed)
        return false;
    std::lock_guard<std::mutex> guard(exclusionMutex);
    return excludedBots.find(guid) == excludedBots.end() && !offlineLeases.count(guid);
}

bool RandomBotLifecycleMgr::IsRetiringOrRetired(uint32 guid) const
{
    // An inheritance recipient's temporary offline lease is not retirement.
    std::lock_guard<std::mutex> guard(exclusionMutex);
    return excludedBots.count(guid) != 0;
}

uint64 RandomBotLifecycleMgr::LoginRevision(uint32 guid) const
{
    std::lock_guard<std::mutex> guard(exclusionMutex);
    auto found = loginRevisions.find(guid);
    return found == loginRevisions.end() ? 0 : found->second;
}

bool RandomBotLifecycleMgr::CanLoadCharacter(uint32 guid, uint64 revision) const
{
    if (sRandomBotEstateService.IsServiceCharacter(guid))
        return false;
    std::lock_guard<std::mutex> guard(exclusionMutex);
    auto found = loginRevisions.find(guid);
    const uint64 current = found == loginRevisions.end() ? 0 : found->second;
    return !offlineLeases.count(guid) && !excludedBots.count(guid) && revision == current;
}

bool RandomBotLifecycleMgr::AcquireOfflineLease(uint32 guid)
{
    // World thread only. Core login completion checks this lease before loading
    // any Player, including holders that were already queued or preloaded.
    if (sObjectMgr.GetPlayer(ObjectGuid(HIGHGUID_PLAYER, guid), false))
        return false;
    std::lock_guard<std::mutex> guard(exclusionMutex);
    if (!offlineLeases.insert(guid).second)
        return false;
    ++loginRevisions[guid];
    return true;
}

void RandomBotLifecycleMgr::ReleaseOfflineLease(uint32 guid)
{
    std::lock_guard<std::mutex> guard(exclusionMutex);
    ++loginRevisions[guid];
    offlineLeases.erase(guid);
}

void RandomBotLifecycleMgr::Update(uint32 /*maxOnlineBots*/)
{
    Initialize();
    // Deliberately unavailable until acknowledged inventory/mail/auction saves,
    // quiescence, and replacement creation are integrated and fault-tested.
    // Do not remove this gate merely because the estate store is implemented.
    static bool reportedUnavailable = false;
    if (sPlayerbotAIConfig.retirementEnabled && !reportedUnavailable)
    {
        sLog.outError("Random bot retirement is unavailable: liquidation/save/replacement integration "
            "is incomplete. No retirements or economic mutations will be performed.");
        reportedUnavailable = true;
    }
}
