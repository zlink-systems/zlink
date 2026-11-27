// SPDX-License-Identifier: MPL-2.0

// Package contracts owns the Go binding value contract.
package contracts

type Version struct {
	Major int
	Minor int
	Patch int
}

// CoreHwmBudgetSnapshot is an immutable value copy of Core's ABI-v1 budget snapshot.
type CoreHwmBudgetSnapshot struct {
	ABIVersion, StructSize                                               uint32
	BudgetGeneration, MeasurementEpoch                                   uint64
	ConfiguredMemoryLimitBytes, ResolvedMemoryLimitBytes                 uint64
	ConfiguredCoreBudgetBytes, EffectiveCoreBudgetBytes                  uint64
	TotalPlannedHwmBytes, TotalAppliedHwmBytes, ManualReservedHwmBytes   uint64
	CoreQueueAccountedBytes, ApplicationAccountedBytes                   uint64
	CurrentAccountedBytes, ProvisionalAccountedBytes, PeakAccountedBytes uint64
	CompletionCurrentAccountedBytes, CompletionPeakAccountedBytes        uint64
	CompletionPendingMessageCount, TotalMessagingAccountedBytes          uint64
	MonitorQueueAppliedHwmBytes, MonitorQueueAccountedBytes              uint64
	TotalInstanceAppliedHwmBytes, TotalInstanceAccountedBytes            uint64
	OversizeAdmissionCount, LargestOversizeMessageBytes                  uint64
	ActiveDirectionalQueueCount, ActiveCompletionDirectionalQueueCount   uint64
	ActiveSendQueueCount, ActiveReceiveQueueCount                        uint64
	OutstandingApplicationLeaseCount, RetiredQueueCount                  uint64
	DeferredOriginCreditBytes, UnlimitedManualQueueCount                 uint64
	BlockedRatioPPM, Flags                                               uint32
	ReservedUint64                                                       [8]uint64
}

func (s CoreHwmBudgetSnapshot) BudgetPlanningActive() bool { return s.Flags&(1<<0) != 0 }
func (s CoreHwmBudgetSnapshot) BudgetInsufficient() bool   { return s.Flags&(1<<1) != 0 }
func (s CoreHwmBudgetSnapshot) AggregateHwmValid() bool    { return s.Flags&(1<<2) != 0 }
func (s CoreHwmBudgetSnapshot) AggregateOverflow() bool    { return s.Flags&(1<<3) != 0 }

type AutoHwmProfile int

const (
	AutoHwmProfileCompact    AutoHwmProfile = AutoHwmProfile(0)
	AutoHwmProfileLowLatency AutoHwmProfile = AutoHwmProfile(1)
	AutoHwmProfileBalanced   AutoHwmProfile = AutoHwmProfile(2)
	AutoHwmProfileThroughput AutoHwmProfile = AutoHwmProfile(3)
)

type AutoHwmRecalcReason uint32

const (
	AutoHwmRecalcReasonNone           AutoHwmRecalcReason = AutoHwmRecalcReason(0)
	AutoHwmRecalcReasonInitial        AutoHwmRecalcReason = AutoHwmRecalcReason(1)
	AutoHwmRecalcReasonRoleChange     AutoHwmRecalcReason = AutoHwmRecalcReason(2)
	AutoHwmRecalcReasonPolicyToggle   AutoHwmRecalcReason = AutoHwmRecalcReason(3)
	AutoHwmRecalcReasonRefresh        AutoHwmRecalcReason = AutoHwmRecalcReason(4)
	AutoHwmRecalcReasonDeferredShrink AutoHwmRecalcReason = AutoHwmRecalcReason(5)
)
