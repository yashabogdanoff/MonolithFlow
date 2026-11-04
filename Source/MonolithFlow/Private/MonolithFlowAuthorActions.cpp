#include "MonolithFlowAuthorActions.h"

#if WITH_FLOW

#include "MonolithParamSchema.h"
#include "MonolithAssetUtils.h"
#include "IMonolithGraphFormatter.h"
#include "MonolithFlowNodeKey.h"

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
#include "Editor/Transactor.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "EditorAssetLibrary.h"
#include "EditorValidatorSubsystem.h"
#include "Misc/DataValidation.h"
#include "JsonObjectConverter.h"
#include "Misc/OutputDeviceNull.h"
#include "Misc/PackageName.h"
#include "UObject/StructOnScope.h"
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
	 * Expected JSON shape for a property, in the form get_node_info's `data` snapshot
	 * writes it, or null when the value is acceptable. FJsonObjectConverter does not
	 * reject a mismatch - an object handed to a bool becomes false and the call
	 * reports success - so the shape is checked up front.
	 */
	const TCHAR* JsonShapeMismatch(const FProperty* Prop, const TSharedPtr<FJsonValue>& Value)
	{
		const EJson Type = Value.IsValid() ? Value->Type : EJson::None;
		auto Expect = [Type](bool bOk, const TCHAR* Expected) -> const TCHAR* { return bOk ? nullptr : Expected; };

		if (Prop->ArrayDim > 1)                         return Expect(Type == EJson::Array, TEXT("array"));
		if (CastField<FBoolProperty>(Prop))             return Expect(Type == EJson::Boolean, TEXT("boolean"));
		if (const FNumericProperty* Num = CastField<FNumericProperty>(Prop))
		{
			return Num->IsEnum()
				? Expect(Type == EJson::String || Type == EJson::Number, TEXT("enum name or number"))
				: Expect(Type == EJson::Number, TEXT("number"));
		}
		if (CastField<FEnumProperty>(Prop))             return Expect(Type == EJson::String || Type == EJson::Number, TEXT("enum name or number"));
		if (CastField<FStrProperty>(Prop) || CastField<FNameProperty>(Prop) || CastField<FTextProperty>(Prop))
		{
			return Expect(Type == EJson::String, TEXT("string"));
		}
		if (const FObjectPropertyBase* ObjectProp = CastField<FObjectPropertyBase>(Prop))
		{
			// An instanced subobject is written inline into the snapshot (an object with
			// _ClassName), so the same shape has to be accepted on the way back in.
			const bool bInstanced = Prop->HasAnyPropertyFlags(CPF_InstancedReference | CPF_PersistentInstance)
				|| (ObjectProp->PropertyClass && ObjectProp->PropertyClass->HasAnyClassFlags(CLASS_DefaultToInstanced | CLASS_EditInlineNew));
			return bInstanced
				? Expect(Type == EJson::String || Type == EJson::Null || Type == EJson::Object, TEXT("object path string, inline object or null"))
				: Expect(Type == EJson::String || Type == EJson::Null, TEXT("object path string or null"));
		}
		if (CastField<FInterfaceProperty>(Prop))
		{
			return Expect(Type == EJson::String || Type == EJson::Null, TEXT("object path string or null"));
		}
		if (CastField<FArrayProperty>(Prop) || CastField<FSetProperty>(Prop)) return Expect(Type == EJson::Array, TEXT("array"));
		if (CastField<FMapProperty>(Prop))              return Expect(Type == EJson::Object, TEXT("object"));
		if (const FStructProperty* StructProp = CastField<FStructProperty>(Prop))
		{
			if (Type == EJson::String)
			{
				// The converter hands a string to ImportText and ignores a parse
				// failure, so try the parse here on a scratch value.
				FStructOnScope Scratch(StructProp->Struct);
				FOutputDeviceNull Quiet;
				const bool bParses = StructProp->ImportText_Direct(
					*Value->AsString(), Scratch.GetStructMemory(), nullptr, PPF_None, &Quiet) != nullptr;
				return Expect(bParses, TEXT("object, or export text that parses, e.g. (R=255,G=0,B=0,A=255)"));
			}
			return Expect(Type == EJson::Object, TEXT("object or export-text string"));
		}
		return nullptr;
	}

	/**
	 * Check the top-level keys of a property JSON against a class before anything
	 * is touched: unknown keys are typos; only properties the Details panel could
	 * edit are accepted - the rest (NodeGuid, pin arrays, internal state) would
	 * corrupt the node; and each value must have the JSON shape of its property.
	 * Nested struct members are left to the converter.
	 */
	/** A failed edit: a stable code for callers that branch on it, and text for people. */
	struct FEditError
	{
		FString Code;
		FString Message;
		int32 OpIndex = INDEX_NONE;
	};

	bool ValidatePropertyKeys(const UClass* Cls, const TSharedPtr<FJsonObject>& Props, FEditError& OutError)
	{
		TArray<FString> Unknown, ReadOnly, WrongShape;
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Props->Values)
		{
			const FProperty* Prop = Cls->FindPropertyByName(FName(*Pair.Key));
			if (!Prop)
			{
				Unknown.Add(Pair.Key);
			}
			else if (!Prop->HasAnyPropertyFlags(CPF_Edit) || Prop->HasAnyPropertyFlags(CPF_EditConst))
			{
				ReadOnly.Add(Pair.Key);
			}
			else if (const TCHAR* Expected = JsonShapeMismatch(Prop, Pair.Value))
			{
				WrongShape.Add(FString::Printf(TEXT("%s (expects %s)"), *Pair.Key, Expected));
			}
		}
		if (Unknown.Num() > 0)
		{
			OutError = { TEXT("unknown_property"), FString::Printf(TEXT("Unknown properties on %s: %s"),
				*Cls->GetName(), *FString::Join(Unknown, TEXT(", "))) };
			return false;
		}
		if (ReadOnly.Num() > 0)
		{
			OutError = { TEXT("read_only_property"), FString::Printf(TEXT("Properties of %s that are not editable in the Details panel: %s"),
				*Cls->GetName(), *FString::Join(ReadOnly, TEXT(", "))) };
			return false;
		}
		if (WrongShape.Num() > 0)
		{
			OutError = { TEXT("wrong_type"), FString::Printf(TEXT("Values of the wrong JSON type for %s: %s"),
				*Cls->GetName(), *FString::Join(WrongShape, TEXT(", "))) };
			return false;
		}
		return true;
	}

	/**
	 * Apply a JSON object onto a node's UPROPERTYs, keys already checked by
	 * ValidatePropertyKeys. Each property gets its own Pre/PostEditChange, the same
	 * notification an edit in the Details panel sends, so nodes that rebuild pins
	 * or caches on a specific property see it.
	 */
	bool ApplyPropertiesJson(UObject* Target, const TSharedPtr<FJsonObject>& Props, FEditError& OutError)
	{
		TArray<FProperty*> Changed;
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Props->Values)
		{
			Changed.Add(Target->GetClass()->FindPropertyByName(FName(*Pair.Key)));
		}

		Target->Modify();
		for (FProperty* Prop : Changed)
		{
			Target->PreEditChange(Prop);
		}
		const bool bApplied = FJsonObjectConverter::JsonObjectToUStruct(
			Props.ToSharedRef(), Target->GetClass(), Target, /*CheckFlags=*/0, /*SkipFlags=*/0);
		for (FProperty* Prop : Changed)
		{
			FPropertyChangedEvent Event(Prop, EPropertyChangeType::ValueSet);
			Target->PostEditChangeProperty(Event);
		}

		if (!bApplied)
		{
			OutError = { TEXT("apply_failed"), FString::Printf(TEXT("Failed to apply property JSON to %s (a value does not match its property type)"),
				*Target->GetClass()->GetName()) };
			return false;
		}
		return true;
	}

	/**
	 * Marks the head of the undo buffer before a mutation, so one that fails
	 * halfway can be reverted for real: FScopedTransaction::Cancel only drops the
	 * undo record and keeps the edits. RollBack undoes the newest transaction only
	 * when it was opened after the mark, never an older edit of the user's.
	 */
	struct FUndoMark
	{
		FGuid HeadBefore;

		FUndoMark()
		{
			if (GEditor && GEditor->Trans)
			{
				HeadBefore = GEditor->Trans->GetUndoContext(/*bCheckWhetherUndoPossible=*/false).TransactionId;
			}
		}

		bool RollBack() const
		{
			if (!GEditor || !GEditor->Trans)
			{
				return false;
			}
			const FGuid Head = GEditor->Trans->GetUndoContext(/*bCheckWhetherUndoPossible=*/false).TransactionId;
			if (!Head.IsValid() || Head == HeadBefore)
			{
				return false;
			}
			return GEditor->UndoTransaction(/*bCanRedo=*/false);
		}
	};

	/** "<node guid>.<pin name>" for a pin on a Flow graph node. */
	FString DescribePin(const UEdGraphPin* Pin)
	{
		const UFlowGraphNode* Owner = Pin ? Cast<UFlowGraphNode>(Pin->GetOwningNode()) : nullptr;
		const UFlowNode* RuntimeNode = Owner ? Cast<UFlowNode>(Owner->GetFlowNodeBase()) : nullptr;
		const FString Guid = RuntimeNode ? MonolithFlow::NodeKeyString(RuntimeNode) : TEXT("?");
		return FString::Printf(TEXT("%s.%s"), *Guid, Pin ? *Pin->PinName.ToString() : TEXT("?"));
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
		return MonolithFlow::NodeKeyString(Node);
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
		TEXT("Save one Flow Asset to disk. Saving also refreshes the flow_* index tables via the live incremental indexer, so read actions see the change. Data validation that the engine or Flow 2.4 runs on save only writes to the editor log and never blocks the save - call validate_asset to get the errors."),
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
		TEXT("Add a node of the given UFlowNode subclass to a Flow Asset at (pos_x, pos_y). Optional `properties` JSON is applied onto the fresh node's UPROPERTYs (same shape as the `data` snapshot from get_node_info) and its pins are rebuilt - so context-pin nodes (e.g. tag-driven switches) come up with correct pins. Returns the new node_guid. Mutates the LIVE asset; pass save=true (or call save_asset later) to persist and refresh the index."),
		FMonolithActionHandler::CreateStatic(&FMonolithFlowAuthorActions::AddNode),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Full UFlowAsset object path"))
			.Required(TEXT("node_class"), TEXT("string"), TEXT("UFlowNode subclass path, e.g. \"/Script/Flow.FlowNode_Log\""))
			.Optional(TEXT("pos_x"), TEXT("integer"), TEXT("Graph X position (default 0)"))
			.Optional(TEXT("pos_y"), TEXT("integer"), TEXT("Graph Y position (default 0)"))
			.Optional(TEXT("properties"), TEXT("object"), TEXT("Property JSON to apply onto the new node"))
			.Optional(TEXT("save"), TEXT("boolean"), TEXT("Save the asset after the edit (default false)"))
			.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("Apply, report the result, then undo - nothing is kept (default false)"))
			.Build());

	Registry.RegisterAction(TEXT("flow"), TEXT("set_node_properties"),
		TEXT("Apply a property JSON onto an existing node's UPROPERTYs and rebuild its pins (ReconstructNode - refreshes context pins). Unknown property names are rejected explicitly. Mutates the LIVE asset; save to persist."),
		FMonolithActionHandler::CreateStatic(&FMonolithFlowAuthorActions::SetNodeProperties),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Full UFlowAsset object path"))
			.Required(TEXT("node_guid"), TEXT("string"), TEXT("Guid of the node to edit"))
			.Required(TEXT("properties"), TEXT("object"), TEXT("Property JSON to apply"))
			.Optional(TEXT("save"), TEXT("boolean"), TEXT("Save the asset after the edit (default false)"))
			.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("Apply, report the result, then undo - nothing is kept (default false)"))
			.Build());

	Registry.RegisterAction(TEXT("flow"), TEXT("remove_node"),
		TEXT("Remove a node from a Flow Asset: breaks its links, destroys the editor wrapper and unregisters the runtime node. Mutates the LIVE asset; save to persist."),
		FMonolithActionHandler::CreateStatic(&FMonolithFlowAuthorActions::RemoveNode),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Full UFlowAsset object path"))
			.Required(TEXT("node_guid"), TEXT("string"), TEXT("Guid of the node to remove"))
			.Optional(TEXT("save"), TEXT("boolean"), TEXT("Save the asset after the edit (default false)"))
			.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("Apply, report the result, then undo - nothing is kept (default false)"))
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
			.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("Apply, report the result, then undo - nothing is kept (default false)"))
			.Build());

	Registry.RegisterAction(TEXT("flow"), TEXT("connect_pins"),
		TEXT("Connect an output pin of one node to an input pin of another (schema-mediated - the same validation as manual wiring), then re-harvest runtime connections. to_pin defaults to \"In\". Mutates the LIVE asset; save to persist."),
		FMonolithActionHandler::CreateStatic(&FMonolithFlowAuthorActions::ConnectPins),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Full UFlowAsset object path"))
			.Required(TEXT("from_node_guid"), TEXT("string"), TEXT("Source node guid"))
			.Required(TEXT("from_pin"), TEXT("string"), TEXT("Source OUTPUT pin name (e.g. \"Out\", \"Completed\", \"Step\")"))
			.Required(TEXT("to_node_guid"), TEXT("string"), TEXT("Target node guid"))
			.Optional(TEXT("to_pin"), TEXT("string"), TEXT("Target INPUT pin name (default \"In\")"))
			.Optional(TEXT("save"), TEXT("boolean"), TEXT("Save the asset after the edit (default false)"))
			.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("Apply, report the result, then undo - nothing is kept (default false)"))
			.Build());

	Registry.RegisterAction(TEXT("flow"), TEXT("disconnect_pins"),
		TEXT("Break every link of one pin on a node (either direction), then re-harvest runtime connections. Mutates the LIVE asset; save to persist."),
		FMonolithActionHandler::CreateStatic(&FMonolithFlowAuthorActions::DisconnectPins),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Full UFlowAsset object path"))
			.Required(TEXT("node_guid"), TEXT("string"), TEXT("Node guid"))
			.Required(TEXT("pin_name"), TEXT("string"), TEXT("Pin whose links to break"))
			.Optional(TEXT("save"), TEXT("boolean"), TEXT("Save the asset after the edit (default false)"))
			.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("Apply, report the result, then undo - nothing is kept (default false)"))
			.Build());

	Registry.RegisterAction(TEXT("flow"), TEXT("apply_batch"),
		TEXT("Several edits to one Flow Asset in one transaction - all of them apply or none does. `operations` is an array of objects, each with `op` (add_node, set_node_properties, remove_node, add_comment, connect_pins, disconnect_pins) and that action's own params minus asset_path. add_node takes an optional `alias`; later operations refer to the new node as \"$alias\" wherever a node guid goes. Every operation is validated before anything changes; a failure while applying undoes the whole batch. Errors carry `code` and `op_index` in the error data. Returns per-operation results, the alias -> guid map and every link the schema broke to make room."),
		FMonolithActionHandler::CreateStatic(&FMonolithFlowAuthorActions::ApplyBatch),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("UFlowAsset object or package path"))
			.Required(TEXT("operations"), TEXT("array"), TEXT("Operations to apply in order, e.g. [{\"op\":\"add_node\",\"alias\":\"log\",\"node_class\":\"/Script/Flow.FlowNode_Log\"},{\"op\":\"connect_pins\",\"from_node_guid\":\"<start guid>\",\"from_pin\":\"Out\",\"to_node_guid\":\"$log\"}]"))
			.Optional(TEXT("save"), TEXT("boolean"), TEXT("Save the asset after the batch (default false)"))
			.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("Apply, report the result, then undo - nothing is kept (default false)"))
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
		const FString ObjectName = PackagePath.Mid(DotIdx + 1);
		PackagePath.LeftInline(DotIdx);
		if (ObjectName != FPackageName::GetShortName(PackagePath))
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("'%s': the object name after the dot must repeat the package name (/Pkg/Name.Name)"), *AssetPath));
		}
	}
	// Checked before the factory runs: AssetTools reports a bad path with a log error or a dialog instead of failing quietly.
	FText Reason;
	if (!FPackageName::IsValidLongPackageName(PackagePath, /*bIncludeReadOnlyRoots=*/false, &Reason))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid asset path '%s': %s"), *AssetPath, *Reason.ToString()));
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
	// Flow 2.3 initializes the editing policies (preload, pin connection) only
	// when a graph is loaded, not when the factory creates one; a SubGraph node
	// added to the fresh asset then hits a check on the missing preload policy.
	// Flow 2.4 does it in CreateGraph; calling it again is harmless.
	FA->SetupForEditing();

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
// Graph edits
//
// Every edit action - the single ones and apply_batch - goes through the same
// path: each operation is validated up front without touching the asset, then
// all of them are applied inside one transaction. Any failure while applying
// undoes the whole transaction; dry_run applies, reports and undoes. Errors
// carry a stable `code` (and `op_index` in a batch) in the error data.
// ============================================================================

