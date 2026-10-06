import {
  zlinkStreamConnectorFactory,
  ZlinkStreamDispatchMode
} from '@zlink-systems/stream-connector';

class LeaderboardUpdate {
  rank = 0;
}
class Ready {
  stage = '';
}
class MatchFound {
  matchId = '';
}
class OrderChanged {
  status = '';
}

export async function runReceiving(endpoint: string): Promise<string> {
  const connector = zlinkStreamConnectorFactory.create({
    endpoint,
    dispatchMode: ZlinkStreamDispatchMode.Manual
  });
  let handled = 0;
  let frames = 0;
  let running = true;
  const renderFrame = (): void => {
    frames++;
    running = false;
  };
  const subscription = connector.on<LeaderboardUpdate>(LeaderboardUpdate, () => {
    handled++;
  });
  try {
    await connector.connect();
    await connector.send({ stage: 'pump' }).packetName('ReceivingStage').submit();
    await connector.waitFor<Ready>(Ready).submit();
    // --8<-- [start:receiving-pump]
    while (running) {
      await connector.dispatch();
      renderFrame();
    }
    // --8<-- [end:receiving-pump]
    // --8<-- [start:receiving-unsubscribe]
    subscription.dispose();
    // --8<-- [end:receiving-unsubscribe]
    await connector.send({ stage: 'unsubscribed' }).packetName('ReceivingStage').submit();
    await connector.waitFor<Ready>(Ready).submit();
    await connector.dispatch();
    await connector.send({ stage: 'match' }).packetName('ReceivingStage').submit();
    // --8<-- [start:receiving-wait]
    const found = await connector
      .waitFor<MatchFound>(MatchFound)
      .where((message) => message.payload.matchId === 'match-7f3a')
      .timeout(30_000)
      .submit();
    // --8<-- [end:receiving-wait]
    // --8<-- [start:receiving-sequence]
    await connector.expectNone<OrderChanged>(OrderChanged).within(100).run();
    await connector.send({ stage: 'orders' }).packetName('ReceivingStage').submit();
    const steps = await connector
      .waitForSequence<OrderChanged>(OrderChanged)
      .expect((message) => message.payload.status === 'paid')
      .expect((message) => message.payload.status === 'shipped')
      .timeout(2_000)
      .run();
    // --8<-- [end:receiving-sequence]
    // --8<-- [start:receiving-count]
    const count = connector.receivedCount('LeaderboardUpdate');
    // --8<-- [end:receiving-count]
    if (
      handled !== 1 ||
      frames !== 1 ||
      count !== 2 ||
      found.payload.matchId !== 'match-7f3a' ||
      steps.length !== 2
    ) {
      throw new Error(
        'Receiving tutorial result did not match the expected handlers and messages.'
      );
    }
    return `receiving: handler=${handled}, frames=${frames}, match=${found.payload.matchId}, sequence=${steps.map((m) => m.payload.status).join(',')}, count=${count}`;
  } finally {
    subscription.dispose();
    await connector.close();
  }
}
