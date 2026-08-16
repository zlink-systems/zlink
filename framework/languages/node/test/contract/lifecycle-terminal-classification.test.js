'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');

const {
  ZLinkFrameworkErrorKind
} = require('../../packages/framework/dist/contracts/Errors/ZLinkFrameworkException');
const {
  wireReplyFailureException
} = require('../../packages/framework/dist/runtime/framework-errors-internal');
const {
  RequestResult
} = require('../../packages/framework/dist/runtime/backend/runtime-values');

//  Ownership-aware classification of lifecycle completion terminals
//  (Actor join, User Spot create/close). These paths formerly collapsed every
//  non-OK terminal to NotFound / InternalFailure; per spec
//  32-framework-error-model:81-118, 99-108 the (terminal, fine) pair determines
//  the public kind, a remote another-node queue is Unavailable, and only the
//  placement admission terminal Backpressured(113) is CapacityExceeded.

test('coarse lifecycle terminals classify by ownership (finding 3)', () => {
  const cases = [
    //  Remote operation-table saturation is another node's queue: Unavailable,
    //  NOT CapacityExceeded and NOT InternalFailure.
    [RequestResult.Busy, 0, ZLinkFrameworkErrorKind.Unavailable],
    [RequestResult.Conflict, 0, ZLinkFrameworkErrorKind.Unavailable],
    [RequestResult.NotConnected, 0, ZLinkFrameworkErrorKind.Unavailable],
    //  A missing handler surfaced as InvalidState is an invalid operation.
    [RequestResult.InvalidState, 0, ZLinkFrameworkErrorKind.InvalidOperation],
    //  Placement admission capacity is the target's admission decision.
    [RequestResult.Backpressured, 0, ZLinkFrameworkErrorKind.CapacityExceeded],
    [RequestResult.TimedOut, 0, ZLinkFrameworkErrorKind.DeadlineExceeded],
    [RequestResult.Terminated, 0, ZLinkFrameworkErrorKind.ShuttingDown],
    [RequestResult.NotFound, 0, ZLinkFrameworkErrorKind.NotFound]
  ];
  for (const [terminal, errno, expected] of cases) {
    const error = wireReplyFailureException(terminal, errno, 'lifecycle failure');
    assert.equal(
      error.kind,
      expected,
      `terminal ${terminal} errno ${errno} => ${expected}`
    );
  }
});

test('fine failure codes refine the coarse terminal (finding 1)', () => {
  //  failureErrno is authoritative when present; the terminal is ignored.
  const cases = [
    [18, ZLinkFrameworkErrorKind.Unavailable],       // workerQueueFull (remote queue)
    [19, ZLinkFrameworkErrorKind.DeadlineExceeded],  // workerTimedOut
    [20, ZLinkFrameworkErrorKind.InternalFailure],   // workerFailed
    [33, ZLinkFrameworkErrorKind.InvalidOperation],  // spotGenerationStale
    [34, ZLinkFrameworkErrorKind.Unavailable],       // spotMoving
    [35, ZLinkFrameworkErrorKind.DataLost],          // relocationDataLost
    [3, ZLinkFrameworkErrorKind.AlreadyExists],      // actorAlreadyExists
    [4, ZLinkFrameworkErrorKind.TypeMismatch],       // actorTypeMismatch
    [8, ZLinkFrameworkErrorKind.InvalidOperation]    // actorSessionNotBound
  ];
  for (const [errno, expected] of cases) {
    const error = wireReplyFailureException(105, errno, 'lifecycle failure');
    assert.equal(error.kind, expected, `errno ${errno} => ${expected}`);
  }
});

test('remote Actor create classifies by ownership, not InternalFailure (round-10 finding 2)', () => {
  //  actor-placement-coordinator now routes every non-OK remote create
  //  completion through wireReplyFailureException instead of collapsing to
  //  ActorCreateFailed/InternalFailure.
  const cases = [
    //  Target operation-table/queue saturation: another node's queue.
    [RequestResult.Busy, 0, ZLinkFrameworkErrorKind.Unavailable],
    //  Reply not received within the deadline.
    [RequestResult.TimedOut, 0, ZLinkFrameworkErrorKind.DeadlineExceeded],
    //  Target shutting down is not an internal failure.
    [RequestResult.Terminated, 0, ZLinkFrameworkErrorKind.ShuttingDown],
    //  Placement admission capacity keeps its round-9 classification.
    [RequestResult.Backpressured, 0, ZLinkFrameworkErrorKind.CapacityExceeded],
    //  A protocol-level fine code refines the terminal.
    [RequestResult.Conflict, 16, ZLinkFrameworkErrorKind.ProtocolError]
  ];
  for (const [terminal, errno, expected] of cases) {
    const error = wireReplyFailureException(terminal, errno, 'remote create failure');
    assert.equal(
      error.kind,
      expected,
      `create terminal ${terminal} errno ${errno} => ${expected}`
    );
  }
});

test('Actor destroy classifies the completion pair, not NotFound (round-10 finding 4)', () => {
  //  actors/index destroy now routes the (terminalResult, failureErrno) pair
  //  through wireReplyFailureException instead of collapsing every failure to
  //  ActorRouteNotFound; only genuine route/target-not-found causes stay
  //  NotFound.
  const cases = [
    [105, 18, ZLinkFrameworkErrorKind.Unavailable],       // workerQueueFull
    [105, 19, ZLinkFrameworkErrorKind.DeadlineExceeded],  // workerTimedOut
    [RequestResult.Terminated, 0, ZLinkFrameworkErrorKind.ShuttingDown],
    [105, 9, ZLinkFrameworkErrorKind.NotFound],           // routeHandlerNotFound
    [105, 14, ZLinkFrameworkErrorKind.NotFound],          // requestTargetNotFound
    [RequestResult.NotFound, 0, ZLinkFrameworkErrorKind.NotFound]
  ];
  for (const [terminal, errno, expected] of cases) {
    const error = wireReplyFailureException(terminal, errno, 'destroy failure');
    assert.equal(
      error.kind,
      expected,
      `destroy terminal ${terminal} errno ${errno} => ${expected}`
    );
  }
});
