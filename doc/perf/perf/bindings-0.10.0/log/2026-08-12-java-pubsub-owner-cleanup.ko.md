+# Java PUBSUB owner cleanup 측정
+
+Release Core `0.10.1`, TCP `MULTI_PUBSUB`, clients `100`, duration `1초`, runs `1`, balanced auto-HWM에서 C와 Java를 순서대로 측정했다.
+
+`TopicMessage`가 소유한 receive wrapper를 일반 close로 폐기하던 경로를 owner cleanup으로 변경해 기존 `MessageWrapperPool`에 반환하도록 했다. public API, multipart, DONTWAIT, message ownership은 변경하지 않았다. Java 전체 test가 통과했다.
+
+| Size (B) | C msg/s | Java msg/s | C 대비 |
+|---:|---:|---:|---:|
+| 64 | 1,520,771 | 660,401 | 43.42% |
+| 256 | 1,480,423 | 635,082 | 42.90% |
+| 1024 | 1,269,058 | 502,218 | 39.57% |
+| 4096 | 514,803 | 203,451 | 39.52% |
+| 65536 | 95,600 | 58,343 | 61.03% |
+| 131072 | 45,514 | 37,433 | 82.25% |
+
+산술평균은 `51.45%`다. C report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260812_090256_java-held2-pub-tcp-c.txt`; Java report: `bindings/java/perf/results/multi/report/perf_java_multi_linux_20260812_090309_java-held2-pub-tcp-topic-owner.txt`.
