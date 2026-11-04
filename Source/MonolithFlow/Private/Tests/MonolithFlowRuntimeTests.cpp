#include "MonolithFlowTestUtils.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_FLOW

#include "Misc/AutomationTest.h"
#include "MonolithFlowRuntimeActions.h"
#include "FlowAsset.h"
#include "FlowSubsystem.h"
#include "Nodes/FlowNode.h"
#include "Nodes/Route/FlowNode_Counter.h"
#include "Nodes/Route/FlowNode_Reroute.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "UObject/StrongObjectPtr.h"

using namespace MonolithFlowTests;

namespace
{
	constexpr EAutomationTestFlags TestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

	/** UGameInstance::WorldContext is protected; a game instance needs one for its world, and Initialize would start every project subsystem. */
	struct FGameInstanceWorldContext : UGameInstance
	{
		static void Set(UGameInstance& Instance, FWorldContext* Context)
		{
			Instance.*(&FGameInstanceWorldContext::WorldContext) = Context;
		}
	};

	/**
	 * A game world with a bare Flow subsystem, no PIE. The template is
	 * Start -> Reroute -> Counter.Increment: a started instance passes the
	 * reroute and stays parked in the counter (Goal 2), so it has one active
	 * node and three recorded ones.
	 */
	struct FRuntimeFixture
	{
		TStrongObjectPtr<UGameInstance> GameInstance;
		TStrongObjectPtr<UFlowSubsystem> Subsystem;
		UWorld* World = nullptr;
		UFlowAsset* Template = nullptr;
		UFlowNode* Reroute = nullptr;
		UFlowNode* Counter = nullptr;
		FNativeFlowAssetEvent SavedAdded;
		FNativeFlowAssetEvent SavedRemoved;

		FRuntimeFixture()
		{
			// The Flow debugger listens to these; keep it out of the test's instances.
			SavedAdded = UFlowSubsystem::OnInstancedTemplateAdded;
			SavedRemoved = UFlowSubsystem::OnInstancedTemplateRemoved;
			UFlowSubsystem::OnInstancedTemplateAdded.Unbind();
			UFlowSubsystem::OnInstancedTemplateRemoved.Unbind();

			GameInstance.Reset(NewObject<UGameInstance>(GEngine));
			World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld=*/false);
			World->SetGameInstance(GameInstance.Get());
			FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
			Context.OwningGameInstance = GameInstance.Get();
			Context.SetCurrentWorld(World);
			FGameInstanceWorldContext::Set(*GameInstance.Get(), &Context);
			Subsystem.Reset(NewObject<UFlowSubsystem>(GameInstance.Get()));

			Template = NewFlowAsset(TEXT("FLOW_Runtime"));
			Reroute = AddNode(Template, UFlowNode_Reroute::StaticClass(), 300, 0);
			Counter = AddNode(Template, UFlowNode_Counter::StaticClass(), 600, 0);
			Connect(StartNodeOf(Template), TEXT("Out"), Reroute);
			Connect(Reroute, TEXT("Out"), Counter, TEXT("Increment"));
		}

		~FRuntimeFixture()
		{
			Subsystem->AbortActiveFlows();
			UFlowSubsystem::OnInstancedTemplateAdded = SavedAdded;
			UFlowSubsystem::OnInstancedTemplateRemoved = SavedRemoved;
			FGameInstanceWorldContext::Set(*GameInstance.Get(), nullptr);
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(false);
			Subsystem.Reset();
			GameInstance.Reset();
			Discard(Template);
		}

		UFlowAsset* Run(UObject* Owner)
		{
			UFlowAsset* Instance = Subsystem->CreateRootFlow(Owner, Template);
			if (Instance)
			{
				Instance->StartFlow(nullptr);
			}
			return Instance;
		}
	};

	TArray<FString> Field(const TSharedPtr<FJsonObject>& Object, const TCHAR* ArrayField, const TCHAR* ItemField)
	{
		TArray<FString> Out;
		const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
		if (Object.IsValid() && Object->TryGetArrayField(ArrayField, Items))
		{
			for (const TSharedPtr<FJsonValue>& Item : *Items)
			{
				const TSharedPtr<FJsonObject>* Row = nullptr;
				FString Value;
				if (Item->TryGetObject(Row) && (*Row)->TryGetStringField(ItemField, Value))
				{
					Out.Add(Value);
				}
			}
		}
		return Out;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithFlowRuntimeNoPieTest, "MonolithFlow.Runtime.NoPieIsAnError", TestFlags)
