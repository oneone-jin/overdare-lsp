#include "doctest.h"
#include "Fixture.h"
#include "Platform/OverdarePlatform.hpp"
#include "Protocol/Workspace.hpp"
#include "LuauFileUtils.hpp"

TEST_SUITE_BEGIN("SourcemapTests");

TEST_CASE("getScriptFilePath")
{
    SourceNode node("test", "ModuleScript", {"test.lua"}, {});
    CHECK_EQ(node.getScriptFilePath(), "test.lua");
}

TEST_CASE("getScriptFilePath returns json file if node is populated by JSON")
{
    SourceNode node("test", "ModuleScript", {"test.json"}, {});
    CHECK_EQ(node.getScriptFilePath(), "test.json");
}

TEST_CASE("getScriptFilePath returns toml file if node is populated by TOML")
{
    SourceNode node("test", "ModuleScript", {"test.toml"}, {});
    CHECK_EQ(node.getScriptFilePath(), "test.toml");
}

TEST_CASE("getScriptFilePath doesn't pick .meta.json")
{
    SourceNode node("init", "ModuleScript", {"init.meta.json", "init.lua"}, {});
    CHECK_EQ(node.getScriptFilePath(), "init.lua");
}

TEST_CASE_FIXTURE(Fixture, "can_access_children_via_dot_properties")
{
    client->globalConfig.diagnostics.strictDatamodelTypes = true;
    loadSourcemap(R"(
            {
                "name": "Game",
                "className": "DataModel",
                "children": [
                    {
                        "name": "TemplateR15",
                        "className": "Part",
                        "children": [
                            {"name": "Head", "className": "Part"}
                        ]
                    }
                ]
            }
        )");

    auto result = check(R"(
        --!strict
        local template = game.TemplateR15
        local head = template.Head
    )");

    LUAU_LSP_REQUIRE_NO_ERRORS(result);
    CHECK(Luau::toString(requireType("template")) == "Part");
    CHECK(Luau::toString(requireType("head")) == "Part");
}

TEST_CASE_FIXTURE(Fixture, "can_access_children_via_find_first_child")
{
    client->globalConfig.diagnostics.strictDatamodelTypes = true;
    loadSourcemap(R"(
            {
                "name": "Game",
                "className": "DataModel",
                "children": [
                    {
                        "name": "TemplateR15",
                        "className": "Part",
                        "children": [
                            {"name": "Head", "className": "Part"}
                        ]
                    }
                ]
            }
        )");

    auto result = check(R"(
        --!strict
        local template = game:FindFirstChild("TemplateR15")
        local head = template.Head
    )");

    LUAU_LSP_REQUIRE_NO_ERRORS(result);
    CHECK(Luau::toString(requireType("template")) == "Part");
    CHECK(Luau::toString(requireType("head")) == "Part");
}

TEST_CASE_FIXTURE(Fixture, "find_first_child_handles_unknown_child")
{
    client->globalConfig.diagnostics.strictDatamodelTypes = true;
    loadSourcemap(R"(
            {
                "name": "Game",
                "className": "DataModel",
                "children": [
                    {
                        "name": "TemplateR15",
                        "className": "Part",
                        "children": [
                            {"name": "Head", "className": "Part"}
                        ]
                    }
                ]
            }
        )");

    auto result = check(R"(
        --!strict
        local template = game:FindFirstChild("Unknown")
    )");

    LUAU_LSP_REQUIRE_NO_ERRORS(result);
    CHECK(Luau::toString(requireType("template")) == "Instance?");
}

TEST_CASE_FIXTURE(Fixture, "find_first_child_works_without_sourcemap")
{
    client->globalConfig.diagnostics.strictDatamodelTypes = true;

    auto result = check(R"(
        --!strict
        local template = game:FindFirstChild("Unknown")
    )");

    LUAU_LSP_REQUIRE_NO_ERRORS(result);
    CHECK(Luau::toString(requireType("template")) == "Instance?");
}


TEST_CASE_FIXTURE(Fixture, "find_first_child_supports_recursive_parameter_with_sourcemap")
{
    client->globalConfig.diagnostics.strictDatamodelTypes = true;
    loadSourcemap(R"(
            {
                "name": "Game",
                "className": "DataModel",
                "children": [
                    {
                        "name": "TemplateR15",
                        "className": "Part",
                        "children": [
                            {"name": "Head", "className": "Part"}
                        ]
                    }
                ]
            }
        )");

    auto result = check(R"(
        --!strict
        local template = game:FindFirstChild("Head", true)
    )");

    LUAU_LSP_REQUIRE_NO_ERRORS(result);
    CHECK(Luau::toString(requireType("template")) == "Part");
}

