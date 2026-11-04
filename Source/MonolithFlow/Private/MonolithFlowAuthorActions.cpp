#include "MonolithFlowAuthorActions.h"

#if WITH_FLOW

#include "MonolithParamSchema.h"
#include "MonolithAssetUtils.h"
#include "IMonolithGraphFormatter.h"

#include "FlowAsset.h"
#include "Nodes/FlowNode.h"
#include "Nodes/FlowNodeBase.h"
#include "Graph/FlowGraphSchema_Actions.h"
#include "Graph/Nodes/FlowGraphNode.h"
#include "Asset/FlowAssetFactory.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "EdGraphNode_Comment.h"

#include "Editor.h"
#include "ScopedTransaction.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "EditorAssetLibrary.h"
#include "EditorValidatorSubsystem.h"
#include "Misc/DataValidation.h"
#include "JsonObjectConverter.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Algo/MaxElement.h"

#define LOCTEXT_NAMESPACE "MonolithFlowAuthorActions"

// ============================================================================
// Local helpers
// ============================================================================

namespace
{
	/** Load a UFlowAsset from the standard asset_path param. */
	UFlowAsset* LoadFlowAssetParam(const TSharedPtr<FJsonObject>& Params, FString& OutError)
	{
		FString AssetPath;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
		{
			OutError = TEXT("asset_path is required");
			return nullptr;
		}
		UFlowAsset* FA = FMonolithAssetUtils::LoadAssetByPath<UFlowAsset>(AssetPath);
		if (!FA)
		{
			OutError = FString::Printf(TEXT("Flow Asset not found: %s"), *AssetPath);
		}
		return FA;
	}

	/** Resolve a runtime node by guid string (accepts hyphenated and compact forms). */
	UFlowNode* FindRuntimeNode(UFlowAsset* FA, const FString& GuidStr, FString& OutError)
	{
		FGuid Guid;
		if (!FGuid::Parse(GuidStr, Guid))
		{
			OutError = FString::Printf(TEXT("Invalid node guid: %s"), *GuidStr);
			return nullptr;
		}
		UFlowNode* Node = FA->GetNodes().FindRef(Guid);
		if (!Node)
		{
			OutError = FString::Printf(TEXT("Node %s not found in %s"), *GuidStr, *FA->GetPathName());
		}
		return Node;
	}

	/** Editor wrapper of a runtime node. */
	UFlowGraphNode* GraphNodeOf(UFlowNode* Node, FString& OutError)
	{
		UFlowGraphNode* GraphNode = Cast<UFlowGraphNode>(Node->GetGraphNode());
		if (!GraphNode)
		{
			OutError = FString::Printf(TEXT("Node %s has no editor graph wrapper"), *Node->GetGuid().ToString());
		}
		return GraphNode;
	}

	/** Resolve a UFlowNode UClass from a class path ("/Script/Pkg.Class" or "/Game/BP.BP_C"). */
	UClass* ResolveFlowNodeClass(const FString& ClassPath, FString& OutError)
	{
		UClass* Cls = LoadObject<UClass>(nullptr, *ClassPath);
		if (!Cls && !ClassPath.EndsWith(TEXT("_C")))
		{
			// Allow passing a BP asset path without the generated-class suffix.
			Cls = LoadObject<UClass>(nullptr, *(ClassPath + TEXT("_C")));
		}
		if (!Cls)
		{
			OutError = FString::Printf(TEXT("Node class not found: %s"), *ClassPath);
			return nullptr;
		}
		if (!Cls->IsChildOf(UFlowNode::StaticClass()))
		{
			OutError = FString::Printf(TEXT("%s is not a UFlowNode subclass"), *ClassPath);
			return nullptr;
		}
		if (Cls->HasAnyClassFlags(CLASS_Abstract))
		{
			OutError = FString::Printf(TEXT("%s is abstract"), *ClassPath);
			return nullptr;
		}
		return Cls;
	}

	/**
	 * Apply a JSON object onto a node's UPROPERTYs. Reports unknown top-level
	 * keys instead of silently ignoring them (typo protection for the caller).
	 */
	bool ApplyPropertiesJson(UObject* Target, const TSharedPtr<FJsonObject>& Props, FString& OutError)
	{
		TArray<FString> Unknown;
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Props->Values)
		{
			if (!Target->GetClass()->FindPropertyByName(FName(*Pair.Key)))
			{
				Unknown.Add(Pair.Key);
			}
		}
		if (Unknown.Num() > 0)
		{
			OutError = FString::Printf(TEXT("Unknown properties on %s: %s"),
				*Target->GetClass()->GetName(), *FString::Join(Unknown, TEXT(", ")));
			return false;
		}

		Target->Modify();
		if (!FJsonObjectConverter::JsonObjectToUStruct(
				Props.ToSharedRef(), Target->GetClass(), Target, /*CheckFlags=*/0, /*SkipFlags=*/0))
		{
			OutError = FString::Printf(TEXT("Failed to apply property JSON to %s"), *Target->GetClass()->GetName());
			return false;
		}
		Target->PostEditChange();
		return true;
	}

	/** Optional bSave param: save the asset after a successful mutation. */
	bool MaybeSave(const TSharedPtr<FJsonObject>& Params, UFlowAsset* FA, const TSharedRef<FJsonObject>& Result)
	{
		bool bSave = false;
		Params->TryGetBoolField(TEXT("save"), bSave);
		if (bSave)
		{
			const bool bSaved = UEditorAssetLibrary::SaveLoadedAsset(FA, false);
			Result->SetBoolField(TEXT("saved"), bSaved);
			return bSaved;
		}
		return true;
	}

	FString GuidStrOf(const UFlowNode* Node)
	{
		return Node->GetGuid().ToString(EGuidFormats::DigitsWithHyphens);
	}
}

