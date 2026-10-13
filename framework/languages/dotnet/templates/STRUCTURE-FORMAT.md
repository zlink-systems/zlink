# ZLink project structure format v1

[한국어](STRUCTURE-FORMAT.ko.md)

`zlink.structure.json` describes a downloadable server project and its diagram. The Initializr and the structure editor produce the same UTF-8 JSON format. This file describes application composition; Framework public contracts determine runtime behavior.

## Root fields

| Field | Type | Meaning |
| --- | --- | --- |
| `version` | integer | Format version. This document defines `1`. Unknown versions must be rejected. |
| `id` | string | Topology ID from the landing diagram, such as `mmo-seamless`. |
| `name` | string | Project name. Initializr replaces `my-game` when generating the project. |
| `language` | string | Server language. This template uses `dotnet`. |
| `nodes` | array | Diagram participants, including external clients and infrastructure. |
| `channels` | array | Named channel registrations owned by application nodes. |
| `spots` | array | Fixed logical Spots and dynamically allocated Spot families. |
| `edges` | array | Directed diagram connections, in display order. |

## Nodes

Each node contains `id`, `role`, `title`, and `config`. IDs are unique, nonempty strings. They identify participants within this composition; node counts and IDs are not fixed to a preset. `title` is the diagram label. `role` identifies the application responsibility: the initial template uses `external`, `session`, `zone`, `instance`, and `location-store`. Roles are not a closed preset list.

`config` is a relative path to the node's configuration file, using `/`. External clients use `null`; no game client project is generated. Paths must remain inside the project root. A `location-store` node references its infrastructure configuration, such as `docker-compose.yml` for Redis. For an application process, the referenced JSON must have an `Id` equal to the node ID.

`instance` is a deployment role. It can have multiple processes. The template starts one process and hosts Dungeon User Spots; this role does not imply the actor-free Framework Instance Spot kind.

## Channels

Each channel contains a nonempty `name`, an `owner` node ID, and a `mesh` name. The pair `(name, owner)` is unique. Multiple nodes can register the same channel name in the same mesh. A channel declaration represents the owner's server registration, not a dedicated transport connection. The seamless template registers `border` on both Zone nodes in `my-game-world`.

## Spots

Each Spot contains a unique nonempty composition `id`, a `kind` (`entry`, `user`, or `instance`), a `type`, and an `owner` node ID. User and Instance Spots use their Framework stable type as `type`. Entry Spots use `null`: Framework issues their runtime IDs and they have no stable-type registration. The owner is the initial host in the downloaded composition. Runtime relocation can change the current owner; this structure file does not replace Location Store authority.

For a fixed User or Instance Spot, `logicalId` is required and gives its exact runtime logical ID. Fixed logical IDs are unique across stable types within the composition. When `dynamic` is `true`, runtime requests choose logical IDs and `logicalId` is absent. For example, composition ID `dungeon` denotes the family of `Dungeon` User Spots created by party ID through `GetOrCreate`. Entry Spots omit both `logicalId` and `dynamic`; their composition ID remains stable when Framework issues a new runtime ID for a replacement node.

Fixed Zone `logicalId` values must equal the owning configuration's `ZoneIds`. `ZoneCapacity` is a placement limit, not the count of initially hosted Zones. Zone B has capacity six so it can receive A's three Zones during drain.

## Edges

Each edge contains existing node IDs `from` and `to`, and a nonempty `label`. Optional `semantic` preserves the landing diagram's semantic tag. Array order controls diagram display. The consistency checker accepts added or reordered edges and does not assign behavior by array index. Edges describe the diagram's primary flows; they do not enumerate every mesh connection or Redis client.

The initial seamless composition has the following directed edges. These are examples, not required edges for every project:

| Index | From | To | Label | Semantic |
| --- | --- | --- | --- | --- |
| 0 | players | session | STREAM | |
| 1 | session | zoneA | relay | |
| 2 | zoneA | zoneB | relocation | |
| 3 | zoneB | zoneA | border pub/sub | border-updates |
| 4 | zoneB | instance | GetOrCreate | |
| 5 | zoneB | location | Location / lease | location-authority |

## Consistency validation

The template's structure check verifies version, nonempty root fields, unique node and composition Spot IDs, unique channel registrations and logical Spot identities, owner and edge references, relative configuration paths within the project, and application configuration IDs. It compares each application node's declared Zone `logicalId` set with its configured `ZoneIds`, and compares declared channel names, meshes and Spot types with server constants. It does not require a preset ID, fixed node or Spot counts, fixed deployment IDs, edge lists, array positions, or drain capacity. Added nodes, Spots and edges are accepted when they satisfy these rules and the application contracts.

Every e2e run validates the requested composition before node startup. Only `--check-structure` also runs the checker self-tests: an extended and reordered valid composition, and deliberately inconsistent inputs that must be rejected. Successful parsing alone is insufficient.

Reference artifact: [MMORPG seamless world](mmorpg-seamless/my-game/zlink.structure.json).
