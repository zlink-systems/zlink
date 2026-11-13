# Supervisor note 11-13 21:20
l3868-s1 finished: L5~L7 (production route path) is FASTER than the L4 bench typed JSON step on 4K (send 4K L4 16,262 -> L5~L7 30,381; bp 4K 8,456 -> 12,611).
Check and write in report.md: does the L4 bench (B/ladder/ladder_payload.hpp, ladder_wire.hpp) call the same production serializer the route path uses (R/channels/route_handler_registry.hpp:147 serialize, route_packet_dispatcher.cpp:444)? If L4 calls a slower/different entry point, say which file:line, and profile the route-path serializer too, because that is what production users run.

# PM 11-13 21:35 — .NET is the reference implementation
.NET keeps the same contract (default JSON, padded base64 byte sequences, same bench DTO) with L4 loss 10-41% and L4÷binding 0.56-0.78 (lad-dotnet-s1). "Remaining cost is required by the contract" is not acceptable while .NET does the same contract cheaper.
In report.md add a table: for each cost on your language's typed JSON path (encode, base64, decode, copies, allocations), the .NET equivalent file:line under framework/languages/dotnet/src/Zlink.Framework/ (e.g. System.Text.Json Utf8JsonWriter/Reader writing base64 straight into a pooled buffer) and what .NET does less. Fold that difference into the fix proposal.