// ============================================================================
// Registration
// ============================================================================

void FMonolithFlowAuthorActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("flow"), TEXT("create_asset"),
		TEXT("Create a new, empty UFlowAsset with its editor graph and default Start node. asset_path is the package path + name, e.g. \"/Game/Flow/FLOW_MyScenario\". Pass save=true to write it to disk immediately."),
		FMonolithActionHandler::CreateStatic(&FMonolithFlowAuthorActions::CreateAsset),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Target path, e.g. \"/Game/Flow/FLOW_MyScenario\""))
			.Optional(TEXT("save"), TEXT("boolean"), TEXT("Save the new asset to disk (default false)"))
			.Build());

	Registry.RegisterAction(TEXT("flow"), TEXT("save_asset"),
		TEXT("Save one Flow Asset to disk. Saving also refreshes the flow_* index tables via the live incremental indexer, so read actions see the change."),
		FMonolithActionHandler::CreateStatic(&FMonolithFlowAuthorActions::SaveAsset),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Full UFlowAsset object path"))
			.Build());

	Registry.RegisterAction(TEXT("flow"), TEXT("validate_asset"),
		TEXT("Run every registered data validator on one Flow Asset - including per-node ValidateNode() checks and project validator plugins. Returns Valid / Invalid / NotValidated plus the error and warning strings."),
		FMonolithActionHandler::CreateStatic(&FMonolithFlowAuthorActions::ValidateAsset),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Full UFlowAsset object path"))
			.Build());

	Registry.RegisterAction(TEXT("flow"), TEXT("add_node"),
		TEXT("Add a node of the given UFlowNode subclass to a Flow Asset at (pos_x, pos_y). Optional `properties` JSON is applied onto the fresh node's UPROPERTYs (same shape as the `data` snapshot from get_node_info) and its pins are rebuilt - so context-pin nodes (e.g. scenario switches) come up with correct pins. Returns the new node_guid. Mutates the LIVE asset; pass save=true (or call save_asset later) to persist and refresh the index."),
		FMonolithActionHandler::CreateStatic(&FMonolithFlowAuthorActions::AddNode),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Full UFlowAsset object path"))
			.Required(TEXT("node_class"), TEXT("string"), TEXT("UFlowNode subclass path, e.g. \"/Script/SIMULATOR.SimulatorFlowNode_Narrator\""))
			.Optional(TEXT("pos_x"), TEXT("integer"), TEXT("Graph X position (default 0)"))
			.Optional(TEXT("pos_y"), TEXT("integer"), TEXT("Graph Y position (default 0)"))
			.Optional(TEXT("properties"), TEXT("object"), TEXT("Property JSON to apply onto the new node"))
			.Optional(TEXT("save"), TEXT("boolean"), TEXT("Save the asset after the edit (default false)"))
			.Build());

	Registry.RegisterAction(TEXT("flow"), TEXT("set_node_properties"),
		TEXT("Apply a property JSON onto an existing node's UPROPERTYs and rebuild its pins (ReconstructNode - refreshes context pins). Unknown property names are rejected explicitly. Mutates the LIVE asset; save to persist."),
		FMonolithActionHandler::CreateStatic(&FMonolithFlowAuthorActions::SetNodeProperties),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Full UFlowAsset object path"))
			.Required(TEXT("node_guid"), TEXT("string"), TEXT("Guid of the node to edit"))
			.Required(TEXT("properties"), TEXT("object"), TEXT("Property JSON to apply"))
			.Optional(TEXT("save"), TEXT("boolean"), TEXT("Save the asset after the edit (default false)"))
			.Build());

	Registry.RegisterAction(TEXT("flow"), TEXT("remove_node"),
		TEXT("Remove a node from a Flow Asset: breaks its links, destroys the editor wrapper and unregisters the runtime node. Mutates the LIVE asset; save to persist."),
		FMonolithActionHandler::CreateStatic(&FMonolithFlowAuthorActions::RemoveNode),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Full UFlowAsset object path"))
			.Required(TEXT("node_guid"), TEXT("string"), TEXT("Guid of the node to remove"))
			.Optional(TEXT("save"), TEXT("boolean"), TEXT("Save the asset after the edit (default false)"))
			.Build());

	Registry.RegisterAction(TEXT("flow"), TEXT("add_comment"),
		TEXT("Add a comment box to the graph at (pos_x, pos_y) with the given size. Mutates the LIVE asset; save to persist."),
		FMonolithActionHandler::CreateStatic(&FMonolithFlowAuthorActions::AddComment),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Full UFlowAsset object path"))
			.Required(TEXT("text"), TEXT("string"), TEXT("Comment text"))
			.Optional(TEXT("pos_x"), TEXT("integer"), TEXT("X position (default 0)"))
			.Optional(TEXT("pos_y"), TEXT("integer"), TEXT("Y position (default 0)"))
			.Optional(TEXT("width"), TEXT("integer"), TEXT("Box width (default 400)"))
			.Optional(TEXT("height"), TEXT("integer"), TEXT("Box height (default 200)"))
			.Optional(TEXT("save"), TEXT("boolean"), TEXT("Save the asset after the edit (default false)"))
			.Build());

	Registry.RegisterAction(TEXT("flow"), TEXT("connect_pins"),
		TEXT("Connect an output pin of one node to an input pin of another (schema-mediated - the same validation as manual wiring), then re-harvest runtime connections. to_pin defaults to \"In\". Mutates the LIVE asset; save to persist."),
		FMonolithActionHandler::CreateStatic(&FMonolithFlowAuthorActions::ConnectPins),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Full UFlowAsset object path"))
			.Required(TEXT("from_node_guid"), TEXT("string"), TEXT("Source node guid"))
			.Required(TEXT("from_pin"), TEXT("string"), TEXT("Source OUTPUT pin name (e.g. \"Out\", \"Completed\", \"Scenario_2\")"))
			.Required(TEXT("to_node_guid"), TEXT("string"), TEXT("Target node guid"))
			.Optional(TEXT("to_pin"), TEXT("string"), TEXT("Target INPUT pin name (default \"In\")"))
			.Optional(TEXT("save"), TEXT("boolean"), TEXT("Save the asset after the edit (default false)"))
			.Build());

	Registry.RegisterAction(TEXT("flow"), TEXT("disconnect_pins"),
		TEXT("Break every link of one pin on a node (either direction), then re-harvest runtime connections. Mutates the LIVE asset; save to persist."),
		FMonolithActionHandler::CreateStatic(&FMonolithFlowAuthorActions::DisconnectPins),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Full UFlowAsset object path"))
			.Required(TEXT("node_guid"), TEXT("string"), TEXT("Node guid"))
			.Required(TEXT("pin_name"), TEXT("string"), TEXT("Pin whose links to break"))
			.Optional(TEXT("save"), TEXT("boolean"), TEXT("Save the asset after the edit (default false)"))
			.Build());

	Registry.RegisterAction(TEXT("flow"), TEXT("auto_layout"),
		TEXT("Auto-layout a Flow graph left-to-right by execution flow. layout_mode: 'all' (default) re-lays the whole graph with a layered algorithm; 'new_only' repositions only nodes sitting at (0,0) next to their connected neighbours; 'selected' does the same for the node_ids list. formatter: 'monolith' (default, built-in), 'blueprint_assist' (requires BA installed AND the asset open in an editor tab), 'auto' (BA when possible, built-in otherwise). Mutates the LIVE asset; save to persist."),
		FMonolithActionHandler::CreateStatic(&FMonolithFlowAuthorActions::AutoLayout),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Full UFlowAsset object path"))
			.Optional(TEXT("layout_mode"), TEXT("string"), TEXT("'all' (default) | 'new_only' | 'selected'"))
			.Optional(TEXT("node_ids"), TEXT("array"), TEXT("Node guids for 'selected' mode"))
			.Optional(TEXT("horizontal_spacing"), TEXT("integer"), TEXT("Distance between layers in px (default 420)"))
			.Optional(TEXT("vertical_spacing"), TEXT("integer"), TEXT("Distance between nodes in a layer in px (default 60)"))
			.Optional(TEXT("formatter"), TEXT("string"), TEXT("'monolith' (default) | 'blueprint_assist' | 'auto'"))
			.Optional(TEXT("save"), TEXT("boolean"), TEXT("Save the asset after the edit (default false)"))
			.Build());
}

