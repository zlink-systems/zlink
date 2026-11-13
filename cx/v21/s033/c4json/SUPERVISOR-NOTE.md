# Supervisor note 11-13 21:20
l3868-s1 finished: L5~L7 (production route path) is FASTER than the L4 bench typed JSON step on 4K (send 4K L4 16,262 -> L5~L7 30,381; bp 4K 8,456 -> 12,611).
Check and write in report.md: does the L4 bench (B/ladder/ladder_payload.hpp, ladder_wire.hpp) call the same production serializer the route path uses (R/channels/route_handler_registry.hpp:147 serialize, route_packet_dispatcher.cpp:444)? If L4 calls a slower/different entry point, say which file:line, and profile the route-path serializer too, because that is what production users run.

# PM 11-13 21:35 — .NET is the reference implementation
.NET keeps the same contract (default JSON, padded base64 byte sequences, same bench DTO) with L4 loss 10-41% and L4÷binding 0.56-0.78 (lad-dotnet-s1). "Remaining cost is required by the contract" is not acceptable while .NET does the same contract cheaper.
In report.md add a table: for each cost on your language's typed JSON path (encode, base64, decode, copies, allocations), the .NET equivalent file:line under framework/languages/dotnet/src/Zlink.Framework/ (e.g. System.Text.Json Utf8JsonWriter/Reader writing base64 straight into a pooled buffer) and what .NET does less. Fold that difference into the fix proposal.

# Supervisor 11-13 22:35 — serializer identity checked, inversion still open
Code check: L4 (ladder_payload.hpp:38-42, default serializer_registry_t for typed_payload_t) and L5~L7 (bench_framework_cpp_server.cpp:98-100 registers the same typed_payload_t on the route) select the same default JSON serializer. So L5~L7 being faster than L4 on 4K is not a serializer difference. In report.md, explain it from your L4 vs L5~L7 profiles: extra copies/allocations in the L4 bench wire path (ladder_wire.hpp, ladder_driver.inc) vs the route path, or different in-flight/concurrency (results.json peak_in_flight, streams). If the L4 bench adds cost that production does not, say so with file:line — then the L4 step definition must be corrected before its loss is charged to the serializer.
Also add the .NET comparison table per cost (PM 21:35 note above).
