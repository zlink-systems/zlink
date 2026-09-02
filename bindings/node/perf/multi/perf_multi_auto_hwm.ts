// SPDX-License-Identifier: MPL-2.0

'use strict';

const zlink = require('@zlink-systems/zlink');
const { MonitorEventType } = zlink;
const { resolveMultiMonitorHwm } = require('./perf_multi_common');

const emittedMultiAutoHwmDetails = new Set();

function autoHwmDetailEnabled() {
  const value = process.env.PERF_MULTI_PRINT_AUTO_HWM_DETAIL
    ?? process.env.PERF_PRINT_AUTO_HWM_DETAIL;
  return value === undefined || value === '' || value !== '0';
}

function autoHwmRoleName(role) {
  switch (role) {
    case 1: return 'control';
    case 2: return 'routed';
    case 3: return 'fanout';
    case 4: return 'recv_ingress';
    case 6: return 'peer_queue';
    case 7: return 'stream';
    default: return 'none';
  }
}

function autoHwmProfileName(profile) {
  if (zlink.AutoHwmProfile) {
    if (profile === zlink.AutoHwmProfile.Compact) return 'compact';
    if (profile === zlink.AutoHwmProfile.LowLatency) return 'low_latency';
    if (profile === zlink.AutoHwmProfile.Balanced) return 'balanced';
    if (profile === zlink.AutoHwmProfile.Throughput) return 'throughput';
  }
  return 'unknown';
}

function autoHwmPolicyClassName(policyClass) {
  switch (policyClass) {
    case 1: return 'fanout';
    case 3: return 'recv_ingress';
    case 4: return 'routed';
    case 5: return 'control';
    case 6: return 'stream';
    case 7: return 'peer_queue';
    default: return 'unknown';
  }
}

function autoHwmRecalcReasonName(reason) {
  switch (reason) {
    case 1: return 'context_create';
    case 2: return 'socket_register';
    case 3: return 'socket_unregister';
    case 4: return 'socket_option';
    case 5: return 'manual';
    case 6: return 'budget_change';
    case 7: return 'topology_change';
    case 8: return 'timer';
    default: return 'unknown';
  }
}

function socketTypeName(socketOrType) {
  const socketType = typeof socketOrType === 'number' ? socketOrType : null;
  if (socketType !== null && zlink.SocketType) {
    switch (socketType) {
      case zlink.SocketType.Pair: return 'pair';
      case zlink.SocketType.Pub: return 'pub';
      case zlink.SocketType.Sub: return 'sub';
      case zlink.SocketType.Dealer: return 'dealer';
      case zlink.SocketType.Router: return 'router';
      case zlink.SocketType.XPub: return 'xpub';
      case zlink.SocketType.XSub: return 'xsub';
      case zlink.SocketType.Stream: return 'stream';
      default: return 'unknown';
    }
  }
  const socket = socketOrType;
  if (typeof socket.recvPacket === 'function') return 'stream';
  if (typeof socket.reply === 'function') return 'router';
  if (typeof socket.request === 'function') return 'dealer';
  if (typeof socket.publish === 'function') return 'pub';
  if (typeof socket.subscribe === 'function') return 'sub';
  if (typeof socket.send === 'function' && typeof socket.recv === 'function') return 'pair';
  return 'unknown';
}

function hwmSndBufDisplay(snapshot, socket) {
  const typeName = socketTypeName(socket);
  const roleName = autoHwmRoleName(snapshot.autoHwmRole);
  return autoHwmSnapshotSendSideVisible(typeName, roleName)
    ? String(snapshot.autoHwmEffectiveSndBuf)
    : '0';
}

function hwmRcvBufDisplay(snapshot, socket) {
  const typeName = socketTypeName(socket);
  const roleName = autoHwmRoleName(snapshot.autoHwmRole);
  return autoHwmSnapshotRecvSideVisible(typeName, roleName)
    ? String(snapshot.autoHwmEffectiveRcvBuf)
    : '0';
}

function autoHwmSnapshotSendSideVisible(socketTypeName_, roleName) {
  if ((socketTypeName_ === 'sub' || socketTypeName_ === 'xsub')
      && (roleName === 'recv_ingress' || roleName === 'control')) {
    return false;
  }
  return true;
}

