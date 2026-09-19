[English](./framework-cpp-0.18.1.md) | [한국어](./framework-cpp-0.18.1.ko.md)

# ZLink C++ Framework 0.18.1 Release Notes

Framework 0.18.1 uses binding 1.2.0 and Core 1.2.0. Each Framework language release is versioned independently.

## Contract Changes

The public API does not change.

## Common Changes

- The distributed zips (`zlink-tutorial-cpp.zip`, `zlink-samples-cpp.zip`) build and run without a repository checkout. `bootstrap.cmake` fetches and installs the three GitHub Release archives (Core prebuilt, C++ binding source, framework source), and the runners distinguish a repository tree from a distributed tree. Each zip root carries `README.ko.md` and `README.md` with Prerequisites, Download and install, Build, Run, Verify, and Troubleshooting sections. The `standalone-zips` CI guard runs those README command blocks verbatim in a job with no checkout. (#655, #669)
- Every Framework GitHub Release attaches eight zips (4 tutorial, 4 samples). Core and binding releases carry the same assets. (#639)
- The C++ tutorial gains the same Instance Spot queue (`MatchQueue`) as .NET. The client cold-activates the queue with its first request to `/match-queues/{mode}`. (#666)
- Sample runners no longer depend on Python. Port selection, role configuration, and JSON are bash; the ZoneWorld ZW-B8 proxy is a C++ program. (#673)
- The guide gains a read-along chapter (50–56) for each of the seven samples, and chapters 01 and 03 read their code from tutorial snippets. (#640, #641)

## Fixes

- A waiter now ends as `Disconnected` the moment the connection it observed ends. Previously a drop did not release waiters at all—only `close` did—so without a reconnect they hung until their own timeout. The six drop sites (heartbeat timeout, read pump, `dispatch_pending`, `receive_next`, `wait_for_packet`, `submit_wait_async`) call one function, `connection_ended` (spec 32 §10.1.1). (#667)
- When an Instance Spot handler called `close()` inside its own turn, that turn's normal reply was overwritten by `requestFailed` (terminal 105). The accepted turn now ends after its reply is handed to the transport rather than at handler return: close only blocks new admissions, and an accepted turn terminates once after its reply (spec §6.2). (#692)
- `bootstrap.cmake` did not select a generator when Ninja was absent and died with `CMAKE_MAKE_PROGRAM is not set`. It now selects Unix Makefiles and shallow-fetches the vcpkg baseline. (#655)
- Redis container id extraction always failed under the default Ubuntu awk (mawk); the README Windows blocks' stdout inheritance and PowerShell string issues are fixed too. (#655)
- Bingo, TicTacToe, and SupportChat samples now match the canonical contracts. The Bingo Session mesh uses an automatic RID with a role prefix instead of a fixed RID, and the Session disconnect callback neither iterates bound Actors nor notifies them. The TicTacToe Play StreamNode enables `enable_actor_dispatch()`. The SupportChat Entry Spot lowers availability on agent disconnect, allow-lists the ConversationId metadata, and pushes messages to every participant except the sender. (#658, #659, #660)

## Installation

Download `zlink-framework-cpp-0.18.1.tar.gz` from the [`framework-cpp/v0.18.1` GitHub Release](https://github.com/zlink-systems/zlink/releases/tag/framework-cpp%2Fv0.18.1) and use `find_package(zlink_framework CONFIG REQUIRED)`. The vcpkg overlay port and Conan recipe are also available.
