---
title: "Result와 errno 대응"
---

[English](04-errno-map.en.md) | 한국어

<!-- zlink-nav:start -->
[Core 스펙 목차](README.ko.md) | [이전: Errors](03-errors.ko.md) | [다음: Events](05-events.ko.md)
<!-- zlink-nav:end -->

# Result와 errno 대응

> **이 장이 정의하는 것** — 작업별 result enum 값과 native errno의 대응표.

이 문서는 ZLink Core raw public API의 result enum과 thread-local errno 대응을 정의한다. Result는
제어 흐름의 기준이고 errno는 같은 실패를 더 세밀하게 설명한다.

## 1. 공통 우선순위

한 호출에서 여러 실패 조건이 겹치면 argument, handle·lifecycle, target·connection 조회, capacity,
transport·internal failure 순서로 하나를 반환한다. 성공한 함수의 errno는 정의하지 않는다.

## 2. Submit result

| Result | errno | 의미 |
|---|---|---|
| `ZLINK_SUBMIT_OK` | - | 함수가 정한 ownership 전이가 완료됨 |
| `ZLINK_SUBMIT_BACKPRESSURED` | `EAGAIN`, `ETIMEDOUT`, `ENOBUFS` | socket queue 또는 reservation capacity 부족 |
| `ZLINK_SUBMIT_NOT_CONNECTED` | `ENOTCONN` | target connection 없음 |
| `ZLINK_SUBMIT_NOT_FOUND` | `ENOENT` | raw target 없음 |
| `ZLINK_SUBMIT_NOT_ADMITTED` | `EACCES` | handshake 또는 raw routing admission 거부 |
| `ZLINK_SUBMIT_TERMINATED` | `ETERM`, `ESHUTDOWN` | Context 또는 socket lifecycle 종료 |
| `ZLINK_SUBMIT_INVALID_HANDLE` | `EFAULT` | handle이 `NULL`이거나 종류가 다름 |
| `ZLINK_SUBMIT_INVALID_ARGUMENT` | `EINVAL`, `EMSGSIZE` | 잘못된 pointer, count, metadata 또는 flags |
| `ZLINK_SUBMIT_NOT_SUPPORTED` | `ENOTSUP` | handle에서 지원하지 않는 operation |
| `ZLINK_SUBMIT_INVALID_STATE` | `EBUSY`, `ESTALE`, `EALREADY`, `ESHUTDOWN` | socket lifecycle 또는 request state 오류 |
| `ZLINK_SUBMIT_THREAD_VIOLATION` | `EDEADLK`, `EPERM` | 금지한 callback 재진입 또는 thread 사용 |
| `ZLINK_SUBMIT_OUT_OF_MEMORY` | `ENOMEM` | 필요한 storage 확보 실패 |
| `ZLINK_SUBMIT_SEQ_EXHAUSTED` | `EOVERFLOW` | operation sequence 공간 소진 |
| `ZLINK_SUBMIT_INTERNAL_ERROR` | 보존된 errno | 다른 공개 분류가 없는 내부 실패 |

각 socket 문서가 input ownership과 socket별 세부 조건을 정의한다.

## 3. Request completion result

| Result | errno | 의미 |
|---|---|---|
| `ZLINK_REQUEST_OK` | - | terminal success |
| `ZLINK_REQUEST_TIMED_OUT` | `ETIMEDOUT` | operation deadline 만료 |
| `ZLINK_REQUEST_NOT_FOUND` | `ENOENT` | terminal target 부재 |
| `ZLINK_REQUEST_TERMINATED` | `ETERM`, `ESHUTDOWN` | owner lifecycle 종료 |
| `ZLINK_REQUEST_PROTOCOL_ERROR` | `EPROTO`, `ENOCOMPATPROTO` | malformed 또는 호환되지 않는 reply |
| `ZLINK_REQUEST_INTERNAL_ERROR` | 보존된 errno | 다른 terminal 분류가 없는 내부 실패 |
| `ZLINK_REQUEST_REJECTED` | `EACCES`, `ECANCELED` | peer 또는 admission 거절 |
| `ZLINK_REQUEST_CONFLICT` | `EEXIST`, `ESTALE` | request correlation 또는 generation 충돌 |
| `ZLINK_REQUEST_BUSY` | `EBUSY` | active request lifecycle 존재 |
| `ZLINK_REQUEST_NOT_CONNECTED` | `ENOTCONN` | terminal route 단절 |
| `ZLINK_REQUEST_INVALID_ARGUMENT` | `EINVAL` | asynchronous validation 실패 |
| `ZLINK_REQUEST_INVALID_STATE` | `ESTALE`, `EALREADY`, `ESHUTDOWN` | terminal request state 오류 |
| `ZLINK_REQUEST_NOT_SUPPORTED` | `ENOTSUP` | operation 미지원 |
| `ZLINK_REQUEST_BACKPRESSURED` | `EAGAIN`, `ENOBUFS` | non-blocking admission 또는 reservation 실패 |

Request submit 성공 뒤에는 operation ID마다 terminal result를 정확히 한 번 reply callback으로 전달한다.

## 4. Receive result

