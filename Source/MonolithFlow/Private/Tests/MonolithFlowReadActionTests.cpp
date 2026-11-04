#include "MonolithFlowTestUtils.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_FLOW

#include "Misc/AutomationTest.h"
#include "MonolithFlowActions.h"
#include "FlowAsset.h"
#include "Nodes/FlowNode.h"
#include "Nodes/Developer/FlowNode_Log.h"
#include "Nodes/Graph/FlowNode_SubGraph.h"
#include "Graph/FlowGraphSchema_Actions.h"
#include "Graph/Nodes/FlowGraphNode.h"
#include "Serialization/JsonSerializer.h"
#include "SQLiteDatabase.h"
#include "UObject/Package.h"

using namespace MonolithFlowTests;

namespace
{
	constexpr EAutomationTestFlags TestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

	using FHandler = FMonolithActionResult (*)(const TSharedPtr<FJsonObject>&);

	FMonolithActionResult Call(FHandler Handler, const FString& ParamsJson)
	{
		return Handler(Json(ParamsJson));
	}

	FString PathParam(const UObject* Asset, const TCHAR* Key = TEXT("asset_path"))
	{
		return FString::Printf(TEXT("\"%s\":\"%s\""), Key, *ObjectPathOf(Asset));
	}

	TArray<FString> StringsOf(const TSharedPtr<FJsonObject>& Object, const TCHAR* ArrayField, const TCHAR* ItemField)
	{
		TArray<FString> Out;
		const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
		if (Object.IsValid() && Object->TryGetArrayField(ArrayField, Items))
		{
			for (const TSharedPtr<FJsonValue>& Item : *Items)
			{
				const TSharedPtr<FJsonObject>* ItemObject = nullptr;
				FString Value;
				if (Item->TryGetObject(ItemObject) && (*ItemObject)->TryGetStringField(ItemField, Value))
				{
					Out.Add(Value);
				}
				else if (Item->TryGetString(Value))
				{
					Out.Add(Value);
				}
			}
		}
		return Out;
	}

	/** Three graphs: Main (Start -> A -> B, C unreachable, A has message "needle%"), Other (calls Main as a subgraph), Empty. */
	struct FReadFixture
	{
		FTempIndex Index;
		UFlowAsset* Main = nullptr;
		UFlowAsset* Other = nullptr;
		UFlowAsset* Empty = nullptr;
		UFlowNode* A = nullptr;
		UFlowNode* B = nullptr;
		UFlowNode* C = nullptr;
		UFlowNode* Caller = nullptr;

		FReadFixture()
		{
			Main = NewFlowAsset(TEXT("FLOW_ReadMain"));
			A = AddNode(Main, UFlowNode_Log::StaticClass(), 300, 0);
			B = AddNode(Main, UFlowNode_Log::StaticClass(), 600, 0);
			C = AddNode(Main, UFlowNode_Log::StaticClass(), 300, 300);
			Connect(StartNodeOf(Main), TEXT("Out"), A);
			Connect(A, TEXT("Out"), B);
			if (FStrProperty* Message = CastField<FStrProperty>(A->GetClass()->FindPropertyByName(TEXT("Message"))))
			{
				Message->SetPropertyValue_InContainer(A, TEXT("find the needle% here"));
			}

			Other = NewFlowAsset(TEXT("FLOW_ReadOther"));
			Caller = AddNode(Other, UFlowNode_SubGraph::StaticClass(), 300, 0);
			if (FSoftObjectProperty* Prop = CastField<FSoftObjectProperty>(Caller->GetClass()->FindPropertyByName(TEXT("Asset"))))
			{
				Prop->SetPropertyValue_InContainer(Caller, FSoftObjectPtr(Main));
			}

			Empty = NewFlowAsset(TEXT("FLOW_ReadEmpty"));
			Index.Index(Main);
			Index.Index(Other);
			Index.Index(Empty);
		}

