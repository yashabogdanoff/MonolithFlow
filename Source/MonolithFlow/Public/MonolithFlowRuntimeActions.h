#pragma once

#include "CoreMinimal.h"

#if WITH_FLOW

#include "MonolithToolRegistry.h"

/**
 * PIE runtime introspection for the `flow` namespace: which Flow instances
 * are live, which nodes are active right now, and the recorded activation
 * history - the AI-facing equivalent of watching a graph highlight during
 * play. Read-only; requires a running PIE session.
 */
class FMonolithFlowRuntimeActions
{
public:
	/** Register all runtime actions with the tool registry */
	static void RegisterActions(FMonolithToolRegistry& Registry);

	/** Every root Flow instance in the PIE world with owner + activity summary. */
	static FMonolithActionResult RuntimeListInstances(const TSharedPtr<FJsonObject>& Params);

	/** One instance's active nodes + recorded activation history. */
	static FMonolithActionResult RuntimeGetInstance(const TSharedPtr<FJsonObject>& Params);

#if WITH_DEV_AUTOMATION_TESTS
	/** Serves the runtime actions from another Flow subsystem while alive, so automation tests need no PIE session. */
	struct FScopedSubsystemOverride
	{
		explicit FScopedSubsystemOverride(class UFlowSubsystem* Subsystem);
		~FScopedSubsystemOverride();

	private:
		class UFlowSubsystem* Previous = nullptr;
	};
#endif
};

#endif // WITH_FLOW
