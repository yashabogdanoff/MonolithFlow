#pragma once

#include "CoreMinimal.h"

#if WITH_FLOW

#include "MonolithToolRegistry.h"

/**
 * Authoring (write) action handlers for the `flow` namespace.
 *
 * These operate on the LIVE asset through the FlowEditor graph API
 * (FFlowGraphSchemaAction_NewNode, UEdGraphSchema, UFlowGraphNode), keeping
 * the editor graph and the runtime node set in sync exactly the way manual
 * editing does. Read actions (FMonolithFlowActions) answer from the SQLite
 * index instead - after mutating, save the asset (bSave param or
 * flow.save_asset) so the incremental indexer refreshes those tables.
 */
class FMonolithFlowAuthorActions
{
public:
	/** Register all authoring actions with the tool registry */
	static void RegisterActions(FMonolithToolRegistry& Registry);

	// --- Asset lifecycle ---

	/** Create a new, empty UFlowAsset (with its editor graph + default Start node). */
	static FMonolithActionResult CreateAsset(const TSharedPtr<FJsonObject>& Params);

	/** Save one Flow Asset (convenience mirror of blueprint.save_asset). */
	static FMonolithActionResult SaveAsset(const TSharedPtr<FJsonObject>& Params);

	/** Run registered data validators (incl. Flow node ValidateNode) on one Flow Asset. */
	static FMonolithActionResult ValidateAsset(const TSharedPtr<FJsonObject>& Params);

	// --- Node authoring ---

	/** Add a node of a given UClass at a position, optionally applying a property JSON. */
	static FMonolithActionResult AddNode(const TSharedPtr<FJsonObject>& Params);

	/** Apply a property JSON onto an existing node and rebuild its pins. */
	static FMonolithActionResult SetNodeProperties(const TSharedPtr<FJsonObject>& Params);

	/** Remove a node (breaks links, destroys the editor wrapper, unregisters the runtime node). */
	static FMonolithActionResult RemoveNode(const TSharedPtr<FJsonObject>& Params);

	/** Add a comment box to the graph. */
	static FMonolithActionResult AddComment(const TSharedPtr<FJsonObject>& Params);

	// --- Wiring ---

	/** Connect an output pin to an input pin (schema-mediated, then re-harvest runtime connections). */
	static FMonolithActionResult ConnectPins(const TSharedPtr<FJsonObject>& Params);

	/** Break all links of one pin (schema-mediated, then re-harvest runtime connections). */
	static FMonolithActionResult DisconnectPins(const TSharedPtr<FJsonObject>& Params);

	/** Several edits to one asset in one transaction: all apply or none; nodes created in the batch are referenced by alias. */
	static FMonolithActionResult ApplyBatch(const TSharedPtr<FJsonObject>& Params);

	// --- Layout ---

	/** Auto-layout the graph: layered left-to-right by execution flow (or Blueprint Assist when available). */
	static FMonolithActionResult AutoLayout(const TSharedPtr<FJsonObject>& Params);
};

#endif // WITH_FLOW
