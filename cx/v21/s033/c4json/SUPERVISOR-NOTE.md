# Supervisor note 11-13 21:20
l3868-s1 finished: L5~L7 (production route path) is FASTER than the L4 bench typed JSON step on 4K (send 4K L4 16,262 -> L5~L7 30,381; bp 4K 8,456 -> 12,611).
Check and write in report.md: does the L4 bench (B/ladder/ladder_payload.hpp, ladder_wire.hpp) call the same production serializer the route path uses (R/channels/route_handler_registry.hpp:147 serialize, route_packet_dispatcher.cpp:444)? If L4 calls a slower/different entry point, say which file:line, and profile the route-path serializer too, because that is what production users run.
