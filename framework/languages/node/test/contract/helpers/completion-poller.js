const zlink = require('@zlink-systems/zlink');

function ownCompletions(socket) {
  const poller = zlink.createPoller();
  const events = zlink.createPollEvents(1);
  let closed = false;
  poller.add(socket, [zlink.PollEventFlag.PollCompletion], 0);
  socket.setReadableHandler(() => {
    if (!closed) poller.wait(events, 0);
  });
  return {
    close() {
      if (closed) return;
      closed = true;
      events.close();
      poller.close();
    }
  };
}

module.exports = { ownCompletions };
