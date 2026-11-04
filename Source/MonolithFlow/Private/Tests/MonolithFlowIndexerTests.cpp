#include "MonolithFlowTestUtils.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_FLOW

#include "Misc/AutomationTest.h"
#include "FlowAsset.h"
#include "Nodes/FlowNode.h"
#include "Nodes/Developer/FlowNode_Log.h"
#include "Nodes/Graph/FlowNode_SubGraph.h"
#include "Nodes/Route/FlowNode_Branch.h"
#include "AddOns/FlowNodeAddOn_PredicateAND.h"
#include "AddOns/FlowNodeAddOn_PredicateNOT.h"
#include "Graph/FlowGraphSchema_Actions.h"
#include "Graph/Nodes/FlowGraphNode.h"
#include "MonolithFlowActions.h"
#include "UObject/UObjectHash.h"

using namespace MonolithFlowTests;

namespace
{
	constexpr EAutomationTestFlags TestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

	FString Q(const FString& Value)
	{
		return TEXT("'") + Value.Replace(TEXT("'"), TEXT("''")) + TEXT("'");
	}

	/** Nodes rows written for a graph, via its flow_assets row. */
	int64 NodeRowsOf(FTempIndex& Index, const UObject* Asset)
	{
		return Index.Count(FString::Printf(
			TEXT("SELECT COUNT(*) FROM flow_nodes WHERE fa_asset_id = (SELECT fa_asset_id FROM flow_assets WHERE fa_path = %s)"),
			*Q(ObjectPathOf(Asset))));
	}

	/** Point a SubGraph node at another graph (Asset is a private UPROPERTY). */
	void PointSubGraphAt(UFlowNode* SubGraphNode, UFlowAsset* Target)
	{
		if (FSoftObjectProperty* Prop = CastField<FSoftObjectProperty>(SubGraphNode->GetClass()->FindPropertyByName(TEXT("Asset"))))
		{
			Prop->SetPropertyValue_InContainer(SubGraphNode, FSoftObjectPtr(Target));
		}
	}

