# MonolithFlow

**[Flow](https://github.com/MothCocoon/FlowGraph) support for [Monolith](https://github.com/tumourlove/monolith)** - a sibling plugin that teaches Monolith's MCP server to read Flow graphs, so AI assistants (Claude Code, Cursor, Cline) can inspect your scenario/narrative assets the same way they inspect Blueprints and Materials.

Registers the `flow` namespace (`flow_query` MCP tool) and a Flow asset indexer feeding Monolith's central SQLite project index.

## What you get

29 actions over Flow content - indexed reads, graph analysis, full authoring, and PIE runtime introspection:

| Category | Actions |
|---|---|
| Asset listing | `list_assets` (glob filter), `get_asset_info` (counts, class breakdown, sample nodes) |
| Graph walk | `list_nodes`, `list_node_pins`, `list_connections`, `list_addons`, `get_node_info` (single-node deep dive: properties JSON + pins + edges + addon tree) |
| Graph analysis | `walk_execution` (reachability BFS from Start: depths, unreachable, terminals), `export_graph` (whole asset in one call: nodes + positions + edges + comments + custom events), `diff_assets` (structural diff of two graphs) |
| Cross-asset reverse lookup | `list_custom_events`, `find_subgraph_callers`, `find_node_class_usages`, `find_pins_by_type`, `find_nodes_by_property` (substring search over design-time property snapshots) |
| Class registry | `list_node_classes` (BP + native, kind/source/glob filters) |
| Authoring | `create_asset`, `add_node` (class + property JSON, context pins rebuilt), `set_node_properties`, `remove_node`, `connect_pins` / `disconnect_pins` (schema-mediated), `add_comment`, `save_asset` - all transactional (editor undo works) |
| Layout | `auto_layout` - layered left-to-right by execution flow (`all` / `new_only` / `selected` modes), or Blueprint Assist via Monolith's graph-formatter feature when installed |
| Validation | `validate_asset` - runs every registered data validator, incl. per-node `ValidateNode()` |
| PIE runtime | `runtime_list_instances`, `runtime_get_instance` (active nodes + activation history of a live graph) |
| Smoke | `ping` |

Typical AI-session workflows: *"which scenarios use my custom Dialogue node?"*, *"is every node in this graph reachable from Start?"*, *"add a Narrator after this node, wire it up and lay the graph out"*, *"what is this scenario doing right now in PIE?"*.

Reads answer from the SQLite index (save an asset to refresh it); authoring mutates the live asset through the FlowEditor graph API - the same code path as manual editing, editor undo included.

## Requirements

- Unreal Engine 5.7+
- [Monolith](https://github.com/tumourlove/monolith) installed at `Plugins/Monolith/` (hard dependency)
- [Flow](https://github.com/MothCocoon/FlowGraph) plugin (optional at compile time - without it the module builds as an inactive stub with zero registered actions)

## Installation

1. Copy this plugin to `YourProject/Plugins/MonolithFlow/` (beside `Plugins/Monolith/`).
2. Rebuild the project (C++ target).
3. Open the editor - the `flow` namespace appears in `monolith_discover()`, and Flow assets are picked up on the next index pass (full or incremental).

No Monolith sources are modified: the plugin registers through Monolith's exported `FMonolithToolRegistry` / `UMonolithIndexSubsystem` API, as described in Monolith's `Docs/SIBLING_PLUGIN_GUIDE.md`.

## Settings

Project Settings → Plugins → **Monolith Flow**:

| Setting | Default | Effect |
|---|---|---|
| Enable Flow | On | Register the `flow` MCP namespace (editor restart to apply) |
| Index Flow Assets | On | Index `UFlowAsset` / `UFlowNodeBlueprint` / `UFlowNodeAddOnBlueprint` into the project index |

## How it works

- **Indexer** (`FMonolithFlowIndexer`): walks each Flow asset at index time into 8 SQLite tables (`flow_assets`, `flow_nodes`, `flow_node_pins`, `flow_node_addons`, `flow_node_connections`, `flow_subgraph_refs`, `flow_custom_events`, `flow_node_classes`). Every node/addon stores a full JSON snapshot of its design-time UPROPERTYs - the flat tables are normalized views over the useful subset.
- **Actions** (`FMonolithFlowActions`): pure SQL over those tables - fast, no asset loads at query time.
- Private Flow members (`UFlowNode_SubGraph::Asset`, `UFlowNode::Connections`) are read via reflection only - resilient to Flow plugin version drift as long as property names stay stable.
- Orphaned pins/connections left behind by dynamic-pin nodes are captured with an `is_orphaned` flag.

See [Docs/SPEC_MonolithFlow.md](Docs/SPEC_MonolithFlow.md) for the full schema and per-action reference. A Claude Code workflow guide ships in [Skills/unreal-flow/](Skills/unreal-flow/unreal-flow.md).

## Roadmap

- AddOn authoring (`add_addon` / `remove_addon`)
- Data-pin wiring for Flow 2.x data pins
- Richer runtime tooling (trigger inputs from MCP, breakpoint-style watches)

## Field log

The plugin is on shakedown before it goes upstream, and its only user is the MCP client. Every
defect, oddity and improvement idea found while using it lands here, with enough detail to
reproduce. An entry is removed once the fix is in and the repro comes back clean - the log carries
open items, not history.

### Open: `flow_assets` keeps rows for packages that no longer exist

Measured 2026-08-12, plugin 0.16.0 on Monolith 0.22.0, project index built by a full pass.

The `flow_*` tables are never purged of renamed or deleted packages, while Monolith's own `assets`
table is. Straight from `Plugins/Monolith/Saved/ProjectIndex.db`:

```sql
select count(*) from flow_assets;                          -- 112
select count(*) from assets where asset_class='FlowAsset'; --  77
```

77 of those 112 rows map to a file on disk. The other 35 are paths this project's modules 08 and 09
carried before their content folders were renamed, and they still drag 653 `flow_nodes` rows behind
them. `list_assets` reports all 112.

Every reverse-lookup action answers off those rows, so it reports graphs that do not exist. The case
that surfaced it - deciding whether a deprecated node could be deleted:

```
flow_query find_node_class_usages
  node_class = /Script/Simulator.SimulatorFlowNode_LocationMark
```

returns 6 hosts and 12 nodes. All six are ghosts. Two independent proofs:

```
project_query get_saved_asset_state
  asset_path = /Module_09_WeltzpechUKMK/01_Office/FLOW_M09_OfficeA
  -> error: not found in registry or on disk
```

and a byte scan of all 77 `FLOW_*.uasset` files present on disk, none of which contains the string
`LocationMark` anywhere in its name table. The live asset at that folder's real path,
`/Module_09_WeltzpechUKMK/01_OfficeA/FLOW_M09_OfficeA`, holds 33 nodes and no such node; the ghost
row claims 34 and holds two.

Re-measured 2026-11-01 on Monolith 0.24.0 right after a full project reindex: `list_assets` returns
114 rows against 78 `FlowAsset` rows in `assets`. A full pass does not clear the ghosts either.

The ghosts are recognisable once suspected - the same node GUIDs repeat across two or three paths
that differ only by folder - but nothing in the answer says a host is stale, and a caller with no
prior knowledge of the renames has no reason to doubt it.

Cost when it bites: a deprecated class reads as still in use and its removal gets blocked, or the
reverse, an author edits the ghost's path and wonders why the graph does not change. Here it cost a
full byte-level re-verification of the content tree to establish that the real usage count was zero.

Why it happens, read from Monolith 0.24.0: Monolith's own tables hang off `assets` with
`ON DELETE CASCADE`, so `DeleteAssetByPath` on a delete or rename clears them, and a full pass
starts with `ResetDatabase`, which drops them by name. The `flow_*` tables have no foreign key to
`assets` (deliberately, so `ResetDatabase` cannot fail on them) and are not in its drop list, so
neither path touches them. Monolith's `MonolithLevelSequence` module stores its tables the same way
and should carry the same ghosts.

### Open: the Flow plugin is only found at `Plugins/Flow`

`MonolithFlow.Build.cs` probes `<Project>/Plugins/Flow/Flow.uplugin`, the engine's
`Plugins/Marketplace` tree and `<Engine>/Plugins/Flow`. Cloning MothCocoon's repository gives a
folder named `FlowGraph`, and any nested layout (`Plugins/ThirdParty/Flow`) is missed too. The
module then builds as the inactive stub, with no error and no warning, and `flow` never appears in
`monolith_discover`. Fix: scan the project's `Plugins` tree recursively for `Flow.uplugin`, and
log which path was found (or that none was).

### Idea: flag or drop rows whose package is gone

Until the indexer purges on delete and rename, `list_assets` and the reverse-lookup actions could
stat the package and either drop the row or return it with an `exists_on_disk` field, the way
`project_query get_saved_asset_state` already does. A caller can then filter instead of learning the
failure mode the hard way.

### Idea: `list_assets` should page

`flow_query list_assets` with `{"limit": 3}` returns every row (114 here) plus the warning
`Unknown param 'limit' for action 'flow:list_assets'`. A caller that only wants to confirm the
namespace is alive, or to look at a few graphs, pulls the whole table into its context. Monolith's
own list actions take `limit` / `offset` and report `has_more`; matching that keeps the contract
uniform.

### Worked: bulk re-pointing of 17 nodes across 12 assets

`set_node_properties` + `save` took a `TObjectPtr<UDataAsset>` property from a plain object path
string (`/Module_10_GPM/Data/Tests/PPE_M10`) on 17 nodes of two classes across 12 Flow assets,
without a single failure or a manual step in the editor. `validate_asset` then confirmed all 12 in
one pass each, per-node `ValidateNode()` included. Node GUIDs came out of the index
(`flow_nodes.node_guid`), so the whole job needed no graph opening at all.

Two small notes for the docs rather than defects: `asset_path` wants the full object path
(`/Pkg/Name.Name`) - the package path alone was not tried and may or may not resolve; and
`applied_properties` counts fields, which is the number worth asserting on in a script.

### The index `data` snapshot doubled as a data recovery point

Worth knowing, and possibly worth stating in the README as a feature. A refactor removed several
UPROPERTYs from two node classes; the values in 16 already-authored nodes became unreadable to the
engine on the next load, and the graphs' own bytes were one save away from being overwritten. The
per-node `data` JSON that `MonolithFlowIndexer` had captured before the rebuild still held every
value, and a plain SQLite read of `flow_nodes.data` restored the whole authoring set - six rows of
four options with their correctness flags, across ten modules.

Nothing about the plugin failed here; the point is that the snapshot is the only artefact in the
project that survives a property being deleted from C++, and callers should be told it can be used
that way before they need it. A `--since` or timestamp column on the snapshot would make it a
proper point-in-time backup rather than a happy accident.

## License

MIT - see [LICENSE](LICENSE). Not affiliated with the Monolith or Flow projects.