// ============================================================================
// Asset lifecycle
// ============================================================================

FMonolithActionResult FMonolithFlowAuthorActions::CreateAsset(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path is required"));
	}

	// Accept both "/Game/Flow/Name" and "/Game/Flow/Name.Name" forms.
	FString PackagePath = AssetPath;
	int32 DotIdx;
	if (PackagePath.FindChar(TEXT('.'), DotIdx))
	{
		PackagePath.LeftInline(DotIdx);
	}
	FString AssetName;
	if (!PackagePath.Split(TEXT("/"), &PackagePath, &AssetName, ESearchCase::CaseSensitive, ESearchDir::FromEnd)
		|| AssetName.IsEmpty())
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Cannot split '%s' into package path + asset name"), *AssetPath));
	}

	if (FMonolithAssetUtils::AssetExists(PackagePath / AssetName))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Asset already exists: %s/%s"), *PackagePath, *AssetName));
	}

	IAssetTools& AssetTools = FAssetToolsModule::GetModule().Get();
	UObject* NewAsset = AssetTools.CreateAsset(AssetName, PackagePath, UFlowAsset::StaticClass(), NewObject<UFlowAssetFactory>());
	UFlowAsset* FA = Cast<UFlowAsset>(NewAsset);
	if (!FA)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create Flow Asset at %s/%s"), *PackagePath, *AssetName));
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), FA->GetPathName());
	Result->SetNumberField(TEXT("node_count"), FA->GetNodes().Num());
	if (!MaybeSave(Params, FA, Result))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Created but failed to save %s"), *FA->GetPathName()));
	}
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithFlowAuthorActions::SaveAsset(const TSharedPtr<FJsonObject>& Params)
{
	FString Err;
	UFlowAsset* FA = LoadFlowAssetParam(Params, Err);
	if (!FA) return FMonolithActionResult::Error(Err);

	const bool bWasDirty = FA->GetOutermost()->IsDirty();
	const bool bSaved = UEditorAssetLibrary::SaveLoadedAsset(FA, false);

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), FA->GetPathName());
	Result->SetBoolField(TEXT("saved"), bSaved);
	Result->SetBoolField(TEXT("was_dirty"), bWasDirty);
	if (!bSaved)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to save %s"), *FA->GetPathName()));
	}
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithFlowAuthorActions::ValidateAsset(const TSharedPtr<FJsonObject>& Params)
{
	FString Err;
	UFlowAsset* FA = LoadFlowAssetParam(Params, Err);
	if (!FA) return FMonolithActionResult::Error(Err);

	if (!GEditor)
	{
		return FMonolithActionResult::Error(TEXT("GEditor not available"));
	}
	UEditorValidatorSubsystem* Validator = GEditor->GetEditorSubsystem<UEditorValidatorSubsystem>();
	if (!Validator)
	{
		return FMonolithActionResult::Error(TEXT("EditorValidatorSubsystem not available"));
	}

	TArray<FText> Errors, Warnings;
	const EDataValidationResult ValidationResult =
		Validator->IsObjectValid(FA, Errors, Warnings, EDataValidationUsecase::Script);

	auto ToJsonArray = [](const TArray<FText>& Texts)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		for (const FText& Text : Texts)
		{
			Out.Add(MakeShared<FJsonValueString>(Text.ToString()));
		}
		return Out;
	};

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), FA->GetPathName());
	switch (ValidationResult)
	{
		case EDataValidationResult::Valid:        Result->SetStringField(TEXT("result"), TEXT("Valid")); break;
		case EDataValidationResult::Invalid:      Result->SetStringField(TEXT("result"), TEXT("Invalid")); break;
		default:                                  Result->SetStringField(TEXT("result"), TEXT("NotValidated")); break;
	}
	Result->SetArrayField(TEXT("errors"), ToJsonArray(Errors));
	Result->SetArrayField(TEXT("warnings"), ToJsonArray(Warnings));
	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// Node authoring
