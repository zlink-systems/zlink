<!-- SPDX-License-Identifier: LicenseRef-Zlink -->
# Network sync wire v1

`wire-v1.json` is the single definition of the packet names and payload fields. Every packet is an existing STREAM typed application one-way message encoded with the default `framework-json-v1` profile. In the fixtures, `packet` is the STREAM packet name and `payload` is the exact JSON payload text. Metadata outside the fixture entry is not part of the wire.

Every defined field is required. `?` allows an explicit `null`; omitting the field is not allowed. Unknown fields are ignored, and duplicate fields are rejected in every object. Strings are case sensitive. `json` is a JSON payload owned by the game; its structure and nullability are defined by the game DTO. Byte sequences use the padded base64 string of the default profile.

| Definition type | Representation and range |
|---|---|
| `u64` | Decimal string without sign or leading zeros, 0..18446744073709551615 |
| `positive_u64` | A `u64` in 1..18446744073709551615 |
| `i64` | Decimal string without leading zeros or a plus sign, -9223372036854775808..9223372036854775807. `-0` is not allowed |
| `string`, `bool` | JSON string, JSON boolean |
| `T[]` | JSON array of T |

`match_id` is an opaque match identifier. `epoch`, `tick`, `target_tick`, `last_snapshot_tick`, `last_applied_input_tick`, `probe_id` and `action_id` are a generation, a tick or an identifying sequence number. `t1_us`, `hold_us`, `since_tick_us`, `arrival_margin_us` and `tick_us` are in microseconds. `tick_us` is positive and only `arrival_margin_us` may be negative. `tick` is the latest completed tick. `input_feedback` is the observation for that input, and `used_fallback_input` tells whether the last applied input tick used fallback input.

`u64`, `positive_u64` and `i64` are 64-bit integers within the ranges in the table, written as decimal JSON strings. `disposition` is written as a JSON string holding a name declared under `enums` in `wire-v1.json`, with matching case. Undeclared names, JSON numbers and numeric ordinal strings are rejected. Integer and enum representation follow [framework-json-v1 §5](../../../framework/doc/framework/common/spec/server/00-foundation/05-message-model.en.md#5-the-framework-json-v1-typed-payload-profile).

Validation: `python3 plugins/network-sync/protocol/validate.py`. It checks only the definition and the fixtures; it does not replace serializer conformance tests in each language.
