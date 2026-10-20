const { badPortsSet } = require('undici/lib/web/fetch/constants');
const { ZLINK_FRAMEWORK_RUNTIME } = require('../../../packages/nestjs/dist');

// Keep OS port allocation; reject only listeners the platform WebSocket cannot use.
async function createBrowserSafeStreamApp(createApp, listenerName) {
  for (;;) {
    const app = await createApp();
    const endpoint = app
      .get(ZLINK_FRAMEWORK_RUNTIME)
      .getListenerStatus('stream', listenerName).endpoint;
    if (!badPortsSet.has(new URL(endpoint).port)) return app;
    await app.close();
  }
}

module.exports = { createBrowserSafeStreamApp };
