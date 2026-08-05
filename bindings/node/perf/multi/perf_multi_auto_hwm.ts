// SPDX-License-Identifier: MPL-2.0

'use strict';

const zlink = require('@zlink-systems/zlink');
const { MonitorEventType } = zlink;

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
    case 5: return 'spot_data';
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
    case 2: return 'spot_data';
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
  if (typeof socket.setPacketHandler === 'function') return 'stream';
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

function autoHwmSnapshotSocketTypeName(socketType) {
  return socketTypeName(typeof socketType === 'number' ? socketType : Number(socketType));
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
      && (roleName === 'spot_data' || roleName === 'control')) {
    return false;
  }
  return true;
}

function autoHwmEffectiveMsgSize(msgSize) {
  if (msgSize && Number(msgSize) !== 0) {
    return Number(msgSize);
  }
  const value = process.env.PERF_MSG_SIZES || process.env.PERF_MULTI_MSG_SIZES;
  if (!value) {
    return 0;
  }
  const parsed = Number.parseInt(String(value).trim(), 10);
  return Number.isNaN(parsed) ? 0 : parsed;
}

function autoHwmLabelIsControlSnapshot(label) {
  return typeof label === 'string' && label.indexOf('control') >= 0;
}

function autoHwmIncludeSpotSnapshotRow(label, socketName) {
  if (!autoHwmLabelIsControlSnapshot(label)) {
    return true;
  }
  return socketName === 'peer_ctrl_pub' || socketName === 'peer_ctrl_sub';
}

const emittedSpotNodeAutoHwmSnapshots = new Set();
const spotNodeIdentityIds = new WeakMap();
let spotNodeIdentitySeq = 0;
function spotNodeIdentity(node) {
  let id = spotNodeIdentityIds.get(node);
  if (id === undefined) {
    spotNodeIdentitySeq += 1;
    id = spotNodeIdentitySeq;
    spotNodeIdentityIds.set(node, id);
  }
  return id;
}

function emitSpotNodeAutoHwmSnapshot(node, label, transport, msgSize) {
  if (typeof node.internalSockets !== 'function') {
    return false;
  }
  const pattern = process.env.PERF_MULTI_PATTERN || process.env.PERF_PATTERN || 'unknown';
  const component = process.env.PERF_MULTI_COMPONENT || 'process';
  const effectiveTransport = transport || process.env.PERF_MULTI_TRANSPORT || 'unknown';
  const snapshotScope = autoHwmLabelIsControlSnapshot(label) ? 'control' : 'data';
  const dedupKey = [
    String(spotNodeIdentity(node)),
    effectiveTransport,
    String(msgSize || 0),
    snapshotScope
  ].join('|');
  if (emittedSpotNodeAutoHwmSnapshots.has(dedupKey)) {
    return true;
  }
  emittedSpotNodeAutoHwmSnapshots.add(dedupKey);

  let rows;
  try {
    rows = node.internalSockets();
  } catch (err) {
    return false;
  }
  if (!Array.isArray(rows) || rows.length === 0) {
    return true;
  }

  const effectiveMsgSize = autoHwmEffectiveMsgSize(msgSize);
  for (const row of rows) {
    if (!row || row.autoHwmVisible === false || row.socketName === 'internal_receiver') {
      continue;
    }
    if (!autoHwmIncludeSpotSnapshotRow(label, row.socketName)) {
      continue;
    }
    const snapshot = row.snapshot;
    if (!snapshot) {
      continue;
    }
    const typeName = autoHwmSnapshotSocketTypeName(row.socketType);
    const roleName = autoHwmRoleName(snapshot.autoHwmRole);
    const ownerNode = Number(row.owner) === 1;
    const owner = ownerNode ? 'node' : 'spot';
    const scope = ownerNode ? 'shared' : 'per-spot';
    const sndbuf = autoHwmSnapshotSendSideVisible(typeName, roleName)
      ? String(snapshot.autoHwmEffectiveSndBuf)
      : '0';
    const rcvbuf = autoHwmSnapshotRecvSideVisible(typeName, roleName)
      ? String(snapshot.autoHwmEffectiveRcvBuf)
      : '0';
    console.log(
      'AUTO_HWM_DETAIL'
      + `,pattern=${pattern}`
      + `,transport=${effectiveTransport}`
      + `,component=${component}`
      + `,label=${row.socketName}`
      + `,owner=${owner}`
      + `,owner_id=${row.ownerId}`
      + `,socket=${row.socketName}`
      + `,socket_type=${typeName}`
      + `,msg_size=${effectiveMsgSize}`
      + ',source=spotnode_snapshot'
      + `,enabled=${snapshot.autoHwmEnabled ? 1 : 0}`
      + `,role=${roleName}`
      + `,role_id=${snapshot.autoHwmRole}`
      + `,profile=${autoHwmProfileName(snapshot.autoHwmProfile)}`
      + `,profile_id=${snapshot.autoHwmProfile}`
      + `,policy_class=${autoHwmPolicyClassName(snapshot.autoHwmPolicyClass)}`
      + `,policy_class_id=${snapshot.autoHwmPolicyClass}`
      + `,unit_budget_bytes=${snapshot.autoHwmUnitBudgetBytes}`
      + `,size_cap=${snapshot.autoHwmSizeCap}`
      + `,scope=${scope}`
      + `,sndhwm=${snapshot.autoHwmAppliedSndHwmBytes}`
      + `,rcvhwm=${snapshot.autoHwmAppliedRcvHwmBytes}`
      + `,socket_message_slots=${snapshot.autoHwmSocketMessageSlots}`
      + `,effective_message_bytes=${snapshot.autoHwmEffectiveMessageBytes}`
      + `,effective_sndbuf=${sndbuf}`
      + `,effective_rcvbuf=${rcvbuf}`
      + `,last_recalc_reason=${autoHwmRecalcReasonName(snapshot.autoHwmLastRecalcReason)}`
    );
  }
  return true;
}

function emitMultiSocketHwmDetail(socket, label, transport, msgSize) {
  if (!socket || !autoHwmDetailEnabled()) {
    return;
  }
  if (typeof label === 'string' && label.indexOf('spotnode') === 0
      && typeof socket.internalSockets === 'function') {
    if (emitSpotNodeAutoHwmSnapshot(socket, label, transport, msgSize)) {
      return;
    }
  }
  if (typeof socket.monitorOpen !== 'function') {
    return;
  }
  const monitor = socket.monitorOpen([MonitorEventType.ConnectionReady]);
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
      String(snapshot.autoHwmUnitBudgetBytes),
      snapshot.autoHwmSizeCap,
      String(snapshot.autoHwmSocketMessageSlots),
      String(snapshot.autoHwmEffectiveMessageBytes),
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
      + `,unit_budget_bytes=${snapshot.autoHwmUnitBudgetBytes}`
      + `,size_cap=${snapshot.autoHwmSizeCap}`
      + `,sndhwm=${snapshot.autoHwmAppliedSndHwmBytes}`
      + `,rcvhwm=${snapshot.autoHwmAppliedRcvHwmBytes}`
      + `,socket_message_slots=${snapshot.autoHwmSocketMessageSlots}`
      + `,effective_message_bytes=${snapshot.autoHwmEffectiveMessageBytes}`
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
