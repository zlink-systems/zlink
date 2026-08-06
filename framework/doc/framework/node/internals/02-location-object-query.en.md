# Node.js Object Location Operational Query

This document defines how the Node.js Framework converts Location Store authority
records into the public `listObjectLocations(...)` result. Application code does not
handle Store keys, scan cursors, or authority payloads.

## Query boundary

The query requires an object kind and optionally accepts a stable type and MeshName.
The runtime keeps only records whose owner lease is currently valid, then projects
each record to a public entry containing the global ID, object generation, MeshName,
Node RID, state, and stable type.

The public page size is `1..1000`. The runtime uses the requested public page size as
the authority scan limit. Reading 1000 authority records and returning only a smaller
page would place unreturned records behind the continuation token and lose them from
small-page queries.

## Distinguishing Instance Spots

The authority-key discriminator distinguishes Actor records from the Spot family,
but it does not distinguish User Spots from Instance Spots. The object query therefore
does not derive object kind from the key discriminator. It uses the allocation kind in
the authority snapshot to distinguish `user_spot` and `instance_spot` records. This
prevents an Instance Spot created by cold activation from being reported as a User
Spot.

## Lifetime and errors

When the authority scan expires, the query fails instead of returning a partial result.
Callers must not treat the previous page as the current complete list. An authority
whose owner lease has expired is omitted; only records with a current owner are
exposed as locations.
