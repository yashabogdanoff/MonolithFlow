#include "MonolithFlowTestUtils.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_FLOW

#include "Misc/AutomationTest.h"
#include "MonolithFlowActions.h"
#include "MonolithFlowAuthorActions.h"
#include "FlowAsset.h"
#include "Nodes/FlowNode.h"
#include "Nodes/FlowPin.h"
#include "Nodes/Developer/FlowNode_Log.h"
#include "Nodes/Graph/FlowNode_FormatText.h"
#include "Nodes/Actor/FlowNode_ExecuteComponent.h"
#include "Graph/Nodes/FlowGraphNode.h"
#include "EdGraph/EdGraphSchema.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"

using namespace MonolithFlowTests;

namespace
{
	constexpr EAutomationTestFlags TestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

	FString Q(const FString& Value)
	{
		return TEXT("'") + Value.Replace(TEXT("'"), TEXT("''")) + TEXT("'");
	}

	FString ToText(const TSharedPtr<FJsonObject>& Object)
	{
		FString Out;
		if (Object.IsValid())
		{
			const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
			FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
		}
		return Out;
	}

	/** Pretend the previous index pass saw an extra property on a node, as before a C++ refactor removed it. */
	void PlantLegacyProperty(FTempIndex& Index, const UFlowNode* Node, const FString& Name, const FString& Value)
	{
		const FString Guid = Q(GuidOf(Node));
		const FString Data = Index.Text(TEXT("SELECT data FROM flow_nodes WHERE node_guid = ") + Guid);
		const FString Planted = FString::Printf(TEXT("{\"%s\":\"%s\",%s"), *Name, *Value, *Data.Mid(1));
		Index.Raw()->Execute(*FString::Printf(TEXT("UPDATE flow_nodes SET data = %s WHERE node_guid = %s"), *Q(Planted), *Guid));
	}

	int64 HistoryRows(FTempIndex& Index, const UFlowNode* Node)
	{
		return Index.Count(TEXT("SELECT COUNT(*) FROM flow_node_history WHERE node_guid = ") + Q(GuidOf(Node)));
	}

	/** First non-exec input pin of a node, as the schema sees it. */
	UEdGraphPin* FirstDataInput(UFlowNode* Node)
	{
		if (UEdGraphNode* GraphNode = Node->GetGraphNode())
		{
			for (UEdGraphPin* Pin : GraphNode->Pins)
			{
				if (Pin->Direction == EGPD_Input && !FFlowPin::IsExecPinCategory(Pin->PinType.PinCategory))
				{
					return Pin;
				}
			}
		}
		return nullptr;
	}
}

// ---------------------------------------------------------------------------
// Snapshot and property history
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithFlowSnapshotCleanTest, "MonolithFlow.Snapshot.NoEditorWrapperPathAndTimestamped", TestFlags)
bool FMonolithFlowSnapshotCleanTest::RunTest(const FString& Parameters)
{
	FTempIndex Index;
	UFlowAsset* Asset = NewFlowAsset(TEXT("FLOW_SnapshotClean"));
	UFlowNode* Log = AddNode(Asset, UFlowNode_Log::StaticClass(), 300, 0);
	Index.Index(Asset);

	const FString Data = Index.Text(TEXT("SELECT data FROM flow_nodes WHERE node_guid = ") + Q(GuidOf(Log)));
	UTEST_TRUE("snapshot written", Data.Contains(TEXT("\"message\"")));
	UTEST_FALSE("no graphNode path in the snapshot", Data.Contains(TEXT("graphNode")));
	UTEST_FALSE("indexed_at set", Index.Text(TEXT("SELECT indexed_at FROM flow_assets")).IsEmpty());

	Discard(Asset);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithFlowHistoryTest, "MonolithFlow.History.KeepsSnapshotWhenPropertiesDisappear", TestFlags)
