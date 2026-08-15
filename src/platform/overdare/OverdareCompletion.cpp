#include "Platform/OverdarePlatform.hpp"

#include <algorithm>
#include <unordered_set>

#include "Luau/TimeTrace.h"
#include "LuauFileUtils.hpp"

#include "LSP/Completion.hpp"

#include "Platform/AutoImports.hpp"
#include "Platform/InstanceRequireAutoImporter.hpp"
#include "Platform/StringRequireAutoImporter.hpp"

LUAU_FASTFLAG(LuauSolverV2)

static constexpr const char* COMMON_SERVICES[] = {
    "Players",
    "ReplicatedStorage",
    "ServerStorage",
    "MessagingService",
    "TeleportService",
    "HttpService",
    "CollectionService",
    "DataStoreService",
    "ContextActionService",
    "UserInputService",
    "Teams",
    "Chat",
    "TextService",
    "TextChatService",
    "GamepadService",
    "VoiceChatService",
};

static constexpr const char* COMMON_INSTANCE_PROPERTIES[] = {
    "Parent",
    "Name",
    // Methods
    "FindFirstChild",
    "IsA",
    "Destroy",
    "GetAttribute",
    "GetChildren",
    "GetDescendants",
    "WaitForChild",
    "Clone",
    "SetAttribute",
};

static constexpr const char* COMMON_SERVICE_PROVIDER_PROPERTIES[] = {
    "GetService",
};

static lsp::CompletionItem createSuggestService(const std::string& service, size_t lineNumber, bool appendNewline = false, bool useConst = false)
{
    auto textEdit = Luau::LanguageServer::AutoImports::createServiceTextEdit(service, lineNumber, appendNewline, useConst);

    lsp::CompletionItem item;
    item.label = service;
    item.kind = lsp::CompletionItemKind::Class;
    item.detail = "Auto-import";
    item.documentation = {lsp::MarkupKind::Markdown, codeBlock("luau", textEdit.newText)};
    item.insertText = service;
    item.sortText = SortText::AutoImports;

    item.additionalTextEdits.emplace_back(textEdit);

    return item;
}