TEST_CASE_FIXTURE(Fixture, "find_first_child_performs_bfs_and_picks_closest_matching_child_first")
{
    client->globalConfig.diagnostics.strictDatamodelTypes = true;
    loadSourcemap(R"(
            {
                "name": "Game",
                "className": "DataModel",
                "children": [
                    {
                        "name": "Long",
                        "className": "Folder",
                        "children": [
                            {
                                "name": "Short",
                                "className": "Folder",
                                "children": [
                                    {"name": "Head", "className": "Folder"}
                                ]
                            }
                        ]
                    },
                    {
                        "name": "TemplateR15",
                        "className": "Part",
                        "children": [
                            {"name": "Head", "className": "Part"}
                        ]
                    }
                ]
            }
        )");

    auto result = check(R"(
        --!strict
        local template = game:FindFirstChild("Head", true)
    )");

    LUAU_LSP_REQUIRE_NO_ERRORS(result);
    CHECK(Luau::toString(requireType("template")) == "Part");
}

TEST_CASE_FIXTURE(Fixture, "find_first_child_still_supports_recursive_parameter_without_sourcemap")
{
    client->globalConfig.diagnostics.strictDatamodelTypes = true;
    auto result = check(R"(
        --!strict
        local template = game:FindFirstChild("Unknown", true)
    )");

    LUAU_LSP_REQUIRE_NO_ERRORS(result);
    CHECK(Luau::toString(requireType("template")) == "Instance?");
}


TEST_CASE_FIXTURE(Fixture, "find_first_child_finds_direct_child_when_searching_recursively")
{
    client->globalConfig.diagnostics.strictDatamodelTypes = true;
    loadSourcemap(R"(
            {
                "name": "Game",
                "className": "DataModel",
                "children": [
                    {
                        "name": "TemplateR15",
                        "className": "Part",
                        "children": [
                            {"name": "Head", "className": "Part"}
                        ]
                    }
                ]
            }
        )");

    auto result = check(R"(
        --!strict
        local template = game:FindFirstChild("TemplateR15", true)
    )");

    LUAU_LSP_REQUIRE_NO_ERRORS(result);
    CHECK(Luau::toString(requireType("template")) == "Part");
}

TEST_CASE_FIXTURE(Fixture, "can_access_children_via_wait_for_child")
{
    client->globalConfig.diagnostics.strictDatamodelTypes = true;
    loadSourcemap(R"(
            {
                "name": "Game",
                "className": "DataModel",
                "children": [
                    {
                        "name": "TemplateR15",
                        "className": "Part",
                        "children": [
                            {"name": "Head", "className": "Part"}
                        ]
                    }
                ]
            }
        )");

    auto result = check(R"(
        --!strict
        local template = game:WaitForChild("TemplateR15")
        local head = template.Head
    )");

    LUAU_LSP_REQUIRE_NO_ERRORS(result);
    CHECK(Luau::toString(requireType("template")) == "Part");
    CHECK(Luau::toString(requireType("head")) == "Part");
}

TEST_CASE_FIXTURE(Fixture, "wait_for_child_handles_unknown_child")
{
    client->globalConfig.diagnostics.strictDatamodelTypes = true;
    loadSourcemap(R"(
            {
                "name": "Game",
                "className": "DataModel",
                "children": [
                    {
                        "name": "TemplateR15",
                        "className": "Part",
                        "children": [
                            {"name": "Head", "className": "Part"}
                        ]
                    }
                ]
            }
        )");

    auto result = check(R"(
        --!strict
        local template = game:WaitForChild("UnknownChild")
    )");

    LUAU_LSP_REQUIRE_NO_ERRORS(result);
    CHECK(Luau::toString(requireType("template")) == "Instance");
}