		~FReadFixture()
		{
			Discard(Main);
			Discard(Other);
			Discard(Empty);
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithFlowListAssetsPagingTest, "MonolithFlow.Read.ListAssetsPages", TestFlags)
bool FMonolithFlowListAssetsPagingTest::RunTest(const FString& Parameters)
{
	FReadFixture Fixture;
	FMonolithFlowActions::FScopedDatabaseOverride Override(Fixture.Index.Raw());

	const FMonolithActionResult First = Call(&FMonolithFlowActions::ListAssets, TEXT("{\"limit\":2}"));
	UTEST_TRUE("page 1 ok", First.bSuccess);
	UTEST_EQUAL("page 1 count", static_cast<int32>(First.Result->GetNumberField(TEXT("count"))), 2);
	UTEST_EQUAL("total", static_cast<int32>(First.Result->GetNumberField(TEXT("total"))), 3);
	UTEST_TRUE("has_more", First.Result->GetBoolField(TEXT("has_more")));

	const FMonolithActionResult Second = Call(&FMonolithFlowActions::ListAssets, TEXT("{\"limit\":2,\"offset\":2}"));
	UTEST_EQUAL("page 2 count", static_cast<int32>(Second.Result->GetNumberField(TEXT("count"))), 1);
	UTEST_FALSE("last page", Second.Result->GetBoolField(TEXT("has_more")));

	const TArray<FString> PageOne = StringsOf(First.Result, TEXT("assets"), TEXT("fa_path"));
	const TArray<FString> PageTwo = StringsOf(Second.Result, TEXT("assets"), TEXT("fa_path"));
	UTEST_FALSE("pages do not overlap", PageOne.Contains(PageTwo.IsEmpty() ? FString() : PageTwo[0]));

	UTEST_FALSE("limit 0 rejected", Call(&FMonolithFlowActions::ListAssets, TEXT("{\"limit\":0}")).bSuccess);
	UTEST_FALSE("negative offset rejected", Call(&FMonolithFlowActions::ListAssets, TEXT("{\"offset\":-1}")).bSuccess);

	const FMonolithActionResult Filtered = Call(&FMonolithFlowActions::ListAssets, TEXT("{\"asset_path_filter\":\"*ReadM*\"}"));
	UTEST_EQUAL("glob filter", static_cast<int32>(Filtered.Result->GetNumberField(TEXT("total"))), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithFlowPackagePathTest, "MonolithFlow.Read.PackagePathAccepted", TestFlags)
bool FMonolithFlowPackagePathTest::RunTest(const FString& Parameters)
{
	FReadFixture Fixture;
	FMonolithFlowActions::FScopedDatabaseOverride Override(Fixture.Index.Raw());

	const FString PackagePath = Fixture.Main->GetPackage()->GetName();
	const FMonolithActionResult Info = Call(&FMonolithFlowActions::GetAssetInfo, FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *PackagePath));
	UTEST_TRUE("package path resolves", Info.bSuccess);
	UTEST_EQUAL("answer echoes the object path", Info.Result->GetStringField(TEXT("fa_path")), ObjectPathOf(Fixture.Main));
	UTEST_EQUAL("node_count", static_cast<int32>(Info.Result->GetNumberField(TEXT("node_count"))), 4);

	UTEST_FALSE("unknown asset is an error",
		Call(&FMonolithFlowActions::GetAssetInfo, TEXT("{\"asset_path\":\"/Temp/MonolithFlowTests/Nope\"}")).bSuccess);
	UTEST_FALSE("missing param is an error", Call(&FMonolithFlowActions::GetAssetInfo, TEXT("{}")).bSuccess);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithFlowWalkExecutionTest, "MonolithFlow.Read.WalkExecutionFindsUnreachable", TestFlags)
bool FMonolithFlowWalkExecutionTest::RunTest(const FString& Parameters)
{
	FReadFixture Fixture;
	FMonolithFlowActions::FScopedDatabaseOverride Override(Fixture.Index.Raw());

	const FMonolithActionResult Walk = Call(&FMonolithFlowActions::WalkExecution, TEXT("{") + PathParam(Fixture.Main) + TEXT("}"));
	UTEST_TRUE("ok", Walk.bSuccess);
	const FString Text = [&]
	{
		FString Out;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Walk.Result.ToSharedRef(), Writer);
		return Out;
	}();
	const TArray<FString> Unreachable = StringsOf(Walk.Result, TEXT("unreachable"), TEXT("node_guid"));
	UTEST_EQUAL("one unreachable node", Unreachable.Num(), 1);
	UTEST_EQUAL("it is C", Unreachable.IsEmpty() ? FString() : Unreachable[0], GuidOf(Fixture.C));
	UTEST_TRUE("B is reached", Text.Contains(GuidOf(Fixture.B)));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithFlowExportGraphTest, "MonolithFlow.Read.ExportGraphMatchesTheAsset", TestFlags)
bool FMonolithFlowExportGraphTest::RunTest(const FString& Parameters)
{
	FReadFixture Fixture;
	FMonolithFlowActions::FScopedDatabaseOverride Override(Fixture.Index.Raw());

	const FMonolithActionResult Export = Call(&FMonolithFlowActions::ExportGraph, TEXT("{") + PathParam(Fixture.Main) + TEXT(",\"include_data\":true}"));
	UTEST_TRUE("ok", Export.bSuccess);
	const TArray<FString> Guids = StringsOf(Export.Result, TEXT("nodes"), TEXT("node_guid"));
	UTEST_EQUAL("every node", Guids.Num(), Fixture.Main->GetNodes().Num());
	for (const TPair<FGuid, UFlowNode*>& Pair : Fixture.Main->GetNodes())
	{
		TestTrue(TEXT("node exported under its key"), Guids.Contains(Pair.Key.ToString(EGuidFormats::DigitsWithHyphens)));
	}
	UTEST_EQUAL("both edges", StringsOf(Export.Result, TEXT("edges"), TEXT("from")).Num(), 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithFlowDiffAssetsTest, "MonolithFlow.Read.DiffAssetsReportsStructure", TestFlags)
bool FMonolithFlowDiffAssetsTest::RunTest(const FString& Parameters)
{
	FReadFixture Fixture;
	FMonolithFlowActions::FScopedDatabaseOverride Override(Fixture.Index.Raw());

	const FMonolithActionResult Same = Call(&FMonolithFlowActions::DiffAssets, FString::Printf(
		TEXT("{\"asset_path_a\":\"%s\",\"asset_path_b\":\"%s\"}"), *ObjectPathOf(Fixture.Main), *ObjectPathOf(Fixture.Main)));
	UTEST_TRUE("self diff ok", Same.bSuccess);
	UTEST_TRUE("self diff is identical", Same.Result->HasField(TEXT("identical")) ? Same.Result->GetBoolField(TEXT("identical")) : true);

	const FMonolithActionResult Diff = Call(&FMonolithFlowActions::DiffAssets, FString::Printf(
		TEXT("{\"asset_path_a\":\"%s\",\"asset_path_b\":\"%s\"}"), *ObjectPathOf(Fixture.Main), *ObjectPathOf(Fixture.Empty)));
	UTEST_TRUE("diff ok", Diff.bSuccess);
	UTEST_EQUAL("nodes only in A", StringsOf(Diff.Result, TEXT("nodes_only_in_a"), TEXT("node_guid")).Num(), 4);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithFlowReverseLookupTest, "MonolithFlow.Read.ReverseLookups", TestFlags)
bool FMonolithFlowReverseLookupTest::RunTest(const FString& Parameters)
{
	FReadFixture Fixture;
	FMonolithFlowActions::FScopedDatabaseOverride Override(Fixture.Index.Raw());

	const FMonolithActionResult Callers = Call(&FMonolithFlowActions::FindSubgraphCallers,
		FString::Printf(TEXT("{\"target_asset_path\":\"%s\"}"), *Fixture.Main->GetPackage()->GetName()));
	UTEST_TRUE("callers ok", Callers.bSuccess);
	UTEST_EQUAL("Other calls Main", StringsOf(Callers.Result, TEXT("callers"), TEXT("host_fa_path")),
		TArray<FString>{ ObjectPathOf(Fixture.Other) });

	const FMonolithActionResult Usages = Call(&FMonolithFlowActions::FindNodeClassUsages, TEXT("{\"node_class\":\"/Script/Flow.FlowNode_Log\"}"));
	UTEST_TRUE("usages ok", Usages.bSuccess);
	UTEST_EQUAL("only Main uses Log", StringsOf(Usages.Result, TEXT("hosts"), TEXT("fa_path")),
		TArray<FString>{ ObjectPathOf(Fixture.Main) });

	const FMonolithActionResult Found = Call(&FMonolithFlowActions::FindNodesByProperty, TEXT("{\"value_substring\":\"needle%\",\"property_name\":\"message\"}"));
	UTEST_TRUE("property search ok", Found.bSuccess);
	FString FoundText;
	{
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&FoundText);
		FJsonSerializer::Serialize(Found.Result.ToSharedRef(), Writer);
	}
	UTEST_TRUE("finds A", FoundText.Contains(GuidOf(Fixture.A)));
	UTEST_FALSE("percent is literal, B's empty message does not match", FoundText.Contains(GuidOf(Fixture.B)));

	const FMonolithActionResult Wildcard = Call(&FMonolithFlowActions::FindNodesByProperty, TEXT("{\"value_substring\":\"needle%%here\"}"));
	FString WildcardText;
	{
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&WildcardText);
		FJsonSerializer::Serialize(Wildcard.Result.ToSharedRef(), Writer);
	}
	UTEST_FALSE("no LIKE wildcard expansion", WildcardText.Contains(GuidOf(Fixture.A)));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithFlowDiffDuplicateTest, "MonolithFlow.Read.DiffOfEditorDuplicateSharesNoNodes", TestFlags)
