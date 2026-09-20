#include "RandomBotEstateStore.h"
#include "Database/DatabaseEnv.h"
#include <algorithm>
#include <vector>
namespace
{
using R=RandomBotReplacementReservation;
bool Valid(R const&r){return r.retiringGuid&&r.guid&&r.account&&!r.name.empty()&&r.name.size()<=12&&r.race&&r.cls&&r.gender<=1&&
 std::find(r.eligibleAccounts.begin(),r.eligibleAccounts.end(),r.account)!=r.eligibleAccounts.end();}
std::string Escape(SqlConnection& c,std::string const& value){std::vector<char> out(value.size()*2+1);unsigned long n=c.escape_string(out.data(),value.data(),value.size());return std::string(out.data(),n);}
}
std::future<bool> RandomBotEstateStore::ReserveReplacement(R r)
{
#if !defined(CMANGOS_ASYNC_TRANSACTION_CALLBACK) || CMANGOS_ASYNC_TRANSACTION_CALLBACK < 4
 std::promise<bool>p;auto f=p.get_future();p.set_value(false);return f;
#else
 return CharacterDatabase.QueueTransaction([r](SqlConnection&c){if(!Valid(r))return false;
  auto l=c.Query(("SELECT status,replacement_required,replacement_status FROM ai_playerbot_lifecycle WHERE guid="+
   std::to_string(r.retiringGuid)+" FOR UPDATE").c_str());if(!l||!(l->Fetch()[0].GetUInt32()==7||l->Fetch()[0].GetUInt32()==9)||
   !l->Fetch()[1].GetBool())return false;if(l->Fetch()[2].GetUInt32()!=0)return false;
  std::string name=Escape(c,r.name);auto used=c.Query(("SELECT COUNT(*) FROM characters WHERE guid="+std::to_string(r.guid)+
   " OR name='"+name+"'").c_str());if(!used||used->Fetch()[0].GetUInt32())return false;
  return c.Execute(("UPDATE ai_playerbot_lifecycle SET replacement_status=1,replacement_guid="+std::to_string(r.guid)+
   ",replacement_account="+std::to_string(r.account)+",replacement_name='"+name+"',replacement_race="+
   std::to_string(r.race)+",replacement_class="+std::to_string(r.cls)+",replacement_gender="+std::to_string(r.gender)+
   ",updated_at=UNIX_TIMESTAMP() WHERE guid="+std::to_string(r.retiringGuid)+" AND replacement_status=0").c_str());});
#endif
}
std::future<bool> RandomBotEstateStore::ConfirmReplacement(R r)
{
#if !defined(CMANGOS_ASYNC_TRANSACTION_CALLBACK) || CMANGOS_ASYNC_TRANSACTION_CALLBACK < 4
 std::promise<bool>p;auto f=p.get_future();p.set_value(false);return f;
#else
 return CharacterDatabase.QueueTransaction([r](SqlConnection&c){if(!Valid(r))return false;std::string name=Escape(c,r.name);
  auto ch=c.Query(("SELECT COUNT(*) FROM characters WHERE guid="+std::to_string(r.guid)+" AND account="+std::to_string(r.account)+
   " AND name='"+name+"' AND race="+std::to_string(r.race)+" AND class="+std::to_string(r.cls)+" AND gender="+
   std::to_string(r.gender)).c_str());if(!ch||ch->Fetch()[0].GetUInt32()!=1)return false;
  return c.Execute(("UPDATE ai_playerbot_lifecycle SET replacement_status=2,updated_at=UNIX_TIMESTAMP() WHERE guid="+
   std::to_string(r.retiringGuid)+" AND replacement_status=1 AND replacement_guid="+std::to_string(r.guid)).c_str());});
#endif
}
