# MonolithFlow - Sibling Plugin Spec

**Host:** [Monolith](https://github.com/tumourlove/monolith) (hard plugin dependency; see its `Docs/SIBLING_PLUGIN_GUIDE.md`)
**Engine:** Unreal Engine 5.7, 5.8 | **Flow:** 2.3 | **Monolith:** 0.24.0+
**Version:** 0.17.0 (Beta)
**Status:** **Standalone sibling plugin** (`Plugins/MonolithFlow/`, beside `Plugins/Monolith/`). Registers into `FMonolithToolRegistry` and `UMonolithIndexSubsystem` through Monolith's exported public API - no Monolith sources are modified. Successor of the retired in-tree fork branch `feature/flow-graph-handler` (0.14.x), which predated Monolith's sibling-plugin support.

---

## MonolithFlow

**Dependencies:** Core, CoreUObject, Engine, DeveloperSettings, DataValidation, EditorScriptingUtilities, AssetTools, Flow, FlowEditor, MonolithCore, MonolithIndex, SQLiteCore, UnrealEd, BlueprintGraph, Kismet, EditorSubsystem, Json, JsonUtilities, GameplayTags, AssetRegistry
**Namespace:** `flow` | **Tool:** `flow_query(action, params)` | **Actions:** 29 - reads (index-backed, `FMonolithFlowActions`), authoring (live asset via FlowEditor graph API, `FMonolithFlowAuthorActions`), PIE runtime (`FMonolithFlowRuntimeActions`)
**Conditional:** MothCocoon Flow plugin features wrapped in `#if WITH_FLOW` (all three translation units, headers included). When Flow is absent, the module compiles to an empty stub (0 actions registered). Build.cs looks for `Flow.uplugin` under the project's whole `Plugins` tree (so `Plugins/FlowGraph` from a GitHub clone and nested layouts are found), under the engine's `Plugins/Marketplace` (Fab obfuscates folder names) and at `Engine/Plugins/Flow`; the walk stops at the first directory that holds any `.uplugin`, so it never descends into plugin content. Matching is on the descriptor file name - folder-name globs would false-positive on neighbours like `FlowValidation/`. The found path, or the stub notice, goes to the build log. `MONOLITH_RELEASE_BUILD=1` forces stub mode.
**Settings:** own `UMonolithFlowSettings` (UDeveloperSettings, Project Settings → Plugins → Monolith Flow, persisted to `DefaultMonolith.ini`): `bEnableFlow` (Modules) + `bIndexFlow` (Indexing), both default True. A sibling cannot inject UPROPERTYs into the host's `UMonolithSettings` - this replaces the fork-era toggles that lived there.

MonolithFlow indexes the [MothCocoon Flow plugin](https://github.com/MothCocoon/FlowGraph) into the central Monolith SQLite database and exposes read actions over it (plus authoring and runtime actions, below). Three of four Flow asset types are covered: `UFlowAsset` (graph), `UFlowNodeBlueprint` (BP-defined node class), `UFlowNodeAddOnBlueprint` (BP-defined addon class). `UFlowAssetParams` is intentionally excluded as still experimental upstream.

### Action Categories

| Category | Actions | Description |
|----------|---------|-------------|
| Asset listing | 2 | `list_assets` (glob filter, `offset` / `limit` paging with `total` + `has_more`), `get_asset_info` (counts, class breakdown, sample nodes) |
| Graph walk | 5 | `list_nodes`, `list_node_pins`, `list_connections`, `list_addons`, `get_node_info` |
| Graph analysis | 3 | `walk_execution` (BFS from Start/CustomInput: depth, unreachable, terminals), `export_graph` (one-call whole-asset dump incl. positions + comments), `diff_assets` (nodes/edges structural diff by guid) |
| Cross-asset reverse lookup | 5 | `list_custom_events`, `find_subgraph_callers`, `find_node_class_usages`, `find_pins_by_type`, `find_nodes_by_property` |
| Class registry | 1 | `list_node_classes` (BP + native, with kind / source / glob filters) |
| Authoring | 8 | `create_asset` (UFlowAssetFactory via IAssetTools), `add_node` (FFlowGraphSchemaAction_NewNode::CreateNode + property JSON + ReconstructNode), `set_node_properties`, `remove_node` (BreakNodeLinks + UFlowGraphNode::DestroyNode), `connect_pins` / `disconnect_pins` (UEdGraphSchema-mediated + HarvestNodeConnections), `add_comment`, `save_asset`. All wrapped in FScopedTransaction (editor undo). Optional `save` param on every mutation. |
| Layout | 1 | `auto_layout` - built-in layered formatter (bounded longest-path layering + 2 barycenter sweeps; cycle-safe) with `all` / `new_only` / `selected` modes, or Blueprint Assist through `IMonolithGraphFormatter` when available |
| Validation | 1 | `validate_asset` - `UEditorValidatorSubsystem::IsObjectValid` (covers node `ValidateNode()` and project validator plugins) |
| PIE runtime | 2 | `runtime_list_instances`, `runtime_get_instance` (`UFlowSubsystem::GetRootInstances`, ActiveNodes / RecordedNodes) |
| Smoke | 1 | `ping` |

### Authoring semantics

Reads answer from the index (stale until the asset is saved and the live indexer refreshes it); authoring actions mutate the LIVE asset through the same FlowEditor code paths as manual editing. After `connect_pins` / `disconnect_pins` / `set_node_properties` the runtime `Connections` map is re-synced via `UFlowAsset::HarvestNodeConnections()`. `add_node` rebuilds pins after applying the property JSON, so context-pin nodes (tag-driven switches etc.) come up with their generated pins. Property JSON shape = the `data` snapshot from `get_node_info` (same `FJsonObjectConverter` conventions both ways). Before any change the top-level keys are checked against the node class: unknown names, properties without `CPF_Edit` or with `CPF_EditConst`, and values whose JSON type does not fit the property (a struct given as a string must parse through `ImportText`) are rejected - `FJsonObjectConverter` itself turns such mismatches into default values and reports success. Each changed property gets `PreEditChange` / `PostEditChangeProperty(ValueSet)`. If a mutation still fails inside its transaction, the transaction is undone (`FScopedTransaction::Cancel` only drops the record), and only when it is the newest one opened since the call started. `connect_pins` returns `broken_links`, the links the Flow schema broke to make room.

**Node identity** is the key of `UFlowAsset::Nodes`, not the node's own `NodeGuid`. They match on healthy assets; duplicated or imported nodes can share one `NodeGuid` under distinct keys, and Flow resolves nodes and connections (`GetNode`, `FConnectedPin`) by the key. The index, authoring and runtime actions all use the key; the indexer logs a warning per asset with mismatches.

**Asset classes.** Monolith routes an asset to an indexer by the exact name of its asset class, so `GetSupportedClasses` lists `FlowAsset` plus every native subclass loaded when the indexer is registered (after engine init). Blueprint subclasses of `UFlowAsset` are not covered.

### Index schema additions (0.16.0)

`flow_nodes` gained `pos_x` / `pos_y` (editor-graph position, NULL when no editor wrapper); new table `flow_graph_comments` (fa_asset_id, comment_text, pos_x, pos_y, width, height, color) captures comment boxes. Both migrate via the ALTER-on-EnsureTablesExist pattern; a full reindex refreshes existing rows.

### Database Schema (9 tables)

| Table | Purpose | Cleanup key |
|-------|---------|-------------|
| `flow_assets` | One row per indexed UFlowAsset (counts, owner class, base params path, asset GUID) | `fa_path` |
| `flow_nodes` | One row per UFlowNode (class, display name, BP flag/path, pin counts, **`data` JSON snapshot of every UPROPERTY**) | `fa_asset_id` |
| `flow_node_pins` | One row per pin (input + output, current + orphan), with full type info from `FFlowPin` | `fa_asset_id` |
| `flow_node_addons` | Recursive addon tree (UFlowNodeAddOn under nodes; per-addon `data` JSON snapshot) | `fa_asset_id` |
| `flow_node_connections` | Flat outgoing-edge list (current + orphan, with `is_orphaned` flag) | `fa_asset_id` |
| `flow_subgraph_refs` | Cross-asset reverse index for `UFlowNode_SubGraph` references (host_fa_path → target_fa_path) | `host_fa_asset_id` |
| `flow_custom_events` | Custom Inputs / Custom Outputs of each Flow Asset (entry / exit names exposed to parent SubGraph nodes) | `fa_asset_id` |
| `flow_graph_comments` | Comment boxes of the editor graph (text, position, size, color) | `fa_asset_id` |
| `flow_node_classes` | Registry of every UFlowNode / UFlowNodeAddOn class referenced (BP + opportunistic native, `class_path` UNIQUE) | `class_path` (BP refresh) / `INSERT OR IGNORE` (native) |

### Key Actions

> **`get_node_info` deep dive.** Joins `flow_nodes` row + per-pin info + outgoing connections + recursive addon tree + parsed `data` JSON (full UPROPERTY snapshot serialized via `FJsonObjectConverter::UStructToJsonObjectString` with `CPF_Transient | CPF_DuplicateTransient` skipped). Single call returns everything needed to reason about one node's design-time state.
>
> **`find_nodes_by_property`.** SQL `LIKE` substring search over the per-node `data` JSON. Optional `property_name` scopes the match to `"property_name":...value` (any nesting). LIKE wildcards in user input are escaped via `ESCAPE '\'` so the substring is treated literally. Powers questions like "find every Dialogue node referencing DataTable X" or "every node where rowName contains Intro".
>
> **`find_subgraph_callers` + `find_node_class_usages`.** Cross-asset reverse lookups. The first asks "which Flow Assets host a `UFlowNode_SubGraph` pointing at target Y?", the second "which Flow Assets host a node of UClass X?". Both group results by host fa_path with per-node guids - useful for refactoring scope ("what scenarios use my custom Dialogue node?").
>
> **`list_node_classes`.** Registry combining BP-defined node classes (full row from UFlowNodeBlueprint asset indexing) and native classes encountered opportunistically during graph walks. Yields `display_name`, `category`, `description` extracted from CDO + UCLASS metadata. Optional filters: `kind` (node / addon / all), `source` (blueprint / native / all), `class_path_filter` (glob).

### Notes

> **Reflection-only access to private members.** `UFlowNode_SubGraph::Asset` and `::AssetParams` are private `TSoftObjectPtr` UPROPERTYs, and `UFlowNode::Connections` is a protected `TMap<FName, FConnectedPin>` UPROPERTY. All three are read via `FProperty::GetPropertyValue_InContainer` / `FMapProperty + FScriptMapHelper`, never linking against Flow internals beyond the public headers. Pattern matches MonolithLogicDriver - version-agnostic as long as property names stay stable.
>
> **Orphan output pins.** UFlowNode subclasses with dynamic pin generation (e.g. a switch that generates one output pin per child of a GameplayTag root) keep stale per-tag output pins after the underlying tag is removed (`bOrphanedPin=True` in the editor wrapper, `PinCategory="exec"` lowercase). The runtime `Connections` map still has entries for those orphan pin names. Flat `flow_node_pins` / `flow_node_connections` capture both current and orphan rows; the new `is_orphaned` column distinguishes them. The full topology also lives inside `flow_nodes.data.connections` as a backup. Orphan input pins are not addressable - they're an editor-wrapper artifact (CustomProperties Pin entries on FlowGraphNode) with no corresponding state on the runtime FlowNode.
>
> **No FK on `assets(id)`.** Mirrors the rest of Monolith - would block `FMonolithIndexDatabase::ResetDatabase()` on full reindex. Because `ResetDatabase` restarts the `assets` autoincrement while `flow_*` keeps its rows, the id an asset gets in a full pass may be the old id of another graph. `IndexAsset` therefore first claims its new id - drops every `flow_assets` row holding it and all children under it - and only then drops its own previous row (found by `fa_path`) with children under the old id. The reverse order, used before 0.17.0, erased the fresh children of whichever graph had taken the old id earlier in the same pass.
>
> **Reconciling with `assets`.** The missing FK also means Monolith's own cleanup never reaches `flow_*`: `DeleteAssetByPath` cascades only over FK children, `UpdateAssetPath` moves the `assets` row and keeps its id, and `ResetDatabase` drops only Monolith's tables. Without help, deleted and renamed graphs stay in `flow_*` and every reverse lookup reports them. `FMonolithFlowIndexer::ReconcileWithAssets` closes the gap. A `flow_assets` row (or a Blueprint `flow_node_classes` row) is live while an `assets` row with the same id has the same object path (`package_path || '.' || asset_name`). A row whose id still exists under a new path of the same asset class, with no other row claiming the id or the new path, is a rename: `fa_path`, `name` and both sides of `flow_subgraph_refs` move along (for node Blueprints also `class_path`, and the `node_class` / `addon_class` of nodes using it). Everything else that is not live is dropped, then child rows without a `flow_assets` parent are swept. It runs from two places: `OnComplete` of a full or resumed pass (drop only - after `ResetDatabase` ids belong to other assets, so a rename cannot be told from reuse), and a 5 s editor ticker that first runs one indexed probe and does nothing when the tables agree (renames followed; skipped while an index run is active or a full index is marked in progress). The first run of a session also sweeps orphan child rows, which clears rows left by versions before 0.17.0.
>
> **Data JSON snapshot semantics.** Every node and addon stores a compact JSON of its design-time UPROPERTYs. SkipFlags = `CPF_Transient | CPF_DuplicateTransient`. `CPF_DisableEditOnInstance` is intentionally NOT skipped - some Flow-internal UPROPERTYs that drive runtime behavior have it set. Consumers should treat this column as the canonical full-fidelity record; flat tables (pins, connections, custom_events, subgraph_refs) are normalized views over the most useful subset.
>
> **`UFlowAssetParams` deferred.** The fourth Flow asset type is still experimental upstream and changes shape between minor versions. `flow_assets.base_params_path` carries `BaseAssetParams.AssetPtr.ToSoftObjectPath()` so callers can join, but the params asset itself isn't introspected.
>
> **Schema migration.** Schema additions (e.g. `is_orphaned` on `flow_node_pins` / `flow_node_connections` in 0.14.7) ship via `ALTER TABLE ADD COLUMN` inside `EnsureTablesExist`. The ALTER silently no-ops on fresh DBs where the column already exists, so existing project DBs upgrade without a forced full reindex.

---
