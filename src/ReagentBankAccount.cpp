#include "ReagentBankAccount.h"
#include <algorithm>
#include <cctype>
#include <unordered_map>

uint32 g_maxOptionsPerPage;
bool g_accountWideReagentBank = false;

// AzerothCore module: Account-wide Reagent Bank
// This script adds a reagent bank NPC that allows players to deposit and
// withdraw reagents account-wide.

class mod_reagent_bank_account : public CreatureScript
{
private:
  // Caches for item templates and icons
  mutable std::unordered_map<uint32, const ItemTemplate *> itemTemplateCache;
  mutable std::unordered_map<uint32, std::string> itemIconCache;
  // Last viewed category + page per player (guidLow -> (category, page))
  mutable std::unordered_map<uint32, std::pair<uint32, uint16>> m_lastCategoryPage;

  // Action codes for item-specific withdraw submenu
  static constexpr uint32 ACTION_WITHDRAW_ONE = 900001;
  static constexpr uint32 ACTION_WITHDRAW_STACK = 900002;
  static constexpr uint32 ACTION_WITHDRAW_ALL = 900003;

  // Helper to resolve the stored key pattern. We store either:
  //  account_id = <acct>, guid = 0   (account-wide mode)
  //  account_id = 0,      guid = <guid> (per-character mode)
  void GetStorageKeys(Player *player, uint32 &accountKey, uint32 &guidKey) const
  {
    if (g_accountWideReagentBank)
    {
      accountKey = player->GetSession()->GetAccountId();
      guidKey = 0;
    }
    else
    {
      accountKey = 0;
      guidKey = player->GetGUID().GetRawValue();
    }
  }

  bool IsCategory(uint32 value) const
  {
    switch (value)
    {
    case ITEM_SUBCLASS_CLOTH:
    case ITEM_SUBCLASS_MEAT:
    case ITEM_SUBCLASS_METAL_STONE:
    case ITEM_SUBCLASS_ENCHANTING:
    case ITEM_SUBCLASS_ELEMENTAL:
    case ITEM_SUBCLASS_PARTS:
    case ITEM_SUBCLASS_TRADE_GOODS_OTHER:
    case ITEM_SUBCLASS_HERB:
    case ITEM_SUBCLASS_LEATHER:
    case ITEM_SUBCLASS_JEWELCRAFTING:
    case ITEM_SUBCLASS_EXPLOSIVES:
    case ITEM_SUBCLASS_DEVICES:
    case ITEM_SUBCLASS_MATERIAL:
    case ITEM_SUBCLASS_ARMOR_ENCHANTMENT:
    case ITEM_SUBCLASS_WEAPON_ENCHANTMENT:
      return true;
    default:
      return false;
    }
  }

  // Get and cache ItemTemplate
  const ItemTemplate *GetCachedItemTemplate(uint32 entry) const
  {
    auto it = itemTemplateCache.find(entry);
    if (it != itemTemplateCache.end())
      return it->second;
    const ItemTemplate *temp = sObjectMgr->GetItemTemplate(entry);
    itemTemplateCache[entry] = temp;
    return temp;
  }

  // Get and cache item icon string
  std::string GetCachedItemIcon(uint32 entry, uint32 width, uint32 height,
                                int x, int y) const
  {
    auto it = itemIconCache.find(entry);
    if (it != itemIconCache.end())
      return it->second;
    std::ostringstream ss;
    ss << "|TInterface";
    const ItemTemplate *temp = GetCachedItemTemplate(entry);
    const ItemDisplayInfoEntry *dispInfo = nullptr;
    if (temp)
    {
      dispInfo = sItemDisplayInfoStore.LookupEntry(temp->DisplayInfoID);
      if (dispInfo)
        ss << "/ICONS/" << dispInfo->inventoryIcon;
    }
    if (!dispInfo)
      ss << "/InventoryItems/WoWUnknownItem01";
    ss << ":" << width << ":" << height << ":" << x << ":" << y << "|t";
    std::string iconStr = ss.str();
    itemIconCache[entry] = iconStr;
    return iconStr;
  }

  // Gets the localized item name for display and comparisons
  std::string GetItemName(uint32 entry, WorldSession *session) const
  {
    int loc_idx = session->GetSessionDbLocaleIndex();
    const ItemTemplate *temp = GetCachedItemTemplate(entry);
    std::string name = temp ? temp->Name1 : "Unknown";
    if (temp)
    {
      if (ItemLocale const *il = sObjectMgr->GetItemLocale(temp->ItemId))
        ObjectMgr::GetLocaleString(il->Name, loc_idx, name);
    }
    return name;
  }

  // Returns a colored item link string for display in gossip menus (no cache,
  // as it may be locale-dependent)
  std::string GetItemLink(uint32 entry, WorldSession *session) const
  {
    const ItemTemplate *temp = GetCachedItemTemplate(entry);
    std::string name = GetItemName(entry, session);
    std::ostringstream oss;
    oss << "|c";
    if (temp)
      oss << std::hex << ItemQualityColors[temp->Quality] << std::dec;
    else
      oss << "ffffffff";
    oss << "|Hitem:" << entry << ":0|h[" << name << "]|h|r";
    return oss.str();
  }