	int64 OrphanRows(FTempIndex& Index)
	{
		return Index.Count(TEXT(
			"SELECT (SELECT COUNT(*) FROM flow_nodes WHERE fa_asset_id NOT IN (SELECT fa_asset_id FROM flow_assets))"
			" + (SELECT COUNT(*) FROM flow_node_pins WHERE fa_asset_id NOT IN (SELECT fa_asset_id FROM flow_assets))"
			" + (SELECT COUNT(*) FROM flow_node_connections WHERE fa_asset_id NOT IN (SELECT fa_asset_id FROM flow_assets))"
			" + (SELECT COUNT(*) FROM flow_subgraph_refs WHERE host_fa_asset_id NOT IN (SELECT fa_asset_id FROM flow_assets))"));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithFlowIndexWritesGraphTest, "MonolithFlow.Index.WritesNodesPinsAndEdges", TestFlags)
bool FMonolithFlowIndexWritesGraphTest::RunTest(const FString& Parameters)
{
	FTempIndex Index;
	UTEST_TRUE("temp index opened", Index.IsOpen());

	UFlowAsset* FlowAsset = NewFlowAsset(TEXT("FLOW_Writes"));
	UFlowNode* Start = StartNodeOf(FlowAsset);
	UFlowNode* LogA = AddNode(FlowAsset, UFlowNode_Log::StaticClass(), 300, 0);
	UFlowNode* LogB = AddNode(FlowAsset, UFlowNode_Log::StaticClass(), 600, 0);
	UTEST_NOT_NULL("start node", Start);
	UTEST_TRUE("start -> A", Connect(Start, TEXT("Out"), LogA));
	UTEST_TRUE("A -> B", Connect(LogA, TEXT("Out"), LogB));

	UTEST_TRUE("indexed", Index.Index(FlowAsset) > 0);

	const FString Path = Q(ObjectPathOf(FlowAsset));
	UTEST_EQUAL("one flow_assets row", Index.Count(TEXT("SELECT COUNT(*) FROM flow_assets")), 1ll);
	UTEST_EQUAL("node_count header", Index.Count(TEXT("SELECT node_count FROM flow_assets WHERE fa_path = ") + Path), 3ll);
	UTEST_EQUAL("node rows", NodeRowsOf(Index, FlowAsset), 3ll);
	UTEST_EQUAL("edges", Index.Count(TEXT("SELECT COUNT(*) FROM flow_node_connections")), 2ll);
	UTEST_EQUAL("edge A -> B under node keys", Index.Count(FString::Printf(
		TEXT("SELECT COUNT(*) FROM flow_node_connections WHERE from_node_guid = %s AND from_pin = 'Out' AND to_node_guid = %s"),
		*Q(GuidOf(LogA)), *Q(GuidOf(LogB)))), 1ll);
	UTEST_TRUE("pins written", Index.Count(TEXT("SELECT COUNT(*) FROM flow_node_pins")) >= 5);
	UTEST_TRUE("property snapshot holds the Log message", Index.Text(FString::Printf(
		TEXT("SELECT data FROM flow_nodes WHERE node_guid = %s"), *Q(GuidOf(LogA)))).Contains(TEXT("\"message\"")));
	UTEST_EQUAL("positions recorded", Index.Count(FString::Printf(
		TEXT("SELECT pos_x FROM flow_nodes WHERE node_guid = %s"), *Q(GuidOf(LogB)))), 600ll);
	UTEST_EQUAL("native node classes registered", Index.Count(
		TEXT("SELECT COUNT(*) FROM flow_node_classes WHERE is_native = 1 AND class_path = '/Script/Flow.FlowNode_Log'")), 1ll);

	Discard(FlowAsset);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithFlowIndexReindexReplacesTest, "MonolithFlow.Index.ReindexReplacesRows", TestFlags)
bool FMonolithFlowIndexReindexReplacesTest::RunTest(const FString& Parameters)
{
	FTempIndex Index;
	UFlowAsset* FlowAsset = NewFlowAsset(TEXT("FLOW_Reindex"));
	AddNode(FlowAsset, UFlowNode_Log::StaticClass(), 300, 0);
	UTEST_TRUE("first pass", Index.Index(FlowAsset) > 0);
	UTEST_EQUAL("2 nodes", NodeRowsOf(Index, FlowAsset), 2ll);

	AddNode(FlowAsset, UFlowNode_Log::StaticClass(), 300, 200);
	UTEST_TRUE("second pass", Index.Index(FlowAsset) > 0);
	UTEST_EQUAL("still one asset row", Index.Count(TEXT("SELECT COUNT(*) FROM flow_assets")), 1ll);
	UTEST_EQUAL("3 nodes, no duplicates", NodeRowsOf(Index, FlowAsset), 3ll);
	UTEST_EQUAL("no orphans", OrphanRows(Index), 0ll);

	Discard(FlowAsset);
	return true;
}

// A full pass resets `assets`, so ids are handed out again while flow_* keeps rows
// under the old ones. Indexing B first under A's old id must not let A's later
// cleanup erase B's fresh rows.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithFlowIndexFullPassIdReuseTest, "MonolithFlow.Index.FullPassWithReusedIdsKeepsEveryGraph", TestFlags)
bool FMonolithFlowIndexFullPassIdReuseTest::RunTest(const FString& Parameters)
{
	FTempIndex Index;
	UFlowAsset* A = NewFlowAsset(TEXT("FLOW_IdReuseA"));
	UFlowAsset* B = NewFlowAsset(TEXT("FLOW_IdReuseB"));
	AddNode(A, UFlowNode_Log::StaticClass(), 300, 0);
	for (int32 i = 0; i < 4; ++i)
	{
		AddNode(B, UFlowNode_Log::StaticClass(), 300, 200.f * i);
	}

	UTEST_TRUE("A first", Index.Index(A) > 0);
	UTEST_TRUE("B second", Index.Index(B) > 0);

	// Full pass: Monolith drops its own tables, flow_* survives; this time B comes first.
	UTEST_TRUE("reset", Index.Database().ResetDatabase());
	UTEST_TRUE("B first after reset", Index.Index(B) > 0);
	UTEST_TRUE("A second after reset", Index.Index(A) > 0);

	UTEST_EQUAL("A keeps its 2 nodes", NodeRowsOf(Index, A), 2ll);
	UTEST_EQUAL("B keeps its 5 nodes", NodeRowsOf(Index, B), 5ll);
	UTEST_EQUAL("two graphs", Index.Count(TEXT("SELECT COUNT(*) FROM flow_assets")), 2ll);
	UTEST_EQUAL("every header matches its rows", Index.Count(TEXT(
		"SELECT COUNT(*) FROM flow_assets f WHERE f.node_count <> (SELECT COUNT(*) FROM flow_nodes n WHERE n.fa_asset_id = f.fa_asset_id)")), 0ll);

	Discard(A);
	Discard(B);
	return true;
}

// Duplicated nodes can share one NodeGuid under distinct keys in UFlowAsset::Nodes;
// each must stay a row of its own, under its key.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithFlowIndexNodeKeyTest, "MonolithFlow.Index.NodesKeyedByMapKeyNotNodeGuid", TestFlags)
bool FMonolithFlowIndexNodeKeyTest::RunTest(const FString& Parameters)
{
	FTempIndex Index;
	UFlowAsset* FlowAsset = NewFlowAsset(TEXT("FLOW_NodeKeys"));
	UFlowNode* LogA = AddNode(FlowAsset, UFlowNode_Log::StaticClass(), 300, 0);
	UFlowNode* LogB = AddNode(FlowAsset, UFlowNode_Log::StaticClass(), 300, 200);
	const FString KeyB = GuidOf(LogB);
	LogB->SetGuid(LogA->GetGuid());

	AddExpectedMessage(TEXT("carry a NodeGuid that differs"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1, /*IsRegex=*/false);
	UTEST_TRUE("indexed", Index.Index(FlowAsset) > 0);
	UTEST_EQUAL("3 rows", NodeRowsOf(Index, FlowAsset), 3ll);
	UTEST_EQUAL("3 distinct guids", Index.Count(TEXT("SELECT COUNT(DISTINCT node_guid) FROM flow_nodes")), 3ll);
	UTEST_EQUAL("B indexed under its key", Index.Count(TEXT("SELECT COUNT(*) FROM flow_nodes WHERE node_guid = ") + Q(KeyB)), 1ll);

	Discard(FlowAsset);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithFlowIndexSubclassesTest, "MonolithFlow.Index.SupportsNativeFlowAssetSubclasses", TestFlags)
bool FMonolithFlowIndexSubclassesTest::RunTest(const FString& Parameters)
{
	FTempIndex Index;
	const TArray<FString> Supported = Index.Indexer().GetSupportedClasses();
	UTEST_TRUE("FlowAsset", Supported.Contains(TEXT("FlowAsset")));

	TArray<UClass*> Derived;
	GetDerivedClasses(UFlowAsset::StaticClass(), Derived, /*bRecursive=*/true);
	int32 Checked = 0;
	for (const UClass* Cls : Derived)
	{
		if (!Cls->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists) && !Cls->ClassGeneratedBy)
		{
			TestTrue(FString::Printf(TEXT("%s is routed to the Flow indexer"), *Cls->GetName()), Supported.Contains(Cls->GetName()));
			++Checked;
		}
	}
	AddInfo(FString::Printf(TEXT("%d native UFlowAsset subclass(es) loaded"), Checked));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithFlowIndexSubgraphRefsTest, "MonolithFlow.Index.SubgraphReferences", TestFlags)
bool FMonolithFlowIndexSubgraphRefsTest::RunTest(const FString& Parameters)
{
	FTempIndex Index;
	UFlowAsset* Target = NewFlowAsset(TEXT("FLOW_SubTarget"));
	UFlowAsset* Host = NewFlowAsset(TEXT("FLOW_SubHost"));
	UFlowNode* SubGraph = AddNode(Host, UFlowNode_SubGraph::StaticClass(), 300, 0);
	PointSubGraphAt(SubGraph, Target);

	UTEST_TRUE("indexed", Index.Index(Host) > 0);
	UTEST_EQUAL("one reference to the target", Index.Count(FString::Printf(
		TEXT("SELECT COUNT(*) FROM flow_subgraph_refs WHERE target_fa_path = %s AND host_node_guid = %s"),
		*Q(ObjectPathOf(Target)), *Q(GuidOf(SubGraph)))), 1ll);
	UTEST_EQUAL("subgraph_count header", Index.Count(TEXT("SELECT subgraph_count FROM flow_assets")), 1ll);

	Discard(Target);
	Discard(Host);
	return true;
}

// ---------------------------------------------------------------------------
// Reconciliation with Monolith's `assets` table
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithFlowReconcileDeleteTest, "MonolithFlow.Reconcile.DeletedGraphIsDropped", TestFlags)
bool FMonolithFlowReconcileDeleteTest::RunTest(const FString& Parameters)
{
	FTempIndex Index;
	UFlowAsset* Kept = NewFlowAsset(TEXT("FLOW_Kept"));
	UFlowAsset* Gone = NewFlowAsset(TEXT("FLOW_Gone"));
	AddNode(Gone, UFlowNode_Log::StaticClass(), 300, 0);
	Index.Index(Kept);
	Index.Index(Gone);

	UTEST_TRUE("Monolith deletes the asset row", Index.Database().DeleteAssetByPath(Gone->GetPackage()->GetName()));
	Index.Indexer().ReconcileWithAssets(Index.Database(), /*bFollowRenames=*/true, /*bSweepOrphans=*/false);

	UTEST_EQUAL("only the kept graph remains", Index.Count(TEXT("SELECT COUNT(*) FROM flow_assets")), 1ll);
	UTEST_EQUAL("kept graph untouched", NodeRowsOf(Index, Kept), 1ll);
	UTEST_EQUAL("no orphan child rows", OrphanRows(Index), 0ll);

	Discard(Kept);
	Discard(Gone);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithFlowReconcileRenameTest, "MonolithFlow.Reconcile.RenamedGraphMovesWithItsReferences", TestFlags)
