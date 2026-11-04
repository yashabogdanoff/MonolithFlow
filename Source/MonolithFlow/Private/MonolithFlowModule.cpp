#include "MonolithFlowModule.h"
#include "MonolithFlowActions.h"
#include "MonolithFlowSettings.h"
#include "MonolithToolRegistry.h"
#include "Misc/CoreDelegates.h"
#include "Runtime/Launch/Resources/Version.h"

#if WITH_FLOW
#include "MonolithFlowAuthorActions.h"
#include "MonolithFlowRuntimeActions.h"
#include "MonolithFlowIndexer.h"
#include "MonolithIndexSubsystem.h"
#include "Editor.h"
#endif

#define LOCTEXT_NAMESPACE "FMonolithFlowModule"

DEFINE_LOG_CATEGORY(LogMonolithFlow);

namespace
{
#if WITH_FLOW
	/** How often flow_* is checked against Monolith's assets table. The probe is a single indexed query. */
	constexpr float ReconcileIntervalSeconds = 5.0f;
#endif

	/** 5.8 moved the delegate behind an accessor and deprecated the static member. */
	FSimpleMulticastDelegate& PostEngineInitDelegate()
	{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8
		return FCoreDelegates::GetOnPostEngineInit();
#else
		return FCoreDelegates::OnPostEngineInit;
#endif
	}
}

void FMonolithFlowModule::StartupModule()
{
	const UMonolithFlowSettings* Settings = GetDefault<UMonolithFlowSettings>();
	if (!Settings)
	{
		return;
	}

#if WITH_FLOW
	if (Settings->bEnableFlow)
	{
		FMonolithFlowActions::RegisterActions(FMonolithToolRegistry::Get());
		FMonolithFlowAuthorActions::RegisterActions(FMonolithToolRegistry::Get());
		FMonolithFlowRuntimeActions::RegisterActions(FMonolithToolRegistry::Get());
		const int32 ActionCount = FMonolithToolRegistry::Get().GetActions(TEXT("flow")).Num();
		UE_LOG(LogMonolithFlow, Log,
			TEXT("MonolithFlow: Loaded (%d actions)"), ActionCount);
	}
	else
	{
		UE_LOG(LogMonolithFlow, Log,
			TEXT("MonolithFlow: actions disabled in settings (bEnableFlow=false)"));
	}

	if (Settings->bIndexFlow)
	{
		PostEngineInitHandle = PostEngineInitDelegate().AddRaw(this, &FMonolithFlowModule::RegisterWithIndex);
	}
#else
	UE_LOG(LogMonolithFlow, Log,
		TEXT("MonolithFlow: Flow plugin not found at compile time, bridge inactive"));
#endif
}

void FMonolithFlowModule::ShutdownModule()
{
	if (PostEngineInitHandle.IsValid())
	{
		PostEngineInitDelegate().Remove(PostEngineInitHandle);
		PostEngineInitHandle.Reset();
	}

#if WITH_FLOW
	if (ReconcileTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(ReconcileTickerHandle);
		ReconcileTickerHandle.Reset();
	}

	if (IndexCompleteHandle.IsValid() && GEditor)
	{
		if (UMonolithIndexSubsystem* IndexSS = GEditor->GetEditorSubsystem<UMonolithIndexSubsystem>())
		{
			IndexSS->OnComplete.Remove(IndexCompleteHandle);
		}
	}
	IndexCompleteHandle.Reset();
	Indexer.Reset();
#endif

	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("flow"));
}

#if WITH_FLOW

void FMonolithFlowModule::RegisterWithIndex()
{
	UMonolithIndexSubsystem* IndexSS = GEditor ? GEditor->GetEditorSubsystem<UMonolithIndexSubsystem>() : nullptr;
	if (!IndexSS)
	{
		return;
	}

	Indexer = MakeShared<FMonolithFlowIndexer>();
	IndexSS->RegisterIndexer(Indexer);
	UE_LOG(LogMonolithFlow, Log, TEXT("MonolithFlow: Registered FMonolithFlowIndexer into MonolithIndex"));

	// Monolith clears its own rows on delete, rename and full pass; flow_* has no
	// foreign key to ride on, so it is brought in line from here (see ReconcileWithAssets).
	IndexCompleteHandle = IndexSS->OnComplete.AddRaw(this, &FMonolithFlowModule::OnIndexComplete);
	ReconcileTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateRaw(this, &FMonolithFlowModule::TickReconcile), ReconcileIntervalSeconds);
}

void FMonolithFlowModule::OnIndexComplete(bool bSuccess)
{
	UMonolithIndexSubsystem* IndexSS = GEditor ? GEditor->GetEditorSubsystem<UMonolithIndexSubsystem>() : nullptr;
	FMonolithIndexDatabase* DB = IndexSS ? IndexSS->GetDatabase() : nullptr;
	if (!Indexer || !DB)
	{
		return;
	}

	// A full pass may have reset the tables, after which ids belong to other
	// assets: drop what no longer matches, never treat it as a rename.
	Indexer->ReconcileWithAssets(*DB, /*bFollowRenames=*/false, /*bSweepOrphans=*/true);
	bOrphansSwept = true;
}

bool FMonolithFlowModule::TickReconcile(float DeltaTime)
{
	UMonolithIndexSubsystem* IndexSS = GEditor ? GEditor->GetEditorSubsystem<UMonolithIndexSubsystem>() : nullptr;
	FMonolithIndexDatabase* DB = IndexSS ? IndexSS->GetDatabase() : nullptr;
	if (!Indexer || !DB || !DB->IsOpen() || IndexSS->IsIndexing())
	{
		return true;
	}

	// An interrupted full pass leaves the reset tables half refilled, so a moved
	// row cannot be told from a reused id. OnIndexComplete settles it when the
	// resumed run finishes.
	if (DB->IsFullIndexInProgress())
	{
		return true;
	}

	Indexer->ReconcileWithAssets(*DB, /*bFollowRenames=*/true, /*bSweepOrphans=*/!bOrphansSwept);
	bOrphansSwept = true;
	return true;
}

#endif // WITH_FLOW

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FMonolithFlowModule, MonolithFlow)
