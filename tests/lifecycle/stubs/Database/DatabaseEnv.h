#pragma once
#include "Common.h"
#include "Database/AcknowledgedTransaction.h"
#include <deque>
#include <future>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#define CMANGOS_ASYNC_TRANSACTION_CALLBACK 4

struct Field
{
    uint64 value;
    std::string text = {};
    uint64 GetUInt64() const { return value; }
    uint32 GetUInt32() const { return static_cast<uint32>(value); }
    int32 GetInt32() const { return static_cast<int32>(value); }
    bool GetBool() const { return value != 0; }
    std::string GetCppString() const { return text; }
};
struct QueryResult
{
    std::vector<Field> fields;
    Field* Fetch() { return fields.data(); }
};
struct ExpectedQuery
{
    std::string fragment;
    std::vector<Field> row;
    bool fail = false;
};
struct SqlConnection
{
    std::deque<ExpectedQuery> queries;
    std::vector<std::string> staged, durable;
    unsigned executeCount = 0, failExecute = 0;
    bool loseCommitAck = false;
    bool mismatch = false;
    bool BeginTransaction() { staged.clear(); executeCount = 0; return true; }
    bool CommitTransaction()
    {
        durable.insert(durable.end(), staged.begin(), staged.end());
        staged.clear();
        if (loseCommitAck)
            throw std::runtime_error("lost commit acknowledgement");
        return true;
    }
    bool RollbackTransaction() { staged.clear(); return true; }
    std::unique_ptr<QueryResult> Query(const char* sql)
    {
        if (queries.empty() || std::string(sql).find(queries.front().fragment) == std::string::npos)
        {
            mismatch = true;
            throw std::runtime_error("unexpected production query");
        }
        const auto expected = queries.front();
        queries.pop_front();
        if (expected.fail)
            return nullptr;
        auto result = std::make_unique<QueryResult>();
        result->fields = expected.row;
        return result;
    }
    bool Execute(const char* sql)
    {
        if (++executeCount == failExecute)
            return false;
        staged.emplace_back(sql);
        return true;
    }
};
struct TestDatabase
{
    SqlConnection connection;
    std::function<bool(SqlConnection&)> queuedSave;
    template<class Body> std::future<bool> QueueTransaction(Body body)
    {
        std::promise<bool> result;
        auto future = result.get_future();
        result.set_value(RunAcknowledgedTransaction(connection, body));
        return future;
    }
    template<class Before, class After>
    std::future<bool> CommitTransactionAcknowledged(Before before, After after)
    {
        return QueueTransaction([&](SqlConnection& c)
        {
            return before(c) && queuedSave && queuedSave(c) && after(c);
        });
    }
};
extern TestDatabase CharacterDatabase;
