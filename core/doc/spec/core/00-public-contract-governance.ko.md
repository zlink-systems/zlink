---
title: "Core 공개 계약 관리"
---

[English](00-public-contract-governance.en.md) | 한국어

<!-- zlink-nav:start -->
[Core 스펙 목차](README.ko.md) | [이전: 개요](README.ko.md) | [다음: Context](01-context.ko.md)
<!-- zlink-nav:end -->

# Core 공개 계약 관리

> **이 장이 정의하는 것** — spec·header·test·package가 서로 어긋나지 않도록 지키는 절차.

이 문서는 ZLink Core 공개 계약의 원본, 문서 책임과 변경 절차를 정의한다. 대상 독자는 Core 공개
C ABI를 설계·구현·검토하는 개발자다.

## 1. 계약 원본

`core/doc/spec/core/`의 정식 spec은 Core가 제공해야 하는 목표 공개 동작을 정의한다.
`core/include/zlink.h`와 이 header가 포함하는 domain header는 같은 계약의 C ABI 표현이다. 구현, contract
test, bindings와 설치 package는 두 표현과 일치해야 한다.

Core 공개 C ABI는 application과 raw binding이 직접 사용하는 context, message, socket, transport,
eventing과 utility 기능만 제공한다. Framework service runtime을 위한 별도 public SPI, private C ABI 또는
compatibility facade를 두지 않는다. 정확한 책임 경계는 [Core runtime 경계](09-runtime-boundary.ko.md)가
소유한다.

정식 spec은 구현 편의를 이유로 축소하지 않는다. Header와 spec이 다르면 구현 완료로 판정하지 않으며
둘 가운데 하나를 별도 호환 계층으로 유지하지 않는다.

## 2. 문서 책임

정식 spec은 function signature, type과 상수, result와 errno, ownership, timeout, thread safety, callback과
close 의미를 정의한다. 사용 목적과 application 예제는 guide가 소유한다. Socket 배선, queue, lock,
thread와 protocol codec은 구현이 확정된 뒤 internals가 설명한다.

정식 spec·guide·internals는 Core 목표 상태만 설명한다. 구현 진행률과 대안은 plan과 execution
ledger가 소유한다. 정식 문서에서 plan을 계약 근거로 참조하지 않는다.

## 3. Core 목표 계약 우선 예외

Core의 service runtime 이관은 major version의 책임 경계를 다시 정한다. 이 작업에서는 공개 header와
현재 구현보다 정식 spec을 먼저 목표 계약으로 확정한다. 아직 구현되지 않은 목표도 draft가 아니라 정식
spec에 현재형으로 기록하고, 구현 차이는 v11 execution ledger에만 남긴다.

이 예외는 Core 이관 범위에만 적용한다. 다른 Core 변경은 저장소의 일반 draft 절차를 따른다.

## 4. 변경 절차

Core 공개 계약 변경은 다음 순서로 진행한다.

1. 한국어와 영문 정식 spec에 목표 계약을 같은 의미로 기록한다.
2. 함수, type, enum, 상수, result, ownership과 thread-safety 표를 검토한다.
3. 공개 header, exported symbol과 implementation을 정식 spec에 맞춘다.
4. contract test와 bindings package snapshot으로 raw public API를 검증한다.
5. 실제 구현과 구조 test가 확정된 뒤 internals를 잔존 구조에 맞춘다.
6. 한국어·영문·header·package의 동일성을 다시 검증한다.

## 5. 한국어와 영문 동일성

한국어와 영문 문서는 heading 순서, C signature, enum·struct field와 숫자 값, 기본값, result, ownership,
thread safety와 link 대상이 같아야 한다. 한 문서에만 존재하는 public 계약은 유효한 Core 계약으로
인정하지 않는다.
