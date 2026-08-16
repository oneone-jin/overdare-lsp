#include "doctest.h"
#include "TempDir.h"
#include "Fixture.h"
#include "Analyze/CliClient.hpp"
#include "LSP/Workspace.hpp"
#include "LSP/WorkspaceFileResolver.hpp"
#include "Platform/OverdarePlatform.hpp"

// Smoke tests for the OVERDARE type definitions merged into scripts/globalTypes.d.luau
// (see scripts/dumpOverdareTypes.py) and the .ovdrjm -> sourcemap.json pipeline
// (see scripts/ovdrjmToSourcemap.py). These guard against regressions like a scraper bug
// producing a definitions file that fails to parse or crashes the type checker.
//
// These use a fresh CliClient/WorkspaceFolder per test (mirroring tests/AnalyzeCli.test.cpp)
// rather than the shared Fixture, since Fixture's constructor already eagerly registers
// "@overdare" from tests/testdata/standard_definitions.d.luau - loading the real production
// file under the same package name a second time would redeclare every type and crash.

TEST_SUITE_BEGIN("OverdareTypes");

namespace
{

CliClient makeClientWithProductionDefinitions()
{
    CliClient client;
    client.globalConfig = Luau::LanguageServer::defaultTestClientConfiguration();
    client.globalConfig.platform.type = LSPPlatformConfig::Overdare;
    client.globalConfig.sourcemap.enabled = true;
    client.globalConfig.sourcemap.sourcemapFile = "sourcemap.json";
    client.definitionsFiles.emplace("@overdare", "scripts/globalTypes.d.luau");
    return client;
}

} // namespace

TEST_CASE("production_global_types_definitions_file_loads_without_errors")
{
    TempDir t("overdare_types_definitions_load");

    CliClient client = makeClientWithProductionDefinitions();
    WorkspaceFolder workspace(&client, "CLI", Uri::file(t.path()), std::nullopt);
    workspace.setupWithConfiguration(client.globalConfig);
    workspace.isReady = true;

    auto filePath = t.write_child("test.luau", R"(
        local x: Instance
        local p: Player
        local h: Humanoid
    )");
    auto cr = workspace.checkSimple(filePath, nullptr);
    CHECK(cr.errors.empty());
}

TEST_CASE("overdare_only_service_names_are_recognised_by_get_service")
{
    TempDir t("overdare_types_get_service");

    CliClient client = makeClientWithProductionDefinitions();
    WorkspaceFolder workspace(&client, "CLI", Uri::file(t.path()), std::nullopt);
    workspace.setupWithConfiguration(client.globalConfig);
    workspace.isReady = true;

    // ActionSequenceService/WorldRankService are OVERDARE-only services that don't exist in
    // OVERDARE-only - GetService's magic function validates the argument against a whitelist read
    // from the definitions file's `--#METADATA#` SERVICES list, not the class declarations,
    // so this needs its own coverage even though the class itself type-checks fine.
    auto filePath = t.write_child("test.luau", R"(
        local ActionSequenceService = game:GetService("ActionSequenceService")
        local WorldRankService = game:GetService("WorldRankService")
        local Players = game:GetService("Players")
    )");
    auto cr = workspace.checkSimple(filePath, nullptr);
    CHECK(cr.errors.empty());
}

TEST_CASE("overdare_only_class_names_are_recognised_by_instance_new")
{
    TempDir t("overdare_types_instance_new");

    CliClient client = makeClientWithProductionDefinitions();
    WorkspaceFolder workspace(&client, "CLI", Uri::file(t.path()), std::nullopt);
    workspace.setupWithConfiguration(client.globalConfig);
    workspace.isReady = true;

    // Fill/Outline are OVERDARE-only classes that never existed in Roblox at all, so
    // dumpOverdareTypes.py's intersect-with-Roblox's-whitelist pruning can't surface them on
    // its own (see EXTRA_CREATABLE_INSTANCES) - like GetService above, Instance.new's magic
    // function validates against the definitions file's `--#METADATA#` CREATABLE_INSTANCES
    // list, not the class declarations, so this needs its own coverage even though both
    // classes type-check fine.
    auto filePath = t.write_child("test.luau", R"(
        local fill = Instance.new("Fill")
        local outline = Instance.new("Outline")
        local part = Instance.new("Part")
    )");
    auto cr = workspace.checkSimple(filePath, nullptr);
    CHECK(cr.errors.empty());
}

TEST_CASE("vector3_vector2_cframe_arithmetic_operators_type_check")
{
    TempDir t("overdare_types_vector_operators");

    CliClient client = makeClientWithProductionDefinitions();
    WorkspaceFolder workspace(&client, "CLI", Uri::file(t.path()), std::nullopt);
    workspace.setupWithConfiguration(client.globalConfig);
    workspace.isReady = true;

    // docs.overdare.com's per-type pages never document metamethod operator overloads, so
    // merging a scraped page wholesale-replaces the block and silently drops any operators
    // inherited from the Roblox base (dumpOverdareTypes.py's declare_datatype simply never
    // emits __add/__sub/__mul/__div/__idiv/__unm). This regressed Vector3/Vector2/CFrame
    // arithmetic - see preserve_operator_overloads in dumpOverdareTypes.py for the fix.
    auto filePath = t.write_child("test.luau", R"(
        local v3 = Vector3.new(1, 2, 3)
        local negatedV3 = -v3
        local halvedV3 = v3 / 2
        local flooredV3 = v3 // 2
        local sumV3 = v3 + Vector3.new(1, 1, 1)
        local diffV3 = v3 - Vector3.new(1, 1, 1)
        local scaledV3 = v3 * 2

        local v2 = Vector2.new(1, 2)
        local negatedV2 = -v2
        local halvedV2 = v2 / 2
        local sumV2 = v2 + Vector2.new(1, 1)

        local cf = CFrame.new(0, 0, 0)
        local composedCf = cf * CFrame.new(0, 0, 0)
        local transformedV3 = cf * v3
        local movedCf = cf + v3
        local movedBackCf = cf - v3
    )");
    auto cr = workspace.checkSimple(filePath, nullptr);
    CHECK(cr.errors.empty());
}