bool FMonolithFlowReconcileRenameTest::RunTest(const FString& Parameters)
{
	FTempIndex Index;
	UFlowAsset* Target = NewFlowAsset(TEXT("FLOW_RenameOld"));
	AddNode(Target, UFlowNode_Log::StaticClass(), 300, 0);
	UFlowAsset* Host = NewFlowAsset(TEXT("FLOW_RenameHost"));
	PointSubGraphAt(AddNode(Host, UFlowNode_SubGraph::StaticClass(), 300, 0), Target);
	Index.Index(Target);
	Index.Index(Host);

	const FString OldPackage = Target->GetPackage()->GetName();
	const FString NewPackage = TEXT("/Temp/MonolithFlowTests/Moved/FLOW_RenameNew");
	const FString NewObjectPath = NewPackage + TEXT(".FLOW_RenameNew");
	UTEST_TRUE("Monolith moves the asset row", Index.Database().UpdateAssetPath(OldPackage, NewPackage, TEXT("FLOW_RenameNew")));
	Index.Indexer().ReconcileWithAssets(Index.Database(), /*bFollowRenames=*/true, /*bSweepOrphans=*/false);

	UTEST_EQUAL("old path gone", Index.Count(TEXT("SELECT COUNT(*) FROM flow_assets WHERE fa_path = ") + Q(ObjectPathOf(Target))), 0ll);
	UTEST_EQUAL("new path present", Index.Count(TEXT("SELECT COUNT(*) FROM flow_assets WHERE fa_path = ") + Q(NewObjectPath)), 1ll);
	UTEST_EQUAL("nodes kept", Index.Count(TEXT("SELECT node_count FROM flow_assets WHERE fa_path = ") + Q(NewObjectPath)), 2ll);
	UTEST_EQUAL("caller points at the new path", Index.Count(TEXT("SELECT COUNT(*) FROM flow_subgraph_refs WHERE target_fa_path = ") + Q(NewObjectPath)), 1ll);
	UTEST_EQUAL("no orphans", OrphanRows(Index), 0ll);

	Discard(Target);
	Discard(Host);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithFlowReconcileNoFollowTest, "MonolithFlow.Reconcile.AfterFullPassRenamesAreDropped", TestFlags)
