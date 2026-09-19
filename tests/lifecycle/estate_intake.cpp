#include "RandomBotEstate.h"
#include "RandomBotEstateStore.h"
#include "Database/DatabaseEnv.h"
#include <cassert>

TestDatabase CharacterDatabase;

RandomBotEstateIntakeLot Request()
{
    RandomBotEstateIntakeLot r;
    r.estateId = 1; r.originalGuid = 100; r.originalAccount = 200;
    r.brokerGuid = 101; r.brokerAccount = 201;
    r.itemGuid = 900; r.itemEntry = 400; r.itemCount = 2;
    r.eligibleAccounts = {200};
    return r;
}

void Setup()
{
    CharacterDatabase.connection = {};
    auto& q = CharacterDatabase.connection.queries;
    q.push_back({"information_schema.tables", {{8}}});
    q.push_back({"SELECT original_guid", {{100}, {200}, {101}, {201}, {0}, {0}}});
    q.push_back({"SELECT COUNT(*) FROM ai_playerbot_estate_operation", {{0}}});
    q.push_back({"SELECT account,status FROM ai_playerbot_lifecycle", {{200}, {2}}});
    q.push_back({"SELECT account,online", {{200}, {0}}});
    q.push_back({"SELECT c.account,c.online,b.enabled", {{201}, {0}, {1}}});
    q.push_back({"SELECT i.itemEntry,i.count", {{400}, {2}}});
    q.push_back({"SELECT (SELECT COUNT(*) FROM character_inventory WHERE bag=", {{0}}});
}

int main()
{
    using S = RandomBotEstateStatus;
    assert(RandomBotEstateRules::CanFinalizeCharacter(S::ESCROWED, true, false, false));
    assert(RandomBotEstateRules::CanFinalizeCharacter(S::LIQUIDATING, true, false, false));
    assert(!RandomBotEstateRules::CanFinalizeCharacter(S::INTAKE, true, false, false));
    assert(!RandomBotEstateRules::CanFinalizeCharacter(S::HELD, true, false, false));
    assert(!RandomBotEstateRules::CanFinalizeCharacter(S::ESCROWED, false, false, false));
    assert(!RandomBotEstateRules::CanFinalizeCharacter(S::ESCROWED, true, true, false));
    assert(!RandomBotEstateRules::CanFinalizeCharacter(S::ESCROWED, true, false, true));
    assert(RandomBotEstateRules::CanDistribute(S::LIQUIDATING, 0, 0, 0, false));
    assert(!RandomBotEstateRules::CanDistribute(S::LIQUIDATING, 0, 1, 0, false));
    assert(!RandomBotEstateRules::CanDistribute(S::LIQUIDATING, 0, 0, 0, true));
    assert(!RandomBotEstateRules::CanTransition(S::INTAKE, S::COMPLETE));
    assert(!RandomBotEstateRules::CanTransition(S::HELD, S::COMPLETE));
    assert(RandomBotEstateRules::CanTransition(S::INTAKE, S::ESCROWED));
    using L = RandomBotEstateLotStatus;
    assert(RandomBotEstateRules::CanTransitionLot(L::LISTED, L::RETURNED));
    assert(RandomBotEstateRules::CanTransitionLot(L::RETURNED, L::READY));
    assert(!RandomBotEstateRules::CanTransitionLot(L::LISTED, L::VENDORED));
    for (unsigned fail = 0; fail <= 4; ++fail)
    {
        Setup();
        auto& db = CharacterDatabase.connection;
        db.failExecute = fail;
        assert(RandomBotEstateStore::TransferInventoryLot(Request()).get() == (fail == 0));
        assert(!db.mismatch && db.durable.size() == (fail ? 0u : 4u));
    }
    Setup();
    CharacterDatabase.connection.loseCommitAck = true;
    assert(!RandomBotEstateStore::TransferInventoryLot(Request()).get());
    assert(CharacterDatabase.connection.durable.size() == 4);
    for (unsigned postcondition = 0; postcondition <= 1; ++postcondition)
    {
        Setup();
        auto& db = CharacterDatabase.connection;
        db.queries.resize(3);
        db.queries[2].row = {{1}};
        db.queries.push_back({"SELECT COUNT(*) FROM ai_playerbot_estate_lot l", {{postcondition}}});
        assert(RandomBotEstateStore::TransferInventoryLot(Request()).get() == bool(postcondition));
        assert(!db.mismatch && db.durable.empty());
    }
    Setup();
    CharacterDatabase.connection.queries[7].row = {{1}}; // Nonempty bag/gift/loot/mail/AH/other lot.
    assert(!RandomBotEstateStore::TransferInventoryLot(Request()).get());
    assert(CharacterDatabase.connection.durable.empty());
    Setup();
    CharacterDatabase.connection.queries[4].row[1].value = 1; // Online donor.
    assert(!RandomBotEstateStore::TransferInventoryLot(Request()).get());
    Setup();
    CharacterDatabase.connection.queries[5].row[1].value = 1; // Online service identity.
    assert(!RandomBotEstateStore::TransferInventoryLot(Request()).get());
    Setup();
    auto invalid = Request(); invalid.eligibleAccounts.push_back(invalid.brokerAccount);
    assert(!RandomBotEstateStore::TransferInventoryLot(invalid).get());
    assert(CharacterDatabase.connection.durable.empty());
}
