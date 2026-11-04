#include "MonolithFlowTestUtils.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_FLOW

#include "Misc/AutomationTest.h"
#include "MonolithFlowActions.h"
#include "MonolithFlowAuthorActions.h"
#include "MonolithToolRegistry.h"
#include "FlowAsset.h"
#include "Nodes/FlowNode.h"
#include "Nodes/Graph/FlowNode_SubGraph.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Editor.h"
#include "UObject/Package.h"

using namespace MonolithFlowTests;

namespace
{
	constexpr EAutomationTestFlags TestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

	bool HasRequiredParam(const TSharedPtr<FJsonObject>& Schema)
	{
		if (!Schema.IsValid())
		{
			return false;
		}
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Param : Schema->Values)
		{
			const TSharedPtr<FJsonObject>* Spec = nullptr;
			bool bRequired = false;
			if (Param.Value->TryGetObject(Spec) && (*Spec)->TryGetBoolField(TEXT("required"), bRequired) && bRequired)
			{
				return true;
			}
		}
		return false;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithFlowRegistryTest, "MonolithFlow.Surface.EveryActionIsRegisteredWithASchema", TestFlags)
bool FMonolithFlowRegistryTest::RunTest(const FString& Parameters)
{
	const TArray<FString> Expected = {
		TEXT("ping"), TEXT("list_assets"), TEXT("get_asset_info"), TEXT("list_nodes"), TEXT("list_node_pins"),
		TEXT("list_connections"), TEXT("list_addons"), TEXT("get_node_info"), TEXT("list_custom_events"),
		TEXT("find_subgraph_callers"), TEXT("find_node_class_usages"), TEXT("find_pins_by_type"),
		TEXT("find_nodes_by_property"), TEXT("list_node_classes"), TEXT("walk_execution"), TEXT("export_graph"),
		TEXT("diff_assets"), TEXT("get_node_history"),
		TEXT("create_asset"), TEXT("save_asset"), TEXT("validate_asset"), TEXT("add_node"), TEXT("set_node_properties"),
		TEXT("remove_node"), TEXT("add_comment"), TEXT("connect_pins"), TEXT("disconnect_pins"), TEXT("apply_batch"),
		TEXT("auto_layout"),
		TEXT("runtime_list_instances"), TEXT("runtime_get_instance") };

	TArray<FString> Registered;
	for (const FMonolithActionInfo& Info : FMonolithToolRegistry::Get().GetActions(TEXT("flow")))
	{
		Registered.Add(Info.Action);
		TestTrue(FString::Printf(TEXT("%s has a description"), *Info.Action), !Info.Description.IsEmpty());
		TestTrue(FString::Printf(TEXT("%s has a param schema"), *Info.Action), Info.ParamSchema.IsValid());
	}
	for (const FString& Action : Expected)
	{
		TestTrue(FString::Printf(TEXT("%s registered"), *Action), Registered.Contains(Action));
	}
	UTEST_EQUAL("nothing undocumented", Registered.Num(), Expected.Num());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithFlowEmptyParamsTest, "MonolithFlow.Surface.EmptyParamsFailCleanly", TestFlags)
bool FMonolithFlowEmptyParamsTest::RunTest(const FString& Parameters)
{
	// Through the registry, as an MCP call arrives. The temporary index keeps the actions that need no params away from the project's.
	FTempIndex Index;
	FMonolithFlowActions::FScopedDatabaseOverride Override(Index.Raw());
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	for (const FMonolithActionInfo& Info : Registry.GetActions(TEXT("flow")))
	{
		const FMonolithActionResult Result = Registry.ExecuteAction(TEXT("flow"), Info.Action, MakeShared<FJsonObject>());
		if (HasRequiredParam(Info.ParamSchema))
		{
			TestFalse(FString::Printf(TEXT("%s without its required params fails"), *Info.Action), Result.bSuccess);
			TestFalse(FString::Printf(TEXT("%s says why"), *Info.Action), Result.ErrorMessage.IsEmpty());
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithFlowCreateAssetPathsTest, "MonolithFlow.Surface.CreateAssetRejectsBadPaths", TestFlags)
bool FMonolithFlowCreateAssetPathsTest::RunTest(const FString& Parameters)
{
	const TCHAR* BadPaths[] = {
		TEXT("Game/MonolithFlowTests/NoLeadingSlash"),
		TEXT("/Game/MonolithFlowTests/Folder/"),
		TEXT("/NoSuchMountPoint/FLOW_X"),
		TEXT("/Game/MonolithFlowTests/FLOW_X.FLOW_Y"),
		TEXT("/Game/MonolithFlowTests/FLOW Bad!"),
	};
	for (const TCHAR* Path : BadPaths)
	{
		const FMonolithActionResult Result = FMonolithFlowAuthorActions::CreateAsset(Json(FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), Path)));
		TestFalse(FString::Printf(TEXT("'%s' rejected"), Path), Result.bSuccess);
		FString PackageName = Path;
		int32 Dot;
		if (PackageName.FindChar(TEXT('.'), Dot)) PackageName.LeftInline(Dot);
		if (PackageName.StartsWith(TEXT("/")) && !PackageName.EndsWith(TEXT("/")))
		{
			TestNull(FString::Printf(TEXT("'%s' left no package"), Path), FindPackage(nullptr, *PackageName));
		}
	}
	UTEST_FALSE("missing param", FMonolithFlowAuthorActions::CreateAsset(Json(TEXT("{}"))).bSuccess);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithFlowCreateAssetTest, "MonolithFlow.Surface.CreateAssetIsReadyForSubgraphs", TestFlags)
