#include <AYInventory/Inventory.h>
#include <AYInventory/SnapshotJson.h>
#include <AYApplication/WorldStateLedger.h>

#include <iostream>
#include <stdexcept>

using namespace ayt::inventory;

namespace {
void check(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}

Store makeStore()
{
    Definition sword;
    sword.id = "sword";
    sword.equipSlots = {"hand"};
    sword.abilityConfiguration = 11;
    auto result = Store::create(2, {sword});
    check(result.has_value(), "catalog");
    return std::move(*result);
}

ayt::app::WorldStateLedger::Participant bind(Store& store)
{
    ayt::app::WorldStateLedger::Participant participant;
    participant.version = 2;
    participant.capture = [&store](std::string& payload, std::string& error) {
        auto encoded = encodeSnapshotJson(store.snapshot());
        if (!encoded) { error = encoded.error().message; return false; }
        payload = std::move(*encoded);
        return true;
    };
    participant.validate = [&store](std::uint32_t version,
                                    std::string_view payload, std::string& error) {
        if (version != 2) { error = "inventory version"; return false; }
        auto decoded = decodeSnapshotJson(payload);
        if (!decoded) { error = decoded.error().message; return false; }
        auto scratch = store;
        auto restored = scratch.restore(*decoded);
        if (!restored) { error = restored.error().message; return false; }
        return true;
    };
    participant.apply = [&store](std::uint32_t, std::string_view payload,
                                 std::string& error) {
        auto decoded = decodeSnapshotJson(payload);
        if (!decoded) { error = decoded.error().message; return false; }
        auto restored = store.restore(*decoded);
        if (!restored) { error = restored.error().message; return false; }
        return true;
    };
    participant.remove = [](std::string& error) {
        error = "inventory root state cannot be deleted";
        return false;
    };
    return participant;
}
} // namespace

int main()
{
    try {
        ayt::app::WorldStateLedger ledger;
        const Location drop{Place::World, "town/drop", ""};
        const Location bag{Place::Container, "player/bag", "slot"};
        {
            auto old = makeStore();
            check(old.addContainer({"player/bag", {"slot"}}).has_value(), "bag");
            check(old.addEquipmentOwner({"player", {"hand"}}).has_value(), "actor");
            check(old.createItem({"sword-1", "sword", 1, drop}).has_value(), "drop");
            check(old.transfer("sword-1", drop, bag, "pickup").has_value(), "pickup");
            check(ledger.registerParticipant("inventory/world", bind(old)), "bind old");
            check(ledger.capture(), "capture");
            check(ledger.unregisterParticipant("inventory/world"), "unbind old");
        }
        std::string savePayload;
        check(ledger.encode(savePayload), "ledger encode");
        ayt::app::WorldStateLedger loaded;
        check(loaded.decode(savePayload), "ledger decode");
        auto rebuilt = makeStore();
        check(loaded.registerParticipant("inventory/world", bind(rebuilt)), "bind new");
        check(loaded.restoreRegistered(), "staged restore");
        const auto* item = rebuilt.findItem("sword-1");
        check(item && item->location == bag && item->abilityBindingId == 1,
              "ownership and Ability ID survive rebuild");
        check(rebuilt.transfer("sword-1", drop, bag, "pickup").has_value(),
              "exact retry remains idempotent");
        std::cout << "AYInventory: world-state ledger roundtrip passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "AYInventory ledger: " << error.what() << '\n';
        return 1;
    }
}