namespace
{
	FMonolithActionResult Fail(const FEditError& Error, const TCHAR* RollbackState = nullptr)
	{
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("code"), Error.Code);
		if (Error.OpIndex != INDEX_NONE)
		{
			Data->SetNumberField(TEXT("op_index"), Error.OpIndex);
		}
		if (RollbackState)
		{
			Data->SetStringField(TEXT("rollback"), RollbackState);
		}
		FString Message = Error.OpIndex != INDEX_NONE
			? FString::Printf(TEXT("Operation %d: %s"), Error.OpIndex, *Error.Message)
			: Error.Message;
		if (RollbackState && FCString::Strcmp(RollbackState, TEXT("done")) == 0)
		{
			Message += TEXT(" - the graph was left unchanged");
		}
		else if (RollbackState)
		{
			Message += TEXT(" - the change could NOT be rolled back, check the graph (editor Undo may revert it)");
		}
		return FMonolithActionResult::Error(Message).WithErrorData(Data);
	}

	/** State shared by the operations of one edit run. */
	struct FEditRun
	{
		UFlowAsset* Asset = nullptr;
		/** Alias (without the leading $) -> key of the node created under it. */
		TMap<FString, FGuid> Aliases;
		/** Links the schema broke to make room for new ones. */
		TArray<TSharedPtr<FJsonValue>> BrokenLinks;
	};

	/** What validation knows before anything is applied. */
	struct FEditPlan
	{
		UFlowAsset* Asset = nullptr;
		/** Alias -> class of the node it will create. */
		TMap<FString, UClass*> AliasClasses;
		/** Existing nodes removed by an earlier operation of the same run. */
		TSet<FGuid> Removed;
	};

	bool IsAliasRef(const FString& Ref)
	{
		return Ref.StartsWith(TEXT("$"));
	}

	/** Node reference: "<guid>" or "$alias". Validation-time: resolves to the node's class. */
	UClass* PlanNodeRef(const FEditPlan& Plan, const FString& Ref, FEditError& Out)
	{
		if (Ref.IsEmpty())
		{
			Out = { TEXT("invalid_params"), TEXT("node reference is empty") };
			return nullptr;
		}
		if (IsAliasRef(Ref))
		{
			if (UClass* const* Cls = Plan.AliasClasses.Find(Ref.Mid(1)))
			{
				return *Cls;
			}
			Out = { TEXT("unknown_alias"), FString::Printf(TEXT("%s is not created by an earlier operation"), *Ref) };
			return nullptr;
		}
		FGuid Guid;
		if (!FGuid::Parse(Ref, Guid))
		{
			Out = { TEXT("invalid_params"), FString::Printf(TEXT("Invalid node guid: %s"), *Ref) };
			return nullptr;
		}
		UFlowNode* Node = Plan.Asset->GetNodes().FindRef(Guid);
		if (!Node || Plan.Removed.Contains(Guid))
		{
			Out = { TEXT("node_not_found"), FString::Printf(TEXT("Node %s not found in %s"), *Ref, *Plan.Asset->GetPathName()) };
			return nullptr;
		}
		return Node->GetClass();
	}

	/** Node reference at apply time. */
	UFlowNode* ResolveNodeRef(const FEditRun& Run, const FString& Ref, FEditError& Out)
	{
		FGuid Guid;
		if (IsAliasRef(Ref))
		{
			if (const FGuid* Key = Run.Aliases.Find(Ref.Mid(1)))
			{
				Guid = *Key;
			}
		}
		else
		{
			FGuid::Parse(Ref, Guid);
		}
		UFlowNode* Node = Guid.IsValid() ? Run.Asset->GetNodes().FindRef(Guid) : nullptr;
		if (!Node)
		{
			Out = { TEXT("node_not_found"), FString::Printf(TEXT("Node %s not found in %s"), *Ref, *Run.Asset->GetPathName()) };
		}
		return Node;
	}

	UFlowGraphNode* GraphNodeOrError(UFlowNode* Node, FEditError& Out)
	{
		FString Err;
		UFlowGraphNode* GraphNode = GraphNodeOf(Node, Err);
		if (!GraphNode)
		{
			Out = { TEXT("no_editor_graph"), Err };
		}
		return GraphNode;
	}

	FString StringField(const TSharedPtr<FJsonObject>& Op, const TCHAR* Field)
	{
		FString Value;
		Op->TryGetStringField(Field, Value);
		return Value;
	}

	bool Require(const TSharedPtr<FJsonObject>& Op, std::initializer_list<const TCHAR*> Fields, FEditError& Out)
	{
		TArray<FString> Missing;
		for (const TCHAR* Field : Fields)
		{
			if (!Op->HasField(Field))
			{
				Missing.Add(Field);
			}
		}
		if (Missing.Num() > 0)
		{
			Out = { TEXT("invalid_params"), FString::Printf(TEXT("Missing: %s"), *FString::Join(Missing, TEXT(", "))) };
			return false;
		}
		return true;
	}

	const TSharedPtr<FJsonObject>* PropertiesOf(const TSharedPtr<FJsonObject>& Op)
	{
		const TSharedPtr<FJsonObject>* Props = nullptr;
		return Op->TryGetObjectField(TEXT("properties"), Props) && Props && Props->IsValid() ? Props : nullptr;
	}

	// --- add_node -----------------------------------------------------------

	bool PlanAddNode(FEditPlan& Plan, const TSharedPtr<FJsonObject>& Op, FEditError& Out)
	{
		if (!Require(Op, { TEXT("node_class") }, Out)) return false;
		FString Err;
		UClass* NodeClass = ResolveFlowNodeClass(StringField(Op, TEXT("node_class")), Err);
		if (!NodeClass)
		{
			Out = { TEXT("class_not_found"), Err };
			return false;
		}
		if (const TSharedPtr<FJsonObject>* Props = PropertiesOf(Op))
		{
			if (!ValidatePropertyKeys(NodeClass, *Props, Out)) return false;
		}
		const FString Alias = StringField(Op, TEXT("alias"));
		if (!Alias.IsEmpty())
		{
			if (Plan.AliasClasses.Contains(Alias))
			{
				Out = { TEXT("duplicate_alias"), FString::Printf(TEXT("Alias %s is used twice"), *Alias) };
				return false;
			}
			Plan.AliasClasses.Add(Alias, NodeClass);
		}
		return true;
	}

	bool ApplyAddNode(FEditRun& Run, const TSharedPtr<FJsonObject>& Op, TSharedPtr<FJsonObject>& Result, FEditError& Out)
	{
		FString Err;
		UClass* NodeClass = ResolveFlowNodeClass(StringField(Op, TEXT("node_class")), Err);
		double PosX = 0, PosY = 0;
		Op->TryGetNumberField(TEXT("pos_x"), PosX);
		Op->TryGetNumberField(TEXT("pos_y"), PosY);

		UEdGraph* Graph = Run.Asset->GetGraph();
		if (!Graph)
		{
			Out = { TEXT("no_editor_graph"), FString::Printf(TEXT("%s has no editor graph"), *Run.Asset->GetPathName()) };
			return false;
		}
		Graph->Modify();
		UFlowGraphNode* GraphNode = FFlowGraphSchemaAction_NewNode::CreateNode(
			Graph, /*FromPin=*/nullptr, NodeClass,
			FVector2f(static_cast<float>(PosX), static_cast<float>(PosY)), /*bSelectNewNode=*/false);
		UFlowNode* Node = GraphNode ? Cast<UFlowNode>(GraphNode->GetFlowNodeBase()) : nullptr;
		if (!Node)
		{
			Out = { TEXT("node_create_failed"), FString::Printf(TEXT("Schema refused to create a node of class %s"), *NodeClass->GetPathName()) };
			return false;
		}
		if (const TSharedPtr<FJsonObject>* Props = PropertiesOf(Op))
		{
			if (!ApplyPropertiesJson(Node, *Props, Out)) return false;
			// Rebuild pins so context-pin nodes reflect the fresh property state.
			GraphNode->ReconstructNode();
		}

		const FString Alias = StringField(Op, TEXT("alias"));
		if (!Alias.IsEmpty())
		{
			Run.Aliases.Add(Alias, MonolithFlow::NodeKeyOf(Node));
			Result->SetStringField(TEXT("alias"), Alias);
		}
		Result->SetStringField(TEXT("node_guid"), GuidStrOf(Node));
		Result->SetStringField(TEXT("node_class"), Node->GetClass()->GetPathName());
		TArray<TSharedPtr<FJsonValue>> InPins, OutPins;
		for (const FFlowPin& Pin : Node->GetInputPins())  InPins.Add(MakeShared<FJsonValueString>(Pin.PinName.ToString()));
		for (const FFlowPin& Pin : Node->GetOutputPins()) OutPins.Add(MakeShared<FJsonValueString>(Pin.PinName.ToString()));
		Result->SetArrayField(TEXT("input_pins"), InPins);
		Result->SetArrayField(TEXT("output_pins"), OutPins);
		return true;
	}

	// --- set_node_properties ------------------------------------------------

	bool PlanSetProperties(FEditPlan& Plan, const TSharedPtr<FJsonObject>& Op, FEditError& Out)
	{
		if (!Require(Op, { TEXT("node_guid"), TEXT("properties") }, Out)) return false;
		UClass* NodeClass = PlanNodeRef(Plan, StringField(Op, TEXT("node_guid")), Out);
		if (!NodeClass) return false;
		const TSharedPtr<FJsonObject>* Props = PropertiesOf(Op);
		if (!Props)
		{
			Out = { TEXT("invalid_params"), TEXT("properties must be an object") };
			return false;
		}
		return ValidatePropertyKeys(NodeClass, *Props, Out);
	}

	bool ApplySetProperties(FEditRun& Run, const TSharedPtr<FJsonObject>& Op, TSharedPtr<FJsonObject>& Result, FEditError& Out)
	{
		UFlowNode* Node = ResolveNodeRef(Run, StringField(Op, TEXT("node_guid")), Out);
		UFlowGraphNode* GraphNode = Node ? GraphNodeOrError(Node, Out) : nullptr;
		if (!GraphNode) return false;
		const TSharedPtr<FJsonObject>* Props = PropertiesOf(Op);
		if (!ApplyPropertiesJson(Node, *Props, Out)) return false;
		GraphNode->ReconstructNode();
		Result->SetStringField(TEXT("node_guid"), GuidStrOf(Node));
		Result->SetNumberField(TEXT("applied_properties"), (*Props)->Values.Num());
		return true;
	}

	// --- remove_node --------------------------------------------------------

	bool PlanRemoveNode(FEditPlan& Plan, const TSharedPtr<FJsonObject>& Op, FEditError& Out)
	{
		if (!Require(Op, { TEXT("node_guid") }, Out)) return false;
		const FString Ref = StringField(Op, TEXT("node_guid"));
		if (!PlanNodeRef(Plan, Ref, Out)) return false;
		FGuid Guid;
		if (IsAliasRef(Ref))
		{
			Plan.AliasClasses.Remove(Ref.Mid(1));
		}
		else if (FGuid::Parse(Ref, Guid))
		{
			// Same rule as the editor's Delete key: Start and other nodes that opt out stay.
			const UFlowNode* Node = Plan.Asset->GetNodes().FindRef(Guid);
			const UEdGraphNode* GraphNode = Node ? Node->GetGraphNode() : nullptr;
			if (GraphNode && !GraphNode->CanUserDeleteNode())
			{
				Out = { TEXT("node_not_deletable"), FString::Printf(TEXT("%s (%s) cannot be deleted - the editor does not allow it either"),
					*Ref, *Node->GetClass()->GetName()) };
				return false;
			}
			Plan.Removed.Add(Guid);
		}
		return true;
	}

	bool ApplyRemoveNode(FEditRun& Run, const TSharedPtr<FJsonObject>& Op, TSharedPtr<FJsonObject>& Result, FEditError& Out)
	{
		UFlowNode* Node = ResolveNodeRef(Run, StringField(Op, TEXT("node_guid")), Out);
		UFlowGraphNode* GraphNode = Node ? GraphNodeOrError(Node, Out) : nullptr;
		if (!GraphNode) return false;
		UEdGraph* Graph = GraphNode->GetGraph();
		const UEdGraphSchema* Schema = Graph ? Graph->GetSchema() : nullptr;
		if (!Schema)
		{
			Out = { TEXT("no_editor_graph"), TEXT("Graph schema not available") };
			return false;
		}

		const FGuid Key = MonolithFlow::NodeKeyOf(Node);
		Graph->Modify();
		GraphNode->Modify();
		Schema->BreakNodeLinks(*GraphNode);
		GraphNode->DestroyNode();
		// DestroyNode only removes the editor wrapper - the runtime node must be
		// unregistered explicitly, mirroring SFlowGraphEditor::DeleteSelectedNodes.
		Run.Asset->UnregisterNode(Key);

		Result->SetStringField(TEXT("removed_node_guid"), Key.ToString(EGuidFormats::DigitsWithHyphens));
		return true;
	}

	// --- add_comment --------------------------------------------------------

	bool PlanAddComment(FEditPlan& Plan, const TSharedPtr<FJsonObject>& Op, FEditError& Out)
	{
		return Require(Op, { TEXT("text") }, Out);
	}

	bool ApplyAddComment(FEditRun& Run, const TSharedPtr<FJsonObject>& Op, TSharedPtr<FJsonObject>& Result, FEditError& Out)
	{
		UEdGraph* Graph = Run.Asset->GetGraph();
		if (!Graph)
		{
			Out = { TEXT("no_editor_graph"), FString::Printf(TEXT("%s has no editor graph"), *Run.Asset->GetPathName()) };
			return false;
		}
		double PosX = 0, PosY = 0, Width = 400, Height = 200;
		Op->TryGetNumberField(TEXT("pos_x"), PosX);
		Op->TryGetNumberField(TEXT("pos_y"), PosY);
		Op->TryGetNumberField(TEXT("width"), Width);
		Op->TryGetNumberField(TEXT("height"), Height);

		Graph->Modify();
		UEdGraphNode_Comment* Comment = NewObject<UEdGraphNode_Comment>(Graph, NAME_None, RF_Transactional);
		Comment->NodeComment = StringField(Op, TEXT("text"));
		Comment->NodePosX = static_cast<int32>(PosX);
		Comment->NodePosY = static_cast<int32>(PosY);
		Comment->NodeWidth = static_cast<int32>(Width);
		Comment->NodeHeight = static_cast<int32>(Height);
		Comment->CreateNewGuid();
		Graph->AddNode(Comment, /*bUserAction=*/false, /*bSelectNewNode=*/false);

		Result->SetStringField(TEXT("comment_guid"), Comment->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
		return true;
	}

	// --- connect_pins -------------------------------------------------------

	bool PlanConnect(FEditPlan& Plan, const TSharedPtr<FJsonObject>& Op, FEditError& Out)
	{
		if (!Require(Op, { TEXT("from_node_guid"), TEXT("from_pin"), TEXT("to_node_guid") }, Out)) return false;
		return PlanNodeRef(Plan, StringField(Op, TEXT("from_node_guid")), Out)
			&& PlanNodeRef(Plan, StringField(Op, TEXT("to_node_guid")), Out);
	}

	bool ApplyConnect(FEditRun& Run, const TSharedPtr<FJsonObject>& Op, TSharedPtr<FJsonObject>& Result, FEditError& Out)
	{
		const FString FromPinName = StringField(Op, TEXT("from_pin"));
		FString ToPinName = StringField(Op, TEXT("to_pin"));
		if (ToPinName.IsEmpty()) ToPinName = TEXT("In");

		UFlowNode* FromNode = ResolveNodeRef(Run, StringField(Op, TEXT("from_node_guid")), Out);
		UFlowNode* ToNode = FromNode ? ResolveNodeRef(Run, StringField(Op, TEXT("to_node_guid")), Out) : nullptr;
		UFlowGraphNode* FromGraphNode = ToNode ? GraphNodeOrError(FromNode, Out) : nullptr;
		UFlowGraphNode* ToGraphNode = FromGraphNode ? GraphNodeOrError(ToNode, Out) : nullptr;
		if (!ToGraphNode) return false;

		UEdGraphPin* FromPin = FromGraphNode->FindPin(FName(*FromPinName), EGPD_Output);
		UEdGraphPin* ToPin = ToGraphNode->FindPin(FName(*ToPinName), EGPD_Input);
		if (!FromPin || !ToPin)
		{
			Out = { TEXT("pin_not_found"), !FromPin
				? FString::Printf(TEXT("Output pin '%s' not found on node %s"), *FromPinName, *GuidStrOf(FromNode))
				: FString::Printf(TEXT("Input pin '%s' not found on node %s"), *ToPinName, *GuidStrOf(ToNode)) };
			return false;
		}
		const UEdGraphSchema* Schema = FromGraphNode->GetSchema();
		const FPinConnectionResponse Response = Schema->CanCreateConnection(FromPin, ToPin);
		if (Response.Response == CONNECT_RESPONSE_DISALLOW)
		{
			Out = { TEXT("connection_refused"), FString::Printf(TEXT("Schema refused %s -> %s: %s"),
				*DescribePin(FromPin), *DescribePin(ToPin), *Response.Message.ToString()) };
			return false;
		}

		// The schema may break existing links to make room (an exec output drives one
		// input, a data input reads one output) - exactly as a wire dragged in the
		// editor does. Remember them so the answer says what was replaced.
		TArray<TPair<UEdGraphPin*, UEdGraphPin*>> LinksBefore;
		for (UEdGraphPin* Linked : FromPin->LinkedTo) LinksBefore.Emplace(FromPin, Linked);
		for (UEdGraphPin* Linked : ToPin->LinkedTo)   LinksBefore.Emplace(Linked, ToPin);

		FromGraphNode->Modify();
		ToGraphNode->Modify();
		if (!Schema->TryCreateConnection(FromPin, ToPin))
		{
			Out = { TEXT("connection_refused"), FString::Printf(TEXT("Schema failed to connect %s -> %s"), *DescribePin(FromPin), *DescribePin(ToPin)) };
			return false;
		}

		TArray<TSharedPtr<FJsonValue>> Broken;
		for (const TPair<UEdGraphPin*, UEdGraphPin*>& Link : LinksBefore)
		{
			if (!Link.Key->LinkedTo.Contains(Link.Value))
			{
				const TSharedPtr<FJsonValue> Value = MakeShared<FJsonValueString>(
					FString::Printf(TEXT("%s -> %s"), *DescribePin(Link.Key), *DescribePin(Link.Value)));
				Broken.Add(Value);
				Run.BrokenLinks.Add(Value);
			}
		}
		Result->SetStringField(TEXT("from"), DescribePin(FromPin));
		Result->SetStringField(TEXT("to"), DescribePin(ToPin));
		Result->SetArrayField(TEXT("broken_links"), Broken);
		return true;
	}

	// --- disconnect_pins ----------------------------------------------------

	bool PlanDisconnect(FEditPlan& Plan, const TSharedPtr<FJsonObject>& Op, FEditError& Out)
	{
		if (!Require(Op, { TEXT("node_guid"), TEXT("pin_name") }, Out)) return false;
		return PlanNodeRef(Plan, StringField(Op, TEXT("node_guid")), Out) != nullptr;
	}

	bool ApplyDisconnect(FEditRun& Run, const TSharedPtr<FJsonObject>& Op, TSharedPtr<FJsonObject>& Result, FEditError& Out)
	{
		UFlowNode* Node = ResolveNodeRef(Run, StringField(Op, TEXT("node_guid")), Out);
		UFlowGraphNode* GraphNode = Node ? GraphNodeOrError(Node, Out) : nullptr;
		if (!GraphNode) return false;
		const FString PinName = StringField(Op, TEXT("pin_name"));
		UEdGraphPin* Pin = GraphNode->FindPin(FName(*PinName));
		if (!Pin)
		{
			Out = { TEXT("pin_not_found"), FString::Printf(TEXT("Pin '%s' not found on node %s"), *PinName, *GuidStrOf(Node)) };
			return false;
		}
		const int32 LinkCount = Pin->LinkedTo.Num();
		GraphNode->Modify();
		GraphNode->GetSchema()->BreakPinLinks(*Pin, /*bSendsNodeNotification=*/true);
		Result->SetStringField(TEXT("pin"), DescribePin(Pin));
		Result->SetNumberField(TEXT("links_broken"), LinkCount);
		return true;
	}

	// --- runner -------------------------------------------------------------

	struct FEditOp
	{
		const TCHAR* Name;
		bool (*Plan)(FEditPlan&, const TSharedPtr<FJsonObject>&, FEditError&);
		bool (*Apply)(FEditRun&, const TSharedPtr<FJsonObject>&, TSharedPtr<FJsonObject>&, FEditError&);
	};

	const FEditOp GEditOps[] =
	{
		{ TEXT("add_node"),            &PlanAddNode,       &ApplyAddNode },
		{ TEXT("set_node_properties"), &PlanSetProperties, &ApplySetProperties },
		{ TEXT("remove_node"),         &PlanRemoveNode,    &ApplyRemoveNode },
		{ TEXT("add_comment"),         &PlanAddComment,    &ApplyAddComment },
		{ TEXT("connect_pins"),        &PlanConnect,       &ApplyConnect },
		{ TEXT("disconnect_pins"),     &PlanDisconnect,    &ApplyDisconnect },
	};

	const FEditOp* FindEditOp(const FString& Name)
	{
		for (const FEditOp& Op : GEditOps)
		{
			if (Name == Op.Name) return &Op;
		}
		return nullptr;
	}

	FString EditOpNames()
	{
		TArray<FString> Names;
		for (const FEditOp& Op : GEditOps) Names.Add(Op.Name);
		return FString::Join(Names, TEXT(", "));
	}

	/**
	 * Validate, then apply a list of operations to one asset in one transaction.
	 * bBatch shapes the answer: a batch reports per-operation results, a single
	 * action answers with its one operation's fields at the top level.
	 */
	FMonolithActionResult RunEdit(const TSharedPtr<FJsonObject>& Params, const TArray<TPair<const FEditOp*, TSharedPtr<FJsonObject>>>& Ops,
		const FText& TransactionTitle, bool bBatch)
	{
		FString Err;
		UFlowAsset* FlowAsset = LoadFlowAssetParam(Params, Err);
		if (!FlowAsset)
		{
			return Fail({ Params.IsValid() && Params->HasField(TEXT("asset_path")) ? TEXT("asset_not_found") : TEXT("invalid_params"), Err });
		}

		bool bDryRun = false;
		Params->TryGetBoolField(TEXT("dry_run"), bDryRun);

		FEditPlan Plan;
		Plan.Asset = FlowAsset;
		for (int32 Index = 0; Index < Ops.Num(); ++Index)
		{
			FEditError Error;
			if (!Ops[Index].Key->Plan(Plan, Ops[Index].Value, Error))
			{
				Error.OpIndex = bBatch ? Index : INDEX_NONE;
				return Fail(Error);
			}
		}

		FEditRun Run;
		Run.Asset = FlowAsset;
		TArray<TSharedPtr<FJsonValue>> OpResults;
		FEditError Error;
		const FUndoMark UndoMark;
		{
			const FScopedTransaction Transaction(TransactionTitle);
			FlowAsset->Modify();
			for (int32 Index = 0; Index < Ops.Num(); ++Index)
			{
				TSharedPtr<FJsonObject> OpResult = MakeShared<FJsonObject>();
				OpResult->SetStringField(TEXT("op"), Ops[Index].Key->Name);
				if (!Ops[Index].Key->Apply(Run, Ops[Index].Value, OpResult, Error))
				{
					Error.OpIndex = bBatch ? Index : INDEX_NONE;
					break;
				}
				OpResults.Add(MakeShared<FJsonValueObject>(OpResult));
			}
			if (Error.Code.IsEmpty())
			{
				FlowAsset->HarvestNodeConnections();
				if (!bDryRun)
				{
					FlowAsset->MarkPackageDirty();
				}
			}
		}

		if (!Error.Code.IsEmpty())
		{
			return Fail(Error, UndoMark.RollBack() ? TEXT("done") : TEXT("failed"));
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		if (bBatch)
		{
			Result->SetArrayField(TEXT("results"), OpResults);
			TSharedRef<FJsonObject> Aliases = MakeShared<FJsonObject>();
			for (const TPair<FString, FGuid>& Pair : Run.Aliases)
			{
				Aliases->SetStringField(Pair.Key, Pair.Value.ToString(EGuidFormats::DigitsWithHyphens));
			}
			Result->SetObjectField(TEXT("aliases"), Aliases);
			Result->SetArrayField(TEXT("broken_links"), Run.BrokenLinks);
		}
		else if (OpResults.Num() == 1)
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : OpResults[0]->AsObject()->Values)
			{
				if (Field.Key != TEXT("op")) Result->SetField(Field.Key, Field.Value);
			}
		}
		Result->SetStringField(TEXT("asset_path"), FlowAsset->GetPathName());
		Result->SetNumberField(TEXT("node_count"), FlowAsset->GetNodes().Num());

		if (bDryRun)
		{
			// Everything above describes the graph as the edit left it; now put it back.
			const bool bRolledBack = UndoMark.RollBack();
			Result->SetBoolField(TEXT("dry_run"), true);
			Result->SetBoolField(TEXT("rolled_back"), bRolledBack);
			if (!bRolledBack)
			{
				return Fail({ TEXT("rollback_failed"), TEXT("Dry run applied but could not be undone") }, TEXT("failed"));
			}
			return FMonolithActionResult::Success(Result);
		}

		if (!MaybeSave(Params, FlowAsset, Result))
		{
			return Fail({ TEXT("save_failed"), FString::Printf(TEXT("Edit applied but failed to save %s"), *FlowAsset->GetPathName()) });
		}
		return FMonolithActionResult::Success(Result);
	}

	/** A single edit action: its params are one operation. */
	FMonolithActionResult RunSingle(const TCHAR* OpName, const TSharedPtr<FJsonObject>& Params, const FText& Title)
	{
		if (!Params.IsValid())
		{
			return Fail({ TEXT("invalid_params"), TEXT("params are required") });
		}
		TArray<TPair<const FEditOp*, TSharedPtr<FJsonObject>>> Ops;
		Ops.Emplace(FindEditOp(OpName), Params);
		return RunEdit(Params, Ops, Title, /*bBatch=*/false);
	}
}

