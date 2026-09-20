#include "RandomBotEstateStore.h"
#include "RandomBotLifecycleMath.h"
#include "Database/DatabaseEnv.h"
#include <algorithm>
#include <sstream>
namespace
{
std::string Accounts(std::vector<uint32>const&a){std::ostringstream s;for(uint32 v:a){if(s.tellp()>0)s<<',';s<<v;}return a.empty()?"0":s.str();}
}
std::future<bool> RandomBotEstateStore::PrepareDistribution(RandomBotEstateDistribution r)
{
#if !defined(CMANGOS_ASYNC_TRANSACTION_CALLBACK) || CMANGOS_ASYNC_TRANSACTION_CALLBACK < 4
 std::promise<bool>p;auto f=p.get_future();p.set_value(false);return f;
#else
 return CharacterDatabase.QueueTransaction([r](SqlConnection&c)
 {
  if(!r.estateId||!r.moneyCap||r.sinkMin>r.sinkMax||r.sinkMax>100||r.minRecipients>r.maxRecipients)return false;
  auto e=c.Query(("SELECT status,assets_detached,escrow,vendor_income FROM ai_playerbot_estate WHERE estate_id="+
   std::to_string(r.estateId)+" FOR UPDATE").c_str());if(!e)return false;if(e->Fetch()[0].GetUInt32()==3)return true;
  if(e->Fetch()[0].GetUInt32()!=2||!e->Fetch()[1].GetBool())return false;
  auto unsettled=c.Query(("SELECT (SELECT COUNT(*) FROM ai_playerbot_estate_lot WHERE estate_id="+std::to_string(r.estateId)+
   " AND status IN (0,1,2,3,8))+(SELECT COUNT(*) FROM ai_playerbot_estate_auction WHERE estate_id="+std::to_string(r.estateId)+
   " AND status<>4)+(SELECT COUNT(*) FROM ai_playerbot_estate_bid_claim WHERE estate_id="+std::to_string(r.estateId)+" AND status<>3)").c_str());
  if(!unsettled||unsettled->Fetch()[0].GetUInt64())return false;
  const uint64 estate=e->Fetch()[2].GetUInt64(),vendor=e->Fetch()[3].GetUInt64();
  const uint32 percent=r.sinkMin+(r.sinkMax-r.sinkMin?uint32(r.estateId%(r.sinkMax-r.sinkMin+1)):0);
  uint64 sink=RandomBotLifecycleMath::CalculateGoldSink(estate,vendor,percent),pool=estate-sink;
  std::vector<uint32> recipients,weights;std::vector<uint64> caps;
  const uint32 wanted=r.minRecipients+(r.maxRecipients-r.minRecipients?uint32(r.estateId%(r.maxRecipients-r.minRecipients+1)):0);
  auto q=c.Query(("SELECT c.guid,1+MOD(CRC32(CONCAT(c.guid,':',"+std::to_string(r.estateId)+")),1000) FROM characters c LEFT JOIN "
   "ai_playerbot_lifecycle l ON l.guid=c.guid WHERE c.account IN ("+Accounts(r.eligibleAccounts)+") AND c.online=0 AND "
   "(l.guid IS NULL OR l.status IN (0,1)) ORDER BY RAND() LIMIT "+std::to_string(wanted)).c_str());
  if(!q&&wanted)return false;if(q)do{recipients.push_back(q->Fetch()[0].GetUInt32());weights.push_back(q->Fetch()[1].GetUInt32());
   caps.push_back(std::min(r.moneyCap,r.recipientCap?r.recipientCap:r.moneyCap));}while(q->NextRow());
  auto shares=RandomBotLifecycleMath::CalculateInheritanceShares(pool,caps,weights);sink+=shares.remainder;
  for(size_t i=0;i<recipients.size();++i)if(shares.shares[i]&&!c.Execute(("INSERT INTO ai_playerbot_estate_inheritance "
   "(estate_id,recipient_guid,amount,created_at,updated_at) VALUES ("+std::to_string(r.estateId)+","+
   std::to_string(recipients[i])+","+std::to_string(shares.shares[i])+",UNIX_TIMESTAMP(),UNIX_TIMESTAMP())").c_str()))return false;
  return c.Execute(("UPDATE ai_playerbot_estate SET status=3,escrow=0,final_estate="+std::to_string(estate)+",sink_percent="+
   std::to_string(percent)+",gold_sunk="+std::to_string(sink)+",updated_at=UNIX_TIMESTAMP() WHERE estate_id="+
   std::to_string(r.estateId)).c_str());
 });
#endif
}
std::future<bool> RandomBotEstateStore::DeliverInheritance(uint64 estateId,uint32 recipient,uint64 moneyCap,std::vector<uint32> accounts)
{
#if !defined(CMANGOS_ASYNC_TRANSACTION_CALLBACK) || CMANGOS_ASYNC_TRANSACTION_CALLBACK < 4
 std::promise<bool>p;auto f=p.get_future();p.set_value(false);return f;
#else
 return CharacterDatabase.QueueTransaction([estateId,recipient,moneyCap,accounts](SqlConnection&c)
 {
  if(!estateId||!recipient||!moneyCap)return false;auto e=c.Query(("SELECT status FROM ai_playerbot_estate WHERE estate_id="+
   std::to_string(estateId)+" FOR UPDATE").c_str());if(!e||e->Fetch()[0].GetUInt32()!=3)return false;
  auto l=c.Query(("SELECT amount,delivered FROM ai_playerbot_estate_inheritance WHERE estate_id="+std::to_string(estateId)+
   " AND recipient_guid="+std::to_string(recipient)+" FOR UPDATE").c_str());if(!l)return false;if(l->Fetch()[1].GetBool())return true;
  uint64 amount=l->Fetch()[0].GetUInt64(),delivered=0;auto ch=c.Query(("SELECT account,money,online FROM characters WHERE guid="+
   std::to_string(recipient)+" FOR UPDATE").c_str());
  if(ch&&!ch->Fetch()[2].GetBool()&&std::find(accounts.begin(),accounts.end(),ch->Fetch()[0].GetUInt32())!=accounts.end())
  {auto retired=c.Query(("SELECT COUNT(*) FROM ai_playerbot_lifecycle WHERE guid="+std::to_string(recipient)+" AND status>=2").c_str());
   uint64 money=ch->Fetch()[1].GetUInt64();if(retired&&!retired->Fetch()[0].GetUInt32()&&money<moneyCap)delivered=std::min(amount,moneyCap-money);}
  if(delivered&&!c.Execute(("UPDATE characters SET money=money+"+std::to_string(delivered)+" WHERE guid="+std::to_string(recipient)).c_str()))return false;
  return c.Execute(("UPDATE ai_playerbot_estate_inheritance SET delivered=1,delivered_amount="+std::to_string(delivered)+
   ",updated_at=UNIX_TIMESTAMP() WHERE estate_id="+std::to_string(estateId)+" AND recipient_guid="+std::to_string(recipient)).c_str())&&
   c.Execute(("UPDATE ai_playerbot_estate SET gold_distributed=gold_distributed+"+std::to_string(delivered)+",gold_sunk=gold_sunk+"+
   std::to_string(amount-delivered)+",updated_at=UNIX_TIMESTAMP() WHERE estate_id="+std::to_string(estateId)).c_str());
 });
#endif
}
std::future<bool> RandomBotEstateStore::CompleteDistribution(uint64 estateId)
{
#if !defined(CMANGOS_ASYNC_TRANSACTION_CALLBACK) || CMANGOS_ASYNC_TRANSACTION_CALLBACK < 4
 std::promise<bool>p;auto f=p.get_future();p.set_value(false);return f;
#else
 return CharacterDatabase.QueueTransaction([estateId](SqlConnection&c){auto e=c.Query(("SELECT status FROM ai_playerbot_estate WHERE estate_id="+
  std::to_string(estateId)+" FOR UPDATE").c_str());if(!e)return false;if(e->Fetch()[0].GetUInt32()==4)return true;
  auto n=c.Query(("SELECT COUNT(*) FROM ai_playerbot_estate_inheritance WHERE estate_id="+std::to_string(estateId)+" AND delivered=0").c_str());
  return e->Fetch()[0].GetUInt32()==3&&n&&!n->Fetch()[0].GetUInt32()&&c.Execute(("UPDATE ai_playerbot_estate SET status=4,updated_at=UNIX_TIMESTAMP() WHERE estate_id="+
  std::to_string(estateId)).c_str());});
#endif
}
