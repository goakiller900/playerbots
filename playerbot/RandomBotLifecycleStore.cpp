#include "RandomBotLifecycleStore.h"
#include "RandomBotLifecycle.h"
#include "RandomBotEstateService.h"
#include "Database/DatabaseEnv.h"
#include "Database/DatabaseImpl.h"

#include <algorithm>
#include <limits>
#include <sstream>

namespace
{
#ifdef CMANGOS_ASYNC_TRANSACTION_CALLBACK
    bool HasTransactionalTables(SqlConnection& connection)
    {
        // Never convert an operator's tables implicitly. Existing installations
        // may differ from the pinned core's InnoDB base schema.
        auto result = connection.Query(
            "SELECT COUNT(*) FROM information_schema.tables WHERE table_schema=DATABASE() "
            "AND table_name IN ('characters','character_inventory','item_instance','mail','mail_items','auction','ai_playerbot_lifecycle',"
            "'ai_playerbot_lifecycle_inheritance') AND engine='InnoDB'");
        return result && result->Fetch()[0].GetUInt32() == 8;
    }
#endif

    std::string Accounts(const std::vector<uint32>& accounts)
    {
        std::ostringstream sql;
        for (uint32 account : accounts)
        {
            if (sql.tellp() > 0)
                sql << ',';
            sql << account;
        }
        return accounts.empty() ? "0" : sql.str();
    }

    std::future<bool> Unsupported()
    {
        std::promise<bool> promise;
        promise.set_value(false);
        return promise.get_future();
    }
}

bool RandomBotLifecycleStore::Supported()
{
#if defined(CMANGOS_ASYNC_TRANSACTION_CALLBACK) && CMANGOS_ASYNC_TRANSACTION_CALLBACK >= 3
    return true;
#else
    return false;
#endif
}

std::future<bool> RandomBotLifecycleStore::PrepareEstate(RandomBotEstateRequest request)
{
    request.eligibleAccounts.erase(std::remove_if(request.eligibleAccounts.begin(), request.eligibleAccounts.end(),
        [](uint32 account) { return sRandomBotEstateService.IsServiceAccount(account); }), request.eligibleAccounts.end());
#ifndef CMANGOS_ASYNC_TRANSACTION_CALLBACK
    return Unsupported();
#else
    return CharacterDatabase.QueueTransaction([request](SqlConnection& connection)
    {
        if (!HasTransactionalTables(connection) || request.sinkPercent > 100 ||
            request.recipientCount > 1000 || !request.moneyCap)
            return false;
        const std::string guid = std::to_string(request.guid);
        const std::string accounts = Accounts(request.eligibleAccounts);
        // These queries use the SAME connection as BEGIN, writes, and COMMIT.
        auto estate = connection.Query(("SELECT status,estate_prepared,vendor_income,estate_escrow,account "
            "FROM ai_playerbot_lifecycle WHERE guid=" + guid + " FOR UPDATE").c_str());
        if (!estate)
            return false;
        Field* fields = estate->Fetch();
        if (fields[4].GetUInt32() != request.account ||
            std::find(request.eligibleAccounts.begin(), request.eligibleAccounts.end(), request.account) ==
                request.eligibleAccounts.end())
            return false;
        if (fields[1].GetBool())
            return true; // Committed previously; never regenerate recipient shares.
        if (fields[0].GetUInt32() != uint32(RandomBotLifecycleStatus::DISTRIBUTING_ESTATE))
            return false;

        auto character = connection.Query(("SELECT money,online,account FROM characters WHERE guid=" +
            guid + " FOR UPDATE").c_str());
        if (!character || character->Fetch()[1].GetBool() || character->Fetch()[2].GetUInt32() != request.account)
            return false;
        auto unsettled = connection.Query(("SELECT (SELECT COUNT(*) FROM mail WHERE receiver=" + guid +
            " AND (money<>0 OR has_items<>0 OR cod<>0)) + (SELECT COUNT(*) FROM auction WHERE itemowner=" +
            guid + " OR buyguid=" + guid + ") + (SELECT COUNT(*) FROM character_inventory WHERE guid=" + guid +
            ") + (SELECT COUNT(*) FROM mail_items WHERE receiver=" + guid +
            ") + (SELECT COUNT(*) FROM item_instance WHERE owner_guid=" + guid + ")").c_str());
        if (!unsettled || unsettled->Fetch()[0].GetUInt64())
            return false;

        const uint64 cash = character->Fetch()[0].GetUInt64();
        const uint64 escrow = fields[3].GetUInt64();
        if (cash > std::numeric_limits<uint64>::max() - escrow)
            return false;
        const uint64 finalEstate = cash + escrow;
        uint64 sink = RandomBotLifecycleMath::CalculateGoldSink(finalEstate,
            fields[2].GetUInt64(), request.sinkPercent);

        std::vector<uint32> recipients;
        std::vector<uint64> caps;
        if (request.recipientCount && finalEstate > sink)
        {
            // No money changes here. Delivery locks and revalidates each recipient
            // under a separate offline lease, and sinks any cap/eligibility excess.
            auto result = connection.Query(("SELECT eligible.guid FROM (SELECT 1) seed LEFT JOIN "
                "(SELECT c.guid FROM characters c LEFT JOIN ai_playerbot_lifecycle l "
                "ON l.guid=c.guid WHERE c.account IN (" + accounts + ") AND c.guid<>" + guid +
                " AND (l.guid IS NULL OR l.status IN (0,1)) ORDER BY RAND() LIMIT " +
                std::to_string(request.recipientCount) + ") eligible ON 1=1").c_str());
            if (!result)
                return false; // Query failure must never be interpreted as an empty pool.
            do
            {
                if (!result->Fetch()[0].IsNULL())
                {
                    recipients.push_back(result->Fetch()[0].GetUInt32());
                    caps.push_back(std::min(request.moneyCap,
                        request.recipientCap ? request.recipientCap : request.moneyCap));
                }
            } while (result->NextRow());
        }
        const auto shares = RandomBotLifecycleMath::CalculateInheritanceShares(finalEstate - sink, caps, request.weights);
        sink += shares.remainder;
        for (size_t i = 0; i < recipients.size(); ++i)
        {
            if (!shares.shares[i])
                continue;
            if (!connection.Execute(("INSERT INTO ai_playerbot_lifecycle_inheritance "
                "(retiring_guid,recipient_guid,amount,delivered,created_at,updated_at) VALUES (" + guid + ',' +
                std::to_string(recipients[i]) + ',' + std::to_string(shares.shares[i]) +
                ",0,UNIX_TIMESTAMP(),UNIX_TIMESTAMP())").c_str()))
                return false;
        }
        if (!connection.Execute(("UPDATE characters SET money=0 WHERE guid=" + guid).c_str()))
            return false;
        return connection.Execute(("UPDATE ai_playerbot_lifecycle SET estate_prepared=1,estate_final=" +
            std::to_string(finalEstate) + ",sink_percent=" + std::to_string(request.sinkPercent) +
            ",gold_sunk=" + std::to_string(sink) + ",inheritance_planned=" + std::to_string(shares.distributed) +
            ",updated_at=UNIX_TIMESTAMP() WHERE guid=" + guid).c_str());
    });
#endif
}