FMonolithActionResult FMonolithFlowAuthorActions::AddNode(const TSharedPtr<FJsonObject>& Params)
{
	return RunSingle(TEXT("add_node"), Params, LOCTEXT("AddFlowNode", "Monolith: Add Flow Node"));
}

FMonolithActionResult FMonolithFlowAuthorActions::SetNodeProperties(const TSharedPtr<FJsonObject>& Params)
{
	return RunSingle(TEXT("set_node_properties"), Params, LOCTEXT("SetFlowNodeProperties", "Monolith: Set Flow Node Properties"));
}

FMonolithActionResult FMonolithFlowAuthorActions::RemoveNode(const TSharedPtr<FJsonObject>& Params)
{
	return RunSingle(TEXT("remove_node"), Params, LOCTEXT("RemoveFlowNode", "Monolith: Remove Flow Node"));
}

FMonolithActionResult FMonolithFlowAuthorActions::AddComment(const TSharedPtr<FJsonObject>& Params)
{
	return RunSingle(TEXT("add_comment"), Params, LOCTEXT("AddFlowComment", "Monolith: Add Flow Graph Comment"));
}

FMonolithActionResult FMonolithFlowAuthorActions::ConnectPins(const TSharedPtr<FJsonObject>& Params)
{
	return RunSingle(TEXT("connect_pins"), Params, LOCTEXT("ConnectFlowPins", "Monolith: Connect Flow Pins"));
}

