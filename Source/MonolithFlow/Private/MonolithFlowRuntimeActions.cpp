#include "MonolithFlowRuntimeActions.h"

#if WITH_FLOW

#include "MonolithParamSchema.h"
#include "MonolithFlowNodeKey.h"

#include "FlowAsset.h"
#include "FlowComponent.h"
#include "FlowSubsystem.h"
#include "Nodes/FlowNode.h"

#include "Editor.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Dom/JsonObject.h"

// ============================================================================
// Local helpers
// ============================================================================

namespace
{
#if WITH_DEV_AUTOMATION_TESTS
	/** Set by FScopedSubsystemOverride. */
	UFlowSubsystem* GSubsystemOverride = nullptr;
#endif

	/** Resolve the PIE-world Flow subsystem (handles project-level UFlowSubsystem overrides). */
	UFlowSubsystem* GetPieFlowSubsystem(FString& OutError)
	{
#if WITH_DEV_AUTOMATION_TESTS
		if (GSubsystemOverride)
		{
			return GSubsystemOverride;
		}
#endif
		if (!GEditor || !GEditor->PlayWorld)
		{
			OutError = TEXT("No PIE session running - start PIE first (editor.start_pie)");
			return nullptr;
		}
		UGameInstance* GameInstance = GEditor->PlayWorld->GetGameInstance();
		if (!GameInstance)
		{
			OutError = TEXT("PIE world has no game instance");
			return nullptr;
		}
		UFlowSubsystem* FlowSubsystem = GameInstance->GetSubsystem<UFlowSubsystem>();
		if (!FlowSubsystem)
		{
			OutError = TEXT("UFlowSubsystem not found on the PIE game instance");
			return nullptr;
		}
		return FlowSubsystem;
	}

	/** Human identity of an instance owner: component + owning actor when applicable. */
	void DescribeOwner(const UObject* Owner, const TSharedPtr<FJsonObject>& Row)
	{
		if (!Owner)
		{
			Row->SetStringField(TEXT("owner"), TEXT("<none>"));
			return;
		}
		Row->SetStringField(TEXT("owner"), Owner->GetName());
		Row->SetStringField(TEXT("owner_class"), Owner->GetClass()->GetPathName());
		if (const UFlowComponent* Component = Cast<UFlowComponent>(Owner))
		{
			if (const AActor* OwnerActor = Component->GetOwner())
			{
				Row->SetStringField(TEXT("owner_actor"), OwnerActor->GetName());
			}
		}
	}

	TSharedPtr<FJsonValue> MakeNodeRow(const UFlowNode* Node)
	{
		auto Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("node_guid"), MonolithFlow::NodeKeyString(Node));
		Row->SetStringField(TEXT("node_class"), Node->GetClass()->GetPathName());
#if WITH_EDITOR
		Row->SetStringField(TEXT("display_name"), Node->GetNodeTitle().ToString());
#endif
		Row->SetStringField(TEXT("activation_state"),
			StaticEnum<EFlowNodeState>()->GetNameStringByValue(static_cast<int64>(Node->GetActivationState())));
		return MakeShared<FJsonValueObject>(Row);
	}

	/** True when the owner filter names the instance owner or the actor owning it. */
	bool OwnerMatches(const UObject* Owner, const FString& Filter)
	{
		if (Filter.IsEmpty())
		{
			return true;
		}
		if (!Owner)
		{
			return false;
		}
		if (Owner->GetName() == Filter || Owner->GetPathName() == Filter)
		{
			return true;
		}
		const UFlowComponent* Component = Cast<UFlowComponent>(Owner);
		const AActor* OwnerActor = Component ? Component->GetOwner() : nullptr;
		return OwnerActor && (OwnerActor->GetName() == Filter || OwnerActor->GetActorNameOrLabel() == Filter);
	}

	/** Template asset path of an instanced Flow asset (falls back to instance path). */
	FString TemplatePathOf(const UFlowAsset* Instance)
	{
		if (const UFlowAsset* Template = Instance->GetTemplateAsset())
		{
			return Template->GetPathName();
		}
		return Instance->GetPathName();
	}
}

#if WITH_DEV_AUTOMATION_TESTS
FMonolithFlowRuntimeActions::FScopedSubsystemOverride::FScopedSubsystemOverride(UFlowSubsystem* Subsystem)
	: Previous(GSubsystemOverride)
{
	GSubsystemOverride = Subsystem;
}

FMonolithFlowRuntimeActions::FScopedSubsystemOverride::~FScopedSubsystemOverride()
{
	GSubsystemOverride = Previous;
}
#endif

// ============================================================================
// Registration
// ============================================================================

void FMonolithFlowRuntimeActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("flow"), TEXT("runtime_list_instances"),
		TEXT("List every ROOT Flow instance in the running PIE session: template asset path, instance name, owner (component + actor), activity flag and active/recorded node counts. Requires PIE."),
		FMonolithActionHandler::CreateStatic(&FMonolithFlowRuntimeActions::RuntimeListInstances),
		FParamSchemaBuilder().Build());

	Registry.RegisterAction(TEXT("flow"), TEXT("runtime_get_instance"),
		TEXT("Deep-dive one running Flow instance matched by its TEMPLATE asset path: currently active nodes and the recorded activation history (most recent last, tail limited by history_limit), each node with its activation_state. When several root instances run from one template, pass owner to pick one; without it the call fails and lists the owners. Requires PIE."),
		FMonolithActionHandler::CreateStatic(&FMonolithFlowRuntimeActions::RuntimeGetInstance),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("TEMPLATE UFlowAsset object path (as authored, not the PIE instance name) or package path"))
			.Optional(TEXT("owner"), TEXT("string"), TEXT("Name of the instance owner or of the actor owning its Flow component, as runtime_list_instances reports them"))
			.Optional(TEXT("history_limit"), TEXT("integer"), TEXT("Max recorded nodes returned, most recent last (default 30, at least 1)"))
			.Build());
}

