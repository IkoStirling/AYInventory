#include <AYInventory/Inventory.h>
#include <AYInventory/SnapshotJson.h>
#include <AYApplication/WorldStateLedger.h>
#include <AYApplication/SaveGameService.h>

#include <filesystem>
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
        namespace fs = std::filesystem;
        const fs::path root = fs::path(AY_INVENTORY_LEDGER_TEST_ROOT);
        std::error_code ec;
        fs::remove_all(root, ec);
        check(fs::create_directories(root, ec), "create slot directory");
        ayt::app::SaveGameService saves({root.string(), "inventory-ledger-test"});
        ayt::app::WorldStateLedger ledger;
        const Location drop{Place::World, "town/drop", ""};
        const Location bag{Place::Container, "player/bag", "slot"};
        auto old = makeStore();
        check(old.addContainer({"player/bag", {"slot"}}).has_value(), "bag");
        check(old.addEquipmentOwner({"player", {"hand"}}).has_value(), "actor");
        check(old.createItem({"sword-1", "sword", 1, drop}).has_value(), "drop");
        check(old.transfer("sword-1", drop, bag, "pickup").has_value(), "pickup");
        check(old.createItem({"retired-sword", "sword", 1,
                              {Place::World, "town/retired", ""}}).has_value(), "temporary item");
        check(old.destroyItem("retired-sword").has_value(), "retire temporary item");
        check(ledger.registerParticipant("inventory/world", bind(old)), "bind old");
        check(ledger.capture(), "capture");
        Definition incompatibleDefinition;
        incompatibleDefinition.id = "sword";
        incompatibleDefinition.equipSlots = {"hand"};
        incompatibleDefinition.abilityConfiguration = 11;
        auto incompatible = Store::create(3, {incompatibleDefinition});
        check(incompatible.has_value(), "incompatible catalog setup");
        check(!ledger.restoreWithParticipants(
            {{"inventory/world", bind(*incompatible)}}),
            "failed staged restore rejects changed catalog");
        check(ledger.capture(), "old binding remains usable after failure");
        std::string savePayload;
        check(ledger.encode(savePayload), "ledger encode");
        check(static_cast<bool>(saves.save("slot", 1, savePayload)), "slot save");
        const auto slot = saves.load("slot", 1);
        check(static_cast<bool>(slot), "slot load");
        ayt::app::WorldStateLedger loaded;
        check(loaded.decode(slot.payloadJson), "ledger decode");
        auto rebuilt = makeStore();
        check(loaded.restoreWithParticipants(
            {{"inventory/world", bind(rebuilt)}}), "staged restore");
        const auto* item = rebuilt.findItem("sword-1");
        check(item && item->location == bag && item->abilityBindingId == 1,
              "ownership and Ability ID survive rebuild");
        check(!rebuilt.createItem({"retired-sword", "sword", 1, drop}),
              "retired identity survives disk and staged restore");
        check(rebuilt.transfer("sword-1", drop, bag, "pickup").has_value(),
              "exact retry remains idempotent");
        loaded.clearParticipants();
        fs::remove_all(root, ec);
        std::cout << "AYInventory: world-state ledger roundtrip passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "AYInventory ledger: " << error.what() << '\n';
        return 1;
    }
}
