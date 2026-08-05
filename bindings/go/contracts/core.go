// SPDX-License-Identifier: MPL-2.0

// Package contracts is the public Go binding projection.
package contracts

import impl "zlink.systems/zlink/v11/internal/native"

type (
	// Version is the native zlink library version (major, minor, patch).
	Version = impl.Version
	// Context is a messaging context: the factory and owner of sockets.
	// Closing the context terminates any sockets still open under it.
	Context = impl.Context
	// ContextOptions are context-wide options governing the I/O threads and defaults shared by every socket.
	ContextOptions = impl.ContextOptions
	// AutoHwmProfile selects an automatic high-water-mark sizing profile.
	AutoHwmProfile = impl.AutoHwmProfile
	// AutoHwmRecalcReason reports what triggered the last automatic high-water-mark recalculation.
	AutoHwmRecalcReason = impl.AutoHwmRecalcReason
	// SocketTarget identifies a socket that can participate in poll and proxy operations.
	SocketTarget = impl.SocketTarget
	// Stopwatch is a high-resolution elapsed-time stopwatch.
	Stopwatch = impl.Stopwatch
	// AtomicCounter is a thread-safe integer counter.
	AtomicCounter = impl.AtomicCounter
	// Thread is a running background thread created by the zlink runtime.
	Thread = impl.Thread
)

const (
	// AutoHwmProfileCompact uses the smallest queues to minimize memory use.
	AutoHwmProfileCompact = impl.AutoHwmProfileCompact
	// AutoHwmProfileLowLatency uses small queues that drain quickly to favor latency.
	AutoHwmProfileLowLatency = impl.AutoHwmProfileLowLatency
	// AutoHwmProfileBalanced balances latency against throughput.
	AutoHwmProfileBalanced = impl.AutoHwmProfileBalanced
	// AutoHwmProfileThroughput uses large queues to maximize throughput.
	AutoHwmProfileThroughput = impl.AutoHwmProfileThroughput
	// AutoHwmRecalcReasonNone indicates no recalculation has occurred yet.
	AutoHwmRecalcReasonNone = impl.AutoHwmRecalcReasonNone
	// AutoHwmRecalcReasonInitial is set on the first recalculation after context creation.
	AutoHwmRecalcReasonInitial = impl.AutoHwmRecalcReasonInitial
	// AutoHwmRecalcReasonRoleChange is set when a socket's topology role changes.
	AutoHwmRecalcReasonRoleChange = impl.AutoHwmRecalcReasonRoleChange
	// AutoHwmRecalcReasonPolicyToggle is set when auto-HWM is enabled or disabled.
	AutoHwmRecalcReasonPolicyToggle = impl.AutoHwmRecalcReasonPolicyToggle
	// AutoHwmRecalcReasonRefresh is set when an explicit recalculation is requested.
	AutoHwmRecalcReasonRefresh = impl.AutoHwmRecalcReasonRefresh
	// AutoHwmRecalcReasonDeferredShrink is set when a scheduled deferred queue shrink fires.
	AutoHwmRecalcReasonDeferredShrink = impl.AutoHwmRecalcReasonDeferredShrink
)

var (
	// RuntimeVersion returns the native zlink library version.
	RuntimeVersion = impl.RuntimeVersion
	// NewContext creates a messaging context; the caller owns it and must close it.
	NewContext = impl.NewContext
	// Has reports whether the native library was built with the named capability.
	Has = impl.Has
	// Proxy forwards messages between two sockets, blocking until the context is terminated.
	Proxy = impl.Proxy
	// ProxySteerable forwards messages between two sockets under runtime control, blocking until terminated.
	ProxySteerable = impl.ProxySteerable
	// Sleep blocks the calling goroutine for the given duration.
	Sleep = impl.Sleep
	// MultipartClose closes every message in a multipart payload.
	MultipartClose = impl.MultipartClose
	// NewStopwatch creates a high-resolution stopwatch; the caller owns it.
	NewStopwatch = impl.NewStopwatch
	// NewAtomicCounter creates a thread-safe integer counter; the caller owns it.
	NewAtomicCounter = impl.NewAtomicCounter
	// NewThread starts target on a zlink-owned native thread; the caller owns the handle.
	NewThread = impl.NewThread
)
