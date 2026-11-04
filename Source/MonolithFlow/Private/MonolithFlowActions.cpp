#include "MonolithFlowActions.h"

#if WITH_FLOW

#include "MonolithToolRegistry.h"
#include "MonolithParamSchema.h"
#include "MonolithIndexSubsystem.h"
#include "MonolithIndexDatabase.h"
#include "SQLiteDatabase.h"
#include "SQLitePreparedStatement.h"
#include "Editor.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"

// ============================================================================
// Local helpers
// ============================================================================

namespace
{
	/** Fetch the SQLite raw handle, or set OutError + return null. */
	FSQLiteDatabase* GetRawDB(FString& OutError)
	{
		if (!GEditor)
		{
			OutError = TEXT("GEditor not available");
			return nullptr;
		}
		UMonolithIndexSubsystem* IndexSS = GEditor->GetEditorSubsystem<UMonolithIndexSubsystem>();
		if (!IndexSS || !IndexSS->GetDatabase())
		{
			OutError = TEXT("Index database not ready");
			return nullptr;
		}
		FSQLiteDatabase* RawDB = IndexSS->GetDatabase()->GetRawDatabase();
		if (!RawDB)
		{
			OutError = TEXT("Raw SQLite database not available");
			return nullptr;
		}
		return RawDB;
	}

	/** Convert a glob pattern (* / ?) to SQL LIKE with single-quote escape. */
	FString GlobToLike(const FString& Glob)
	{
		return Glob
			.Replace(TEXT("'"), TEXT("''"))
			.Replace(TEXT("*"), TEXT("%"))
			.Replace(TEXT("?"), TEXT("_"));
	}

	/**
	 * fa_path is an object path ("/Game/Flow/Foo.Foo"). Callers often hold the package
	 * path ("/Game/Flow/Foo") instead - complete it, since a package name never has a dot.
	 */
	FString ToFlowObjectPath(const FString& Path)
	{
		int32 LastSlash = INDEX_NONE;
		if (Path.Contains(TEXT(".")) || !Path.FindLastChar(TEXT('/'), LastSlash) || LastSlash == Path.Len() - 1)
		{
			return Path;
		}
		return Path + TEXT(".") + Path.Mid(LastSlash + 1);
	}

	/** Read an optional integer param within [Min, Max]; false + OutError when it is out of range. */
	bool ReadBoundedInt(const TSharedPtr<FJsonObject>& Params, const TCHAR* Name, int32 Default, int32 Min, int32 Max, int32& OutValue, FString& OutError)
	{
		OutValue = Default;
		double Raw = 0.0;
		if (!Params.IsValid() || !Params->TryGetNumberField(Name, Raw))
		{
			return true;
		}
		if (Raw < Min || Raw > Max || Raw != FMath::FloorToDouble(Raw))
		{
			OutError = FString::Printf(TEXT("%s must be an integer in [%d, %d]"), Name, Min, Max);
			return false;
		}
		OutValue = static_cast<int32>(Raw);
		return true;
	}

	/**
	 * Lookup flow_assets row id + asset id by fa_path. Returns -1 if not found.
	 * InOutFaPath is completed to the object path first, so callers echo the canonical form.
	 */
	void SelectFlowAssetIds(FSQLiteDatabase* RawDB, FString& InOutFaPath, int64& OutRowId, int64& OutAssetId)
	{
		InOutFaPath = ToFlowObjectPath(InOutFaPath);
		const FString& FaPath = InOutFaPath;
		OutRowId = -1;
		OutAssetId = -1;
		FSQLitePreparedStatement Stmt;
		if (!Stmt.Create(*RawDB, TEXT("SELECT id, fa_asset_id FROM flow_assets WHERE fa_path = ?")))
		{
			return;
		}
		Stmt.SetBindingValueByIndex(1, FaPath);
		if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			Stmt.GetColumnValueByIndex(0, OutRowId);
			Stmt.GetColumnValueByIndex(1, OutAssetId);
		}
		Stmt.Destroy();
	}

	/** Try parse a JSON string into a JsonValue (object/array); returns null JsonValue on failure. */
	TSharedPtr<FJsonValue> TryParseJsonValue(const FString& JsonStr)
	{
		if (JsonStr.IsEmpty())
		{
			return MakeShared<FJsonValueNull>();
		}
		TSharedPtr<FJsonValue> Out;
		TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(JsonStr);
		if (!FJsonSerializer::Deserialize(Reader, Out) || !Out.IsValid())
		{
			return MakeShared<FJsonValueNull>();
		}
		return Out;
	}
}

// ============================================================================
// Registration
// ============================================================================

void FMonolithFlowActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("flow"), TEXT("ping"),
		TEXT("Smoke test - returns {status:ok, module:MonolithFlow} when the module is loaded."),
		FMonolithActionHandler::CreateStatic(&FMonolithFlowActions::Ping),
		FParamSchemaBuilder().Build());

	Registry.RegisterAction(TEXT("flow"), TEXT("list_assets"),
		TEXT("List UFlowAssets in the index, with node_count / addon_count / subgraph_count summary, ordered by fa_path and paged. Reports total + has_more so a truncated page is never mistaken for the whole project. Optional asset_path_filter is a glob (* and ?) matched against fa_path."),
		FMonolithActionHandler::CreateStatic(&FMonolithFlowActions::ListAssets),
		FParamSchemaBuilder()
			.Optional(TEXT("asset_path_filter"), TEXT("string"), TEXT("Glob pattern to filter fa_path (e.g., \"/Game/Flow/*\")"))
			.Optional(TEXT("offset"), TEXT("number"), TEXT("Zero-based result offset"), TEXT("0"))
			.Optional(TEXT("limit"), TEXT("number"), TEXT("Maximum assets to return (1-1000)"), TEXT("200"))
			.Build());

	Registry.RegisterAction(TEXT("flow"), TEXT("get_asset_info"),
		TEXT("Return summary information for a single Flow Asset: name, asset class, expected owner class, world-bound flag, base-params soft path, total counts (nodes / addons / subgraphs / pins), and a sample of up to 10 nodes for orientation."),
		FMonolithActionHandler::CreateStatic(&FMonolithFlowActions::GetAssetInfo),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("UFlowAsset object path (e.g. \"/Game/Flow/MyScenario.MyScenario\") or package path (\"/Game/Flow/MyScenario\")"))
			.Build());

	Registry.RegisterAction(TEXT("flow"), TEXT("list_nodes"),
		TEXT("List every UFlowNode inside one Flow Asset. Each row carries node_guid, node_class, display_name, is_blueprint flag, blueprint_path (if BP-defined), addon_count, input/output pin counts, and parsed `data` JSON of design-time UPROPERTYs. Optional node_class filter narrows the result."),
		FMonolithActionHandler::CreateStatic(&FMonolithFlowActions::ListNodes),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("UFlowAsset object path (/Pkg/Name.Name) or package path (/Pkg/Name)"))
			.Optional(TEXT("node_class"), TEXT("string"), TEXT("Exact UClass path filter (e.g. \"/Script/Flow.FlowNode_Start\")"))
			.Build());

	Registry.RegisterAction(TEXT("flow"), TEXT("list_node_pins"),
		TEXT("List every pin (input + output) on nodes in one Flow Asset, with full type info (pin_type_name from FFlowPin, plus pin_subcategory_object such as the UClass path for Object pins). Optional filters narrow by node, direction, or pin type."),
		FMonolithActionHandler::CreateStatic(&FMonolithFlowActions::ListNodePins),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("UFlowAsset object path (/Pkg/Name.Name) or package path (/Pkg/Name)"))
			.Optional(TEXT("node_guid"), TEXT("string"), TEXT("Restrict to one node by its FGuid string"))
			.Optional(TEXT("pin_direction"), TEXT("string"), TEXT("\"input\" | \"output\" | \"all\" (default)"))
			.Optional(TEXT("pin_type_name"), TEXT("string"), TEXT("Restrict to pins with this PinTypeName (e.g. \"Exec\", \"Bool\", \"Object\")"))
			.Build());

	Registry.RegisterAction(TEXT("flow"), TEXT("list_connections"),
		TEXT("List every output-pin -> input-pin connection inside one Flow Asset (one row per outgoing edge). Optional from_node_guid / to_node_guid filters narrow the result."),
		FMonolithActionHandler::CreateStatic(&FMonolithFlowActions::ListConnections),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("UFlowAsset object path (/Pkg/Name.Name) or package path (/Pkg/Name)"))
			.Optional(TEXT("from_node_guid"), TEXT("string"), TEXT("Restrict to edges originating at this node"))
			.Optional(TEXT("to_node_guid"), TEXT("string"), TEXT("Restrict to edges terminating at this node"))
			.Build());

	Registry.RegisterAction(TEXT("flow"), TEXT("list_addons"),
		TEXT("List every UFlowNodeAddOn attached to nodes in one Flow Asset (recursive - addons can have child addons). Each row carries owner_node_guid, parent_addon_id (NULL for top-level), depth, addon_class, and parsed `data` JSON of the addon's design-time UPROPERTYs. Optional node_guid filter restricts to one node's addon stack."),
		FMonolithActionHandler::CreateStatic(&FMonolithFlowActions::ListAddons),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("UFlowAsset object path (/Pkg/Name.Name) or package path (/Pkg/Name)"))
			.Optional(TEXT("node_guid"), TEXT("string"), TEXT("Restrict to addons under this node"))
			.Build());

	Registry.RegisterAction(TEXT("flow"), TEXT("get_node_info"),
		TEXT("Single-node deep dive. Joins the flow_nodes row with its parsed `data` JSON, every pin (with type info), every outgoing connection, and the recursive addon tree for one node identified by GUID inside one Flow Asset."),
		FMonolithActionHandler::CreateStatic(&FMonolithFlowActions::GetNodeInfo),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("UFlowAsset object path (/Pkg/Name.Name) or package path (/Pkg/Name)"))
			.Required(TEXT("node_guid"), TEXT("string"), TEXT("Node FGuid string (digits-with-hyphens, matches list_nodes output)"))
			.Build());

	Registry.RegisterAction(TEXT("flow"), TEXT("list_custom_events"),
		TEXT("List Custom Inputs and Custom Outputs of one Flow Asset (the named entry/exit pins exposed to parent SubGraph nodes). Sourced from UFlowAsset::GatherCustomInputNodeEventNames / GatherCustomOutputNodeEventNames - runtime-safe, walks actual graph nodes."),
		FMonolithActionHandler::CreateStatic(&FMonolithFlowActions::ListCustomEvents),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("UFlowAsset object path (/Pkg/Name.Name) or package path (/Pkg/Name)"))
			.Optional(TEXT("kind"), TEXT("string"), TEXT("\"input\" | \"output\" | \"all\" (default)"))
			.Build());

	Registry.RegisterAction(TEXT("flow"), TEXT("find_subgraph_callers"),
		TEXT("Cross-asset reverse lookup: every Flow Asset that hosts a UFlowNode_SubGraph referencing the target Flow Asset. Returns one row per call site (host_fa_path + host_node_guid + target_params_path). Optional asset_path_filter glob narrows the host search."),
		FMonolithActionHandler::CreateStatic(&FMonolithFlowActions::FindSubgraphCallers),
		FParamSchemaBuilder()
			.Required(TEXT("target_asset_path"), TEXT("string"), TEXT("Object path (/Pkg/Name.Name) or package path (/Pkg/Name) of the target subgraph"))
			.Optional(TEXT("asset_path_filter"), TEXT("string"), TEXT("Glob restricting host_fa_path"))
			.Build());

	Registry.RegisterAction(TEXT("flow"), TEXT("find_node_class_usages"),
		TEXT("Cross-asset reverse lookup: every Flow Asset containing a node of the specified UClass path. Returns rows grouped by host_fa_path with per-node guids. Optional asset_path_filter glob narrows the host search."),
		FMonolithActionHandler::CreateStatic(&FMonolithFlowActions::FindNodeClassUsages),
		FParamSchemaBuilder()
			.Required(TEXT("node_class"), TEXT("string"), TEXT("Exact UClass path (e.g. \"/Script/Flow.FlowNode_Log\")"))
			.Optional(TEXT("asset_path_filter"), TEXT("string"), TEXT("Glob restricting host_fa_path"))
			.Build());

	Registry.RegisterAction(TEXT("flow"), TEXT("find_pins_by_type"),
		TEXT("Cross-asset reverse lookup over flow_node_pins: every pin in the index whose pin_type_name matches. Optional pin_subcategory_object (UClass path for Object/Class pins or UScriptStruct path for Struct pins) further narrows results. Optional pin_direction restricts to input/output. Optional asset_path_filter glob restricts host fa_path. Rows grouped by host fa_path."),
		FMonolithActionHandler::CreateStatic(&FMonolithFlowActions::FindPinsByType),
		FParamSchemaBuilder()
			.Required(TEXT("pin_type_name"), TEXT("string"), TEXT("FFlowPin pin_type_name (e.g. \"Exec\", \"Bool\", \"Object\")"))
			.Optional(TEXT("pin_subcategory_object"), TEXT("string"), TEXT("Path of the pin's sub-object (UClass / UScriptStruct)"))
			.Optional(TEXT("pin_direction"), TEXT("string"), TEXT("\"input\" | \"output\" | \"all\" (default)"))
			.Optional(TEXT("asset_path_filter"), TEXT("string"), TEXT("Glob restricting host fa_path"))
			.Build());

	Registry.RegisterAction(TEXT("flow"), TEXT("find_nodes_by_property"),
		TEXT("Cross-asset reverse lookup over the per-node `data` JSON snapshot of design-time UPROPERTYs. Substring match (case-sensitive, SQL LIKE; reserved chars _ % \\ are escaped). Optional property_name scopes the match so it only fires inside `\"property_name\":...value_substring`. Optional node_class narrows to one UClass. Optional asset_path_filter glob narrows host fa_path. Returns matching nodes grouped by host fa_path with their parsed data."),
		FMonolithActionHandler::CreateStatic(&FMonolithFlowActions::FindNodesByProperty),
		FParamSchemaBuilder()
			.Required(TEXT("value_substring"), TEXT("string"), TEXT("Literal substring to find inside the node `data` JSON"))
			.Optional(TEXT("property_name"), TEXT("string"), TEXT("If set, restrict the match to the value side of `\"property_name\":...` (any nesting)"))
			.Optional(TEXT("node_class"), TEXT("string"), TEXT("Restrict to nodes of this UClass path"))
			.Optional(TEXT("asset_path_filter"), TEXT("string"), TEXT("Glob restricting host fa_path"))
			.Build());

	Registry.RegisterAction(TEXT("flow"), TEXT("list_node_classes"),
		TEXT("Registry of every UFlowNode / UFlowNodeAddOn UClass referenced by the project. Two sources populate it: UFlowNodeBlueprint / UFlowNodeAddOnBlueprint assets contribute full BP rows (is_native=0, with bp_path), and any native class encountered while walking a Flow Asset graph contributes an opportunistic row (is_native=1, bp_path=NULL). Lookup key is class_path so flow_nodes.node_class joins straight here. Optional kind narrows to node|addon, optional source narrows to blueprint|native, optional class_path_filter glob restricts the result."),
		FMonolithActionHandler::CreateStatic(&FMonolithFlowActions::ListNodeClasses),
		FParamSchemaBuilder()
			.Optional(TEXT("kind"), TEXT("string"), TEXT("\"node\" | \"addon\" | \"all\" (default)"))
			.Optional(TEXT("source"), TEXT("string"), TEXT("\"blueprint\" | \"native\" | \"all\" (default)"))
			.Optional(TEXT("class_path_filter"), TEXT("string"), TEXT("Glob restricting class_path"))
			.Build());

	Registry.RegisterAction(TEXT("flow"), TEXT("walk_execution"),
		TEXT("BFS over the (non-orphaned) execution edges of one Flow Asset. Entry set defaults to FlowNode_Start + FlowNode_CustomInput nodes; pass start_node_guid to walk from a specific node instead. Returns reached nodes with their BFS depth, unreachable nodes (present in the graph but never visited), and terminal reached nodes (no outgoing edges). Reads the index - save the asset first for fresh results."),
		FMonolithActionHandler::CreateStatic(&FMonolithFlowActions::WalkExecution),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("UFlowAsset object path (/Pkg/Name.Name) or package path (/Pkg/Name)"))
			.Optional(TEXT("start_node_guid"), TEXT("string"), TEXT("Walk from this node instead of the Start/CustomInput entry set"))
			.Build());

	Registry.RegisterAction(TEXT("flow"), TEXT("export_graph"),
		TEXT("One-call compact dump of a whole Flow Asset: asset info, every node (guid, class, display name, editor position, pin counts; full data JSON when include_data=true), the complete edge list, graph comment boxes, and custom input/output events. Designed to replace a list_nodes + list_connections + list_custom_events round-trip when reasoning about a whole graph. Reads the index - save the asset first for fresh results."),
		FMonolithActionHandler::CreateStatic(&FMonolithFlowActions::ExportGraph),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("UFlowAsset object path (/Pkg/Name.Name) or package path (/Pkg/Name)"))
			.Optional(TEXT("include_data"), TEXT("boolean"), TEXT("Include each node's full design-time data JSON (default false - counts and identity only)"))
			.Build());

	Registry.RegisterAction(TEXT("flow"), TEXT("diff_assets"),
		TEXT("Structural diff of two Flow Assets by node guid: nodes only in A, only in B, nodes present in both whose class or data JSON differs, and execution edges added/removed. Useful for comparing scenario variants or verifying a refactor. Reads the index - save both assets first."),
		FMonolithActionHandler::CreateStatic(&FMonolithFlowActions::DiffAssets),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path_a"), TEXT("string"), TEXT("UFlowAsset object or package path (baseline)"))
			.Required(TEXT("asset_path_b"), TEXT("string"), TEXT("UFlowAsset object or package path (compared)"))
			.Build());
}

