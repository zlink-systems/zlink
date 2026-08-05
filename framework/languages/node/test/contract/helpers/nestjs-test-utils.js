const net = require('node:net');
const { DiscoveryService, ModuleRef } = require('@nestjs/core');

function providerTokens(module) {
  return new Set(module.providers.map((provider) => provider.provide));
}

async function resolveModuleProviders(module, requestedTokens) {
  const values = new Map();
  const providers = new Map(module.providers.map((provider) => [provider.provide, provider]));

  for (const token of requestedTokens) {
    await resolveToken(token);
  }

  return values;

  async function resolveToken(token) {
    if (values.has(token)) {
      return values.get(token);
    }
    if (token === ModuleRef) {
      const moduleRef = {
        async create(type) {
          return new type();
        },
        get(type) {
          return values.get(type);
        }
      };
      values.set(token, moduleRef);
      return moduleRef;
    }
    if (token === DiscoveryService) {
      const discovery = {
        getProviders() {
          return [...providers.values()].map((provider) => ({
            token: provider.provide,
            metatype: typeof provider === 'function' ? provider : provider.useClass,
            instance: values.get(provider.provide)
          }));
        }
      };
      values.set(token, discovery);
      return discovery;
    }
    const provider = providers.get(token);
    if (provider === undefined) {
      throw new Error(`Could not find provider: ${String(token)}`);
    }
    if ('useValue' in provider && provider.useValue !== undefined) {
      values.set(provider.provide, provider.useValue);
      return provider.useValue;
    }

    if ('useFactory' in provider && provider.useFactory !== undefined) {
      const dependencies = [];
      for (const dependency of provider.inject ?? []) {
        dependencies.push(await resolveToken(dependency));
      }
      const value = await provider.useFactory(...dependencies);
      values.set(provider.provide, value);
      return value;
    }

    if ('useClass' in provider && provider.useClass !== undefined) {
      const value = new provider.useClass();
      values.set(provider.provide, value);
      return value;
    }

    throw new Error(`Provider ${String(token)} has no supported value or factory.`);
  }
}

async function reserveTcpEndpoint() {
  const server = net.createServer();
  await new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(0, '127.0.0.1', resolve);
  });
  const { port } = server.address();
  await new Promise((resolve, reject) => {
    server.close((error) => error ? reject(error) : resolve());
  });
  return `tcp://127.0.0.1:${port}`;
}

module.exports = {
  providerTokens,
  reserveTcpEndpoint,
  resolveModuleProviders
};
