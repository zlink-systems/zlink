# Go binding Core candidate 재확인

이 기록은 현재 Core worktree에서 만들어진 다른 workstream의 11.2.0 candidate를 Go 11.1.0 package의
승인 입력으로 재사용할 수 있는지 확인한 결과다. 이 candidate는 Go package 승인이나 V11-R2 독립 review를
의미하지 않는다.

## 비교한 입력

| 항목 | 기존 Go package 입력 | 현재 Core worktree candidate |
|------|----------------------|------------------------------|
| Candidate | `candidate-reply-match-completion-hwm-20260801.json` | `candidate-python-final-20260804.json` |
| Candidate file SHA-256 | `d318525a4cf8496b6bef5d900c9a88330ea6d7e10ed4120ac0fd9f19d23f6765` | `483df3ff20925fda60b3ac5a1c75e71e47c5eab871242623ee2c7fa66dd644bd` |
| Aggregate SHA-256 | `327587596195a162374498b630f51a043977dd392eb556061af615bf05186703` | `7f32732d7831728b7cac9cf7e8a691631797be7b89a931a3d6c87334114fc91` |
| Base revision | `73a9ce6d5bf275e9675333fc01e50948dbf895a2` | `1d724e5b3f2abbcde7b41a6143b6f6fbb947c588` |
| Core version | `11.1.0` | `11.2.0` |

기존 review evidence는 다음 candidate 하나만 승인한다.

```text
review: .artifacts/v11/evidence/V11-R2/core-candidate-reply-match-completion-hwm-review-20260801.json
reviewSha256: 171a9cc8f7203500de08050dcb74ecd36b4c9ce55a75a14ce1bee283705c9e04
approvedCandidateManifestSha256: d318525a4cf8496b6bef5d900c9a88330ea6d7e10ed4120ac0fd9f19d23f6765
independent: false
```

## 검증 명령과 결과

현재 candidate에 기존 review를 입력하면 다음과 같이 거부된다.

```bash
node scripts/local-package/core/verify-candidate.mjs \
  --candidate-manifest /home/hep7/project/kairos/zlink/.artifacts/v11/evidence/V11-M3-CORE-VERIFY/candidate-python-final-20260804.json \
  --review-evidence /home/hep7/project/kairos/zlink/.artifacts/v11/evidence/V11-R2/core-candidate-reply-match-completion-hwm-review-20260801.json \
  --repo-root /home/hep7/project/kairos/zlink
# review evidence does not approve the supplied candidate manifest SHA-256
# exit code: 1
```

반대로 기존 승인 candidate와 review를 현재 worktree에 입력하면 Core 변경이 candidate snapshot과 달라 다음과
같이 종료된다.

```bash
node scripts/local-package/core/verify-candidate.mjs \
  --candidate-manifest /home/hep7/project/kairos/zlink/.artifacts/v11/evidence/V11-M3-CORE-VERIFY/candidate-reply-match-completion-hwm-20260801.json \
  --review-evidence /home/hep7/project/kairos/zlink/.artifacts/v11/evidence/V11-R2/core-candidate-reply-match-completion-hwm-review-20260801.json \
  --repo-root /home/hep7/project/kairos/zlink
# candidate content drift: core/CMakeLists.txt
# exit code: 1
```

따라서 현재 Core 0.9.0.2.0 worktree candidate를 Go 11.1.0 승인 입력으로 승격하지 않는다. 반대로 기존 Go
package evidence도 현재 Core 0.9.0.2.0 변경의 승인으로 사용하지 않는다. Go package의 candidate identity는
기존 승인 Core 0.9.0.1.0 입력에 고정하며, 새 Core candidate를 사용하려면 별도 V11-R2 review, Core package
evidence와 Go package 재검증이 모두 필요하다.
