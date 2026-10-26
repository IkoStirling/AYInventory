#include <AYInventory/WorldEquipmentCommit.h>
#include <AYAbility/Ability.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace i = ayt::inventory;
namespace s = ayt::stats;
namespace a = ayt::ability;
namespace fs = std::filesystem;

namespace {
void check(bool condition, const char* message)
{ if (!condition) throw std::runtime_error(message); }
template<class T, class E> void check(const std::expected<T, E>& result,
                                     const char* message)
{ check(result.has_value(), message); }

std::shared_ptr<const a::Catalog> catalog()
{
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
    check(built, "ability catalog");
    return *built;
}
s::StatSet stats()
{
    s::Definition attack{.name="attack",.mode=s::Mode::Advanced,
        .initial=10,.zones={{1,s::ZoneOp::Flat}}};
    auto schema = s::Schema::compile({attack});
    check(schema, "stat schema");
    auto set = s::StatSet::create(*schema);
    check(set, "stat set");
    return std::move(*set);
}
i::Store store()
{
    i::Definition sword;
    sword.id = "sword";
    sword.equipSlots = {"hand"};
    sword.abilityConfiguration = 101;
    sword.statBonuses = {{s::statId("attack"), 5}};
    auto built = i::Store::create(1, {sword});
    check(built, "item catalog");
    auto value = std::move(*built);
    check(value.addEquipmentOwner({"hero", {"hand"}}), "hero equipment owner");
    check(value.createItem({"sword-1", "sword", 1,
        {i::Place::World, "11111111111111111111111111111111", {}}}),
        "world sword");
    return value;
}
const i::ModifierFactory factory = [](const i::Store::EquippedProfile& profile,
                                       std::size_t index) {
    const auto& bonus = profile.statBonuses[index];
    return s::Modifier{.handle=profile.abilityBindingId*16+index+1,
        .effect=profile.abilityBindingId,.rule=index+1,
        .target=bonus.statId,.zone=1,.value=bonus.flat};
};

struct PickupHost final : i::IDurablePickupHost {
    bool present = true;
    i::Result<void> validateCurrent(const i::InteractionCandidate&,
                                    std::string_view) override { return {}; }
    i::Result<void> authorize(const i::InteractionCandidate&,
                              std::string_view) override { return {}; }
    i::Result<void> consumeWorldPickup(const i::InteractionCandidate&) override
    { return std::unexpected(i::Diagnostic{i::Error::Invalid, "wrong path"}); }
    i::Result<void> cleanupCommittedPickup(const i::InteractionCandidate&) override
    { present = false; return {}; }
};
}

