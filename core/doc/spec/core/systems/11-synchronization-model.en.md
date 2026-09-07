---
title: "Synchronization model"
---

[한국어](https://zlink-systems.github.io/zlink/ko/spec/core/systems/11-synchronization-model/) | English

<!-- zlink-nav:start -->
[Systems Index](README.en.md) | [Previous: Core Hot Path](10-hot-path.en.md)
<!-- zlink-nav:end -->

# Synchronization model

> **What this chapter defines** — the units in which Core owns mutable state, the devices that
> connect those units, and the conditions under which a lock may sit on the per-message path
> (hot path). The sentences here describe the implementation — the code is the fact, and this
> chapter follows the code. The exceptions are [§4](#4-when-a-lock-is-allowed) and
> [§5](#5-forbidden-forms), which are structural rules every Core change must follow; when the
> code differs there, the code is fixed.

## 1. Synchronization overview

Core's hot path is an application thread putting a message into a queue and an I/O thread
taking it out to send it, or the reverse. This chapter decides which thread owns what when
several threads touch that queue and the state around it, and which device — a lock, a
single-producer queue, an atomic value — is used at each point where ownership changes hands.

The contract callers rely on does not change. That one socket may be used by several
application threads at once is owned by [Socket common §2 Thread safety](../socket/README.en.md#2-thread-safety),
and which APIs are serialized is owned by [Thread safety](04-thread-safety.en.md). This chapter
decides **exactly which exclusion devices are needed to keep that contract, and why nothing
more is added**.

| Party | What this chapter decides for it |
|---|---|
| An application thread calling a socket | One public operation takes one socket turn and touches socket state inside it |
| The I/O thread serving a connection | Owns the session end of the pipe and the engine state alone, and speaks to the socket side only through commands and atomic values |
| A Core maintainer | The question that must be answered, and the measurements that must be submitted, when adding or removing a lock on the hot path |

This chapter writes down the rules for a consistent and effective synchronization
implementation. The model is the framework's
[state lane](../../../../framework/doc/framework/common/spec/server/01-execution/06-state-ownership-and-lanes.en.md);
the vocabulary follows that document, and since Core has no separate executor that runs a lane,
the socket turn plays that role.

## 2. State classification

Mutable state falls into one of three classes by what has to be kept true about it; the class
fixes the device that protects it, and no other device is substituted.

| Class | When it applies | So it is protected by | What it is in Core |
|---|---|---|---|
| **C1 — lookup registry** | One map; operations are lookup, add and remove only, with no condition to keep true together with other state | Changes go under one lock; hot-path lookups read a snapshot without locking | The route shard's RID→pipe table, the public poller's handle table, the context's socket registry |
| **C2 — cross-invariant state** | Several fields must change together to stay correct, or a decision is made and asynchronous behaviour follows from it | **One owning turn** runs serially, so fields inside the turn are not locked | All of the socket semantic layer's state — active pipe set, receive partition, lb/fq/dist state, multipart progress, deferred-termination queue |
| **C3 — atomic values** | Integer increments, monotonic maxima, flag checks, a single reference swap | release/acquire atomic operations | A pipe's `_state`, `_in_active`, `_out_active`, the bytes the peer has read, auto-HWM planned/applied, the head of the deferred-termination queue |

When one object mixes the three, **C2 wins** — the strongest requirement decides the device for
the whole object. Carving part of a C2 object out under its own lock or atomic breaks, at that
boundary, the very condition that had to hold across the fields. The reverse is not required:
C1 and C3 need not be pulled under a C2 turn; each is complete with its own device.

## 3. Ownership units

### 3.1 The socket turn

When a public operation starts it takes the socket's one execution right, and releases it when
it ends. This right is the **socket turn**. All of the socket's C2 state is owned by the turn, so
nothing in that state is locked inside it. The implementation is the `public_api_sync` bit of the
public-API entry word, taken with one compare-and-swap. Uncontended it costs one pair of atomic
operations; contended it follows the backoff rule in
[Thread safety §3](04-thread-safety.en.md#3-internal-rules).

Two parties take the turn.

| Party | When it takes the turn | So that |
|---|---|---|
| An application thread | While executing send, recv, request, reply, option, bind, connect or close | Other application threads wait on the same socket — this is how the thread-safe socket contract is realized |
| The command owner (an application thread, or the I/O thread acting as asynchronous executor) | While taking commands out of the socket's mailbox and applying them | Applying a command and running a public operation never touch the same state at the same time |

Both parties take it **always**. There is no exception by socket type or by command kind. An
exception would require showing, with the table in §6, that the command touches no C2 state;
"it runs often, so keep it lock-free" is not grounds for an exception — that assumption is
something to measure. Code holding the turn does not call the same socket's public API again.

### 3.2 The two ends of a pipe

A pipe carries messages between a socket and a session ([Architecture](01-architecture.en.md));
each of its two ends is pinned to one thread. The socket end is owned by the socket turn, the
session end by the connection's I/O thread
([Threading model §3](02-threading-model.en.md#3-cross-thread-communication)). The queue
between the ends (`ypipe_t`) has one producer and one consumer, so it has no lock. When one end
must wake the other, it sends a command (`activate_read`, `activate_write`).

So the default for a pipe end's state is "only the thread owning that end writes it", and only
values the other end has to read — for example the bytes the peer has consumed — are published as
C3 atomics. A lock inside a pipe exists only when it meets the condition in
[§4](#4-when-a-lock-is-allowed).

Internal check condition: the sleep/awake transition of `ypipe_t` and the conditions under which
`activate_read`/`activate_write` are sent realize the level rules of [Polling](../05-polling.en.md).
Changing them changes when POLLIN and POLLOUT become true, so a pipe-side change is submitted
together with proof that those conditions are the same before and after.

### 3.3 The mailbox

The mailbox, the channel that carries commands between threads, is multi-producer
single-consumer: many threads insert, one owning thread drains. Because there are many
producers, one lock at the insertion point is justified — the only "many producers" lock allowed
on the hot path. The draining side is one owning thread and takes no lock.

### 3.4 Context-level registries

Context-level state — the physical queue registry, the socket registry, the auto-HWM plan — is
C1 or C3. The hot path reads it without locking, through an atomic load or a value cached in the
handle. Only changes — registration, removal, a plan update — happen under a lock, and those
happen once per connection or plan event, not per message.

## 4. When a lock is allowed

A lock on the hot path must answer the following question; if the answer is "none", the lock is
removed.

> **Without this lock, is there actually a second thread on this path that writes or reads the
> same state at the same time?**

| Who the second thread is | So what is used |
|---|---|
| Another application thread | The socket turn. No additional lock |
| The other end of the pipe (I/O thread ↔ socket) | The single-producer queue and C3 atomics. No lock |
| Several producers inserting into one channel (mailbox) | One lock at the insertion point |
| A path that runs zero times per message (pipe detach, peer identification — teardown and identity) | The cold path takes the lock; the hot path is structured so it does not |

A change that brings a new lock onto the hot path is submitted together with the answer to this
question and the measurements in [§6](#6-verification-requirements).

## 5. Forbidden forms

| Form | Why it is forbidden | A real example, now removed |
|---|---|---|
| A lock that guards no condition | Even uncontended, every lock pair costs instructions and a cache-line transfer | `_out_sync` around `activate_read` handling; the context lock around an always-empty deferred-termination queue; the registry lock taken even when planned equalled applied |
| Splitting one condition across several locks | The condition breaks at the split (the C2 rule) | Socket serialization as three layers — `public_api_sync`, the command owner, and `receive.sync` per command |
| A turn exception by socket type or command kind | "It runs often" is something to measure, not a basis for a rule | PAIR commands applied without the turn |
| Reading a value written by the other pipe end under a lock | It wraps a single-producer structure in a lock again | The peer-consumed byte counts read under the session-side `_out_sync` |
| A device whose name and nature differ | A recursive mutex combined with `pthread_cond_wait` is undefined behaviour | `fast_mutex_t` was in fact a recursive mutex — split into plain `mutex_t` and `recursive_mutex_t`; debug builds catch re-entry immediately |
| The hot path carrying a lock for a cold path's sake | A path that runs zero times per message forces a per-message lock | `_out_sync` taken by the hot-path write for the benefit of pipe detach and peer identification |

Replacing a lock with a semaphore, a spin, or a `try_lock` retry does not change the form. Left in
the same place for the same reason, it meets the same prohibition.

## 6. Verification requirements

Every change that adds, removes or re-scopes a lock on the hot path submits the following. The
first four are internal check conditions; the last is observed on the public surface.

1. **A per-lock table** — acquisition site, acquisitions per message (callgrind), the condition it
   guards, the second thread, the action taken. The instruments are the reduced cell and the five
   `hotpath_gate` cells of [Core hot path §5](10-hot-path.en.md#5-performance-gates).
2. **Wake conditions unchanged** — the internal check condition of [§3.2](#32-the-two-ends-of-a-pipe):
   the code is identical before and after, or, if it differs, why the condition is the same.
3. **TSan** — the difference between the warning sets before and after is empty. A new data race
   is never waved through "for performance".
4. **Lost-wake tests** — the `test_wake_invariants` family with `--repeat until-fail:10` or more,
   20 when the wake path was touched, including the regression tests of the known lost-wake
   fixes.
5. **Accounting values unchanged** (publicly observed) — the charge values defined by
   [Auto-HWM](06-auto-hwm.en.md) and [Per-connection memory](05-connection-memory.en.md) stay the
   same. Observing them earlier is allowed; if a sentence pins the moment to a command boundary,
   the change first follows the contract-decision procedure.

## 7. Current inventory

Mutex acquisitions per message on the STREAM tcp 1024 B cell (CCU 20, callgrind). Locks outside
Core (inside the boost.asio reactor) are outside this chapter but listed for comparison. The table
is updated each time a lock change lands.

| Lock | Class | Second thread | 2026-09-07 | Target |
|---|---|---|---|---|
| mailbox insertion `_sync` | §3.3 many producers | many threads | 2.7 | 2.7 (structural) |
| socket serialization: `public_api_sync` + command owner + `receive.sync` per command | C2 → one turn | application thread and command owner | 1.47 | the turn's CAS only |
| `read_activated` / `has_in` receive partition | C2 | same cluster as above | 1.28 | 0 |
| session-side `pipe_t::write`/`flush` `_out_sync` | §3.2 SPSC + C3 | the I/O thread alone | 2.0 | 0 (peer-consumed bytes as atomics, `_out_active` by CAS) |
| socket-side `_out_sync` | C2 → turn | application thread | 1.0 | 0 (cold path keeps it) |
| route shard `sync` | C1 | lookup only on the hot path | 1.0 | 0 (snapshot lookup) |
| public poller handle table | C1 | lookup only on the hot path | 0.56 | 0 |
| boost.asio internals | outside Core | — | 4.0 | — |
| **Total** | | | **15.1** | **≈ 8.3 (Core-owned 10.1 → 3.3)** |

libzmq processes commands inside its one socket lock and has no mutex in the pipe, leaving only
the mailbox's 1.7. What keeps Core from the same number is the asio reactor's internal locking,
which belongs to the reactor choice in [I/O thread](03-io-thread.en.md).

## 8. Change procedure

- A subsystem adding state to the hot path classifies it first by [§2](#2-state-classification).
  C2 goes under the socket turn; state spanning both pipe ends is designed as C3 atomics. "Lock it
  for now and optimize later" is not done.
- A lock change is submitted through the procedure in
  [Core hot path §6](10-hot-path.en.md#6-change-procedure) plus the five items of
  [§6](#6-verification-requirements).
- This chapter alone cannot change behaviour a caller observes. Changing that first follows the
  contract procedures of [Socket common §2](../socket/README.en.md#2-thread-safety) and
  [Polling](../05-polling.en.md).

<!-- zlink-nav:start -->
[Systems Index](README.en.md) | [Previous: Core Hot Path](10-hot-path.en.md)
<!-- zlink-nav:end -->