  // Withdraws a stack or all of a reagent from the account-wide bank for the
  // player
  void WithdrawItem(Player *player, uint32 entry)
  {
    uint32 accountKey, guidKey;
    GetStorageKeys(player, accountKey, guidKey);
    QueryResult result = CharacterDatabase.Query(
        "SELECT amount FROM mod_reagent_bank_account WHERE account_id = {} AND guid = {} AND item_entry = {}",
        accountKey, guidKey, entry);
    if (result)
    {
      uint32 storedAmount = (*result)[0].Get<uint32>();
      const ItemTemplate *temp = sObjectMgr->GetItemTemplate(entry);
      if (!temp)
      {
        ChatHandler(player->GetSession())
            .PSendSysMessage("Error: Item template not found for entry {}.",
                             entry);
        return;
      }
      uint32 stackSize = temp->GetMaxStackSize();
      if (storedAmount <= stackSize)
      {
        // Give the player all of the item and remove it from the DB
        ItemPosCountVec dest;
        InventoryResult msg = player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest,
                                                      entry, storedAmount);
        if (msg == EQUIP_ERR_OK)
        {
          CharacterDatabase.DirectExecute(
              "DELETE FROM mod_reagent_bank_account WHERE account_id = {} AND guid = {} AND item_entry = {}",
              accountKey, guidKey, entry);
          Item *item = player->StoreNewItem(dest, entry, true);
          player->SendNewItem(item, storedAmount, true, false);
          ChatHandler(player->GetSession())
              .PSendSysMessage("Withdrew {} x {}.", storedAmount,
                               temp->Name1);
        }
        else
        {
          player->SendEquipError(msg, nullptr, nullptr, entry);
          ChatHandler(player->GetSession())
              .PSendSysMessage("Not enough bag space to withdraw {} x {}.",
                               storedAmount, temp->Name1);
          return;
        }
      }
      else
      {
        // Give the player a single stack
        ItemPosCountVec dest;
        InventoryResult msg = player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest,
                                                      entry, stackSize);
        if (msg == EQUIP_ERR_OK)
        {
          CharacterDatabase.DirectExecute(
              "UPDATE mod_reagent_bank_account SET amount = {} WHERE account_id = {} AND guid = {} AND item_entry = {}",
              storedAmount - stackSize, accountKey, guidKey, entry);
          Item *item = player->StoreNewItem(dest, entry, true);
          player->SendNewItem(item, stackSize, true, false);
          ChatHandler(player->GetSession())
              .PSendSysMessage("Withdrew {} x {}.", stackSize,
                               temp->Name1);
        }
        else
        {
          player->SendEquipError(msg, nullptr, nullptr, entry);
          ChatHandler(player->GetSession())
              .PSendSysMessage("Not enough bag space to withdraw {} x {}.",
                               stackSize, temp->Name1);
          return;
        }
      }
    }
  }

  // Withdraw one unit regardless of stack size
  void WithdrawOne(Player *player, uint32 entry)
  {
    uint32 accountKey, guidKey;
    GetStorageKeys(player, accountKey, guidKey);
    QueryResult result = CharacterDatabase.Query(
        "SELECT amount FROM mod_reagent_bank_account WHERE account_id = {} AND guid = {} AND item_entry = {}",
        accountKey, guidKey, entry);
    if (!result)
      return;
    uint32 stored = (*result)[0].Get<uint32>();
    if (stored == 0)
      return;
    const ItemTemplate *temp = sObjectMgr->GetItemTemplate(entry);
    if (!temp)
      return;
    ItemPosCountVec dest;
    InventoryResult msg = player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, entry, 1);
    if (msg != EQUIP_ERR_OK)
    {
      player->SendEquipError(msg, nullptr, nullptr, entry);
      ChatHandler(player->GetSession()).PSendSysMessage("Not enough space to withdraw 1 x {}.", temp->Name1);
      return;
    }
    if (stored == 1)
      CharacterDatabase.DirectExecute("DELETE FROM mod_reagent_bank_account WHERE account_id = {} AND guid = {} AND item_entry = {}", accountKey, guidKey, entry);
    else
      CharacterDatabase.DirectExecute("UPDATE mod_reagent_bank_account SET amount = {} WHERE account_id = {} AND guid = {} AND item_entry = {}", stored - 1, accountKey, guidKey, entry);
    Item *item = player->StoreNewItem(dest, entry, true);
    player->SendNewItem(item, 1, true, false);
    ChatHandler(player->GetSession()).PSendSysMessage("Withdrew 1 x {}.", temp->Name1);
  }

  // Withdraw up to one full stack (or remaining if smaller)
  void WithdrawStack(Player *player, uint32 entry)
  {
    uint32 accountKey, guidKey;
    GetStorageKeys(player, accountKey, guidKey);
    QueryResult result = CharacterDatabase.Query(
        "SELECT amount FROM mod_reagent_bank_account WHERE account_id = {} AND guid = {} AND item_entry = {}",
        accountKey, guidKey, entry);
    if (!result)
      return;
    uint32 stored = (*result)[0].Get<uint32>();
    if (stored == 0)
      return;
    const ItemTemplate *temp = sObjectMgr->GetItemTemplate(entry);
    if (!temp)
      return;
    uint32 stackSize = temp->GetMaxStackSize();
    uint32 toGive = std::min(stackSize, stored);
    ItemPosCountVec dest;
    InventoryResult msg = player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, entry, toGive);
    if (msg != EQUIP_ERR_OK)
    {
      player->SendEquipError(msg, nullptr, nullptr, entry);
      ChatHandler(player->GetSession()).PSendSysMessage("Not enough space to withdraw {} x {}.", toGive, temp->Name1);
      return;
    }
    if (toGive == stored)
      CharacterDatabase.DirectExecute("DELETE FROM mod_reagent_bank_account WHERE account_id = {} AND guid = {} AND item_entry = {}", accountKey, guidKey, entry);
    else
      CharacterDatabase.DirectExecute("UPDATE mod_reagent_bank_account SET amount = {} WHERE account_id = {} AND guid = {} AND item_entry = {}", stored - toGive, accountKey, guidKey, entry);
    Item *item = player->StoreNewItem(dest, entry, true);
    player->SendNewItem(item, toGive, true, false);
    ChatHandler(player->GetSession()).PSendSysMessage("Withdrew {} x {}.", toGive, temp->Name1);
  }

  // Withdraw all (multiple stacks as needed)
  void WithdrawAllOfItem(Player *player, uint32 entry)
  {
    uint32 accountKey, guidKey;
    GetStorageKeys(player, accountKey, guidKey);
    QueryResult result = CharacterDatabase.Query(
        "SELECT amount FROM mod_reagent_bank_account WHERE account_id = {} AND guid = {} AND item_entry = {}",
        accountKey, guidKey, entry);
    if (!result)
      return;
    uint32 remaining = (*result)[0].Get<uint32>();
    if (remaining == 0)
      return;
    const ItemTemplate *temp = sObjectMgr->GetItemTemplate(entry);
    if (!temp)
      return;
    uint32 stackSize = temp->GetMaxStackSize();
    uint32 givenTotal = 0;
    while (remaining > 0)
    {
      uint32 toGive = std::min(stackSize, remaining);
      ItemPosCountVec dest;
      InventoryResult msg = player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, entry, toGive);
      if (msg != EQUIP_ERR_OK)
      {
        player->SendEquipError(msg, nullptr, nullptr, entry);
        ChatHandler(player->GetSession()).PSendSysMessage("Bag full after withdrawing {} x {} (remaining {}).", givenTotal, temp->Name1, remaining);
        break;
      }
      Item *item = player->StoreNewItem(dest, entry, true);
      player->SendNewItem(item, toGive, true, false);
      givenTotal += toGive;
      remaining -= toGive;
    }
    if (remaining == 0)
      CharacterDatabase.DirectExecute("DELETE FROM mod_reagent_bank_account WHERE account_id = {} AND guid = {} AND item_entry = {}", accountKey, guidKey, entry);
    else
      CharacterDatabase.DirectExecute("UPDATE mod_reagent_bank_account SET amount = {} WHERE account_id = {} AND guid = {} AND item_entry = {}", remaining, accountKey, guidKey, entry);
    if (givenTotal > 0)
      ChatHandler(player->GetSession()).PSendSysMessage("Withdrew {} x {}.", givenTotal, temp->Name1);
  }

  void ShowItemWithdrawMenu(Player *player, Creature *creature, uint32 category, uint16 pageIndex, uint32 itemEntry)
  {
    uint32 accountKey, guidKey;
    GetStorageKeys(player, accountKey, guidKey);
    QueryResult result = CharacterDatabase.Query(
        "SELECT amount FROM mod_reagent_bank_account WHERE account_id = {} AND guid = {} AND item_entry = {}",
        accountKey, guidKey, itemEntry);
    uint32 stored = result ? (*result)[0].Get<uint32>() : 0;
    const ItemTemplate *temp = sObjectMgr->GetItemTemplate(itemEntry);
    std::string name = temp ? temp->Name1 : "Unknown";
    player->PlayerTalkClass->ClearMenus();
    constexpr int ICON_SIZE = 18;
    constexpr int ICON_X = 0;
    constexpr int ICON_Y = 0;
    constexpr int GOSSIP_ICON_NONE = 0;
    std::string icon = GetCachedItemIcon(itemEntry, ICON_SIZE, ICON_SIZE, ICON_X, ICON_Y);
    AddGossipItemFor(player, GOSSIP_ICON_NONE, icon + GetItemLink(itemEntry, player->GetSession()) + " |cff000000Stored: " + std::to_string(stored) + "|r", 0, 0);
    if (stored > 0)
      AddGossipItemFor(player, GOSSIP_ICON_NONE, "Withdraw 1", ACTION_WITHDRAW_ONE, itemEntry);
    if (stored > 1 && temp && temp->GetMaxStackSize() > 1)
      AddGossipItemFor(player, GOSSIP_ICON_NONE, "Withdraw Stack", ACTION_WITHDRAW_STACK, itemEntry);
    if (stored > 0)
      AddGossipItemFor(player, GOSSIP_ICON_NONE, "Withdraw All", ACTION_WITHDRAW_ALL, itemEntry);
    AddGossipItemFor(player, GOSSIP_ICON_NONE, "Back", category, pageIndex);
    SendGossipMenuFor(player, NPC_TEXT_ID, creature->GetGUID());
  }

  // Updates the item count maps and removes the item from the player's
  // inventory
  void UpdateItemCount(std::map<uint32, uint32> &entryToAmountMap,
                       std::map<uint32, uint32> &entryToSubclassMap,
                       std::map<uint32, uint32> &itemsAddedMap, Item *pItem,
                       Player *player, uint32 bagSlot, uint32 itemSlot)
  {
    uint32 count = pItem->GetCount();
    ItemTemplate const *itemTemplate = pItem->GetTemplate();

    // Only allow trade goods and gems, and skip unique items
    if (!(itemTemplate->Class == ITEM_CLASS_TRADE_GOODS ||
          itemTemplate->Class == ITEM_CLASS_GEM) ||
        itemTemplate->GetMaxStackSize() == 1)
      return;
    uint32 itemEntry = itemTemplate->ItemId;
    uint32 itemSubclass = itemTemplate->SubClass;

    // Put gems to ITEM_SUBCLASS_JEWELCRAFTING section
    if (itemTemplate->Class == ITEM_CLASS_GEM)
    {
      itemSubclass = ITEM_SUBCLASS_JEWELCRAFTING;
    }

    // Update or add to the amount and subclass maps
    if (!entryToAmountMap.count(itemEntry))
    {
      entryToAmountMap[itemEntry] = count;
      entryToSubclassMap[itemEntry] = itemSubclass;
    }
    else
    {
      uint32 existingCount = entryToAmountMap.find(itemEntry)->second;
      entryToAmountMap[itemEntry] = existingCount + count;
    }

    // Track what was deposited for feedback
    if (!itemsAddedMap.count(itemEntry))
    {
      itemsAddedMap[itemEntry] = count;
    }
    else
    {
      uint32 existingCount = itemsAddedMap.find(itemEntry)->second;
      itemsAddedMap[itemEntry] = existingCount + count;
    }

    // Remove the item from the player's inventory
    player->DestroyItem(bagSlot, itemSlot, true);
  }

  // Deposits all reagents from the player's bags into the account-wide bank
  void DepositAllReagents(Player *player)
  {
    WorldSession *session = player->GetSession();
    uint32 accountKey, guidKey;
    GetStorageKeys(player, accountKey, guidKey);
    std::string query = "SELECT item_entry, item_subclass, amount FROM mod_reagent_bank_account WHERE account_id = " + std::to_string(accountKey) + " AND guid = " + std::to_string(guidKey);
    session->GetQueryProcessor().AddCallback(
        CharacterDatabase.AsyncQuery(query).WithCallback(
            [=, this](QueryResult result)
            {
              std::map<uint32, uint32> entryToAmountMap;
              std::map<uint32, uint32> entryToSubclassMap;
              std::map<uint32, uint32> itemsAddedMap;
              if (result)
              {
                do
                {
                  uint32 itemEntry = (*result)[0].Get<uint32>();
                  uint32 itemSubclass = (*result)[1].Get<uint32>();
                  uint32 itemAmount = (*result)[2].Get<uint32>();
                  entryToAmountMap[itemEntry] = itemAmount;
                  entryToSubclassMap[itemEntry] = itemSubclass;
                } while (result->NextRow());
              }
              // Inventory Items
              for (uint8 i = INVENTORY_SLOT_ITEM_START;
                   i < INVENTORY_SLOT_ITEM_END; ++i)
              {
                if (Item *pItem =
                        player->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
                {
                  UpdateItemCount(entryToAmountMap, entryToSubclassMap,
                                  itemsAddedMap, pItem, player,
                                  INVENTORY_SLOT_BAG_0, i);
                }
              }
              // Bag Items
              for (uint32 i = INVENTORY_SLOT_BAG_START;
                   i < INVENTORY_SLOT_BAG_END; i++)
              {
                Bag *bag = player->GetBagByPos(i);
                if (!bag)
                  continue;
                for (uint32 j = 0; j < bag->GetBagSize(); j++)
                {
                  if (Item *pItem = player->GetItemByPos(i, j))
                  {
                    UpdateItemCount(entryToAmountMap, entryToSubclassMap,
                                    itemsAddedMap, pItem, player, i, j);
                  }
                }
              }
              // Write all changes to the DB in a transaction
              if (entryToAmountMap.size() != 0)
              {
                auto trans = CharacterDatabase.BeginTransaction();
                for (std::pair<uint32, uint32> mapEntry : entryToAmountMap)
                {
                  uint32 itemEntry = mapEntry.first;
                  uint32 itemAmount = mapEntry.second;
                  uint32 itemSubclass =
                      entryToSubclassMap.find(itemEntry)->second;
                  trans->Append("REPLACE INTO mod_reagent_bank_account (account_id, guid, item_entry, item_subclass, amount) VALUES ({}, {}, {}, {}, {})",
                                accountKey, guidKey, itemEntry, itemSubclass, itemAmount);
                }
                CharacterDatabase.CommitTransaction(
                    trans); // <-- just call, don't check return value
              }
              // Feedback to player
              if (itemsAddedMap.size() != 0)
              {
                ChatHandler(player->GetSession())
                    .SendSysMessage("The following was deposited:");
                for (std::pair<uint32, uint32> mapEntry : itemsAddedMap)
                {
                  uint32 itemEntry = mapEntry.first;
                  uint32 itemAmount = mapEntry.second;
                  ItemTemplate const *itemTemplate =
                      sObjectMgr->GetItemTemplate(itemEntry);
                  std::string itemName = itemTemplate->Name1;
                  ChatHandler(player->GetSession())
                      .SendSysMessage(std::to_string(itemAmount) + " " +
                                      itemName);
                }
              }
              else
              {
                ChatHandler(player->GetSession())
                    .PSendSysMessage("No reagents to deposit.");
              }
            }));

    CloseGossipMenuFor(player);
  }

  void DepositAllReagentsForCategory(Player *player, uint32 item_subclass)
  {
    uint32 accountKey, guidKey;
    GetStorageKeys(player, accountKey, guidKey);

    std::map<uint32, uint32> entryToAmountMap;
    std::map<uint32, uint32> entryToSubclassMap;
    std::map<uint32, uint32> itemsAddedMap;

    // Inventory Items
    for (uint8 i = INVENTORY_SLOT_ITEM_START; i < INVENTORY_SLOT_ITEM_END;
         ++i)
    {
      if (Item *pItem = player->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
      {
        ItemTemplate const *itemTemplate = pItem->GetTemplate();
        uint32 subclass = (itemTemplate->Class == ITEM_CLASS_GEM)
                              ? ITEM_SUBCLASS_JEWELCRAFTING
                              : itemTemplate->SubClass;
        if ((itemTemplate->Class == ITEM_CLASS_TRADE_GOODS ||
             itemTemplate->Class == ITEM_CLASS_GEM) &&
            itemTemplate->GetMaxStackSize() > 1 && subclass == item_subclass)
        {
          UpdateItemCount(entryToAmountMap, entryToSubclassMap, itemsAddedMap,
                          pItem, player, INVENTORY_SLOT_BAG_0, i);
        }
      }
    }
    // Bag Items
    for (uint32 i = INVENTORY_SLOT_BAG_START; i < INVENTORY_SLOT_BAG_END; i++)
    {
      Bag *bag = player->GetBagByPos(i);
      if (!bag)
        continue;
      for (uint32 j = 0; j < bag->GetBagSize(); j++)
      {
        if (Item *pItem = player->GetItemByPos(i, j))
        {
          ItemTemplate const *itemTemplate = pItem->GetTemplate();
          uint32 subclass = (itemTemplate->Class == ITEM_CLASS_GEM)
                                ? ITEM_SUBCLASS_JEWELCRAFTING
                                : itemTemplate->SubClass;
          if ((itemTemplate->Class == ITEM_CLASS_TRADE_GOODS ||
               itemTemplate->Class == ITEM_CLASS_GEM) &&
              itemTemplate->GetMaxStackSize() > 1 &&
              subclass == item_subclass)
          {
            UpdateItemCount(entryToAmountMap, entryToSubclassMap, itemsAddedMap,
                            pItem, player, i, j);
          }
        }
      }
    }
    // Write all changes to the DB in a transaction
    if (entryToAmountMap.size() != 0)
    {
      auto trans = CharacterDatabase.BeginTransaction();
      for (std::pair<uint32, uint32> mapEntry : entryToAmountMap)
      {
        uint32 itemEntry = mapEntry.first;
        uint32 itemAmount = mapEntry.second;
        uint32 itemSubclass = entryToSubclassMap.find(itemEntry)->second;
        trans->Append("REPLACE INTO mod_reagent_bank_account (account_id, guid, item_entry, item_subclass, amount) VALUES ({}, {}, {}, {}, {})",
                      accountKey, guidKey, itemEntry, itemSubclass, itemAmount);
      }
      CharacterDatabase.CommitTransaction(trans);
    }
    // Feedback to player
    if (itemsAddedMap.size() != 0)
    {
      ChatHandler(player->GetSession())
          .SendSysMessage("The following was deposited:");
      for (std::pair<uint32, uint32> mapEntry : itemsAddedMap)
      {
        uint32 itemEntry = mapEntry.first;
        uint32 itemAmount = mapEntry.second;
        ItemTemplate const *itemTemplate =
            sObjectMgr->GetItemTemplate(itemEntry);
        std::string itemName = itemTemplate->Name1;
        ChatHandler(player->GetSession())
            .SendSysMessage(std::to_string(itemAmount) + " " + itemName);
      }
    }
    else
    {
      ChatHandler(player->GetSession())
          .PSendSysMessage("No reagents to deposit in this category.");
    }
    CloseGossipMenuFor(player);
  }

  // Helper: Withdraw all items in a category for the player
  void WithdrawAllInCategory(Player *player, uint32 item_subclass)
  {
    uint32 accountKey, guidKey;
    GetStorageKeys(player, accountKey, guidKey);
    QueryResult result = CharacterDatabase.Query(
        "SELECT item_entry, amount FROM mod_reagent_bank_account WHERE account_id = {} AND guid = {} AND item_subclass = {}",
        accountKey, guidKey, item_subclass);

    if (!result)
    {
      ChatHandler(player->GetSession())
          .PSendSysMessage("No reagents to withdraw in this category.");
      return;
    }

    bool anyWithdrawn = false;
    do
    {
      uint32 itemEntry = (*result)[0].Get<uint32>();
      uint32 amount = (*result)[1].Get<uint32>();
      const ItemTemplate *temp = sObjectMgr->GetItemTemplate(itemEntry);
      if (!temp)
        continue;

      uint32 stackSize = temp->GetMaxStackSize();
      uint32 remaining = amount;
      while (remaining > 0)
      {
        uint32 toGive = std::min(stackSize, remaining);
        ItemPosCountVec dest;
        InventoryResult msg = player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest,
                                                      itemEntry, toGive);
        if (msg == EQUIP_ERR_OK)
        {
          // Remove or update the reagent in the DB
          if (toGive == remaining)
            CharacterDatabase.DirectExecute(
                "DELETE FROM mod_reagent_bank_account WHERE account_id = {} AND guid = {} AND item_entry = {}",
                accountKey, guidKey, itemEntry);
          else
            CharacterDatabase.DirectExecute(
                "UPDATE mod_reagent_bank_account SET amount = {} WHERE account_id = {} AND guid = {} AND item_entry = {}",
                remaining - toGive, accountKey, guidKey, itemEntry);

          Item *item = player->StoreNewItem(dest, itemEntry, true);
          player->SendNewItem(item, toGive, true, false);
          ChatHandler(player->GetSession())
              .PSendSysMessage("Withdrew {} x {}.", toGive,
                               temp->Name1);
          anyWithdrawn = true;
          remaining -= toGive;
        }
        else
        {
          player->SendEquipError(msg, nullptr, nullptr, itemEntry);
          ChatHandler(player->GetSession())
              .PSendSysMessage("Not enough bag space to withdraw {} x {}.",
                               toGive, temp->Name1);
          break;
        }
      }
    } while (result->NextRow());

    if (!anyWithdrawn)
      ChatHandler(player->GetSession())
          .PSendSysMessage("No reagents withdrawn.");
  }

public:
  // Constructor: reads config for max options per page
  mod_reagent_bank_account() : CreatureScript("mod_reagent_bank_account")
  {
    g_maxOptionsPerPage = sConfigMgr->GetOption<uint32>(
        "ReagentBankAccount.MaxOptionsPerPage", DEFAULT_MAX_OPTIONS);
    g_accountWideReagentBank =
        sConfigMgr->GetOption<bool>("ReagentBankAccount.AccountWide", false);
  }

  // Main menu for the reagent banker NPC
  bool OnGossipHello(Player *player, Creature *creature) override
  {
    constexpr int MAIN_ICON_SIZE = 24;
    constexpr int MAIN_ICON_X = 0;
    constexpr int MAIN_ICON_Y = 0;
    constexpr int GOSSIP_ICON_NONE = 0;

    AddGossipItemFor(player, GOSSIP_ICON_NONE, "Deposit All Reagents",
                     DEPOSIT_ALL_REAGENTS, 0);
    AddGossipItemFor(player, GOSSIP_ICON_NONE, "Withdraw All Reagents",
                     WITHDRAW_ALL_REAGENTS, 0);
    AddGossipItemFor(player, GOSSIP_ICON_NONE,
                     GetCachedItemIcon(2589, MAIN_ICON_SIZE, MAIN_ICON_SIZE,
                                       MAIN_ICON_X, MAIN_ICON_Y) +
                         "Cloth",
                     ITEM_SUBCLASS_CLOTH, 0);
    AddGossipItemFor(player, GOSSIP_ICON_NONE,
                     GetCachedItemIcon(12208, MAIN_ICON_SIZE, MAIN_ICON_SIZE,
                                       MAIN_ICON_X, MAIN_ICON_Y) +
                         "Meat",
                     ITEM_SUBCLASS_MEAT, 0);
    AddGossipItemFor(player, GOSSIP_ICON_NONE,
                     GetCachedItemIcon(2772, MAIN_ICON_SIZE, MAIN_ICON_SIZE,
                                       MAIN_ICON_X, MAIN_ICON_Y) +
                         "Metal & Stone",
                     ITEM_SUBCLASS_METAL_STONE, 0);
    AddGossipItemFor(player, GOSSIP_ICON_NONE,
                     GetCachedItemIcon(10940, MAIN_ICON_SIZE, MAIN_ICON_SIZE,
                                       MAIN_ICON_X, MAIN_ICON_Y) +
                         "Enchanting",
                     ITEM_SUBCLASS_ENCHANTING, 0);
    AddGossipItemFor(player, GOSSIP_ICON_NONE,
                     GetCachedItemIcon(7068, MAIN_ICON_SIZE, MAIN_ICON_SIZE,
                                       MAIN_ICON_X, MAIN_ICON_Y) +
                         "Elemental",
                     ITEM_SUBCLASS_ELEMENTAL, 0);
    AddGossipItemFor(player, GOSSIP_ICON_NONE,
                     GetCachedItemIcon(4359, MAIN_ICON_SIZE, MAIN_ICON_SIZE,
                                       MAIN_ICON_X, MAIN_ICON_Y) +
                         "Parts",
                     ITEM_SUBCLASS_PARTS, 0);
    AddGossipItemFor(player, GOSSIP_ICON_NONE,
                     GetCachedItemIcon(2604, MAIN_ICON_SIZE, MAIN_ICON_SIZE,
                                       MAIN_ICON_X, MAIN_ICON_Y) +
                         "Other Trade Goods",
                     ITEM_SUBCLASS_TRADE_GOODS_OTHER, 0);
    AddGossipItemFor(player, GOSSIP_ICON_NONE,
                     GetCachedItemIcon(2453, MAIN_ICON_SIZE, MAIN_ICON_SIZE,
                                       MAIN_ICON_X, MAIN_ICON_Y) +
                         "Herb",
                     ITEM_SUBCLASS_HERB, 0);
    AddGossipItemFor(player, GOSSIP_ICON_NONE,
                     GetCachedItemIcon(2318, MAIN_ICON_SIZE, MAIN_ICON_SIZE,
                                       MAIN_ICON_X, MAIN_ICON_Y) +
                         "Leather",
                     ITEM_SUBCLASS_LEATHER, 0);
    AddGossipItemFor(player, GOSSIP_ICON_NONE,
                     GetCachedItemIcon(1206, MAIN_ICON_SIZE, MAIN_ICON_SIZE,
                                       MAIN_ICON_X, MAIN_ICON_Y) +
                         "Jewelcrafting",
                     ITEM_SUBCLASS_JEWELCRAFTING, 0);
    AddGossipItemFor(player, GOSSIP_ICON_NONE,
                     GetCachedItemIcon(4358, MAIN_ICON_SIZE, MAIN_ICON_SIZE,
                                       MAIN_ICON_X, MAIN_ICON_Y) +
                         "Explosives",
                     ITEM_SUBCLASS_EXPLOSIVES, 0);
    AddGossipItemFor(player, GOSSIP_ICON_NONE,
                     GetCachedItemIcon(4388, MAIN_ICON_SIZE, MAIN_ICON_SIZE,
                                       MAIN_ICON_X, MAIN_ICON_Y) +
                         "Devices",
                     ITEM_SUBCLASS_DEVICES, 0);
    AddGossipItemFor(player, GOSSIP_ICON_NONE,
                     GetCachedItemIcon(23572, MAIN_ICON_SIZE, MAIN_ICON_SIZE,
                                       MAIN_ICON_X, MAIN_ICON_Y) +
                         "Nether Material",
                     ITEM_SUBCLASS_MATERIAL, 0);
    AddGossipItemFor(player, GOSSIP_ICON_NONE,
                     GetCachedItemIcon(38682, MAIN_ICON_SIZE, MAIN_ICON_SIZE,
                                       MAIN_ICON_X, MAIN_ICON_Y) +
                         "Armor Vellum",
                     ITEM_SUBCLASS_ARMOR_ENCHANTMENT, 0);
    AddGossipItemFor(player, GOSSIP_ICON_NONE,
                     GetCachedItemIcon(39349, MAIN_ICON_SIZE, MAIN_ICON_SIZE,
                                       MAIN_ICON_X, MAIN_ICON_Y) +
                         "Weapon Vellum",
                     ITEM_SUBCLASS_WEAPON_ENCHANTMENT, 0);

    SendGossipMenuFor(player, NPC_TEXT_ID, creature->GetGUID());
    return true;
  }

  // Handles menu selections and confirmation dialogs
  bool OnGossipSelect(Player *player, Creature *creature, uint32 item_subclass,
                      uint32 gossipPageNumber) override
  {
    player->PlayerTalkClass->ClearMenus();

    if (item_subclass == DEPOSIT_ALL_REAGENTS)
    {
      if (gossipPageNumber == 0)
      {
        // Main menu: deposit all categories
        DepositAllReagents(player);
      }
      else
      {
        // Category menu: deposit only this category
        DepositAllReagentsForCategory(player, gossipPageNumber);
      }
      return true;
    }
    else if (item_subclass == WITHDRAW_ALL_REAGENTS)
    {
      if (gossipPageNumber == 0)
      {
        // Main menu: withdraw all categories
        std::vector<uint32> subclasses = {ITEM_SUBCLASS_CLOTH,
                                          ITEM_SUBCLASS_MEAT,
                                          ITEM_SUBCLASS_METAL_STONE,
                                          ITEM_SUBCLASS_ENCHANTING,
                                          ITEM_SUBCLASS_ELEMENTAL,
                                          ITEM_SUBCLASS_PARTS,
                                          ITEM_SUBCLASS_TRADE_GOODS_OTHER,
                                          ITEM_SUBCLASS_HERB,
                                          ITEM_SUBCLASS_LEATHER,
                                          ITEM_SUBCLASS_JEWELCRAFTING,
                                          ITEM_SUBCLASS_EXPLOSIVES,
                                          ITEM_SUBCLASS_DEVICES,
                                          ITEM_SUBCLASS_MATERIAL,
                                          ITEM_SUBCLASS_ARMOR_ENCHANTMENT,
                                          ITEM_SUBCLASS_WEAPON_ENCHANTMENT};
        for (uint32 subclass : subclasses)
          WithdrawAllInCategory(player, subclass);
      }
      else
      {
        // Category menu: withdraw only this category
        WithdrawAllInCategory(player, gossipPageNumber);
      }
      CloseGossipMenuFor(player);
      return true;
    }
    else if (item_subclass == MAIN_MENU)
    {
      OnGossipHello(player, creature);
      return true;
    }
    else if (IsCategory(item_subclass))
    {
      // A category was selected (or changing pages inside it)
      ShowReagentItems(player, creature, item_subclass, gossipPageNumber);
      return true;
    }
    else
    {
      // Check if this is one of the submenu actions
      uint32 guidLow = player->GetGUID().GetCounter();
      if (item_subclass == ACTION_WITHDRAW_ONE || item_subclass == ACTION_WITHDRAW_STACK || item_subclass == ACTION_WITHDRAW_ALL)
      {
        uint32 itemEntry = gossipPageNumber; // action stores item entry in this branch
        // Retrieve last category/page (fallback to main menu if missing)
        auto it = m_lastCategoryPage.find(guidLow);
        uint32 category = 0;
        uint16 pageIndex = 0;
        if (it != m_lastCategoryPage.end())
        {
          category = it->second.first;
          pageIndex = it->second.second;
        }
        if (item_subclass == ACTION_WITHDRAW_ONE)
          WithdrawOne(player, itemEntry);
        else if (item_subclass == ACTION_WITHDRAW_STACK)
          WithdrawStack(player, itemEntry);
        else if (item_subclass == ACTION_WITHDRAW_ALL)
          WithdrawAllOfItem(player, itemEntry);
        if (IsCategory(category))
          ShowReagentItems(player, creature, category, pageIndex);
        else
          OnGossipHello(player, creature);
        return true;
      }
      // Otherwise treat it as an item entry -> show submenu
      uint32 itemEntry = item_subclass;
      const ItemTemplate *temp = sObjectMgr->GetItemTemplate(itemEntry);
      if (!temp)
      {
        OnGossipHello(player, creature);
        return true;
      }
      uint32 cat = (temp->Class == ITEM_CLASS_GEM) ? ITEM_SUBCLASS_JEWELCRAFTING : temp->SubClass;
      m_lastCategoryPage[guidLow] = {cat, (uint16)gossipPageNumber};
      ShowItemWithdrawMenu(player, creature, cat, (uint16)gossipPageNumber, itemEntry);
      return true;
    }
  }

  // Shows the list of stored reagents for a category, with pagination
  void ShowReagentItems(Player *player, Creature *creature,
                        uint32 item_subclass, uint16 gossipPageNumber)
  {
    WorldSession *session = player->GetSession();
    uint32 accountKey, guidKey;
    GetStorageKeys(player, accountKey, guidKey);
    std::string query = "SELECT item_entry, amount FROM mod_reagent_bank_account WHERE account_id = " + std::to_string(accountKey) + " AND guid = " + std::to_string(guidKey) + " AND item_subclass = " + std::to_string(item_subclass);
    session->GetQueryProcessor().AddCallback(CharacterDatabase.AsyncQuery(query).WithCallback([=, this](QueryResult result)
                                                                                              {
      // Build arrays first
      std::map<uint32, uint32> entryToAmountMap;
      std::vector<uint32> itemEntries;
      uint32 totalAmount = 0;
      if (result) {
        do {
          uint32 itemEntry = (*result)[0].Get<uint32>();
          uint32 itemAmount = (*result)[1].Get<uint32>();
          entryToAmountMap[itemEntry] = itemAmount;
          itemEntries.push_back(itemEntry);
          totalAmount += itemAmount;
        } while (result->NextRow());
      }

      auto getSortKey = [this, session](uint32 entry) {
        std::string name = GetItemName(entry, session);
        std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        return name;
      };
      std::sort(itemEntries.begin(), itemEntries.end(),
                [getSortKey](uint32 lhs, uint32 rhs) {
                  std::string lhsKey = getSortKey(lhs);
                  std::string rhsKey = getSortKey(rhs);
                  if (lhsKey == rhsKey)
                    return lhs < rhs;
                  return lhsKey < rhsKey;
                });

      uint32 totalItems = itemEntries.size();
      uint32 totalPages = (totalItems == 0) ? 1 : ((totalItems - 1) / g_maxOptionsPerPage) + 1;
      uint32 clampedPageIndex = gossipPageNumber;
      if (clampedPageIndex >= totalPages)
        clampedPageIndex = (totalPages == 0 ? 0 : totalPages - 1);
      uint32 startValue = clampedPageIndex * g_maxOptionsPerPage;
      uint32 endValue = (clampedPageIndex + 1) * g_maxOptionsPerPage - 1;
      uint32 currentPage = clampedPageIndex + 1;
      uint32 effectivePageNumber = clampedPageIndex;

      // Category name
      std::string categoryName;
      switch (item_subclass) {
      case ITEM_SUBCLASS_CLOTH: categoryName = "Cloth"; break;
      case ITEM_SUBCLASS_MEAT: categoryName = "Meat"; break;
      case ITEM_SUBCLASS_METAL_STONE: categoryName = "Metal & Stone"; break;
      case ITEM_SUBCLASS_ENCHANTING: categoryName = "Enchanting"; break;
      case ITEM_SUBCLASS_ELEMENTAL: categoryName = "Elemental"; break;
      case ITEM_SUBCLASS_PARTS: categoryName = "Parts"; break;
      case ITEM_SUBCLASS_TRADE_GOODS_OTHER: categoryName = "Other Trade Goods"; break;
      case ITEM_SUBCLASS_HERB: categoryName = "Herb"; break;
      case ITEM_SUBCLASS_LEATHER: categoryName = "Leather"; break;
      case ITEM_SUBCLASS_JEWELCRAFTING: categoryName = "Jewelcrafting"; break;
      case ITEM_SUBCLASS_EXPLOSIVES: categoryName = "Explosives"; break;
      case ITEM_SUBCLASS_DEVICES: categoryName = "Devices"; break;
      case ITEM_SUBCLASS_MATERIAL: categoryName = "Nether Material"; break;
      case ITEM_SUBCLASS_ARMOR_ENCHANTMENT: categoryName = "Armor Vellum"; break;
      case ITEM_SUBCLASS_WEAPON_ENCHANTMENT: categoryName = "Weapon Vellum"; break;
      default: categoryName = "Reagents"; break;
      }

      constexpr int ICON_SIZE = 18;
      constexpr int ICON_X = 0;
      constexpr int ICON_Y = 0;
      constexpr int GOSSIP_ICON_NONE = 0;

      AddGossipItemFor(player, GOSSIP_ICON_NONE, "|cff003366" + categoryName + ": " + std::to_string(totalItems) + " types, " + std::to_string(totalAmount) + " total|r", 0, 0);
      AddGossipItemFor(player, GOSSIP_ICON_NONE, GetCachedItemIcon(2901, ICON_SIZE, ICON_SIZE, ICON_X, ICON_Y) + " |cff1eff00Deposit All|r", DEPOSIT_ALL_REAGENTS, item_subclass);
      AddGossipItemFor(player, GOSSIP_ICON_NONE, GetCachedItemIcon(2901, ICON_SIZE, ICON_SIZE, ICON_X, ICON_Y) + " |cff0070ddWithdraw All|r", WITHDRAW_ALL_REAGENTS, item_subclass);

      if (endValue < entryToAmountMap.size()) {
        AddGossipItemFor(player, GOSSIP_ICON_NONE, GetCachedItemIcon(23705, ICON_SIZE, ICON_SIZE, ICON_X, ICON_Y) + " |cff003366Next Page|r ▶ (" + std::to_string(currentPage + 1) + "/" + std::to_string(totalPages) + ")", item_subclass, effectivePageNumber + 1);
      }
      if (effectivePageNumber > 0) {
        AddGossipItemFor(player, GOSSIP_ICON_NONE, "◀ |cff003366Previous Page|r " + GetCachedItemIcon(23705, ICON_SIZE, ICON_SIZE, ICON_X, ICON_Y) + " (" + std::to_string(currentPage - 1) + "/" + std::to_string(totalPages) + ")", item_subclass, effectivePageNumber - 1);
      }

      for (uint32 i = startValue; i <= endValue; i++) {
        if (itemEntries.empty() || i > itemEntries.size() - 1)
          break;
        uint32 itemEntry = itemEntries.at(i);
        uint32 amount = entryToAmountMap.find(itemEntry)->second;
        std::string link = GetItemLink(itemEntry, session);
        std::string icon = GetCachedItemIcon(itemEntry, ICON_SIZE, ICON_SIZE, ICON_X, ICON_Y);
        AddGossipItemFor(player, GOSSIP_ICON_NONE, icon + link + " |cff000000x " + std::to_string(amount) + "|r", itemEntry, effectivePageNumber);
      }

      AddGossipItemFor(player, GOSSIP_ICON_NONE, GetCachedItemIcon(6948, ICON_SIZE, ICON_SIZE, ICON_X, ICON_Y) + " |cff666666Back to Categories|r", MAIN_MENU, 0);
      SendGossipMenuFor(player, NPC_TEXT_ID, creature->GetGUID()); }));
  }
};

// Add all scripts in one
void AddSC_mod_reagent_bank_account() { new mod_reagent_bank_account(); }
