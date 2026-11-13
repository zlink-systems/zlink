# Issue #3867 / PR #3884 C++ native service-wire tests

## Cases added and expected-byte source

All accepted cases compare native `*_message` bytes and vector encoder bytes to test-authored expected bytes. Expected bytes are assembled in this test with the existing primitive writers (`prefix`, `put_u16`/`put_u32`/`put_u64`, `put_text8`, and `put_bytes8`); the expected values do not come from a vector encoder.

- Channel request and send header channel names: 255 accepted, 256 rejected. The test builds each expected header from the wire prefix, correlation where applicable, and text8 bytes.
- Spot request and send headers: source SpotId and target SpotId each at 255 accepted and 256 rejected. Expected headers are built field-by-field, including request correlation and route fence.
- Actor request and send headers: source Actor ID and target Actor ID each at 255 accepted and 256 rejected. Expected headers include the optional source generation and route fence.
- Spot and Actor request/send target-node RID: 255 accepted, 256 rejected. Expected headers are built field-by-field with test-authored RID bytes.
- Actor bound-session source Session RID: limit accepted and limit+1 rejected. `ridBytes` is 255 in the generated service-wire constants, so these are 255 accepted and 256 rejected. Expected bytes include the bound-session flags, RID and binding fields.
- Bound-session send Actor owner-node RID: 255 accepted and 256 rejected. Expected command bytes are built field-by-field.
- Send metadata combinations: no metadata (header, application), present-empty metadata (header, empty part, application), and raw metadata parts of lengths 1, 255, 256 and 4096. The expected header and application parts are independent byte arrays; metadata bytes are test-created and compared byte-for-byte, and the native part list is checked for exact order.
- Flow-version-2 application envelope: independent expected bytes for a small inline payload and a two-part multipart payload, each with flow ID and application origin. Native and vector encoders are compared against both goldens.
- Every tested over-length native encoder call is required to throw `service_wire_error_t`; the test retains no result from the rejected call, so no partial message is returned.

`send_with_header_result` has no existing unit-test seam in this file. The metadata cases therefore mirror its header / optional metadata / application native-part assembly using the existing binding-part materializer, while independently checking the part encoders and ordering. Direct execution of `send_with_header_result` remains untested without a production change.

## Verification

- Codec target: `ctest --test-dir /root/jobs/fx/cx/v21/s033/salloc/build -R '^test_cpp_framework_service_wire_codec$' --output-on-failure` — passed (1/1).
- C++ unit and contract labels: `/root/jobs/fx/cx/measure-shared.sh ctest --test-dir /root/jobs/fx/cx/v21/s033/salloc/build -L 'framework-unit|framework-contract' --parallel 3 --output-on-failure` — passed (287/287).
- Format: `bash scripts/format/format.sh --check cpp` — passed.
- Build: codec test target built through `/root/jobs/fx/cx/cpp-build-slot.sh` with `--parallel 3`; MemAvailable was 26.4 GiB before compilation.

## Commit

`927adc2f9014cbcd9e45f49a82e786ac2183ab0d` (`test(cpp): cover native service wire boundaries`)

## Not finished

- Direct `send_with_header_result` execution coverage is unavailable through current test seams; the part-assembly fallback above is covered.
