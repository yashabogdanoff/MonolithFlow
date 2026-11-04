#pragma once

#include "Modules/ModuleManager.h"
#include "Logging/LogMacros.h"
#include "Containers/Ticker.h"

DECLARE_LOG_CATEGORY_EXTERN(LogMonolithFlow, Log, All);

class FMonolithFlowIndexer;

class FMonolithFlowModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
#if WITH_FLOW
	void RegisterWithIndex();
	void OnIndexComplete(bool bSuccess);
	bool TickReconcile(float DeltaTime);

	TSharedPtr<FMonolithFlowIndexer> Indexer;
	FDelegateHandle IndexCompleteHandle;
	FTSTicker::FDelegateHandle ReconcileTickerHandle;
	bool bOrphansSwept = false;
#endif

	FDelegateHandle PostEngineInitHandle;
};
