#include "MonolithFlowTestUtils.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_FLOW

#include "Misc/AutomationTest.h"
#include "MonolithFlowAuthorActions.h"
#include "MonolithToolRegistry.h"
#include "FlowAsset.h"
#include "Nodes/FlowNode.h"
#include "Nodes/Developer/FlowNode_Log.h"
#include "Graph/Nodes/FlowGraphNode.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphNode_Comment.h"
#include "Editor.h"
#include "Editor/Transactor.h"

using namespace MonolithFlowTests;

namespace
{
	constexpr EAutomationTestFlags TestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

	using FHandler = FMonolithActionResult (*)(const TSharedPtr<FJsonObject>&);

	/** Calls an action with "asset_path" filled in. Extra is the rest of a JSON object body, without braces. */
	FMonolithActionResult Call(FHandler Handler, const UFlowAsset* Asset, const FString& Extra = FString())
	{
		const FString Body = FString::Printf(TEXT("{\"asset_path\":\"%s\"%s%s}"),
			*Asset->GetPathName(), Extra.IsEmpty() ? TEXT("") : TEXT(","), *Extra);
		return Handler(Json(Body));
	}

	FString MessageOf(const UFlowNode* Node)
	{
		const FStrProperty* Prop = CastField<FStrProperty>(Node->GetClass()->FindPropertyByName(TEXT("Message")));
		return Prop ? Prop->GetPropertyValue_InContainer(Node) : FString();
	}

	int32 OpIndexOf(const FMonolithActionResult& Result)
	{
		const TSharedPtr<FJsonObject>* Data = nullptr;
		double Index = -1;
		if (Result.ErrorData.IsValid() && Result.ErrorData->TryGetObject(Data))
		{
			(*Data)->TryGetNumberField(TEXT("op_index"), Index);
		}
		return static_cast<int32>(Index);
	}

	FGuid UndoHead()
	{
		return GEditor && GEditor->Trans ? GEditor->Trans->GetUndoContext(false).TransactionId : FGuid();
	}

	UFlowNode* NodeByKey(UFlowAsset* Asset, const FString& Key)
	{
		FGuid Guid;
		return FGuid::Parse(Key, Guid) ? Asset->GetNodes().FindRef(Guid) : nullptr;
	}

	bool IsLinked(UFlowNode* From, FName FromPin, UFlowNode* To, FName ToPin = TEXT("In"))
	{
		const UFlowGraphNode* FromGraphNode = Cast<UFlowGraphNode>(From->GetGraphNode());
		const UEdGraphPin* Out = FromGraphNode ? FromGraphNode->FindPin(FromPin, EGPD_Output) : nullptr;
		if (!Out) return false;
		for (const UEdGraphPin* Linked : Out->LinkedTo)
		{
			if (Linked && Linked->GetOwningNode() == To->GetGraphNode() && Linked->PinName == ToPin) return true;
		}
		return false;
	}

	/** Start -> A, plus a free node B. */
	struct FEditFixture
	{
		UFlowAsset* Asset = nullptr;
		UFlowNode* Start = nullptr;
		UFlowNode* A = nullptr;
		UFlowNode* B = nullptr;

