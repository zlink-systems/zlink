# ZLink project structure format v1

[한국어](STRUCTURE-FORMAT.ko.md)

`zlink.structure.json` describes a downloadable server project and its diagram. The Initializr and the structure editor produce the same UTF-8 JSON format. This file describes application composition; Framework public contracts determine runtime behavior. Every genre template in every server language uses this format.

## Root fields

| Field | Type | Meaning |
| --- | --- | --- |
| `version` | integer | Format version. This document defines `1`. Unknown versions must be rejected. |
| `id` | string | Topology ID from the landing diagram, such as `mmo-seamless` or `board-tables`. |
| `name` | string | Project name. Initializr replaces `my-game` when generating the project. |
| `language` | string | Server language: `dotnet`, `java`, `kotlin`, `node`, or `cpp`. |
| `run` | object | Required in format v1. Build and e2e commands, plus the ordered e2e scenario IDs. |
| `nodes` | array | Diagram participants, including external clients and infrastructure. |
| `channels` | array | Named channel registrations owned by application nodes. |
| `spots` | array | Fixed logical Spots and dynamically allocated Spot families. |
| `edges` | array | Directed diagram connections, in display order. |

## Run

Format v1 requires `run.commands` and `run.scenarios`. `commands` contains exactly the `sh` and `powershell` keys. Each value is a nonempty array of nonempty command strings; the structure checker validates this shape but does not execute or compare the command text. The arrays list the Build, default Local e2e, and scenario-selection commands shown in the project README.

`scenarios` is an array of e2e scenario IDs in execution order. It must match the canonical scenario list in the project's e2e client, including order.

## Nodes

Each node contains `id`, `role`, `title`, and `config`. IDs are unique, nonempty strings. They identify participants within this composition; node counts and IDs are not fixed to a preset. `title` is the diagram label. `role` identifies the application responsibility, such as `external`, `session`, `zone`, `table`, `instance`, or `location-store`. Roles are not a closed preset list.

`config` is a relative path to the node's configuration file, using `/`. External clients use `null`; no game client project is generated. Paths must remain inside the project root. A `location-store` node references its infrastructure configuration, such as `docker-compose.yml` for Redis. For an application process, the referenced JSON must have an `Id` equal to the node ID.

A role is a deployment responsibility. A role shown as ×N in the diagram, such as `instance` or `table`, can run as multiple processes. A role does not select a Framework Spot kind; for example, `instance` hosts Dungeon User Spots, not the actor-free Framework Instance Spot kind.

## Channels

Each channel contains a nonempty `name`, an `owner` node ID, and a `mesh` name. The pair `(name, owner)` is unique. Multiple nodes can register the same channel name in the same mesh. A channel declaration represents the owner's server registration, not a dedicated transport connection.

## Spots

Each Spot contains a unique nonempty composition `id`, a `kind` (`entry`, `user`, or `instance`), a `type`, and an `owner` node ID. User and Instance Spots use their Framework stable type as `type`. Entry Spots use `null`: Framework issues their runtime IDs and they have no stable-type registration. Declare an Entry Spot only on a node where the application registers an Entry and its first-packet handler. Do not declare the default Entry that Framework creates internally on each Object Server. The owner is the initial host in the downloaded composition. Runtime relocation can change the current owner; this structure file does not replace Location Store authority.

For a fixed User or Instance Spot, `logicalId` is required and gives its exact runtime logical ID. A Spot's runtime identity is its mesh and logical ID, so fixed logical IDs are unique across stable types within one mesh. A Spot names its mesh in an optional `mesh` field; the field is required when the composition declares more than one mesh, for example one mesh per world. When `dynamic` is `true`, runtime requests choose logical IDs and `logicalId` is absent. For example, composition ID `dungeon` denotes the family of `Dungeon` User Spots created by party ID through `GetOrCreate`. Entry Spots omit both `logicalId` and `dynamic`; their composition ID remains stable when Framework issues a new runtime ID for a replacement node.

## Edges

Each edge contains existing node IDs `from` and `to`, and a nonempty `label`. Optional `semantic` preserves the landing diagram's semantic tag. Array order controls diagram display. The consistency checker accepts added or reordered edges and does not assign behavior by array index. Edges describe the diagram's primary flows; they do not enumerate every mesh connection or Redis client.

## Consistency validation

Every e2e run validates the structure before node startup. The check verifies version, nonempty root fields, the `run` shape and scenario list, unique node and composition Spot IDs, unique channel registrations and logical Spot identities, owner and edge references, relative configuration paths within the project, and application configuration IDs. It compares declared channel names, meshes and Spot types with server constants. A template can add checks that compare its own configuration with declared Spots; the seamless template compares each node's fixed Zone `logicalId` set with its configured `ZoneIds`.

The check does not require a preset ID, fixed node or Spot counts, fixed deployment IDs, edge lists, or array positions. Added nodes, Spots and edges are accepted when they satisfy these rules and the application contracts. Successful parsing alone is insufficient.

Reference artifacts:

- [MMO seamless world, .NET](../languages/dotnet/templates/mmo-seamless/my-game/zlink.structure.json)