bool FMonolithFlowDiffDuplicateTest::RunTest(const FString& Parameters)
{
	FReadFixture Fixture;
	// The editor's Duplicate goes through the same call; Flow gives every node of the copy a new guid.
	UPackage* CopyPackage = CreatePackage(TEXT("/Temp/MonolithFlowTests/FLOW_ReadCopy"));
	CopyPackage->SetFlags(RF_Transient);
	UFlowAsset* Copy = Cast<UFlowAsset>(StaticDuplicateObject(Fixture.Main, CopyPackage, TEXT("FLOW_ReadCopy")));
	UTEST_NOT_NULL("duplicated", Copy);
	UTEST_EQUAL("same node count", Copy->GetNodes().Num(), Fixture.Main->GetNodes().Num());
	for (const TPair<FGuid, UFlowNode*>& Pair : Copy->GetNodes())
	{
		TestFalse(TEXT("copy keys are new"), Fixture.Main->GetNodes().Contains(Pair.Key));
		TestEqual(TEXT("copy is consistent: key == NodeGuid"), Pair.Value ? Pair.Value->GetGuid() : FGuid(), Pair.Key);
	}
	Fixture.Index.Index(Copy);
	FMonolithFlowActions::FScopedDatabaseOverride Override(Fixture.Index.Raw());

	const FMonolithActionResult Diff = Call(&FMonolithFlowActions::DiffAssets, FString::Printf(
		TEXT("{\"asset_path_a\":\"%s\",\"asset_path_b\":\"%s\"}"), *ObjectPathOf(Fixture.Main), *ObjectPathOf(Copy)));
	UTEST_TRUE("ok", Diff.bSuccess);
	UTEST_EQUAL("nothing shared", static_cast<int32>(Diff.Result->GetNumberField(TEXT("shared_node_count"))), 0);
	UTEST_TRUE("explains why", Diff.Result->HasField(TEXT("note")));
	UTEST_EQUAL("every node of B is new", StringsOf(Diff.Result, TEXT("nodes_only_in_b"), TEXT("node_guid")).Num(), 4);
	UTEST_EQUAL("copy's edges point at its own nodes", StringsOf(Diff.Result, TEXT("edges_added"), TEXT("from")).Num(), 2);
	Discard(Copy);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithFlowDiffVersionsTest, "MonolithFlow.Read.DiffMatchesVersionsByGuid", TestFlags)