std::optional<Luau::AutocompleteEntryMap> OverdarePlatform::completionCallback(
    const std::string& tag, std::optional<const Luau::ExternType*> ctx, std::optional<std::string> contents, const Luau::ModuleName& moduleName)
{
    if (auto parentResult = LSPPlatform::completionCallback(tag, ctx, contents, moduleName))
        return parentResult;

    std::optional<OverdareDefinitionsFileMetadata> metadata = workspaceFolder->definitionsFileMetadata;

    if (tag == "ClassNames")
    {
        if (auto instanceType = workspaceFolder->frontend.globals.globalScope->lookupType("Instance"))
        {
            if (auto* ctv = Luau::get<Luau::ExternType>(instanceType->type))
            {
                std::optional<std::unordered_set<std::string>> validClasses;
                if (metadata.has_value() && !metadata->CLASSES.empty())
                    validClasses = std::unordered_set<std::string>(metadata->CLASSES.begin(), metadata->CLASSES.end());

                Luau::AutocompleteEntryMap result;
                for (auto& [_, ty] : workspaceFolder->frontend.globals.globalScope->exportedTypeBindings)
                {
                    if (auto* c = Luau::get<Luau::ExternType>(ty.type))
                    {
                        // Check if the ctv is a subclass of instance
                        if (!Luau::isSubclass(c, ctv))
                            continue;

                        // If the definitions file declares which classes are real OVERDARE
                        // classes, don't suggest the rest (leftover/untouched Roblox classes
                        // still declared in the file for other classes to extend/reference).
                        if (validClasses.has_value() && validClasses->find(c->name) == validClasses->end())
                            continue;

                        result.insert_or_assign(
                            c->name, Luau::AutocompleteEntry{Luau::AutocompleteEntryKind::String,
                                         workspaceFolder->frontend.builtinTypes->stringType, false, false, Luau::TypeCorrectKind::Correct});
                    }
                }

                return result;
            }
        }
    }
    else if (tag == "Properties")
    {
        if (ctx && ctx.value())
        {
            Luau::AutocompleteEntryMap result;
            auto ctv = ctx.value();
            while (ctv)
            {
                for (auto& [propName, prop] : ctv->props)
                {
                    // Don't include functions or events
                    LUAU_ASSERT(prop.readTy);
                    auto ty = Luau::follow(*prop.readTy);
                    if (Luau::get<Luau::FunctionType>(ty) || Luau::isOverloadedFunction(ty))
                        continue;
                    else if (auto ttv = Luau::get<Luau::TableType>(ty); ttv && ttv->name && ttv->name.value() == "RBXScriptSignal")
                        continue;
                    else if (Luau::hasTag(prop, kSourcemapGeneratedTag))
                        continue;

                    result.insert_or_assign(
                        propName, Luau::AutocompleteEntry{Luau::AutocompleteEntryKind::String, workspaceFolder->frontend.builtinTypes->stringType,
                                      false, false, Luau::TypeCorrectKind::Correct});
                }
                if (ctv->parent)
                    ctv = Luau::get<Luau::ExternType>(*ctv->parent);
                else
                    break;
            }
            return result;
        }
    }
    else if (tag == "Children")
    {
        if (auto ctv = ctx.value())
        {
            Luau::AutocompleteEntryMap result;
            for (auto& [propName, prop] : ctv->props)
            {
                if (Luau::hasTag(prop, kSourcemapGeneratedTag) &&
                    !(prop.readTy && (Luau::is<Luau::FunctionType>(*prop.readTy) || Luau::isOverloadedFunction(*prop.readTy))))
                    result.insert_or_assign(
                        propName, Luau::AutocompleteEntry{Luau::AutocompleteEntryKind::String, workspaceFolder->frontend.builtinTypes->stringType,
                                      false, false, Luau::TypeCorrectKind::Correct});
            }
            return result;
        }
    }
    else if (tag == "Enums")
    {
        auto it = workspaceFolder->frontend.globals.globalScope->importedTypeBindings.find("Enum");
        if (it == workspaceFolder->frontend.globals.globalScope->importedTypeBindings.end())
            return std::nullopt;

        Luau::AutocompleteEntryMap result;
        for (auto& [enumName, _] : it->second)
            result.insert_or_assign(enumName, Luau::AutocompleteEntry{Luau::AutocompleteEntryKind::String,
                                                  workspaceFolder->frontend.builtinTypes->stringType, false, false, Luau::TypeCorrectKind::Correct});

        return result;
    }
    else if (tag == "CreatableInstances")
    {
        Luau::AutocompleteEntryMap result;
        if (metadata)
        {
            for (const auto& className : metadata->CREATABLE_INSTANCES)
                result.insert_or_assign(
                    className, Luau::AutocompleteEntry{Luau::AutocompleteEntryKind::String, workspaceFolder->frontend.builtinTypes->stringType, false,
                                   false, Luau::TypeCorrectKind::Correct});
        }
        return result;
    }
    else if (tag == "Services")
    {
        Luau::AutocompleteEntryMap result;

        // We are autocompleting a `game:GetService("$1")` call, so we set a flag to
        // highlight this so that we can prioritise common services first in the list
        if (metadata)
        {
            for (const auto& className : metadata->SERVICES)
                result.insert_or_assign(
                    className, Luau::AutocompleteEntry{Luau::AutocompleteEntryKind::String, workspaceFolder->frontend.builtinTypes->stringType, false,
                                   false, Luau::TypeCorrectKind::Correct});
        }

        return result;
    }

    return std::nullopt;
}

