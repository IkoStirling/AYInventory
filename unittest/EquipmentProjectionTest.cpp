#include <AYInventory/EquipmentProjection.h>

#include <array>
#include <iostream>
#include <stdexcept>

namespace i = ayt::inventory;
namespace s = ayt::stats;
namespace a = ayt::ability;

namespace {
void check(bool okay, const char* why) { if (!okay) throw std::runtime_error(why); }
std::shared_ptr<const a::Catalog> catalog() {
    a::ActionDefinition action{.id=10,.behaviorId=a::fire::Behavior,.defaults={
        {a::fire::Amount,a::Unit::Amount,10},
        {a::fire::IntervalTicks,a::Unit::Ticks,4},
        {a::fire::MinimumIntervalTicks,a::Unit::Ticks,2},
        {a::fire::IntervalStepTicks,a::Unit::Ticks,1},
        {a::fire::StreakResetTicks,a::Unit::Ticks,4},
        {a::fire::MagazineCapacity,a::Unit::Count,6},
        {a::fire::ReloadTicks,a::Unit::Ticks,3},
        {a::fire::Projectiles,a::Unit::Count,1},
        {a::fire::SkillCooldownTicks,a::Unit::Ticks,0}}};
    a::EquipmentActionConfig equipment{.id=101,.action=10};
    auto built = a::Catalog::compile({action}, {equipment}, {});
    check(bool(built), "ability catalog");
    return *built;
}
s::StatSet stats() {
    s::Definition attack{.name="attack",.mode=s::Mode::Advanced,
        .initial=10,.zones={{1,s::ZoneOp::Flat}}};
    auto schema = s::Schema::compile({attack});
    check(bool(schema), "stat schema");
    auto set = s::StatSet::create(*schema);
    check(bool(set), "stat set");
    return std::move(*set);
}
}