bool FMonolithFlowDiffVersionsTest::RunTest(const FString& Parameters)
{
	FTempIndex Index;
	// Two versions of one graph, as a copy on disk or another branch would have them: the same node keys.
	const FGuid KeyA(0x0A, 0, 0, 1), KeyB(0x0B, 0, 0, 2);
	auto Build = [&](const TCHAR* Name, const TCHAR* Message)
	{
		UFlowAsset* Asset = NewFlowAsset(Name);
		UFlowGraphNode* GraphA = FFlowGraphSchemaAction_NewNode::ImportNode(Asset->GetGraph(), nullptr, UFlowNode_Log::StaticClass(), KeyA, FVector2D(300, 0));
		UFlowGraphNode* GraphB = FFlowGraphSchemaAction_NewNode::ImportNode(Asset->GetGraph(), nullptr, UFlowNode_Log::StaticClass(), KeyB, FVector2D(600, 0));
		UFlowNode* A = Cast<UFlowNode>(GraphA->GetFlowNodeBase());
		UFlowNode* B = Cast<UFlowNode>(GraphB->GetFlowNodeBase());
		Connect(A, TEXT("Out"), B);
		if (FStrProperty* Prop = CastField<FStrProperty>(B->GetClass()->FindPropertyByName(TEXT("Message"))))
		{
			Prop->SetPropertyValue_InContainer(B, Message);
		}
		Index.Index(Asset);
		return Asset;
	};
	UFlowAsset* Before = Build(TEXT("FLOW_DiffBefore"), TEXT("old text"));
	UFlowAsset* After = Build(TEXT("FLOW_DiffAfter"), TEXT("new text"));
	FMonolithFlowActions::FScopedDatabaseOverride Override(Index.Raw());

	const FMonolithActionResult Diff = Call(&FMonolithFlowActions::DiffAssets, FString::Printf(
		TEXT("{\"asset_path_a\":\"%s\",\"asset_path_b\":\"%s\"}"), *ObjectPathOf(Before), *ObjectPathOf(After)));
	UTEST_TRUE("ok", Diff.bSuccess);
	UTEST_EQUAL("both logs shared", static_cast<int32>(Diff.Result->GetNumberField(TEXT("shared_node_count"))), 2);
	UTEST_FALSE("no note when nodes are shared", Diff.Result->HasField(TEXT("note")));
	UTEST_EQUAL("only the edited node changed", StringsOf(Diff.Result, TEXT("nodes_changed"), TEXT("node_guid")),
		TArray<FString>{ KeyB.ToString(EGuidFormats::DigitsWithHyphens) });
	const FString EdgeAB = FString::Printf(TEXT("%s.Out -> %s.In"),
		*KeyA.ToString(EGuidFormats::DigitsWithHyphens), *KeyB.ToString(EGuidFormats::DigitsWithHyphens));
	UTEST_FALSE("the shared edge is not reported", StringsOf(Diff.Result, TEXT("edges_added"), TEXT("from")).Contains(EdgeAB)
		|| StringsOf(Diff.Result, TEXT("edges_removed"), TEXT("from")).Contains(EdgeAB));
	// Each version has its own Start, made by the graph with a random guid.
	UTEST_EQUAL("start only in A", StringsOf(Diff.Result, TEXT("nodes_only_in_a"), TEXT("node_guid")).Num(), 1);

	UTEST_FALSE("unindexed side is an error", Call(&FMonolithFlowActions::DiffAssets, FString::Printf(
		TEXT("{\"asset_path_a\":\"%s\",\"asset_path_b\":\"/Temp/MonolithFlowTests/FLOW_Nope\"}"), *ObjectPathOf(Before))).bSuccess);
	Discard(Before);
	Discard(After);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithFlowWalkCycleTest, "MonolithFlow.Read.WalkExecutionCyclesAndDepth", TestFlags)