void OverdarePlatform::handleCompletion(
    const TextDocument& textDocument, const Luau::SourceModule& module, Luau::Position position, std::vector<lsp::CompletionItem>& items)
{
    // Type-annotation completion (`local x: |`, etc.) is Luau core's own autocomplete
    // behaviour, not something routed through completionCallback's tag system - so unlike
    // GetService/Instance.new/IsA, there's no metadata whitelist hook for it upstream. Post-
    // filter here instead: drop any suggested Instance-derived class name that isn't a real
    // OVERDARE class (leaving non-Instance types - Vector3, CFrame, generic aliases, etc. -
    // alone since CLASSES only enumerates Instance subclasses), and separately drop any
    // suggested EnumX type name that isn't a real OVERDARE enum.
    std::optional<OverdareDefinitionsFileMetadata> metadata = workspaceFolder->definitionsFileMetadata;
    if (!metadata.has_value())
        return;

    auto instanceType = workspaceFolder->frontend.globals.globalScope->lookupType("Instance");
    auto* instanceCtv = instanceType ? Luau::get<Luau::ExternType>(instanceType->type) : nullptr;
    auto enumItemType = workspaceFolder->frontend.globals.globalScope->lookupType("EnumItem");
    auto* enumItemCtv = enumItemType ? Luau::get<Luau::ExternType>(enumItemType->type) : nullptr;

    if ((!instanceCtv || metadata->CLASSES.empty()) && (!enumItemCtv || metadata->ENUMS.empty()))
        return;

    std::unordered_set<std::string> validClasses(metadata->CLASSES.begin(), metadata->CLASSES.end());
    std::unordered_set<std::string> validEnums(metadata->ENUMS.begin(), metadata->ENUMS.end());

    items.erase(std::remove_if(items.begin(), items.end(),
                    [&](const lsp::CompletionItem& item)
                    {
                        if (item.kind != lsp::CompletionItemKind::Interface)
                            return false;

                        auto ty = workspaceFolder->frontend.globals.globalScope->lookupType(item.label);
                        if (!ty)
                            return false;
                        auto* ctv = Luau::get<Luau::ExternType>(ty->type);
                        if (!ctv)
                            return false;

                        if (instanceCtv && !metadata->CLASSES.empty() && Luau::isSubclass(ctv, instanceCtv))
                            return validClasses.find(item.label) == validClasses.end();

                        // "Enum"/"EnumItem" themselves are foundational plumbing types, not
                        // real enums, and aren't in ENUMS - only filter the "EnumFoo" ones.
                        if (enumItemCtv && !metadata->ENUMS.empty() && item.label != "EnumItem" && item.label != "Enum" &&
                            Luau::isSubclass(ctv, enumItemCtv))
                        {
                            auto bareName = Luau::startsWith(item.label, "Enum") ? item.label.substr(4) : item.label;
                            return validEnums.find(bareName) == validEnums.end();
                        }

                        return false;
                    }),
        items.end());
}

const char* OverdarePlatform::handleSortText(
    const Luau::Frontend& frontend, const std::string& name, const Luau::AutocompleteEntry& entry, const std::unordered_set<std::string>& tags)
{
    // If it's a `game:GetSerivce("$1")` call, then prioritise common services
    if (tags.count("Services"))
        if (auto it = std::find(std::begin(COMMON_SERVICES), std::end(COMMON_SERVICES), name); it != std::end(COMMON_SERVICES))
            return SortText::PrioritisedSuggestion;

    // If calling a property on ServiceProvider, then prioritise these properties
    auto& completionGlobals = FFlag::LuauSolverV2 ? frontend.globals : frontend.globalsForAutocomplete;
    if (auto dataModelType = completionGlobals.globalScope->lookupType("ServiceProvider");
        dataModelType && Luau::get<Luau::ExternType>(dataModelType->type) && entry.containingExternType &&
        Luau::isSubclass(entry.containingExternType.value(), Luau::get<Luau::ExternType>(dataModelType->type)) && !entry.wrongIndexType)
    {
        if (auto it = std::find(std::begin(COMMON_SERVICE_PROVIDER_PROPERTIES), std::end(COMMON_SERVICE_PROVIDER_PROPERTIES), name);
            it != std::end(COMMON_SERVICE_PROVIDER_PROPERTIES))
            return SortText::PrioritisedSuggestion;
    }

    // If calling a property on an Instance, then prioritise these properties
    else if (auto instanceType = completionGlobals.globalScope->lookupType("Instance");
        instanceType && Luau::get<Luau::ExternType>(instanceType->type) && entry.containingExternType &&
        Luau::isSubclass(entry.containingExternType.value(), Luau::get<Luau::ExternType>(instanceType->type)) && !entry.wrongIndexType)
    {
        if (auto it = std::find(std::begin(COMMON_INSTANCE_PROPERTIES), std::end(COMMON_INSTANCE_PROPERTIES), name);
            it != std::end(COMMON_INSTANCE_PROPERTIES))
            return SortText::PrioritisedSuggestion;
    }

    return nullptr;
}