bool FMonolithFlowCreateAssetTest::RunTest(const FString& Parameters)
{
	// Created in memory only (no save) under a fresh name, so repeated runs never collide.
	const FString Name = FString::Printf(TEXT("FLOW_Created_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString Path = FString::Printf(TEXT("/Game/__MonolithFlowTests/%s.%s"), *Name, *Name);
	const FMonolithActionResult Created = FMonolithFlowAuthorActions::CreateAsset(Json(FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *Path)));
	UTEST_TRUE("created", Created.bSuccess);
	UFlowAsset* Asset = FindObject<UFlowAsset>(nullptr, *Path);
	UTEST_NOT_NULL("in memory", Asset);
	UTEST_EQUAL("starts with one node", Asset->GetNodes().Num(), 1);
	UTEST_NOT_NULL("and it is Start", StartNodeOf(Asset));
	UTEST_TRUE("dirty, not saved", Asset->GetPackage()->IsDirty());

	UTEST_FALSE("same path again is refused", FMonolithFlowAuthorActions::CreateAsset(Json(FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *Path))).bSuccess);

	// A SubGraph node in a fresh asset needs the editing policies set up (Flow 2.3 checks the preload policy).
	const FMonolithActionResult Added = FMonolithFlowAuthorActions::AddNode(Json(FString::Printf(
		TEXT("{\"asset_path\":\"%s\",\"node_class\":\"%s\",\"pos_x\":300}"), *Path, *UFlowNode_SubGraph::StaticClass()->GetPathName())));
	UTEST_TRUE("subgraph node added", Added.bSuccess);
	UTEST_EQUAL("two nodes", Asset->GetNodes().Num(), 2);

	const FMonolithActionResult Validated = FMonolithFlowAuthorActions::ValidateAsset(Json(FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *Path)));
	UTEST_TRUE("validate runs", Validated.bSuccess);
	UTEST_TRUE("result is reported", Validated.Result->HasField(TEXT("result")));

	Asset->GetPackage()->SetDirtyFlag(false);
	FAssetRegistryModule::AssetDeleted(Asset);
	Discard(Asset);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithFlowValidateTest, "MonolithFlow.Surface.ValidateAsset", TestFlags)
bool FMonolithFlowValidateTest::RunTest(const FString& Parameters)
{
	UFlowAsset* Asset = NewFlowAsset(TEXT("FLOW_Validate"));
	const FMonolithActionResult Healthy = FMonolithFlowAuthorActions::ValidateAsset(Json(FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *ObjectPathOf(Asset))));
	UTEST_TRUE("ok", Healthy.bSuccess);
	UTEST_NOT_EQUAL("a Start-only graph is not invalid", Healthy.Result->GetStringField(TEXT("result")), FString(TEXT("Invalid")));
	UTEST_EQUAL("no errors", Healthy.Result->GetArrayField(TEXT("errors")).Num(), 0);

	UTEST_FALSE("missing asset", FMonolithFlowAuthorActions::ValidateAsset(Json(TEXT("{\"asset_path\":\"/Temp/MonolithFlowTests/FLOW_Nope\"}"))).bSuccess);
	Discard(Asset);
	return true;
}

#endif
