#include <AYInventory/Inventory.h>

#include <iostream>

using namespace ayt::inventory;

int main()
{
    Definition sword;
    sword.id = "practice-sword";
    sword.equipSlots = {"hand"};
    sword.abilityConfiguration = 7; // Existing Ability catalog configuration.
    sword.statBonuses.push_back({42, 3}); // Existing Stats stat ID and flat bonus.
    auto inventory = Store::create(1, {sword});
    if (!inventory || !inventory->addContainer({"player/bag", {"slot-1"}})
        || !inventory->addEquipmentOwner({"player", {"hand"}})) return 1;
    const Location drop{Place::World, "field/drop-1", ""};
    const Location bag{Place::Container, "player/bag", "slot-1"};
    const Location hand{Place::Equipment, "player", "hand"};
    if (!inventory->createItem({"sword-instance-1", "practice-sword", 1, drop})
        || !inventory->transfer("sword-instance-1", drop, bag, "pickup-1")
        || !inventory->transfer("sword-instance-1", bag, hand, "equip-1")) return 2;
    const auto profiles = inventory->equippedProfiles("player");
    if (profiles.size() != 1) return 3;
    std::cout << profiles.front().itemId << " equipped in " << profiles.front().slot
              << ", ability config " << profiles.front().abilityConfiguration << '\n';
}
