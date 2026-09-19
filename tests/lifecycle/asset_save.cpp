// Scripted DB boundary tests, not real Item/Player serialization or MySQL tests.
#include "RandomBotLifecycleStore.h"
#include "Database/DatabaseEnv.h"
#include <cassert>

TestDatabase CharacterDatabase;

RandomBotAssetSave Request()
{
    RandomBotAssetSave r;
    r.guid = 100; r.account = 200; r.assetGuid = 900;
    r.itemEntry = 400; r.itemCount = 2; r.moneyBefore = 50;
    r.proceeds = 120; r.eligibleAccounts = {200};
    return r;
}

void Setup()
{
    CharacterDatabase.connection = {};
    auto& q = CharacterDatabase.connection.queries;
    q.push_back({"information_schema.tables", {{9}}});
    q.push_back({"SELECT account,status", {{200}, {3}, {0}, {0}, {0}, {0}}});
    q.push_back({"SELECT account,money,online", {{200}, {50}, {0}}});
    q.push_back({"SELECT COUNT(*) FROM auction", {{0}}});
    q.push_back({"SELECT COUNT(*) FROM ai_playerbot_lifecycle_operation", {{0}}});
    q.push_back({"SELECT i.itemEntry,i.count", {{400}, {2}}});
    q.push_back({"SELECT (SELECT COUNT(*) FROM item_instance", {{0}}});
    q.push_back({"SELECT money FROM characters", {{50}}});
    CharacterDatabase.queuedSave = [](SqlConnection& c)
    {
        return c.Execute("core serialized item removal");
    };
}

int main()
{
    for (unsigned fail = 0; fail <= 3; ++fail)
    {
        Setup();
        auto& db = CharacterDatabase.connection;
        db.failExecute = fail;
        assert(RandomBotLifecycleStore::CommitAssetSave(Request()).get() == (fail == 0));
        assert(!db.mismatch);
        assert(db.durable.size() == (fail ? 0u : 3u));
    }
    Setup();
    CharacterDatabase.connection.loseCommitAck = true;
    assert(!RandomBotLifecycleStore::CommitAssetSave(Request()).get());
    assert(CharacterDatabase.connection.durable.size() == 3);
    for (unsigned receipt = 0; receipt <= 1; ++receipt)
        for (unsigned remaining = 0; remaining <= 1; ++remaining)
        {
            CharacterDatabase.connection = {};
            auto& db = CharacterDatabase.connection;
            db.queries.push_back({"SELECT COUNT(*) FROM ai_playerbot_lifecycle_operation o", {{receipt}}});
            if (receipt)
                db.queries.push_back({"SELECT (SELECT COUNT(*) FROM item_instance", {{remaining}}});
            assert(RandomBotLifecycleStore::ConfirmAssetSave(Request()).get() == (receipt && !remaining));
            assert(!db.mismatch && db.durable.empty());
        }
    // Online, owner/bidder obligations, duplicate operation, incomplete removal.
    for (unsigned index : {2u, 3u, 4u, 6u})
    {
        Setup();
        auto& db = CharacterDatabase.connection;
        db.queries[index].row[index == 2 ? 2 : 0].value = 1;
        assert(!RandomBotLifecycleStore::CommitAssetSave(Request()).get());
        assert(!db.mismatch && db.durable.empty());
    }
    Setup();
    CharacterDatabase.connection.queries[3].fail = true;
    assert(!RandomBotLifecycleStore::CommitAssetSave(Request()).get());
    assert(CharacterDatabase.connection.durable.empty());
    Setup();
    auto foreign = Request(); foreign.account = 999;
    assert(!RandomBotLifecycleStore::CommitAssetSave(foreign).get());
    assert(CharacterDatabase.connection.durable.empty());
}
