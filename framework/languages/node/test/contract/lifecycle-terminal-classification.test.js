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