// ============================================================================
// Handlers
// ============================================================================

FMonolithActionResult FMonolithFlowActions::Ping(const TSharedPtr<FJsonObject>& Params)
{
	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("status"), TEXT("ok"));
	Result->SetStringField(TEXT("module"), TEXT("MonolithFlow"));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithFlowActions::ListAssets(const TSharedPtr<FJsonObject>& Params)
{
	FString Err;
	FSQLiteDatabase* RawDB = GetRawDB(Err);
	if (!RawDB) return FMonolithActionResult::Error(Err);

	FString PathFilter;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("asset_path_filter"), PathFilter);
	}

	int32 Offset = 0, Limit = 0;
	if (!ReadBoundedInt(Params, TEXT("offset"), 0, 0, MAX_int32, Offset, Err)
		|| !ReadBoundedInt(Params, TEXT("limit"), 200, 1, 1000, Limit, Err))
	{
		return FMonolithActionResult::Error(Err);
	}

	FString Where;
	if (!PathFilter.IsEmpty())
	{
		Where = FString::Printf(TEXT(" WHERE fa_path LIKE '%s'"), *GlobToLike(PathFilter));
	}

	int64 Total = 0;
	{
		FSQLitePreparedStatement CountStmt;
		if (CountStmt.Create(*RawDB, *(TEXT("SELECT COUNT(*) FROM flow_assets") + Where))
			&& CountStmt.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			CountStmt.GetColumnValueByIndex(0, Total);
		}
		CountStmt.Destroy();
	}

	const FString SQL = FString(TEXT(
		"SELECT fa_path, name, asset_class, expected_owner_class, base_params_path, "
		"world_bound, node_count, addon_count, subgraph_count "
		"FROM flow_assets"))
		+ Where
		+ FString::Printf(TEXT(" ORDER BY fa_path LIMIT %d OFFSET %d"), Limit, Offset);

	FSQLitePreparedStatement Stmt;
	if (!Stmt.Create(*RawDB, *SQL))
	{
		return FMonolithActionResult::Error(TEXT("Failed to prepare list_assets SQL"));
	}

	TArray<TSharedPtr<FJsonValue>> Rows;
	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FString FaPath, Name, AssetClass, OwnerCls, BaseParams;
		int64 World = 1, NodeCount = 0, AddonCount = 0, SubgraphCount = 0;
		Stmt.GetColumnValueByIndex(0, FaPath);
		Stmt.GetColumnValueByIndex(1, Name);
		Stmt.GetColumnValueByIndex(2, AssetClass);
		Stmt.GetColumnValueByIndex(3, OwnerCls);
		Stmt.GetColumnValueByIndex(4, BaseParams);
		Stmt.GetColumnValueByIndex(5, World);
		Stmt.GetColumnValueByIndex(6, NodeCount);
		Stmt.GetColumnValueByIndex(7, AddonCount);
		Stmt.GetColumnValueByIndex(8, SubgraphCount);

		auto Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("fa_path"), FaPath);
		Row->SetStringField(TEXT("name"), Name);
		Row->SetStringField(TEXT("asset_class"), AssetClass);
		if (!OwnerCls.IsEmpty()) Row->SetStringField(TEXT("expected_owner_class"), OwnerCls);
		if (!BaseParams.IsEmpty()) Row->SetStringField(TEXT("base_params_path"), BaseParams);
		Row->SetBoolField(TEXT("world_bound"), World != 0);
		Row->SetNumberField(TEXT("node_count"), NodeCount);
		Row->SetNumberField(TEXT("addon_count"), AddonCount);
		Row->SetNumberField(TEXT("subgraph_count"), SubgraphCount);
		Rows.Add(MakeShared<FJsonValueObject>(Row));
	}
	Stmt.Destroy();

	auto Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("assets"), Rows);
	Result->SetNumberField(TEXT("count"), Rows.Num());
	Result->SetNumberField(TEXT("offset"), Offset);
	Result->SetNumberField(TEXT("limit"), Limit);
	Result->SetNumberField(TEXT("total"), Total);
	Result->SetBoolField(TEXT("has_more"), Offset + Rows.Num() < Total);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithFlowActions::GetAssetInfo(const TSharedPtr<FJsonObject>& Params)
{
	FString Err;
	FSQLiteDatabase* RawDB = GetRawDB(Err);
	if (!RawDB) return FMonolithActionResult::Error(Err);

	FString AssetPath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path is required"));
	}

	int64 RowId = -1, AssetId = -1;
	SelectFlowAssetIds(RawDB, AssetPath, RowId, AssetId);
	if (RowId <= 0)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("No Flow Asset indexed at path '%s'. Pass the object path (\"/Game/Flow/Foo.Foo\") or the package path (\"/Game/Flow/Foo\")."),
			*AssetPath));
	}

	auto Result = MakeShared<FJsonObject>();

	// Header row from flow_assets
	{
		FSQLitePreparedStatement Stmt;
		if (!Stmt.Create(*RawDB, TEXT(
			"SELECT name, asset_class, asset_guid, expected_owner_class, base_params_path, "
			"world_bound, node_count, addon_count, subgraph_count "
			"FROM flow_assets WHERE id = ?")))
		{
			return FMonolithActionResult::Error(TEXT("Failed to prepare get_asset_info header SQL"));
		}
		Stmt.SetBindingValueByIndex(1, RowId);
		if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			FString Name, AssetClass, Guid, Owner, BaseParams;
			int64 World = 1, NodeCount = 0, AddonCount = 0, SubgraphCount = 0;
			Stmt.GetColumnValueByIndex(0, Name);
			Stmt.GetColumnValueByIndex(1, AssetClass);
			Stmt.GetColumnValueByIndex(2, Guid);
			Stmt.GetColumnValueByIndex(3, Owner);
			Stmt.GetColumnValueByIndex(4, BaseParams);
			Stmt.GetColumnValueByIndex(5, World);
			Stmt.GetColumnValueByIndex(6, NodeCount);
			Stmt.GetColumnValueByIndex(7, AddonCount);
			Stmt.GetColumnValueByIndex(8, SubgraphCount);

			Result->SetStringField(TEXT("fa_path"), AssetPath);
			Result->SetStringField(TEXT("name"), Name);
			Result->SetStringField(TEXT("asset_class"), AssetClass);
			if (!Guid.IsEmpty()) Result->SetStringField(TEXT("asset_guid"), Guid);
			if (!Owner.IsEmpty()) Result->SetStringField(TEXT("expected_owner_class"), Owner);
			if (!BaseParams.IsEmpty()) Result->SetStringField(TEXT("base_params_path"), BaseParams);
			Result->SetBoolField(TEXT("world_bound"), World != 0);
			Result->SetNumberField(TEXT("node_count"), NodeCount);
			Result->SetNumberField(TEXT("addon_count"), AddonCount);
			Result->SetNumberField(TEXT("subgraph_count"), SubgraphCount);
		}
		Stmt.Destroy();
	}

	// Pin total
	{
		FSQLitePreparedStatement Stmt;
		if (Stmt.Create(*RawDB, TEXT("SELECT COUNT(*) FROM flow_node_pins WHERE fa_asset_id = ?")))
		{
			Stmt.SetBindingValueByIndex(1, AssetId);
			if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
			{
				int64 PinCount = 0;
				Stmt.GetColumnValueByIndex(0, PinCount);
				Result->SetNumberField(TEXT("pin_count"), PinCount);
			}
			Stmt.Destroy();
		}
	}

	// Node-class breakdown
	{
		FSQLitePreparedStatement Stmt;
		if (Stmt.Create(*RawDB, TEXT(
			"SELECT node_class, COUNT(*) AS n FROM flow_nodes WHERE fa_asset_id = ? "
			"GROUP BY node_class ORDER BY n DESC, node_class")))
		{
			Stmt.SetBindingValueByIndex(1, AssetId);
			TSharedRef<FJsonObject> Breakdown = MakeShared<FJsonObject>();
			while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
			{
				FString Cls;
				int64 N = 0;
				Stmt.GetColumnValueByIndex(0, Cls);
				Stmt.GetColumnValueByIndex(1, N);
				Breakdown->SetNumberField(Cls, N);
			}
			Stmt.Destroy();
			Result->SetObjectField(TEXT("node_class_breakdown"), Breakdown);
		}
	}

	// Sample of up to 10 nodes
	{
		FSQLitePreparedStatement Stmt;
		if (Stmt.Create(*RawDB, TEXT(
			"SELECT node_guid, node_class, display_name, input_pin_count, output_pin_count "
			"FROM flow_nodes WHERE fa_asset_id = ? "
			"ORDER BY node_class, node_guid LIMIT 10")))
		{
			Stmt.SetBindingValueByIndex(1, AssetId);
			TArray<TSharedPtr<FJsonValue>> Sample;
			while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
			{
				FString Guid, Cls, Display;
				int64 InN = 0, OutN = 0;
				Stmt.GetColumnValueByIndex(0, Guid);
				Stmt.GetColumnValueByIndex(1, Cls);
				Stmt.GetColumnValueByIndex(2, Display);
				Stmt.GetColumnValueByIndex(3, InN);
				Stmt.GetColumnValueByIndex(4, OutN);

				auto Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("node_guid"), Guid);
				Row->SetStringField(TEXT("node_class"), Cls);
				if (!Display.IsEmpty()) Row->SetStringField(TEXT("display_name"), Display);
				Row->SetNumberField(TEXT("input_pin_count"), InN);
				Row->SetNumberField(TEXT("output_pin_count"), OutN);
				Sample.Add(MakeShared<FJsonValueObject>(Row));
			}
			Stmt.Destroy();
			Result->SetArrayField(TEXT("sample_nodes"), Sample);
		}
	}

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithFlowActions::ListNodes(const TSharedPtr<FJsonObject>& Params)
{
	FString Err;
	FSQLiteDatabase* RawDB = GetRawDB(Err);
	if (!RawDB) return FMonolithActionResult::Error(Err);

	FString AssetPath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path is required"));
	}

	int64 RowId = -1, AssetId = -1;
	SelectFlowAssetIds(RawDB, AssetPath, RowId, AssetId);
	if (RowId <= 0)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("No Flow Asset indexed at path '%s'."), *AssetPath));
	}

	FString NodeClassFilter;
	Params->TryGetStringField(TEXT("node_class"), NodeClassFilter);

	FString SQL = TEXT(
		"SELECT node_guid, node_class, display_name, is_blueprint, blueprint_path, "
		"addon_count, input_pin_count, output_pin_count, data "
		"FROM flow_nodes WHERE fa_asset_id = ?");
	if (!NodeClassFilter.IsEmpty())
	{
		SQL += TEXT(" AND node_class = ?");
	}
	SQL += TEXT(" ORDER BY node_class, node_guid");

	FSQLitePreparedStatement Stmt;
	if (!Stmt.Create(*RawDB, *SQL))
	{
		return FMonolithActionResult::Error(TEXT("Failed to prepare list_nodes SQL"));
	}
	Stmt.SetBindingValueByIndex(1, AssetId);
	if (!NodeClassFilter.IsEmpty())
	{
		Stmt.SetBindingValueByIndex(2, NodeClassFilter);
	}

	TArray<TSharedPtr<FJsonValue>> Rows;
	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FString Guid, Cls, Display, BpPath, DataJson;
		int64 IsBp = 0, AddonN = 0, InN = 0, OutN = 0;
		Stmt.GetColumnValueByIndex(0, Guid);
		Stmt.GetColumnValueByIndex(1, Cls);
		Stmt.GetColumnValueByIndex(2, Display);
		Stmt.GetColumnValueByIndex(3, IsBp);
		Stmt.GetColumnValueByIndex(4, BpPath);
		Stmt.GetColumnValueByIndex(5, AddonN);
		Stmt.GetColumnValueByIndex(6, InN);
		Stmt.GetColumnValueByIndex(7, OutN);
		Stmt.GetColumnValueByIndex(8, DataJson);

		auto Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("node_guid"), Guid);
		Row->SetStringField(TEXT("node_class"), Cls);
		if (!Display.IsEmpty()) Row->SetStringField(TEXT("display_name"), Display);
		Row->SetBoolField(TEXT("is_blueprint"), IsBp != 0);
		if (!BpPath.IsEmpty()) Row->SetStringField(TEXT("blueprint_path"), BpPath);
		Row->SetNumberField(TEXT("addon_count"), AddonN);
		Row->SetNumberField(TEXT("input_pin_count"), InN);
		Row->SetNumberField(TEXT("output_pin_count"), OutN);
		Row->SetField(TEXT("data"), TryParseJsonValue(DataJson));
		Rows.Add(MakeShared<FJsonValueObject>(Row));
	}
	Stmt.Destroy();

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("fa_path"), AssetPath);
	Result->SetArrayField(TEXT("nodes"), Rows);
	Result->SetNumberField(TEXT("count"), Rows.Num());
	return FMonolithActionResult::Success(Result);
}

