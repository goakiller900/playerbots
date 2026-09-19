#include "RandomBotEstateStore.h"
#include "Database/DatabaseEnv.h"
#include "Entities/Item.h"
#include "Entities/Bag.h"
#include <cmath>
#include <limits>
#include <sstream>

bool RandomBotEstateStore::QuoteLotLiquidation(Item const& item, RandomBotEstateLiquidation& r)
{
#if !defined(CMANGOS_ASYNC_TRANSACTION_CALLBACK) || CMANGOS_ASYNC_TRANSACTION_CALLBACK < 4
    (void)item;
    (void)r;
    return false;
#else
    const ItemPrototype* proto = item.GetProto();
    if (!proto || !item.GetCount() || item.GetOwnerGuid().GetCounter() != r.brokerGuid ||
        item.HasFlag(ITEM_FIELD_FLAGS, ITEM_DYNFLAG_WRAPPED) || item.HasSavedLoot() ||
        (item.IsBag() && !static_cast<Bag const&>(item).IsEmpty()) || (r.destroy && !r.destroyAllowed))
        return false;
    if (!r.destroy)
    {
        const uint64 base = uint64(proto->SellPrice) * item.GetCount();
        if (!proto->SellPrice || base > std::numeric_limits<uint32>::max())
            return false;
        for (uint32 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
        {
            const auto& spell = proto->Spells[i];
            if (spell.SpellId && spell.SpellCharges < 0)
            {
                const float ratio = static_cast<float>(item.GetSpellCharges(i)) / spell.SpellCharges;
                const float scaled = static_cast<float>(base) * ratio;
                if (!std::isfinite(ratio) || ratio < 0 || ratio > 1 ||
                    !std::isfinite(scaled) || static_cast<double>(scaled) > std::numeric_limits<uint32>::max())
                    return false;
                break;
            }
        }
    }
    r.itemGuid = item.GetGUIDLow();
    r.itemEntry = item.GetEntry();
    r.itemCount = item.GetCount();
    r.proceeds = r.destroy ? 0 : item.GetVendorSellValue(item.GetCount());
    std::ostringstream charges;
    for (uint32 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
        charges << item.GetSpellCharges(i) << ' ';
    r.charges = charges.str();
    return true;
#endif
}