bool FMonolithFlowReconcileNoFollowTest::RunTest(const FString& Parameters)
{
	FTempIndex Index;
	UFlowAsset* FlowAsset = NewFlowAsset(TEXT("FLOW_NoFollow"));
	Index.Index(FlowAsset);
	Index.Database().UpdateAssetPath(FlowAsset->GetPackage()->GetName(), TEXT("/Temp/MonolithFlowTests/FLOW_NoFollowNew"), TEXT("FLOW_NoFollowNew"));

	Index.Indexer().ReconcileWithAssets(Index.Database(), /*bFollowRenames=*/false, /*bSweepOrphans=*/true);
	UTEST_EQUAL("row dropped, not moved", Index.Count(TEXT("SELECT COUNT(*) FROM flow_assets")), 0ll);
	UTEST_EQUAL("children swept", OrphanRows(Index), 0ll);

	Discard(FlowAsset);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithFlowReconcileStableTest, "MonolithFlow.Reconcile.ConsistentIndexIsLeftAlone", TestFlags)
bool FMonolithFlowReconcileStableTest::RunTest(const FString& Parameters)
{
	FTempIndex Index;
	UFlowAsset* FlowAsset = NewFlowAsset(TEXT("FLOW_Stable"));
	AddNode(FlowAsset, UFlowNode_Log::StaticClass(), 300, 0);
	Index.Index(FlowAsset);
	const int64 PinsBefore = Index.Count(TEXT("SELECT COUNT(*) FROM flow_node_pins"));

	Index.Indexer().ReconcileWithAssets(Index.Database(), /*bFollowRenames=*/true, /*bSweepOrphans=*/true);
	UTEST_EQUAL("asset kept", Index.Count(TEXT("SELECT COUNT(*) FROM flow_assets")), 1ll);
	UTEST_EQUAL("nodes kept", NodeRowsOf(Index, FlowAsset), 2ll);
	UTEST_EQUAL("pins kept", Index.Count(TEXT("SELECT COUNT(*) FROM flow_node_pins")), PinsBefore);

	Discard(FlowAsset);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithFlowNestedAddOnsTest, "MonolithFlow.Index.NestedAddOnsKeepTheirTree", TestFlags)
