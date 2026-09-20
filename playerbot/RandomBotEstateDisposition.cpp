#include "RandomBotEstateStore.h"
#include "Database/DatabaseEnv.h"
namespace
{
bool Detached(SqlConnection&c,uint64 estate,uint32 guid,uint32 account)
{auto e=c.Query(("SELECT COUNT(*) FROM ai_playerbot_estate WHERE estate_id="+std::to_string(estate)+" AND original_guid="+
 std::to_string(guid)+" AND original_account="+std::to_string(account)+" AND assets_detached=1 AND status>=2").c_str());
 return e&&e->Fetch()[0].GetUInt32()==1;}
}
std::future<bool> RandomBotEstateStore::FinalizeArchive(uint64 estate,uint32 guid,uint32 account)
{
#if !defined(CMANGOS_ASYNC_TRANSACTION_CALLBACK) || CMANGOS_ASYNC_TRANSACTION_CALLBACK < 4
 std::promise<bool>p;auto f=p.get_future();p.set_value(false);return f;
#else
 return CharacterDatabase.QueueTransaction([estate,guid,account](SqlConnection&c){if(!Detached(c,estate,guid,account))return false;
  return c.Execute(("UPDATE ai_playerbot_lifecycle SET status=7,disposition_completed=1,retired_at=UNIX_TIMESTAMP(),updated_at=UNIX_TIMESTAMP() WHERE guid="+
   std::to_string(guid)+" AND account="+std::to_string(account)+" AND status=10 AND disposition=0").c_str());});
#endif
}
std::future<bool> RandomBotEstateStore::MarkDeletePending(uint64 estate,uint32 guid,uint32 account)
{
#if !defined(CMANGOS_ASYNC_TRANSACTION_CALLBACK) || CMANGOS_ASYNC_TRANSACTION_CALLBACK < 4
 std::promise<bool>p;auto f=p.get_future();p.set_value(false);return f;
#else
 return CharacterDatabase.QueueTransaction([estate,guid,account](SqlConnection&c){if(!Detached(c,estate,guid,account))return false;
  return c.Execute(("UPDATE ai_playerbot_lifecycle SET status=8,updated_at=UNIX_TIMESTAMP() WHERE guid="+std::to_string(guid)+
   " AND account="+std::to_string(account)+" AND status=10 AND disposition=1").c_str());});
#endif
}
std::future<bool> RandomBotEstateStore::ConfirmDeleted(uint32 guid,uint32 account)
{
#if !defined(CMANGOS_ASYNC_TRANSACTION_CALLBACK) || CMANGOS_ASYNC_TRANSACTION_CALLBACK < 4
 std::promise<bool>p;auto f=p.get_future();p.set_value(false);return f;
#else
 return CharacterDatabase.QueueTransaction([guid,account](SqlConnection&c){auto exists=c.Query(("SELECT COUNT(*) FROM characters WHERE guid="+
  std::to_string(guid)).c_str());if(!exists||exists->Fetch()[0].GetUInt32())return false;return c.Execute(("UPDATE ai_playerbot_lifecycle SET status=9,"
  "disposition_completed=1,retired_at=UNIX_TIMESTAMP(),updated_at=UNIX_TIMESTAMP() WHERE guid="+std::to_string(guid)+" AND account="+
  std::to_string(account)+" AND status=8").c_str());});
#endif
}
