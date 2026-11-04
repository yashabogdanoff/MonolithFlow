#pragma once

#include "CoreMinimal.h"

#if WITH_FLOW

#include "FlowAsset.h"
#include "Nodes/FlowNode.h"

namespace MonolithFlow
{
	/**
	 * Key of a node in its asset's UFlowAsset::Nodes map - the identity Flow resolves
	 * nodes and connections by, and the one the index stores. A node's own NodeGuid
	 * normally equals it, but duplicated or imported nodes can share a NodeGuid under
	 * distinct keys. Falls back to NodeGuid for a node that is not in a map.
	 */
	inline FGuid NodeKeyOf(const UFlowNode* Node)
	{
		if (const UFlowAsset* FlowAsset = Node ? Node->GetFlowAsset() : nullptr)
		{
			for (const TPair<FGuid, UFlowNode*>& Pair : FlowAsset->GetNodes())
			{
				if (Pair.Value == Node)
				{
					return Pair.Key;
				}
			}
		}
		return Node ? Node->GetGuid() : FGuid();
	}

	inline FString NodeKeyString(const UFlowNode* Node)
	{
		return NodeKeyOf(Node).ToString(EGuidFormats::DigitsWithHyphens);
	}
}

#endif // WITH_FLOW
