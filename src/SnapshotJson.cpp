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
        Json root = {{"format", "AliyatInventory"},
                     {"schemaVersion", snapshot.schemaVersion},
                     {"catalogVersion", snapshot.catalogVersion},
                     {"nextAbilityBindingId", snapshot.nextAbilityBindingId},
                     {"containers", Json::array()}, {"actors", Json::array()},
                     {"items", Json::array()}, {"transfers", Json::array()}};
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
            || !u32(root, "catalogVersion", snapshot.catalogVersion)
            || !u64(root, "nextAbilityBindingId", snapshot.nextAbilityBindingId))
            return invalid<Snapshot>("invalid inventory header");
        for (const char* key : {"containers", "actors", "items", "transfers"})
            if (!root.contains(key) || !root.at(key).is_array())
                return invalid<Snapshot>(std::string("invalid ") + key + " array");
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
                || place > static_cast<std::uint32_t>(Place::Equipment))
                return invalid<Snapshot>("invalid item record");
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
        return snapshot;
    } catch (const std::exception& ex) {
        return invalid<Snapshot>(ex.what());
    }
}

} // namespace ayt::inventory