// ============================================================================

FMonolithActionResult FMonolithFlowAuthorActions::AddNode(const TSharedPtr<FJsonObject>& Params)
{
	FString Err;
	UFlowAsset* FA = LoadFlowAssetParam(Params, Err);
	if (!FA) return FMonolithActionResult::Error(Err);

	FString ClassPath;
	if (!Params->TryGetStringField(TEXT("node_class"), ClassPath) || ClassPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("node_class is required"));
	}
	UClass* NodeClass = ResolveFlowNodeClass(ClassPath, Err);
	if (!NodeClass) return FMonolithActionResult::Error(Err);

	UEdGraph* Graph = FA->GetGraph();
	if (!Graph)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("%s has no editor graph"), *FA->GetPathName()));
	}

	double PosX = 0, PosY = 0;
	Params->TryGetNumberField(TEXT("pos_x"), PosX);
	Params->TryGetNumberField(TEXT("pos_y"), PosY);

	const FScopedTransaction Transaction(LOCTEXT("AddFlowNode", "Monolith: Add Flow Node"));
	FA->Modify();
	Graph->Modify();

	UFlowGraphNode* GraphNode = FFlowGraphSchemaAction_NewNode::CreateNode(
		Graph, /*FromPin=*/nullptr, NodeClass,
		FVector2f(static_cast<float>(PosX), static_cast<float>(PosY)), /*bSelectNewNode=*/false);
	if (!GraphNode)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Schema refused to create node of class %s"), *ClassPath));
	}

	UFlowNode* Node = Cast<UFlowNode>(GraphNode->GetFlowNodeBase());
	if (!Node)
	{
		return FMonolithActionResult::Error(TEXT("Created graph node has no runtime UFlowNode"));
	}

	const TSharedPtr<FJsonObject>* PropsObj = nullptr;
	if (Params->TryGetObjectField(TEXT("properties"), PropsObj) && PropsObj && PropsObj->IsValid())
	{
		if (!ApplyPropertiesJson(Node, *PropsObj, Err))
		{
			return FMonolithActionResult::Error(Err);
		}
		// Rebuild pins so context-pin nodes reflect the fresh property state.
		GraphNode->ReconstructNode();
	}

	FA->MarkPackageDirty();

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), FA->GetPathName());
	Result->SetStringField(TEXT("node_guid"), GuidStrOf(Node));
	Result->SetStringField(TEXT("node_class"), Node->GetClass()->GetPathName());
	{
		TArray<TSharedPtr<FJsonValue>> InPins, OutPins;
		for (const FFlowPin& Pin : Node->GetInputPins())  InPins.Add(MakeShared<FJsonValueString>(Pin.PinName.ToString()));
		for (const FFlowPin& Pin : Node->GetOutputPins()) OutPins.Add(MakeShared<FJsonValueString>(Pin.PinName.ToString()));
		Result->SetArrayField(TEXT("input_pins"), InPins);
		Result->SetArrayField(TEXT("output_pins"), OutPins);
	}
	if (!MaybeSave(Params, FA, Result))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Node added but failed to save %s"), *FA->GetPathName()));
	}
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithFlowAuthorActions::SetNodeProperties(const TSharedPtr<FJsonObject>& Params)
{
	FString Err;
	UFlowAsset* FA = LoadFlowAssetParam(Params, Err);
	if (!FA) return FMonolithActionResult::Error(Err);

	FString NodeGuid;
	if (!Params->TryGetStringField(TEXT("node_guid"), NodeGuid) || NodeGuid.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("node_guid is required"));
	}
	const TSharedPtr<FJsonObject>* PropsObj = nullptr;
	if (!Params->TryGetObjectField(TEXT("properties"), PropsObj) || !PropsObj || !PropsObj->IsValid())
	{
		return FMonolithActionResult::Error(TEXT("properties object is required"));
	}

	UFlowNode* Node = FindRuntimeNode(FA, NodeGuid, Err);
	if (!Node) return FMonolithActionResult::Error(Err);
	UFlowGraphNode* GraphNode = GraphNodeOf(Node, Err);
	if (!GraphNode) return FMonolithActionResult::Error(Err);

	const FScopedTransaction Transaction(LOCTEXT("SetFlowNodeProperties", "Monolith: Set Flow Node Properties"));
	FA->Modify();

	if (!ApplyPropertiesJson(Node, *PropsObj, Err))
	{
		return FMonolithActionResult::Error(Err);
	}
	GraphNode->ReconstructNode();
	FA->HarvestNodeConnections();
	FA->MarkPackageDirty();

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), FA->GetPathName());
	Result->SetStringField(TEXT("node_guid"), GuidStrOf(Node));
	Result->SetNumberField(TEXT("applied_properties"), (*PropsObj)->Values.Num());
	if (!MaybeSave(Params, FA, Result))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Properties applied but failed to save %s"), *FA->GetPathName()));
	}
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithFlowAuthorActions::RemoveNode(const TSharedPtr<FJsonObject>& Params)
{
	FString Err;
	UFlowAsset* FA = LoadFlowAssetParam(Params, Err);
	if (!FA) return FMonolithActionResult::Error(Err);

	FString NodeGuid;
	if (!Params->TryGetStringField(TEXT("node_guid"), NodeGuid) || NodeGuid.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("node_guid is required"));
	}
	UFlowNode* Node = FindRuntimeNode(FA, NodeGuid, Err);
	if (!Node) return FMonolithActionResult::Error(Err);
	UFlowGraphNode* GraphNode = GraphNodeOf(Node, Err);
	if (!GraphNode) return FMonolithActionResult::Error(Err);

	UEdGraph* Graph = GraphNode->GetGraph();
	const UEdGraphSchema* Schema = Graph ? Graph->GetSchema() : nullptr;
	if (!Schema)
	{
		return FMonolithActionResult::Error(TEXT("Graph schema not available"));
	}

	const FScopedTransaction Transaction(LOCTEXT("RemoveFlowNode", "Monolith: Remove Flow Node"));
	FA->Modify();
	Graph->Modify();
	GraphNode->Modify();

	const FGuid RuntimeGuid = Node->GetGuid();
	Schema->BreakNodeLinks(*GraphNode);
	GraphNode->DestroyNode();
	// DestroyNode only removes the editor wrapper - the runtime node must be
	// unregistered explicitly, mirroring SFlowGraphEditor::DeleteSelectedNodes.
	FA->UnregisterNode(RuntimeGuid);
	FA->HarvestNodeConnections();
	FA->MarkPackageDirty();

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), FA->GetPathName());
	Result->SetStringField(TEXT("removed_node_guid"), NodeGuid);
	Result->SetNumberField(TEXT("node_count"), FA->GetNodes().Num());
	if (!MaybeSave(Params, FA, Result))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Node removed but failed to save %s"), *FA->GetPathName()));
	}
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithFlowAuthorActions::AddComment(const TSharedPtr<FJsonObject>& Params)
{
	FString Err;
	UFlowAsset* FA = LoadFlowAssetParam(Params, Err);
	if (!FA) return FMonolithActionResult::Error(Err);

	FString Text;
	if (!Params->TryGetStringField(TEXT("text"), Text))
	{
		return FMonolithActionResult::Error(TEXT("text is required"));
	}
	UEdGraph* Graph = FA->GetGraph();
	if (!Graph)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("%s has no editor graph"), *FA->GetPathName()));
	}

	double PosX = 0, PosY = 0, Width = 400, Height = 200;
	Params->TryGetNumberField(TEXT("pos_x"), PosX);
	Params->TryGetNumberField(TEXT("pos_y"), PosY);
	Params->TryGetNumberField(TEXT("width"), Width);
	Params->TryGetNumberField(TEXT("height"), Height);

	const FScopedTransaction Transaction(LOCTEXT("AddFlowComment", "Monolith: Add Flow Graph Comment"));
	Graph->Modify();

	UEdGraphNode_Comment* Comment = NewObject<UEdGraphNode_Comment>(Graph, NAME_None, RF_Transactional);
	Comment->NodeComment = Text;
	Comment->NodePosX = static_cast<int32>(PosX);
	Comment->NodePosY = static_cast<int32>(PosY);
	Comment->NodeWidth = static_cast<int32>(Width);
	Comment->NodeHeight = static_cast<int32>(Height);
	Comment->CreateNewGuid();
	Graph->AddNode(Comment, /*bUserAction=*/false, /*bSelectNewNode=*/false);
	FA->MarkPackageDirty();

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), FA->GetPathName());
	Result->SetStringField(TEXT("comment_guid"), Comment->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
	if (!MaybeSave(Params, FA, Result))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Comment added but failed to save %s"), *FA->GetPathName()));
	}
	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// Wiring
