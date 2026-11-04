# Changelog

## [0.18.0] - 2026-11-04

Builds and passes its automation tests on UE 5.7 + Flow 2.3, UE 5.8 + Flow 2.3 and UE 5.8 + the Flow 2.4 development line.

### Added

- **`apply_batch`**: several edits to one Flow Asset in one transaction, all or nothing. `operations` is a list of `add_node`, `set_node_properties`, `remove_node`, `add_comment`, `connect_pins`, `disconnect_pins` with their usual params; `add_node` takes an `alias`, and later operations refer to the new node as `"$alias"`. Every operation is checked before anything changes, a failure while applying undoes the whole batch, one editor Undo reverts it.
- **`dry_run`** on every graph edit and on `apply_batch`: the edit is applied, reported (new guids, broken links, per-operation results) and undone. Nothing is kept, the package is not dirtied.
- **Error codes.** A failed edit carries `code` in the error data (and `op_index` in a batch): `invalid_params`, `asset_not_found`, `no_editor_graph`, `unknown_op`, `unknown_alias`, `duplicate_alias`, `node_not_found`, `node_not_deletable`, `class_not_found`, `node_create_failed`, `unknown_property`, `read_only_property`, `wrong_type`, `pin_not_found`, `connection_refused`, `apply_failed`, `save_failed`, `rollback_failed`. An agent can fix and retry without parsing the message.
- **`get_node_history`** and the `flow_node_history` table: when a re-index finds that a node lost properties (a UPROPERTY removed or renamed in C++), the previous snapshot is kept, last 5 per node, with the names of the lost properties and the time it was taken. Setting *Keep Property History*, on by default.
- `flow_assets.indexed_at`: when each graph was last indexed, so a snapshot read from `flow_nodes.data` has a date.
- **Data-pin links** are indexed as edges with `kind = 'data'`, producer output to consumer input. `walk_execution` follows execution edges only.
- `runtime_get_instance` reports `activation_state` of every active and recorded node, takes an `owner` filter, and refuses to pick silently when several root instances run from one template: the error lists their owners.
- `diff_assets` reports `shared_node_count`, and a `note` when the two assets share no node guids - the case of a graph duplicated in the editor, which Flow gives new guids.
- Automation tests: 54 tests under `MonolithFlow.*` (indexer, reconcile, reads, analysis, authoring, batch and dry run, layout, validation, PIE runtime without PIE, registered surface). They build fixtures in memory and index into a temporary SQLite file, so they never touch project content or the project index. See *Running the tests* in the README.

### Fixed

- Data-pin links were indexed backwards, as orphaned output edges from the consumer, plus a fake `Exec` output pin named after the input.
- `is_orphaned` of connections was never written (the INSERT bound 5 of 6 values), so every edge read as current.
- `find_nodes_by_property` with `property_name` matched the value of any later property of the same node.
- Glob filters treated `_` and `%` in the pattern as SQL wildcards: `/Game/A_B*` also matched `/Game/AxB`.
- Read actions did not find a node given a lower-case or braced guid, which authoring accepts. Guid params are normalized everywhere, and enum params (`kind`, `pin_direction`, `source`, `layout_mode`, `formatter`) reject unknown values instead of returning nothing.
- The property snapshot carried the path of the node's editor wrapper, so two copies of one node never compared equal.
- Instanced object properties were rejected by `set_node_properties` in the inline shape the snapshot itself uses.
- `remove_node` deleted the Start node, which the editor does not allow (`node_not_deletable`).
- `auto_layout`: one loop in the graph stretched the layout to as many layers as the graph has nodes; `new_only` moved the Start node, which the factory places at the origin; an unknown `formatter` silently fell back to the built-in one.
- `create_asset` on Flow 2.3: a SubGraph node added to the fresh asset crashed the editor on the missing preload policy. Bad paths (no leading slash, trailing slash, unknown mount point, object name not repeating the package name, invalid characters) are now refused before the factory runs.
- `walk_execution`: a native subclass of `FlowNode_Start` or `FlowNode_CustomInput` is an entry point; a node whose only edge points at a node missing from the graph is a terminal; the order of `reached` and `unreachable` is stable.
- During a full index pass, a graph whose old id has gone to another graph no longer loses its rows before its own turn: they are parked under a negative id, so its previous property snapshot is still there to compare against. Reconciliation drops whatever is still parked when the pass ends.

## [0.17.0] - 2026-11-04

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
