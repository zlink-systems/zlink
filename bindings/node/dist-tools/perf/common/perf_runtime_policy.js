// SPDX-License-Identifier: MPL-2.0
'use strict';
Object.defineProperty(exports, "__esModule", { value: true });
const { integerEnv } = require('./perf_args');
function manualSocketOverridesEnabled(suite) {
    const upper = String(suite || '').trim().toUpperCase();
    const suiteFlag = upper ? `PERF_${upper}_ALLOW_MANUAL_SOCKET_OVERRIDES` : '';
    return (suiteFlag && integerEnv(suiteFlag, 0) > 0)
        || integerEnv('PERF_ALLOW_MANUAL_SOCKET_OVERRIDES', 0) > 0;
}
function resolveAutoHwmProfile(zlink) {
    const env = process.env.PERF_CTX_AUTO_HWM_PROFILE || process.env.PERF_AUTO_HWM_PROFILE || '';
    if (!zlink.AutoHwmProfile) {
        return undefined;
    }
    if (env === 'compact') {
        return zlink.AutoHwmProfile.Compact;
    }
    if (env === 'low_latency' || env === 'low-latency') {
        return zlink.AutoHwmProfile.LowLatency;
    }
    if (env === 'throughput') {
        return zlink.AutoHwmProfile.Throughput;
    }
    return zlink.AutoHwmProfile.Balanced;
}
function applyAutoHwmProfile(ctx, zlink) {
    if (!ctx?.options || !('coreHwmProfile' in ctx.options) || !zlink.AutoHwmProfile) {
        return;
    }
    ctx.options.coreHwmProfile = resolveAutoHwmProfile(zlink);
}
module.exports = {
    applyAutoHwmProfile,
    manualSocketOverridesEnabled,
    resolveAutoHwmProfile
};