std::future<bool> RandomBotLifecycleStore::DeliverInheritance(uint32 retiringGuid, uint32 recipientGuid,
    uint64 moneyCap, std::vector<uint32> eligibleAccounts)
{
    eligibleAccounts.erase(std::remove_if(eligibleAccounts.begin(), eligibleAccounts.end(),
        [](uint32 account) { return sRandomBotEstateService.IsServiceAccount(account); }), eligibleAccounts.end());
#ifndef CMANGOS_ASYNC_TRANSACTION_CALLBACK
    return Unsupported();
#else
    return CharacterDatabase.QueueTransaction([retiringGuid, recipientGuid, moneyCap, eligibleAccounts](SqlConnection& connection)
    {
        if (!HasTransactionalTables(connection) || !moneyCap)
            return false;
        const std::string donor = std::to_string(retiringGuid);
        const std::string recipient = std::to_string(recipientGuid);
        const std::string ledgerKey = "retiring_guid=" + donor + " AND recipient_guid=" + recipient;
        auto estate = connection.Query(("SELECT estate_prepared,account,status FROM ai_playerbot_lifecycle WHERE guid=" +
            donor + " FOR UPDATE").c_str());
        if (!estate || !estate->Fetch()[0].GetBool() ||
            estate->Fetch()[2].GetUInt32() != uint32(RandomBotLifecycleStatus::DISTRIBUTING_ESTATE) ||
            std::find(eligibleAccounts.begin(), eligibleAccounts.end(), estate->Fetch()[1].GetUInt32()) == eligibleAccounts.end())
            return false;
        auto ledger = connection.Query(("SELECT amount,delivered FROM ai_playerbot_lifecycle_inheritance WHERE " +
            ledgerKey + " FOR UPDATE").c_str());
        if (!ledger)
            return false;
        if (ledger->Fetch()[1].GetBool())
            return true;

        const uint64 amount = ledger->Fetch()[0].GetUInt64();
        auto exists = connection.Query(("SELECT COUNT(*) FROM characters WHERE guid=" + recipient).c_str());
        if (!exists)
            return false;
        uint64 delivered = 0;
        if (exists->Fetch()[0].GetUInt32())
        {
            auto character = connection.Query(("SELECT money,online,account FROM characters WHERE guid=" +
                recipient + " FOR UPDATE").c_str());
            if (!character || character->Fetch()[1].GetBool())
                return false;
            Field* fields = character->Fetch();
            auto state = connection.Query(("SELECT COUNT(*) FROM ai_playerbot_lifecycle WHERE guid=" +
                recipient + " AND status>=2").c_str());
            if (!state)
                return false;
            const bool eligible = recipientGuid != retiringGuid && !state->Fetch()[0].GetUInt32() &&
                std::find(eligibleAccounts.begin(), eligibleAccounts.end(), fields[2].GetUInt32()) != eligibleAccounts.end();
            const uint64 current = fields[0].GetUInt64();
            delivered = eligible && current < moneyCap ? std::min(amount, moneyCap - current) : 0;
            if (delivered && !connection.Execute(("UPDATE characters SET money=money+" +
                std::to_string(delivered) + " WHERE guid=" + recipient).c_str()))
                return false;
        }
        if (!connection.Execute(("UPDATE ai_playerbot_lifecycle_inheritance SET delivered=1,delivered_amount=" +
            std::to_string(delivered) + ",updated_at=UNIX_TIMESTAMP() WHERE " + ledgerKey).c_str()))
            return false;
        return connection.Execute(("UPDATE ai_playerbot_lifecycle SET gold_distributed=gold_distributed+" +
            std::to_string(delivered) + ",gold_sunk=gold_sunk+" + std::to_string(amount - delivered) +
            ",updated_at=UNIX_TIMESTAMP() WHERE guid=" + donor).c_str());
    });
#endif
}
