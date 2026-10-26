cmake_minimum_required(VERSION 3.25)
project(AYInventory VERSION 0.1.0 LANGUAGES CXX)
include(CTest)

add_library(AYInventoryCore STATIC src/Inventory.cpp src/WorldTransactions.cpp)
add_library(AYInventory::Core ALIAS AYInventoryCore)
target_compile_features(AYInventoryCore PUBLIC cxx_std_23)
target_include_directories(AYInventoryCore PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
if(MSVC)
    target_compile_options(AYInventoryCore PRIVATE /W4 /utf-8)
else()
    target_compile_options(AYInventoryCore PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(TARGET AYEntityCore AND TARGET AYPhysics)
    add_library(AYInventoryWorld3DHost STATIC src/ScenePhysicsHost3D.cpp)
    add_library(AYInventory::World3DHost ALIAS AYInventoryWorld3DHost)
    target_compile_features(AYInventoryWorld3DHost PUBLIC cxx_std_23)
    target_include_directories(AYInventoryWorld3DHost PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
    target_link_libraries(AYInventoryWorld3DHost PUBLIC
        AYInventoryCore AYEntityCore AYPhysics)
    if(MSVC)
        target_compile_options(AYInventoryWorld3DHost PRIVATE /W4 /utf-8)
    endif()
endif()

if(TARGET AYStatsCore AND TARGET AYAbilityCore)
    add_library(AYInventoryEquipmentProjection STATIC src/EquipmentProjection.cpp)
    add_library(AYInventory::EquipmentProjection ALIAS AYInventoryEquipmentProjection)
    target_compile_features(AYInventoryEquipmentProjection PUBLIC cxx_std_23)
    target_include_directories(AYInventoryEquipmentProjection PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
    target_link_libraries(AYInventoryEquipmentProjection PUBLIC
        AYInventoryCore AYStatsCore AYAbilityCore)
    if(MSVC)
        target_compile_options(AYInventoryEquipmentProjection PRIVATE /W4 /utf-8)
    endif()
endif()

option(AY_INVENTORY_BUILD_JSON "Build versioned inventory snapshot JSON codec" ON)
if(AY_INVENTORY_BUILD_JSON)
    find_package(nlohmann_json CONFIG QUIET)
    if(nlohmann_json_FOUND)
        add_library(AYInventoryJson STATIC src/SnapshotJson.cpp)
        add_library(AYInventory::Json ALIAS AYInventoryJson)
        target_link_libraries(AYInventoryJson PUBLIC AYInventoryCore
            PRIVATE nlohmann_json::nlohmann_json)
        if(MSVC)
            target_compile_options(AYInventoryJson PRIVATE /W4 /utf-8)
        endif()
    elseif(NOT PROJECT_IS_TOP_LEVEL)
        message(FATAL_ERROR "AYInventory JSON codec requires nlohmann_json")
    else()
        message(STATUS "nlohmann_json not found; building AYInventoryCore only")
    endif()
endif()

if(TARGET AYApplicationSaveGame AND TARGET AYInventoryJson)
    add_library(AYInventoryWorldCommit STATIC src/WorldCommit.cpp)
    add_library(AYInventory::WorldCommit ALIAS AYInventoryWorldCommit)
    target_compile_features(AYInventoryWorldCommit PUBLIC cxx_std_23)
    target_include_directories(AYInventoryWorldCommit PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
    target_link_libraries(AYInventoryWorldCommit PUBLIC
        AYInventoryJson AYApplicationSaveGame
        PRIVATE nlohmann_json::nlohmann_json)
    if(MSVC)
        target_compile_options(AYInventoryWorldCommit PRIVATE /W4 /utf-8)
    endif()
endif()

if(TARGET AYInventoryWorldCommit AND TARGET AYInventoryEquipmentProjection
   AND TARGET AYStatsAssets AND TARGET AYAbilityAuthoring)
    add_library(AYInventoryWorldEquipmentCommit STATIC
        src/WorldEquipmentCommit.cpp)
    add_library(AYInventory::WorldEquipmentCommit ALIAS
        AYInventoryWorldEquipmentCommit)
    target_compile_features(AYInventoryWorldEquipmentCommit PUBLIC cxx_std_23)
    target_include_directories(AYInventoryWorldEquipmentCommit PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(AYInventoryWorldEquipmentCommit PUBLIC
        AYInventoryWorldCommit AYInventoryEquipmentProjection
        AYStatsAssets AYAbilityAuthoring)
    if(MSVC)
        target_compile_options(AYInventoryWorldEquipmentCommit PRIVATE /W4 /utf-8)
    endif()
    if(BUILD_TESTING)
        add_executable(AYInventoryWorldEquipmentCommitTest
            unittest/WorldEquipmentCommitTest.cpp)
        target_link_libraries(AYInventoryWorldEquipmentCommitTest PRIVATE
            AYInventoryWorldEquipmentCommit)
        target_compile_definitions(AYInventoryWorldEquipmentCommitTest PRIVATE
            "AY_INVENTORY_WORLD_EQUIPMENT_TEST_ROOT=\"${CMAKE_CURRENT_BINARY_DIR}/world-equipment-test-output\"")
        add_test(NAME AYInventoryWorldEquipmentCommitTest
            COMMAND AYInventoryWorldEquipmentCommitTest)
    endif()
endif()

if(BUILD_TESTING)
    if(TARGET AYInventoryEquipmentProjection)
        add_executable(AYInventoryEquipmentProjectionTest unittest/EquipmentProjectionTest.cpp)
        target_link_libraries(AYInventoryEquipmentProjectionTest PRIVATE AYInventoryEquipmentProjection)
        add_test(NAME AYInventoryEquipmentProjectionTest COMMAND AYInventoryEquipmentProjectionTest)
    endif()
    add_executable(AYInventoryTest unittest/InventoryTest.cpp)
    target_link_libraries(AYInventoryTest PRIVATE AYInventoryCore)
    if(TARGET AYInventoryJson)
        target_link_libraries(AYInventoryTest PRIVATE AYInventoryJson)
        target_compile_definitions(AYInventoryTest PRIVATE AY_INVENTORY_JSON=1)
    endif()
    add_test(NAME AYInventoryTest COMMAND AYInventoryTest)
    add_executable(AYInventoryWorldTransactionTest unittest/WorldTransactionTest.cpp)
    target_link_libraries(AYInventoryWorldTransactionTest PRIVATE AYInventoryCore)
    if(TARGET AYInventoryJson)
        target_link_libraries(AYInventoryWorldTransactionTest PRIVATE AYInventoryJson)
        target_compile_definitions(AYInventoryWorldTransactionTest PRIVATE AY_INVENTORY_JSON=1)
    endif()
    add_test(NAME AYInventoryWorldTransactionTest COMMAND AYInventoryWorldTransactionTest)
    if(TARGET AYInventoryWorld3DHost AND TARGET AYEntity)
        add_executable(AYInventoryWorld3DHostTest unittest/ScenePhysicsHost3DTest.cpp)
        target_link_libraries(AYInventoryWorld3DHostTest PRIVATE
            AYInventoryWorld3DHost AYEntity)
        add_test(NAME AYInventoryWorld3DHostTest COMMAND AYInventoryWorld3DHostTest)
    endif()
    if(TARGET AYApplicationSaveGame AND TARGET AYInventoryJson)
        add_executable(AYInventoryLedgerTest unittest/InventoryLedgerTest.cpp)
        target_link_libraries(AYInventoryLedgerTest PRIVATE
            AYInventoryJson AYApplicationSaveGame)
        target_compile_definitions(AYInventoryLedgerTest PRIVATE
            "AY_INVENTORY_LEDGER_TEST_ROOT=\"${CMAKE_CURRENT_BINARY_DIR}/inventory-ledger-test-output\"")
        add_test(NAME AYInventoryLedgerTest COMMAND AYInventoryLedgerTest)
        add_executable(AYInventoryWorldCommitTest unittest/WorldCommitTest.cpp)
        target_link_libraries(AYInventoryWorldCommitTest PRIVATE
            AYInventoryWorldCommit)
        if(TARGET AYProgressionJson)
            target_link_libraries(AYInventoryWorldCommitTest PRIVATE
                AYProgressionJson)
            target_compile_definitions(AYInventoryWorldCommitTest PRIVATE
                AY_WORLD_COMMIT_WITH_PROGRESSION=1)
        endif()
        target_compile_definitions(AYInventoryWorldCommitTest PRIVATE
            "AY_INVENTORY_WORLD_COMMIT_TEST_ROOT=\"${CMAKE_CURRENT_BINARY_DIR}/world-commit-test-output\"")
        add_test(NAME AYInventoryWorldCommitTest COMMAND AYInventoryWorldCommitTest)
    endif()
    if(TARGET AYScene AND TARGET AYApplicationSaveGame
       AND TARGET AYInventoryJson AND TARGET AYInventoryWorld3DHost)
        add_executable(AYInventorySceneRestoreTest
            unittest/InventorySceneRestoreTest.cpp)
        target_link_libraries(AYInventorySceneRestoreTest PRIVATE
            AYInventoryWorld3DHost AYInventoryWorldCommit
            AYInventoryJson AYApplicationSaveGame
            AYScene AYEntity)
        target_compile_definitions(AYInventorySceneRestoreTest PRIVATE
            "AY_INVENTORY_SCENE_TEST_ROOT=\"${CMAKE_CURRENT_BINARY_DIR}/inventory-scene-test-output\"")
        add_test(NAME AYInventorySceneRestoreTest COMMAND AYInventorySceneRestoreTest)
    endif()
endif()

add_executable(AYInventoryPickupExample demo/PickupEquip.cpp)
target_link_libraries(AYInventoryPickupExample PRIVATE AYInventoryCore)
add_executable(AYInventoryCraftPlaceExample demo/CraftPlace.cpp)
target_link_libraries(AYInventoryCraftPlaceExample PRIVATE AYInventoryCore)
