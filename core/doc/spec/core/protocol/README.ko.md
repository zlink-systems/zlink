---
title: "Protocol"
---

[English](https://zlink-systems.github.io/zlink/spec/core/protocol/) | 한국어

<!-- zlink-nav:start -->
[Core 스펙 목차](../README.ko.md) | [이전: STREAM](../socket/08-stream.ko.md) | [다음: ZMP 프로토콜 상세](01-zmp.ko.md)
<!-- zlink-nav:end -->

# Protocol

> **이 장이 정의하는 것** — ZLink Core가 사용하는 wire protocol 문서의 색인.

이 문서는 ZLink Core의 wire protocol 문서를 연결한다. 각 문서는 해당 protocol의
상호운용 계약(byte 배치)과 그 배치를 만드는 내부 절차를 함께 설명한다.

| 문서 | 내용 |
|---|---|
| [01. Protocol — ZMP v1.0](01-zmp.ko.md) | ZMP wire protocol의 byte 배치와 그 encode/decode 내부 구현 |
| [02. Protocol — RAW](02-raw.ko.md) | STREAM socket이 사용하는, ZMP framing 없는 RAW wire format |
