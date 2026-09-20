#include "playerbot/playerbot.h"
#include "RandomBotLifecycle.h"
#include "RandomBotEstateService.h"
#include "RandomBotEstateStore.h"
#include "RandomBotEstateAuctionMgr.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotLoginMgr.h"
#include "RandomPlayerbotFactory.h"
#include "Entities/Player.h"
#include "World/World.h"
#include <chrono>
#include <sstream>
#include <algorithm>

namespace
{
std::vector<uint32> EligibleAccounts()
{
    std::vector<uint32> result;
    for(uint32 account:sPlayerbotAIConfig.randomBotAccounts)
        if(!sRandomBotEstateService.IsServiceAccount(account)) result.push_back(account);
    return result;
}
std::string AccountList(std::vector<uint32> const& accounts)
{
    std::ostringstream out;for(uint32 a:accounts){if(out.tellp()>0)out<<',';out<<a;}return accounts.empty()?"0":out.str();
}
}

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
    schemaAvailable = !exclusionsFailed;
}

void RandomBotLifecycleMgr::RefreshExcludedBots()
{
    // LEFT JOIN supplies one NULL row when no exclusions exist. A null result
    // therefore means a database failure, not permission to log retired bots in.
    auto result = CharacterDatabase.Query(
        "SELECT x.guid,IF(x.reservation=1,x.account,c.account) FROM (SELECT 1) seed LEFT JOIN ("
        "SELECT guid,account,0 AS reservation FROM ai_playerbot_lifecycle WHERE status>=2 UNION ALL "
        "SELECT replacement_guid,replacement_account,1 FROM ai_playerbot_lifecycle "
        "WHERE replacement_status=1 AND replacement_guid<>0) x ON 1=1 "
        "LEFT JOIN characters c ON c.guid=x.guid AND c.account=x.account");
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

bool RandomBotLifecycleMgr::CanLoadCharacter(uint32 guid, uint32 account, uint64 revision) const
{
    if (sRandomBotEstateService.IsServiceCharacter(guid))
        return false;
    // The core login hook also runs for ordinary client characters. Lifecycle
    // exclusions and fail-closed schema handling are deliberately scoped to
    // the existing random-account population.
    if (!sPlayerbotAIConfig.IsInRandomAccountList(account))
        return true;
    std::lock_guard<std::mutex> guard(exclusionMutex);
    if (exclusionsFailed)
        return false;
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
    // The complete worker remains unreachable in production builds pending the
    // documented isolated Linux/MariaDB crash matrix. No config can bypass it.
    static bool reportedUnavailable = false;
    if (!ExecutionAllowed())
    {
        if (sPlayerbotAIConfig.retirementEnabled && !reportedUnavailable)
        {
            sLog.outError("Random bot retirement is unavailable pending the documented Linux/MariaDB acceptance gates. "
                "No retirements or economic mutations will be performed.");
            reportedUnavailable = true;
        }
        return;
    }
    // Retirement.Enabled controls admission of new characters.  Persisted
    // intakes and detached estates must keep recovering when an operator turns
    // admission off, otherwise already-owned assets can remain stranded.
    if(!schemaAvailable||!sRandomBotEstateService.Ready())return;
    if(workPending)
    {
        if(workResult.wait_for(std::chrono::seconds(0))!=std::future_status::ready)return;
        bool ok=false;try{ok=workResult.get();}catch(...){ok=false;}
        workPending=false;
        const bool reloadLoginPool=workReloadLoginPool;
        workReloadLoginPool=false;
        const uint64 completedEstateId=workCompletedEstateId;
        workCompletedEstateId=0;
        const std::string successMessage=workSuccessMessage;
        workSuccessMessage.clear();
        const uint32 completedLifecycleGuid=workLifecycleGuid;
        workLifecycleGuid=0;
        if(workLeaseGuid)
        {
            ReleaseOfflineLease(workLeaseGuid);
            workLeaseGuid=0;
        }
        RefreshExcludedBots();
        if(ok&&reloadLoginPool)
            sPlayerBotLoginMgr.RequestBotPoolReload();
        if(ok&&!successMessage.empty())
            sLog.outString("%s",successMessage.c_str());
        if(ok&&completedEstateId)
        {
            auto summary=CharacterDatabase.PQuery("SELECT original_guid,final_estate,vendor_income,auction_income,auction_fees,"
                "auction_cuts,gold_sunk,gold_distributed,(SELECT COUNT(*) FROM ai_playerbot_estate_inheritance i "
                "WHERE i.estate_id=e.estate_id AND i.delivered_amount<>0) FROM ai_playerbot_estate e WHERE estate_id='" UI64FMTD "'",
                completedEstateId);
            if(summary)
            {
                Field* s=summary->Fetch();
                sLog.outString("Retirement estate " UI64FMTD " for bot %u completed: final=" UI64FMTD
                    " vendor=" UI64FMTD " auction=" UI64FMTD " fees=" UI64FMTD " cuts=" UI64FMTD
                    " sunk=" UI64FMTD " inherited=" UI64FMTD " recipients=%u copper.",completedEstateId,
                    s[0].GetUInt32(),s[1].GetUInt64(),s[2].GetUInt64(),s[3].GetUInt64(),s[4].GetUInt64(),
                    s[5].GetUInt64(),s[6].GetUInt64(),s[7].GetUInt64(),s[8].GetUInt32());
            }
        }
        if(!ok)
        {
            if(completedLifecycleGuid)
                CharacterDatabase.PExecute("UPDATE ai_playerbot_lifecycle SET updated_at=UNIX_TIMESTAMP()+30 WHERE guid=%u AND status IN (2,7,9,10)",
                    completedLifecycleGuid);
            nextWorkAt=time(nullptr)+5;
            sLog.outError("Random bot retirement work did not confirm; durable state will be reconciled before retry.");
        }
    }
    if(time(nullptr)<nextWorkAt)return;
    nextWorkAt=time(nullptr)+1;
    ScheduleWork();
}

void RandomBotLifecycleMgr::ScheduleWork()
{
    const time_t now=time(nullptr);
    const auto accounts=EligibleAccounts();
    if(accounts.empty())return;
    const std::string in=AccountList(accounts);
    if(sPlayerbotAIConfig.retirementEnabled && now>=nextEligibilityAt)
    {
        nextEligibilityAt=now+sPlayerbotAIConfig.retirementCheckInterval;
        const uint32 maxLevel=sWorld.getConfig(CONFIG_UINT32_MAX_PLAYER_LEVEL);
        const uint32 maxPer=sPlayerbotAIConfig.retirementMaxPerCycle;
        const uint32 maxPct=sPlayerbotAIConfig.retirementMaxLevelPopulationPercent;
        const uint64 minSince=uint64(now)-std::min<uint64>(uint64(now),sPlayerbotAIConfig.retirementMinMaxLevelTime);
        workResult=CharacterDatabase.QueueTransaction([in,maxLevel,maxPer,maxPct,minSince](SqlConnection& c)
        {
            if(!c.Execute(("INSERT IGNORE INTO ai_playerbot_lifecycle (guid,account,status,updated_at) SELECT guid,account,0,UNIX_TIMESTAMP() "
                "FROM characters WHERE account IN ("+in+")").c_str())||
                !c.Execute(("UPDATE ai_playerbot_lifecycle l INNER JOIN characters c ON c.guid=l.guid AND c.account=l.account "
                "SET l.status=0,l.max_level_since=0,l.updated_at=UNIX_TIMESTAMP() WHERE l.status=1 AND c.level<"+
                std::to_string(maxLevel)).c_str())||!c.Execute(("UPDATE ai_playerbot_lifecycle l INNER JOIN characters c "
                "ON c.guid=l.guid AND c.account=l.account SET l.status=1,l.max_level_since=UNIX_TIMESTAMP(),l.updated_at=UNIX_TIMESTAMP() "
                "WHERE l.status=0 AND c.level>="+std::to_string(maxLevel)).c_str()))return false;
            auto counts=c.Query(("SELECT COUNT(*),SUM(c.level>="+std::to_string(maxLevel)+") FROM characters c LEFT JOIN "
                "ai_playerbot_lifecycle l ON l.guid=c.guid WHERE c.account IN ("+in+") AND (l.guid IS NULL OR l.status<2)").c_str());
            if(!counts||!maxPer||!counts->Fetch()[0].GetUInt64()||
                counts->Fetch()[1].GetUInt64()*100<=counts->Fetch()[0].GetUInt64()*maxPct)return true;
            return c.Execute(("UPDATE ai_playerbot_lifecycle l INNER JOIN (SELECT guid FROM (SELECT l2.guid FROM "
                "ai_playerbot_lifecycle l2 INNER JOIN characters c2 ON c2.guid=l2.guid AND c2.account=l2.account "
                "WHERE l2.status=1 AND l2.protected=0 AND l2.max_level_since>0 AND l2.max_level_since<="+
                std::to_string(minSince)+" AND c2.level>="+std::to_string(maxLevel)+" AND c2.online=0 AND c2.account IN ("+in+") "
                "AND NOT EXISTS (SELECT 1 FROM group_member gm WHERE gm.memberGuid=c2.guid) ORDER BY RAND() LIMIT "+
                std::to_string(maxPer)+") chosen_rows) chosen ON chosen.guid=l.guid SET l.status=2,l.retirement_started_at=UNIX_TIMESTAMP(),"
                "l.updated_at=UNIX_TIMESTAMP() WHERE l.status=1").c_str());
        });
        workPending=true;return;
    }
    auto row=CharacterDatabase.PQuery("SELECT l.guid,l.account,c.race,l.status,l.disposition,e.estate_id,e.status,e.broker_guid,e.broker_account "
        "FROM ai_playerbot_lifecycle l INNER JOIN characters c ON c.guid=l.guid AND c.account=l.account LEFT JOIN "
        "ai_playerbot_estate e ON e.original_guid=l.guid WHERE l.status IN (2,10) AND c.online=0 "
        "AND l.updated_at<=UNIX_TIMESTAMP() ORDER BY l.updated_at,l.guid LIMIT 1");
    if(!row)
    {
        auto deleting=CharacterDatabase.PQuery("SELECT guid,account FROM ai_playerbot_lifecycle WHERE status=8 ORDER BY updated_at,guid LIMIT 1");
        if(deleting)
        {
            uint32 guid=deleting->Fetch()[0].GetUInt32(),account=deleting->Fetch()[1].GetUInt32();
            auto exists=CharacterDatabase.PQuery("SELECT COUNT(*) FROM characters WHERE guid=%u AND account=%u",guid,account);
            if(exists&&exists->Fetch()[0].GetUInt32()){Player::DeleteFromDB(ObjectGuid(HIGHGUID_PLAYER,guid),account,true,true);nextWorkAt=now+10;return;}
            workSuccessMessage="Random bot "+std::to_string(guid)+" retirement deletion finalized.";
            workResult=RandomBotEstateStore::ConfirmDeleted(guid,account);workPending=true;return;
        }
        auto replacement=CharacterDatabase.PQuery("SELECT guid,account,replacement_status,replacement_guid,replacement_account,"
            "replacement_name,replacement_race,replacement_class,replacement_gender FROM ai_playerbot_lifecycle WHERE status IN (7,9) "
            "AND replacement_required=1 AND replacement_status<2 AND updated_at<=UNIX_TIMESTAMP() ORDER BY updated_at,guid LIMIT 1");
        if(replacement)
        {
            Field* x=replacement->Fetch();RandomBotReplacementReservation r;r.retiringGuid=x[0].GetUInt32();
            if(x[2].GetUInt32()==0)
            {
                r.account=x[1].GetUInt32();RandomPlayerbotFactory factory(r.account);r.cls=factory.GetRandomClass();r.race=factory.GetRandomRace(r.cls);
                r.gender=urand(0,1);r.name=RandomPlayerbotFactory::CreateRandomBotName(RandomPlayerbotFactory::CombineRaceAndGender(r.gender,r.race));
                if(r.name.empty())return;r.guid=sObjectMgr.GeneratePlayerLowGuid();r.eligibleAccounts=accounts;
                workLifecycleGuid=r.retiringGuid;
                workSuccessMessage="Random bot "+std::to_string(r.retiringGuid)+" reserved replacement "+std::to_string(r.guid)+".";
                workResult=RandomBotEstateStore::ReserveReplacement(r);workPending=true;return;
            }
            r.guid=x[3].GetUInt32();r.account=x[4].GetUInt32();r.name=x[5].GetCppString();r.race=x[6].GetUInt8();
            r.cls=x[7].GetUInt8();r.gender=x[8].GetUInt8();r.eligibleAccounts=accounts;
            auto exists=CharacterDatabase.PQuery("SELECT COUNT(*) FROM characters WHERE guid=%u AND account=%u",
                r.guid,r.account);
            if(exists&&exists->Fetch()[0].GetUInt32())
            {
                // CreateRandomBot normally participates in the factory's bulk
                // save/logout pass.  A lifecycle replacement is a single
                // reserved creation, so finish that same cleanup once its row
                // is visible before confirming the reservation.
                if(Player* created=sObjectMgr.GetPlayer(ObjectGuid(HIGHGUID_PLAYER,r.guid),false))
                {
                    WorldSession* session=created->GetSession();
                    if(session)session->LogoutPlayer();
                    sObjectAccessor.RemoveObject(created);
                    delete created;
                    delete session;
                }
                workLifecycleGuid=r.retiringGuid;workReloadLoginPool=true;
                workSuccessMessage="Random bot "+std::to_string(r.retiringGuid)+" replacement "+std::to_string(r.guid)+" finalized.";
                workResult=RandomBotEstateStore::ConfirmReplacement(r);workPending=true;return;
            }
            if(sObjectMgr.GetPlayer(ObjectGuid(HIGHGUID_PLAYER,r.guid),false)){nextWorkAt=now+5;return;}
            RandomPlayerbotFactory factory(r.account);
            if(!factory.CreateRandomBot(r.cls,r.race,r.guid,r.name,r.gender))
            {
                CharacterDatabase.PExecute("UPDATE ai_playerbot_lifecycle SET updated_at=UNIX_TIMESTAMP()+30 WHERE guid=%u AND replacement_status=1",
                    r.retiringGuid);
                sLog.outError("Random bot retirement replacement %u for lifecycle %u could not be created; retry deferred.",
                    r.guid,r.retiringGuid);
            }
            else if(Player* created=sObjectMgr.GetPlayer(ObjectGuid(HIGHGUID_PLAYER,r.guid),false))
            {
                // Persist through the same core Player save path used by bulk
                // random-bot creation. Keep the object registered until the DB
                // row is observed so this reservation cannot be recreated in
                // the asynchronous save window.
                created->SaveToDB();
                sLog.outString("Random bot retirement replacement %u (%s) created for lifecycle %u; awaiting durable save.",
                    r.guid,r.name.c_str(),r.retiringGuid);
            }
            nextWorkAt=now+5;return;
        }
        // Do not let one estate with outstanding long-running auctions starve
        // every estate behind it.  Status 2 is selected only when all lots,
        // auction mail and inherited-bid claims have reached a terminal state;
        // status 3 remains selectable while its persisted inheritance ledger
        // is delivered.
        auto estate=CharacterDatabase.PQuery("SELECT e.estate_id,e.status FROM ai_playerbot_estate e WHERE e.status=3 OR "
            "(e.status=2 AND NOT EXISTS (SELECT 1 FROM ai_playerbot_estate_lot l WHERE l.estate_id=e.estate_id AND l.status IN (0,1,2,3,8)) "
            "AND NOT EXISTS (SELECT 1 FROM ai_playerbot_estate_auction a WHERE a.estate_id=e.estate_id AND a.status<>4) "
            "AND NOT EXISTS (SELECT 1 FROM ai_playerbot_estate_bid_claim b WHERE b.estate_id=e.estate_id AND b.status<>3)) "
            "ORDER BY e.updated_at,e.estate_id LIMIT 1");
        if(!estate)return;uint64 estateId=estate->Fetch()[0].GetUInt64();
        if(estate->Fetch()[1].GetUInt32()==2)
        {RandomBotEstateDistribution r;r.estateId=estateId;r.moneyCap=MAX_MONEY_AMOUNT;r.recipientCap=sPlayerbotAIConfig.retirementMaxInheritancePerBot;
         r.sinkMin=sPlayerbotAIConfig.retirementGoldSinkMinPercent;r.sinkMax=sPlayerbotAIConfig.retirementGoldSinkMaxPercent;
         r.minRecipients=sPlayerbotAIConfig.retirementInheritanceMinRecipients;r.maxRecipients=sPlayerbotAIConfig.retirementInheritanceMaxRecipients;
         r.eligibleAccounts=accounts;workResult=RandomBotEstateStore::PrepareDistribution(r);workPending=true;return;}
        auto inheritance=CharacterDatabase.PQuery("SELECT recipient_guid FROM ai_playerbot_estate_inheritance WHERE estate_id='" UI64FMTD "' AND delivered=0 ORDER BY recipient_guid LIMIT 1",estateId);
        if(inheritance)
        {
            const uint32 recipient=inheritance->Fetch()[0].GetUInt32();
            if(!AcquireOfflineLease(recipient)){nextWorkAt=now+5;return;}
            workLeaseGuid=recipient;
            workResult=RandomBotEstateStore::DeliverInheritance(estateId,recipient,MAX_MONEY_AMOUNT,accounts);
        }
        else
        {
            workCompletedEstateId=estateId;
            workResult=RandomBotEstateStore::CompleteDistribution(estateId);
        }
        workPending=true;return;
    }
    Field* f=row->Fetch();const uint32 guid=f[0].GetUInt32(),account=f[1].GetUInt32(),race=f[2].GetUInt32(),lifeStatus=f[3].GetUInt32();
    const uint64 estate=f[5].IsNULL()?0:f[5].GetUInt64();
    if(lifeStatus==10)
    {
        if(!AcquireOfflineLease(guid)){nextWorkAt=now+5;return;}
        workLeaseGuid=guid;
        workLifecycleGuid=guid;
        workSuccessMessage="Random bot "+std::to_string(guid)+(f[4].GetUInt32()==0?
            " archived after retirement intake.":" marked delete-pending after retirement intake.");
        workResult=f[4].GetUInt32()==0?RandomBotEstateStore::FinalizeArchive(estate,guid,account):
            RandomBotEstateStore::MarkDeletePending(estate,guid,account);workPending=true;return;
    }
    if(f[5].IsNULL())
    {
        if(!AcquireOfflineLease(guid)){nextWorkAt=now+5;return;}
        workLeaseGuid=guid;
        workLifecycleGuid=guid;
        RandomBotEstateIntake r;r.originalGuid=guid;r.originalAccount=account;
        const uint32 faction=Player::TeamForRace(race)==HORDE?1:0;r.brokerGuid=sRandomBotEstateService.SellerBroker(faction);
        r.brokerAccount=faction?sPlayerbotAIConfig.retirementHordeBrokerAccount:sPlayerbotAIConfig.retirementAllianceBrokerAccount;
        r.disposition=uint32(sPlayerbotAIConfig.retirementDisposition);r.replacementRequired=sPlayerbotAIConfig.retirementCreateReplacement;
        r.eligibleAccounts=accounts;
        workSuccessMessage="Random bot "+std::to_string(guid)+" retirement intake started.";
        workResult=RandomBotEstateStore::BeginIntake(r);workPending=true;return;
    }
    const uint32 broker=f[7].GetUInt32(),brokerAccount=f[8].GetUInt32();
    auto mail=CharacterDatabase.PQuery("SELECT id,money,"
        "IF(messageType=2 AND SUBSTRING_INDEX(subject,':',-1)='2',CAST(SUBSTRING_INDEX(SUBSTRING_INDEX(body,':',2),':',-1) AS UNSIGNED),0),"
        "IF(messageType=2 AND SUBSTRING_INDEX(subject,':',-1)='2',CAST(SUBSTRING_INDEX(SUBSTRING_INDEX(body,':',4),':',-1) AS UNSIGNED),0),"
        "IF(messageType=2 AND SUBSTRING_INDEX(subject,':',-1)='2',CAST(SUBSTRING_INDEX(body,':',-1) AS UNSIGNED),0) FROM mail "
        "WHERE receiver=%u AND cod=0 AND (money<>0 OR has_items<>0) ORDER BY id LIMIT 1",guid);
    if(mail)
    {
        if(!AcquireOfflineLease(guid)){nextWorkAt=now+5;return;}
        workLeaseGuid=guid;
        workLifecycleGuid=guid;
        RandomBotEstateMailIntake r;r.estateId=estate;r.originalGuid=guid;r.originalAccount=account;r.brokerGuid=broker;
        r.brokerAccount=brokerAccount;r.mailId=mail->Fetch()[0].GetUInt32();r.money=mail->Fetch()[1].GetUInt64();
        r.auctionBid=mail->Fetch()[2].GetUInt32();r.auctionDeposit=mail->Fetch()[3].GetUInt32();
        r.auctionCut=mail->Fetch()[4].GetUInt32();
        auto items=CharacterDatabase.PQuery("SELECT i.guid,i.itemEntry,i.count FROM mail_items mi INNER JOIN item_instance i ON i.guid=mi.item_guid "
            "WHERE mi.mail_id=%u AND mi.receiver=%u ORDER BY i.guid",r.mailId,guid);
        if(items)do{RandomBotEstateMailItem i;i.itemGuid=items->Fetch()[0].GetUInt32();i.itemEntry=items->Fetch()[1].GetUInt32();
            i.itemCount=items->Fetch()[2].GetUInt32();r.items.push_back(i);}while(items->NextRow());
        workResult=RandomBotEstateStore::TransferMail(r);workPending=true;return;
    }
    auto auction=CharacterDatabase.PQuery("SELECT id,houseid,itemguid,item_template,item_count,startbid,buyoutprice,buyguid,lastbid,deposit,time,moneyTime "
        "FROM auction WHERE itemowner=%u ORDER BY id LIMIT 1",guid);
    if(auction)
    {
        if(!AcquireOfflineLease(guid)){nextWorkAt=now+5;return;}
        workLeaseGuid=guid;
        workLifecycleGuid=guid;
        RandomBotEstateAuctionIntake r;r.estateId=estate;r.originalGuid=guid;r.brokerGuid=broker;Field* a=auction->Fetch();
        r.auctionId=a[0].GetUInt32();r.houseId=a[1].GetUInt32();r.itemGuid=a[2].GetUInt32();r.itemEntry=a[3].GetUInt32();
        r.itemCount=a[4].GetUInt32();r.startBid=a[5].GetUInt32();r.buyout=a[6].GetUInt32();r.bidder=a[7].GetUInt32();
        r.bid=a[8].GetUInt32();r.deposit=a[9].GetUInt32();r.expiresAt=a[10].GetUInt64();r.payoutAt=a[11].GetUInt64();
        if(r.payoutAt)
        {
            AuctionEntry* pending=sAuctionMgr.FindAuction(r.auctionId);
            if(!pending||pending->owner!=guid||pending->itemGuidLow||pending->moneyDeliveryTime!=time_t(r.payoutAt))
            {ReleaseOfflineLease(guid);workLeaseGuid=0;workLifecycleGuid=0;nextWorkAt=now+5;return;}
            r.cut=pending->GetAuctionCut();
        }
        sRandomBotEstateAuctionMgr.FenceAdoption(r.auctionId);
        workResult=RandomBotEstateStore::AdoptAuction(r);workPending=true;return;
    }
    auto bid=CharacterDatabase.PQuery("SELECT id,lastbid,itemguid,item_template,item_count FROM auction WHERE buyguid=%u AND moneyTime=0 ORDER BY id LIMIT 1",guid);
    if(bid)
    {
        if(!AcquireOfflineLease(guid)){nextWorkAt=now+5;return;}
        workLeaseGuid=guid;
        workLifecycleGuid=guid;
        const uint32 faction=Player::TeamForRace(race)==HORDE?1:0;RandomBotEstateBidIntake r;r.estateId=estate;
        r.originalGuid=guid;r.bidderBrokerGuid=sRandomBotEstateService.BidderBroker(faction);r.auctionId=bid->Fetch()[0].GetUInt32();
        r.bid=bid->Fetch()[1].GetUInt32();r.itemGuid=bid->Fetch()[2].GetUInt32();r.itemEntry=bid->Fetch()[3].GetUInt32();
        r.itemCount=bid->Fetch()[4].GetUInt32();sRandomBotEstateAuctionMgr.FenceAdoption(r.auctionId);
        workResult=RandomBotEstateStore::AdoptBid(r);workPending=true;return;
    }
    auto item=CharacterDatabase.PQuery("SELECT i.guid,i.itemEntry,i.count FROM character_inventory v INNER JOIN item_instance i ON i.guid=v.item "
        "WHERE v.guid=%u AND NOT EXISTS (SELECT 1 FROM character_inventory child WHERE child.guid=v.guid AND child.bag=i.guid) ORDER BY i.guid LIMIT 1",guid);
    if(item)
    {
        if(!AcquireOfflineLease(guid)){nextWorkAt=now+5;return;}
        workLeaseGuid=guid;
        workLifecycleGuid=guid;
        RandomBotEstateIntakeLot r;r.estateId=estate;r.originalGuid=guid;r.originalAccount=account;r.brokerGuid=broker;
        r.brokerAccount=brokerAccount;r.itemGuid=item->Fetch()[0].GetUInt32();r.itemEntry=item->Fetch()[1].GetUInt32();
        r.itemCount=item->Fetch()[2].GetUInt32();r.eligibleAccounts=accounts;workResult=RandomBotEstateStore::TransferInventoryLot(r);
        workPending=true;return;
    }
    auto blockedMail=CharacterDatabase.PQuery("SELECT COUNT(*) FROM mail WHERE receiver=%u AND cod<>0 AND has_items<>0",guid);
    if(!blockedMail){nextWorkAt=now+30;return;}
    if(blockedMail->Fetch()[0].GetUInt32())
    {
        sLog.outError("Random bot %u retirement intake is waiting for %u COD mail(s); assets will not be discarded.",
            guid,blockedMail->Fetch()[0].GetUInt32());
        CharacterDatabase.PExecute("UPDATE ai_playerbot_lifecycle SET updated_at=UNIX_TIMESTAMP()+60 WHERE guid=%u AND status=2",guid);
        nextWorkAt=now+1;return;
    }
    if(!AcquireOfflineLease(guid)){nextWorkAt=now+5;return;}
    workLeaseGuid=guid;
    workLifecycleGuid=guid;
    workSuccessMessage="Random bot "+std::to_string(guid)+" retirement assets detached into estate "+std::to_string(estate)+".";
    workResult=RandomBotEstateStore::FinalizeIntake(estate,guid,account);workPending=true;
}
