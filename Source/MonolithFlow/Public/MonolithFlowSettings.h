#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "MonolithFlowSettings.generated.h"

/**
 * Settings for the MonolithFlow sibling plugin.
 *
 * Lives in its own UDeveloperSettings class (Project Settings -> Plugins ->
 * Monolith Flow, right below the host's Monolith page) because a sibling
 * plugin cannot inject UPROPERTYs into Monolith's own UMonolithSettings,
 * and Project Settings sections map one settings object to one section.
 * Persisted to the same DefaultMonolith.ini as the host plugin, under this
 * class's own section.
 */
UCLASS(config=Monolith, defaultconfig, meta=(DisplayName="Monolith Flow"))
class MONOLITHFLOW_API UMonolithFlowSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	//~ UDeveloperSettings - sit next to Monolith's own page
	virtual FName GetCategoryName() const override { return FName(TEXT("Plugins")); }

	/** Register the `flow` MCP namespace (flow_query tool). Takes effect on next editor restart. */
	UPROPERTY(config, EditAnywhere, Category="Modules", DisplayName="Enable Flow")
	bool bEnableFlow = true;

	/** Index Flow assets (UFlowAsset, UFlowNodeBlueprint, UFlowNodeAddOnBlueprint) into Monolith's project index. */
	UPROPERTY(config, EditAnywhere, Category="Indexing", DisplayName="Index Flow Assets")
	bool bIndexFlow = true;
};