// ============================================================================

FMonolithActionResult FMonolithFlowAuthorActions::ConnectPins(const TSharedPtr<FJsonObject>& Params)
{
	FString Err;
	UFlowAsset* FA = LoadFlowAssetParam(Params, Err);
	if (!FA) return FMonolithActionResult::Error(Err);

	FString FromGuid, FromPinName, ToGuid, ToPinName;
	Params->TryGetStringField(TEXT("from_node_guid"), FromGuid);
	Params->TryGetStringField(TEXT("from_pin"), FromPinName);
	Params->TryGetStringField(TEXT("to_node_guid"), ToGuid);
	Params->TryGetStringField(TEXT("to_pin"), ToPinName);
	if (FromGuid.IsEmpty() || FromPinName.IsEmpty() || ToGuid.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("from_node_guid, from_pin and to_node_guid are required"));
	}
	if (ToPinName.IsEmpty()) ToPinName = TEXT("In");

	UFlowNode* FromNode = FindRuntimeNode(FA, FromGuid, Err);
	if (!FromNode) return FMonolithActionResult::Error(Err);
	UFlowNode* ToNode = FindRuntimeNode(FA, ToGuid, Err);
	if (!ToNode) return FMonolithActionResult::Error(Err);

	UFlowGraphNode* FromGraphNode = GraphNodeOf(FromNode, Err);
	if (!FromGraphNode) return FMonolithActionResult::Error(Err);
	UFlowGraphNode* ToGraphNode = GraphNodeOf(ToNode, Err);
	if (!ToGraphNode) return FMonolithActionResult::Error(Err);

	UEdGraphPin* FromPin = FromGraphNode->FindPin(FName(*FromPinName), EGPD_Output);
	if (!FromPin)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Output pin '%s' not found on node %s"), *FromPinName, *FromGuid));
	}
	UEdGraphPin* ToPin = ToGraphNode->FindPin(FName(*ToPinName), EGPD_Input);
	if (!ToPin)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Input pin '%s' not found on node %s"), *ToPinName, *ToGuid));
	}

	UEdGraph* Graph = FromGraphNode->GetGraph();
	const UEdGraphSchema* Schema = Graph ? Graph->GetSchema() : nullptr;
	if (!Schema)
	{
		return FMonolithActionResult::Error(TEXT("Graph schema not available"));
	}

	const FScopedTransaction Transaction(LOCTEXT("ConnectFlowPins", "Monolith: Connect Flow Pins"));
	FA->Modify();
	FromGraphNode->Modify();
	ToGraphNode->Modify();

	if (!Schema->TryCreateConnection(FromPin, ToPin))
	{
		const FPinConnectionResponse Response = Schema->CanCreateConnection(FromPin, ToPin);
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Schema refused connection %s.%s -> %s.%s: %s"),
			*FromGuid, *FromPinName, *ToGuid, *ToPinName, *Response.Message.ToString()));
	}

	FA->HarvestNodeConnections();
	FA->MarkPackageDirty();

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), FA->GetPathName());
	Result->SetStringField(TEXT("from"), FString::Printf(TEXT("%s.%s"), *FromGuid, *FromPinName));
	Result->SetStringField(TEXT("to"), FString::Printf(TEXT("%s.%s"), *ToGuid, *ToPinName));
	if (!MaybeSave(Params, FA, Result))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Connected but failed to save %s"), *FA->GetPathName()));
	}
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithFlowAuthorActions::DisconnectPins(const TSharedPtr<FJsonObject>& Params)
{
	FString Err;
	UFlowAsset* FA = LoadFlowAssetParam(Params, Err);
	if (!FA) return FMonolithActionResult::Error(Err);

	FString NodeGuid, PinName;
	Params->TryGetStringField(TEXT("node_guid"), NodeGuid);
	Params->TryGetStringField(TEXT("pin_name"), PinName);
	if (NodeGuid.IsEmpty() || PinName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("node_guid and pin_name are required"));
	}

	UFlowNode* Node = FindRuntimeNode(FA, NodeGuid, Err);
	if (!Node) return FMonolithActionResult::Error(Err);
	UFlowGraphNode* GraphNode = GraphNodeOf(Node, Err);
	if (!GraphNode) return FMonolithActionResult::Error(Err);

	UEdGraphPin* Pin = GraphNode->FindPin(FName(*PinName));
	if (!Pin)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Pin '%s' not found on node %s"), *PinName, *NodeGuid));
	}

	UEdGraph* Graph = GraphNode->GetGraph();
	const UEdGraphSchema* Schema = Graph ? Graph->GetSchema() : nullptr;
	if (!Schema)
	{
		return FMonolithActionResult::Error(TEXT("Graph schema not available"));
	}

	const int32 LinkCount = Pin->LinkedTo.Num();

	const FScopedTransaction Transaction(LOCTEXT("DisconnectFlowPins", "Monolith: Disconnect Flow Pins"));
	FA->Modify();
	GraphNode->Modify();

	Schema->BreakPinLinks(*Pin, /*bSendsNodeNotification=*/true);
	FA->HarvestNodeConnections();
	FA->MarkPackageDirty();

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), FA->GetPathName());
	Result->SetStringField(TEXT("pin"), FString::Printf(TEXT("%s.%s"), *NodeGuid, *PinName));
	Result->SetNumberField(TEXT("links_broken"), LinkCount);
	if (!MaybeSave(Params, FA, Result))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Disconnected but failed to save %s"), *FA->GetPathName()));
	}
	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// Layout