TEST_CASE("datatype_constructors_with_multiple_overloads_type_check")
{
    TempDir t("overdare_types_ctor_overloads");

    CliClient client = makeClientWithProductionDefinitions();
    WorkspaceFolder workspace(&client, "CLI", Uri::file(t.path()), std::nullopt);
    workspace.setupWithConfiguration(client.globalConfig);
    workspace.isReady = true;

    // declare_datatype_constructor used to emit one `name: (...) -> T` table field per
    // documented overload, but a Luau table type can't repeat a key - duplicate `new:`
    // entries silently collapsed to just the last one, shadowing every other overload (e.g.
    // CFrame.new() and CFrame.new(Vector3) both broke, leaving only CFrame.new(x,y,z)
    // working). Affected every datatype with same-named overloads: CFrame,
    // PhysicalProperties, NumberSequence, ColorSequence, NumberSequenceKeypoint. Fixed by
    // joining same-named overloads with `&` instead.
    auto filePath = t.write_child("test.luau", R"(
        local cf1 = CFrame.new()
        local cf2 = CFrame.new(Vector3.new(1, 2, 3))
        local cf3 = CFrame.new(Vector3.new(1, 2, 3), Vector3.new(4, 5, 6))
        local cf4 = CFrame.new(1, 2, 3)

        local pp1 = PhysicalProperties.new(Enum.Material.Plastic)
        local pp2 = PhysicalProperties.new(1, 2, 3)
        local pp3 = PhysicalProperties.new(1, 2, 3, 4, 5)

        local ns1 = NumberSequence.new(1)
        local ns2 = NumberSequence.new({})
        local ns3 = NumberSequence.new(1, 2)

        local cs1 = ColorSequence.new(Color3.new(1, 0, 0))
        local cs2 = ColorSequence.new({})
        local cs3 = ColorSequence.new(Color3.new(1, 0, 0), Color3.new(0, 1, 0))

        local nsk1 = NumberSequenceKeypoint.new(0, 1)
        local nsk2 = NumberSequenceKeypoint.new(0, 1, 2)
    )");
    auto cr = workspace.checkSimple(filePath, nullptr);
    CHECK(cr.errors.empty());
}

TEST_CASE("isnil_global_is_available_and_warn_is_not")
{
    TempDir t("overdare_types_isnil");

    CliClient client = makeClientWithProductionDefinitions();
    WorkspaceFolder workspace(&client, "CLI", Uri::file(t.path()), std::nullopt);
    workspace.setupWithConfiguration(client.globalConfig);
    workspace.isReady = true;

    auto isnilFile = t.write_child("uses_isnil.luau", R"(
        local x: number? = nil
        local wasNil: boolean = isnil(x)
    )");
    auto isnilResult = workspace.checkSimple(isnilFile, nullptr);
    CHECK(isnilResult.errors.empty());

    auto warnFile = t.write_child("uses_warn.luau", R"(
        warn("this should not resolve on OVERDARE")
    )");
    auto warnResult = workspace.checkSimple(warnFile, nullptr);
    CHECK_FALSE(warnResult.errors.empty());
}

TEST_CASE("ovdrjm_shaped_sourcemap_resolves_requires_with_dedup_suffixed_siblings")
{
    // Mirrors the shape scripts/ovdrjmToSourcemap.py produces from a real .ovdrjm: an
    // OVERDARE-only service (ActionSequenceService) alongside standard ones, and two
    // same-named Script instances disambiguated by a flat `_1` suffix in filePaths (since
    // OVERDARE's Lua/ export folder is flat, unlike Rojo's nested paths).
    TempDir t("overdare_types_sourcemap");

    t.write_child("sourcemap.json", R"(
        {
            "name": "Game",
            "className": "DataModel",
            "children": [
                {
                    "name": "ServerStorage",
                    "className": "ServerStorage",
                    "children": [
                        { "name": "Script", "className": "Script", "filePaths": ["overdare/Lua/Script.lua"] },
                        { "name": "Script", "className": "Script", "filePaths": ["overdare/Lua/Script_1.lua"] }
                    ]
                },
                {
                    "name": "ActionSequenceService",
                    "className": "ActionSequenceService",
                    "children": []
                }
            ]
        }
    )");
    auto filePath = t.write_child("overdare/Lua/Script_1.lua", R"(
        local ActionSequenceService = game:GetService("ActionSequenceService")
        local ServerStorage = game:GetService("ServerStorage")
    )");

    CliClient client = makeClientWithProductionDefinitions();
    WorkspaceFolder workspace(&client, "CLI", Uri::file(t.path()), std::nullopt);
    workspace.setupWithConfiguration(client.globalConfig);
    workspace.isReady = true;

    auto* overdarePlatform = dynamic_cast<OverdarePlatform*>(workspace.platform.get());
    REQUIRE(overdarePlatform);
    REQUIRE(overdarePlatform->rootSourceNode != nullptr);

    auto cr = workspace.checkSimple(filePath, nullptr);
    CHECK(cr.errors.empty());
}

TEST_SUITE_END();
