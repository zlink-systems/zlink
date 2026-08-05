function waitForShutdown(): Promise<void> {
  return new Promise<void>((resolve) => {
    const keepAlive = setInterval(() => undefined, 1000);
    const stop = () => {
      clearInterval(keepAlive);
      resolve();
    };
    process.once('SIGINT', stop);
    process.once('SIGTERM', stop);
  });
}

export {
  waitForShutdown
};
