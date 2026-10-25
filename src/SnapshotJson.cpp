#include <AYInventory/SnapshotJson.h>

#include <nlohmann/json.hpp>

#include <limits>
#include <exception>
#include <utility>

namespace ayt::inventory {
namespace {
using Json = nlohmann::json;

template<class T> Result<T> invalid(std::string message)
{
    return std::unexpected(Diagnostic{Error::Invalid, std::move(message)});
}

bool u32(const Json& object, const char* key, std::uint32_t& out)
{
    if (!object.is_object() || !object.contains(key) ||
        !object.at(key).is_number_unsigned()) return false;
    const auto value = object.at(key).get<std::uint64_t>();
    if (value > std::numeric_limits<std::uint32_t>::max()) return false;
    out = static_cast<std::uint32_t>(value);
    return true;
}

bool u64(const Json& object, const char* key, std::uint64_t& out)
{
    if (!object.is_object() || !object.contains(key)
        || !object.at(key).is_number_unsigned()) return false;
    out = object.at(key).get<std::uint64_t>();
    return true;
}

bool text(const Json& object, const char* key, std::string& out)
{
    if (!object.is_object() || !object.contains(key) ||
        !object.at(key).is_string()) return false;
    out = object.at(key).get<std::string>();
    return true;
}

Result<std::set<std::string>> slots(const Json& data)
{
    if (!data.is_array()) return invalid<std::set<std::string>>("slots must be array");
    std::set<std::string> result;
    for (const auto& value : data) {
        if (!value.is_string())
            return invalid<std::set<std::string>>("slot must be string");
        auto slot = value.get<std::string>();
        if (slot.empty() || !result.insert(slot).second)
            return invalid<std::set<std::string>>("empty or duplicate slot");
    }
    return result;
}
} // namespace

Result<std::string> encodeSnapshotJson(const Snapshot& snapshot)
{
    try {
        if (snapshot.schemaVersion < 2 || snapshot.schemaVersion > 8)
            return invalid<std::string>("unsupported inventory schema");
        if (snapshot.schemaVersion < 4 && !snapshot.retiredItemIds.empty())
            return invalid<std::string>("legacy inventory schema cannot encode retired IDs");
        if (snapshot.schemaVersion < 5)
            for (const auto& item : snapshot.items)
                if (item.location.place == Place::Attachment)
                    return invalid<std::string>("legacy inventory schema cannot encode attachments");
        if (snapshot.schemaVersion < 6 && !snapshot.completedMerges.empty())
            return invalid<std::string>("legacy inventory schema cannot encode merge receipts");
        if (snapshot.schemaVersion < 7 && !snapshot.pendingCrafts.empty())
            return invalid<std::string>("legacy inventory schema cannot encode pending crafts");
        if (snapshot.schemaVersion < 8 && !snapshot.pendingItemHolds.empty())
            return invalid<std::string>("legacy inventory schema cannot encode pending item holds");
        Json root = {{"format", "AliyatInventory"},
                     {"schemaVersion", snapshot.schemaVersion},
                     {"catalogVersion", snapshot.catalogVersion},
                     {"nextAbilityBindingId", snapshot.nextAbilityBindingId},
                     {"containers", Json::array()}, {"actors", Json::array()},
                     {"items", Json::array()}, {"transfers", Json::array()},
                     {"crafts", Json::array()}};
        if (snapshot.schemaVersion >= 4)
            root["retiredItems"] = snapshot.retiredItemIds;
        if (snapshot.schemaVersion >= 6)
            root["merges"] = Json::array();
        if (snapshot.schemaVersion >= 7)
            root["pendingCrafts"] = Json::array();
        if (snapshot.schemaVersion >= 8)
            root["pendingItemHolds"] = Json::array();
        for (const auto& value : snapshot.containers)
            root["containers"].push_back({{"id", value.id}, {"slots", value.slots}});
        for (const auto& value : snapshot.actors)
            root["actors"].push_back({{"id", value.id}, {"slots", value.slots}});
        for (const auto& value : snapshot.items) {
            root["items"].push_back({{"id", value.id},
                                     {"definition", value.definitionId},
                                     {"quantity", value.quantity},
                                     {"abilityBindingId", value.abilityBindingId},
                                     {"place", static_cast<unsigned>(value.location.place)},
                                     {"owner", value.location.ownerId},
                                     {"slot", value.location.slot}});
        }
        for (const auto& [request, fingerprint] : snapshot.completedTransfers)
            root["transfers"].push_back({{"request", request},
                                         {"fingerprint", fingerprint}});
        for (const auto& [request, receipt] : snapshot.completedCrafts)
            root["crafts"].push_back({{"request", request},
                {"output", receipt.outputItemId},
                {"place", static_cast<unsigned>(receipt.destination.place)},
                {"owner", receipt.destination.ownerId},
                {"slot", receipt.destination.slot}});
        if (snapshot.schemaVersion >= 6)
            for (const auto& [request, receipt] : snapshot.completedMerges)
                root["merges"].push_back({{"request", request},
                    {"source", receipt.sourceItemId},
                    {"target", receipt.targetItemId},
                    {"quantity", receipt.quantity},
                    {"sourceAfter", receipt.sourceAfter},
                    {"targetAfter", receipt.targetAfter}});
        if (snapshot.schemaVersion >= 7)
            for (const auto& [request, craft] : snapshot.pendingCrafts) {
                Json recipeInputs = Json::array();
                for (const auto& input : craft.recipe.inputs)
                    recipeInputs.push_back({{"definition", input.definitionId},
                                            {"quantity", input.quantity}});
                Json materials = Json::array();
                for (const auto& input : craft.inputs)
                    materials.push_back({{"item", input.itemId},
                                         {"quantity", input.quantity}});
                root["pendingCrafts"].push_back({{"request", request},
                    {"source", craft.sourceContainerId},
                    {"recipe", {{"id", craft.recipe.id},
                        {"inputs", std::move(recipeInputs)},
                        {"outputDefinition", craft.recipe.outputDefinitionId},
                        {"outputQuantity", craft.recipe.outputQuantity}}},
                    {"materials", std::move(materials)}});
            }
        if (snapshot.schemaVersion >= 8)
            for (const auto& [request, hold] : snapshot.pendingItemHolds) {
                Json inputs = Json::array();
                for (const auto& input : hold.inputs)
                    inputs.push_back({{"item", input.itemId},
                                      {"quantity", input.quantity}});
                root["pendingItemHolds"].push_back({{"request", request},
                    {"source", hold.sourceContainerId},
                    {"definition", hold.definitionId},
                    {"quantity", hold.quantity},
                    {"inputs", std::move(inputs)}});
            }
        return root.dump();
    } catch (const std::exception& ex) {
        return invalid<std::string>(ex.what());
    }
}

Result<Snapshot> decodeSnapshotJson(std::string_view payloadJson)
{
    if (payloadJson.size() > 8u * 1024u * 1024u)
        return invalid<Snapshot>("inventory payload exceeds 8 MiB");
    try {
        const auto root = Json::parse(payloadJson.begin(), payloadJson.end());
        Snapshot snapshot;
        std::string format;
        if (!text(root, "format", format) || format != "AliyatInventory"
            || !u32(root, "schemaVersion", snapshot.schemaVersion)
            || snapshot.schemaVersion < 2 || snapshot.schemaVersion > 8
            || !u32(root, "catalogVersion", snapshot.catalogVersion)
            || !u64(root, "nextAbilityBindingId", snapshot.nextAbilityBindingId))
            return invalid<Snapshot>("invalid inventory header");
        for (const char* key : {"containers", "actors", "items", "transfers"})
            if (!root.contains(key) || !root.at(key).is_array())
                return invalid<Snapshot>(std::string("invalid ") + key + " array");
        if ((snapshot.schemaVersion >= 3
                && (!root.contains("crafts") || !root.at("crafts").is_array()))
            || (root.contains("crafts") && !root.at("crafts").is_array()))
            return invalid<Snapshot>("invalid crafts array");
        if ((snapshot.schemaVersion >= 4
             && (!root.contains("retiredItems")
                 || !root.at("retiredItems").is_array()))
            || (snapshot.schemaVersion < 4 && root.contains("retiredItems")))
            return invalid<Snapshot>("invalid retired items array");
        if ((snapshot.schemaVersion >= 6
             && (!root.contains("merges") || !root.at("merges").is_array()))
            || (snapshot.schemaVersion < 6 && root.contains("merges")))
            return invalid<Snapshot>("invalid merges array");
        if ((snapshot.schemaVersion >= 7
             && (!root.contains("pendingCrafts") || !root.at("pendingCrafts").is_array()))
            || (snapshot.schemaVersion < 7 && root.contains("pendingCrafts")))
            return invalid<Snapshot>("invalid pending crafts array");
        if ((snapshot.schemaVersion >= 8
             && (!root.contains("pendingItemHolds") || !root.at("pendingItemHolds").is_array()))
            || (snapshot.schemaVersion < 8 && root.contains("pendingItemHolds")))
            return invalid<Snapshot>("invalid pending item holds array");
        if (snapshot.schemaVersion >= 4) {
            for (const auto& value : root["retiredItems"]) {
                if (!value.is_string())
                    return invalid<Snapshot>("retired item ID must be string");
                auto id = value.get<std::string>();
                if (id.empty() || !snapshot.retiredItemIds.insert(std::move(id)).second)
                    return invalid<Snapshot>("empty or duplicate retired item ID");
            }
        }
        for (const auto& value : root["containers"]) {
            Container entry;
            if (!text(value, "id", entry.id) || !value.contains("slots"))
                return invalid<Snapshot>("invalid container");
            auto parsed = slots(value.at("slots"));
            if (!parsed) return invalid<Snapshot>(parsed.error().message);
            entry.slots = std::move(*parsed);
            snapshot.containers.push_back(std::move(entry));
        }
        for (const auto& value : root["actors"]) {
            EquipmentOwner entry;
            if (!text(value, "id", entry.id) || !value.contains("slots"))
                return invalid<Snapshot>("invalid equipment owner");
            auto parsed = slots(value.at("slots"));
            if (!parsed) return invalid<Snapshot>(parsed.error().message);
            entry.slots = std::move(*parsed);
            snapshot.actors.push_back(std::move(entry));
        }
        for (const auto& value : root["items"]) {
            Item entry;
            std::uint32_t place = 0;
            if (!text(value, "id", entry.id)
                || !text(value, "definition", entry.definitionId)
                || !u32(value, "quantity", entry.quantity)
                || !u64(value, "abilityBindingId", entry.abilityBindingId)
                || !u32(value, "place", place)
                || !text(value, "owner", entry.location.ownerId)
                || !text(value, "slot", entry.location.slot)
                || place > static_cast<std::uint32_t>(Place::Attachment))
                return invalid<Snapshot>("invalid item record");
            if (snapshot.schemaVersion < 5
                && place == static_cast<std::uint32_t>(Place::Attachment))
                return invalid<Snapshot>("legacy inventory item has attachment location");
            entry.location.place = static_cast<Place>(place);
            snapshot.items.push_back(std::move(entry));
        }
        for (const auto& value : root["transfers"]) {
            std::string request, fingerprint;
            if (!text(value, "request", request)
                || !text(value, "fingerprint", fingerprint)
                || request.empty() || fingerprint.empty()
                || !snapshot.completedTransfers.emplace(request, fingerprint).second)
                return invalid<Snapshot>("invalid transfer receipt");
        }
        if (root.contains("crafts")) for (const auto& value : root["crafts"]) {
            std::string request;
            CraftReceipt receipt;
            std::uint32_t place = 0;
            if (!text(value, "request", request)
                || !text(value, "output", receipt.outputItemId)
                || !u32(value, "place", place)
                || !text(value, "owner", receipt.destination.ownerId)
                || !text(value, "slot", receipt.destination.slot)
                || place > static_cast<std::uint32_t>(Place::Attachment)
                || request.empty() || receipt.outputItemId.empty()
                || receipt.destination.ownerId.empty())
                return invalid<Snapshot>("invalid craft receipt");
            receipt.destination.place = static_cast<Place>(place);
            if (!snapshot.completedCrafts.emplace(std::move(request),
                                                   std::move(receipt)).second)
                return invalid<Snapshot>("duplicate craft receipt");
        }
        if (snapshot.schemaVersion >= 6)
            for (const auto& value : root["merges"]) {
                std::string request;
                MergeReceipt receipt;
                if (!text(value, "request", request)
                    || !text(value, "source", receipt.sourceItemId)
                    || !text(value, "target", receipt.targetItemId)
                    || !u32(value, "quantity", receipt.quantity)
                    || !u32(value, "sourceAfter", receipt.sourceAfter)
                    || !u32(value, "targetAfter", receipt.targetAfter)
                    || request.empty() || receipt.sourceItemId.empty()
                    || receipt.targetItemId.empty()
                    || receipt.sourceItemId == receipt.targetItemId
                    || receipt.quantity == 0
                    || receipt.targetAfter < receipt.quantity
                    || !snapshot.completedMerges.emplace(request, receipt).second)
                    return invalid<Snapshot>("invalid merge receipt");
            }
        if (snapshot.schemaVersion >= 7)
            for (const auto& value : root["pendingCrafts"]) {
                std::string request;
                PendingCraft craft;
                if (!text(value, "request", request)
                    || !text(value, "source", craft.sourceContainerId)
                    || !value.contains("recipe") || !value.at("recipe").is_object()
                    || !value.contains("materials") || !value.at("materials").is_array())
                    return invalid<Snapshot>("invalid pending craft record");
                const auto& recipe = value.at("recipe");
                if (!text(recipe, "id", craft.recipe.id)
                    || !text(recipe, "outputDefinition", craft.recipe.outputDefinitionId)
                    || !u32(recipe, "outputQuantity", craft.recipe.outputQuantity)
                    || !recipe.contains("inputs") || !recipe.at("inputs").is_array())
                    return invalid<Snapshot>("invalid pending craft recipe");
                for (const auto& input : recipe.at("inputs")) {
                    Recipe::Ingredient ingredient;
                    if (!text(input, "definition", ingredient.definitionId)
                        || !u32(input, "quantity", ingredient.quantity))
                        return invalid<Snapshot>("invalid pending craft ingredient");
                    craft.recipe.inputs.push_back(std::move(ingredient));
                }
                for (const auto& input : value.at("materials")) {
                    ReservedCraftInput material;
                    if (!text(input, "item", material.itemId)
                        || !u32(input, "quantity", material.quantity))
                        return invalid<Snapshot>("invalid pending craft material");
                    craft.inputs.push_back(std::move(material));
                }
                if (!snapshot.pendingCrafts.emplace(std::move(request),
                                                    std::move(craft)).second)
                    return invalid<Snapshot>("duplicate pending craft request");
            }
        if (snapshot.schemaVersion >= 8)
            for (const auto& value : root["pendingItemHolds"]) {
                std::string request;
                PendingItemHold hold;
                if (!text(value, "request", request)
                    || !text(value, "source", hold.sourceContainerId)
                    || !text(value, "definition", hold.definitionId)
                    || !u32(value, "quantity", hold.quantity)
                    || !value.contains("inputs") || !value.at("inputs").is_array())
                    return invalid<Snapshot>("invalid pending item hold record");
                for (const auto& input : value.at("inputs")) {
                    ReservedCraftInput selected;
                    if (!text(input, "item", selected.itemId)
                        || !u32(input, "quantity", selected.quantity))
                        return invalid<Snapshot>("invalid pending item hold input");
                    hold.inputs.push_back(std::move(selected));
                }
                if (!snapshot.pendingItemHolds.emplace(std::move(request),
                                                       std::move(hold)).second)
                    return invalid<Snapshot>("duplicate pending item hold request");
            }
        return snapshot;
    } catch (const std::exception& ex) {
        return invalid<Snapshot>(ex.what());
    }
}

} // namespace ayt::inventory
