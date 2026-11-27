<!-- SPDX-License-Identifier: LicenseRef-Zlink -->
# Network sync wire v1

`wire-v1.json`은 packet 이름과 payload 필드의 단일 정의입니다. 기존 STREAM의 typed application one-way 메시지이며 기본 `framework-json-v1`을 사용합니다. fixture의 `packet`은 STREAM packet 이름이고 `payload`는 실제 JSON payload 텍스트입니다. fixture 바깥의 메타데이터는 wire에 포함되지 않습니다.

모든 정의된 필드는 필수입니다. `?`는 명시적인 null을 허용하며 필드 생략은 허용하지 않습니다. 알 수 없는 필드는 무시하고, 모든 객체의 중복 필드는 거부합니다. 문자열은 대소문자를 구분합니다. `json`은 게임이 정하는 JSON payload이며 그 구조와 null 허용 여부는 게임 DTO가 정합니다. byte sequence는 기본 profile의 padding된 base64 문자열입니다.

| 정의 타입 | 표현과 범위 |
|---|---|
| `u64` | 부호와 선행 0 없는 10진 문자열, 0..18446744073709551615 |
| `positive_u64` | `u64` 중 1..18446744073709551615 |
| `i64` | 선행 0과 양수 부호가 없는 10진 문자열, -9223372036854775808..9223372036854775807. `-0`은 허용하지 않음 |
| `string`, `bool` | JSON 문자열, JSON boolean |
| `T[]` | T의 JSON 배열 |

`match_id`는 불투명한 매치 식별자입니다. `epoch`, `tick`, `target_tick`, `last_snapshot_tick`, `last_applied_input_tick`, `probe_id`, `action_id`는 각각 세대·틱 또는 식별 순번입니다. `t1_us`, `hold_us`, `since_tick_us`, `arrival_margin_us`, `tick_us`의 단위는 마이크로초입니다. `tick_us`는 양수이고 `arrival_margin_us`만 음수를 허용합니다. `tick`은 최신 완료 틱입니다. `input_feedback`은 해당 입력의 관측값이고 `used_fallback_input`은 마지막 적용 입력 틱의 대체 입력 사용 여부입니다.

`u64`, `positive_u64`, `i64`는 표의 범위를 따르는 64비트 정수이며 10진 JSON 문자열로 표현합니다. `disposition`은 `wire-v1.json`의 `enums`에 선언한 이름을 대소문자까지 맞춰 JSON 문자열로 표현합니다. 선언하지 않은 이름, JSON 숫자, 숫자 ordinal 문자열은 거부합니다. 정수와 enum 표현은 [framework-json-v1 §5](../../../doc/framework/common/spec/server/00-foundation/05-message-model.ko.md#5-framework-json-v1-typed-payload-profile)를 따릅니다.

검증: `python3 framework/plugins/network-sync/protocol/validate.py`. 이 검증은 정의와 fixture만 검사하며 실제 언어별 serializer의 적합성 시험을 대신하지 않습니다.
