#pragma once

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_FLOW

#include "MonolithIndexDatabase.h"
#include "MonolithFlowIndexer.h"
#include "Dom/JsonObject.h"
#include "Misc/AutomationTest.h"

class UFlowAsset;
class UFlowNode;
class FSQLiteDatabase;
struct FMonolithActionResult;

/**
 * Fixtures shared by the MonolithFlow automation tests.
 *
 * Nothing here touches the project: Flow assets live in memory under
 * /Temp/MonolithFlowTests and are never saved, and every test indexes into its
 * own SQLite file in the automation transient directory, created with
 * Monolith's schema and deleted afterwards.
 */
namespace MonolithFlowTests
{
	/** A throwaway Monolith index database plus a Flow indexer writing into it. */
	class FTempIndex
	{
	public:
		FTempIndex();
		~FTempIndex();

		bool IsOpen() const { return DB.IsOpen(); }
		FMonolithIndexDatabase& Database() { return DB; }
		FSQLiteDatabase* Raw() const { return DB.GetRawDatabase(); }
		FMonolithFlowIndexer& Indexer() { return FlowIndexer; }

		/** Insert the core `assets` row for an object, as Monolith's own pass does. Returns its id. */
		int64 AddAssetRow(const UObject* Asset);

		/** AddAssetRow (when the asset has no row yet) + IndexAsset. Returns the asset id, or -1. */
		int64 Index(UObject* Asset);

		/** Scalar query helpers. */
		int64 Count(const FString& Sql) const;
		FString Text(const FString& Sql) const;

	private:
		FMonolithIndexDatabase DB;
		FMonolithFlowIndexer FlowIndexer;
		FString Path;
	};

	/** A new Flow asset with its editor graph and Start node, in memory only. */
	UFlowAsset* NewFlowAsset(const FString& Name, UClass* AssetClass = nullptr);

	/** Add a node of a class through the Flow graph schema, as the editor does. */
	UFlowNode* AddNode(UFlowAsset* FlowAsset, UClass* NodeClass, float X = 0.f, float Y = 0.f);

	/** Wire an output pin to an input pin through the graph schema. */
	bool Connect(UFlowNode* From, FName FromPin, UFlowNode* To, FName ToPin = TEXT("In"));

	/** The Start node of a graph made by NewFlowAsset. */
	UFlowNode* StartNodeOf(UFlowAsset* FlowAsset);

	/** Mark a test asset for collection so the next test starts clean. */
	void Discard(UFlowAsset* FlowAsset);

	/** "/Temp/MonolithFlowTests/<Name>.<Name>" */
	FString ObjectPathOf(const UObject* Asset);

	/** Key of a node in its asset's Nodes map, hyphenated. */
	FString GuidOf(const UFlowNode* Node);

	/** Parse a JSON object literal (test params). */
	TSharedPtr<FJsonObject> Json(const FString& Literal);

	/** The `code` from an action error's structured data, or empty. */
	FString ErrorCodeOf(const FMonolithActionResult& Result);

	/**
	 * Base for tests that undo the creation of a node. Flow 2.3 logs an empty
	 * error from UFlowGraph::UpdateFlowGraphNodeErrorMessage while the node is
	 * torn down, which would fail the test on its own; the test's checks still
	 * decide the result.
	 */
	class FUndoingTestBase : public FAutomationTestBase
	{
	public:
		FUndoingTestBase(const FString& InName, const bool bInComplexTask) : FAutomationTestBase(InName, bInComplexTask) {}
		virtual bool SuppressLogErrors() override { return true; }
	};
}

#endif
