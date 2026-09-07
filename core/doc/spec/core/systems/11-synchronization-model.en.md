---
title: "Synchronization model"
---

[한국어](https://zlink-systems.github.io/zlink/ko/spec/core/systems/11-synchronization-model/) | English

<!-- zlink-nav:start -->
[Systems Index](README.en.md) | [Core Spec Index](../README.en.md) | [Previous: Core Hot Path](10-hot-path.en.md)
<!-- zlink-nav:end -->

# Synchronization model

> **What this chapter defines** — the synchronization rules the Core implementation follows:
> which state has to be kept consistent together, who changes it and when, which device joins
> the owners, and the conditions under which a lock may sit on the per-message path (hot path).
> Where the code differs from these rules, the code is fixed. Where the code still differs today
> and in what order it is brought into line is not recorded here — progress is owned by the plan
> documents.

## 1. Synchronization overview

Core's hot path is an application thread calling a socket putting a message into a queue, and
the I/O thread serving the connection taking it out to send it, or the reverse. The same state is
also touched when two sockets face each other over one pipe with no I/O thread, as with inproc,
and by context observers reading a connection's memory state. This chapter decides who owns what
when several execution parties touch that state, which device — a lock, a single-producer queue,
an atomic value — is used at each point where ownership changes hands, and how that device is
handled.

The contract callers rely on does not change. The permitted range of concurrent calls and the
result of close are owned by [Socket common §2 Thread safety](../socket/README.en.md#2-thread-safety)
and the per-function contracts; readiness and waiter progress by [Polling](../05-polling.en.md);
memory accounting by [Auto-HWM](06-auto-hwm.en.md). [Thread safety](04-thread-safety.en.md)
describes the current implementation; this chapter is the protection rule that implementation
must follow. It decides **exactly which exclusion devices are needed to keep those contracts,
and why nothing more is added**.

The right that lets exactly one execution party change a socket's state at a time is the
**socket turn**. Every rule here follows one principle: state that shares an invariant has one
owner. The model shares that single-ownership principle with the framework's
[state lane](../../../../../framework/doc/framework/common/spec/server/01-execution/06-state-ownership-and-lanes.en.md),
but the two are not the same device: the framework's lane is an execution unit that provides a
FIFO queue and immediate rejection of re-entry, while Core's turn provides exclusion only.
Scheduling which thread runs work belongs to the [I/O thread](03-io-thread.en.md) and the
mailbox's asynchronous owner; the turn does not replace it.

| Party | What this chapter decides for it |
|---|---|
| An application thread calling a socket | Holds the socket turn while a permitted send/receive/control operation changes socket state |
| The I/O thread serving a connection | Owns the connection's engine state and the session end of the pipe, and changes socket-side state only through commands and published atomic values |
| Context observers (memory snapshots, HWM re-planning) | Read published values, and take a registry lock only when a consistent query needs one |
| A Core maintainer | When adding, removing or re-scoping a lock, states the invariant it protects and the parties that access the state together, and submits the verification in [§8](#8-verification-requirements) |

## 2. What must hold together, and the state classes

Mutable state is classified by "what must be kept true together", and the class fixes the
device. The unit of classification is not a physical object but **the range of one invariant
that must hold together**; an object holding several such ranges is classified range by range.

| What must hold together | Class | Device to apply | Examples in Core |
|---|---|---|---|
| One table with lookup, add and remove only, and no condition shared with other state | **C1 — lookup table** | Changes under one lock; hot-path lookups read a lock-free snapshot, with the lifetime of what the snapshot points at guaranteed by a held reference or a generation | The route shard's RID→pipe table, the public poller's handle table |
| Several fields must change together, or a decision is made and asynchronous behaviour follows from it | **C2 — cross-invariant** | One execution owner runs serially, so the fields are not locked inside the owner | The state a socket uses to know which pipes can be received from and which comes next, the state that picks the pipe to send on, multipart progress, the deferred-termination queue's links; a pipe send end's queue, active flag and generation as a bundle; the context socket registry's socket set, slots and free-slot list |
| One value that may be observed independently of other state — an increment, a monotonic maximum, a flag check, one reference swap | **C3 — published value** | Atomic operations; a value read by another thread is published with release and read with acquire | A pipe's `_state` and `_in_active`, the messages and bytes the peer has consumed, auto-HWM planned/applied, the deferred-termination queue's head used only to ask "is it empty" |

Fields that share one invariant have one owner. Carving some of them out under a separate lock
or atomic breaks the condition at the cut — changing a field's type to atomic does not make it C3.
The reverse is not required: C1 and C3 need not be pulled under a C2 owner. The deferred
termination queue is the pattern: only "is it empty" is observed through an atomic head, and the
links are changed under the owner.

## 3. Owners and access rules

### 3.1 The socket turn

**What it owns.** All of the socket's C2 state. Nothing in that state is locked inside the turn.

**Who holds it, and when.**

| Party | When | So that |
|---|---|---|
| An application thread | While a permitted send, recv, request, reply or control operation changes socket state | Another application thread's operation of the same kind waits on that socket — this is how the contract that allows concurrent calls is realized |
| The command owner (an application thread, or the I/O thread acting as asynchronous executor) | While taking a batch of commands out of the socket's mailbox and applying them | Applying a command and running a public operation never touch the same state at the same time |

Commands are applied inside the turn regardless of socket type or command kind. A thread that
already holds the turn applies commands inside that same turn without taking it again. There is
no exception of the form "it runs often, so leave it unlocked".

close is not an operation that waits for the turn. close is a lifecycle gate that refuses while
an API call is in progress; its result (`EBUSY`) and ordering are owned by
[Socket common §2](../socket/README.en.md#2-thread-safety). Nor does the turn replace receive's
single-consumer constraint or the multipart-owner constraint — those follow their own contracts.

**How it is taken and released.** The turn is one bit of the public-API entry word. The word
also carries the in-flight call count and the close state, so taking and releasing the turn are
**read-modify-write operations that preserve the other bits**. Releasing with a plain store would
overwrite admission or close state changed meanwhile by another thread. When taking fails, the
thread does not read the socket's C2 state; it re-reads the entry word only, backs off briefly and
retries.

**What is not done inside the turn.** The same socket's public API is not called again, another
socket's turn is not waited for, and no wait is started. When a wait is needed, the turn is
released as in §3.4.

Internal check condition: the code that reads or writes the socket's C2 state is exactly one
execution party, the one holding the turn.

### 3.2 The two ends of a pipe and what lies between

A pipe carries messages between a socket and a session ([Architecture](01-architecture.en.md));
each end has **exactly one concurrent execution party**. The socket end is run by whoever holds
the socket turn at that moment; the session end by the connection's I/O thread
([Threading model §3](02-threading-model.en.md#3-cross-thread-communication)). The application
thread running the socket end may differ from call to call — what is fixed is "one at a time",
not the thread. For an inproc pipe the far end is the other socket's turn instead of a session.

**The queue.** The queue between the ends has one producer and one consumer. No separate mutex is
added for producer/consumer state. In flush the producer decides with one atomic exchange whether
the consumer had gone to sleep, and sends an `activate_read` command only then; when the queue runs
empty the consumer marks itself asleep with the same exchange.

**Values owned by one end.** That end's queue position, its written message and byte counts, its
active flag and its generation are changed only by that end's execution party. A value the other
end or a third party (monitor, context observer) must read is published as C3 — release by the
writer, acquire by the reader. An unpublished value may be read by another party only under the
same lock as its owner. **A reader keeping its lock does not allow the writer to drop its lock** —
to drop the writer's lock, the value is published or the reader is removed.

**Values shared by both ends.** The peer lifetime link and the head-reclassification marker are
written by both ends, so they are not values one end owns. They are changed under their own lock
or with a CAS that confirms a transition, and that lock is taken only on paths that run zero times
per message (pipe detach, peer identification, termination). The hot-path write, read and flush
must not take it.

**Credit.** How much may be sent, the boundaries between candidate, committed and returned
charge, and the condition that wakes a waiter (LWM) are owned by [Auto-HWM](06-auto-hwm.en.md)
and [Socket common](../socket/README.en.md). This chapter fixes only the access rule: each end
writes its own counter and reads the other's through the published value, and neither end locks
the other's counter.

Internal check condition: every value at a pipe end has one writer, and if another party reads
it, it is published.

### 3.3 The mailbox and wake-ups

The mailbox, the channel that carries commands between threads, is inserted into by many threads
and drained by one owner at a time. The owner of a socket mailbox alternates between the thread
holding the public API and the asynchronous executor; that hand-over is defined by
[I/O thread](03-io-thread.en.md).

**Insertion.** Because there are many producers, there is one lock at the insertion point. It
protects command publication together with its notification state — the asleep decision, waiter
registration, poller notification. It is the only lock allowed on the hot path on the grounds of
"many producers".

**Wake-up.** A consumer is woken for more than one reason: when the queue transitions from asleep
to awake, when an applied command changes public readiness so a poller must be woken again, and
when state is changed without a command and signalled explicitly. The rule is the same for each —
**notify once at the transition that creates the reason, and do not notify a consumer that is
already awake.** The consumer goes to sleep only after draining the mailbox, and checks the queue
once more just before sleeping so that a command inserted in between is not missed. When a public
poller and the command owner share one signal, which of them consumes it first and which re-arms
it is owned by [Polling](../05-polling.en.md).

**Draining.** Taking an item out of the queue itself takes no lock. A command that changes a
socket's C2 state is applied inside the socket turn of [§3.1](#31-the-socket-turn); a command that
changes a connection's engine state is applied by that connection's I/O thread
([I/O thread](03-io-thread.en.md)). Each mailbox's commands are applied by the owner of the
destination state.

Internal check condition: no wake-up is lost between waiter registration and notification — the
queue is re-checked after registering and before sleeping, and the notification is issued after
the registration has been seen.

### 3.4 Waiting and re-taking

When a public operation must wait for admission or for a message, the order is:

```text
application thread                          command owner / I/O thread
  take turn → check condition → fails
  register waiter (inside the turn)
  release turn
  wait on signaler/CV  ◄──── notify ──────  deliver message/credit → waiter present → notify
  re-take turn → re-check condition → proceed, or wait again
```

- The turn is not held while waiting. If the party that would end the wait needs that turn, the
  wait never ends.
- A condition-variable wait holds its mutex on entry, releases it while waiting and re-takes it on
  return; that form is allowed. What is forbidden is not releasing it while waiting.
- A wait with a timeout re-takes the turn on expiry, re-checks the condition once more and then
  decides the result.

Internal check condition: waiter registration happens inside the turn; the wait happens outside
it.

### 3.5 Context-level state

The physical queue registry, the socket registry and the auto-HWM plan are context-level state.
The cost rule that the normal message path takes no context-wide mutex is owned by
[Auto-HWM](06-auto-hwm.en.md). This chapter fixes the following.

- Hot-path accounting uses the handle cached in the connection and published values.
- Registration, removal and consistent queries (memory snapshots, re-planning) run under the
  registry lock. When a query reads a pipe end's value it follows §3.2 — it reads a published value
  or takes the same lock as the owner.
- Deferred HWM application returns without taking the lock when the planned and applied values
  are equal.

## 4. When a lock is allowed

A lock on the hot path must answer the following question; if the answer is "none", the lock is
removed.

> **Without this lock, is there actually a second execution party on this path that writes or
> reads the same state at the same time? If so, who is it, and what must be kept true together?**

| Concurrent party and condition | State to keep together | Device to apply |
|---|---|---|
| Another application thread changes the same socket's C2 state | All socket state | The socket turn. No additional lock |
| The other pipe end writes the same value | A lifetime or marker value shared by both ends | That value's lock or a CAS transition, taken on the cold path only |
| The other pipe end, a monitor or a context observer **only reads** the value | That one value | C3 publication (release/acquire). No lock |
| Several threads insert into the same channel | Insertion order and notification state | One lock at the insertion point |
| Two or more readers and no writer | Nothing | No device |

A change that brings a new lock onto the hot path is submitted with the answer to this question
and the verification in [§8](#8-verification-requirements).

## 5. Violations and the rule to follow

| Violating condition | Resulting problem | Rule to follow |
|---|---|---|
| A lock on the hot path guards no condition | Even uncontended, acquire/release instructions and the word update are paid per message | [§4](#4-when-a-lock-is-allowed): no answer, no lock |
| State sharing one invariant is split across several locks | The condition breaks at the split | [§2](#2-what-must-hold-together-and-the-state-classes): one invariant, one owner |
| A command is applied outside the turn because of socket type or frequency | A public operation and the command touch the same state at once | [§3.1](#31-the-socket-turn) |
| The writer's lock is dropped while only the reader keeps its lock | Reader and writer are no longer synchronized | [§3.2](#32-the-two-ends-of-a-pipe-and-what-lies-between): publish the value or remove the reader |
| A thread waits while holding a turn or lock the waker needs | The wait never ends | [§3.4](#34-waiting-and-re-taking) |
| A re-entrant lock is used with a condition variable | With a re-entry depth above one the wait does not actually release the mutex, and progress stops | [§6](#6-lock-kinds-ordering-and-memory-ordering): only non-reentrant locks with CVs |
| The hot path takes a lock for the sake of a zero-per-message path | The cold path forces a per-message cost | [§3.2](#32-the-two-ends-of-a-pipe-and-what-lies-between): the cold path alone takes it |

Replacing a lock with a semaphore or a `try_lock` retry does not change the form; left in the same
place for the same reason, it meets the same rule. The socket turn's own short retry is not
subject to this rule — it is the device that establishes the single owner, not an additional lock
guarding state.

## 6. Lock kinds, ordering and memory ordering

- **Kinds.** The default is the non-reentrant `mutex_t`. `recursive_mutex_t` is used only where
  the same thread must re-take the same lock, and that re-entrant path is written in a comment at
  the declaration. The only lock used with a condition variable is `mutex_t`. On the POSIX backend,
  debug and sanitizer builds create `mutex_t` in the mode that catches re-entry immediately
  (`ERRORCHECK`); other backends do not provide that detection, so the no-re-entry rule is kept
  there by review and by §8.
- **Order.** As a design rule, locks are taken only in the order socket turn → the pipe ends that
  socket owns → context registries. Never the reverse, and never another socket's turn or pipe end
  while holding one socket's turn; a change affecting the other socket is sent as a command. When a
  registry queries a pipe end's value it either releases the registry lock first or reads only
  published values.
- **Memory ordering.** A value published to another thread uses a release store and is read with
  an acquire load. Where neither of two updates may be missed — waiter registration and
  notification — a `seq_cst` fence is placed on both sides. Relaxed is used only for values whose
  order has no meaning, such as statistics.

## 7. Change procedure

- A subsystem adding state to the hot path classifies it first by
  [§2](#2-what-must-hold-together-and-the-state-classes). C2 goes under the socket turn; a value
  another party must read is designed as a C3 publication. "Lock it for now and optimize later" is
  not done.
- A change that adds, removes or re-scopes a lock states the invariant it protects, the parties
  that access the state together and the device applied, and is submitted through the procedure in
  [Core hot path §6](10-hot-path.en.md#6-change-procedure) plus the verification in
  [§8](#8-verification-requirements).
- This chapter alone cannot change behaviour a caller observes. Changing that first follows the
  contract procedures of [Socket common §2](../socket/README.en.md#2-thread-safety),
  [Polling](../05-polling.en.md) and [Auto-HWM](06-auto-hwm.en.md).

## 8. Verification requirements

A change that adds, removes or re-scopes a lock keeps the following observable results. The
internal check conditions live next to their rules and are not repeated here.

1. **Readiness and waiter progress** — the level results and lost-wake prevention defined by
   [Polling](../05-polling.en.md) are unchanged. The wake-path regression tests (the
   `test_wake_invariants` family) run repeatedly without failure; the repeat count and the list
   are set by the gate procedure in [Core hot path §5](10-hot-path.en.md#5-performance-gates).
2. **Accounting values** — the charge values and return boundaries defined by
   [Auto-HWM](06-auto-hwm.en.md) are unchanged. A change to when they are observed first follows
   that document's contract procedure.
3. **Data races** — the set of TSan warnings is the same before and after. A new warning is not
   waved through for performance.
4. **Instruction counts** — the `hotpath_gate` cells of
   [Core hot path §5](10-hot-path.en.md#5-performance-gates) stay within their reference, and the
   per-lock acquisition table is submitted.

<!-- zlink-nav:start -->
[Systems Index](README.en.md) | [Core Spec Index](../README.en.md) | [Previous: Core Hot Path](10-hot-path.en.md)
<!-- zlink-nav:end -->
