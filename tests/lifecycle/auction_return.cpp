// Production return store with a scripted connection and queued MailDraft-save
// double. Real core serialization and SQL semantics still require LAB02.
#include "RandomBotLifecycleStore.h"
#include "Database/DatabaseEnv.h"
#include <cassert>

TestDatabase CharacterDatabase;

RandomBotLifecycleStore::AuctionReturn Request()
{
    RandomBotLifecycleStore::AuctionReturn request;
    request.auctionId = 77;
    request.owner = 100;
    request.account = 200;
    request.itemGuid = 900;
    request.itemEntry = 400;
    request.itemCount = 2;
    request.mailId = 800;
    request.expiresAt = 500;
    request.eligibleAccounts = {200};
    return request;
}

void Setup()
{
    CharacterDatabase.connection = {};
    auto& q = CharacterDatabase.connection.queries;
    q.push_back({"information_schema.tables", {{7}}});
    q.push_back({"SELECT account,status", {{200}, {4}, {0}}});
    q.push_back({"SELECT account,online", {{200}, {0}}});
    q.push_back({"SELECT COUNT(*) FROM ai_playerbot_lifecycle_auction_return", {{0}}});
    q.push_back({"SELECT itemowner,itemguid", {{100}, {900}, {400}, {2}, {500}, {0}, {0}, {1}}});
    q.push_back({"SELECT COUNT(*) FROM mail m", {{1}}});
    CharacterDatabase.queuedSave = [](SqlConnection& c)
    {
        return c.Execute("INSERT INTO mail (test serialized save)") &&
            c.Execute("INSERT INTO mail_items (test serialized save)");
    };
}

int main()
{
    for (unsigned fail = 0; fail <= 4; ++fail)
    {
        Setup();
        auto& db = CharacterDatabase.connection;
        db.failExecute = fail;
        assert(RandomBotLifecycleStore::CommitAuctionReturn(Request()).get() == (fail == 0));
        assert(!db.mismatch);
        assert(db.durable.size() == (fail == 0 ? 4u : 0u));
        if (!fail)
        {
            assert(db.durable[0].find("INSERT INTO ai_playerbot_lifecycle_auction_return") == 0);
            assert(db.durable[3] == "DELETE FROM auction WHERE id=77");
        }
    }
    Setup();
    CharacterDatabase.connection.loseCommitAck = true;
    assert(!RandomBotLifecycleStore::CommitAuctionReturn(Request()).get());
    assert(CharacterDatabase.connection.durable.size() == 4);
    CharacterDatabase.connection = {};
    auto& db = CharacterDatabase.connection;
    db.queries.push_back({"SELECT COUNT(*) FROM ai_playerbot_lifecycle_auction_return", {{1}}});
    db.queries.push_back({"SELECT COUNT(*) FROM auction", {{0}}});
    db.queries.push_back({"SELECT COUNT(*) FROM mail m", {{1}}});
    assert(RandomBotLifecycleStore::ConfirmAuctionReturn(Request()).get());
    assert(db.durable.empty() && db.queries.empty() && !db.mismatch);
    Setup();
    db.queries[3].row = {{1}}; // Receipt means never replay mail inserts.
    assert(!RandomBotLifecycleStore::CommitAuctionReturn(Request()).get());
    assert(db.durable.empty());
    Setup();
    db.queries[4].row[5].value = 100; // Bidder obligation: not an expiry return.
    assert(!RandomBotLifecycleStore::CommitAuctionReturn(Request()).get());
    Setup();
    db.queries.back().row = {{0}}; // Serializer didn't produce the expected attachment.
    assert(!RandomBotLifecycleStore::CommitAuctionReturn(Request()).get());
    assert(db.durable.empty());
    Setup();
    db.queries[2].row[1].value = 1; // Online receiver must not get an invisible mailbox update.
    assert(!RandomBotLifecycleStore::CommitAuctionReturn(Request()).get());
    assert(db.durable.empty());
}