bool FMonolithFlowHistoryTest::RunTest(const FString& Parameters)
{
	FTempIndex Index;
	UFlowAsset* Asset = NewFlowAsset(TEXT("FLOW_History"));
	UFlowNode* Log = AddNode(Asset, UFlowNode_Log::StaticClass(), 300, 0);
	Index.Index(Asset);

	UTEST_TRUE("plain re-index keeps no history", Index.Index(Asset) > 0);
	UTEST_EQUAL("nothing kept", HistoryRows(Index, Log), 0ll);

	PlantLegacyProperty(Index, Log, TEXT("legacyLine"), TEXT("keep me"));
	UTEST_TRUE("re-index after the refactor", Index.Index(Asset) > 0);
	UTEST_EQUAL("one snapshot kept", HistoryRows(Index, Log), 1ll);
	UTEST_EQUAL("removed property named", Index.Text(TEXT("SELECT removed_properties FROM flow_node_history")), FString(TEXT("legacyLine")));
	UTEST_TRUE("old value kept", Index.Text(TEXT("SELECT data FROM flow_node_history")).Contains(TEXT("keep me")));

	FMonolithFlowActions::FScopedDatabaseOverride Override(Index.Raw());
	const FMonolithActionResult History = FMonolithFlowActions::GetNodeHistory(Json(FString::Printf(
		TEXT("{\"asset_path\":\"%s\",\"node_guid\":\"%s\"}"), *Asset->GetPackage()->GetName(), *GuidOf(Log).ToLower())));
	UTEST_TRUE("get_node_history ok (package path, lower-case guid)", History.bSuccess);
	UTEST_EQUAL("one snapshot returned", static_cast<int32>(History.Result->GetNumberField(TEXT("count"))), 1);
	UTEST_TRUE("value in the answer", ToText(History.Result).Contains(TEXT("keep me")));

	for (int32 i = 0; i < 6; ++i)
	{
		PlantLegacyProperty(Index, Log, FString::Printf(TEXT("legacy%d"), i), TEXT("x"));
		Index.Index(Asset);
	}
	UTEST_EQUAL("bounded to five per node", HistoryRows(Index, Log), 5ll);

	Discard(Asset);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithFlowHistoryFullPassTest, "MonolithFlow.History.SurvivesFullPassWithReusedIds", TestFlags)
bool FMonolithFlowHistoryFullPassTest::RunTest(const FString& Parameters)
{
	FTempIndex Index;
	UFlowAsset* A = NewFlowAsset(TEXT("FLOW_HistPassA"));
	UFlowAsset* B = NewFlowAsset(TEXT("FLOW_HistPassB"));
	UFlowNode* LogA = AddNode(A, UFlowNode_Log::StaticClass(), 300, 0);
	AddNode(B, UFlowNode_Log::StaticClass(), 300, 0);
	Index.Index(A);
	Index.Index(B);
	PlantLegacyProperty(Index, LogA, TEXT("legacyLine"), TEXT("survive the pass"));

	// Full pass in reverse order: B takes A's old id before A's turn.
	Index.Database().ResetDatabase();
	Index.Index(B);
	Index.Index(A);

	UTEST_EQUAL("A's lost property still caught", HistoryRows(Index, LogA), 1ll);
	UTEST_TRUE("with its value", Index.Text(TEXT("SELECT data FROM flow_node_history")).Contains(TEXT("survive the pass")));
	UTEST_EQUAL("both graphs intact", Index.Count(TEXT(
		"SELECT COUNT(*) FROM flow_assets f WHERE f.node_count <> (SELECT COUNT(*) FROM flow_nodes n WHERE n.fa_asset_id = f.fa_asset_id)")), 0ll);

	Index.Indexer().ReconcileWithAssets(Index.Database(), /*bFollowRenames=*/false, /*bSweepOrphans=*/true);
	UTEST_EQUAL("nothing left parked", Index.Count(TEXT("SELECT COUNT(*) FROM flow_assets WHERE fa_asset_id < 0")), 0ll);
	UTEST_EQUAL("history kept by reconciliation", HistoryRows(Index, LogA), 1ll);

	Discard(A);
	Discard(B);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithFlowHistoryReconcileTest, "MonolithFlow.History.FollowsRenamesAndGoesWithDeletes", TestFlags)
