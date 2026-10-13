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
| `edges` | array | Directed diagram connections, in landing diagram order. |

## Nodes

Each node contains `id`, `role`, `title`, and `config`. IDs are unique, nonempty strings and are identical to the landing diagram IDs. `title` is the diagram label. `role` identifies the application responsibility: this template uses `external`, `session`, `zone`, `instance`, and `location-store`.

`config` is a relative path to the node's configuration file, using `/`. External clients use `null`; no game client project is generated. Paths must remain inside the project root. For Redis, `config` names `docker-compose.yml`. For an application process, the referenced JSON must have an `Id` equal to the node ID.

`instance` is a deployment role. It can have multiple processes. The template starts one process and hosts Dungeon User Spots; this role does not imply the actor-free Framework Instance Spot kind.

## Channels

Each channel contains a nonempty `name`, an `owner` node ID, and a `mesh` name. The pair `(name, owner)` is unique. Multiple nodes can register the same channel name in the same mesh. A channel declaration represents the owner's server registration, not a dedicated transport connection. The seamless template registers `border` on both Zone nodes in `my-game-world`.

## Spots

Each Spot contains a unique nonempty composition `id`, a `kind` (`entry`, `user`, or `instance`), a `type`, and an `owner` node ID. User and Instance Spots use their Framework stable type as `type`. Entry Spots use `null`: Framework issues their runtime IDs and they have no stable-type registration. The owner is the initial host in the downloaded composition. Runtime relocation can change the current owner; this structure file does not replace Location Store authority.

For a fixed User or Instance Spot, `logicalId` is required and gives its exact runtime logical ID. When `dynamic` is `true`, runtime requests choose logical IDs and `logicalId` is absent. For example, composition ID `dungeon` denotes the family of `Dungeon` User Spots created by party ID through `GetOrCreate`. Entry Spots omit both `logicalId` and `dynamic`; their composition ID remains stable when Framework issues a new runtime ID for a replacement node.

Fixed Zone `logicalId` values must equal the owning configuration's `ZoneIds`. `ZoneCapacity` is a placement limit, not the count of initially hosted Zones. Zone B has capacity six so it can receive A's three Zones during drain.

## Edges

Each edge contains existing node IDs `from` and `to`, and a nonempty `label`. Optional `semantic` preserves the landing diagram's semantic tag. Array order must match that diagram so moment highlights continue to address the same edge indices. Edges describe the diagram's primary flows; they do not enumerate every mesh connection or Redis client.

The seamless topology has the following exact directed edges:

| Index | From | To | Label | Semantic |
| --- | --- | --- | --- | --- |
| 0 | players | session | STREAM | |
| 1 | session | zoneA | relay | |
| 2 | zoneA | zoneB | relocation | |
| 3 | zoneB | zoneA | border pub/sub | border-updates |
| 4 | zoneB | instance | GetOrCreate | |
| 5 | zoneB | location | Location / lease | location-authority |

## Consistency validation

The template's structure check verifies version, unique identifiers, owner and edge references, safe configuration paths, node configuration IDs, initial Zone ownership, and the declared channel and Spot types against the server contracts. A malformed structure must fail before node startup. The validation suite also tests deliberately inconsistent structures; successful parsing alone is insufficient.

Reference artifact: [MMORPG seamless world](mmorpg-seamless/my-game/zlink.structure.json).