int main() {
  try {
    i::Definition sword;
    sword.id="sword"; sword.equipSlots={"hand"};
    sword.abilityConfiguration=101;
    sword.statBonuses={{s::statId("attack"),5}};
    auto created = i::Store::create(1,{sword});
    check(bool(created), "inventory catalog");
    auto store=std::move(*created);
    check(bool(store.addEquipmentOwner({"hero",{"hand"}})), "hero slot");
    check(bool(store.addEquipmentOwner({"ally",{"hand"}})), "ally slot");
    check(bool(store.addContainer({"bag",{"a"}})), "bag slot");
    const i::Location drop{i::Place::World,"drop",""};
    const i::Location heroHand{i::Place::Equipment,"hero","hand"};
    const i::Location allyHand{i::Place::Equipment,"ally","hand"};
    const i::Location bag{i::Place::Container,"bag","a"};
    check(bool(store.createItem({"sword-1","sword",1,drop})), "spawn item");
    const a::Identity hero{1,10,1}, ally{1,20,1};
    auto heroProjection=i::EquipmentProjection::create(hero,1);
    auto allyProjection=i::EquipmentProjection::create(ally,1);
    check(bool(heroProjection) && bool(allyProjection), "projections");
    auto heroStats=stats(), allyStats=stats();
    a::Runtime runtime(catalog());
    const i::ModifierFactory factory=[](const i::Store::EquippedProfile& profile,
        std::size_t index) {
        const auto& bonus=profile.statBonuses[index];
        return s::Modifier{.handle=profile.abilityBindingId*16+index+1,
            .effect=profile.abilityBindingId,.rule=index+1,
            .target=bonus.statId,.zone=1,.value=bonus.flat};
    };
    const a::Identity swordId{1,1,1};
    check(bool(store.transfer("sword-1",drop,heroHand,"equip-1")), "hero equip transfer");
    auto initial=heroProjection->sync(store,"hero","sword-1",heroStats,runtime,factory);
    if (!initial) std::cerr << initial.error().message << '\n';
    check(bool(initial), "hero projection");
    check(heroStats.read(s::statId("attack"))->effective==15, "hero bonus");
    check(runtime.equipped(hero)->value()==swordId, "hero selected sword");
    check(bool(runtime.fire({hero,1})), "hero shot");
    const auto ammo=runtime.equipment(swordId)->ammunition;
    check(bool(store.transfer("sword-1",heroHand,bag,"stow-1")), "stow");
    check(bool(heroProjection->sync(store,"hero","",heroStats,runtime,factory)), "stow projection");
    check(heroStats.read(s::statId("attack"))->effective==10, "stowed bonus removed");
    check(!runtime.equipped(hero)->has_value() && runtime.equipment(swordId)->ammunition==ammo,
        "stowed ability retains ammo");
    check(bool(store.transfer("sword-1",bag,heroHand,"reequip-1")), "reequip");
    check(bool(heroProjection->sync(store,"hero","sword-1",heroStats,runtime,factory)), "reequip projection");
    check(runtime.equipment(swordId)->ammunition==ammo, "ammo survives re-equip");

    const i::ModifierFactory bad=[](const i::Store::EquippedProfile& profile,
        std::size_t index) {
        return s::Modifier{.handle=99,.effect=99,.rule=1,
            .target=profile.statBonuses[index].statId,.zone=1,.value=999};
    };
    const auto version=heroStats.version();
    check(!heroProjection->sync(store,"hero","sword-1",heroStats,runtime,bad),
        "invalid modifier rejected");
    check(heroStats.version()==version && runtime.equipment(swordId)->ammunition==ammo,
        "failed projection preserves both runtimes");

    const i::ModifierFactory wrongZone=[](const i::Store::EquippedProfile& profile,
        std::size_t index) {
        const auto& bonus=profile.statBonuses[index];
        return s::Modifier{.handle=17,.effect=1,.rule=1,
            .target=bonus.statId,.zone=99,.value=bonus.flat};
    };
    const std::array<i::EquipmentSyncTarget,2> rejectedTargets{{
        {&*heroProjection,"hero","",&heroStats,factory},
        {&*allyProjection,"ally","sword-1",&allyStats,wrongZone}}};
    check(!i::transferAndSyncEquipment(store,"sword-1",heroHand,allyHand,
            "give-1",runtime,rejectedTargets),
        "invalid destination Stats refuses entire transfer");
    check(store.findItem("sword-1")->location==heroHand
        && heroStats.read(s::statId("attack"))->effective==15
        && allyStats.read(s::statId("attack"))->effective==10
        && runtime.equipment(swordId)->owner==hero,
        "failed transfer preserves Store and both actors");
    const std::array<i::EquipmentSyncTarget,2> acceptedTargets{{
        {&*heroProjection,"hero","",&heroStats,factory},
        {&*allyProjection,"ally","sword-1",&allyStats,factory}}};
    check(!i::transferAndSyncEquipment(store,"sword-1",heroHand,allyHand,
            "give-1",runtime,std::span(acceptedTargets).subspan(0,1)),
        "destination actor projection is required");
    check(store.findItem("sword-1")->location==heroHand
        && heroStats.read(s::statId("attack"))->effective==15,
        "missing target leaves all owners unchanged");
    check(bool(i::transferAndSyncEquipment(store,"sword-1",heroHand,allyHand,
            "give-1",runtime,acceptedTargets)), "atomic give to ally");
    check(store.findItem("sword-1")->location==allyHand
        && heroStats.read(s::statId("attack"))->effective==10,
        "source effects clear with ownership transfer");
    check(runtime.equipment(swordId)->owner==ally
        && runtime.equipment(swordId)->ammunition==ammo
        && allyStats.read(s::statId("attack"))->effective==15, "transfer keeps state");
    check(bool(i::transferAndSyncEquipment(store,"sword-1",heroHand,allyHand,
            "give-1",runtime,acceptedTargets)), "exact retry is harmless");
    check(runtime.equipment(swordId)->owner==ally
        && allyStats.read(s::statId("attack"))->effective==15,
        "retry does not duplicate modifiers or equipment");
    check(bool(store.transfer("sword-1",allyHand,bag,"stow-2")), "ally stows");
    check(bool(allyProjection->sync(store,"ally","",allyStats,runtime,factory)), "ally removes effects");
    check(bool(store.destroyItem("sword-1")), "destroy item");
    check(bool(allyProjection->sync(store,"ally","",allyStats,runtime,factory)), "remove retired ability");
    check(!runtime.equipment(swordId), "destroyed item ability gone");
    std::cout << "PASS equipment stats/ability projection, stow, transfer, rejection and destruction\n";
  } catch (const std::exception& ex) {
    std::cerr << "FAIL " << ex.what() << '\n';
    return 1;
  }
}