// ============================================================================

namespace
{
	/** Rough pixel height of a Flow node from its pin rows. */
	int32 EstimateNodeHeight(const UFlowGraphNode* GraphNode)
	{
		int32 InPins = 0, OutPins = 0;
		for (const UEdGraphPin* Pin : GraphNode->Pins)
		{
			if (Pin->Direction == EGPD_Input) ++InPins; else ++OutPins;
		}
		return 96 + 24 * FMath::Max(0, FMath::Max(InPins, OutPins) - 2);
	}

	struct FFlowLayoutGraph
	{
		TArray<UFlowGraphNode*> Nodes;
		TMap<UFlowGraphNode*, int32> IndexOf;
		TArray<TArray<int32>> Out;   // successor indices
		TArray<TArray<int32>> In;    // predecessor indices

		void Build(UEdGraph* Graph)
		{
			for (UEdGraphNode* EdNode : Graph->Nodes)
			{
				if (UFlowGraphNode* FlowNode = Cast<UFlowGraphNode>(EdNode))
				{
					IndexOf.Add(FlowNode, Nodes.Num());
					Nodes.Add(FlowNode);
				}
			}
			Out.SetNum(Nodes.Num());
			In.SetNum(Nodes.Num());
			for (int32 i = 0; i < Nodes.Num(); ++i)
			{
				for (const UEdGraphPin* Pin : Nodes[i]->Pins)
				{
					if (Pin->Direction != EGPD_Output) continue;
					for (const UEdGraphPin* Linked : Pin->LinkedTo)
					{
						UFlowGraphNode* Target = Linked ? Cast<UFlowGraphNode>(Linked->GetOwningNode()) : nullptr;
						const int32* TargetIdx = Target ? IndexOf.Find(Target) : nullptr;
						if (TargetIdx)
						{
							Out[i].AddUnique(*TargetIdx);
							In[*TargetIdx].AddUnique(i);
						}
					}
				}
			}
		}
	};

