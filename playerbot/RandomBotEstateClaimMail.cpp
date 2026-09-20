#include "RandomBotEstateStore.h"
#include "Database/DatabaseEnv.h"
namespace
{
using R=RandomBotEstateClaimMail;
bool Valid(R const&r){return r.estateId&&r.auctionId&&r.brokerGuid&&r.bid&&r.mailId&&(r.status==1||(r.status==2&&r.itemGuid&&r.itemEntry&&r.itemCount));}
#if defined(CMANGOS_ASYNC_TRANSACTION_CALLBACK) && CMANGOS_ASYNC_TRANSACTION_CALLBACK >= 4
bool Post(SqlConnection&c,R const&r)
{
 auto claim=c.Query(("SELECT COUNT(*) FROM ai_playerbot_estate_bid_claim WHERE auction_id="+std::to_string(r.auctionId)+" AND status=3").c_str());
 auto mail=c.Query(("SELECT COUNT(*) FROM mail WHERE id="+std::to_string(r.mailId)).c_str());
 auto op=c.Query(("SELECT COUNT(*) FROM ai_playerbot_estate_operation WHERE estate_id="+std::to_string(r.estateId)+" AND kind=8 AND asset_id="+std::to_string(r.auctionId)).c_str());
 return claim&&claim->Fetch()[0].GetUInt32()==1&&mail&&!mail->Fetch()[0].GetUInt32()&&op&&op->Fetch()[0].GetUInt32()==1;
}
#endif
}
std::future<bool> RandomBotEstateStore::ConsumeClaimMail(R r)
{
#if !defined(CMANGOS_ASYNC_TRANSACTION_CALLBACK) || CMANGOS_ASYNC_TRANSACTION_CALLBACK < 4
 std::promise<bool>p;auto f=p.get_future();p.set_value(false);return f;
#else
 return CharacterDatabase.QueueTransaction([r](SqlConnection&c)
 {
  if(!Valid(r))return false;
  auto claim=c.Query(("SELECT estate_id,broker_guid,bid,status,mail_id,item_guid,item_entry,item_count FROM ai_playerbot_estate_bid_claim WHERE auction_id="+
   std::to_string(r.auctionId)+" FOR UPDATE").c_str());if(!claim)return false;Field*f=claim->Fetch();
  if(f[0].GetUInt64()!=r.estateId||f[1].GetUInt32()!=r.brokerGuid||f[2].GetUInt32()!=r.bid||f[3].GetUInt32()!=r.status||
   f[4].GetUInt32()!=r.mailId||f[5].GetUInt32()!=r.itemGuid||f[6].GetUInt32()!=r.itemEntry||f[7].GetUInt32()!=r.itemCount)return false;
  auto mail=c.Query(("SELECT receiver,money,cod,has_items FROM mail WHERE id="+std::to_string(r.mailId)+" FOR UPDATE").c_str());if(!mail)return false;Field*m=mail->Fetch();
  if(m[0].GetUInt32()!=r.brokerGuid||m[2].GetUInt32()||m[1].GetUInt32()!=(r.status==1?r.bid:0)||m[3].GetBool()!=(r.status==2))return false;
  if(r.status==1)
  {if(!c.Execute(("UPDATE ai_playerbot_estate SET escrow=escrow+"+std::to_string(r.bid)+",updated_at=UNIX_TIMESTAMP() WHERE estate_id="+std::to_string(r.estateId)).c_str()))return false;}
  else
  {
   auto item=c.Query(("SELECT COUNT(*) FROM mail_items mi INNER JOIN item_instance i ON i.guid=mi.item_guid WHERE mi.mail_id="+
    std::to_string(r.mailId)+" AND mi.item_guid="+std::to_string(r.itemGuid)+" AND i.owner_guid="+std::to_string(r.brokerGuid)+" FOR UPDATE").c_str());
   if(!item||item->Fetch()[0].GetUInt32()!=1||!c.Execute(("DELETE FROM mail_items WHERE mail_id="+std::to_string(r.mailId)).c_str())||
    !c.Execute(("INSERT INTO ai_playerbot_estate_lot (estate_id,item_guid,item_entry,item_count,status,updated_at) VALUES ("+
    std::to_string(r.estateId)+","+std::to_string(r.itemGuid)+","+std::to_string(r.itemEntry)+","+std::to_string(r.itemCount)+",0,UNIX_TIMESTAMP())").c_str()))return false;
  }
  return c.Execute(("DELETE FROM mail WHERE id="+std::to_string(r.mailId)).c_str())&&c.Execute(("UPDATE ai_playerbot_estate_bid_claim SET status=3,updated_at=UNIX_TIMESTAMP() WHERE auction_id="+
   std::to_string(r.auctionId)).c_str())&&c.Execute(("INSERT INTO ai_playerbot_estate_operation (estate_id,kind,asset_id,item_entry,item_count,amount,completed_at) VALUES ("+
   std::to_string(r.estateId)+",8,"+std::to_string(r.auctionId)+","+std::to_string(r.itemEntry)+","+std::to_string(r.itemCount)+","+
   std::to_string(r.status==1?r.bid:0)+",UNIX_TIMESTAMP())").c_str());
 });
#endif
}
std::future<bool> RandomBotEstateStore::ConfirmClaimMail(R r)
{
#if !defined(CMANGOS_ASYNC_TRANSACTION_CALLBACK) || CMANGOS_ASYNC_TRANSACTION_CALLBACK < 4
 std::promise<bool>p;auto f=p.get_future();p.set_value(false);return f;
#else
 return CharacterDatabase.QueueTransaction([r](SqlConnection&c){return Valid(r)&&Post(c,r);});
#endif
}
