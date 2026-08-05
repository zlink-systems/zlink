export interface ServerOptions {
  rid: string;
  httpUrl: string;
  redisEndpoint: string;
  redisKeyPrefix: string;
  routerEndpoint: string;
  pubSubEndpoint: string;
  evidenceFile: string;
  logDir: string;
}

export function parseServerOptions(args: readonly string[]): ServerOptions {
  const values = new Map<string, string>();
  for (let index = 0; index < args.length; index += 2) {
    const value = args[index + 1];
    if (value === undefined) throw new Error(`Missing value for '${args[index]}'.`);
    values.set(args[index].replace(/^--/, ''), value);
  }
  const required = (name: string): string => {
    const value = values.get(name);
    if (value === undefined) throw new Error(`--${name} is required.`);
    return value;
  };
  return {
    rid: required('rid'),
    httpUrl: required('http-url'),
    redisEndpoint: required('redis-endpoint'),
    redisKeyPrefix: required('redis-key-prefix'),
    routerEndpoint: required('router-endpoint'),
    pubSubEndpoint: required('pubsub-endpoint'),
    evidenceFile: required('evidence-file'),
    logDir: required('log-dir')
  };
}