TEST_CASE_FIXTURE(Fixture, "wait_for_child_still_supports_timeout_parameter_with_sourcemap")
{
    client->globalConfig.diagnostics.strictDatamodelTypes = true;
    loadSourcemap(R"(
            {
                "name": "Game",
                "className": "DataModel",
                "children": [
                    {
                        "name": "TemplateR15",
                        "className": "Part",
                        "children": [
                            {"name": "Head", "className": "Part"}
                        ]
                    }
                ]
            }
        )");

    auto result = check(R"(
        --!strict
        local template = game:WaitForChild("UnknownChild", 10)
    )");

    LUAU_LSP_REQUIRE_NO_ERRORS(result);
    CHECK(Luau::toString(requireType("template")) == "Instance?");
}

TEST_CASE_FIXTURE(Fixture, "wait_for_child_still_supports_timeout_parameter_without_sourcemap")
{
    client->globalConfig.diagnostics.strictDatamodelTypes = true;

    auto result = check(R"(
        --!strict
        local template = game:WaitForChild("TemplateR15", 10)
    )");

    LUAU_LSP_REQUIRE_NO_ERRORS(result);
    CHECK(Luau::toString(requireType("template")) == "Instance?");
}

TEST_CASE_FIXTURE(Fixture, "wait_for_child_finds_direct_child_with_timeout_parameter")
{
    client->globalConfig.diagnostics.strictDatamodelTypes = true;
    loadSourcemap(R"(
            {
                "name": "Game",
                "className": "DataModel",
                "children": [
                    {
                        "name": "TemplateR15",
                        "className": "Part",
                        "children": [
                            {"name": "Head", "className": "Part"}
                        ]
                    }
                ]
            }
        )");

    auto result = check(R"(
        --!strict
        local template = game:WaitForChild("TemplateR15", 10)
    )");

    LUAU_LSP_REQUIRE_NO_ERRORS(result);
    CHECK(Luau::toString(requireType("template")) == "Part");
}

TEST_CASE_FIXTURE(Fixture, "can_access_ancestor_via_find_first_ancestor")
{
    client->globalConfig.diagnostics.strictDatamodelTypes = true;
    loadSourcemap(R"(
            {
                "name": "Game",
                "className": "DataModel",
                "children": [
                    {
                        "name": "TemplateR15",
                        "className": "Part",
                        "children": [
                            {
                                "name": "Head",
                                "className": "Part",
                                "children": [{ "name": "Attachment", "className": "Part" }]
                            }
                        ]
                    }
                ]
            }
        )");

    auto result = check(R"(
        --!strict
        local head = game.TemplateR15.Head.Attachment
        local template = head:FindFirstAncestor("TemplateR15")
    )");

    LUAU_LSP_REQUIRE_NO_ERRORS(result);
    CHECK(Luau::toString(requireType("head")) == "Part");
    CHECK(Luau::toString(requireType("template")) == "Part");
}

TEST_CASE_FIXTURE(Fixture, "find_first_ancestor_handles_unknown_ancestor")
{
    client->globalConfig.diagnostics.strictDatamodelTypes = true;
    loadSourcemap(R"(
            {
                "name": "Game",
                "className": "DataModel",
                "children": [
                    {
                        "name": "TemplateR15",
                        "className": "Part",
                        "children": [
                            {"name": "Head", "className": "Part"}
                        ]
                    }
                ]
            }
        )");

    auto result = check(R"(
        --!strict
        local head = game.TemplateR15.Head
        local random = head:FindFirstAncestor("Unknown")
    )");

    LUAU_LSP_REQUIRE_NO_ERRORS(result);
    CHECK(Luau::toString(requireType("head")) == "Part");
    CHECK(Luau::toString(requireType("random")) == "Instance?");
}

TEST_CASE_FIXTURE(Fixture, "find_first_ancestor_works_without_sourcemap")
{
    client->globalConfig.diagnostics.strictDatamodelTypes = true;

    auto result = check(R"(
        --!strict
        local template = game:FindFirstAncestor("Unknown")
    )");

    LUAU_LSP_REQUIRE_NO_ERRORS(result);
    CHECK(Luau::toString(requireType("template")) == "Instance?");
}