// ----- Internal helpers shared by Step 3 actions -----

namespace
{
	/** Read every pin row for one node into a JSON array (used by GetNodeInfo). */
	TArray<TSharedPtr<FJsonValue>> ReadPinsForNode(FSQLiteDatabase* RawDB, int64 FaAssetId, const FString& NodeGuid)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		FSQLitePreparedStatement Stmt;
		if (!Stmt.Create(*RawDB, TEXT(
			"SELECT pin_direction, pin_index, pin_name, pin_friendly_name, "
			"pin_type_name, pin_subcategory_object, container_type, tooltip, is_orphaned "
			"FROM flow_node_pins WHERE fa_asset_id = ? AND node_guid = ? "
			"ORDER BY pin_direction, pin_index")))
		{
			return Out;
		}
		Stmt.SetBindingValueByIndex(1, FaAssetId);
		Stmt.SetBindingValueByIndex(2, NodeGuid);
		while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			FString Dir, PinName, Friendly, TypeName, SubCat, Container, Tooltip;
			int64 PinIdx = 0, Orphan = 0;
			Stmt.GetColumnValueByIndex(0, Dir);
			Stmt.GetColumnValueByIndex(1, PinIdx);
			Stmt.GetColumnValueByIndex(2, PinName);
			Stmt.GetColumnValueByIndex(3, Friendly);
			Stmt.GetColumnValueByIndex(4, TypeName);
			Stmt.GetColumnValueByIndex(5, SubCat);
			Stmt.GetColumnValueByIndex(6, Container);
			Stmt.GetColumnValueByIndex(7, Tooltip);
			Stmt.GetColumnValueByIndex(8, Orphan);

