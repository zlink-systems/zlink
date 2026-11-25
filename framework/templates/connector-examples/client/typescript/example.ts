import {
  ZlinkStreamConnectionState,
  ZlinkStreamDispatchMode,
  zlinkStreamConnectorFactory
} from '@zlink-systems/stream-connector';

const timeoutMs = 5000;
const score = '9223372036854775807';

class Notice {
  declare text: string;
}

interface Echoed {
  score: string;
  nickname: string | null;
  label: string;
}

export async function runExample(endpoint: string, report: (line: string) => void): Promise<void> {
  const connector = zlinkStreamConnectorFactory.create({
    endpoint,
    dispatchMode: ZlinkStreamDispatchMode.Immediate,
    requestTimeoutMs: timeoutMs,
    waitTimeoutMs: timeoutMs
  });

  try {
    await connector.connect();
    report('Connected');

    // Publish is one-way. Notice is a separate server push.
    await connector.send({}).packetName('Publish').submit();
    const { payload: notice } = await connector.waitFor<Notice>(Notice).submit();
    if (notice.text !== 'Hello from the server') throw new Error('Unexpected Notice');
    report('Push: ' + notice.text);

    // Subscribe before asking the server to drop this session. The connector reconnects.
    const { promise, resolve } = Promise.withResolvers<void>();
    const subscription = connector.onConnectionStateChanged(({ current }) => {
      if (current === ZlinkStreamConnectionState.Connected) resolve();
    });
    try {
      await connector.send({}).packetName('Reconnect').submit();
      const deadline = AbortSignal.timeout(timeoutMs);
      await Promise.race([
        promise,
        new Promise<never>((_, reject) => {
          deadline.addEventListener('abort', () => reject(deadline.reason), { once: true });
        })
      ]);
    } finally {
      subscription.dispose();
    }
    report('Reconnected');

    // A 64-bit integer is a decimal string. nickname is required, and label can be omitted.
    const echoed = await connector
      .request({ score, nickname: null })
      .packetName('Echo')
      .submit<Echoed>();
    if (echoed.score !== score || echoed.nickname !== null || echoed.label !== 'default') {
      throw new Error('Unexpected Echoed: ' + JSON.stringify(echoed));
    }
    report('Reply: ' + JSON.stringify(echoed));

    // Wait for the server to acknowledge Leave before closing the connection.
    const left = await connector.request({}).packetName('Leave').submit<{ accepted: boolean }>();
    if (left.accepted !== true) throw new Error('Leave was rejected');
    report('Leave acknowledged');
  } finally {
    await connector.close();
  }
  report('Closed');
}