bool FMonolithFlowWalkCycleTest::RunTest(const FString& Parameters)
{
	FTempIndex Index;
	// Start -> A -> B -> A, Start's depth 0. B -> A closes the loop.
	UFlowAsset* Asset = NewFlowAsset(TEXT("FLOW_WalkCycle"));
	UFlowNode* A = AddNode(Asset, UFlowNode_Log::StaticClass(), 300, 0);
	UFlowNode* B = AddNode(Asset, UFlowNode_Log::StaticClass(), 600, 0);
	Connect(StartNodeOf(Asset), TEXT("Out"), A);
	Connect(A, TEXT("Out"), B);
	Connect(B, TEXT("Out"), A);
	Index.Index(Asset);
	FMonolithFlowActions::FScopedDatabaseOverride Override(Index.Raw());

	const FMonolithActionResult Walk = Call(&FMonolithFlowActions::WalkExecution, TEXT("{") + PathParam(Asset) + TEXT("}"));
	UTEST_TRUE("terminates", Walk.bSuccess);
	UTEST_EQUAL("each node once, in BFS order", StringsOf(Walk.Result, TEXT("reached"), TEXT("node_guid")),
		(TArray<FString>{ GuidOf(StartNodeOf(Asset)), GuidOf(A), GuidOf(B) }));
	UTEST_EQUAL("no terminal in a loop", StringsOf(Walk.Result, TEXT("terminal"), TEXT("node_guid")).Num(), 0);

	const FMonolithActionResult FromB = Call(&FMonolithFlowActions::WalkExecution, TEXT("{") + PathParam(Asset)
		+ FString::Printf(TEXT(",\"start_node_guid\":\"%s\"}"), *GuidOf(B).ToLower()));
	UTEST_TRUE("explicit start, lowercase", FromB.bSuccess);
	UTEST_EQUAL("start is unreachable from B", StringsOf(FromB.Result, TEXT("unreachable"), TEXT("node_guid")),
		TArray<FString>{ GuidOf(StartNodeOf(Asset)) });

	UTEST_FALSE("unknown start node", Call(&FMonolithFlowActions::WalkExecution, TEXT("{") + PathParam(Asset)
		+ TEXT(",\"start_node_guid\":\"00000000-0000-0000-0000-0000000000AA\"}")).bSuccess);
	Discard(Asset);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithFlowWalkDanglingTest, "MonolithFlow.Read.WalkExecutionDanglingEdgeIsTerminal", TestFlags)
