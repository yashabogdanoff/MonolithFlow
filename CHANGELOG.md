# Changelog

## [0.17.0] - Unreleased

First public release as a standalone repository.

### Fixed

- **A full re-index over an existing database silently erased the nodes, pins and edges of some graphs**, leaving their `flow_assets` row (with the right `node_count`) in place. A full pass resets Monolith's `assets` table, so asset ids are handed out again while `flow_*` still holds rows under the old ones; the indexer cleaned up by the old id first, and when that id had already gone to a graph indexed earlier in the same pass, it deleted that graph's fresh rows. On a 78-graph project one full pass left 8 graphs without 114 nodes. The cleanup order is fixed. **After upgrading, run one full re-index** (`monolith_reindex` with `force: true`) to repair a database written by an earlier version - a graph is damaged when `flow_assets.node_count` differs from its number of `flow_nodes` rows.
- `flow_*` index tables kept rows for deleted and renamed Flow assets, so `list_assets` and every reverse lookup reported graphs that no longer exist. They are now reconciled against Monolith's `assets` table after each full index pass and on a short timer: rows of deleted packages are dropped, rows of renamed or moved assets follow the new path, orphaned child rows are swept.
- The Flow plugin was only detected at `Plugins/Flow`; a GitHub clone in `Plugins/FlowGraph` or any nested layout silently built the inactive stub. Detection now walks the project's whole `Plugins` tree, and the build log states where Flow was found or that it was not.
- Nodes were indexed under their own `NodeGuid`. Duplicated or imported nodes can share one `NodeGuid` under distinct keys in `UFlowAsset::Nodes`, and such nodes were merged into one row with their edges collapsed (10 of 12 nodes lost on one sample asset). The index, authoring answers and runtime rows now use the map key, which is what Flow resolves nodes and connections by; the indexer logs a warning for assets with mismatched guids.
- Assets of native `UFlowAsset` subclasses were never indexed: Monolith routes assets to an indexer by exact class name, and only `FlowAsset` was registered. Every native subclass loaded at editor start is now registered.
- `add_node` / `set_node_properties` could leave a half-applied change behind on error (a node created, then the call failed on its properties). Property JSON is now checked before anything is touched: unknown names, properties not editable in the Details panel (`NodeGuid`, pin arrays) and values of the wrong JSON shape are rejected - the converter itself silently turns a mismatch into a default value. A failure inside the edit rolls the transaction back.
- Each changed property now gets its own `PreEditChange` / `PostEditChangeProperty`, as an edit in the Details panel does.
- `connect_pins` reports `broken_links`: the links the schema removed to make room (an exec output drives one input).
- Builds against the Flow 2.4 line (`UFlowAsset::GetExpectedOwnerClass` now returns `TSoftClassPtr`).
- UE 5.8 deprecation of `FCoreDelegates::OnPostEngineInit`.

### Changed

- `list_assets` is paged: `offset` / `limit` (default 200, max 1000), answer carries `total` and `has_more`.
- Read actions accept the package path (`/Game/Flow/Foo`) as well as the object path (`/Game/Flow/Foo.Foo`) in `asset_path`, `asset_path_a` / `asset_path_b` and `target_asset_path`.
- Workflow skill rewritten for the full 29-action surface.

## [0.16.0]

Sibling-plugin port of the former in-tree Monolith fork handler: 29 actions (indexed reads, graph analysis, transactional authoring, auto-layout, validation, PIE runtime), own settings page, `flow_graph_comments` table and node positions in the index.