function autoHwmSnapshotRecvSideVisible(socketTypeName_, roleName) {
    if ((socketTypeName_ === 'pub' || socketTypeName_ === 'xpub')
      && roleName === 'control') {
    return false;
  }
  return true;
}

function emitMultiSocketHwmDetail(socket, label, transport, msgSize) {
  if (!socket || !autoHwmDetailEnabled()) {
    return;
  }
  if (typeof socket.monitorOpen !== 'function') {
    return;
  }
  const monitor = socket.monitorOpen(
    [MonitorEventType.ConnectionReady],
    BigInt(resolveMultiMonitorHwm())
  );
  try {
    const snapshot = monitor.status();
    const pattern = process.env.PERF_MULTI_PATTERN || process.env.PERF_PATTERN || 'unknown';
    const component = process.env.PERF_MULTI_COMPONENT || 'process';
    const effectiveTransport = transport || process.env.PERF_MULTI_TRANSPORT || 'unknown';
    const socketType = socketTypeName(socket);
    const key = [
      pattern,
      effectiveTransport,
      component,
      label || 'socket',
      msgSize || 0,
      autoHwmRoleName(snapshot.autoHwmRole),
      snapshot.autoHwmAppliedSndHwmBytes,
      snapshot.autoHwmAppliedRcvHwmBytes,
      snapshot.autoHwmProfile,
      snapshot.autoHwmPolicyClass,
      String(snapshot.sndPendingBytes),
      String(snapshot.rcvPendingBytes),
      snapshot.autoHwmEffectiveSndBuf,
      snapshot.autoHwmEffectiveRcvBuf,
    ].join('|');
    if (emittedMultiAutoHwmDetails.has(key)) {
      return;
    }
    emittedMultiAutoHwmDetails.add(key);
    console.log(
      'AUTO_HWM_DETAIL'
      + `,pattern=${pattern}`
      + `,transport=${effectiveTransport}`
      + `,component=${component}`
      + `,label=${label || 'socket'}`
      + `,socket_type=${socketType}`
      + `,msg_size=${msgSize || 0}`
      + ',source=monitor_snapshot'
      + `,enabled=${snapshot.autoHwmEnabled ? 1 : 0}`
      + `,role=${autoHwmRoleName(snapshot.autoHwmRole)}`
      + `,role_id=${snapshot.autoHwmRole}`
      + `,profile=${autoHwmProfileName(snapshot.autoHwmProfile)}`
      + `,profile_id=${snapshot.autoHwmProfile}`
      + `,policy_class=${autoHwmPolicyClassName(snapshot.autoHwmPolicyClass)}`
      + `,policy_class_id=${snapshot.autoHwmPolicyClass}`
      + `,sndhwm=${snapshot.autoHwmAppliedSndHwmBytes}`
      + `,rcvhwm=${snapshot.autoHwmAppliedRcvHwmBytes}`
      + `,snd_pending_bytes=${snapshot.sndPendingBytes}`
      + `,rcv_pending_bytes=${snapshot.rcvPendingBytes}`
      + `,effective_sndbuf=${hwmSndBufDisplay(snapshot, socket)}`
      + `,effective_rcvbuf=${hwmRcvBufDisplay(snapshot, socket)}`
      + `,last_recalc_ms=${snapshot.autoHwmLastRecalcMs}`
      + `,last_recalc_reason=${autoHwmRecalcReasonName(snapshot.autoHwmLastRecalcReason)}`
      + `,send_blocked_ratio_ppm=${snapshot.autoHwmSendBlockedRatioPpm}`
      + `,deferred_sndhwm=${snapshot.autoHwmDeferredSndHwmValid ? snapshot.autoHwmDeferredSndHwmBytes : '-'}`
      + `,deferred_rcvhwm=${snapshot.autoHwmDeferredRcvHwmValid ? snapshot.autoHwmDeferredRcvHwmBytes : '-'}`
    );
  } catch (err) {
    // Diagnostic output must not turn a valid perf run into a failure.
  } finally {
    monitor?.close();
  }
}

module.exports = {
  emitMultiSocketHwmDetail
};
