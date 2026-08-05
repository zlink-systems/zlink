import fs from 'node:fs';
import { Module, type DynamicModule } from '@nestjs/common';
import { ConfigModule, ConfigService } from '@nestjs/config';

const PUBSUB_OPTIONS = Symbol.for('@zlink-systems/e2e-pubsub:options');

function createPubSubConfigurationModule<T>(validate: (value: unknown) => T): DynamicModule {
  const args = process.argv.slice(2);
  if (args.length !== 2 || args[0] !== '--config' || args[1].startsWith('--')) throw new Error('--config <path> is the only supported framework host argument.');
  const configPath = args[1];
  class PubSubConfigurationModule {}
  Module({})(PubSubConfigurationModule);
  return {
    module: PubSubConfigurationModule,
    imports: [ConfigModule.forRoot({
      cache: true,
      ignoreEnvFile: true,
      isGlobal: false,
      load: [() => ({ e2e: readConfig(configPath) })],
      skipProcessEnv: true,
      validatePredefined: false
    })],
    providers: [{ provide: PUBSUB_OPTIONS, inject: [ConfigService], useFactory: (config: ConfigService) => validate(config.get('e2e')) }],
    exports: [PUBSUB_OPTIONS]
  };
}

function readConfig(path: string): unknown {
  const document = JSON.parse(fs.readFileSync(path, 'utf8')) as { e2e?: unknown };
  if (document.e2e === undefined) throw new Error("Configuration section 'e2e' is required.");
  return document.e2e;
}

export { PUBSUB_OPTIONS, createPubSubConfigurationModule };