	/** Full layered relayout. Returns number of nodes moved. */
	int32 LayoutAll(UEdGraph* Graph, int32 HSpacing, int32 VSpacing)
	{
		FFlowLayoutGraph G;
		G.Build(Graph);
		const int32 N = G.Nodes.Num();
		if (N == 0) return 0;

		// Longest-path layering via bounded relaxation (cycle-safe: N passes max).
		TArray<int32> Layer;
		Layer.Init(0, N);
		for (int32 Pass = 0; Pass < N; ++Pass)
		{
			bool bChanged = false;
			for (int32 i = 0; i < N; ++i)
			{
				for (int32 Succ : G.Out[i])
				{
					if (Layer[Succ] < Layer[i] + 1 && Layer[Succ] < N)
					{
						Layer[Succ] = Layer[i] + 1;
						bChanged = true;
					}
				}
			}
			if (!bChanged) break;
		}

		// Bucket by layer, initial order = current Y (keeps manual grouping stable-ish).
		const int32 MaxLayer = FMath::Max(0, *Algo::MaxElement(Layer));
		TArray<TArray<int32>> Buckets;
		Buckets.SetNum(MaxLayer + 1);
		for (int32 i = 0; i < N; ++i) Buckets[Layer[i]].Add(i);
		for (TArray<int32>& Bucket : Buckets)
		{
			Bucket.Sort([&G](int32 A, int32 B) { return G.Nodes[A]->NodePosY < G.Nodes[B]->NodePosY; });
		}

		// Two barycenter sweeps (order each layer by mean predecessor order).
		TArray<int32> OrderInLayer;
		OrderInLayer.Init(0, N);
		for (int32 Sweep = 0; Sweep < 2; ++Sweep)
		{
			for (int32 L = 0; L <= MaxLayer; ++L)
			{
				for (int32 Pos = 0; Pos < Buckets[L].Num(); ++Pos) OrderInLayer[Buckets[L][Pos]] = Pos;
			}
			for (int32 L = 1; L <= MaxLayer; ++L)
			{
				Buckets[L].StableSort([&](int32 A, int32 B)
				{
					auto Barycenter = [&](int32 NodeIdx) -> float
					{
						if (G.In[NodeIdx].Num() == 0) return static_cast<float>(OrderInLayer[NodeIdx]);
						float Sum = 0.f;
						for (int32 Pred : G.In[NodeIdx]) Sum += OrderInLayer[Pred];
						return Sum / G.In[NodeIdx].Num();
					};
					return Barycenter(A) < Barycenter(B);
				});
			}
		}

		// Assign positions.
		int32 Moved = 0;
		for (int32 L = 0; L <= MaxLayer; ++L)
		{
			int32 Y = 0;
			for (int32 NodeIdx : Buckets[L])
			{
				UFlowGraphNode* Node = G.Nodes[NodeIdx];
				const int32 NewX = L * HSpacing;
				const int32 NewY = Y;
				if (Node->NodePosX != NewX || Node->NodePosY != NewY)
				{
					Node->Modify();
					Node->NodePosX = NewX;
					Node->NodePosY = NewY;
					++Moved;
				}
				Y += EstimateNodeHeight(Node) + VSpacing;
			}
		}
		return Moved;
	}

	/** Place each target node next to its first connected neighbour (fallback: below the graph). */
	int32 LayoutRelative(UEdGraph* Graph, const TSet<UFlowGraphNode*>& Targets, int32 HSpacing, int32 VSpacing)
	{
		// Occupied slots of untouched nodes, for collision nudging.
		TArray<FIntPoint> Occupied;
		int32 GraphMinX = 0, GraphMaxY = 0;
		for (UEdGraphNode* EdNode : Graph->Nodes)
		{
			if (UFlowGraphNode* FlowNode = Cast<UFlowGraphNode>(EdNode))
			{
				if (!Targets.Contains(FlowNode))
				{
					Occupied.Add(FIntPoint(FlowNode->NodePosX, FlowNode->NodePosY));
					GraphMinX = FMath::Min(GraphMinX, FlowNode->NodePosX);
					GraphMaxY = FMath::Max(GraphMaxY, FlowNode->NodePosY);
				}
			}
		}

		auto NudgeFree = [&Occupied](int32 X, int32 Y) -> FIntPoint
		{
			bool bCollides = true;
			while (bCollides)
			{
				bCollides = false;
				for (const FIntPoint& Slot : Occupied)
				{
					if (FMath::Abs(Slot.X - X) < 240 && FMath::Abs(Slot.Y - Y) < 90)
					{
						Y = Slot.Y + 120;
						bCollides = true;
						break;
					}
				}
			}
			return FIntPoint(X, Y);
		};

		int32 Moved = 0;
		int32 FallbackY = GraphMaxY + 300;
		for (UFlowGraphNode* Node : Targets)
		{
			// Prefer sitting right of a connected predecessor; else left of a successor.
			UFlowGraphNode* Anchor = nullptr;
			bool bAnchorIsPredecessor = true;
			for (const UEdGraphPin* Pin : Node->Pins)
			{
				for (const UEdGraphPin* Linked : Pin->LinkedTo)
				{
					UFlowGraphNode* Other = Linked ? Cast<UFlowGraphNode>(Linked->GetOwningNode()) : nullptr;
					if (Other && !Targets.Contains(Other))
					{
						Anchor = Other;
						bAnchorIsPredecessor = (Pin->Direction == EGPD_Input);
						break;
					}
				}
				if (Anchor) break;
			}

			FIntPoint NewPos;
			if (Anchor)
			{
				const int32 X = Anchor->NodePosX + (bAnchorIsPredecessor ? HSpacing : -HSpacing);
				NewPos = NudgeFree(X, Anchor->NodePosY);
			}
			else
			{
				NewPos = NudgeFree(GraphMinX, FallbackY);
				FallbackY = NewPos.Y + 160;
			}

			if (Node->NodePosX != NewPos.X || Node->NodePosY != NewPos.Y)
			{
				Node->Modify();
				Node->NodePosX = NewPos.X;
				Node->NodePosY = NewPos.Y;
				++Moved;
			}
			Occupied.Add(NewPos);
		}
		return Moved;
	}
}