TEST_CASE_FIXTURE(Fixture, "relative_and_absolute_types_are_consistent")
{
    client->globalConfig.diagnostics.strictDatamodelTypes = true;
    loadSourcemap(R"(
            {
                "name": "Game",
                "className": "DataModel",
                "children": [
                    {
                        "name": "ReplicatedStorage",
                        "className": "ReplicatedStorage",
                        "children": [
                            {
                                "name": "Shared",
                                "className": "Part",
                                "children": [{"name": "Part", "className": "Part"}, {"name": "Script", "className": "Instance"}]
                            }
                        ]
                    }
                ]
            }
        )");

    auto result = check(R"(
        --!strict
        local script: typeof(game.ReplicatedStorage.Shared.Script) -- mimic script
        local absolutePart = game:GetService("ReplicatedStorage"):FindFirstChild("Shared"):FindFirstChild("Part")
        local relativePart = script.Parent.Part
    )");

    LUAU_LSP_REQUIRE_NO_ERRORS(result);

    auto absoluteTy = requireType("absolutePart");
    auto relativeTy = requireType("relativePart");
    CHECK(Luau::toString(absoluteTy) == "Part");
    CHECK(Luau::toString(relativeTy) == "Part");
    CHECK((absoluteTy == relativeTy));
}

TEST_CASE_FIXTURE(Fixture, "get_virtual_module_name_from_real_path")
{
#ifdef _WIN32
    workspace.rootUri = Uri::parse("file:///c%3A/Users/Development/project");
    workspace.fileResolver.rootUri = Uri::parse("file:///c%3A/Users/Development/project");
    loadSourcemap(R"(
        {
            "name": "Game",
            "className": "DataModel",
            "children": [{"name": "MainScript", "className": "ModuleScript", "filePaths": ["Foo\\Test.luau"]}]
        }
    )");
#else
    workspace.rootUri = Uri::parse("file:///home/project");
    workspace.fileResolver.rootUri = Uri::parse("file:///home/project");
    loadSourcemap(R"(
        {
            "name": "Game",
            "className": "DataModel",
            "children": [{"name": "MainScript", "className": "ModuleScript", "filePaths": ["Foo/Test.luau"]}]
        }
    )");
#endif

    auto uri = workspace.rootUri.resolvePath("Foo/Test.luau");

    CHECK_EQ(workspace.fileResolver.getModuleName(uri), "game/MainScript");
}

TEST_CASE_FIXTURE(Fixture, "same_named_siblings_get_distinct_virtual_module_names")
{
#ifdef _WIN32
    workspace.rootUri = Uri::parse("file:///c%3A/Users/Development/project");
    workspace.fileResolver.rootUri = Uri::parse("file:///c%3A/Users/Development/project");
    loadSourcemap(R"(
        {
            "name": "Game",
            "className": "DataModel",
            "children": [
                {"name": "Parent", "className": "Folder", "children": [
                    {"name": "LocalScript", "className": "LocalScript", "filePaths": ["Lua\\LocalScript.luau"]},
                    {"name": "LocalScript", "className": "LocalScript", "filePaths": ["Lua\\LocalScript_1.luau"]}
                ]}
            ]
        }
    )");
#else
    workspace.rootUri = Uri::parse("file:///home/project");
    workspace.fileResolver.rootUri = Uri::parse("file:///home/project");
    loadSourcemap(R"(
        {
            "name": "Game",
            "className": "DataModel",
            "children": [
                {"name": "Parent", "className": "Folder", "children": [
                    {"name": "LocalScript", "className": "LocalScript", "filePaths": ["Lua/LocalScript.luau"]},
                    {"name": "LocalScript", "className": "LocalScript", "filePaths": ["Lua/LocalScript_1.luau"]}
                ]}
            ]
        }
    )");
#endif

    auto firstUri = workspace.rootUri.resolvePath("Lua/LocalScript.luau");
    auto secondUri = workspace.rootUri.resolvePath("Lua/LocalScript_1.luau");

    auto firstModuleName = workspace.fileResolver.getModuleName(firstUri);
    auto secondModuleName = workspace.fileResolver.getModuleName(secondUri);

    // Regression test: same-named siblings previously collapsed to the identical virtual
    // module name (just "base/childName", with no disambiguation for repeated names), which
    // Frontend uses as its module identity/cache key - so both real files were treated as one
    // module, and diagnostics/type-checking for one silently applied to both instead.
    CHECK_NE(firstModuleName, secondModuleName);
    CHECK_EQ(workspace.platform->resolveToRealPath(firstModuleName), firstUri);
    CHECK_EQ(workspace.platform->resolveToRealPath(secondModuleName), secondUri);
}