FMonolithActionResult FMonolithFlowAuthorActions::DisconnectPins(const TSharedPtr<FJsonObject>& Params)
{
	return RunSingle(TEXT("disconnect_pins"), Params, LOCTEXT("DisconnectFlowPins", "Monolith: Disconnect Flow Pins"));
}

FMonolithActionResult FMonolithFlowAuthorActions::ApplyBatch(const TSharedPtr<FJsonObject>& Params)
{
	const TArray<TSharedPtr<FJsonValue>>* OpValues = nullptr;
	if (!Params.IsValid() || !Params->TryGetArrayField(TEXT("operations"), OpValues) || OpValues->Num() == 0)
	{
		return Fail({ TEXT("invalid_params"), TEXT("operations must be a non-empty array") });
	}

	TArray<TPair<const FEditOp*, TSharedPtr<FJsonObject>>> Ops;
	for (int32 Index = 0; Index < OpValues->Num(); ++Index)
	{
		const TSharedPtr<FJsonObject>* OpObject = nullptr;
		FString OpName;
		if (!(*OpValues)[Index]->TryGetObject(OpObject) || !(*OpObject)->TryGetStringField(TEXT("op"), OpName))
		{
			FEditError Error{ TEXT("invalid_params"), TEXT("each operation must be an object with an `op` field") };
			Error.OpIndex = Index;
			return Fail(Error);
		}
		const FEditOp* Op = FindEditOp(OpName);
		if (!Op)
		{
			FEditError Error{ TEXT("unknown_op"), FString::Printf(TEXT("Unknown op '%s' (expected one of: %s)"), *OpName, *EditOpNames()) };
			Error.OpIndex = Index;
			return Fail(Error);
		}
		Ops.Emplace(Op, *OpObject);
	}
	return RunEdit(Params, Ops, LOCTEXT("ApplyFlowBatch", "Monolith: Edit Flow Graph"), /*bBatch=*/true);
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

		// Loops (retry, wait-until) would make longest-path layering climb forever,
		// so drop back edges first: a DFS from the entry nodes (no predecessors),
		// then from anything left, marks every edge into a node still on the stack.
		TArray<TSet<int32>> Forward;
		Forward.SetNum(N);
		{
			TArray<uint8> State;   // 0 unvisited, 1 on stack, 2 done
			State.Init(0, N);
			auto Visit = [&](int32 Root)
			{
				TArray<TPair<int32, int32>> Stack;   // node, next successor index
				Stack.Emplace(Root, 0);
				State[Root] = 1;
				while (Stack.Num() > 0)
				{
					TPair<int32, int32>& Top = Stack.Last();
					const int32 Node = Top.Key;
					if (Top.Value < G.Out[Node].Num())
					{
						const int32 Succ = G.Out[Node][Top.Value++];
						if (State[Succ] == 1)
						{
							continue;   // back edge
						}
						Forward[Node].Add(Succ);
						if (State[Succ] == 0)
						{
							State[Succ] = 1;
							Stack.Emplace(Succ, 0);
						}
					}
					else
					{
						State[Node] = 2;
						Stack.Pop();
					}
				}
			};
			for (int32 i = 0; i < N; ++i) if (State[i] == 0 && G.In[i].Num() == 0) Visit(i);
			for (int32 i = 0; i < N; ++i) if (State[i] == 0) Visit(i);
		}

		// Longest-path layering over the acyclic rest, in topological order.
		TArray<int32> Layer;
		Layer.Init(0, N);
		{
			TArray<int32> Pending;
			Pending.Init(0, N);
			for (int32 i = 0; i < N; ++i) for (int32 Succ : Forward[i]) ++Pending[Succ];
			TArray<int32> Ready;
			for (int32 i = 0; i < N; ++i) if (Pending[i] == 0) Ready.Add(i);
			while (Ready.Num() > 0)
			{
				const int32 Node = Ready.Pop(EAllowShrinking::No);
				for (int32 Succ : Forward[Node])
				{
					Layer[Succ] = FMath::Max(Layer[Succ], Layer[Node] + 1);
					if (--Pending[Succ] == 0) Ready.Add(Succ);
				}
			}
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

	if (Formatter != TEXT("monolith") && Formatter != TEXT("blueprint_assist") && Formatter != TEXT("auto"))
	{
		return Fail({ TEXT("invalid_params"), FString::Printf(TEXT("Unknown formatter '%s' (monolith | blueprint_assist | auto)"), *Formatter) });
	}
	if (LayoutMode != TEXT("all") && LayoutMode != TEXT("new_only") && LayoutMode != TEXT("selected"))
	{
		return Fail({ TEXT("invalid_params"), FString::Printf(TEXT("Unknown layout_mode '%s' (all | new_only | selected)"), *LayoutMode) });
	}

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
					// Nodes the graph was created with (Start - the editor will not delete
					// them) sit at the origin by design and anchor the rest.
					if (FlowNode->NodePosX == 0 && FlowNode->NodePosY == 0 && FlowNode->CanUserDeleteNode())
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