bool FMonolithFlowNestedAddOnsTest::RunTest(const FString& Parameters)
{
	FTempIndex Index;
	// Branch with a predicate tree AND(NOT, NOT), attached the way the editor's context menu does it.
	UFlowAsset* FlowAsset = NewFlowAsset(TEXT("FLOW_AddOns"));
	UFlowNode* Branch = AddNode(FlowAsset, UFlowNode_Branch::StaticClass(), 300, 0);
	UFlowGraphNode* BranchGraphNode = Cast<UFlowGraphNode>(Branch->GetGraphNode());
	UTEST_NOT_NULL("branch wrapper", BranchGraphNode);
	auto Attach = [FlowAsset](UFlowGraphNode* Parent, UClass* AddOnClass)
	{
		UFlowNodeAddOn* AddOn = NewObject<UFlowNodeAddOn>(FlowAsset, AddOnClass, NAME_None, RF_Transactional);
		return FFlowSchemaAction_NewSubNode::RecreateNode(FlowAsset->GetGraph(), nullptr, Parent, AddOn);
	};
	UFlowGraphNode* And = Attach(BranchGraphNode, UFlowNodeAddOn_PredicateAND::StaticClass());
	UTEST_NOT_NULL("AND attached", And);
	UTEST_NOT_NULL("first NOT attached", Attach(And, UFlowNodeAddOn_PredicateNOT::StaticClass()));
	UTEST_NOT_NULL("second NOT attached", Attach(And, UFlowNodeAddOn_PredicateNOT::StaticClass()));
	UTEST_EQUAL("runtime tree built", Branch->GetFlowNodeAddOnChildren().Num(), 1);

	const int64 AssetId = Index.Index(FlowAsset);
	UTEST_TRUE("indexed", AssetId > 0);
	const FString Owner = Q(GuidOf(Branch));
	UTEST_EQUAL("three addon rows", Index.Count(FString::Printf(TEXT("SELECT COUNT(*) FROM flow_node_addons WHERE owner_node_guid = %s"), *Owner)), 3ll);
	UTEST_EQUAL("AND is top level", Index.Text(FString::Printf(
		TEXT("SELECT addon_class FROM flow_node_addons WHERE owner_node_guid = %s AND depth = 0 AND parent_addon_id IS NULL"), *Owner)),
		UFlowNodeAddOn_PredicateAND::StaticClass()->GetPathName());
	UTEST_EQUAL("both NOTs hang under AND, in order", Index.Count(FString::Printf(TEXT(
		"SELECT COUNT(*) FROM flow_node_addons c JOIN flow_node_addons p ON c.parent_addon_id = p.id "
		"WHERE c.owner_node_guid = %s AND c.depth = 1 AND p.depth = 0 AND c.addon_index IN (0, 1)"), *Owner)), 2ll);
	UTEST_EQUAL("node counts the whole tree", Index.Count(FString::Printf(
		TEXT("SELECT addon_count FROM flow_nodes WHERE node_guid = %s"), *Owner)), 3ll);
	UTEST_EQUAL("asset counts it too", Index.Count(FString::Printf(
		TEXT("SELECT addon_count FROM flow_assets WHERE fa_asset_id = %lld"), AssetId)), 3ll);
	UTEST_EQUAL("addon classes registered", Index.Count(TEXT("SELECT COUNT(*) FROM flow_node_classes WHERE kind = 'addon'")), 2ll);

	FMonolithFlowActions::FScopedDatabaseOverride Override(Index.Raw());
	const FMonolithActionResult Listed = FMonolithFlowActions::ListAddons(Json(FString::Printf(
		TEXT("{\"asset_path\":\"%s\",\"node_guid\":\"%s\"}"), *ObjectPathOf(FlowAsset), *GuidOf(Branch).ToLower())));
	UTEST_TRUE("list_addons ok", Listed.bSuccess);
	int32 WithParent = 0, Rows = 0;
	const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
	if (Listed.Result->TryGetArrayField(TEXT("addons"), Items))
	{
		for (const TSharedPtr<FJsonValue>& Item : *Items)
		{
			++Rows;
			WithParent += Item->AsObject()->HasField(TEXT("parent_addon_id")) ? 1 : 0;
		}
	}
	UTEST_EQUAL("list_addons returns the tree", Rows, 3);
	UTEST_EQUAL("only nested rows carry a parent", WithParent, 2);
	Discard(FlowAsset);
	return true;
}

#endif