			auto Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("pin_direction"), Dir);
			Row->SetNumberField(TEXT("pin_index"), PinIdx);
			Row->SetStringField(TEXT("pin_name"), PinName);
			if (!Friendly.IsEmpty()) Row->SetStringField(TEXT("pin_friendly_name"), Friendly);
			Row->SetStringField(TEXT("pin_type_name"), TypeName);
			if (!SubCat.IsEmpty()) Row->SetStringField(TEXT("pin_subcategory_object"), SubCat);
			Row->SetStringField(TEXT("container_type"), Container);
			if (!Tooltip.IsEmpty()) Row->SetStringField(TEXT("tooltip"), Tooltip);
			if (Orphan != 0) Row->SetBoolField(TEXT("is_orphaned"), true);
			Out.Add(MakeShared<FJsonValueObject>(Row));
		}
		Stmt.Destroy();
		return Out;
	}

	/** Read outgoing connections for one node into a JSON array. */
	TArray<TSharedPtr<FJsonValue>> ReadOutgoingConnections(FSQLiteDatabase* RawDB, int64 FaAssetId, const FString& NodeGuid)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		FSQLitePreparedStatement Stmt;
		if (!Stmt.Create(*RawDB, TEXT(
			"SELECT from_pin, to_node_guid, to_pin, is_orphaned "
			"FROM flow_node_connections WHERE fa_asset_id = ? AND from_node_guid = ? "
			"ORDER BY from_pin, to_node_guid")))
		{
			return Out;
		}
		Stmt.SetBindingValueByIndex(1, FaAssetId);
		Stmt.SetBindingValueByIndex(2, NodeGuid);
		while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			FString FromPin, ToGuid, ToPin;
			int64 Orphan = 0;
			Stmt.GetColumnValueByIndex(0, FromPin);
			Stmt.GetColumnValueByIndex(1, ToGuid);
			Stmt.GetColumnValueByIndex(2, ToPin);
			Stmt.GetColumnValueByIndex(3, Orphan);

			auto Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("from_pin"), FromPin);
			Row->SetStringField(TEXT("to_node_guid"), ToGuid);
			Row->SetStringField(TEXT("to_pin"), ToPin);
			if (Orphan != 0) Row->SetBoolField(TEXT("is_orphaned"), true);
			Out.Add(MakeShared<FJsonValueObject>(Row));
		}
		Stmt.Destroy();
		return Out;
	}

	/**
	 * Read all addons under a node and assemble a tree of JSON objects.
	 * Each addon row becomes:
	 *   { id, addon_class, addon_index, depth, data, children: [...recursive...] }
	 */
	TArray<TSharedPtr<FJsonValue>> ReadAddonTreeForNode(FSQLiteDatabase* RawDB, int64 FaAssetId, const FString& NodeGuid)
	{
		// Single SELECT, then assemble in memory by parent_addon_id.
		struct FAddonRow
		{
			int64 Id;
			int64 ParentId; // -1 when NULL
			FString AddonClass;
			int64 AddonIndex;
			int64 Depth;
			FString DataJson;
		};
		TArray<FAddonRow> Rows;

		FSQLitePreparedStatement Stmt;
		if (!Stmt.Create(*RawDB, TEXT(
			"SELECT id, parent_addon_id, addon_class, addon_index, depth, data "
			"FROM flow_node_addons WHERE fa_asset_id = ? AND owner_node_guid = ? "
			"ORDER BY depth, addon_index")))
		{
			return {};
		}
		Stmt.SetBindingValueByIndex(1, FaAssetId);
		Stmt.SetBindingValueByIndex(2, NodeGuid);
		while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			FAddonRow R;
			Stmt.GetColumnValueByIndex(0, R.Id);
			// SQLite returns 0 for NULL via the int64 overload - we re-check with a separate IS NULL helper if needed.
			// Trick: read parent as text then re-parse, or use IsColumnValueNull. Simpler: use IFNULL in SQL.
			int64 ParentRaw = 0;
			Stmt.GetColumnValueByIndex(1, ParentRaw);
			R.ParentId = (ParentRaw > 0) ? ParentRaw : -1;
			Stmt.GetColumnValueByIndex(2, R.AddonClass);
			Stmt.GetColumnValueByIndex(3, R.AddonIndex);
			Stmt.GetColumnValueByIndex(4, R.Depth);
			Stmt.GetColumnValueByIndex(5, R.DataJson);
			Rows.Add(MoveTemp(R));
		}
		Stmt.Destroy();

		// Build JSON objects keyed by id.
		TMap<int64, TSharedPtr<FJsonObject>> NodesById;
		for (const FAddonRow& R : Rows)
		{
			auto Obj = MakeShared<FJsonObject>();
			Obj->SetNumberField(TEXT("id"), R.Id);
			Obj->SetStringField(TEXT("addon_class"), R.AddonClass);
			Obj->SetNumberField(TEXT("addon_index"), R.AddonIndex);
			Obj->SetNumberField(TEXT("depth"), R.Depth);
			TSharedPtr<FJsonValue> DataVal;
			if (!R.DataJson.IsEmpty())
			{
				TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(R.DataJson);
				if (!FJsonSerializer::Deserialize(Reader, DataVal) || !DataVal.IsValid())
				{
					DataVal = MakeShared<FJsonValueNull>();
				}
			}
			else
			{
				DataVal = MakeShared<FJsonValueNull>();
			}
			Obj->SetField(TEXT("data"), DataVal);
			Obj->SetArrayField(TEXT("children"), {});
			NodesById.Add(R.Id, Obj);
		}

		// Link children to parents.
		TArray<TSharedPtr<FJsonValue>> Roots;
		for (const FAddonRow& R : Rows)
		{
			TSharedPtr<FJsonObject> Self = NodesById.FindRef(R.Id);
			if (!Self.IsValid()) continue;

			if (R.ParentId > 0)
			{
				if (TSharedPtr<FJsonObject> Parent = NodesById.FindRef(R.ParentId))
				{
					TArray<TSharedPtr<FJsonValue>> Children = Parent->GetArrayField(TEXT("children"));
					Children.Add(MakeShared<FJsonValueObject>(Self));
					Parent->SetArrayField(TEXT("children"), Children);
				}
			}
			else
			{
				Roots.Add(MakeShared<FJsonValueObject>(Self));
			}
		}

		return Roots;
	}
}

