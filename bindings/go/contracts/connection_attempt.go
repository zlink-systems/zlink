// SPDX-License-Identifier: MPL-2.0

package contracts

// ConnectionAttemptState is the closed Core state of one outbound connection registration.
type ConnectionAttemptState uint32

const (
	ConnectionConnecting    ConnectionAttemptState = 0
	ConnectionConnected     ConnectionAttemptState = 1
	ConnectionAttemptFailed ConnectionAttemptState = 2
)

// ConnectionFailureKind is the closed Core cause of the latest connection failure.
type ConnectionFailureKind uint32

const (
	ConnectionFailureNone            ConnectionFailureKind = 0
	ConnectionFailureRefused         ConnectionFailureKind = 1
	ConnectionFailureEndpointMissing ConnectionFailureKind = 2
	ConnectionFailureTimedOut        ConnectionFailureKind = 3
	ConnectionFailureUnreachable     ConnectionFailureKind = 4
	ConnectionFailureNameResolution  ConnectionFailureKind = 5
	ConnectionFailureAuthFailed      ConnectionFailureKind = 6
	ConnectionFailureProtocolFailed  ConnectionFailureKind = 7
	ConnectionFailureConnectionLost  ConnectionFailureKind = 8
	ConnectionFailureOther           ConnectionFailureKind = 9
)

// ConnectionFailureStage is the closed Core stage at which the latest failure happened.
type ConnectionFailureStage uint32

const (
	ConnectionStageNone          ConnectionFailureStage = 0
	ConnectionStageResolve       ConnectionFailureStage = 1
	ConnectionStageTransport     ConnectionFailureStage = 2
	ConnectionStageTLS           ConnectionFailureStage = 3
	ConnectionStageWebSocket     ConnectionFailureStage = 4
	ConnectionStageCoreHandshake ConnectionFailureStage = 5
	ConnectionStageEstablished   ConnectionFailureStage = 6
)

// ConnectionFailure is Core's typed failure. NormalizedErrno and
// DisconnectReason are diagnostic values.
type ConnectionFailure struct {
	Kind             ConnectionFailureKind
	Stage            ConnectionFailureStage
	NormalizedErrno  int32
	DisconnectReason uint32
}

// ConnectionAttempt is one current outbound registration. IntentID and
// Revision are nonzero opaque tokens; LastFailureID is 0 without a failure
// history. Compare tokens only for equality.
type ConnectionAttempt struct {
	Endpoint      string
	IntentID      uint64
	Revision      uint64
	LastFailureID uint64
	State         ConnectionAttemptState
	LastFailure   ConnectionFailure
}