std::optional<lsp::CompletionItemKind> OverdarePlatform::handleEntryKind(const Luau::AutocompleteEntry& entry)
{
    if (entry.type.has_value())
    {
        auto id = Luau::follow(entry.type.value());

        if (auto ttv = Luau::get<Luau::TableType>(id))
        {
            // Special case the RBXScriptSignal type as a connection
            if (ttv->name && ttv->name.value() == "RBXScriptSignal")
                return lsp::CompletionItemKind::Event;
        }
    }

    return std::nullopt;
}

void OverdarePlatform::handleSuggestImports(const TextDocument& textDocument, const Luau::SourceModule& module, const ClientConfiguration& config,
    size_t hotCommentsLineNumber, bool completingTypeReferencePrefix, std::vector<lsp::CompletionItem>& items)
{
    LUAU_TIMETRACE_SCOPE("OverdarePlatform::handleSuggestImports", "LSP");

    // Find all import calls
    Luau::LanguageServer::AutoImports::OverdareFindImportsVisitor importsVisitor;
    importsVisitor.visit(module.root);

    if (config.completion.imports.suggestServices && !completingTypeReferencePrefix)
    {
        std::optional<OverdareDefinitionsFileMetadata> metadata = workspaceFolder->definitionsFileMetadata;

        auto services = metadata.has_value() ? metadata->SERVICES : std::vector<std::string>{};
        for (auto& service : services)
        {
            // ASSUMPTION: if the service was defined, it was defined with the exact same name
            if (contains(importsVisitor.serviceLineMap, service))
                continue;

            if ((!config.completion.imports.includedServices.empty() && !contains(config.completion.imports.includedServices, service)) ||
                contains(config.completion.imports.excludedServices, service))
                continue;

            size_t lineNumber = importsVisitor.findBestLineForService(service, hotCommentsLineNumber);

            bool appendNewline = false;
            if (config.completion.imports.separateGroupsWithLine && importsVisitor.firstRequireLine &&
                importsVisitor.firstRequireLine.value() - lineNumber == 0)
                appendNewline = true;

            items.emplace_back(createSuggestService(service, lineNumber, appendNewline, config.completion.imports.useConst));
        }
    }

    if (config.completion.imports.suggestRequires)
    {
        if (config.completion.imports.stringRequires.enabled)
        {
            Luau::LanguageServer::AutoImports::StringRequireAutoImporterContext ctx{
                module.name,
                Luau::NotNull(&textDocument),
                getAutoImportsModuleVisitor(module.name),
                Luau::NotNull(workspaceFolder),
                Luau::NotNull(&config.completion.imports),
                hotCommentsLineNumber,
                Luau::NotNull(&importsVisitor),
                getAutoImportsRequirePathComputer(module.name, config.completion.imports.requireStyle),
            };

            return Luau::LanguageServer::AutoImports::suggestStringRequires(ctx, items);
        }
        else
        {
            Luau::LanguageServer::AutoImports::InstanceRequireAutoImporterContext ctx{
                module.name,
                Luau::NotNull(&textDocument),
                Luau::NotNull(&workspaceFolder->frontend),
                Luau::NotNull(workspaceFolder),
                Luau::NotNull(&config.completion.imports),
                hotCommentsLineNumber,
                Luau::NotNull(&importsVisitor),
                Luau::NotNull(this),
            };

            return Luau::LanguageServer::AutoImports::suggestInstanceRequires(ctx, items);
        }
    }
}