FMonolithActionResult FMonolithFlowActions::ListNodePins(const TSharedPtr<FJsonObject>& Params)
{
	FString Err;
	FSQLiteDatabase* RawDB = GetRawDB(Err);
	if (!RawDB) return FMonolithActionResult::Error(Err);

	FString AssetPath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path is required"));
	}

	int64 RowId = -1, AssetId = -1;
	SelectFlowAssetIds(RawDB, AssetPath, RowId, AssetId);
	if (RowId <= 0)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("No Flow Asset indexed at path '%s'."), *AssetPath));
	}

	FString NodeGuid, Direction, PinTypeName;
	Params->TryGetStringField(TEXT("node_guid"), NodeGuid);
	Params->TryGetStringField(TEXT("pin_direction"), Direction);
	Params->TryGetStringField(TEXT("pin_type_name"), PinTypeName);

	FString SQL = TEXT(
		"SELECT node_guid, pin_direction, pin_index, pin_name, pin_friendly_name, "
		"pin_type_name, pin_subcategory_object, container_type, tooltip, is_orphaned "
		"FROM flow_node_pins WHERE fa_asset_id = ?");
	int32 NextBind = 2;
	if (!NodeGuid.IsEmpty())
	{
		SQL += TEXT(" AND node_guid = ?");
	}
	if (!Direction.IsEmpty() && Direction != TEXT("all"))
	{
		SQL += TEXT(" AND pin_direction = ?");
	}
	if (!PinTypeName.IsEmpty())
	{
		SQL += TEXT(" AND pin_type_name = ?");
	}
	SQL += TEXT(" ORDER BY node_guid, pin_direction, pin_index");

	FSQLitePreparedStatement Stmt;
	if (!Stmt.Create(*RawDB, *SQL))
	{
		return FMonolithActionResult::Error(TEXT("Failed to prepare list_node_pins SQL"));
	}
	Stmt.SetBindingValueByIndex(1, AssetId);
	if (!NodeGuid.IsEmpty()) Stmt.SetBindingValueByIndex(NextBind++, NodeGuid);
	if (!Direction.IsEmpty() && Direction != TEXT("all")) Stmt.SetBindingValueByIndex(NextBind++, Direction);
	if (!PinTypeName.IsEmpty()) Stmt.SetBindingValueByIndex(NextBind++, PinTypeName);

	TArray<TSharedPtr<FJsonValue>> Rows;
	int32 OrphanCount = 0;
	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FString Guid, Dir, PinName, FriendlyName, TypeName, SubCat, Container, Tooltip;
		int64 PinIdx = 0, Orphan = 0;
		Stmt.GetColumnValueByIndex(0, Guid);
		Stmt.GetColumnValueByIndex(1, Dir);
		Stmt.GetColumnValueByIndex(2, PinIdx);
		Stmt.GetColumnValueByIndex(3, PinName);
		Stmt.GetColumnValueByIndex(4, FriendlyName);
		Stmt.GetColumnValueByIndex(5, TypeName);
		Stmt.GetColumnValueByIndex(6, SubCat);
		Stmt.GetColumnValueByIndex(7, Container);
		Stmt.GetColumnValueByIndex(8, Tooltip);
		Stmt.GetColumnValueByIndex(9, Orphan);

		auto Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("node_guid"), Guid);
		Row->SetStringField(TEXT("pin_direction"), Dir);
		Row->SetNumberField(TEXT("pin_index"), PinIdx);
		Row->SetStringField(TEXT("pin_name"), PinName);
		if (!FriendlyName.IsEmpty()) Row->SetStringField(TEXT("pin_friendly_name"), FriendlyName);
		Row->SetStringField(TEXT("pin_type_name"), TypeName);
		if (!SubCat.IsEmpty()) Row->SetStringField(TEXT("pin_subcategory_object"), SubCat);
		Row->SetStringField(TEXT("container_type"), Container);
		if (!Tooltip.IsEmpty()) Row->SetStringField(TEXT("tooltip"), Tooltip);
		if (Orphan != 0) { Row->SetBoolField(TEXT("is_orphaned"), true); ++OrphanCount; }
		Rows.Add(MakeShared<FJsonValueObject>(Row));
	}
	Stmt.Destroy();

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("fa_path"), AssetPath);
	Result->SetArrayField(TEXT("pins"), Rows);
	Result->SetNumberField(TEXT("count"), Rows.Num());
	if (OrphanCount > 0) Result->SetNumberField(TEXT("orphan_count"), OrphanCount);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithFlowActions::ListConnections(const TSharedPtr<FJsonObject>& Params)
{
	FString Err;
	FSQLiteDatabase* RawDB = GetRawDB(Err);
	if (!RawDB) return FMonolithActionResult::Error(Err);

	FString AssetPath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path is required"));
	}

	int64 RowId = -1, AssetId = -1;
	SelectFlowAssetIds(RawDB, AssetPath, RowId, AssetId);
	if (RowId <= 0)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("No Flow Asset indexed at path '%s'."), *AssetPath));
	}

	FString FromGuid, ToGuid;
	Params->TryGetStringField(TEXT("from_node_guid"), FromGuid);
	Params->TryGetStringField(TEXT("to_node_guid"), ToGuid);

	FString SQL = TEXT(
		"SELECT from_node_guid, from_pin, to_node_guid, to_pin, is_orphaned "
		"FROM flow_node_connections WHERE fa_asset_id = ?");
	int32 NextBind = 2;
	if (!FromGuid.IsEmpty()) SQL += TEXT(" AND from_node_guid = ?");
	if (!ToGuid.IsEmpty())   SQL += TEXT(" AND to_node_guid = ?");
	SQL += TEXT(" ORDER BY from_node_guid, from_pin, to_node_guid");

	FSQLitePreparedStatement Stmt;
	if (!Stmt.Create(*RawDB, *SQL))
	{
		return FMonolithActionResult::Error(TEXT("Failed to prepare list_connections SQL"));
	}
	Stmt.SetBindingValueByIndex(1, AssetId);
	if (!FromGuid.IsEmpty()) Stmt.SetBindingValueByIndex(NextBind++, FromGuid);
	if (!ToGuid.IsEmpty())   Stmt.SetBindingValueByIndex(NextBind++, ToGuid);

	TArray<TSharedPtr<FJsonValue>> Rows;
	int32 OrphanCount = 0;
	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FString FromG, FromP, ToG, ToP;
		int64 Orphan = 0;
		Stmt.GetColumnValueByIndex(0, FromG);
		Stmt.GetColumnValueByIndex(1, FromP);
		Stmt.GetColumnValueByIndex(2, ToG);
		Stmt.GetColumnValueByIndex(3, ToP);
		Stmt.GetColumnValueByIndex(4, Orphan);

		auto Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("from_node_guid"), FromG);
		Row->SetStringField(TEXT("from_pin"), FromP);
		Row->SetStringField(TEXT("to_node_guid"), ToG);
		Row->SetStringField(TEXT("to_pin"), ToP);
		if (Orphan != 0) { Row->SetBoolField(TEXT("is_orphaned"), true); ++OrphanCount; }
		Rows.Add(MakeShared<FJsonValueObject>(Row));
	}
	Stmt.Destroy();

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("fa_path"), AssetPath);
	Result->SetArrayField(TEXT("connections"), Rows);
	Result->SetNumberField(TEXT("count"), Rows.Num());
	if (OrphanCount > 0) Result->SetNumberField(TEXT("orphan_count"), OrphanCount);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithFlowActions::ListAddons(const TSharedPtr<FJsonObject>& Params)
{
	FString Err;
	FSQLiteDatabase* RawDB = GetRawDB(Err);
	if (!RawDB) return FMonolithActionResult::Error(Err);

	FString AssetPath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path is required"));
	}

	int64 RowId = -1, AssetId = -1;
	SelectFlowAssetIds(RawDB, AssetPath, RowId, AssetId);
	if (RowId <= 0)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("No Flow Asset indexed at path '%s'."), *AssetPath));
	}

	FString NodeGuid;
	Params->TryGetStringField(TEXT("node_guid"), NodeGuid);

	FString SQL = TEXT(
		"SELECT id, owner_node_guid, parent_addon_id, addon_class, addon_index, depth, data "
		"FROM flow_node_addons WHERE fa_asset_id = ?");
	if (!NodeGuid.IsEmpty()) SQL += TEXT(" AND owner_node_guid = ?");
	SQL += TEXT(" ORDER BY owner_node_guid, depth, addon_index");

	FSQLitePreparedStatement Stmt;
	if (!Stmt.Create(*RawDB, *SQL))
	{
		return FMonolithActionResult::Error(TEXT("Failed to prepare list_addons SQL"));
	}
	Stmt.SetBindingValueByIndex(1, AssetId);
	if (!NodeGuid.IsEmpty()) Stmt.SetBindingValueByIndex(2, NodeGuid);

	TArray<TSharedPtr<FJsonValue>> Rows;
	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		int64 Id = 0, ParentId = 0, AddonIdx = 0, Depth = 0;
		FString Owner, Cls, DataJson;
		Stmt.GetColumnValueByIndex(0, Id);
		Stmt.GetColumnValueByIndex(1, Owner);
		Stmt.GetColumnValueByIndex(2, ParentId);
		Stmt.GetColumnValueByIndex(3, Cls);
		Stmt.GetColumnValueByIndex(4, AddonIdx);
		Stmt.GetColumnValueByIndex(5, Depth);
		Stmt.GetColumnValueByIndex(6, DataJson);

		auto Row = MakeShared<FJsonObject>();
		Row->SetNumberField(TEXT("id"), Id);
		Row->SetStringField(TEXT("owner_node_guid"), Owner);
		if (ParentId > 0) Row->SetNumberField(TEXT("parent_addon_id"), ParentId);
		Row->SetStringField(TEXT("addon_class"), Cls);
		Row->SetNumberField(TEXT("addon_index"), AddonIdx);
		Row->SetNumberField(TEXT("depth"), Depth);
		Row->SetField(TEXT("data"), TryParseJsonValue(DataJson));
		Rows.Add(MakeShared<FJsonValueObject>(Row));
	}
	Stmt.Destroy();

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("fa_path"), AssetPath);
	Result->SetArrayField(TEXT("addons"), Rows);
	Result->SetNumberField(TEXT("count"), Rows.Num());
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithFlowActions::GetNodeInfo(const TSharedPtr<FJsonObject>& Params)
{
	FString Err;
	FSQLiteDatabase* RawDB = GetRawDB(Err);
	if (!RawDB) return FMonolithActionResult::Error(Err);

	FString AssetPath, NodeGuid;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path is required"));
	}
	if (!Params->TryGetStringField(TEXT("node_guid"), NodeGuid) || NodeGuid.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("node_guid is required"));
	}

	int64 RowId = -1, AssetId = -1;
	SelectFlowAssetIds(RawDB, AssetPath, RowId, AssetId);
	if (RowId <= 0)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("No Flow Asset indexed at path '%s'."), *AssetPath));
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("fa_path"), AssetPath);
	Result->SetStringField(TEXT("node_guid"), NodeGuid);

	bool bFound = false;
	{
		FSQLitePreparedStatement Stmt;
		if (!Stmt.Create(*RawDB, TEXT(
			"SELECT node_class, display_name, is_blueprint, blueprint_path, "
			"addon_count, input_pin_count, output_pin_count, data "
			"FROM flow_nodes WHERE fa_asset_id = ? AND node_guid = ?")))
		{
			return FMonolithActionResult::Error(TEXT("Failed to prepare get_node_info SQL"));
		}
		Stmt.SetBindingValueByIndex(1, AssetId);
		Stmt.SetBindingValueByIndex(2, NodeGuid);
		if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			bFound = true;
			FString Cls, Display, BpPath, DataJson;
			int64 IsBp = 0, AddonN = 0, InN = 0, OutN = 0;
			Stmt.GetColumnValueByIndex(0, Cls);
			Stmt.GetColumnValueByIndex(1, Display);
			Stmt.GetColumnValueByIndex(2, IsBp);
			Stmt.GetColumnValueByIndex(3, BpPath);
			Stmt.GetColumnValueByIndex(4, AddonN);
			Stmt.GetColumnValueByIndex(5, InN);
			Stmt.GetColumnValueByIndex(6, OutN);
			Stmt.GetColumnValueByIndex(7, DataJson);

			Result->SetStringField(TEXT("node_class"), Cls);
			if (!Display.IsEmpty()) Result->SetStringField(TEXT("display_name"), Display);
			Result->SetBoolField(TEXT("is_blueprint"), IsBp != 0);
			if (!BpPath.IsEmpty()) Result->SetStringField(TEXT("blueprint_path"), BpPath);
			Result->SetNumberField(TEXT("addon_count"), AddonN);
			Result->SetNumberField(TEXT("input_pin_count"), InN);
			Result->SetNumberField(TEXT("output_pin_count"), OutN);
			Result->SetField(TEXT("data"), TryParseJsonValue(DataJson));
		}
		Stmt.Destroy();
	}

	if (!bFound)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("No node with guid '%s' in '%s'."), *NodeGuid, *AssetPath));
	}

	Result->SetArrayField(TEXT("pins"), ReadPinsForNode(RawDB, AssetId, NodeGuid));
	Result->SetArrayField(TEXT("outgoing_connections"), ReadOutgoingConnections(RawDB, AssetId, NodeGuid));
	Result->SetArrayField(TEXT("addons"), ReadAddonTreeForNode(RawDB, AssetId, NodeGuid));

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithFlowActions::ListCustomEvents(const TSharedPtr<FJsonObject>& Params)
{
	FString Err;
	FSQLiteDatabase* RawDB = GetRawDB(Err);
	if (!RawDB) return FMonolithActionResult::Error(Err);

	FString AssetPath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path is required"));
	}

	int64 RowId = -1, AssetId = -1;
	SelectFlowAssetIds(RawDB, AssetPath, RowId, AssetId);
	if (RowId <= 0)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("No Flow Asset indexed at path '%s'."), *AssetPath));
	}

	FString Kind;
	Params->TryGetStringField(TEXT("kind"), Kind);

	FString SQL = TEXT("SELECT kind, event_name FROM flow_custom_events WHERE fa_asset_id = ?");
	if (!Kind.IsEmpty() && Kind != TEXT("all"))
	{
		SQL += TEXT(" AND kind = ?");
	}
	SQL += TEXT(" ORDER BY kind, event_name");

	FSQLitePreparedStatement Stmt;
	if (!Stmt.Create(*RawDB, *SQL))
	{
		return FMonolithActionResult::Error(TEXT("Failed to prepare list_custom_events SQL"));
	}
	Stmt.SetBindingValueByIndex(1, AssetId);
	if (!Kind.IsEmpty() && Kind != TEXT("all"))
	{
		Stmt.SetBindingValueByIndex(2, Kind);
	}

	TArray<TSharedPtr<FJsonValue>> Inputs, Outputs;
	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FString K, Name;
		Stmt.GetColumnValueByIndex(0, K);
		Stmt.GetColumnValueByIndex(1, Name);
		auto Val = MakeShared<FJsonValueString>(Name);
		if (K == TEXT("input")) Inputs.Add(Val); else Outputs.Add(Val);
	}
	Stmt.Destroy();

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("fa_path"), AssetPath);
	Result->SetArrayField(TEXT("custom_inputs"), Inputs);
	Result->SetArrayField(TEXT("custom_outputs"), Outputs);
	Result->SetNumberField(TEXT("count"), Inputs.Num() + Outputs.Num());
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithFlowActions::FindSubgraphCallers(const TSharedPtr<FJsonObject>& Params)
{
	FString Err;
	FSQLiteDatabase* RawDB = GetRawDB(Err);
	if (!RawDB) return FMonolithActionResult::Error(Err);

	FString TargetPath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("target_asset_path"), TargetPath) || TargetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("target_asset_path is required"));
	}
	TargetPath = ToFlowObjectPath(TargetPath);

	FString PathFilter;
	Params->TryGetStringField(TEXT("asset_path_filter"), PathFilter);

	FString SQL = TEXT(
		"SELECT host_fa_path, host_node_guid, target_params_path "
		"FROM flow_subgraph_refs WHERE target_fa_path = ?");
	int32 NextBind = 2;
	if (!PathFilter.IsEmpty())
	{
		const FString Like = GlobToLike(PathFilter);
		SQL += FString::Printf(TEXT(" AND host_fa_path LIKE '%s'"), *Like);
	}
	SQL += TEXT(" ORDER BY host_fa_path, host_node_guid");

	FSQLitePreparedStatement Stmt;
	if (!Stmt.Create(*RawDB, *SQL))
	{
		return FMonolithActionResult::Error(TEXT("Failed to prepare find_subgraph_callers SQL"));
	}
	Stmt.SetBindingValueByIndex(1, TargetPath);
	(void)NextBind;

	TArray<TSharedPtr<FJsonValue>> Rows;
	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FString HostPath, HostGuid, ParamsPath;
		Stmt.GetColumnValueByIndex(0, HostPath);
		Stmt.GetColumnValueByIndex(1, HostGuid);
		Stmt.GetColumnValueByIndex(2, ParamsPath);

		auto Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("host_fa_path"), HostPath);
		Row->SetStringField(TEXT("host_node_guid"), HostGuid);
		if (!ParamsPath.IsEmpty()) Row->SetStringField(TEXT("target_params_path"), ParamsPath);
		Rows.Add(MakeShared<FJsonValueObject>(Row));
	}
	Stmt.Destroy();

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("target_asset_path"), TargetPath);
	Result->SetArrayField(TEXT("callers"), Rows);
	Result->SetNumberField(TEXT("count"), Rows.Num());
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithFlowActions::FindNodeClassUsages(const TSharedPtr<FJsonObject>& Params)
{
	FString Err;
	FSQLiteDatabase* RawDB = GetRawDB(Err);
	if (!RawDB) return FMonolithActionResult::Error(Err);

	FString NodeClass;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("node_class"), NodeClass) || NodeClass.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("node_class is required"));
	}

	FString PathFilter;
	Params->TryGetStringField(TEXT("asset_path_filter"), PathFilter);

	// Join flow_nodes with flow_assets to get host fa_path per matching node.
	FString SQL = TEXT(
		"SELECT fa.fa_path, n.node_guid, n.display_name "
		"FROM flow_nodes n "
		"JOIN flow_assets fa ON fa.fa_asset_id = n.fa_asset_id "
		"WHERE n.node_class = ?");
	if (!PathFilter.IsEmpty())
	{
		const FString Like = GlobToLike(PathFilter);
		SQL += FString::Printf(TEXT(" AND fa.fa_path LIKE '%s'"), *Like);
	}
	SQL += TEXT(" ORDER BY fa.fa_path, n.node_guid");

	FSQLitePreparedStatement Stmt;
	if (!Stmt.Create(*RawDB, *SQL))
	{
		return FMonolithActionResult::Error(TEXT("Failed to prepare find_node_class_usages SQL"));
	}
	Stmt.SetBindingValueByIndex(1, NodeClass);

	// Group on the way out by fa_path.
	TMap<FString, TArray<TSharedPtr<FJsonValue>>> NodesByFa;
	int32 TotalNodes = 0;
	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FString FaPath, Guid, Display;
		Stmt.GetColumnValueByIndex(0, FaPath);
		Stmt.GetColumnValueByIndex(1, Guid);
		Stmt.GetColumnValueByIndex(2, Display);

		auto NodeObj = MakeShared<FJsonObject>();
		NodeObj->SetStringField(TEXT("node_guid"), Guid);
		if (!Display.IsEmpty()) NodeObj->SetStringField(TEXT("display_name"), Display);

		NodesByFa.FindOrAdd(FaPath).Add(MakeShared<FJsonValueObject>(NodeObj));
		++TotalNodes;
	}
	Stmt.Destroy();

	TArray<TSharedPtr<FJsonValue>> Hosts;
	for (const TPair<FString, TArray<TSharedPtr<FJsonValue>>>& Pair : NodesByFa)
	{
		auto HostObj = MakeShared<FJsonObject>();
		HostObj->SetStringField(TEXT("fa_path"), Pair.Key);
		HostObj->SetArrayField(TEXT("nodes"), Pair.Value);
		HostObj->SetNumberField(TEXT("node_count"), Pair.Value.Num());
		Hosts.Add(MakeShared<FJsonValueObject>(HostObj));
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("node_class"), NodeClass);
	Result->SetArrayField(TEXT("hosts"), Hosts);
	Result->SetNumberField(TEXT("host_count"), Hosts.Num());
	Result->SetNumberField(TEXT("total_node_count"), TotalNodes);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithFlowActions::FindPinsByType(const TSharedPtr<FJsonObject>& Params)
{
	FString Err;
	FSQLiteDatabase* RawDB = GetRawDB(Err);
	if (!RawDB) return FMonolithActionResult::Error(Err);

	FString PinTypeName;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("pin_type_name"), PinTypeName) || PinTypeName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("pin_type_name is required"));
	}

	FString SubCat, Direction, PathFilter;
	Params->TryGetStringField(TEXT("pin_subcategory_object"), SubCat);
	Params->TryGetStringField(TEXT("pin_direction"), Direction);
	Params->TryGetStringField(TEXT("asset_path_filter"), PathFilter);

	FString SQL = TEXT(
		"SELECT fa.fa_path, p.node_guid, p.pin_direction, p.pin_index, p.pin_name, "
		"p.pin_type_name, p.pin_subcategory_object, p.container_type, p.is_orphaned "
		"FROM flow_node_pins p "
		"JOIN flow_assets fa ON fa.fa_asset_id = p.fa_asset_id "
		"WHERE p.pin_type_name = ?");
	int32 NextBind = 2;
	if (!SubCat.IsEmpty())
	{
		SQL += TEXT(" AND p.pin_subcategory_object = ?");
	}
	if (!Direction.IsEmpty() && Direction != TEXT("all"))
	{
		SQL += TEXT(" AND p.pin_direction = ?");
	}
	if (!PathFilter.IsEmpty())
	{
		const FString Like = GlobToLike(PathFilter);
		SQL += FString::Printf(TEXT(" AND fa.fa_path LIKE '%s'"), *Like);
	}
	SQL += TEXT(" ORDER BY fa.fa_path, p.node_guid, p.pin_direction, p.pin_index");

	FSQLitePreparedStatement Stmt;
	if (!Stmt.Create(*RawDB, *SQL))
	{
		return FMonolithActionResult::Error(TEXT("Failed to prepare find_pins_by_type SQL"));
	}
	Stmt.SetBindingValueByIndex(1, PinTypeName);
	if (!SubCat.IsEmpty()) Stmt.SetBindingValueByIndex(NextBind++, SubCat);
	if (!Direction.IsEmpty() && Direction != TEXT("all")) Stmt.SetBindingValueByIndex(NextBind++, Direction);

	TMap<FString, TArray<TSharedPtr<FJsonValue>>> PinsByFa;
	int32 TotalPins = 0;
	int32 OrphanCount = 0;
	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FString FaPath, Guid, Dir, PinName, TypeName, SubCatRow, Container;
		int64 PinIdx = 0, Orphan = 0;
		Stmt.GetColumnValueByIndex(0, FaPath);
		Stmt.GetColumnValueByIndex(1, Guid);
		Stmt.GetColumnValueByIndex(2, Dir);
		Stmt.GetColumnValueByIndex(3, PinIdx);
		Stmt.GetColumnValueByIndex(4, PinName);
		Stmt.GetColumnValueByIndex(5, TypeName);
		Stmt.GetColumnValueByIndex(6, SubCatRow);
		Stmt.GetColumnValueByIndex(7, Container);
		Stmt.GetColumnValueByIndex(8, Orphan);

		auto Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("node_guid"), Guid);
		Row->SetStringField(TEXT("pin_direction"), Dir);
		Row->SetNumberField(TEXT("pin_index"), PinIdx);
		Row->SetStringField(TEXT("pin_name"), PinName);
		Row->SetStringField(TEXT("pin_type_name"), TypeName);
		if (!SubCatRow.IsEmpty()) Row->SetStringField(TEXT("pin_subcategory_object"), SubCatRow);
		Row->SetStringField(TEXT("container_type"), Container);
		if (Orphan != 0) { Row->SetBoolField(TEXT("is_orphaned"), true); ++OrphanCount; }

		PinsByFa.FindOrAdd(FaPath).Add(MakeShared<FJsonValueObject>(Row));
		++TotalPins;
	}
	Stmt.Destroy();

	TArray<TSharedPtr<FJsonValue>> Hosts;
	for (const TPair<FString, TArray<TSharedPtr<FJsonValue>>>& Pair : PinsByFa)
	{
		auto HostObj = MakeShared<FJsonObject>();
		HostObj->SetStringField(TEXT("fa_path"), Pair.Key);
		HostObj->SetArrayField(TEXT("pins"), Pair.Value);
		HostObj->SetNumberField(TEXT("pin_count"), Pair.Value.Num());
		Hosts.Add(MakeShared<FJsonValueObject>(HostObj));
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("pin_type_name"), PinTypeName);
	if (!SubCat.IsEmpty()) Result->SetStringField(TEXT("pin_subcategory_object"), SubCat);
	Result->SetArrayField(TEXT("hosts"), Hosts);
	Result->SetNumberField(TEXT("host_count"), Hosts.Num());
	Result->SetNumberField(TEXT("total_pin_count"), TotalPins);
	if (OrphanCount > 0) Result->SetNumberField(TEXT("orphan_count"), OrphanCount);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithFlowActions::FindNodesByProperty(const TSharedPtr<FJsonObject>& Params)
{
	FString Err;
	FSQLiteDatabase* RawDB = GetRawDB(Err);
	if (!RawDB) return FMonolithActionResult::Error(Err);

	FString ValueSub;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("value_substring"), ValueSub) || ValueSub.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("value_substring is required"));
	}

	FString PropName, NodeClassFilter, PathFilter;
	Params->TryGetStringField(TEXT("property_name"), PropName);
	Params->TryGetStringField(TEXT("node_class"), NodeClassFilter);
	Params->TryGetStringField(TEXT("asset_path_filter"), PathFilter);

	// Escape SQL LIKE wildcards in user input - we want literal substring match. ESCAPE char is backslash.
	auto EscapeLike = [](const FString& In) -> FString
	{
		return In
			.Replace(TEXT("\\"), TEXT("\\\\"))
			.Replace(TEXT("%"),  TEXT("\\%"))
			.Replace(TEXT("_"),  TEXT("\\_"));
	};

	const FString EscValue = EscapeLike(ValueSub);
	FString DataPattern;
	if (!PropName.IsEmpty())
	{
		const FString EscProp = EscapeLike(PropName);
		DataPattern = FString::Printf(TEXT("%%\"%s\"%%%s%%"), *EscProp, *EscValue);
	}
	else
	{
		DataPattern = FString::Printf(TEXT("%%%s%%"), *EscValue);
	}

	FString SQL = TEXT(
		"SELECT fa.fa_path, n.node_guid, n.node_class, n.display_name, n.data "
		"FROM flow_nodes n "
		"JOIN flow_assets fa ON fa.fa_asset_id = n.fa_asset_id "
		"WHERE n.data LIKE ? ESCAPE '\\'");
	int32 NextBind = 2;
	if (!NodeClassFilter.IsEmpty())
	{
		SQL += TEXT(" AND n.node_class = ?");
	}
	if (!PathFilter.IsEmpty())
	{
		const FString FaLike = GlobToLike(PathFilter);
		SQL += FString::Printf(TEXT(" AND fa.fa_path LIKE '%s'"), *FaLike);
	}
	SQL += TEXT(" ORDER BY fa.fa_path, n.node_guid");

	FSQLitePreparedStatement Stmt;
	if (!Stmt.Create(*RawDB, *SQL))
	{
		return FMonolithActionResult::Error(TEXT("Failed to prepare find_nodes_by_property SQL"));
	}
	Stmt.SetBindingValueByIndex(1, DataPattern);
	if (!NodeClassFilter.IsEmpty()) Stmt.SetBindingValueByIndex(NextBind++, NodeClassFilter);

	TMap<FString, TArray<TSharedPtr<FJsonValue>>> NodesByFa;
	int32 TotalNodes = 0;
	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FString FaPath, Guid, Cls, Display, DataJson;
		Stmt.GetColumnValueByIndex(0, FaPath);
		Stmt.GetColumnValueByIndex(1, Guid);
		Stmt.GetColumnValueByIndex(2, Cls);
		Stmt.GetColumnValueByIndex(3, Display);
		Stmt.GetColumnValueByIndex(4, DataJson);

		auto NodeObj = MakeShared<FJsonObject>();
		NodeObj->SetStringField(TEXT("node_guid"), Guid);
		NodeObj->SetStringField(TEXT("node_class"), Cls);
		if (!Display.IsEmpty()) NodeObj->SetStringField(TEXT("display_name"), Display);
		NodeObj->SetField(TEXT("data"), TryParseJsonValue(DataJson));

		NodesByFa.FindOrAdd(FaPath).Add(MakeShared<FJsonValueObject>(NodeObj));
		++TotalNodes;
	}
	Stmt.Destroy();

	TArray<TSharedPtr<FJsonValue>> Hosts;
	for (const TPair<FString, TArray<TSharedPtr<FJsonValue>>>& Pair : NodesByFa)
	{
		auto HostObj = MakeShared<FJsonObject>();
		HostObj->SetStringField(TEXT("fa_path"), Pair.Key);
		HostObj->SetArrayField(TEXT("nodes"), Pair.Value);
		HostObj->SetNumberField(TEXT("node_count"), Pair.Value.Num());
		Hosts.Add(MakeShared<FJsonValueObject>(HostObj));
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("value_substring"), ValueSub);
	if (!PropName.IsEmpty()) Result->SetStringField(TEXT("property_name"), PropName);
	Result->SetArrayField(TEXT("hosts"), Hosts);
	Result->SetNumberField(TEXT("host_count"), Hosts.Num());
	Result->SetNumberField(TEXT("total_node_count"), TotalNodes);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithFlowActions::ListNodeClasses(const TSharedPtr<FJsonObject>& Params)
{
	FString Err;
	FSQLiteDatabase* RawDB = GetRawDB(Err);
	if (!RawDB) return FMonolithActionResult::Error(Err);

	FString Kind, Source, PathFilter;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("kind"), Kind);
		Params->TryGetStringField(TEXT("source"), Source);
		Params->TryGetStringField(TEXT("class_path_filter"), PathFilter);
	}

	FString SQL = TEXT(
		"SELECT class_path, bp_path, kind, parent_class_path, "
		"display_name, category, description, is_native, is_abstract "
		"FROM flow_node_classes WHERE 1=1");
	int32 NextBind = 1;
	if (!Kind.IsEmpty() && Kind != TEXT("all"))
	{
		SQL += TEXT(" AND kind = ?");
	}
	if (!Source.IsEmpty() && Source != TEXT("all"))
	{
		if (Source == TEXT("blueprint"))      SQL += TEXT(" AND is_native = 0");
		else if (Source == TEXT("native"))    SQL += TEXT(" AND is_native = 1");
		else return FMonolithActionResult::Error(TEXT("source must be \"blueprint\", \"native\", or \"all\""));
	}
	if (!PathFilter.IsEmpty())
	{
		const FString Like = GlobToLike(PathFilter);
		SQL += FString::Printf(TEXT(" AND class_path LIKE '%s'"), *Like);
	}
	SQL += TEXT(" ORDER BY is_native, kind, class_path");

	FSQLitePreparedStatement Stmt;
	if (!Stmt.Create(*RawDB, *SQL))
	{
		return FMonolithActionResult::Error(TEXT("Failed to prepare list_node_classes SQL"));
	}
	if (!Kind.IsEmpty() && Kind != TEXT("all"))
	{
		Stmt.SetBindingValueByIndex(NextBind++, Kind);
	}

	TArray<TSharedPtr<FJsonValue>> Rows;
	int32 NativeCount = 0;
	int32 BpCount = 0;
	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FString ClassPath, BpPath, K, ParentCls, Display, Category, Desc;
		int64 IsNative = 0, IsAbstract = 0;
		Stmt.GetColumnValueByIndex(0, ClassPath);
		Stmt.GetColumnValueByIndex(1, BpPath);
		Stmt.GetColumnValueByIndex(2, K);
		Stmt.GetColumnValueByIndex(3, ParentCls);
		Stmt.GetColumnValueByIndex(4, Display);
		Stmt.GetColumnValueByIndex(5, Category);
		Stmt.GetColumnValueByIndex(6, Desc);
		Stmt.GetColumnValueByIndex(7, IsNative);
		Stmt.GetColumnValueByIndex(8, IsAbstract);

		auto Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("class_path"), ClassPath);
		if (!BpPath.IsEmpty()) Row->SetStringField(TEXT("bp_path"), BpPath);
		Row->SetStringField(TEXT("kind"), K);
		if (!ParentCls.IsEmpty()) Row->SetStringField(TEXT("parent_class_path"), ParentCls);
		if (!Display.IsEmpty()) Row->SetStringField(TEXT("display_name"), Display);
		if (!Category.IsEmpty()) Row->SetStringField(TEXT("category"), Category);
		if (!Desc.IsEmpty()) Row->SetStringField(TEXT("description"), Desc);
		Row->SetBoolField(TEXT("is_native"), IsNative != 0);
		Row->SetBoolField(TEXT("is_abstract"), IsAbstract != 0);
		Rows.Add(MakeShared<FJsonValueObject>(Row));

		if (IsNative != 0) ++NativeCount; else ++BpCount;
	}
	Stmt.Destroy();

	auto Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("classes"), Rows);
	Result->SetNumberField(TEXT("count"), Rows.Num());
	Result->SetNumberField(TEXT("native_count"), NativeCount);
	Result->SetNumberField(TEXT("blueprint_count"), BpCount);
	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// walk_execution