bool FMonolithFlowHistoryReconcileTest::RunTest(const FString& Parameters)
{
	FTempIndex Index;
	UFlowAsset* Asset = NewFlowAsset(TEXT("FLOW_HistMove"));
	UFlowNode* Log = AddNode(Asset, UFlowNode_Log::StaticClass(), 300, 0);
	Index.Index(Asset);
	PlantLegacyProperty(Index, Log, TEXT("legacyLine"), TEXT("v"));
	Index.Index(Asset);

	const FString NewPackage = TEXT("/Temp/MonolithFlowTests/FLOW_HistMoved");
	Index.Database().UpdateAssetPath(Asset->GetPackage()->GetName(), NewPackage, TEXT("FLOW_HistMoved"));
	Index.Indexer().ReconcileWithAssets(Index.Database(), /*bFollowRenames=*/true, /*bSweepOrphans=*/false);
	UTEST_EQUAL("history moved", Index.Text(TEXT("SELECT fa_path FROM flow_node_history")), NewPackage + TEXT(".FLOW_HistMoved"));

	Index.Database().DeleteAssetByPath(NewPackage);
	Index.Indexer().ReconcileWithAssets(Index.Database(), /*bFollowRenames=*/true, /*bSweepOrphans=*/false);
	UTEST_EQUAL("history dropped with the graph", Index.Count(TEXT("SELECT COUNT(*) FROM flow_node_history")), 0ll);

	Discard(Asset);
	return true;
}

// ---------------------------------------------------------------------------
// Connections
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithFlowDataPinTest, "MonolithFlow.Index.DataPinLinksPointForward", TestFlags)
bool FMonolithFlowDataPinTest::RunTest(const FString& Parameters)
{
	FTempIndex Index;
	UFlowAsset* Asset = NewFlowAsset(TEXT("FLOW_DataPins"));
	UFlowNode* Producer = AddNode(Asset, UFlowNode_FormatText::StaticClass(), 300, 300);
	UFlowNode* Consumer = AddNode(Asset, UFlowNode_FormatText::StaticClass(), 700, 300);
	Connect(StartNodeOf(Asset), TEXT("Out"), Consumer);

	UEdGraphPin* Out = Producer->GetGraphNode()->FindPin(UFlowNode_FormatText::OUTPIN_TextOutput, EGPD_Output);
	UEdGraphPin* In = FirstDataInput(Consumer);
	UTEST_NOT_NULL("producer data output", Out);
	UTEST_NOT_NULL("consumer data input", In);
	UTEST_TRUE("schema connects data pins", Out->GetSchema()->TryCreateConnection(Out, In));
	Asset->HarvestNodeConnections();

	Index.Index(Asset);
	UTEST_EQUAL("one data link, producer -> consumer", Index.Count(FString::Printf(
		TEXT("SELECT COUNT(*) FROM flow_node_connections WHERE kind = 'data' AND is_orphaned = 0 AND from_node_guid = %s AND to_node_guid = %s AND to_pin = %s"),
		*Q(GuidOf(Producer)), *Q(GuidOf(Consumer)), *Q(In->PinName.ToString()))), 1ll);
	UTEST_EQUAL("no fake output pin for the data input", Index.Count(FString::Printf(
		TEXT("SELECT COUNT(*) FROM flow_node_pins WHERE node_guid = %s AND pin_direction = 'output' AND pin_name = %s"),
		*Q(GuidOf(Consumer)), *Q(In->PinName.ToString()))), 0ll);

	FMonolithFlowActions::FScopedDatabaseOverride Override(Index.Raw());
	const FMonolithActionResult Walk = FMonolithFlowActions::WalkExecution(Json(FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *Asset->GetPathName())));
	UTEST_TRUE("walk ok", Walk.bSuccess);
	FString Unreachable;
	{
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Unreachable);
		FJsonSerializer::Serialize(Walk.Result->GetArrayField(TEXT("unreachable")), Writer);
	}
	UTEST_TRUE("a data link does not make the producer reachable", Unreachable.Contains(GuidOf(Producer)));

	Discard(Asset);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithFlowOrphanEdgeTest, "MonolithFlow.Index.OrphanedEdgesAreFlagged", TestFlags)
