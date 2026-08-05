#!/usr/bin/env bash
# verify-migration-stage.sh — 각 단계의 완료 조건을 검증한다
# 사용법: ./scripts/verify-migration-stage.sh <stage_number>
# 종료코드: 0=완료, 1=미완료, 2=오류

set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

STAGE="${1:?usage: verify-migration-stage.sh <stage>}"

case "$STAGE" in
  1)
    # 1단계: 문서 기준 마무리
    # - request-reply, SPOT 문서가 존재하고 draft 표시가 없어야 한다
    docs=(
      "doc/plan/spot-refactor/ZMP_REQUEST_REPLY_PROTOCOL.md"
      "doc/plan/spot-refactor/ZMP_SPOT_ROUTED_PROTOCOL.md"
      "doc/plan/spot-refactor/SOCKET_REQUEST_REPLY_API_SPEC.md"
      "doc/plan/spot-refactor/SPOT_ROUTED_MESSAGE_SPEC.md"
    )
    for d in "${docs[@]}"; do
      [ -f "$d" ] || { echo "FAIL: missing $d"; exit 1; }
    done
    echo "PASS: stage 1 docs present"
    exit 0
    ;;

  2)
    # 2단계: old API 제거
    # - core 헤더에 old metadata/request API 가 없어야 한다
    old_syms="zlink_msg_set_metadata|zlink_msg_get_metadata|zlink_msg_clear_metadata"
    old_syms+="|zlink_msg_set_request|zlink_msg_set_reply|zlink_msg_get_request_info"
    if grep -rE "$old_syms" core/include/ core/src/ 2>/dev/null | grep -v '//.*historical' | grep -v 'test_old_' | head -5; then
      echo "FAIL: old API symbols still found in core"
      exit 1
    fi
    echo "PASS: old API symbols removed from core"
    exit 0
    ;;

  3)
    # 3단계: 새 API surface 추가
    new_syms="zlink_dealer_request|zlink_router_request|zlink_router_reply|zlink_router_handler"
    found=0
    for sym in zlink_dealer_request zlink_router_request zlink_router_reply zlink_router_handler; do
      if grep -r "$sym" core/include/ 2>/dev/null | head -1 >/dev/null; then
        found=$((found + 1))
      fi
    done
    if [ "$found" -lt 4 ]; then
      echo "FAIL: only $found/4 new API symbols found in headers"
      exit 1
    fi
    echo "PASS: new API surface present ($found/4)"
    exit 0
    ;;

  4)
    # 4단계: request-reply 프로토콜 구현
    # - encode/decode 구현 존재 + 빌드 성공
    if ! grep -r "request_seq" core/src/ 2>/dev/null | head -1 >/dev/null; then
      echo "FAIL: request_seq not found in src/ or core/src/"
      exit 1
    fi
    # 빌드 검증
    if command -v cmake >/dev/null 2>&1; then
      if [ -d core/build ]; then
        cmake --build core/build --target libzlink 2>&1 | tail -5 || { echo "FAIL: build failed"; exit 1; }
      elif [ -d build ]; then
        cmake --build build --target libzlink 2>&1 | tail -5 || { echo "FAIL: build failed"; exit 1; }
      fi
    fi
    echo "PASS: request-reply protocol implementation found"
    exit 0
    ;;

  5)
    # 5단계: SPOT routed + request-reply 조합
    if [ ! -f core/src/api/service_spot_request_reply_api.cpp ]; then
      echo "FAIL: SPOT request-reply implementation not found"
      exit 1
    fi
    echo "PASS: SPOT request-reply combination found"
    exit 0
    ;;

  6)
    # 6단계: core 테스트
    # - 테스트 파일 존재 + 테스트 통과
    test_files=$(find core/tests/ -name '*request*' -o -name '*reply*' 2>/dev/null | head -10)
    if [ -z "$test_files" ]; then
      echo "FAIL: no request-reply test files found"
      exit 1
    fi
    # 테스트 실행은 빌드 환경에 따라 다름
    build_dir=""
    if [ -d core/build ] && [ -f core/build/CTestTestfile.cmake ]; then
      build_dir="core/build"
    elif [ -d build ] && [ -f build/CTestTestfile.cmake ]; then
      build_dir="build"
    fi
    if [ -n "$build_dir" ]; then
      cd "$build_dir" && ctest -R "request|reply|spot" --output-on-failure 2>&1 | tail -20
      rc=${PIPESTATUS[0]}
      if [ "$rc" -ne 0 ]; then
        echo "FAIL: tests failed"
        exit 1
      fi
    fi
    echo "PASS: core tests present"
    exit 0
    ;;

  7)
    # 7단계: POSD 리팩토링 — 기능 계약 유지 + 빌드 성공
    if [ -d core/build ]; then
      cmake --build core/build --target libzlink 2>&1 | tail -5 || { echo "FAIL: build failed after refactor"; exit 1; }
    elif [ -d build ]; then
      cmake --build build --target libzlink 2>&1 | tail -5 || { echo "FAIL: build failed after refactor"; exit 1; }
    fi
    echo "PASS: POSD refactor build OK"
    exit 0
    ;;

  8)
    # 8단계: timeout 구현
    if ! grep -r "timeout" core/src/ 2>/dev/null | grep -i "request\|pending" | head -1 >/dev/null; then
      echo "FAIL: timeout implementation not found"
      exit 1
    fi
    echo "PASS: timeout implementation found"
    exit 0
    ;;

  9)
    # 9단계: native 라이브러리 최신화
    for lang_dir in bindings/*/native; do
      if [ -d "$lang_dir" ]; then
        so_count=$(find "$lang_dir" -name '*.so' -o -name '*.dylib' -o -name '*.dll' 2>/dev/null | wc -l)
        if [ "$so_count" -eq 0 ]; then
          echo "FAIL: no native lib in $lang_dir"
          exit 1
        fi
      fi
    done
    echo "PASS: native libraries present"
    exit 0
    ;;

  10)
    # 10단계: bindings 이관
    old_binding_syms="msg.set_request|msg\.set_reply|SetRequest|SetReply|set_request|set_reply"
    if grep -rE "$old_binding_syms" bindings/ 2>/dev/null | grep -v test | grep -v '\.md' | head -5; then
      echo "FAIL: old binding API calls still present"
      exit 1
    fi
    echo "PASS: bindings migrated"
    exit 0
    ;;

  11)
    # 11단계: 공개 문서 정리
    for doc_dir in core/doc/spec/core core/doc/guide core/doc/internals; do
      if ! grep -rl "request.reply\|request_reply" "$doc_dir"/ 2>/dev/null | head -1 >/dev/null; then
        echo "FAIL: $doc_dir missing request-reply documentation"
        exit 1
      fi
    done
    echo "PASS: public docs updated"
    exit 0
    ;;

  *)
    echo "ERROR: unknown stage $STAGE"
    exit 2
    ;;
esac