// ============================================================================

FMonolithActionResult FMonolithFlowActions::WalkExecution(const TSharedPtr<FJsonObject>& Params)
{
	FString Err;
	FSQLiteDatabase* RawDB = GetRawDB(Err);
	if (!RawDB) return FMonolithActionResult::Error(Err);

	FString AssetPath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path is required"));
	}

	int64 RowId = -1, AssetId = -1;
	SelectFlowAssetIds(RawDB, AssetPath, RowId, AssetId);
	if (RowId <= 0)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("No Flow Asset indexed at path '%s'."), *AssetPath));
	}

	FString StartGuid;
	Params->TryGetStringField(TEXT("start_node_guid"), StartGuid);

	// Node identity map: guid -> (class, display)
	struct FNodeRow { FString Class; FString Display; };
	TMap<FString, FNodeRow> NodeRows;
	{
		FSQLitePreparedStatement Stmt;
		if (!Stmt.Create(*RawDB, TEXT(
			"SELECT node_guid, node_class, display_name FROM flow_nodes WHERE fa_asset_id = ?")))
		{
			return FMonolithActionResult::Error(TEXT("Failed to prepare walk_execution nodes SQL"));
		}
		Stmt.SetBindingValueByIndex(1, AssetId);
		while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			FString Guid, Cls, Disp;
			Stmt.GetColumnValueByIndex(0, Guid);
			Stmt.GetColumnValueByIndex(1, Cls);
			Stmt.GetColumnValueByIndex(2, Disp);
			NodeRows.Add(Guid, FNodeRow{Cls, Disp});
		}
		Stmt.Destroy();
	}

	// Adjacency over non-orphaned edges.
	TMultiMap<FString, FString> Adjacency;
	{
		FSQLitePreparedStatement Stmt;
		if (!Stmt.Create(*RawDB, TEXT(
			"SELECT from_node_guid, to_node_guid FROM flow_node_connections "
			"WHERE fa_asset_id = ? AND is_orphaned = 0")))
		{
			return FMonolithActionResult::Error(TEXT("Failed to prepare walk_execution edges SQL"));
		}
		Stmt.SetBindingValueByIndex(1, AssetId);
		while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			FString FromG, ToG;
			Stmt.GetColumnValueByIndex(0, FromG);
			Stmt.GetColumnValueByIndex(1, ToG);
			Adjacency.Add(FromG, ToG);
		}
		Stmt.Destroy();
	}

	// Entry set.
	TArray<FString> Frontier;
	if (!StartGuid.IsEmpty())
	{
		if (!NodeRows.Contains(StartGuid))
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("start_node_guid '%s' not found in '%s'."), *StartGuid, *AssetPath));
		}
		Frontier.Add(StartGuid);
	}
	else
	{
		for (const TPair<FString, FNodeRow>& Pair : NodeRows)
		{
			if (Pair.Value.Class == TEXT("/Script/Flow.FlowNode_Start") ||
				Pair.Value.Class == TEXT("/Script/Flow.FlowNode_CustomInput"))
			{
				Frontier.Add(Pair.Key);
			}
		}
		if (Frontier.IsEmpty())
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("No FlowNode_Start / FlowNode_CustomInput entry nodes in '%s' - pass start_node_guid."), *AssetPath));
		}
	}

	// BFS with depth.
	TMap<FString, int32> Depth;
	for (const FString& Entry : Frontier) Depth.Add(Entry, 0);
	int32 Cursor = 0;
	while (Cursor < Frontier.Num())
	{
		const FString Current = Frontier[Cursor++];
		const int32 NextDepth = Depth[Current] + 1;
		TArray<FString> Targets;
		Adjacency.MultiFind(Current, Targets);
		for (const FString& Target : Targets)
		{
			if (!Depth.Contains(Target) && NodeRows.Contains(Target))
			{
				Depth.Add(Target, NextDepth);
				Frontier.Add(Target);
			}
		}
	}

	TArray<TSharedPtr<FJsonValue>> ReachedRows, UnreachableRows, TerminalRows;
	for (const FString& Guid : Frontier)
	{
		const FNodeRow& Info = NodeRows[Guid];
		auto Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("node_guid"), Guid);
		Row->SetStringField(TEXT("node_class"), Info.Class);
		Row->SetStringField(TEXT("display_name"), Info.Display);
		Row->SetNumberField(TEXT("depth"), Depth[Guid]);
		ReachedRows.Add(MakeShared<FJsonValueObject>(Row));

		if (!Adjacency.Contains(Guid))
		{
			auto TermRow = MakeShared<FJsonObject>();
			TermRow->SetStringField(TEXT("node_guid"), Guid);
			TermRow->SetStringField(TEXT("node_class"), Info.Class);
			TermRow->SetStringField(TEXT("display_name"), Info.Display);
			TerminalRows.Add(MakeShared<FJsonValueObject>(TermRow));
		}
	}
	for (const TPair<FString, FNodeRow>& Pair : NodeRows)
	{
		if (Depth.Contains(Pair.Key)) continue;
		auto Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("node_guid"), Pair.Key);
		Row->SetStringField(TEXT("node_class"), Pair.Value.Class);
		Row->SetStringField(TEXT("display_name"), Pair.Value.Display);
		UnreachableRows.Add(MakeShared<FJsonValueObject>(Row));
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("fa_path"), AssetPath);
	Result->SetArrayField(TEXT("reached"), ReachedRows);
	Result->SetArrayField(TEXT("unreachable"), UnreachableRows);
	Result->SetArrayField(TEXT("terminal"), TerminalRows);
	Result->SetNumberField(TEXT("reached_count"), ReachedRows.Num());
	Result->SetNumberField(TEXT("unreachable_count"), UnreachableRows.Num());
	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// export_graph
