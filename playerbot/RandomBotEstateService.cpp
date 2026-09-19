#include "playerbot/playerbot.h"
#include "RandomBotEstateService.h"
#include "PlayerbotAIConfig.h"
#include "Entities/Player.h"

bool RandomBotEstateService::IsServiceCharacter(uint32 guid) const
{
    std::lock_guard<std::mutex> guard(mutex);
    return characters.count(guid) != 0;
}

bool RandomBotEstateService::IsServiceAccount(uint32 account) const
{
    std::lock_guard<std::mutex> guard(mutex);
    return accounts.count(account) != 0;
}

bool RandomBotEstateService::HasServiceAccounts() const
{
    std::lock_guard<std::mutex> guard(mutex);
    return !accounts.empty();
}

bool RandomBotEstateService::ValidateBroker(uint32 guid, uint32 account, uint32 faction)
{
    if (!guid || !account || !IsServiceCharacter(guid) ||
        sPlayerbotAIConfig.IsInRandomAccountList(account) ||
        sObjectMgr.GetPlayer(ObjectGuid(HIGHGUID_PLAYER, guid), false))
        return false;
    auto result = CharacterDatabase.PQuery("SELECT c.account,c.race,c.online,b.faction,b.enabled "
        "FROM characters c INNER JOIN ai_playerbot_estate_broker b ON b.guid=c.guid AND b.account=c.account "
        "WHERE c.guid=%u", guid);
    if (!result)
        return false;
    Field* f = result->Fetch();
    if (f[0].GetUInt32() != account || f[2].GetBool() || f[3].GetUInt32() != faction || !f[4].GetBool() ||
        Player::TeamForRace(f[1].GetUInt8()) != (faction == 0 ? ALLIANCE : HORDE))
        return false;
    // Exact pinned realmd predicate for a permanent account ban. Provisioning
    // must be explicit; neither account.locked nor expires_at=0 proves a ban on
    // this core. The service never inserts or changes authentication records.
    auto banned = LoginDatabase.PQuery("SELECT COUNT(*) FROM account_banned WHERE account_id=%u "
        "AND active=1 AND expires_at=banned_at", account);
    auto unrelated = CharacterDatabase.PQuery("SELECT COUNT(*) FROM characters c LEFT JOIN "
        "ai_playerbot_estate_broker b ON b.guid=c.guid AND b.account=c.account "
        "WHERE c.account=%u AND b.guid IS NULL", account);
    return banned && banned->Fetch()[0].GetUInt32() && unrelated && !unrelated->Fetch()[0].GetUInt32();
}

void RandomBotEstateService::Initialize()
{
    if (initialized)
        return;
    initialized = true;
    auto schema = CharacterDatabase.Query("SELECT COUNT(*) FROM information_schema.tables "
        "WHERE table_schema=DATABASE() AND table_name='ai_playerbot_estate_broker'");
    if (!schema || !schema->Fetch()[0].GetUInt32())
    {
        if (sPlayerbotAIConfig.retirementEnabled || sPlayerbotAIConfig.retirementAllianceBrokerGuid ||
            sPlayerbotAIConfig.retirementHordeBrokerGuid)
            sLog.outError("Retirement estate service: broker registry unavailable; processing disabled.");
        return;
    }
    // Persisted roles survive disabled config. A GUID/account mismatch does not
    // silently seize a character that changed accounts: it is never processed.
    auto registry = CharacterDatabase.Query("SELECT b.guid,b.account FROM (SELECT 1) seed "
        "LEFT JOIN ai_playerbot_estate_broker b ON 1=1 "
        "LEFT JOIN characters c ON c.guid=b.guid AND c.account=b.account "
        "WHERE b.guid IS NULL OR c.guid IS NOT NULL");
    if (!registry)
    {
        sLog.outError("Retirement estate service: broker registry snapshot unavailable or has no valid identities; processing disabled.");
        return;
    }
    {
        std::lock_guard<std::mutex> guard(mutex);
        do
        {
            if (!registry->Fetch()[0].IsNULL())
            {
                characters.insert(registry->Fetch()[0].GetUInt32());
                accounts.insert(registry->Fetch()[1].GetUInt32());
            }
        } while (registry->NextRow());
    }
    const auto& c = sPlayerbotAIConfig;
    ready = ValidateBroker(c.retirementAllianceBrokerGuid, c.retirementAllianceBrokerAccount, 0) &&
        ValidateBroker(c.retirementHordeBrokerGuid, c.retirementHordeBrokerAccount, 1) &&
        c.retirementAllianceBrokerGuid != c.retirementHordeBrokerGuid;
    if (!ready && c.retirementEnabled)
        sLog.outError("Retirement estate service: broker validation failed; no estate processing allowed. "
            "Require registered offline Alliance/Horde identities on dedicated permanently banned accounts.");
}
