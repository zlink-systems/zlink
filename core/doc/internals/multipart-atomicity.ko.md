---
title: "Multipart atomicity"
---

<!-- zlink-nav:start -->
[가이드 목차](../guide/README.ko.md) | [이전: Connection별 memory](connection-memory.ko.md) | [다음: Core raw runtime 내부 경계](runtime-boundary.ko.md)
<!-- zlink-nav:end -->

# Multipart atomicity

> **이 장의 계약 소유 문서** — multipart 프레이밍의 공개 계약은
> [메시지 API 레퍼런스](../spec/core/02-message.ko.md)가 다룬다. 이 장은 다른 sender의 part가
> 섞이지 않도록 내부에서 어떻게 보호하는지 설명한다.

Core는 `ZLINK_PART_MORE`부터 `ZLINK_PART_FINAL`까지의 part를 하나의 논리적 multipart sequence로
처리한다. 다른 sender의 message part가 이 sequence 사이에 삽입되지 않도록 socket별 transaction state가
send 경로를 보호한다.

## Send

첫 part가 transaction을 시작하고 final part가 commit한다. Send가 중간에 실패하면 남은 caller-owned
part를 소비하지 않으며, 내부 transaction state를 정리해 다음 message가 이전 sequence를 이어받지 않게
한다. Caller는 반환값에 따라 아직 소유한 part를 close하거나 재사용한다.

## Receive

Typed receive API는 part 하나와 `ZLINK_PART_MORE` 또는 `ZLINK_PART_FINAL`을 반환한다. Receive helper는
같은 handle, socket family, source와 owner thread가 sequence 동안 바뀌지 않는지 확인한다. Sequence를
중단하면 buffered part를 close하고 helper state를 초기화한다.

## Request/reply

Request control part와 application payload는 한 transaction으로 전송된다. Receive 경로는 control part를
검증하고 제거한 뒤 request sequence와 peer routing id를 typed metadata로 반환한다.

## Concurrency

여러 thread가 독립된 message를 보낼 수 있지만 하나의 multipart message를 thread 사이에 나누면 안 된다.
Receive는 single-consumer 계약을 따른다.
