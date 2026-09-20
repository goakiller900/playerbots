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

bool RandomBotEstateService::ValidateBroker(uint32 guid, uint32 account, uint32 faction, uint32 role)
{
    if (!guid || !account || !IsServiceCharacter(guid) ||
        sPlayerbotAIConfig.IsInRandomAccountList(account) ||
        sObjectMgr.GetPlayer(ObjectGuid(HIGHGUID_PLAYER, guid), false))
        return false;
    auto result = CharacterDatabase.PQuery("SELECT c.account,c.race,c.online,b.faction,b.role,b.enabled "
        "FROM characters c INNER JOIN ai_playerbot_estate_broker b ON b.guid=c.guid AND b.account=c.account "
        "WHERE c.guid=%u", guid);
    if (!result)
        return false;
    Field* f = result->Fetch();
    if (f[0].GetUInt32() != account || f[2].GetBool() || f[3].GetUInt32() != faction ||
        f[4].GetUInt32() != role || !f[5].GetBool() ||
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
    auto engines=CharacterDatabase.Query("SELECT COUNT(*) FROM information_schema.tables WHERE table_schema=DATABASE() AND engine='InnoDB' AND table_name IN ("
        "'characters','character_inventory','item_instance','mail','mail_items','auction','character_gifts','item_loot','ai_playerbot_lifecycle',"
        "'ai_playerbot_estate_broker','ai_playerbot_estate','ai_playerbot_estate_lot','ai_playerbot_estate_operation',"
        "'ai_playerbot_estate_auction','ai_playerbot_estate_bid_claim','ai_playerbot_estate_auction_bid','ai_playerbot_estate_inheritance')");
    if(!engines||engines->Fetch()[0].GetUInt32()!=17)
    {
        if(sPlayerbotAIConfig.retirementEnabled)
            sLog.outError("Retirement estate service: required transaction tables are absent or not InnoDB; processing disabled.");
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
    auto historicalIds = CharacterDatabase.Query(
        "SELECT (SELECT COALESCE(MAX(auction_id),0) FROM ai_playerbot_estate_auction),GREATEST("
        "(SELECT COALESCE(MAX(GREATEST(winner_mail_id,seller_mail_id,return_mail_id)),0) FROM ai_playerbot_estate_auction),"
        "(SELECT COALESCE(MAX(mail_id),0) FROM ai_playerbot_estate_bid_claim),"
        "(SELECT COALESCE(MAX(GREATEST(refund_mail_id,winner_mail_id)),0) FROM ai_playerbot_estate_auction_bid))");
    if (!historicalIds)
    {
        sLog.outError("Retirement estate service: auction identity history unavailable; processing disabled.");
        return;
    }
    sObjectMgr.EnsureAuctionIdAbove(historicalIds->Fetch()[0].GetUInt32());
    sObjectMgr.EnsureMailIdAbove(historicalIds->Fetch()[1].GetUInt32());
    auto historicalCharacters=CharacterDatabase.Query("SELECT COALESCE(MAX(replacement_guid),0) FROM ai_playerbot_lifecycle");
    if(!historicalCharacters)
    {
        sLog.outError("Retirement estate service: replacement identity history unavailable; processing disabled.");
        return;
    }
    sObjectMgr.EnsurePlayerGuidAbove(historicalCharacters->Fetch()[0].GetUInt32());
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
    ready = ValidateBroker(c.retirementAllianceBrokerGuid, c.retirementAllianceBrokerAccount, 0, 0) &&
        ValidateBroker(c.retirementHordeBrokerGuid, c.retirementHordeBrokerAccount, 1, 0) &&
        ValidateBroker(c.retirementAllianceBidBrokerGuid, c.retirementAllianceBidBrokerAccount, 0, 1) &&
        ValidateBroker(c.retirementHordeBidBrokerGuid, c.retirementHordeBidBrokerAccount, 1, 1);
    allianceSeller = c.retirementAllianceBrokerGuid;
    hordeSeller = c.retirementHordeBrokerGuid;
    allianceBidder = c.retirementAllianceBidBrokerGuid;
    hordeBidder = c.retirementHordeBidBrokerGuid;
    std::unordered_set<uint32> distinct = {allianceSeller, hordeSeller, allianceBidder, hordeBidder};
    ready = ready && !distinct.count(0) && distinct.size() == 4;
    if (!ready && c.retirementEnabled)
        sLog.outError("Retirement estate service: broker validation failed; no estate processing allowed. "
            "Require registered offline Alliance/Horde identities on dedicated permanently banned accounts.");
}