bool FMonolithFlowOrphanEdgeTest::RunTest(const FString& Parameters)
{
	FTempIndex Index;
	UFlowAsset* Asset = NewFlowAsset(TEXT("FLOW_OrphanEdge"));
	UFlowNode* Log = AddNode(Asset, UFlowNode_Log::StaticClass(), 300, 0);
	UFlowNode* Target = AddNode(Asset, UFlowNode_Log::StaticClass(), 600, 0);

	// A connection left behind under a pin the node no longer has.
	FMapProperty* ConnectionsProp = CastField<FMapProperty>(UFlowNode::StaticClass()->FindPropertyByName(TEXT("Connections")));
	UTEST_NOT_NULL("Connections map", ConnectionsProp);
	FScriptMapHelper Helper(ConnectionsProp, ConnectionsProp->ContainerPtrToValuePtr<void>(Log));
	const FName GhostPin(TEXT("RemovedOutput"));
	const FConnectedPin GhostTarget(Target->GetGuid(), TEXT("In"));
	Helper.AddPair(&GhostPin, &GhostTarget);

	Index.Index(Asset);
	UTEST_EQUAL("orphan edge flagged", Index.Count(FString::Printf(
		TEXT("SELECT COUNT(*) FROM flow_node_connections WHERE from_node_guid = %s AND from_pin = 'RemovedOutput' AND is_orphaned = 1"),
		*Q(GuidOf(Log)))), 1ll);

	Discard(Asset);
	return true;
}

// ---------------------------------------------------------------------------
// Read actions
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithFlowLiteralGlobTest, "MonolithFlow.Read.GlobTreatsUnderscoreLiterally", TestFlags)
bool FMonolithFlowLiteralGlobTest::RunTest(const FString& Parameters)
{
	FTempIndex Index;
	UFlowAsset* Underscore = NewFlowAsset(TEXT("FLOW_A_B"));
	UFlowAsset* Other = NewFlowAsset(TEXT("FLOW_AxB"));
	Index.Index(Underscore);
	Index.Index(Other);

	FMonolithFlowActions::FScopedDatabaseOverride Override(Index.Raw());
	const FMonolithActionResult List = FMonolithFlowActions::ListAssets(Json(TEXT("{\"asset_path_filter\":\"*FLOW_A_B*\"}")));
	UTEST_TRUE("ok", List.bSuccess);
	UTEST_EQUAL("only the underscore asset", static_cast<int32>(List.Result->GetNumberField(TEXT("total"))), 1);

	Discard(Underscore);
	Discard(Other);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithFlowPropertyScopeTest, "MonolithFlow.Read.PropertySearchStaysInTheProperty", TestFlags)
