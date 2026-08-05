[한국어](design-decisions.ko.md)

# Core design decisions

## Asynchronous I/O

Boost.Asio provides one completion-based I/O model across supported network
transports. Engines keep protocol parsing and transport operations behind the
session interface.

## Message ownership

Small payloads are stored inline and larger payloads use shared storage.
Explicit move, copy, and close operations make ownership visible at the C API
boundary without exposing allocator choices.

## Multipart atomicity

A multipart send sequence remains one logical queue operation. Socket-specific
send code delegates transaction handling to the common multipart path so
partial failure cleanup is not duplicated by callers.

## Typed socket surfaces

Pattern-specific metadata is returned through typed APIs. Routing ids, topics,
request sequences, and subscription state are not inserted into application
payload frames.

## Eventing separation

Pollers report readiness, monitors report transport/protocol transitions, and
generic timers report time events. These mechanisms do not interpret
application payloads.