TEST_CASE_FIXTURE(Fixture, "get_real_path_from_virtual_name")
{
#ifdef _WIN32
    workspace.rootUri = Uri::parse("file:///c%3A/Users/Development/project");
    workspace.fileResolver.rootUri = Uri::parse("file:///c%3A/Users/Development/project");
    loadSourcemap(R"(
        {
            "name": "Game",
            "className": "DataModel",
            "children": [{"name": "MainScript", "className": "ModuleScript", "filePaths": ["Foo\\Test.luau"]}]
        }
    )");
#else
    workspace.rootUri = Uri::parse("file:///random/project");
    workspace.fileResolver.rootUri = Uri::parse("file:///random/project");
    loadSourcemap(R"(
        {
            "name": "Game",
            "className": "DataModel",
            "children": [{"name": "MainScript", "className": "ModuleScript", "filePaths": ["Foo/Test.luau"]}]
        }
    )");
#endif

    CHECK_EQ(workspace.platform->resolveToRealPath("game/MainScript"), workspace.rootUri.resolvePath("Foo/Test.luau"));
}

TEST_CASE_FIXTURE(Fixture, "sourcemap_path_is_normalised_to_match_root_uri_subchild_with_lower_case_drive_letter")
{
#ifdef _WIN32
    workspace.rootUri = Uri::parse("file:///c%3A/Users/Development/project");
    workspace.fileResolver.rootUri = Uri::parse("file:///c%3A/Users/Development/project");
    loadSourcemap(R"(
        {
            "name": "RootNode",
            "className": "ModuleScript",
            "filePaths": ["Packages\\_Index\\example_package\\Test.luau"]
        }
    )");
#else
    workspace.rootUri = Uri::parse("file:///random/project");
    workspace.fileResolver.rootUri = Uri::parse("file:///random/project");
    loadSourcemap(R"(
        {
            "name": "RootNode",
            "className": "ModuleScript",
            "filePaths": ["Packages/_Index/example_package/Test.luau"]
        }
    )");
#endif

    auto rootNode = getRootSourceNode();
    auto filePath = rootNode->getScriptFilePath();
    REQUIRE(filePath);

    auto normalisedPath = dynamic_cast<OverdarePlatform*>(workspace.platform.get())->getRealPathFromSourceNode(rootNode);
    REQUIRE(normalisedPath);

    CHECK_EQ(workspace.rootUri.resolvePath(*filePath), normalisedPath);
}

TEST_CASE_FIXTURE(Fixture, "sourcemap_path_matches_ignore_globs")
{
#ifdef _WIN32
    workspace.rootUri = Uri::parse("file:///c%3A/Users/Development/project");
    workspace.fileResolver.rootUri = Uri::parse("file:///c%3A/Users/Development/project");
    loadSourcemap(R"(
        {
            "name": "RootNode",
            "className": "ModuleScript",
            "filePaths": ["Packages\\_Index\\example_package\\Test.luau"]
        }
    )");
#else
    workspace.rootUri = Uri::parse("file:///home/project");
    workspace.fileResolver.rootUri = Uri::parse("file:///home/project");
    loadSourcemap(R"(
        {
            "name": "RootNode",
            "className": "ModuleScript",
            "filePaths": ["Packages/_Index/example_package/Test.luau"]
        }
    )");
#endif
    client->globalConfig.completion.imports.ignoreGlobs = {"**/_Index/**"};


    auto rootNode = getRootSourceNode();
    auto filePath = dynamic_cast<OverdarePlatform*>(workspace.platform.get())->getRealPathFromSourceNode(rootNode);
    REQUIRE(filePath);

    CHECK(workspace.isIgnoredFileForAutoImports(*filePath));
}