bool FMonolithFlowRuntimeNoPieTest::RunTest(const FString& Parameters)
{
	if (GEditor && GEditor->PlayWorld)
	{
		AddInfo(TEXT("A PIE session is running; skipped."));
		return true;
	}
	const FMonolithActionResult List = FMonolithFlowRuntimeActions::RuntimeListInstances(Json(TEXT("{}")));
	UTEST_FALSE("list fails", List.bSuccess);
	UTEST_TRUE("says why", List.ErrorMessage.Contains(TEXT("PIE")));
	UTEST_FALSE("get fails", FMonolithFlowRuntimeActions::RuntimeGetInstance(Json(TEXT("{\"asset_path\":\"/Game/X\"}"))).bSuccess);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithFlowRuntimeListTest, "MonolithFlow.Runtime.ListsRootInstances", TestFlags)
bool FMonolithFlowRuntimeListTest::RunTest(const FString& Parameters)
{
	FRuntimeFixture Fixture;
	UTEST_NOT_NULL("instance started", Fixture.Run(Fixture.GameInstance.Get()));
	FMonolithFlowRuntimeActions::FScopedSubsystemOverride Override(Fixture.Subsystem.Get());

	const FMonolithActionResult List = FMonolithFlowRuntimeActions::RuntimeListInstances(Json(TEXT("{}")));
	UTEST_TRUE("ok", List.bSuccess);
	UTEST_EQUAL("one root", static_cast<int32>(List.Result->GetNumberField(TEXT("count"))), 1);
	UTEST_EQUAL("template path", Field(List.Result, TEXT("instances"), TEXT("template_asset_path")),
		TArray<FString>{ ObjectPathOf(Fixture.Template) });
	UTEST_EQUAL("owner", Field(List.Result, TEXT("instances"), TEXT("owner")),
		TArray<FString>{ Fixture.GameInstance->GetName() });

	const TSharedPtr<FJsonObject>& Row = List.Result->GetArrayField(TEXT("instances"))[0]->AsObject();
	UTEST_TRUE("active", Row->GetBoolField(TEXT("is_active")));
	UTEST_EQUAL("parked in the counter", static_cast<int32>(Row->GetNumberField(TEXT("active_node_count"))), 1);
	UTEST_EQUAL("start, reroute, counter recorded", static_cast<int32>(Row->GetNumberField(TEXT("recorded_node_count"))), 3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithFlowRuntimeGetTest, "MonolithFlow.Runtime.InstanceShowsActiveAndRecordedNodes", TestFlags)
bool FMonolithFlowRuntimeGetTest::RunTest(const FString& Parameters)
{
	FRuntimeFixture Fixture;
	UTEST_NOT_NULL("instance started", Fixture.Run(Fixture.GameInstance.Get()));
	FMonolithFlowRuntimeActions::FScopedSubsystemOverride Override(Fixture.Subsystem.Get());

	// Package path form, as for every other flow action.
	const FMonolithActionResult Info = FMonolithFlowRuntimeActions::RuntimeGetInstance(Json(FString::Printf(
		TEXT("{\"asset_path\":\"%s\"}"), *Fixture.Template->GetPackage()->GetName())));
	UTEST_TRUE("ok", Info.bSuccess);
	UTEST_EQUAL("active node is the counter, under its template key",
		Field(Info.Result, TEXT("active_nodes"), TEXT("node_guid")), TArray<FString>{ GuidOf(Fixture.Counter) });
	UTEST_EQUAL("activation state of an active node",
		Field(Info.Result, TEXT("active_nodes"), TEXT("activation_state")), TArray<FString>{ TEXT("Active") });
	UTEST_EQUAL("history in activation order", Field(Info.Result, TEXT("recorded_nodes"), TEXT("node_guid")),
		(TArray<FString>{ GuidOf(StartNodeOf(Fixture.Template)), GuidOf(Fixture.Reroute), GuidOf(Fixture.Counter) }));
	UTEST_EQUAL("passed nodes are finished, the parked one is not", Field(Info.Result, TEXT("recorded_nodes"), TEXT("activation_state")),
		(TArray<FString>{ TEXT("Completed"), TEXT("Completed"), TEXT("Active") }));

	const FMonolithActionResult Tail = FMonolithFlowRuntimeActions::RuntimeGetInstance(Json(FString::Printf(
		TEXT("{\"asset_path\":\"%s\",\"history_limit\":1}"), *ObjectPathOf(Fixture.Template))));
	UTEST_EQUAL("tail of one", Field(Tail.Result, TEXT("recorded_nodes"), TEXT("node_guid")), TArray<FString>{ GuidOf(Fixture.Counter) });
	UTEST_EQUAL("total still counted", static_cast<int32>(Tail.Result->GetNumberField(TEXT("recorded_total"))), 3);

	const FMonolithActionResult Clamped = FMonolithFlowRuntimeActions::RuntimeGetInstance(Json(FString::Printf(
		TEXT("{\"asset_path\":\"%s\",\"history_limit\":-5}"), *ObjectPathOf(Fixture.Template))));
	UTEST_EQUAL("a limit below one returns one", Field(Clamped.Result, TEXT("recorded_nodes"), TEXT("node_guid")).Num(), 1);

	UTEST_FALSE("unknown template", FMonolithFlowRuntimeActions::RuntimeGetInstance(
		Json(TEXT("{\"asset_path\":\"/Temp/MonolithFlowTests/FLOW_Nope\"}"))).bSuccess);
	UTEST_FALSE("missing asset_path", FMonolithFlowRuntimeActions::RuntimeGetInstance(Json(TEXT("{}"))).bSuccess);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithFlowRuntimeOwnersTest, "MonolithFlow.Runtime.SeveralInstancesNeedAnOwner", TestFlags)
bool FMonolithFlowRuntimeOwnersTest::RunTest(const FString& Parameters)
{
	FRuntimeFixture Fixture;
	// Any concrete object can own a root flow; the counter graph needs nothing from it.
	UObject* SecondOwner = NewObject<UGameInstance>(Fixture.GameInstance.Get(), TEXT("SecondOwner"));
	UTEST_NOT_NULL("first", Fixture.Run(Fixture.GameInstance.Get()));
	UTEST_NOT_NULL("second", Fixture.Run(SecondOwner));
	FMonolithFlowRuntimeActions::FScopedSubsystemOverride Override(Fixture.Subsystem.Get());

	UTEST_EQUAL("both listed", static_cast<int32>(FMonolithFlowRuntimeActions::RuntimeListInstances(Json(TEXT("{}")))
		.Result->GetNumberField(TEXT("count"))), 2);

	const FString Path = ObjectPathOf(Fixture.Template);
	const FMonolithActionResult Ambiguous = FMonolithFlowRuntimeActions::RuntimeGetInstance(Json(FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *Path)));
	UTEST_FALSE("no silent pick", Ambiguous.bSuccess);
	UTEST_TRUE("names the owners", Ambiguous.ErrorMessage.Contains(TEXT("SecondOwner")) && Ambiguous.ErrorMessage.Contains(Fixture.GameInstance->GetName()));

	const FMonolithActionResult Picked = FMonolithFlowRuntimeActions::RuntimeGetInstance(Json(FString::Printf(
		TEXT("{\"asset_path\":\"%s\",\"owner\":\"SecondOwner\"}"), *Path)));
	UTEST_TRUE("owner picks one", Picked.bSuccess);
	UTEST_EQUAL("the right one", Picked.Result->GetStringField(TEXT("owner")), FString(TEXT("SecondOwner")));

	UTEST_FALSE("unknown owner", FMonolithFlowRuntimeActions::RuntimeGetInstance(Json(FString::Printf(
		TEXT("{\"asset_path\":\"%s\",\"owner\":\"Nobody\"}"), *Path))).bSuccess);
	return true;
}

#endif
