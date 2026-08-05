import * as fs from 'node:fs';
import { Module, type DynamicModule } from '@nestjs/common';
import { ConfigModule, ConfigService } from '@nestjs/config';

type DeliveryDispatchServerConfig = {
  dispatchApiHttpUrl: string;
  dispatchEndpoint: string;
  dispatchSpotEndpoint: string;
  courierStreamEndpoint: string;
  courierSessionSpotEndpoint: string;
  courierActorNode1SpotEndpoint: string;
  courierActorNode2SpotEndpoint: string;
  trackingEndpoint: string;
  trackingSpotEndpoint: string;
  sessionStreamEndpoint: string;
  sessionSpotRouterEndpoint: string;
  redisEndpoint: string;
  redisKeyPrefix: string;
  logDir: string;
  workDir: string;
};

const DELIVERYDISPATCH_SAMPLE_CONFIG = Symbol.for('DELIVERYDISPATCH_SAMPLE_CONFIG');
class DeliveryDispatchConfigurationModule {}
Module({})(DeliveryDispatchConfigurationModule);

function createDeliveryDispatchConfigurationModule(
  requiredKeys: readonly (keyof DeliveryDispatchServerConfig)[]
): DynamicModule {
  const configPath = readConfigPath(process.argv.slice(2));
  return {
    module: DeliveryDispatchConfigurationModule,
    imports: [
      ConfigModule.forRoot({
        cache: true,
        ignoreEnvFile: true,
        isGlobal: false,
        load: [() => ({ sample: readSampleConfig(configPath) })],
        skipProcessEnv: true,
        validatePredefined: false
      })
    ],
    providers: [{
      provide: DELIVERYDISPATCH_SAMPLE_CONFIG,
      inject: [ConfigService],
      useFactory: (config: ConfigService) => validateSampleConfig(config.get('sample'), requiredKeys)
    }],
    exports: [DELIVERYDISPATCH_SAMPLE_CONFIG]
  };
}

function readConfigPath(args: readonly string[]): string {
  if (args.length !== 2 || args[0] !== '--config' || args[1].startsWith('--')) {
    throw new Error('--config <path> is the only supported framework host argument.');
  }
  return args[1];
}

function readSampleConfig(configPath: string): unknown {
  const document = JSON.parse(fs.readFileSync(configPath, 'utf8')) as { sample?: unknown };
  if (document.sample === undefined) throw new Error("Configuration section 'sample' is required.");
  return document.sample;
}

function validateSampleConfig(
  value: unknown,
  requiredKeys: readonly (keyof DeliveryDispatchServerConfig)[]
): DeliveryDispatchServerConfig {
  if (value === null || typeof value !== 'object' || Array.isArray(value)) {
    throw new Error("Configuration section 'sample' must be an object.");
  }
  const config = value as Record<string, unknown>;
  for (const key of requiredKeys) {
    if (typeof config[key] !== 'string' || (config[key] as string).length === 0) {
      throw new Error(`Configuration value 'sample.${key}' must be a non-empty string.`);
    }
  }
  return config as DeliveryDispatchServerConfig;
}

export { DELIVERYDISPATCH_SAMPLE_CONFIG, createDeliveryDispatchConfigurationModule };
export type { DeliveryDispatchServerConfig };
