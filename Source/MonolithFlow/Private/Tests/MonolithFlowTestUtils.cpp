#include "MonolithFlowTestUtils.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_FLOW

#include "MonolithFlowNodeKey.h"
#include "MonolithToolRegistry.h"
#include "FlowAsset.h"
#include "Nodes/FlowNode.h"
#include "Nodes/Graph/FlowNode_Start.h"
#include "Graph/FlowGraph.h"
#include "Graph/FlowGraphSchema_Actions.h"
#include "Graph/Nodes/FlowGraphNode.h"
#include "EdGraph/EdGraphSchema.h"
#include "AssetRegistry/AssetData.h"
#include "SQLiteDatabase.h"
#include "SQLitePreparedStatement.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"

namespace MonolithFlowTests
{
	FTempIndex::FTempIndex()
	{
		Path = FPaths::Combine(FPaths::AutomationTransientDir(), TEXT("MonolithFlow"),
			FString::Printf(TEXT("Index_%s.db"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
		DB.Open(Path);
	}

	FTempIndex::~FTempIndex()
	{
		DB.Close();
		IFileManager::Get().Delete(*Path, /*RequireExists=*/false, /*EvenReadOnly=*/true, /*Quiet=*/true);
	}

	int64 FTempIndex::AddAssetRow(const UObject* Asset)
	{
		FIndexedAsset Row;
		Row.PackagePath = Asset->GetPackage()->GetName();
		Row.AssetName = Asset->GetName();
		Row.AssetClass = Asset->GetClass()->GetName();
		return DB.InsertAsset(Row);
	}

	int64 FTempIndex::Index(UObject* Asset)
	{
		int64 AssetId = DB.GetAssetId(Asset->GetPackage()->GetName());
		if (AssetId <= 0)
		{
			AssetId = AddAssetRow(Asset);
		}
		if (AssetId <= 0)
		{
			return -1;
		}
		return FlowIndexer.IndexAsset(FAssetData(Asset), Asset, DB, AssetId) ? AssetId : -1;
	}

	int64 FTempIndex::Count(const FString& Sql) const
	{
		int64 Value = -1;
		FSQLitePreparedStatement Stmt;
		if (Raw() && Stmt.Create(*Raw(), *Sql) && Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			Stmt.GetColumnValueByIndex(0, Value);
		}
		Stmt.Destroy();
		return Value;
	}

	FString FTempIndex::Text(const FString& Sql) const
	{
		FString Value;
		FSQLitePreparedStatement Stmt;
		if (Raw() && Stmt.Create(*Raw(), *Sql) && Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			Stmt.GetColumnValueByIndex(0, Value);
		}
		Stmt.Destroy();
		return Value;
	}

	UFlowAsset* NewFlowAsset(const FString& Name, UClass* AssetClass)
	{
		UPackage* Package = CreatePackage(*FString::Printf(TEXT("/Temp/MonolithFlowTests/%s"), *Name));
		Package->SetFlags(RF_Transient);
		UFlowAsset* FlowAsset = NewObject<UFlowAsset>(Package, AssetClass ? AssetClass : UFlowAsset::StaticClass(),
			FName(*Name), RF_Public | RF_Standalone | RF_Transactional);
		UFlowGraph::CreateGraph(FlowAsset);
		FlowAsset->SetupForEditing();
		return FlowAsset;
	}

	UFlowNode* AddNode(UFlowAsset* FlowAsset, UClass* NodeClass, float X, float Y)
	{
		UFlowGraphNode* GraphNode = FFlowGraphSchemaAction_NewNode::CreateNode(
			FlowAsset->GetGraph(), /*FromPin=*/nullptr, NodeClass, FVector2f(X, Y), /*bSelectNewNode=*/false);
		return GraphNode ? Cast<UFlowNode>(GraphNode->GetFlowNodeBase()) : nullptr;
	}

	bool Connect(UFlowNode* From, FName FromPin, UFlowNode* To, FName ToPin)
	{
		UFlowGraphNode* FromGraphNode = Cast<UFlowGraphNode>(From->GetGraphNode());
		UFlowGraphNode* ToGraphNode = Cast<UFlowGraphNode>(To->GetGraphNode());
		UEdGraphPin* Out = FromGraphNode ? FromGraphNode->FindPin(FromPin, EGPD_Output) : nullptr;
		UEdGraphPin* In = ToGraphNode ? ToGraphNode->FindPin(ToPin, EGPD_Input) : nullptr;
		if (!Out || !In)
		{
			return false;
		}
		const bool bConnected = FromGraphNode->GetSchema()->TryCreateConnection(Out, In);
		From->GetFlowAsset()->HarvestNodeConnections();
		return bConnected;
	}

	UFlowNode* StartNodeOf(UFlowAsset* FlowAsset)
	{
		for (const TPair<FGuid, UFlowNode*>& Pair : FlowAsset->GetNodes())
		{
			if (Pair.Value && Pair.Value->IsA<UFlowNode_Start>())
			{
				return Pair.Value;
			}
		}
		return nullptr;
	}

	void Discard(UFlowAsset* FlowAsset)
	{
		if (!FlowAsset)
		{
			return;
		}
		UPackage* Package = FlowAsset->GetPackage();
		FlowAsset->ClearFlags(RF_Public | RF_Standalone);
		FlowAsset->MarkAsGarbage();
		if (Package)
		{
			Package->MarkAsGarbage();
		}
	}

	FString ObjectPathOf(const UObject* Asset)
	{
		return Asset->GetPathName();
	}

	FString GuidOf(const UFlowNode* Node)
	{
		return MonolithFlow::NodeKeyString(Node);
	}

	TSharedPtr<FJsonObject> Json(const FString& Literal)
	{
		TSharedPtr<FJsonObject> Object;
		const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(Literal);
		FJsonSerializer::Deserialize(Reader, Object);
		return Object.IsValid() ? Object : MakeShared<FJsonObject>();
	}

	FString ErrorCodeOf(const FMonolithActionResult& Result)
	{
		const TSharedPtr<FJsonObject>* Data = nullptr;
		FString Code;
		if (Result.ErrorData.IsValid() && Result.ErrorData->TryGetObject(Data) && Data)
		{
			(*Data)->TryGetStringField(TEXT("code"), Code);
		}
		return Code;
	}
}

#endif