int main()
{
    try {
        const fs::path root = fs::path(AY_INVENTORY_WORLD_EQUIPMENT_TEST_ROOT);
        std::error_code ec;
        fs::remove_all(root, ec);
        fs::create_directories(root, ec);
        check(!ec, "save root");
        const fs::path blocked = root / "blocked";
        std::ofstream(blocked) << "file";
        ayt::app::SaveGameService unavailable({blocked.string(), "equipment"});
        ayt::app::SaveGameService saves({root.string(), "equipment"});
        auto activeStore = store();
        ayt::app::WorldStateLedger ledger;
        auto activeStats = stats();
        a::Runtime activeAbility(catalog());
        const a::Identity hero{1,10,1}, sword{1,1,1};
        auto activeProjection = i::EquipmentProjection::create(hero, 1);
        check(activeProjection, "equipment projection");
        i::WorldEquipmentCommitAdapter adapter(activeStats, activeAbility,
            *activeProjection, "hero", "sword-1", "hero-equipment", factory);
        const i::InteractionCandidate pickup{i::InteractionCandidate::Kind::Pickup,
            "11111111111111111111111111111111", "sword-1", 1.0};
        const i::Location hand{i::Place::Equipment, "hero", "hand"};
        PickupHost host;
        i::WorldItemCommit blockedCommit(activeStore, ledger, unavailable, "slot");
        check(!blockedCommit.pickup(host, pickup, "hero", hand,
            "equip-sword-1", "equip/sword-1", adapter.extension()),
            "save failure rejects direct equipment pickup");
        check(activeStore.findItem("sword-1")->location.place == i::Place::World
            && activeStats.read(s::statId("attack"))->effective == 10
            && !activeAbility.equipment(sword) && host.present,
            "save fault preserves Store, Stats, Ability and World");

        i::WorldItemCommit commit(activeStore, ledger, saves, "slot");
        auto picked = commit.pickup(host, pickup, "hero", hand,
            "equip-sword-1", "equip/sword-1", adapter.extension());
        check(picked && picked->status == i::WorldCommitStatus::Committed,
            "direct equipment pickup committed");
        check(activeStore.findItem("sword-1")->location == hand
            && activeStats.read(s::statId("attack"))->effective == 15
            && activeAbility.equipped(hero)->value() == sword && !host.present,
            "live equipment effects and World agree");

        auto restoredStore = store();
        ayt::app::WorldStateLedger restoredLedger;
        auto restoredStats = stats();
        a::Runtime restoredAbility(catalog());
        auto restoredProjection = i::EquipmentProjection::create(hero, 1);
        check(restoredProjection, "restart projection");
        const i::ModifierFactory wrong = [](const i::Store::EquippedProfile& profile,
                                             std::size_t index) {
            auto modifier = factory(profile, index);
            ++modifier.value;
            return modifier;
        };
        i::WorldEquipmentCommitAdapter wrongAdapter(restoredStats,
            restoredAbility, *restoredProjection, "hero", "sword-1",
            "hero-equipment", wrong);
        check(!i::restoreWorldItemCommitSlot(restoredStore, restoredLedger,
            saves, "slot",
            [&](ayt::app::SaveParticipantBundle& bundle, std::string& error) {
                return wrongAdapter.registerRestoreSections(bundle, error);
            }, {},
            [&](const i::Store& candidate,
                const ayt::app::WorldStateLedger& candidateLedger,
                std::string& error) {
                return wrongAdapter.prepareRestored(candidate, candidateLedger,
                                                    error);
            }, [&](std::string& error) {
                return wrongAdapter.publishRestoredChecked(error);
            }), "mismatched projection policy rejects restore");
        check(restoredStore.findItem("sword-1")->location.place
                == i::Place::World
            && restoredStats.read(s::statId("attack"))->effective == 10
            && !restoredAbility.equipment(sword),
            "failed restore leaves all live owners old");

        i::WorldEquipmentCommitAdapter restoredAdapter(restoredStats,
            restoredAbility, *restoredProjection, "hero", "sword-1",
            "hero-equipment", factory);
        check(i::restoreWorldItemCommitSlot(restoredStore, restoredLedger,
            saves, "slot",
            [&](ayt::app::SaveParticipantBundle& bundle, std::string& error) {
                return restoredAdapter.registerRestoreSections(bundle, error);
            }, {},
            [&](const i::Store& candidate,
                const ayt::app::WorldStateLedger& candidateLedger,
                std::string& error) {
                return restoredAdapter.prepareRestored(candidate,
                    candidateLedger, error);
            }, [&](std::string& error) {
                return restoredAdapter.publishRestoredChecked(error);
            }), "restore direct equipment pickup");
        check(restoredStore.findItem("sword-1")->location == hand
            && restoredStats.read(s::statId("attack"))->effective == 15
            && restoredAbility.equipped(hero)->value() == sword
            && restoredLedger.hasReceipt("equip-sword-1"),
            "restart recovers equipment owner, modifier, Ability and receipt");
        // The recovered projection knows which modifier to remove.
        const i::Location bag{i::Place::Container, "bag", "slot"};
        check(restoredStore.addContainer({"bag", {"slot"}}), "stow bag");
        check(restoredStore.transfer("sword-1", hand, bag, "stow-after-load"),
              "stow after restore");
        check(restoredProjection->sync(restoredStore, "hero", "", restoredStats,
                                       restoredAbility, factory),
              "stow recovered projection");
        check(restoredStats.read(s::statId("attack"))->effective == 10
            && !restoredAbility.equipped(hero)->has_value(),
            "restored projection removes equipment effects exactly once");
        fs::remove_all(root, ec);
        std::cout << "AYInventory: durable equipment pickup and restart passed\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "AYInventory World equipment commit: " << ex.what() << '\n';
        return 1;
    }
}
