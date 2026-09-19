#include "RandomBotEstateStore.h"
#include "Database/DatabaseEnv.h"
#include <cassert>

TestDatabase CharacterDatabase;

RandomBotEstateLiquidation Request()
{
    RandomBotEstateLiquidation r;
    r.estateId = 1; r.lotId = 2; r.brokerGuid = 101; r.brokerAccount = 201;
    r.itemGuid = 900; r.itemEntry = 400; r.itemCount = 2;
    r.proceeds = 120; r.charges = "-1 0 0 0 0 ";
    return r;
}

void Setup()
{
    CharacterDatabase.connection = {};
    auto& q = CharacterDatabase.connection.queries;
    q.push_back({"information_schema.tables", {{9}}});
    q.push_back({"SELECT broker_guid,broker_account", {{101}, {201}, {2}, {1}, {0}, {0}}});
    q.push_back({"SELECT c.account,c.online,b.enabled", {{201}, {0}, {1}}});
    q.push_back({"SELECT i.itemEntry,i.count,i.charges", {{400}, {2}, {0, "-1 0 0 0 0 "}, {0}}});
    q.push_back({"SELECT (SELECT COUNT(*) FROM character_inventory", {{0}}});
    q.push_back({"SELECT COUNT(*) FROM item_instance", {{0}}});
    CharacterDatabase.queuedSave = [](SqlConnection& c) { return c.Execute("core Item::SaveToDB removal"); };
}

int main()
{
    for (unsigned fail = 0; fail <= 4; ++fail)
    {
        Setup();
        auto& db = CharacterDatabase.connection;
        db.failExecute = fail;
        assert(RandomBotEstateStore::CommitLotLiquidation(Request()).get() == (fail == 0));
        assert(!db.mismatch && db.durable.size() == (fail ? 0u : 4u));
        if (!fail)
            assert(db.durable[2].find("escrow=escrow+120,vendor_income=vendor_income+120") != std::string::npos);
    }
    Setup();
    CharacterDatabase.connection.loseCommitAck = true;
    assert(!RandomBotEstateStore::CommitLotLiquidation(Request()).get());
    assert(CharacterDatabase.connection.durable.size() == 4);
    for (unsigned remaining = 0; remaining <= 1; ++remaining)
    {
        CharacterDatabase.connection = {};
        auto& db = CharacterDatabase.connection;
        db.queries.push_back({"SELECT COUNT(*) FROM ai_playerbot_estate_operation o", {{1}}});
        db.queries.push_back({"SELECT COUNT(*) FROM item_instance", {{remaining}}});
        assert(RandomBotEstateStore::ConfirmLotLiquidation(Request()).get() == !remaining);
        assert(!db.mismatch && db.durable.empty());
    }
    Setup();
    CharacterDatabase.connection.queries[3].row[2].text = "0 0 0 0 0 "; // Stale charges invalidate quote.
    assert(!RandomBotEstateStore::CommitLotLiquidation(Request()).get());
    assert(CharacterDatabase.connection.durable.empty());
    Setup();
    CharacterDatabase.connection.queries[3].row[3].value = 8; // Wrapped contents.
    assert(!RandomBotEstateStore::CommitLotLiquidation(Request()).get());
    Setup();
    CharacterDatabase.connection.queries[1].row[4].value = ~uint64(0); // Escrow overflow.
    assert(!RandomBotEstateStore::CommitLotLiquidation(Request()).get());
    Setup();
    auto destroy = Request(); destroy.destroy = true; destroy.proceeds = 0;
    assert(!RandomBotEstateStore::CommitLotLiquidation(destroy).get());
    assert(CharacterDatabase.connection.durable.empty());
    Setup();
    destroy.destroyAllowed = true;
    assert(RandomBotEstateStore::CommitLotLiquidation(destroy).get());
    assert(CharacterDatabase.connection.durable[2].find("escrow=escrow+0,vendor_income=vendor_income+0") != std::string::npos);
}