bool FMonolithFlowPropertyScopeTest::RunTest(const FString& Parameters)
{
	FTempIndex Index;
	UFlowAsset* Asset = NewFlowAsset(TEXT("FLOW_PropScope"));
	UFlowNode* Plain = AddNode(Asset, UFlowNode_Log::StaticClass(), 300, 0);
	UFlowNode* Match = AddNode(Asset, UFlowNode_Log::StaticClass(), 300, 200);
	CastField<FStrProperty>(UFlowNode_Log::StaticClass()->FindPropertyByName(TEXT("Message")))->SetPropertyValue_InContainer(Match, TEXT("Warning issued"));
	Index.Index(Asset);

	// Every Log node has "verbosity":"Warning" after its message.
	FMonolithFlowActions::FScopedDatabaseOverride Override(Index.Raw());
	const FMonolithActionResult Found = FMonolithFlowActions::FindNodesByProperty(Json(TEXT("{\"value_substring\":\"Warning\",\"property_name\":\"message\"}")));
	UTEST_TRUE("ok", Found.bSuccess);
	UTEST_EQUAL("only the node whose message says it", static_cast<int32>(Found.Result->GetNumberField(TEXT("total_node_count"))), 1);
	UTEST_TRUE("and it is that node", ToText(Found.Result).Contains(GuidOf(Match)));
	UTEST_FALSE("not the plain one", ToText(Found.Result).Contains(GuidOf(Plain)));

	Discard(Asset);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithFlowParamValidationTest, "MonolithFlow.Read.ParamsAreValidated", TestFlags)
bool FMonolithFlowParamValidationTest::RunTest(const FString& Parameters)
{
	FTempIndex Index;
	UFlowAsset* Asset = NewFlowAsset(TEXT("FLOW_Params"));
	UFlowNode* Log = AddNode(Asset, UFlowNode_Log::StaticClass(), 300, 0);
	Index.Index(Asset);
	FMonolithFlowActions::FScopedDatabaseOverride Override(Index.Raw());
	const FString Path = Asset->GetPathName();

	UTEST_TRUE("lower-case guid accepted", FMonolithFlowActions::GetNodeInfo(Json(FString::Printf(
		TEXT("{\"asset_path\":\"%s\",\"node_guid\":\"%s\"}"), *Path, *GuidOf(Log).ToLower()))).bSuccess);
	UTEST_FALSE("bad pin_direction", FMonolithFlowActions::ListNodePins(Json(FString::Printf(
		TEXT("{\"asset_path\":\"%s\",\"pin_direction\":\"sideways\"}"), *Path))).bSuccess);
	UTEST_FALSE("bad kind", FMonolithFlowActions::ListNodeClasses(Json(TEXT("{\"kind\":\"widget\"}"))).bSuccess);
	UTEST_FALSE("bad source", FMonolithFlowActions::ListNodeClasses(Json(TEXT("{\"source\":\"cloud\"}"))).bSuccess);
	UTEST_FALSE("bad custom event kind", FMonolithFlowActions::ListCustomEvents(Json(FString::Printf(
		TEXT("{\"asset_path\":\"%s\",\"kind\":\"both\"}"), *Path))).bSuccess);

	Discard(Asset);
	return true;
}

// ---------------------------------------------------------------------------
// Authoring and layout
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithFlowStartUndeletableTest, "MonolithFlow.Author.StartNodeCannotBeRemoved", TestFlags)
bool FMonolithFlowStartUndeletableTest::RunTest(const FString& Parameters)
{
	UFlowAsset* Asset = NewFlowAsset(TEXT("FLOW_KeepStart"));
	UFlowNode* Start = StartNodeOf(Asset);
	const FMonolithActionResult Result = FMonolithFlowAuthorActions::RemoveNode(Json(FString::Printf(
		TEXT("{\"asset_path\":\"%s\",\"node_guid\":\"%s\"}"), *Asset->GetPathName(), *GuidOf(Start))));
	UTEST_FALSE("refused", Result.bSuccess);
	UTEST_EQUAL("code", ErrorCodeOf(Result), FString(TEXT("node_not_deletable")));
	UTEST_NOT_NULL("Start still there", StartNodeOf(Asset));
	Discard(Asset);
	return true;
}

IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(FMonolithFlowInstancedShapeTest, FUndoingTestBase, "MonolithFlow.Author.InstancedObjectsAcceptInlineShape", TestFlags)
bool FMonolithFlowInstancedShapeTest::RunTest(const FString& Parameters)
{
	UFlowAsset* Asset = NewFlowAsset(TEXT("FLOW_Instanced"));
	UFlowNode* Node = AddNode(Asset, UFlowNode_ExecuteComponent::StaticClass(), 300, 0);
	const FMonolithActionResult Result = FMonolithFlowAuthorActions::SetNodeProperties(Json(FString::Printf(
		TEXT("{\"asset_path\":\"%s\",\"node_guid\":\"%s\",\"dry_run\":true,\"properties\":{\"componentTemplate\":{\"_ClassName\":\"/Script/Engine.SceneComponent\"}}}"),
		*Asset->GetPathName(), *GuidOf(Node))));
	UTEST_NOT_EQUAL("the snapshot's inline shape is not a type error", ErrorCodeOf(Result), FString(TEXT("wrong_type")));
	Discard(Asset);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithFlowLayoutLoopTest, "MonolithFlow.Layout.LoopsStayCompact", TestFlags)
bool FMonolithFlowLayoutLoopTest::RunTest(const FString& Parameters)
{
	UFlowAsset* Asset = NewFlowAsset(TEXT("FLOW_LayoutLoop"));
	UFlowNode* A = AddNode(Asset, UFlowNode_Log::StaticClass(), 10, 10);
	UFlowNode* B = AddNode(Asset, UFlowNode_Log::StaticClass(), 20, 20);
	Connect(StartNodeOf(Asset), TEXT("Out"), A);
	Connect(A, TEXT("Out"), B);
	Connect(B, TEXT("Out"), A);
	for (int32 i = 0; i < 20; ++i)
	{
		AddNode(Asset, UFlowNode_Log::StaticClass(), 30 + i, 30 + i);
	}

	const FMonolithActionResult Result = FMonolithFlowAuthorActions::AutoLayout(Json(FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *Asset->GetPathName())));
	UTEST_TRUE("ok", Result.bSuccess);
	int32 MaxX = 0;
	for (const TPair<FGuid, UFlowNode*>& Pair : Asset->GetNodes())
	{
		MaxX = FMath::Max(MaxX, Pair.Value->GetGraphNode()->NodePosX);
	}
	UTEST_TRUE(FString::Printf(TEXT("three layers at most (max x %d)"), MaxX), MaxX <= 2 * 420);
	UTEST_TRUE("A before B", A->GetGraphNode()->NodePosX < B->GetGraphNode()->NodePosX);
	Discard(Asset);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithFlowLayoutNewOnlyTest, "MonolithFlow.Layout.NewOnlyLeavesStartAlone", TestFlags)
bool FMonolithFlowLayoutNewOnlyTest::RunTest(const FString& Parameters)
{
	UFlowAsset* Asset = NewFlowAsset(TEXT("FLOW_LayoutNew"));
	UFlowNode* Start = StartNodeOf(Asset);
	UFlowNode* Fresh = AddNode(Asset, UFlowNode_Log::StaticClass(), 0, 0);
	Connect(Start, TEXT("Out"), Fresh);
	const FIntPoint StartBefore(Start->GetGraphNode()->NodePosX, Start->GetGraphNode()->NodePosY);

	const FMonolithActionResult Result = FMonolithFlowAuthorActions::AutoLayout(Json(FString::Printf(
		TEXT("{\"asset_path\":\"%s\",\"layout_mode\":\"new_only\"}"), *Asset->GetPathName())));
	UTEST_TRUE("ok", Result.bSuccess);
	UTEST_EQUAL("Start stays", FIntPoint(Start->GetGraphNode()->NodePosX, Start->GetGraphNode()->NodePosY), StartBefore);
	UTEST_TRUE("the new node moved right of Start", Fresh->GetGraphNode()->NodePosX > StartBefore.X);
	UTEST_FALSE("unknown formatter rejected", FMonolithFlowAuthorActions::AutoLayout(Json(FString::Printf(
		TEXT("{\"asset_path\":\"%s\",\"formatter\":\"magic\"}"), *Asset->GetPathName()))).bSuccess);
	Discard(Asset);
	return true;
}

#endif