// ============================================================================

FMonolithActionResult FMonolithFlowActions::ExportGraph(const TSharedPtr<FJsonObject>& Params)
{
	FString Err;
	FSQLiteDatabase* RawDB = GetRawDB(Err);
	if (!RawDB) return FMonolithActionResult::Error(Err);

	FString AssetPath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path is required"));
	}
	bool bIncludeData = false;
	Params->TryGetBoolField(TEXT("include_data"), bIncludeData);

	int64 RowId = -1, AssetId = -1;
	SelectFlowAssetIds(RawDB, AssetPath, RowId, AssetId);
	if (RowId <= 0)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("No Flow Asset indexed at path '%s'."), *AssetPath));
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("fa_path"), AssetPath);

	// Asset header row.
	{
		FSQLitePreparedStatement Stmt;
		if (Stmt.Create(*RawDB, TEXT(
			"SELECT name, asset_class, expected_owner_class, world_bound, node_count, addon_count, subgraph_count "
			"FROM flow_assets WHERE id = ?")))
		{
			Stmt.SetBindingValueByIndex(1, RowId);
			if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
			{
				FString Name, Cls, Owner;
				int64 WorldBound = 0, NodeCount = 0, AddonCount = 0, SubCount = 0;
				Stmt.GetColumnValueByIndex(0, Name);
				Stmt.GetColumnValueByIndex(1, Cls);
				Stmt.GetColumnValueByIndex(2, Owner);
				Stmt.GetColumnValueByIndex(3, WorldBound);
				Stmt.GetColumnValueByIndex(4, NodeCount);
				Stmt.GetColumnValueByIndex(5, AddonCount);
				Stmt.GetColumnValueByIndex(6, SubCount);
				Result->SetStringField(TEXT("name"), Name);
				Result->SetStringField(TEXT("asset_class"), Cls);
				if (!Owner.IsEmpty()) Result->SetStringField(TEXT("expected_owner_class"), Owner);
				Result->SetBoolField(TEXT("world_bound"), WorldBound != 0);
				Result->SetNumberField(TEXT("node_count"), NodeCount);
				Result->SetNumberField(TEXT("addon_count"), AddonCount);
				Result->SetNumberField(TEXT("subgraph_count"), SubCount);
			}
			Stmt.Destroy();
		}
	}

	// Nodes.
	{
		TArray<TSharedPtr<FJsonValue>> Rows;
		FSQLitePreparedStatement Stmt;
		if (!Stmt.Create(*RawDB, TEXT(
			"SELECT node_guid, node_class, display_name, input_pin_count, output_pin_count, addon_count, pos_x, pos_y, data "
			"FROM flow_nodes WHERE fa_asset_id = ? ORDER BY pos_x, pos_y")))
		{
			return FMonolithActionResult::Error(TEXT("Failed to prepare export_graph nodes SQL"));
		}
		Stmt.SetBindingValueByIndex(1, AssetId);
		while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			FString Guid, Cls, Disp, DataJson;
			int64 InPins = 0, OutPins = 0, Addons = 0, PosX = 0, PosY = 0;
			Stmt.GetColumnValueByIndex(0, Guid);
			Stmt.GetColumnValueByIndex(1, Cls);
			Stmt.GetColumnValueByIndex(2, Disp);
			Stmt.GetColumnValueByIndex(3, InPins);
			Stmt.GetColumnValueByIndex(4, OutPins);
			Stmt.GetColumnValueByIndex(5, Addons);
			Stmt.GetColumnValueByIndex(6, PosX);
			Stmt.GetColumnValueByIndex(7, PosY);
			Stmt.GetColumnValueByIndex(8, DataJson);

			auto Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("node_guid"), Guid);
			Row->SetStringField(TEXT("node_class"), Cls);
			Row->SetStringField(TEXT("display_name"), Disp);
			Row->SetNumberField(TEXT("in_pins"), InPins);
			Row->SetNumberField(TEXT("out_pins"), OutPins);
			if (Addons > 0) Row->SetNumberField(TEXT("addons"), Addons);
			Row->SetNumberField(TEXT("pos_x"), PosX);
			Row->SetNumberField(TEXT("pos_y"), PosY);
			if (bIncludeData)
			{
				Row->SetField(TEXT("data"), TryParseJsonValue(DataJson));
			}
			Rows.Add(MakeShared<FJsonValueObject>(Row));
		}
		Stmt.Destroy();
		Result->SetArrayField(TEXT("nodes"), Rows);
	}

	// Edges.
	{
		TArray<TSharedPtr<FJsonValue>> Rows;
		FSQLitePreparedStatement Stmt;
		if (Stmt.Create(*RawDB, TEXT(
			"SELECT from_node_guid, from_pin, to_node_guid, to_pin, is_orphaned "
			"FROM flow_node_connections WHERE fa_asset_id = ? ORDER BY from_node_guid, from_pin")))
		{
			Stmt.SetBindingValueByIndex(1, AssetId);
			while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
			{
				FString FromG, FromP, ToG, ToP;
				int64 Orphan = 0;
				Stmt.GetColumnValueByIndex(0, FromG);
				Stmt.GetColumnValueByIndex(1, FromP);
				Stmt.GetColumnValueByIndex(2, ToG);
				Stmt.GetColumnValueByIndex(3, ToP);
				Stmt.GetColumnValueByIndex(4, Orphan);

				auto Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("from"), FromG);
				Row->SetStringField(TEXT("from_pin"), FromP);
				Row->SetStringField(TEXT("to"), ToG);
				Row->SetStringField(TEXT("to_pin"), ToP);
				if (Orphan != 0) Row->SetBoolField(TEXT("is_orphaned"), true);
				Rows.Add(MakeShared<FJsonValueObject>(Row));
			}
			Stmt.Destroy();
		}
		Result->SetArrayField(TEXT("edges"), Rows);
	}

	// Comment boxes.
	{
		TArray<TSharedPtr<FJsonValue>> Rows;
		FSQLitePreparedStatement Stmt;
		if (Stmt.Create(*RawDB, TEXT(
			"SELECT comment_text, pos_x, pos_y, width, height FROM flow_graph_comments WHERE fa_asset_id = ?")))
		{
			Stmt.SetBindingValueByIndex(1, AssetId);
			while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
			{
				FString Text;
				int64 PosX = 0, PosY = 0, Width = 0, Height = 0;
				Stmt.GetColumnValueByIndex(0, Text);
				Stmt.GetColumnValueByIndex(1, PosX);
				Stmt.GetColumnValueByIndex(2, PosY);
				Stmt.GetColumnValueByIndex(3, Width);
				Stmt.GetColumnValueByIndex(4, Height);

				auto Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("text"), Text);
				Row->SetNumberField(TEXT("pos_x"), PosX);
				Row->SetNumberField(TEXT("pos_y"), PosY);
				Row->SetNumberField(TEXT("width"), Width);
				Row->SetNumberField(TEXT("height"), Height);
				Rows.Add(MakeShared<FJsonValueObject>(Row));
			}
			Stmt.Destroy();
		}
		if (Rows.Num() > 0) Result->SetArrayField(TEXT("comments"), Rows);
	}

	// Custom events.
	{
		TArray<TSharedPtr<FJsonValue>> Inputs, Outputs;
		FSQLitePreparedStatement Stmt;
		if (Stmt.Create(*RawDB, TEXT(
			"SELECT kind, event_name FROM flow_custom_events WHERE fa_asset_id = ?")))
		{
			Stmt.SetBindingValueByIndex(1, AssetId);
			while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
			{
				FString Kind, EventName;
				Stmt.GetColumnValueByIndex(0, Kind);
				Stmt.GetColumnValueByIndex(1, EventName);
				auto& Target = (Kind == TEXT("input")) ? Inputs : Outputs;
				Target.Add(MakeShared<FJsonValueString>(EventName));
			}
			Stmt.Destroy();
		}
		if (Inputs.Num() > 0) Result->SetArrayField(TEXT("custom_inputs"), Inputs);
		if (Outputs.Num() > 0) Result->SetArrayField(TEXT("custom_outputs"), Outputs);
	}

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// diff_assets
// ============================================================================