TEST_CASE_FIXTURE(Fixture, "sourcemap_updates_marks_files_as_dirty")
{
    loadSourcemap(R"(
        {
            "name": "game",
            "className": "DataModel",
            "children": [
                {
                    "name": "Workspace",
                    "className": "Workspace",
                    "children": [{ "name": "Part", "className": "Part" }]
                }
            ]
        }
    )");

    auto document = newDocument("foo.luau", R"(
        local part = game.Workspace.Part
    )");

    lsp::HoverParams params;
    params.textDocument = {document};
    params.position = lsp::Position{1, 16};
    auto hover = workspace.hover(params, nullptr);

    REQUIRE(hover);
    CHECK_EQ(hover->contents.value, codeBlock("luau", "local part: Part"));

    loadSourcemap(R"(
        {
            "name": "game",
            "className": "DataModel",
            "children": [
                {
                    "name": "Workspace",
                    "className": "Workspace",
                    "children": [{ "name": "Part2", "className": "Part" }]
                }
            ]
        }
    )");

    auto hover2 = workspace.hover(params, nullptr);
    REQUIRE(hover2);
    if (FFlag::LuauSolverV2)
        CHECK_EQ(hover2->contents.value, codeBlock("luau", "local part: any"));
    else
        CHECK_EQ(hover2->contents.value, codeBlock("luau", "local part: *error-type*"));
}

TEST_CASE_FIXTURE(Fixture, "can_modify_the_parent_of_types_in_strict_mode")
{
    ENABLE_NEW_SOLVER();

    client->globalConfig.diagnostics.strictDatamodelTypes = true;
    loadSourcemap(R"(
        {
            "name": "game",
            "className": "DataModel",
            "children": [
                {
                    "name": "Workspace",
                    "className": "Workspace",
                    "children": [{ "name": "Part", "className": "Part" }]
                }
            ]
        }
    )");

    auto result = check(R"(
        --!strict
        local part = game.Workspace.Part
        part.Parent = Instance.new("TextLabel")
    )");

    LUAU_LSP_REQUIRE_NO_ERRORS(result);
}

TEST_CASE_FIXTURE(Fixture, "child_properties_of_services_are_cleared_when_the_service_is_removed_from_sourcemap")
{
    client->globalConfig.diagnostics.strictDatamodelTypes = true;

    loadSourcemap(R"(
        {
            "name": "game",
            "className": "DataModel",
            "children": [
                {
                    "name": "ReplicatedStorage",
                    "className": "ReplicatedStorage",
                    "children": [{ "name": "Part", "className": "Part" }]
                }
            ]
        }
    )");

    auto source = R"(
        --!strict
        local ReplicatedStorage = game:GetService("ReplicatedStorage")
        print(ReplicatedStorage.Part)
    )";

    auto result = check(source);

    LUAU_LSP_REQUIRE_NO_ERRORS(result);

    loadSourcemap(R"(
        {
            "name": "game",
            "className": "DataModel",
            "children": []
        }
    )");

    auto result2 = check(source);

    REQUIRE_EQ(result2.errors.size(), 1);
    CHECK_EQ(Luau::get<Luau::UnknownProperty>(result2.errors[0])->key, "Part");
}

TEST_CASE_FIXTURE(Fixture, "child_properties_of_game_are_cleared_when_an_invalid_sourcemap_is_given")
{
    client->globalConfig.diagnostics.strictDatamodelTypes = true;

    loadSourcemap(R"(
        {
            "name": "game",
            "className": "DataModel",
            "children": [
                {
                    "name": "Part",
                    "className": "Part"
                }
            ]
        }
    )");

    auto source = R"(
        --!strict
        print(game.Part)
    )";

    auto result = check(source);

    LUAU_LSP_REQUIRE_NO_ERRORS(result);

    loadSourcemap("");

    auto result2 = check(source);

    REQUIRE_EQ(result2.errors.size(), 1);
    CHECK_EQ(Luau::get<Luau::UnknownProperty>(result2.errors[0])->key, "Part");
}

TEST_CASE_FIXTURE(Fixture, "plugin_managed_flag_persists_through_sourcemap_reload")
{
    client->globalConfig.diagnostics.strictDatamodelTypes = true;

    // Load a sourcemap that includes pluginManaged flags (simulating a previously saved sourcemap)
    loadSourcemap(R"(
        {
            "name": "game",
            "className": "DataModel",
            "children": [
                {
                    "name": "ReplicatedStorage",
                    "className": "ReplicatedStorage",
                    "filePaths": ["src/ReplicatedStorage.luau"]
                },
                {
                    "name": "ServerStorage",
                    "className": "ServerStorage",
                    "pluginManaged": true
                }
            ]
        }
    )");

    auto platform = dynamic_cast<OverdarePlatform*>(workspace.platform.get());
    REQUIRE(platform->rootSourceNode);

    // Verify ReplicatedStorage is NOT plugin managed
    auto rsNode = platform->rootSourceNode->findChild("ReplicatedStorage");
    REQUIRE(rsNode);
    CHECK_FALSE((*rsNode)->pluginManaged);

    // Verify ServerStorage IS plugin managed (flag persisted from JSON)
    auto ssNode = platform->rootSourceNode->findChild("ServerStorage");
    REQUIRE(ssNode);
    CHECK((*ssNode)->pluginManaged);
}

