#include "Database/AcknowledgedTransaction.h"
#include <cassert>
#include <stdexcept>
#include <string>

struct FakeConnection
{
    bool begin = true;
    bool commit = true;
    bool rollbackThrows = false;
    std::string calls;
    bool BeginTransaction() { calls += 'B'; return begin; }
    bool CommitTransaction() { calls += 'C'; return commit; }
    bool RollbackTransaction()
    {
        calls += 'R';
        if (rollbackThrows)
            throw std::runtime_error("disconnected rollback");
        return true;
    }
};

struct AssetState
{
    bool itemExists = true;
    bool receipt = false;
    unsigned escrow = 0;
};

// Tests the production transaction runner, not MySQL/core serialization.
struct AssetConnection
{
    AssetState durable;
    AssetState staged;
    unsigned fault = 0;
    bool BeginTransaction() { staged = durable; return fault != 1; }
    bool CommitTransaction()
    {
        if (fault == 4)
            return false;
        durable = staged;
        if (fault == 5)
            throw std::runtime_error("commit succeeded but acknowledgement was lost");
        return true;
    }
    bool RollbackTransaction() { staged = durable; return true; }
};

void CheckAssetRollback()
{
    auto sell = [](AssetConnection& c)
    {
        if (c.staged.receipt)
            return false; // Caller reconciles the existing receipt, never replays.
        c.staged.itemExists = false;
        if (c.fault == 2)
            return false;
        c.staged.escrow += 100;
        c.staged.receipt = true;
        return c.fault != 3;
    };
    for (unsigned fault = 0; fault <= 5; ++fault)
    {
        AssetConnection c;
        c.fault = fault;
        const bool acknowledged = RunAcknowledgedTransaction(c, sell);
        assert(acknowledged == (fault == 0));
        const bool committed = fault == 0 || fault == 5;
        assert(c.durable.receipt == committed);
        assert(c.durable.itemExists != committed);
        assert(c.durable.escrow == (committed ? 100u : 0u));
        c.fault = 0;
        assert(RunAcknowledgedTransaction(c, sell) == !committed);
        assert(c.durable.receipt && !c.durable.itemExists && c.durable.escrow == 100);
    }
}

int main()
{
    CheckAssetRollback();
    auto save = [](FakeConnection& c) { c.calls += 'S'; return true; };
    FakeConnection success;
    assert(RunAcknowledgedTransaction(success, save));
    assert(success.calls == "BSC");

    FakeConnection beginFailure;
    beginFailure.begin = false;
    assert(!RunAcknowledgedTransaction(beginFailure, save));
    assert(beginFailure.calls == "B"); // Never run writes outside BEGIN.

    FakeConnection statementFailure;
    assert(!RunAcknowledgedTransaction(statementFailure,
        [](FakeConnection& c) { c.calls += 'S'; return false; }));
    assert(statementFailure.calls == "BSR");

    FakeConnection commitFailure;
    commitFailure.commit = false;
    assert(!RunAcknowledgedTransaction(commitFailure, save));
    assert(commitFailure.calls == "BSCR");

    FakeConnection exception;
    exception.rollbackThrows = true;
    assert(!RunAcknowledgedTransaction(exception, [](FakeConnection& c) -> bool
        { c.calls += 'S'; throw std::runtime_error("lost connection"); }));
    assert(exception.calls == "BSR");
}
