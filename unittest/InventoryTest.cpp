#include <AYInventory/Inventory.h>
#ifdef AY_INVENTORY_JSON
#include <AYInventory/SnapshotJson.h>
#endif

#include <iostream>
#include <stdexcept>

using namespace ayt::inventory;

namespace {
void check(bool value, const char* detail)
{
    if (!value) throw std::runtime_error(detail);
}

Store makeStore()
{
    Definition sword;
    sword.id = "iron-sword";
    sword.equipSlots = {"hand"};
    sword.abilityConfiguration = 123;
    sword.statBonuses.push_back({456, 5});
    Definition potion;
    potion.id = "potion";
    potion.maxStack = 10;
    auto store = Store::create(3, {sword, potion});
    check(store.has_value(), "valid catalog");
    check(store->addContainer({"chest", {"a", "b"}}).has_value(), "add chest");
    check(store->addEquipmentOwner({"player", {"hand", "head"}}).has_value(),
          "add actor");
    return std::move(*store);
}

void catalogAndTransfers()
{
    check(!Store::create(1, {{"bad", 0}}), "zero stack rejected");
    check(!Store::create(1, {{"same", 1}, {"same", 1}}), "duplicate type rejected");
    auto store = makeStore();
    const Location drop{Place::World, "scene/drop-1", ""};
    const Location chestA{Place::Container, "chest", "a"};
    const Location chestB{Place::Container, "chest", "b"};
    const Location equipped{Place::Equipment, "player", "hand"};
    check(store.createItem({"sword-1", "iron-sword", 1, drop}).has_value(),
          "create drop");
    check(!store.createItem({"sword-2", "iron-sword", 1, drop}),
          "one item per drop object");
    check(store.transfer("sword-1", drop, chestA, "pickup-1").has_value(),
          "pickup");
    check(store.transfer("sword-1", drop, chestA, "pickup-1").has_value(),
          "exact retry is idempotent");
    check(!store.transfer("sword-1", drop, chestB, "pickup-1"),
          "request reuse with different destination rejected");
    check(!store.transfer("sword-1", drop, equipped, "stale"),
          "stale source rejected");
    check(!store.createItem({"potion-1", "potion", 1, chestA}),
          "container capacity enforced");
    check(store.transfer("sword-1", chestA, equipped, "equip-1").has_value(),
          "equip");
    check(store.equippedProfiles("player").size() == 1, "profile exists");
    const auto profile = store.equippedProfiles("player").front();
    check(profile.abilityConfiguration == 123 && profile.statBonuses.size() == 1
              && profile.statBonuses.front().statId == 456
              && profile.abilityBindingId == 1,
          "ability/stats configuration retained");
    check(!store.createItem({"potion-1", "potion", 1, equipped}),
          "potion cannot equip");
    check(store.findItem("sword-1")->location == equipped, "single owner");
}

void stacksAndOwnerDestruction()
{
    auto store = makeStore();
    const Location drop{Place::World, "scene/potions", ""};
    const Location chestA{Place::Container, "chest", "a"};
    const Location chestB{Place::Container, "chest", "b"};
    check(store.createItem({"potions", "potion", 9, drop}).has_value(), "stack");
    check(store.split("potions", 3, "split", chestA).has_value(), "split");
    check(store.findItem("potions")->quantity == 6, "source reduced");
    check(!store.split("potions", 2, "again", chestA), "occupied split rejected");
    check(store.createItem({"other", "potion", 5, chestB}).has_value(), "target");
    check(!store.merge("potions", "other", 7), "overdraw rejected");
    check(!store.merge("potions", "other", 6), "target stack limit enforced");
    check(store.merge("potions", "other", 4).has_value(), "partial merge");
    check(store.findItem("potions")->quantity == 2, "source remainder");
    check(store.findItem("other")->quantity == 9, "target count");
    check(!store.removeContainer("chest"), "occupied owner cannot vanish");
    check(store.destroyItem("split").has_value(), "explicit dispose A");
    check(store.destroyItem("other").has_value(), "explicit dispose B");
    check(store.removeContainer("chest").has_value(), "empty owner may vanish");
    check(store.findItem("potions") != nullptr, "unrelated world item survives");
}

void restoreAndCorruption()
{
    auto store = makeStore();
    const Location drop{Place::World, "scene/drop", ""};
    const Location chestA{Place::Container, "chest", "a"};
    check(store.createItem({"sword", "iron-sword", 1, drop}).has_value(), "item");
    check(store.transfer("sword", drop, chestA, "pickup").has_value(), "transfer");
    auto saved = store.snapshot();
    check(saved.schemaVersion == 5 && saved.nextAbilityBindingId == 2,
          "Ability binding allocator persisted");
    auto restored = makeStore();
    check(restored.restore(saved).has_value(), "snapshot restore");
    auto version4 = saved;
    version4.schemaVersion = 4;
    check(restored.restore(version4).has_value(), "v4 ownership snapshot remains readable");
    check(restored.findItem("sword")->location == chestA, "restored owner");
    check(restored.findItem("sword")->abilityBindingId == 1,
          "Ability binding identity restored");
    check(restored.transfer("sword", drop, chestA, "pickup").has_value(),
          "retry receipt persists");
    auto corrupt = saved;
    corrupt.items.push_back({"clone", "iron-sword", 1, chestA});
    check(!restored.restore(corrupt), "duplicate owner rejected");
    check(restored.findItem("clone") == nullptr, "failed restore atomic");
    corrupt = saved;
    corrupt.catalogVersion = 4;
    check(!restored.restore(corrupt), "content version mismatch rejected");
    corrupt = saved;
    corrupt.nextAbilityBindingId = 1;
    check(!restored.restore(corrupt), "allocator rewind rejected");
    corrupt = saved;
    corrupt.items.push_back({"second", "iron-sword", 1,
                             {Place::World, "scene/other", ""}, 1});
    check(!restored.restore(corrupt), "duplicate Ability binding rejected");
    check(restored.createItem({"new-sword", "iron-sword", 1,
                               {Place::World, "scene/new", ""}}).has_value(),
          "new binding after restore");
    check(restored.findItem("new-sword")->abilityBindingId == 2,
          "next binding is collision free");
    check(restored.destroyItem("new-sword").has_value(), "retire binding");
    check(restored.createItem({"newer-sword", "iron-sword", 1,
                               {Place::World, "scene/newer", ""}}).has_value(),
          "create after retire");
    check(restored.findItem("newer-sword")->abilityBindingId == 3,
          "retired binding not reused");
#ifdef AY_INVENTORY_JSON
    auto json = encodeSnapshotJson(saved);
    check(json.has_value(), "JSON encode");
    auto decoded = decodeSnapshotJson(*json);
    check(decoded.has_value(), "JSON decode");
    check(restored.restore(*decoded).has_value(), "JSON restore");
    check(!decodeSnapshotJson(R"({"format":"AliyatInventory","schemaVersion":1})"),
          "malformed JSON rejected");
#endif
}

void abilityBindingSurvivesSplit()
{
    Definition kit;
    kit.id = "charged-kit";
    kit.maxStack = 5;
    kit.equipSlots = {"hand"};
    kit.abilityConfiguration = 900;
    auto created = Store::create(1, {kit});
    check(created.has_value(), "binding catalog");
    auto store = std::move(*created);
    check(store.addContainer({"bag", {"a"}}).has_value(), "bag");
    check(store.createItem({"kit-1", "charged-kit", 3,
                            {Place::World, "drop/kit", ""}}).has_value(), "kit");
    check(store.split("kit-1", 1, "kit-2",
                      {Place::Container, "bag", "a"}).has_value(), "split kit");
    check(store.findItem("kit-1")->abilityBindingId == 1
          && store.findItem("kit-2")->abilityBindingId == 2,
          "split gets new Ability identity");
    auto restored = Store::create(1, {kit});
    check(restored.has_value() && restored->restore(store.snapshot()).has_value(),
          "restore split bindings");
    check(restored->findItem("kit-1")->abilityBindingId == 1
          && restored->findItem("kit-2")->abilityBindingId == 2,
          "binding IDs survive snapshot");
}

void retiredInstanceIdsCannotImpersonateOldTransfers()
{
    auto store = makeStore();
    const Location drop{Place::World, "drop/old", ""};
    const Location bag{Place::Container, "chest", "a"};
    check(store.createItem({"sword-old", "iron-sword", 1, drop}).has_value(), "old item");
    check(store.transfer("sword-old", drop, bag, "pickup-old").has_value(), "old pickup");
    check(store.destroyItem("sword-old").has_value(), "destroy old item");
    check(!store.createItem({"sword-old", "iron-sword", 1, bag}),
          "destroyed instance ID cannot be reused");
    check(!store.transfer("sword-old", drop, bag, "pickup-old"),
          "old receipt cannot accept an absent instance");
    check(store.createItem({"new-sword", "iron-sword", 1, bag}).has_value(),
          "distinct identity can use freed slot");
    const auto saved = store.snapshot();
    check(saved.retiredItemIds.contains("sword-old"), "retirement saved");

    auto restored = makeStore();
    check(restored.restore(saved).has_value(), "retirement restored");
    check(!restored.createItem({"sword-old", "iron-sword", 1, drop}),
          "retired ID remains unavailable after restore");
    auto corrupt = saved;
    corrupt.retiredItemIds.insert("new-sword");
    check(!restored.restore(corrupt), "active and retired overlap rejected");
    check(restored.findItem("new-sword") != nullptr,
          "failed restore preserves active ownership");
#ifdef AY_INVENTORY_JSON
    auto encoded = encodeSnapshotJson(saved);
    check(encoded.has_value(), "encode retired identities");
    auto decoded = decodeSnapshotJson(*encoded);
    check(decoded.has_value() && decoded->retiredItemIds == saved.retiredItemIds,
          "JSON retains retired identities");
    check(restored.restore(*decoded).has_value(), "JSON retired restore");
    check(!restored.createItem({"sword-old", "iron-sword", 1, drop}),
          "JSON restore blocks identity reuse");
    check(!decodeSnapshotJson(R"({"format":"AliyatInventory","schemaVersion":4,"catalogVersion":3,"nextAbilityBindingId":1,"containers":[],"actors":[],"items":[],"transfers":[],"crafts":[],"retiredItems":["x","x"]})"),
          "duplicate retired IDs rejected during JSON decode");
#endif
}

void fullMergeRetiresSourceIdentity()
{
    auto store = makeStore();
    const Location first{Place::Container, "chest", "a"};
    const Location second{Place::Container, "chest", "b"};
    check(store.createItem({"stack-source", "potion", 2, first}).has_value(), "source stack");
    check(store.createItem({"stack-target", "potion", 8, second}).has_value(), "target stack");
    check(store.merge("stack-source", "stack-target", 2).has_value(), "full merge");
    check(store.findItem("stack-source") == nullptr
          && store.findItem("stack-target")->quantity == 10,
          "one remaining stack");
    check(store.snapshot().retiredItemIds.contains("stack-source"),
          "merged instance retired");
    check(!store.createItem({"stack-source", "potion", 1, first}),
          "merged instance ID cannot be reused");
}

void attachedPartsFollowEquipmentAndRestore()
{
    Definition frame;
    frame.id = "frame";
    frame.equipSlots = {"hand"};
    frame.abilityConfiguration = 100;
    frame.attachmentSlots = {{"optic", {"scope"}}, {"rail", {"rail"}}};
    Definition scope;
    scope.id = "scope";
    scope.statBonuses.push_back({12, 3});
    scope.attachmentSlots = {{"lens", {"lens"}}};
    Definition rail;
    rail.id = "rail";
    Definition lens;
    lens.id = "lens";
    lens.statBonuses.push_back({13, 2});
    auto created = Store::create(7, {frame, scope, rail, lens});
    check(created.has_value(), "part catalog");
    auto store = std::move(*created);
    check(store.addContainer({"bag", {"a", "b", "c"}}).has_value(), "part bag");
    check(store.addEquipmentOwner({"hero", {"hand"}}).has_value(), "part actor");
    const Location frameInBag{Place::Container, "bag", "a"};
    const Location scopeInBag{Place::Container, "bag", "b"};
    const Location frameEquipped{Place::Equipment, "hero", "hand"};
    const Location scopeAttached{Place::Attachment, "frame-1", "optic"};
    check(store.createItem({"frame-1", "frame", 1, frameInBag}).has_value(), "frame");
    check(store.createItem({"scope-1", "scope", 1, scopeInBag}).has_value(), "scope");
    check(store.createItem({"lens-1", "lens", 1,
                            {Place::Attachment, "scope-1", "lens"}}).has_value(), "lens");
    check(!store.createItem({"wrong", "rail", 1, scopeAttached}),
          "attachment compatibility enforced");
    check(store.transfer("scope-1", scopeInBag, scopeAttached, "attach-optic").has_value(),
          "attach part");
    check(!store.transfer("frame-1", frameInBag,
                          {Place::Attachment, "scope-1", "lens"}, "cycle"),
          "ancestor cycle rejected");
    check(!store.destroyItem("frame-1"), "cannot destroy attached parent");
    const Store beforeEquip = store;
    check(store.transfer("frame-1", frameInBag, frameEquipped, "equip-frame").has_value(),
          "equip assembly");
    const auto equipPlan = equipmentDelta(beforeEquip, store, "hero");
    check(equipPlan.removals.empty() && equipPlan.additions.size() == 3,
          "part effects enter together at Sim boundary");
    const auto profiles = store.equippedProfiles("hero");
    check(profiles.size() == 3, "equipped assembly exposes all profiles");
    check(profiles.at(0).itemId == "frame-1" && profiles.at(0).slot == "hand"
          && profiles.at(0).abilityConfiguration == 100, "root profile");
    check(profiles.at(1).itemId == "lens-1"
          && profiles.at(1).slot == "hand/optic/lens", "nested part profile");
    check(profiles.at(2).itemId == "scope-1"
          && profiles.at(2).slot == "hand/optic", "direct part profile");
    const auto noChange = equipmentDelta(store, store, "hero");
    check(noChange.removals.empty() && noChange.additions.empty(),
          "unchanged assembly emits no effect updates");
    const auto saved = store.snapshot();
    auto restored = Store::create(7, {frame, scope, rail, lens});
    check(restored.has_value() && restored->restore(saved).has_value(), "part snapshot restore");
    check(restored->equippedProfiles("hero").size() == 3, "restored assembly profiles");
    auto broken = saved;
    for (auto& item : broken.items)
        if (item.id == "scope-1") item.location.ownerId = "missing";
    check(!restored->restore(broken), "missing parent rejected");
    check(restored->equippedProfiles("hero").size() == 3,
          "invalid restore preserves assembly");
#ifdef AY_INVENTORY_JSON
    const auto encoded = encodeSnapshotJson(saved);
    check(encoded.has_value(), "part JSON encode");
    const auto decoded = decodeSnapshotJson(*encoded);
    check(decoded.has_value() && restored->restore(*decoded).has_value(),
          "part JSON roundtrip");
#endif
}
} // namespace

int main()
{
    try {
        catalogAndTransfers();
        stacksAndOwnerDestruction();
        restoreAndCorruption();
        abilityBindingSurvivesSplit();
        retiredInstanceIdsCannotImpersonateOldTransfers();
        fullMergeRetiresSourceIdentity();
        attachedPartsFollowEquipmentAndRestore();
        std::cout << "AYInventory: all ownership tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "AYInventory: " << error.what() << '\n';
        return 1;
    }
}