// ============================================================================
// Handlers
// ============================================================================

FMonolithActionResult FMonolithFlowRuntimeActions::RuntimeListInstances(const TSharedPtr<FJsonObject>& Params)
{
	FString Err;
	UFlowSubsystem* FlowSubsystem = GetPieFlowSubsystem(Err);
	if (!FlowSubsystem) return FMonolithActionResult::Error(Err);

	TArray<TSharedPtr<FJsonValue>> Rows;
	for (const TPair<UObject*, UFlowAsset*>& Pair : FlowSubsystem->GetRootInstances())
	{
		const UFlowAsset* Instance = Pair.Value;
		if (!Instance) continue;

		auto Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("template_asset_path"), TemplatePathOf(Instance));
		Row->SetStringField(TEXT("instance_name"), Instance->GetName());
		DescribeOwner(Pair.Key, Row);
		Row->SetBoolField(TEXT("is_active"), Instance->IsActive());
		Row->SetNumberField(TEXT("active_node_count"), Instance->GetActiveNodes().Num());
		Row->SetNumberField(TEXT("recorded_node_count"), Instance->GetRecordedNodes().Num());
		Rows.Add(MakeShared<FJsonValueObject>(Row));
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("instances"), Rows);
	Result->SetNumberField(TEXT("count"), Rows.Num());
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithFlowRuntimeActions::RuntimeGetInstance(const TSharedPtr<FJsonObject>& Params)
{
	FString Err;
	UFlowSubsystem* FlowSubsystem = GetPieFlowSubsystem(Err);
	if (!FlowSubsystem) return FMonolithActionResult::Error(Err);

	FString AssetPath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path is required"));
	}
	double HistoryLimit = 30;
	Params->TryGetNumberField(TEXT("history_limit"), HistoryLimit);
	FString OwnerFilter;
	Params->TryGetStringField(TEXT("owner"), OwnerFilter);

	// Match by template path; also accept package-path-only form ("/Game/X/FLOW_Y").
	const UFlowAsset* Match = nullptr;
	const UObject* MatchOwner = nullptr;
	TArray<FString> MatchOwners;
	for (const TPair<UObject*, UFlowAsset*>& Pair : FlowSubsystem->GetRootInstances())
	{
		const UFlowAsset* Instance = Pair.Value;
		if (!Instance) continue;
		const FString TemplatePath = TemplatePathOf(Instance);
		if ((TemplatePath == AssetPath || TemplatePath.StartsWith(AssetPath + TEXT("."))) && OwnerMatches(Pair.Key, OwnerFilter))
		{
			Match = Instance;
			MatchOwner = Pair.Key;
			MatchOwners.Add(Pair.Key ? Pair.Key->GetName() : FString(TEXT("<none>")));
		}
	}
	if (!Match)
	{
		return FMonolithActionResult::Error(OwnerFilter.IsEmpty()
			? FString::Printf(TEXT("No running root Flow instance for template '%s' (see runtime_list_instances)"), *AssetPath)
			: FString::Printf(TEXT("No running root Flow instance for template '%s' owned by '%s' (see runtime_list_instances)"), *AssetPath, *OwnerFilter));
	}
	if (MatchOwners.Num() > 1)
	{
		MatchOwners.Sort();
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("%d root Flow instances run from template '%s'; pass owner to pick one of: %s"),
			MatchOwners.Num(), *AssetPath, *FString::Join(MatchOwners, TEXT(", "))));
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("template_asset_path"), TemplatePathOf(Match));
	Result->SetStringField(TEXT("instance_name"), Match->GetName());
	DescribeOwner(MatchOwner, Result);
	Result->SetBoolField(TEXT("is_active"), Match->IsActive());

	{
		TArray<TSharedPtr<FJsonValue>> Rows;
		for (const UFlowNode* Node : Match->GetActiveNodes())
		{
			if (Node) Rows.Add(MakeNodeRow(Node));
		}
		Result->SetArrayField(TEXT("active_nodes"), Rows);
	}
	{
		const TArray<UFlowNode*>& Recorded = Match->GetRecordedNodes();
		const int32 Limit = FMath::Max(1, static_cast<int32>(HistoryLimit));
		const int32 Start = FMath::Max(0, Recorded.Num() - Limit);
		TArray<TSharedPtr<FJsonValue>> Rows;
		for (int32 i = Start; i < Recorded.Num(); ++i)
		{
			if (Recorded[i]) Rows.Add(MakeNodeRow(Recorded[i]));
		}
		Result->SetArrayField(TEXT("recorded_nodes"), Rows);
		Result->SetNumberField(TEXT("recorded_total"), Recorded.Num());
	}

	return FMonolithActionResult::Success(Result);
}

#endif // WITH_FLOW
