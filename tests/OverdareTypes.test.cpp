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