TEST_CASE_FIXTURE(Fixture, "source_node_to_json_only_includes_nodes_with_file_paths")
{
    // Create a source node tree with some nodes having filePaths and some without
    Luau::TypedAllocator<SourceNode> allocator;

    auto child1 = allocator.allocate(SourceNode("ModuleA", "ModuleScript", {"src/ModuleA.luau"}, {}));
    auto child2 = allocator.allocate(SourceNode("PartNoFile", "Part", {}, {}));
    auto child3 = allocator.allocate(SourceNode("ModuleB", "ModuleScript", {"src/ModuleB.luau"}, {}));

    auto root = allocator.allocate(SourceNode("game", "DataModel", {}, {child1, child2, child3}));

    auto jsonOutput = root->toJson();

    CHECK_EQ(jsonOutput["name"], "game");
    CHECK_EQ(jsonOutput["className"], "DataModel");
    REQUIRE(jsonOutput.contains("children"));

    // Only nodes with filePaths should be in the output
    auto& children = jsonOutput["children"];
    CHECK_EQ(children.size(), 2);

    bool hasModuleA = false;
    bool hasModuleB = false;
    bool hasPartNoFile = false;

    for (const auto& child : children)
    {
        if (child["name"] == "ModuleA")
            hasModuleA = true;
        if (child["name"] == "ModuleB")
            hasModuleB = true;
        if (child["name"] == "PartNoFile")
            hasPartNoFile = true;
    }

    CHECK(hasModuleA);
    CHECK(hasModuleB);
    CHECK_FALSE(hasPartNoFile);
}

TEST_CASE_FIXTURE(Fixture, "source_node_to_json_includes_plugin_managed_flag")
{
    Luau::TypedAllocator<SourceNode> allocator;

    auto child = allocator.allocate(SourceNode("ServerStorage", "ServerStorage", {"src/server.luau"}, {}));
    child->pluginManaged = true;

    auto root = allocator.allocate(SourceNode("game", "DataModel", {}, {child}));

    auto jsonOutput = root->toJson();

    REQUIRE(jsonOutput.contains("children"));
    auto& children = jsonOutput["children"];
    REQUIRE_EQ(children.size(), 1);

    CHECK(children[0].contains("pluginManaged"));
    CHECK_EQ(children[0]["pluginManaged"], true);
}

TEST_CASE_FIXTURE(Fixture, "sourcemap_update_invalidates_stale_module_type_graphs")
{
    // Use an unmanaged on-disk file: recomputeDiagnostics (triggered by the sourcemap update
    // when expressive DataModel types are enabled) would immediately recheck a managed file,
    // which prevents observing the invalidation
    auto uri = Uri::file(tempDir.write_child("foo.luau", "local x = 1"));
    auto moduleName = workspace.fileResolver.getModuleName(uri);

    workspace.frontend.check(moduleName);
    REQUIRE(workspace.frontend.allModuleDependenciesValid(moduleName, /* forAutocomplete= */ false));

    loadSourcemap(R"(
        {
            "name": "game",
            "className": "DataModel",
            "children": []
        }
    )");

    // The retained type graph of an already-checked module may reference the sourcemap types
    // that were just destroyed. It must not be considered valid for incremental (fragment)
    // autocomplete until the module has been rechecked
    CHECK_FALSE(workspace.frontend.allModuleDependenciesValid(moduleName, /* forAutocomplete= */ false));
    CHECK_FALSE(workspace.frontend.allModuleDependenciesValid(moduleName, /* forAutocomplete= */ true));

    // A recheck makes the module valid again
    workspace.frontend.check(moduleName);
    CHECK(workspace.frontend.allModuleDependenciesValid(moduleName, /* forAutocomplete= */ false));
}

