#pragma once

#include <AYInventory/Inventory.h>

namespace ayt::inventory {

/// Versioned JSON payload for WorldStateLedger or SaveGameService.
/// Input is data only; call Store::restore for catalog/ownership validation.
Result<std::string> encodeSnapshotJson(const Snapshot& snapshot);
Result<Snapshot> decodeSnapshotJson(std::string_view payloadJson);

} // namespace ayt::inventory