| Result | errno | 의미 |
|---|---|---|
| `ZLINK_RECV_OK` | - | complete record 하나 이상 수신 |
| `ZLINK_RECV_NO_DATA` | `EAGAIN`, `ETIMEDOUT` | nonblocking 또는 receive timeout에 data 없음 |
| `ZLINK_RECV_BUSY` | `EBUSY` | 다른 receive mode 사용 중 |
| `ZLINK_RECV_TERMINATED` | `ETERM` | Context 종료 |
| `ZLINK_RECV_INVALID_HANDLE` | `EFAULT` | handle 또는 필수 output pointer가 유효하지 않음 |
| `ZLINK_RECV_NOT_SUPPORTED` | `ENOTSUP` | handle이 해당 receive를 지원하지 않음 |
| `ZLINK_RECV_INTERNAL_ERROR` | 보존된 errno | 다른 공개 분류가 없는 내부 실패 |
| `ZLINK_RECV_BUFFER_TOO_SMALL` | `ENOBUFS` | caller output capacity 부족 |
| `ZLINK_RECV_INVALID_STATE` | `EINVAL`, `ESTALE`, `ESHUTDOWN` | receive lifecycle state 오류 |

Raw subscription의 `BUFFER_TOO_SMALL`에서는 필요한 topic 길이만 기록하고 queued topic·payload와 다른
output을 변경하지 않는다.

## 5. Handler와 close result

| Result | errno | 의미 |
|---|---|---|
| `ZLINK_HANDLER_INVALID_ARGUMENT` | `EINVAL` | handler 또는 mask가 잘못됨 |
| `ZLINK_HANDLER_BUSY` | `EBUSY` | 배타적인 receive model이 이미 등록됨 |
| `ZLINK_HANDLER_NOT_SUPPORTED` | `ENOTSUP` | handle에서 handler를 지원하지 않음 |
| `ZLINK_HANDLER_DEADLOCK` | `EDEADLK` | 같은 callback 안의 금지한 등록·해제 |
| `ZLINK_HANDLER_INVALID_HANDLE` | `EFAULT` | handle이 유효하지 않음 |
| `ZLINK_HANDLER_INTERNAL_ERROR` | 보존된 errno | 다른 공개 분류가 없는 내부 실패 |
| `ZLINK_CLOSE_BUSY` | `EBUSY`, `EDEADLK` | active child·callback·API가 존재하거나 같은 handle close가 재진입함 |
| `ZLINK_CLOSE_SHUTDOWN` | `ESHUTDOWN` | 이미 종료된 handle |
| `ZLINK_CLOSE_INVALID_HANDLE` | `EFAULT`, `ESTALE` | pointer 또는 opaque value가 유효하지 않음 |
| `ZLINK_CLOSE_INTERNAL_ERROR` | 보존된 errno | 다른 공개 분류가 없는 내부 실패 |

## 6. Bind와 connect result

| Result | errno | 의미 |
|---|---|---|
| `ZLINK_BIND_INVALID_ARGUMENT` | `EINVAL` | endpoint가 잘못됨 |
| `ZLINK_BIND_ADDR_IN_USE` | `EADDRINUSE` | endpoint가 이미 사용 중 |
| `ZLINK_BIND_NOT_SUPPORTED` | `ENOTSUP`, `EPROTONOSUPPORT` | transport 미지원 |
| `ZLINK_BIND_INVALID_HANDLE` | `EFAULT` | handle이 유효하지 않음 |
| `ZLINK_BIND_INTERNAL_ERROR` | 보존된 errno | 다른 공개 분류가 없는 bind 실패 |
| `ZLINK_CONNECT_INVALID_ARGUMENT` | `EINVAL` | endpoint 또는 expected RID가 잘못됨 |
| `ZLINK_CONNECT_NOT_SUPPORTED` | `ENOTSUP`, `EPROTONOSUPPORT` | transport 또는 operation 미지원 |
| `ZLINK_CONNECT_INVALID_HANDLE` | `EFAULT` | handle이 유효하지 않음 |
| `ZLINK_CONNECT_INTERNAL_ERROR` | 보존된 errno | 다른 공개 분류가 없는 connect 실패 |
| `ZLINK_CONNECT_NOT_FOUND` | `ENOENT` | connection intent가 없음 |
| `ZLINK_CONNECT_CONFLICT` | `EEXIST`, `ESTALE`, `EADDRINUSE` | routing ID, endpoint 또는 connection lifecycle 충돌 |
| `ZLINK_CONNECT_BUSY` | `EBUSY`, `ESHUTDOWN` | lifecycle이 변경을 허용하지 않음 |
| `ZLINK_CONNECT_AUTH_FAILED` | `EACCES` | transport peer 인증 실패 |

## 7. Configuration result

| Result | errno | 의미 |
|---|---|---|
| `ZLINK_CONFIG_INVALID_HANDLE` | `EFAULT` | handle 또는 output pointer가 유효하지 않음 |
| `ZLINK_CONFIG_INVALID_ARGUMENT` | `EINVAL`, `EMSGSIZE` | option, size, name 또는 value가 잘못됨 |
| `ZLINK_CONFIG_NOT_SUPPORTED` | `ENOTSUP` | handle과 option 조합 미지원 |
| `ZLINK_CONFIG_INTERNAL_ERROR` | 보존된 errno | 다른 공개 분류가 없는 내부 실패 |
| `ZLINK_CONFIG_INVALID_STATE` | `EINVAL`, `EBUSY`, `ESTALE`, `EALREADY`, `ESHUTDOWN`, `ENOTCONN`, `ETIMEDOUT`, `EPROTO` | socket lifecycle 또는 terminal state가 변경을 거부함 |
| `ZLINK_CONFIG_NOT_FOUND` | `ENOENT` | local query target 없음 |
| `ZLINK_CONFIG_CONFLICT` | `EEXIST` | 중복 identity, endpoint 또는 등록 값 |
| `ZLINK_CONFIG_BUFFER_TOO_SMALL` | `ENOBUFS` | caller output capacity 부족, partial output 없음 |
| `ZLINK_CONFIG_BUSY` | `EBUSY` | 같은 mutable object를 동시에 사용함 |