FMonolithActionResult FMonolithFlowAuthorActions::AutoLayout(const TSharedPtr<FJsonObject>& Params)
{
	FString Err;
	UFlowAsset* FA = LoadFlowAssetParam(Params, Err);
	if (!FA) return FMonolithActionResult::Error(Err);

	UEdGraph* Graph = FA->GetGraph();
	if (!Graph)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("%s has no editor graph"), *FA->GetPathName()));
	}

	FString LayoutMode = TEXT("all");
	Params->TryGetStringField(TEXT("layout_mode"), LayoutMode);
	FString Formatter = TEXT("monolith");
	Params->TryGetStringField(TEXT("formatter"), Formatter);
	double HSpacing = 420, VSpacing = 60;
	Params->TryGetNumberField(TEXT("horizontal_spacing"), HSpacing);
	Params->TryGetNumberField(TEXT("vertical_spacing"), VSpacing);

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), FA->GetPathName());

	// External formatter path (Blueprint Assist via Monolith's modular feature).
	if (Formatter == TEXT("blueprint_assist") || Formatter == TEXT("auto"))
	{
		const bool bAvailable = IMonolithGraphFormatter::IsAvailable()
			&& IMonolithGraphFormatter::Get().SupportsGraph(Graph);
		if (bAvailable)
		{
			int32 NodesFormatted = 0;
			FString FormatError;
			const FScopedTransaction Transaction(LOCTEXT("AutoLayoutFlowBA", "Monolith: Auto-Layout Flow Graph (BA)"));
			if (IMonolithGraphFormatter::Get().FormatGraph(Graph, NodesFormatted, FormatError))
			{
				FA->MarkPackageDirty();
				Result->SetStringField(TEXT("formatter"), TEXT("blueprint_assist"));
				Result->SetNumberField(TEXT("nodes_moved"), NodesFormatted);
				if (!MaybeSave(Params, FA, Result))
				{
					return FMonolithActionResult::Error(FString::Printf(TEXT("Laid out but failed to save %s"), *FA->GetPathName()));
				}
				return FMonolithActionResult::Success(Result);
			}
			if (Formatter == TEXT("blueprint_assist"))
			{
				return FMonolithActionResult::Error(FString::Printf(
					TEXT("Blueprint Assist formatter failed: %s (the asset must be open in an editor tab)"), *FormatError));
			}
			// 'auto' falls through to the built-in formatter.
		}
		else if (Formatter == TEXT("blueprint_assist"))
		{
			return FMonolithActionResult::Error(TEXT("Blueprint Assist formatter not available for this graph"));
		}
	}

	const FScopedTransaction Transaction(LOCTEXT("AutoLayoutFlow", "Monolith: Auto-Layout Flow Graph"));
	Graph->Modify();

	int32 Moved = 0;
	if (LayoutMode == TEXT("all"))
	{
		Moved = LayoutAll(Graph, static_cast<int32>(HSpacing), static_cast<int32>(VSpacing));
	}
	else if (LayoutMode == TEXT("new_only") || LayoutMode == TEXT("selected"))
	{
		TSet<UFlowGraphNode*> Targets;
		if (LayoutMode == TEXT("new_only"))
		{
			for (UEdGraphNode* EdNode : Graph->Nodes)
			{
				if (UFlowGraphNode* FlowNode = Cast<UFlowGraphNode>(EdNode))
				{
					if (FlowNode->NodePosX == 0 && FlowNode->NodePosY == 0)
					{
						Targets.Add(FlowNode);
					}
				}
			}
		}
		else
		{
			const TArray<TSharedPtr<FJsonValue>>* NodeIds = nullptr;
			if (!Params->TryGetArrayField(TEXT("node_ids"), NodeIds) || !NodeIds || NodeIds->Num() == 0)
			{
				return FMonolithActionResult::Error(TEXT("layout_mode 'selected' requires a non-empty node_ids array"));
			}
			for (const TSharedPtr<FJsonValue>& Value : *NodeIds)
			{
				FString GuidStr;
				if (!Value.IsValid() || !Value->TryGetString(GuidStr)) continue;
				if (UFlowNode* Node = FindRuntimeNode(FA, GuidStr, Err))
				{
					if (UFlowGraphNode* GraphNode = Cast<UFlowGraphNode>(Node->GetGraphNode()))
					{
						Targets.Add(GraphNode);
					}
				}
			}
			if (Targets.Num() == 0)
			{
				return FMonolithActionResult::Error(TEXT("None of node_ids resolved to graph nodes"));
			}
		}
		Moved = LayoutRelative(Graph, Targets, static_cast<int32>(HSpacing), static_cast<int32>(VSpacing));
	}
	else
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Unknown layout_mode '%s' (all | new_only | selected)"), *LayoutMode));
	}

	if (Moved > 0)
	{
		FA->MarkPackageDirty();
		Graph->NotifyGraphChanged();
	}

	Result->SetStringField(TEXT("formatter"), TEXT("monolith"));
	Result->SetStringField(TEXT("layout_mode"), LayoutMode);
	Result->SetNumberField(TEXT("nodes_moved"), Moved);
	if (!MaybeSave(Params, FA, Result))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Laid out but failed to save %s"), *FA->GetPathName()));
	}
	return FMonolithActionResult::Success(Result);
}

#undef LOCTEXT_NAMESPACE

#endif // WITH_FLOW