		explicit FEditFixture(const TCHAR* Name)
		{
			Asset = NewFlowAsset(Name);
			Start = StartNodeOf(Asset);
			A = AddNode(Asset, UFlowNode_Log::StaticClass(), 300, 0);
			B = AddNode(Asset, UFlowNode_Log::StaticClass(), 300, 200);
			Connect(Start, TEXT("Out"), A);
		}
		~FEditFixture() { Discard(Asset); }
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithFlowAddNodeTest, "MonolithFlow.Author.AddNodeAppliesProperties", TestFlags)
bool FMonolithFlowAddNodeTest::RunTest(const FString& Parameters)
{
	FEditFixture Fixture(TEXT("FLOW_AuthorAdd"));
	const int32 Before = Fixture.Asset->GetNodes().Num();

	const FMonolithActionResult Result = Call(&FMonolithFlowAuthorActions::AddNode, Fixture.Asset,
		TEXT("\"node_class\":\"/Script/Flow.FlowNode_Log\",\"pos_x\":900,\"pos_y\":40,\"properties\":{\"message\":\"hello\",\"duration\":2.5}"));
	UTEST_TRUE("ok", Result.bSuccess);
	UTEST_EQUAL("one more node", Fixture.Asset->GetNodes().Num(), Before + 1);
	UFlowNode* Node = NodeByKey(Fixture.Asset, Result.Result->GetStringField(TEXT("node_guid")));
	UTEST_NOT_NULL("returned guid is the map key", Node);
	UTEST_EQUAL("message applied", MessageOf(Node), FString(TEXT("hello")));
	UTEST_EQUAL("position", Node->GetGraphNode()->NodePosX, 900);
	UTEST_TRUE("pins reported", Result.Result->GetArrayField(TEXT("output_pins")).Num() > 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithFlowAddNodeRejectsTest, "MonolithFlow.Author.InvalidPropertiesChangeNothing", TestFlags)
bool FMonolithFlowAddNodeRejectsTest::RunTest(const FString& Parameters)
{
	FEditFixture Fixture(TEXT("FLOW_AuthorReject"));
	const int32 Before = Fixture.Asset->GetNodes().Num();
	const FGuid HeadBefore = UndoHead();

	struct FCase { const TCHAR* Extra; const TCHAR* Code; };
	const FCase Cases[] =
	{
		{ TEXT("\"node_class\":\"/Script/Flow.FlowNode_Log\",\"properties\":{\"mesage\":\"typo\"}"), TEXT("unknown_property") },
		{ TEXT("\"node_class\":\"/Script/Flow.FlowNode_Log\",\"properties\":{\"nodeGuid\":\"00000000-0000-0000-0000-000000000000\"}"), TEXT("read_only_property") },
		{ TEXT("\"node_class\":\"/Script/Flow.FlowNode_Log\",\"properties\":{\"bPrintToScreen\":{\"x\":1}}"), TEXT("wrong_type") },
		{ TEXT("\"node_class\":\"/Script/Flow.FlowNode_Log\",\"properties\":{\"duration\":\"long\"}"), TEXT("wrong_type") },
		{ TEXT("\"node_class\":\"/Script/Flow.FlowNode_Log\",\"properties\":{\"textColor\":\"not a color\"}"), TEXT("wrong_type") },
		{ TEXT("\"node_class\":\"/Script/Flow.NoSuchNode\""), TEXT("class_not_found") },
		{ TEXT("\"node_class\":\"/Script/Engine.Actor\""), TEXT("class_not_found") },
	};
	for (const FCase& Case : Cases)
	{
		const FMonolithActionResult Result = Call(&FMonolithFlowAuthorActions::AddNode, Fixture.Asset, Case.Extra);
		TestFalse(FString::Printf(TEXT("%s fails"), Case.Code), Result.bSuccess);
		TestEqual(FString::Printf(TEXT("code for %s"), Case.Extra), ErrorCodeOf(Result), FString(Case.Code));
	}
	UTEST_EQUAL("no node added", Fixture.Asset->GetNodes().Num(), Before);
	UTEST_EQUAL("no transaction recorded", UndoHead(), HeadBefore);

	const FString Original = MessageOf(Fixture.A);
	const FMonolithActionResult Set = Call(&FMonolithFlowAuthorActions::SetNodeProperties, Fixture.Asset, FString::Printf(
		TEXT("\"node_guid\":\"%s\",\"properties\":{\"message\":\"changed\",\"bPrintToScreen\":\"yes\"}"), *GuidOf(Fixture.A)));
	UTEST_FALSE("mixed valid and invalid is rejected whole", Set.bSuccess);
	UTEST_EQUAL("message untouched", MessageOf(Fixture.A), Original);

	UTEST_EQUAL("missing node", ErrorCodeOf(Call(&FMonolithFlowAuthorActions::SetNodeProperties, Fixture.Asset,
		TEXT("\"node_guid\":\"11111111-2222-3333-4444-555555555555\",\"properties\":{}"))), FString(TEXT("node_not_found")));
	UTEST_EQUAL("missing asset", ErrorCodeOf(FMonolithFlowAuthorActions::AddNode(Json(
		TEXT("{\"asset_path\":\"/Temp/MonolithFlowTests/Nope\",\"node_class\":\"/Script/Flow.FlowNode_Log\"}")))), FString(TEXT("asset_not_found")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithFlowConnectTest, "MonolithFlow.Author.ConnectReportsReplacedLinks", TestFlags)
bool FMonolithFlowConnectTest::RunTest(const FString& Parameters)
{
	FEditFixture Fixture(TEXT("FLOW_AuthorConnect"));
	const FMonolithActionResult Result = Call(&FMonolithFlowAuthorActions::ConnectPins, Fixture.Asset, FString::Printf(
		TEXT("\"from_node_guid\":\"%s\",\"from_pin\":\"Out\",\"to_node_guid\":\"%s\""), *GuidOf(Fixture.Start), *GuidOf(Fixture.B)));
	UTEST_TRUE("ok", Result.bSuccess);
	UTEST_TRUE("Start -> B", IsLinked(Fixture.Start, TEXT("Out"), Fixture.B));
	UTEST_FALSE("Start -> A replaced", IsLinked(Fixture.Start, TEXT("Out"), Fixture.A));
	const TArray<TSharedPtr<FJsonValue>> Broken = Result.Result->GetArrayField(TEXT("broken_links"));
	UTEST_EQUAL("one broken link reported", Broken.Num(), 1);
	UTEST_TRUE("it names A", Broken[0]->AsString().Contains(GuidOf(Fixture.A)));

	UTEST_EQUAL("unknown pin", ErrorCodeOf(Call(&FMonolithFlowAuthorActions::ConnectPins, Fixture.Asset, FString::Printf(
		TEXT("\"from_node_guid\":\"%s\",\"from_pin\":\"Nope\",\"to_node_guid\":\"%s\""), *GuidOf(Fixture.A), *GuidOf(Fixture.B)))),
		FString(TEXT("pin_not_found")));
	UTEST_TRUE("still Start -> B after the failed call", IsLinked(Fixture.Start, TEXT("Out"), Fixture.B));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithFlowRemoveDisconnectTest, "MonolithFlow.Author.RemoveAndDisconnect", TestFlags)
bool FMonolithFlowRemoveDisconnectTest::RunTest(const FString& Parameters)
{
	FEditFixture Fixture(TEXT("FLOW_AuthorRemove"));
	const FString KeyA = GuidOf(Fixture.A);

	const FMonolithActionResult Disconnect = Call(&FMonolithFlowAuthorActions::DisconnectPins, Fixture.Asset, FString::Printf(
		TEXT("\"node_guid\":\"%s\",\"pin_name\":\"Out\""), *GuidOf(Fixture.Start)));
	UTEST_TRUE("disconnect ok", Disconnect.bSuccess);
	UTEST_EQUAL("one link broken", static_cast<int32>(Disconnect.Result->GetNumberField(TEXT("links_broken"))), 1);
	UTEST_FALSE("Start no longer drives A", IsLinked(Fixture.Start, TEXT("Out"), Fixture.A));
	UTEST_TRUE("runtime connections harvested", Fixture.Start->GetConnection(TEXT("Out")).NodeGuid != Fixture.A->GetGuid());

	const FMonolithActionResult Remove = Call(&FMonolithFlowAuthorActions::RemoveNode, Fixture.Asset, FString::Printf(TEXT("\"node_guid\":\"%s\""), *KeyA));
	UTEST_TRUE("remove ok", Remove.bSuccess);
	UTEST_NULL("A unregistered from the asset", NodeByKey(Fixture.Asset, KeyA));
	UTEST_EQUAL("removed guid echoed", Remove.Result->GetStringField(TEXT("removed_node_guid")), KeyA);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithFlowCommentTest, "MonolithFlow.Author.AddComment", TestFlags)
bool FMonolithFlowCommentTest::RunTest(const FString& Parameters)
{
	FEditFixture Fixture(TEXT("FLOW_AuthorComment"));
	const FMonolithActionResult Result = Call(&FMonolithFlowAuthorActions::AddComment, Fixture.Asset,
		TEXT("\"text\":\"Gate logic\",\"pos_x\":10,\"pos_y\":20,\"width\":500,\"height\":300"));
	UTEST_TRUE("ok", Result.bSuccess);
	int32 Found = 0;
	for (const UEdGraphNode* Node : Fixture.Asset->GetGraph()->Nodes)
	{
		if (const UEdGraphNode_Comment* Comment = Cast<UEdGraphNode_Comment>(Node))
		{
			Found += Comment->NodeComment == TEXT("Gate logic") && Comment->NodeWidth == 500 ? 1 : 0;
		}
	}
	UTEST_EQUAL("comment box in the graph", Found, 1);
	return true;
}

IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(FMonolithFlowUndoTest, FUndoingTestBase, "MonolithFlow.Author.EditsAreUndoable", TestFlags)
bool FMonolithFlowUndoTest::RunTest(const FString& Parameters)
{
	FEditFixture Fixture(TEXT("FLOW_AuthorUndo"));
	const int32 Before = Fixture.Asset->GetNodes().Num();
	UTEST_TRUE("add", Call(&FMonolithFlowAuthorActions::AddNode, Fixture.Asset, TEXT("\"node_class\":\"/Script/Flow.FlowNode_Log\"")).bSuccess);
	UTEST_EQUAL("added", Fixture.Asset->GetNodes().Num(), Before + 1);
	UTEST_TRUE("editor undo", GEditor->UndoTransaction());
	UTEST_EQUAL("undone", Fixture.Asset->GetNodes().Num(), Before);
	return true;
}

IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(FMonolithFlowDryRunTest, FUndoingTestBase, "MonolithFlow.Author.DryRunLeavesNoTrace", TestFlags)
bool FMonolithFlowDryRunTest::RunTest(const FString& Parameters)
{
	FEditFixture Fixture(TEXT("FLOW_AuthorDryRun"));
	const int32 Before = Fixture.Asset->GetNodes().Num();
	const FGuid HeadBefore = UndoHead();

	const FMonolithActionResult Add = Call(&FMonolithFlowAuthorActions::AddNode, Fixture.Asset,
		TEXT("\"node_class\":\"/Script/Flow.FlowNode_Log\",\"dry_run\":true"));
	UTEST_TRUE("ok", Add.bSuccess);
	UTEST_TRUE("flagged", Add.Result->GetBoolField(TEXT("dry_run")));
	UTEST_TRUE("rolled back", Add.Result->GetBoolField(TEXT("rolled_back")));
	UTEST_EQUAL("graph unchanged", Fixture.Asset->GetNodes().Num(), Before);

	const FMonolithActionResult Connect = Call(&FMonolithFlowAuthorActions::ConnectPins, Fixture.Asset, FString::Printf(
		TEXT("\"from_node_guid\":\"%s\",\"from_pin\":\"Out\",\"to_node_guid\":\"%s\",\"dry_run\":true"), *GuidOf(Fixture.Start), *GuidOf(Fixture.B)));
	UTEST_TRUE("dry connect ok", Connect.bSuccess);
	UTEST_EQUAL("would break Start -> A", Connect.Result->GetArrayField(TEXT("broken_links")).Num(), 1);
	UTEST_TRUE("Start -> A still there", IsLinked(Fixture.Start, TEXT("Out"), Fixture.A));
	UTEST_EQUAL("undo history unchanged", UndoHead(), HeadBefore);
	return true;
}

// ---------------------------------------------------------------------------
// apply_batch
// ---------------------------------------------------------------------------

IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(FMonolithFlowBatchTest, FUndoingTestBase, "MonolithFlow.Batch.CreatesAndWiresWithAliases", TestFlags)
bool FMonolithFlowBatchTest::RunTest(const FString& Parameters)
{
	FEditFixture Fixture(TEXT("FLOW_BatchWire"));
	const int32 Before = Fixture.Asset->GetNodes().Num();
	const FString Ops = FString::Printf(TEXT("\"operations\":["
		"{\"op\":\"add_node\",\"alias\":\"first\",\"node_class\":\"/Script/Flow.FlowNode_Log\",\"pos_x\":600},"
		"{\"op\":\"add_node\",\"alias\":\"second\",\"node_class\":\"/Script/Flow.FlowNode_Log\",\"pos_x\":900},"
		"{\"op\":\"connect_pins\",\"from_node_guid\":\"%s\",\"from_pin\":\"Out\",\"to_node_guid\":\"$first\"},"
		"{\"op\":\"connect_pins\",\"from_node_guid\":\"$first\",\"from_pin\":\"Out\",\"to_node_guid\":\"$second\"},"
		"{\"op\":\"set_node_properties\",\"node_guid\":\"$second\",\"properties\":{\"message\":\"end\"}},"
		"{\"op\":\"add_comment\",\"text\":\"batch\"}]"), *GuidOf(Fixture.A));

	const FMonolithActionResult Result = Call(&FMonolithFlowAuthorActions::ApplyBatch, Fixture.Asset, Ops);
	UTEST_TRUE("ok", Result.bSuccess);
	UTEST_EQUAL("six results", Result.Result->GetArrayField(TEXT("results")).Num(), 6);
	const TSharedPtr<FJsonObject> Aliases = Result.Result->GetObjectField(TEXT("aliases"));
	UFlowNode* First = NodeByKey(Fixture.Asset, Aliases->GetStringField(TEXT("first")));
	UFlowNode* Second = NodeByKey(Fixture.Asset, Aliases->GetStringField(TEXT("second")));
	UTEST_NOT_NULL("first created", First);
	UTEST_NOT_NULL("second created", Second);
	UTEST_EQUAL("two nodes added", Fixture.Asset->GetNodes().Num(), Before + 2);
	UTEST_TRUE("A -> first", IsLinked(Fixture.A, TEXT("Out"), First));
	UTEST_TRUE("first -> second", IsLinked(First, TEXT("Out"), Second));
	UTEST_EQUAL("alias resolved for set_node_properties", MessageOf(Second), FString(TEXT("end")));

	UTEST_TRUE("one undo reverts the whole batch", GEditor->UndoTransaction());
	UTEST_EQUAL("all gone", Fixture.Asset->GetNodes().Num(), Before);
	return true;
}

IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(FMonolithFlowBatchAtomicTest, FUndoingTestBase, "MonolithFlow.Batch.FailureUndoesEverything", TestFlags)
bool FMonolithFlowBatchAtomicTest::RunTest(const FString& Parameters)
{
	FEditFixture Fixture(TEXT("FLOW_BatchAtomic"));
	const int32 Before = Fixture.Asset->GetNodes().Num();
	const FString OriginalMessage = MessageOf(Fixture.A);

	// Fails while applying (the pin only exists once the node does), after two operations already landed.
	const FString Ops = FString::Printf(TEXT("\"operations\":["
		"{\"op\":\"add_node\",\"alias\":\"n\",\"node_class\":\"/Script/Flow.FlowNode_Log\"},"
		"{\"op\":\"set_node_properties\",\"node_guid\":\"%s\",\"properties\":{\"message\":\"touched\"}},"
		"{\"op\":\"connect_pins\",\"from_node_guid\":\"$n\",\"from_pin\":\"NoSuchPin\",\"to_node_guid\":\"%s\"}]"),
		*GuidOf(Fixture.A), *GuidOf(Fixture.B));
	const FMonolithActionResult Result = Call(&FMonolithFlowAuthorActions::ApplyBatch, Fixture.Asset, Ops);
	UTEST_FALSE("fails", Result.bSuccess);
	UTEST_EQUAL("code", ErrorCodeOf(Result), FString(TEXT("pin_not_found")));
	UTEST_EQUAL("failing operation", OpIndexOf(Result), 2);
	UTEST_EQUAL("node creation undone", Fixture.Asset->GetNodes().Num(), Before);
	UTEST_EQUAL("property change undone", MessageOf(Fixture.A), OriginalMessage);
	UTEST_TRUE("Start -> A intact", IsLinked(Fixture.Start, TEXT("Out"), Fixture.A));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithFlowBatchValidationTest, "MonolithFlow.Batch.ValidationBeforeAnyChange", TestFlags)
bool FMonolithFlowBatchValidationTest::RunTest(const FString& Parameters)
{
	FEditFixture Fixture(TEXT("FLOW_BatchValidate"));
	const int32 Before = Fixture.Asset->GetNodes().Num();
	const FGuid HeadBefore = UndoHead();
	const FString KeyA = GuidOf(Fixture.A);

	struct FCase { FString Ops; const TCHAR* Code; int32 OpIndex; };
	const FCase Cases[] =
	{
		{ TEXT("[{\"op\":\"add_node\",\"node_class\":\"/Script/Flow.FlowNode_Log\"},{\"op\":\"explode\"}]"), TEXT("unknown_op"), 1 },
		{ TEXT("[{\"op\":\"set_node_properties\",\"node_guid\":\"$later\",\"properties\":{}},{\"op\":\"add_node\",\"alias\":\"later\",\"node_class\":\"/Script/Flow.FlowNode_Log\"}]"), TEXT("unknown_alias"), 0 },
		{ TEXT("[{\"op\":\"add_node\",\"alias\":\"x\",\"node_class\":\"/Script/Flow.FlowNode_Log\"},{\"op\":\"add_node\",\"alias\":\"x\",\"node_class\":\"/Script/Flow.FlowNode_Log\"}]"), TEXT("duplicate_alias"), 1 },
		{ FString::Printf(TEXT("[{\"op\":\"add_node\",\"alias\":\"x\",\"node_class\":\"/Script/Flow.FlowNode_Log\"},{\"op\":\"set_node_properties\",\"node_guid\":\"$x\",\"properties\":{\"mesage\":1}},{\"op\":\"remove_node\",\"node_guid\":\"%s\"}]"), *KeyA), TEXT("unknown_property"), 1 },
		{ FString::Printf(TEXT("[{\"op\":\"remove_node\",\"node_guid\":\"%s\"},{\"op\":\"set_node_properties\",\"node_guid\":\"%s\",\"properties\":{}}]"), *KeyA, *KeyA), TEXT("node_not_found"), 1 },
		{ TEXT("[{\"op\":\"connect_pins\",\"from_pin\":\"Out\"}]"), TEXT("invalid_params"), 0 },
	};
	for (const FCase& Case : Cases)
	{
		const FMonolithActionResult Result = Call(&FMonolithFlowAuthorActions::ApplyBatch, Fixture.Asset, TEXT("\"operations\":") + Case.Ops);
		TestFalse(FString::Printf(TEXT("%s fails"), Case.Code), Result.bSuccess);
		TestEqual(FString::Printf(TEXT("code (%s)"), Case.Code), ErrorCodeOf(Result), FString(Case.Code));
		TestEqual(FString::Printf(TEXT("op_index (%s)"), Case.Code), OpIndexOf(Result), Case.OpIndex);
	}
	UTEST_EQUAL("nothing changed", Fixture.Asset->GetNodes().Num(), Before);
	UTEST_EQUAL("no transaction recorded", UndoHead(), HeadBefore);
	UTEST_EQUAL("empty batch rejected", ErrorCodeOf(Call(&FMonolithFlowAuthorActions::ApplyBatch, Fixture.Asset, TEXT("\"operations\":[]"))),
		FString(TEXT("invalid_params")));
	return true;
}

IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(FMonolithFlowBatchDryRunTest, FUndoingTestBase, "MonolithFlow.Batch.DryRunReportsAndUndoes", TestFlags)
bool FMonolithFlowBatchDryRunTest::RunTest(const FString& Parameters)
{
	FEditFixture Fixture(TEXT("FLOW_BatchDry"));
	const int32 Before = Fixture.Asset->GetNodes().Num();
	const FString Ops = FString::Printf(TEXT("\"dry_run\":true,\"operations\":["
		"{\"op\":\"add_node\",\"alias\":\"n\",\"node_class\":\"/Script/Flow.FlowNode_Log\"},"
		"{\"op\":\"connect_pins\",\"from_node_guid\":\"%s\",\"from_pin\":\"Out\",\"to_node_guid\":\"$n\"}]"), *GuidOf(Fixture.Start));
	const FMonolithActionResult Result = Call(&FMonolithFlowAuthorActions::ApplyBatch, Fixture.Asset, Ops);
	UTEST_TRUE("ok", Result.bSuccess);
	UTEST_TRUE("dry run", Result.Result->GetBoolField(TEXT("dry_run")));
	UTEST_EQUAL("reports the link it would break", Result.Result->GetArrayField(TEXT("broken_links")).Num(), 1);
	UTEST_EQUAL("graph unchanged", Fixture.Asset->GetNodes().Num(), Before);
	UTEST_TRUE("Start -> A intact", IsLinked(Fixture.Start, TEXT("Out"), Fixture.A));
	return true;
}

#endif