FMonolithActionResult FMonolithFlowActions::DiffAssets(const TSharedPtr<FJsonObject>& Params)
{
	FString Err;
	FSQLiteDatabase* RawDB = GetRawDB(Err);
	if (!RawDB) return FMonolithActionResult::Error(Err);

	FString PathA, PathB;
	if (!Params.IsValid()
		|| !Params->TryGetStringField(TEXT("asset_path_a"), PathA) || PathA.IsEmpty()
		|| !Params->TryGetStringField(TEXT("asset_path_b"), PathB) || PathB.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path_a and asset_path_b are required"));
	}

	int64 RowIdA = -1, AssetIdA = -1, RowIdB = -1, AssetIdB = -1;
	SelectFlowAssetIds(RawDB, PathA, RowIdA, AssetIdA);
	SelectFlowAssetIds(RawDB, PathB, RowIdB, AssetIdB);
	if (RowIdA <= 0) return FMonolithActionResult::Error(FString::Printf(TEXT("No Flow Asset indexed at path '%s'."), *PathA));
	if (RowIdB <= 0) return FMonolithActionResult::Error(FString::Printf(TEXT("No Flow Asset indexed at path '%s'."), *PathB));

	struct FNodeRow { FString Class; FString Display; FString Data; };
	auto LoadNodes = [RawDB](int64 AssetId, TMap<FString, FNodeRow>& Out) -> bool
	{
		FSQLitePreparedStatement Stmt;
		if (!Stmt.Create(*RawDB, TEXT(
			"SELECT node_guid, node_class, display_name, data FROM flow_nodes WHERE fa_asset_id = ?")))
		{
			return false;
		}
		Stmt.SetBindingValueByIndex(1, AssetId);
		while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			FString Guid, Cls, Disp, Data;
			Stmt.GetColumnValueByIndex(0, Guid);
			Stmt.GetColumnValueByIndex(1, Cls);
			Stmt.GetColumnValueByIndex(2, Disp);
			Stmt.GetColumnValueByIndex(3, Data);
			Out.Add(Guid, FNodeRow{Cls, Disp, Data});
		}
		Stmt.Destroy();
		return true;
	};

	auto LoadEdges = [RawDB](int64 AssetId, TSet<FString>& Out) -> bool
	{
		FSQLitePreparedStatement Stmt;
		if (!Stmt.Create(*RawDB, TEXT(
			"SELECT from_node_guid, from_pin, to_node_guid, to_pin FROM flow_node_connections "
			"WHERE fa_asset_id = ? AND is_orphaned = 0")))
		{
			return false;
		}
		Stmt.SetBindingValueByIndex(1, AssetId);
		while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			FString FromG, FromP, ToG, ToP;
			Stmt.GetColumnValueByIndex(0, FromG);
			Stmt.GetColumnValueByIndex(1, FromP);
			Stmt.GetColumnValueByIndex(2, ToG);
			Stmt.GetColumnValueByIndex(3, ToP);
			Out.Add(FString::Printf(TEXT("%s.%s -> %s.%s"), *FromG, *FromP, *ToG, *ToP));
		}
		Stmt.Destroy();
		return true;
	};

	TMap<FString, FNodeRow> NodesA, NodesB;
	TSet<FString> EdgesA, EdgesB;
	if (!LoadNodes(AssetIdA, NodesA) || !LoadNodes(AssetIdB, NodesB)
		|| !LoadEdges(AssetIdA, EdgesA) || !LoadEdges(AssetIdB, EdgesB))
	{
		return FMonolithActionResult::Error(TEXT("Failed to prepare diff_assets SQL"));
	}

	auto MakeNodeRow = [](const FString& Guid, const FNodeRow& Info)
	{
		auto Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("node_guid"), Guid);
		Row->SetStringField(TEXT("node_class"), Info.Class);
		Row->SetStringField(TEXT("display_name"), Info.Display);
		return MakeShared<FJsonValueObject>(Row);
	};

	TArray<TSharedPtr<FJsonValue>> OnlyInA, OnlyInB, Changed;
	for (const TPair<FString, FNodeRow>& Pair : NodesA)
	{
		const FNodeRow* Other = NodesB.Find(Pair.Key);
		if (!Other)
		{
			OnlyInA.Add(MakeNodeRow(Pair.Key, Pair.Value));
		}
		else if (Other->Class != Pair.Value.Class || Other->Data != Pair.Value.Data)
		{
			auto Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("node_guid"), Pair.Key);
			Row->SetStringField(TEXT("node_class"), Pair.Value.Class);
			Row->SetStringField(TEXT("display_name"), Pair.Value.Display);
			if (Other->Class != Pair.Value.Class)
			{
				Row->SetStringField(TEXT("node_class_b"), Other->Class);
			}
			else
			{
				Row->SetField(TEXT("data_a"), TryParseJsonValue(Pair.Value.Data));
				Row->SetField(TEXT("data_b"), TryParseJsonValue(Other->Data));
			}
			Changed.Add(MakeShared<FJsonValueObject>(Row));
		}
	}
	for (const TPair<FString, FNodeRow>& Pair : NodesB)
	{
		if (!NodesA.Contains(Pair.Key))
		{
			OnlyInB.Add(MakeNodeRow(Pair.Key, Pair.Value));
		}
	}

	TArray<TSharedPtr<FJsonValue>> EdgesRemoved, EdgesAdded;
	for (const FString& Edge : EdgesA)
	{
		if (!EdgesB.Contains(Edge)) EdgesRemoved.Add(MakeShared<FJsonValueString>(Edge));
	}
	for (const FString& Edge : EdgesB)
	{
		if (!EdgesA.Contains(Edge)) EdgesAdded.Add(MakeShared<FJsonValueString>(Edge));
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path_a"), PathA);
	Result->SetStringField(TEXT("asset_path_b"), PathB);
	Result->SetArrayField(TEXT("nodes_only_in_a"), OnlyInA);
	Result->SetArrayField(TEXT("nodes_only_in_b"), OnlyInB);
	Result->SetArrayField(TEXT("nodes_changed"), Changed);
	Result->SetArrayField(TEXT("edges_removed"), EdgesRemoved);
	Result->SetArrayField(TEXT("edges_added"), EdgesAdded);
	Result->SetBoolField(TEXT("identical"),
		OnlyInA.Num() == 0 && OnlyInB.Num() == 0 && Changed.Num() == 0
		&& EdgesRemoved.Num() == 0 && EdgesAdded.Num() == 0);
	return FMonolithActionResult::Success(Result);
}

#endif // WITH_FLOW
