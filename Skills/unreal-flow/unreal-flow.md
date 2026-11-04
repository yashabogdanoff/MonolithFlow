---
name: unreal-flow
description: Use when reading, analyzing or editing MothCocoon Flow plugin assets via Monolith MCP - listing Flow assets, walking and exporting node graphs, finding cross-asset references and node-class usages, searching design-time node properties, adding/wiring/removing nodes, laying out and validating graphs, inspecting live graphs in PIE. Triggers on Flow, FlowAsset, FlowNode, FlowGraph, SubGraph, custom input/output event, FlowComponent, flow_query.
---

# Unreal Flow Workflows

**29 Flow actions** via `flow_query()`, provided by the MonolithFlow sibling plugin. Reads answer from Monolith's SQLite project index; authoring edits the live asset through the FlowEditor graph API. Discover with `monolith_discover({ namespace: "flow" })`.

**Conditional on the Flow plugin.** Without Flow the module compiles to an empty stub (0 actions). If `flow` is missing from `monolith_discover`, check the build log for `MonolithFlow: Flow plugin found at ...` or the stub notice.

## Key Parameters

- `asset_path` - the Flow Asset, as object path (`/Game/Flow/MyGraph.MyGraph`) or package path (`/Game/Flow/MyGraph`). Answers echo the object path.
- `node_guid` - node FGuid in `DigitsWithHyphens` form (e.g. `4EBF9E55-4CE1-C76C-BB8C-7BA7C8DD9BAF`). Every action that lists nodes returns it.
- `node_class` - full UClass path: `/Script/Flow.FlowNode_Log` (Flow's own), `/Script/MyGame.MyFlowNode_Custom` (project native), `/Game/Foo/BPNode.BPNode_C` (Blueprint node).
- `properties` - JSON object applied onto node UPROPERTYs. Same shape as the `data` snapshot from `get_node_info`; unknown property names are rejected.
- `pin_type_name` / `pin_subcategory_object` - `FFlowPin` type (usually `Exec`) and the Object/Class/Struct path of typed data pins.
- `asset_path_filter` / `class_path_filter` - glob (`*`, `?`) over host fa_path or class_path.
- `save` - on every mutation: `true` writes the package and refreshes the index; otherwise call `save_asset` at the end.

## Action Reference

| Action | Key Params | Purpose |
|--------|-----------|---------|
| **Smoke (1)** | | |
| `ping` | - | `{status:"ok", module:"MonolithFlow"}` |
| **Asset listing (2)** | | |
| `list_assets` | `asset_path_filter`?, `offset`?, `limit`? (200, max 1000) | Flow Assets with node/addon/subgraph counts; `total` + `has_more` |
| `get_asset_info` | `asset_path` | Counts, node-class breakdown, 10 sample nodes, owner class, asset GUID, base params path |
| **Graph walk (5)** | | |
| `list_nodes` | `asset_path`, `node_class`? | Every node with its `data` JSON snapshot |
| `list_node_pins` | `asset_path`, `node_guid`?, `pin_direction`?, `pin_type_name`? | Pins (input + output, current + orphan) with type info and `is_orphaned` |
| `list_connections` | `asset_path`, `from_node_guid`?, `to_node_guid`? | Outgoing edges (current + orphan) |
| `list_addons` | `asset_path`, `node_guid`? | Recursive AddOn tree with per-addon `data` |
| `get_node_info` | `asset_path`, `node_guid` | One node: row + `data` + pins + edges + addons |
| **Graph analysis (3)** | | |
| `walk_execution` | `asset_path`, `start_node_guid`? | BFS from Start (or a given node): depth per node, unreachable nodes, terminals |
| `export_graph` | `asset_path`, `include_data`? | Whole graph in one call: nodes, positions, edges, comments, custom events |
| `diff_assets` | `asset_path_a`, `asset_path_b` | Structural diff by node guid: added, removed, changed class/data, edge changes |
| **Cross-asset reverse lookup (5)** | | |
| `list_custom_events` | `asset_path`, `kind`? (`input`/`output`/`all`) | Custom Inputs / Outputs of one asset (what a parent SubGraph node sees) |
| `find_subgraph_callers` | `target_asset_path`, `asset_path_filter`? | Every asset hosting a SubGraph node that points at the target |
| `find_node_class_usages` | `node_class`, `asset_path_filter`? | Every asset using a node class, with per-node guids |
| `find_pins_by_type` | `pin_type_name`, `pin_subcategory_object`?, `pin_direction`?, `asset_path_filter`? | Pins of a given type across the project |
| `find_nodes_by_property` | `value_substring`, `property_name`?, `node_class`?, `asset_path_filter`? | Literal substring search over node `data` JSON; `property_name` scopes it to `"property_name":...value` |
| **Class registry (1)** | | |
| `list_node_classes` | `kind`?, `source`?, `class_path_filter`? | Node / AddOn classes (Blueprint rows + native classes seen in graphs) with display name, category, description, parent, abstract flag |
| **Authoring (8)** - live asset, one transaction each (editor undo works) | | |
| `create_asset` | `asset_path`, `save`? | New empty Flow Asset with its editor graph and Start node |
| `add_node` | `asset_path`, `node_class`, `pos_x`?, `pos_y`?, `properties`?, `save`? | Add a node; properties applied, then pins rebuilt (context-pin nodes come up right). Returns `node_guid` |
| `set_node_properties` | `asset_path`, `node_guid`, `properties`, `save`? | Change UPROPERTYs of a node; reports `applied_properties` |
| `remove_node` | `asset_path`, `node_guid`, `save`? | Break links and delete a node |
| `connect_pins` | `asset_path`, `from_node_guid`, `from_pin`, `to_node_guid`, `to_pin`?, `save`? | Wire an output pin to an input (schema-validated) |
| `disconnect_pins` | `asset_path`, `node_guid`, `pin_name`, `save`? | Break every link on one pin |
| `add_comment` | `asset_path`, `text`, `pos_x`?, `pos_y`?, `width`?, `height`?, `save`? | Comment box on the graph |
| `save_asset` | `asset_path` | Save the package (also refreshes the index) |
| **Layout (1)** | | |
| `auto_layout` | `asset_path`, `layout_mode`? (`all`/`new_only`/`selected`), `node_ids`?, `horizontal_spacing`?, `vertical_spacing`?, `formatter`?, `save`? | Layered left-to-right by execution flow, or Blueprint Assist when installed |
| **Validation (1)** | | |
| `validate_asset` | `asset_path` | Every registered data validator, including per-node `ValidateNode()` |
| **PIE runtime (2)** | | |
| `runtime_list_instances` | - | Root Flow instances running in PIE |
| `runtime_get_instance` | `asset_path`, `history_limit`? | Active nodes and activation history of a live graph |

## Typical Workflow: "What's in this graph?"

```
1. flow_query("export_graph", { "asset_path": "/Game/Flow/MyGraph" })
   -> nodes, positions, edges, comments, custom events in one answer
2. flow_query("walk_execution", { "asset_path": "/Game/Flow/MyGraph" })
   -> what is reachable from Start, what is dead, where execution ends
```

## Typical Workflow: "Who uses this graph / this node class?"

```
flow_query("find_subgraph_callers", { "target_asset_path": "/Game/Flow/Shared/Intro" })
flow_query("find_node_class_usages", { "node_class": "/Script/MyGame.MyFlowNode_Custom" })
```

## Typical Workflow: "Find every Dialogue node referencing DataTable X"

```
flow_query("find_nodes_by_property", {
  "value_substring": "DT_MyLines",
  "property_name": "dataTable",
  "node_class": "/Script/MyGame.MyFlowNode_Dialogue"
})
```

## Typical Workflow: "Add a node after this one"

```
1. flow_query("add_node", { "asset_path": "/Game/Flow/MyGraph", "node_class": "/Script/Flow.FlowNode_Log",
     "properties": { "message": "Reached the gate" } })            -> node_guid
2. flow_query("connect_pins", { "asset_path": "/Game/Flow/MyGraph",
     "from_node_guid": "<existing>", "from_pin": "Out", "to_node_guid": "<new>" })
3. flow_query("auto_layout", { "asset_path": "/Game/Flow/MyGraph", "layout_mode": "new_only" })
4. flow_query("validate_asset", { "asset_path": "/Game/Flow/MyGraph" })
5. flow_query("save_asset", { "asset_path": "/Game/Flow/MyGraph" })
```

Property names in `properties` follow the `data` snapshot: read one node of the class with `get_node_info` first.

## Caveats

- **Reads lag until save.** Read actions answer from the index, which refreshes when the asset is saved. After authoring without `save`, the live asset and the index differ until `save_asset`.
- **Deletes and renames reach the index within seconds.** The plugin reconciles `flow_*` with Monolith's `assets` table on a short timer and after every full pass, so a deleted graph drops out and a renamed one moves to its new path without a re-save.
- **Orphan output pins** (a node that generates one output pin per tag keeps the pin after the tag is removed) are reported with `is_orphaned: true`. They are real stale edges, useful for cleanup audits.
- **`UFlowAssetParams` is not indexed** - only `flow_assets.base_params_path` is captured.
- **`data` JSON is the full record** of design-time UPROPERTYs; flat tables are views over a subset. It also survives a UPROPERTY being removed from C++ until the asset is re-indexed - read it to recover values before saving the affected assets.
- **Most pins are `Exec`.** `find_pins_by_type` for data-pin types returns nothing in projects that do not use Flow data pins.