bool FMonolithFlowWalkDanglingTest::RunTest(const FString& Parameters)
{
	FTempIndex Index;
	UFlowAsset* Asset = NewFlowAsset(TEXT("FLOW_WalkDangling"));
	UFlowNode* A = AddNode(Asset, UFlowNode_Log::StaticClass(), 300, 0);
	Connect(StartNodeOf(Asset), TEXT("Out"), A);
	const int64 AssetId = Index.Index(Asset);
	// A link to a node that is not in the graph, as a hand-edited or damaged asset can have.
	Index.Raw()->Execute(*FString::Printf(TEXT(
		"INSERT INTO flow_node_connections (fa_asset_id, from_node_guid, from_pin, to_node_guid, to_pin, is_orphaned) "
		"VALUES (%lld, '%s', 'Out', '00000000-0000-0000-0000-0000000000BB', 'In', 0)"), AssetId, *GuidOf(A)));
	FMonolithFlowActions::FScopedDatabaseOverride Override(Index.Raw());

	const FMonolithActionResult Walk = Call(&FMonolithFlowActions::WalkExecution, TEXT("{") + PathParam(Asset) + TEXT("}"));
	UTEST_TRUE("ok", Walk.bSuccess);
	UTEST_EQUAL("A ends the walk", StringsOf(Walk.Result, TEXT("terminal"), TEXT("node_guid")), TArray<FString>{ GuidOf(A) });
	Discard(Asset);
	return true;
}

#endif
