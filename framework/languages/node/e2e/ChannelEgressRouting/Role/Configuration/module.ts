import * as fs from 'node:fs';
import { ConfigModule, ConfigService } from '@nestjs/config';
import { Module, type DynamicModule } from '@nestjs/common';

export const CHANNEL_EGRESS_OPTIONS = Symbol.for('@zlink-systems/e2e-channel-egress:options');

export function createChannelEgressConfiguration(validate: (value: unknown) => unknown): DynamicModule {
  const args = process.argv.slice(2);
  if (args.length !== 2 || args[0] !== '--config') throw new Error('--config <path> is required.');
  const configPath = args[1];
  class ChannelEgressConfigurationModule {}
  Module({})(ChannelEgressConfigurationModule);
  return {
    module: ChannelEgressConfigurationModule,
    imports: [ConfigModule.forRoot({
      cache: true,
      ignoreEnvFile: true,
      isGlobal: false,
      load: [() => ({ e2e: readConfig(configPath) })],
      skipProcessEnv: true,
      validatePredefined: false
    })],
    providers: [{
      provide: CHANNEL_EGRESS_OPTIONS,
      inject: [ConfigService],
      useFactory: (service: ConfigService) => validate(service.get('e2e'))
    }],
    exports: [CHANNEL_EGRESS_OPTIONS]
  };
}

function readConfig(path: string): unknown {
  const document = JSON.parse(fs.readFileSync(path, 'utf8')) as { e2e?: unknown };
  if (document.e2e === undefined) throw new Error('Configuration section e2e is required.');
  return document.e2e;
}