TEST_CASE_FIXTURE(Fixture, "sourcemap_file_change_detection_works_with_simple_filename")
{
    client->globalConfig.sourcemap.sourcemapFile = "sourcemap.json";
    client->notificationQueue.clear();

    lsp::FileEvent event;
    event.uri = workspace.rootUri.resolvePath("sourcemap.json");
    event.type = lsp::FileChangeType::Changed;

    auto platform = dynamic_cast<OverdarePlatform*>(workspace.platform.get());
    platform->onDidChangeWatchedFiles(event);

    bool foundLogMessage = false;
    for (const auto& [method, params] : client->notificationQueue)
    {
        if (method == "window/logMessage" && params)
        {
            auto message = params->value("message", "");
            if (message.find("Registering sourcemap changed") != std::string::npos)
            {
                foundLogMessage = true;
                break;
            }
        }
    }
    CHECK(foundLogMessage);
}

TEST_CASE_FIXTURE(Fixture, "sourcemap_file_change_detection_works_with_relative_paths")
{
    client->globalConfig.sourcemap.sourcemapFile = "subdir/sourcemap.json";
    client->notificationQueue.clear();

    lsp::FileEvent event;
    event.uri = workspace.rootUri.resolvePath("subdir/sourcemap.json");
    event.type = lsp::FileChangeType::Changed;

    auto platform = dynamic_cast<OverdarePlatform*>(workspace.platform.get());
    platform->onDidChangeWatchedFiles(event);

    bool foundLogMessage = false;
    for (const auto& [method, params] : client->notificationQueue)
    {
        if (method == "window/logMessage" && params)
        {
            auto message = params->value("message", "");
            if (message.find("Registering sourcemap changed") != std::string::npos)
            {
                foundLogMessage = true;
                break;
            }
        }
    }
    CHECK(foundLogMessage);
}

TEST_CASE_FIXTURE(Fixture, "source_node_get_script_context_resolution")
{
    auto platform = dynamic_cast<OverdarePlatform*>(workspace.platform.get());
    REQUIRE(platform);

    loadSourcemap(R"(
        {
            "name": "game",
            "className": "DataModel",
            "children": [
                {
                    "name": "Workspace",
                    "className": "Workspace",
                    "children": [
                        { "name": "SharedModule", "className": "ModuleScript" },
                        { "name": "LocalScriptInWorkspace", "className": "LocalScript" },
                        { "name": "ScriptInWorkspace", "className": "Script" }
                    ]
                },
                {
                    "name": "ServerScriptService",
                    "className": "ServerScriptService",
                    "children": [
                        {
                            "name": "Folder",
                            "className": "Folder",
                            "children": [
                                { "name": "NestedServerModule", "className": "ModuleScript" }
                            ]
                        }
                    ]
                },
                {
                    "name": "StarterPlayer",
                    "className": "StarterPlayer",
                    "children": [
                        { "name": "ClientModule", "className": "ModuleScript" }
                    ]
                }
            ]
        }
    )");

    REQUIRE(platform->rootSourceNode);
    auto root = platform->rootSourceNode;

    auto workspaceNode = root->findChild("Workspace");
    REQUIRE(workspaceNode);
    auto sharedModule = (*workspaceNode)->findChild("SharedModule");
    REQUIRE(sharedModule);
    CHECK_EQ((*sharedModule)->scriptContext, ScriptContext::Shared);

    auto localInWorkspace = (*workspaceNode)->findChild("LocalScriptInWorkspace");
    REQUIRE(localInWorkspace);
    CHECK_EQ((*localInWorkspace)->scriptContext, ScriptContext::Client);

    auto scriptInWorkspace = (*workspaceNode)->findChild("ScriptInWorkspace");
    REQUIRE(scriptInWorkspace);
    CHECK_EQ((*scriptInWorkspace)->scriptContext, ScriptContext::Server);

    auto sss = root->findChild("ServerScriptService");
    REQUIRE(sss);
    auto folder = (*sss)->findChild("Folder");
    REQUIRE(folder);
    auto nestedServerModule = (*folder)->findChild("NestedServerModule");
    REQUIRE(nestedServerModule);
    CHECK_EQ((*nestedServerModule)->scriptContext, ScriptContext::Server);

    auto starterPlayer = root->findChild("StarterPlayer");
    REQUIRE(starterPlayer);
    auto clientModule = (*starterPlayer)->findChild("ClientModule");
    REQUIRE(clientModule);
    CHECK_EQ((*clientModule)->scriptContext, ScriptContext::Client);
}

TEST_SUITE_END();
