# MonolithFlow

**[Flow](https://github.com/MothCocoon/FlowGraph) support for [Monolith](https://github.com/tumourlove/monolith)** - a sibling plugin that teaches Monolith's MCP server to read and edit Flow graphs, so AI assistants (Claude Code, Cursor, Cline) can work with your Flow assets the same way they work with Blueprints and Materials.

Registers the `flow` namespace (`flow_query` MCP tool) and a Flow asset indexer feeding Monolith's central SQLite project index.

## What you get

29 actions over Flow content - indexed reads, graph analysis, full authoring, and PIE runtime introspection:

| Category | Actions |
|---|---|
| Asset listing | `list_assets` (glob filter, paged), `get_asset_info` (counts, class breakdown, sample nodes) |
| Graph walk | `list_nodes`, `list_node_pins`, `list_connections`, `list_addons`, `get_node_info` (single-node deep dive: properties JSON + pins + edges + addon tree) |
| Graph analysis | `walk_execution` (reachability BFS from Start: depths, unreachable, terminals), `export_graph` (whole asset in one call: nodes + positions + edges + comments + custom events), `diff_assets` (structural diff of two graphs) |
| Cross-asset reverse lookup | `list_custom_events`, `find_subgraph_callers`, `find_node_class_usages`, `find_pins_by_type`, `find_nodes_by_property` (substring search over design-time property snapshots) |
| Class registry | `list_node_classes` (BP + native, kind/source/glob filters) |
| Authoring | `create_asset`, `add_node` (class + property JSON, context pins rebuilt), `set_node_properties`, `remove_node`, `connect_pins` / `disconnect_pins` (schema-mediated), `add_comment`, `save_asset` - all transactional (editor undo works) |
| Layout | `auto_layout` - layered left-to-right by execution flow (`all` / `new_only` / `selected` modes), or Blueprint Assist via Monolith's graph-formatter feature when installed |
| Validation | `validate_asset` - runs every registered data validator, incl. per-node `ValidateNode()` |
| PIE runtime | `runtime_list_instances`, `runtime_get_instance` (active nodes + activation history of a live graph) |
| Smoke | `ping` |

Typical questions it answers: *"which graphs use my custom Dialogue node?"*, *"is every node in this graph reachable from Start?"*, *"add a Log node after this one, wire it up and lay the graph out"*, *"what is this graph doing right now in PIE?"*.

Reads answer from the SQLite index (save an asset to refresh it); authoring mutates the live asset through the FlowEditor graph API - the same code path as manual editing, editor undo included. Any action that takes `asset_path` accepts either the object path (`/Game/Flow/Foo.Foo`) or the package path (`/Game/Flow/Foo`).

## Requirements

| | Version |
|---|---|
| Unreal Engine | 5.7 or 5.8 |
| [Monolith](https://github.com/tumourlove/monolith) | 0.24.0 or newer (hard dependency) |
| [Flow](https://github.com/MothCocoon/FlowGraph) | 2.3 |

Flow is optional at compile time: without it the module builds as an inactive stub with zero registered actions, and the build log says so.

## Installation

**From a release.** Download the zip for your engine version from [Releases](https://github.com/yashabogdanoff/MonolithFlow/releases) and unpack it into `YourProject/Plugins/MonolithFlow/`, next to Monolith and Flow. The zip carries editor binaries, so a project that already runs Monolith and Flow opens without a C++ rebuild. The release notes name the Monolith and Flow versions the binaries were built against - with a different Monolith or Flow build, delete `Plugins/MonolithFlow/Binaries` and let the editor rebuild the plugin from source.

**From source.** Clone this repository into `YourProject/Plugins/MonolithFlow/` and rebuild the editor target.

Flow can sit anywhere under the project's `Plugins` folder (`Plugins/Flow`, `Plugins/FlowGraph` from a GitHub clone, a nested `Plugins/ThirdParty/...`), in the engine's `Plugins/Marketplace`, or in `Engine/Plugins/Flow`. The build log prints `MonolithFlow: Flow plugin found at ...` or the stub notice, so a silent miss is visible.

Open the editor: the `flow` namespace appears in `monolith_discover()`, and Flow assets are picked up on the next index pass (full or incremental).

No Monolith sources are modified: the plugin registers through Monolith's exported `FMonolithToolRegistry` / `UMonolithIndexSubsystem` API, as described in Monolith's `Docs/SIBLING_PLUGIN_GUIDE.md`.

## Settings

Project Settings → Plugins → **Monolith Flow**:

| Setting | Default | Effect |
|---|---|---|
| Enable Flow | On | Register the `flow` MCP namespace (editor restart to apply) |
| Index Flow Assets | On | Index `UFlowAsset` / `UFlowNodeBlueprint` / `UFlowNodeAddOnBlueprint` into the project index |

## How it works

- **Indexer** (`FMonolithFlowIndexer`): walks each Flow asset at index time into 9 SQLite tables (`flow_assets`, `flow_nodes`, `flow_node_pins`, `flow_node_addons`, `flow_node_connections`, `flow_subgraph_refs`, `flow_custom_events`, `flow_graph_comments`, `flow_node_classes`). Every node/addon stores a full JSON snapshot of its design-time UPROPERTYs - the flat tables are normalized views over the useful subset.
- **Keeping up with deletes and renames**: the `flow_*` tables have no foreign key into Monolith's `assets` table, so Monolith's own cleanup never reaches them. The plugin reconciles them against `assets` itself - after every full index pass, and on a short timer in between: rows of deleted packages are dropped, rows of renamed or moved assets follow the new path.
- **Actions** (`FMonolithFlowActions`): pure SQL over those tables - fast, no asset loads at query time.
- Private Flow members (`UFlowNode_SubGraph::Asset`, `UFlowNode::Connections`) are read via reflection only - resilient to Flow plugin version drift as long as property names stay stable.
- Orphaned pins/connections left behind by dynamic-pin nodes are captured with an `is_orphaned` flag.

See [Docs/SPEC_MonolithFlow.md](Docs/SPEC_MonolithFlow.md) for the full schema and per-action reference. A Claude Code workflow guide ships in [Skills/unreal-flow/](Skills/unreal-flow/unreal-flow.md).

### The property snapshot outlives the C++

`flow_nodes.data` holds every design-time property value of every node as it was at the last index pass. When a refactor removes or renames a UPROPERTY, the engine can no longer read those values from the asset, and the next save overwrites them - but the index still has them until the asset is re-indexed. A plain SQLite read of `flow_nodes.data` (or `get_node_info`) is enough to recover the values and put them back with `set_node_properties`. Do it before saving the affected assets.

## Roadmap

- AddOn authoring (`add_addon` / `remove_addon`)
- Data-pin wiring for Flow 2.x data pins
- Richer runtime tooling (trigger inputs from MCP, breakpoint-style watches)

Ideas and known gaps are tracked in [Issues](https://github.com/yashabogdanoff/MonolithFlow/issues).

## License

MIT - see [LICENSE](LICENSE). Not affiliated with the Monolith or Flow projects.
