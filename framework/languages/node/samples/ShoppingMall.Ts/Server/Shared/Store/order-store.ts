import fs from 'node:fs';
import path from 'node:path';
import { DecimalAmount, StartOrderWorkflowReq } from '../../../Shared/Contracts/messages';
import type {
  OrderLineInput,
  OrderState,
  ServerAssertionReq,
  StartOrderReq,
  StartOrderWorkflowRes
} from '../../../Shared/Contracts/messages';
import type { VerifyExpectedVersionFenceRes } from '../Internal/shoppingmall-workflow-messages';
import type { OrderEventType, StoredOrderEvent } from '../Domain/order-events';
import { OrderAggregate } from '../../OrderWorkflow/Domain/ShoppingMall/order-domain';

type MappingStatus = 'pending' | 'started';

interface CartSeed { lines: readonly OrderLineInput[]; amount: DecimalAmount; currency: string; }
interface PaymentSeed { shouldAuthorize: boolean; failureReason?: string; }
interface EffectResult { accepted: boolean; reason?: string; attempts: number; }

interface CommerceData {
  mappings: Record<string, { orderId: string; status: MappingStatus } | undefined>;
  carts: Record<string, CartSeed | undefined>;
  addresses: Record<string, true | undefined>;
  inventory: Record<string, number | undefined>;
  paymentMethods: Record<string, PaymentSeed | undefined>;
  orderPaymentMethods: Record<string, string | undefined>;
  reservations: Record<string, EffectResult | undefined>;
  releases: Record<string, { released: boolean; attempts: number } | undefined>;
  payments: Record<string, EffectResult | undefined>;
  evidence: string[];
}

interface OrderData {
  stream: StoredOrderEvent[];
  projection?: OrderState;
  ownerInstanceId?: string;
  evidence: string[];
}

class ExpectedVersionConflict extends Error {
  constructor(readonly expectedVersion: number, readonly actualVersion: number) {
    super(`Expected version ${expectedVersion}, actual ${actualVersion}.`);
  }
}

class OrderStore {
  private readonly commercePath: string;
  private readonly ordersDir: string;

  constructor(workDir: string) {
    this.commercePath = path.join(workDir, 'commerce.json');
    this.ordersDir = path.join(workDir, 'orders');
    fs.mkdirSync(this.ordersDir, { recursive: true });
  }

  seedSelfCheck(): { seeded: boolean } {
    return this.updateCommerce((data) => {
      data.carts['cart-success'] = { lines: [{ sku: 'sku-normal', quantity: 2 }], amount: DecimalAmount.fromMinorUnits(12_000n), currency: 'USD' };
      data.carts['cart-inventory-fail'] = { lines: [{ sku: 'sku-limited', quantity: 3 }], amount: DecimalAmount.fromMinorUnits(7_500n), currency: 'USD' };
      data.carts['cart-payment-fail'] = { lines: [{ sku: 'sku-normal', quantity: 1 }], amount: DecimalAmount.fromMinorUnits(6_000n), currency: 'USD' };
      data.addresses['addr-home'] = true;
      data.addresses['addr-office'] = true;
      data.inventory['sku-normal'] = 100;
      data.inventory['sku-limited'] = 1;
      data.paymentMethods['pm-ok'] = { shouldAuthorize: true };
      data.paymentMethods['pm-decline'] = { shouldAuthorize: false, failureReason: 'payment authorization failed' };
      data.evidence.push('self-check seeds loaded');
      return { seeded: true };
    });
  }

  reserveOrder(request: StartOrderReq): StartOrderWorkflowReq {
    return this.updateCommerce((data) => {
      const cart = data.carts[request.cartId];
      if (cart === undefined || data.addresses[request.shippingAddressId] !== true || data.paymentMethods[request.paymentMethodId] === undefined) {
        throw new Error('Cart, shipping address, or payment method is not seeded.');
      }
      const existing = data.mappings[request.idempotencyKey];
      const orderId = existing?.orderId ?? `order-${request.idempotencyKey.replace(/[^a-zA-Z0-9-]/g, '-')}`;
      if (existing === undefined) {
        data.mappings[request.idempotencyKey] = { orderId, status: 'pending' };
        data.evidence.push(`mapping pending key=${request.idempotencyKey} order=${orderId}`);
      }
      data.orderPaymentMethods[orderId] = request.paymentMethodId;
      return new StartOrderWorkflowReq(
        orderId, request.cartId, request.shippingAddressId, request.paymentMethodId,
        request.idempotencyKey, `start-${request.idempotencyKey}`, cart.lines, cart.amount, cart.currency
      );
    });
  }

  createPendingMapping(idempotencyKey: string, orderId: string, ownerInstanceId: string): { created: boolean } {
    return this.updateCommerce((data) => {
      if (data.mappings[idempotencyKey] !== undefined) return { created: false };
      data.mappings[idempotencyKey] = { orderId, status: 'pending' };
      data.evidence.push(`mapping pending key=${idempotencyKey} order=${orderId} via=${ownerInstanceId}`);
      return { created: true };
    });
  }

  startOrder(request: StartOrderWorkflowReq, instanceId: string): StartOrderWorkflowRes {
    for (;;) {
      const current = this.readOrder(request.orderId);
      const duplicate = current.stream.some((event) => event.sourceCommandId === request.sourceCommandId);
      if (duplicate) {
        const state = this.healProjection(request.orderId, instanceId);
        this.confirmMapping(request.idempotencyKey, request.orderId);
        return { state };
      }
      if (current.stream.length !== 0) throw new Error(`Order '${request.orderId}' was started by another command.`);
      try {
        const state = this.appendEvent(request.orderId, 0, 'OrderStarted', (eventId) => ({
          eventId,
          orderId: request.orderId,
          cartId: request.cartId,
          shippingAddressId: request.shippingAddressId,
          lines: request.lines,
          amount: request.amount,
          currency: request.currency,
          sourceCommandId: request.sourceCommandId
        }), request.sourceCommandId, instanceId);
        this.confirmMapping(request.idempotencyKey, request.orderId);
        return { state };
      } catch (error) {
        if (!(error instanceof ExpectedVersionConflict)) throw error;
      }
    }
  }

  prepareInventoryReserved(request: StartOrderWorkflowReq, instanceId: string): StartOrderWorkflowRes {
    this.startOrder(request, instanceId);
    for (;;) {
      const order = this.readOrder(request.orderId);
      const aggregate = this.foldAggregate(order.stream);
      if (aggregate.lastEventType() !== 'OrderStarted') return { state: aggregate.snapshot() };
      const result = this.reserveInventoryEffect(request.orderId, request.lines);
      try {
        const state = result.accepted
          ? this.appendEvent(request.orderId, order.stream.length, 'InventoryReserved', (eventId) => ({
              eventId, orderId: request.orderId, reservationId: this.reservationId(request.orderId)
            }), undefined, instanceId)
          : this.appendEvent(request.orderId, order.stream.length, 'InventoryReservationFailed', (eventId) => ({
              eventId, orderId: request.orderId, reason: result.reason
            }), undefined, instanceId);
        return { state };
      } catch (error) {
        if (!(error instanceof ExpectedVersionConflict)) throw error;
      }
    }
  }

  prepareInventoryEffect(request: StartOrderWorkflowReq, instanceId: string): StartOrderWorkflowRes {
    const started = this.startOrder(request, instanceId);
    this.reserveInventoryEffect(request.orderId, request.lines);
    this.appendOrderEvidence(request.orderId, `interrupted after inventory effect owner=${instanceId}`);
    return started;
  }

  continueOrder(orderId: string, instanceId: string): { state: OrderState } {
    for (;;) {
      const order = this.readOrder(orderId);
      if (order.stream.length === 0) throw new Error(`Order '${orderId}' does not exist.`);
      const aggregate = this.foldAggregate(order.stream);
      this.healProjection(orderId, instanceId);
      const last = aggregate.lastEventType();
      if (last === 'OrderConfirmed' || last === 'OrderFailed') return { state: aggregate.snapshot() };

      const started = this.startedPayload(order.stream);
      let type: OrderEventType;
      let payload: (eventId: string) => Record<string, unknown>;
      if (last === 'OrderStarted') {
        const result = this.reserveInventoryEffect(orderId, started.lines);
        type = result.accepted ? 'InventoryReserved' : 'InventoryReservationFailed';
        payload = result.accepted
          ? (eventId) => ({ eventId, orderId, reservationId: this.reservationId(orderId) })
          : (eventId) => ({ eventId, orderId, reason: result.reason });
      } else if (last === 'InventoryReservationFailed') {
        type = 'OrderFailed';
        payload = (eventId) => ({ eventId, orderId, reason: aggregate.snapshot().reason, failedAtUnixMs: Date.now() });
      } else if (last === 'InventoryReserved') {
        const result = this.authorizePaymentEffect(orderId);
        type = result.accepted ? 'PaymentAuthorized' : 'PaymentFailed';
        payload = result.accepted
          ? (eventId) => ({ eventId, orderId, paymentId: this.paymentId(orderId) })
          : (eventId) => ({ eventId, orderId, reason: result.reason });
      } else if (last === 'PaymentAuthorized') {
        type = 'OrderConfirmed';
        payload = (eventId) => ({ eventId, orderId, confirmedAtUnixMs: Date.now() });
      } else if (last === 'PaymentFailed') {
        const reason = aggregate.snapshot().reason ?? 'payment authorization failed';
        this.releaseInventoryEffect(orderId, started.lines);
        type = 'InventoryReleased';
        payload = (eventId) => ({ eventId, orderId, reservationId: this.reservationId(orderId), reason });
      } else {
        type = 'OrderFailed';
        payload = (eventId) => ({ eventId, orderId, reason: aggregate.snapshot().reason, failedAtUnixMs: Date.now() });
      }

      try {
        this.appendEvent(orderId, order.stream.length, type, payload, undefined, instanceId);
      } catch (error) {
        if (!(error instanceof ExpectedVersionConflict)) throw error;
      }
    }
  }

  verifyExpectedVersionFence(orderId: string, writerInstanceId: string): VerifyExpectedVersionFenceRes {
    const order = this.readOrder(orderId);
    if (order.stream.length === 0) throw new Error(`Order '${orderId}' does not exist.`);
    if (order.ownerInstanceId === undefined) throw new Error(`Order '${orderId}' has no owner evidence.`);
    const expectedVersion = order.stream.length - 1;
    if (writerInstanceId === order.ownerInstanceId) {
      return {
        rejected: false,
        expectedVersion,
        actualVersion: order.stream.length,
        writerInstanceId,
        ownerInstanceId: order.ownerInstanceId
      };
    }
    try {
      this.appendEvent(orderId, expectedVersion, 'OrderConfirmed', (eventId) => ({
        eventId, orderId, confirmedAtUnixMs: Date.now()
      }));
    } catch (error) {
      if (error instanceof ExpectedVersionConflict) {
        this.appendOrderEvidence(orderId, `overlap writer rejected writer=${writerInstanceId} owner=${order.ownerInstanceId} expected=${error.expectedVersion} actual=${error.actualVersion}`);
        return {
          rejected: true,
          expectedVersion: error.expectedVersion,
          actualVersion: error.actualVersion,
          writerInstanceId,
          ownerInstanceId: order.ownerInstanceId
        };
      }
      throw error;
    }
    throw new Error(`Stale expected version was accepted for '${orderId}'.`);
  }

  getOrder(orderId: string): { state: OrderState } {
    const state = this.readOrder(orderId).projection;
    if (state === undefined) throw new Error(`Order projection '${orderId}' does not exist.`);
    return { state };
  }

  deleteProjection(orderId: string): { deleted: boolean } {
    return this.updateOrder(orderId, (data) => {
      const deleted = data.projection !== undefined;
      delete data.projection;
      data.evidence.push(`projection deleted order=${orderId}`);
      return { deleted };
    });
  }

  rebuildProjection(orderId: string): { state: OrderState } {
    return this.updateOrder(orderId, (data) => {
      const state = this.fold(data.stream);
      data.projection = state;
      data.evidence.push(`projection replayed order=${orderId} events=${data.stream.length}`);
      return { state };
    });
  }

  assertEvidence(request: ServerAssertionReq): { passed: boolean; evidence: string[] } {
    const commerce = this.readCommerce();
    const problems: string[] = [];
    const evidence = [...commerce.evidence];
    const expected = new Map<string, readonly OrderEventType[]>([
      [request.successfulOrderId, ['OrderStarted', 'InventoryReserved', 'PaymentAuthorized', 'OrderConfirmed']],
      [request.pendingRecoveredOrderId, ['OrderStarted', 'InventoryReserved', 'PaymentAuthorized', 'OrderConfirmed']],
      [request.concurrentOrderId, ['OrderStarted', 'InventoryReserved', 'PaymentAuthorized', 'OrderConfirmed']],
      [request.resumedOrderId, ['OrderStarted', 'InventoryReserved', 'PaymentAuthorized', 'OrderConfirmed']],
      [request.interruptedOrderId, ['OrderStarted', 'InventoryReserved', 'PaymentAuthorized', 'OrderConfirmed']],
      [request.inventoryFailureOrderId, ['OrderStarted', 'InventoryReservationFailed', 'OrderFailed']],
      [request.paymentFailureOrderId, ['OrderStarted', 'InventoryReserved', 'PaymentFailed', 'InventoryReleased', 'OrderFailed']]
    ]);
    for (const orderId of request.scaleOutOrderIds) {
      expected.set(orderId, ['OrderStarted', 'InventoryReserved', 'PaymentAuthorized', 'OrderConfirmed']);
    }
    for (const [orderId, eventTypes] of expected) {
      const order = this.readOrder(orderId);
      evidence.push(...order.evidence);
      this.expectEvents(order, orderId, eventTypes, problems);
    }
    const scaleOwners = new Set(request.scaleOutOrderIds.map((orderId) => this.readOrder(orderId).ownerInstanceId));
    if (scaleOwners.size !== 2 || scaleOwners.has(undefined)) problems.push(`scale owners were ${JSON.stringify([...scaleOwners])}`);
    const released = commerce.releases[this.reservationId(request.paymentFailureOrderId)];
    if (released?.released !== true) problems.push(`inventory was not released order=${request.paymentFailureOrderId}`);
    const interruptedReservation = commerce.reservations[this.reservationId(request.interruptedOrderId)];
    if (interruptedReservation?.attempts !== 2) problems.push(`interrupted reservation attempts=${interruptedReservation?.attempts ?? 0}`);
    if (!this.readOrder(request.successfulOrderId).evidence.some((line) => line.includes('overlap writer rejected'))) {
      problems.push(`stale owner fencing evidence missing order=${request.successfulOrderId}`);
    }
    return { passed: problems.length === 0, evidence: [...evidence, ...problems] };
  }

  private appendEvent(
    orderId: string,
    expectedVersion: number,
    eventType: OrderEventType,
    payloadFactory: (eventId: string) => Record<string, unknown>,
    sourceCommandId?: string,
    instanceId?: string
  ): OrderState {
    return this.updateOrder(orderId, (data) => {
      if (data.stream.length !== expectedVersion) throw new ExpectedVersionConflict(expectedVersion, data.stream.length);
      const version = expectedVersion + 1;
      const eventId = `${orderId}:${version}:${eventType}`;
      data.stream.push({
        eventId,
        sourceCommandId,
        orderId,
        eventType,
        payload: [...Buffer.from(JSON.stringify(payloadFactory(eventId)), 'utf8')],
        version,
        createdAtUnixMs: Date.now()
      });
      data.ownerInstanceId ??= instanceId;
      data.projection = this.fold(data.stream);
      data.evidence.push(`event order=${orderId} version=${version} type=${eventType}`);
      return data.projection;
    });
  }

  private healProjection(orderId: string, instanceId?: string): OrderState {
    return this.updateOrder(orderId, (data) => {
      const state = this.fold(data.stream);
      data.projection = state;
      data.ownerInstanceId ??= instanceId;
      return state;
    });
  }

  private reserveInventoryEffect(orderId: string, lines: readonly OrderLineInput[]): EffectResult {
    const reservationId = this.reservationId(orderId);
    return this.updateCommerce((data) => {
      const previous = data.reservations[reservationId];
      if (previous !== undefined) {
        previous.attempts += 1;
        data.evidence.push(`inventory replay reservation=${reservationId} attempts=${previous.attempts}`);
        return { ...previous };
      }
      const unavailable = lines.find((line) => (data.inventory[line.sku] ?? 0) < line.quantity);
      const result: EffectResult = unavailable === undefined
        ? { accepted: true, attempts: 1 }
        : { accepted: false, reason: 'inventory reservation failed', attempts: 1 };
      if (result.accepted) {
        for (const line of lines) data.inventory[line.sku] = (data.inventory[line.sku] ?? 0) - line.quantity;
      }
      data.reservations[reservationId] = result;
      data.evidence.push(`inventory effect reservation=${reservationId} accepted=${result.accepted}`);
      return { ...result };
    });
  }

  private authorizePaymentEffect(orderId: string): EffectResult {
    const paymentId = this.paymentId(orderId);
    return this.updateCommerce((data) => {
      const previous = data.payments[paymentId];
      if (previous !== undefined) {
        previous.attempts += 1;
        return { ...previous };
      }
      const paymentMethodId = data.orderPaymentMethods[orderId];
      if (paymentMethodId === undefined) throw new Error(`Order '${orderId}' has no payment method snapshot.`);
      const seed = data.paymentMethods[paymentMethodId];
      if (seed === undefined) throw new Error(`Payment method '${paymentMethodId}' is not seeded.`);
      const result: EffectResult = { accepted: seed.shouldAuthorize, reason: seed.failureReason, attempts: 1 };
      data.payments[paymentId] = result;
      return { ...result };
    });
  }

  private releaseInventoryEffect(orderId: string, lines: readonly OrderLineInput[]): void {
    const reservationId = this.reservationId(orderId);
    this.updateCommerce((data) => {
      const previous = data.releases[reservationId];
      if (previous !== undefined) {
        previous.attempts += 1;
        return;
      }
      for (const line of lines) data.inventory[line.sku] = (data.inventory[line.sku] ?? 0) + line.quantity;
      data.releases[reservationId] = { released: true, attempts: 1 };
    });
  }

  private confirmMapping(idempotencyKey: string, orderId: string): void {
    this.updateCommerce((data) => {
      const mapping = data.mappings[idempotencyKey];
      if (mapping === undefined || mapping.orderId !== orderId) throw new Error(`Idempotency mapping for '${idempotencyKey}' does not own '${orderId}'.`);
      mapping.status = 'started';
    });
  }

  private fold(stream: readonly StoredOrderEvent[]): OrderState {
    return this.foldAggregate(stream).snapshot();
  }

  private foldAggregate(stream: readonly StoredOrderEvent[]): OrderAggregate {
    if (stream.length === 0) throw new Error('Cannot fold an empty order stream.');
    const aggregate = new OrderAggregate();
    for (const event of stream) aggregate.apply(event.orderId, event.eventType, this.decodePayload(event), event.createdAtUnixMs);
    return aggregate;
  }

  private startedPayload(stream: readonly StoredOrderEvent[]): { lines: readonly OrderLineInput[] } {
    const event = stream.find((candidate) => candidate.eventType === 'OrderStarted');
    if (event === undefined) throw new Error('Order stream has no OrderStarted event.');
    return this.decodePayload(event) as { lines: readonly OrderLineInput[] };
  }

  private decodePayload(event: StoredOrderEvent): Record<string, any> {
    return JSON.parse(Buffer.from(event.payload).toString('utf8')) as Record<string, any>;
  }

  private expectEvents(order: OrderData, orderId: string, expected: readonly OrderEventType[], problems: string[]): void {
    const actual = order.stream.map((event) => event.eventType);
    if (JSON.stringify(actual) !== JSON.stringify(expected)) problems.push(`order=${orderId} events=${actual.join(',')} expected=${expected.join(',')}`);
    if (new Set(order.stream.map((event) => event.eventId)).size !== order.stream.length) problems.push(`duplicate event id order=${orderId}`);
    if (order.stream.some((event, index) => event.version !== index + 1)) problems.push(`non-contiguous version order=${orderId}`);
    for (const event of order.stream) {
      this.expectEventContract(event, orderId, problems);
    }
  }

  private expectEventContract(event: StoredOrderEvent, orderId: string, problems: string[]): void {
    let payload: Record<string, unknown>;
    try {
      payload = this.decodePayload(event);
    } catch {
      problems.push(`event payload is not UTF-8 JSON bytes order=${orderId} event=${event.eventId}`);
      return;
    }
    const encoded = Buffer.from(JSON.stringify(payload), 'utf8');
    if (!encoded.equals(Buffer.from(event.payload))) problems.push(`event payload bytes are not canonical UTF-8 JSON order=${orderId} event=${event.eventId}`);
    if (event.orderId !== orderId || payload.orderId !== orderId || payload.eventId !== event.eventId) {
      problems.push(`event identity fields differ order=${orderId} event=${event.eventId}`);
    }
    const required: Record<OrderEventType, readonly string[]> = {
      OrderStarted: ['eventId', 'orderId', 'cartId', 'shippingAddressId', 'lines', 'amount', 'currency', 'sourceCommandId'],
      InventoryReserved: ['eventId', 'orderId', 'reservationId'],
      InventoryReservationFailed: ['eventId', 'orderId', 'reason'],
      PaymentAuthorized: ['eventId', 'orderId', 'paymentId'],
      PaymentFailed: ['eventId', 'orderId', 'reason'],
      InventoryReleased: ['eventId', 'orderId', 'reservationId', 'reason'],
      OrderConfirmed: ['eventId', 'orderId', 'confirmedAtUnixMs'],
      OrderFailed: ['eventId', 'orderId', 'reason', 'failedAtUnixMs']
    };
    const missing = required[event.eventType].filter((field) => payload[field] === undefined);
    if (missing.length !== 0) problems.push(`event payload fields missing order=${orderId} event=${event.eventId} fields=${missing.join(',')}`);
    if (event.eventType === 'OrderStarted') {
      if (event.sourceCommandId === undefined || payload.sourceCommandId !== event.sourceCommandId) {
        problems.push(`OrderStarted SourceCommandId differs order=${orderId} event=${event.eventId}`);
      }
    } else if (event.sourceCommandId !== undefined) {
      problems.push(`non-start event has SourceCommandId order=${orderId} event=${event.eventId}`);
    }
  }

  private appendOrderEvidence(orderId: string, line: string): void {
    this.updateOrder(orderId, (data) => { data.evidence.push(line); });
  }

  private reservationId(orderId: string): string { return `reservation-${orderId}`; }
  private paymentId(orderId: string): string { return `payment-${orderId}`; }
  private orderPath(orderId: string): string { return path.join(this.ordersDir, `${Buffer.from(orderId).toString('base64url')}.json`); }

  private readCommerce(): CommerceData {
    return this.rehydrateCommerce(this.readJson(this.commercePath, this.emptyCommerce()));
  }

  private rehydrateCommerce(data: CommerceData): CommerceData {
    for (const cart of Object.values(data.carts)) {
      if (cart !== undefined) {
        cart.amount = DecimalAmount.fromWire(cart.amount);
      }
    }
    return data;
  }

  private readOrder(orderId: string): OrderData {
    return this.rehydrateOrder(this.readJson<OrderData>(
      this.orderPath(orderId),
      { stream: [], evidence: [] }
    ));
  }

  private rehydrateOrder(data: OrderData): OrderData {
    if (data.projection?.amount !== undefined) {
      data.projection = {
        ...data.projection,
        amount: DecimalAmount.fromWire(data.projection.amount)
      };
    }
    return data;
  }

  private updateCommerce<T>(mutate: (data: CommerceData) => T): T {
    return this.updateFile(
      this.commercePath,
      this.emptyCommerce(),
      (data) => mutate(this.rehydrateCommerce(data))
    );
  }

  private updateOrder<T>(orderId: string, mutate: (data: OrderData) => T): T {
    return this.updateFile(
      this.orderPath(orderId),
      { stream: [], evidence: [] },
      (data) => mutate(this.rehydrateOrder(data))
    );
  }

  private updateFile<TData, TResult>(filePath: string, initial: TData, mutate: (data: TData) => TResult): TResult {
    const lockPath = `${filePath}.lock`;
    const lock = this.acquireLock(lockPath);
    try {
      const data = this.readJson(filePath, initial);
      const result = mutate(data);
      const temporary = `${filePath}.${process.pid}.${Math.random().toString(16).slice(2)}.tmp`;
      fs.writeFileSync(temporary, JSON.stringify(data, null, 2));
      fs.renameSync(temporary, filePath);
      return result;
    } finally {
      fs.closeSync(lock);
      fs.rmSync(lockPath, { force: true });
    }
  }

  private acquireLock(lockPath: string): number {
    for (let attempt = 0; attempt < 1000; attempt++) {
      try {
        const descriptor = fs.openSync(lockPath, 'wx');
        fs.writeFileSync(descriptor, String(process.pid));
        return descriptor;
      } catch (error) {
        if ((error as NodeJS.ErrnoException).code !== 'EEXIST') throw error;
        this.removeAbandonedLock(lockPath);
        Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, 2);
      }
    }
    throw new Error(`Timed out acquiring '${lockPath}'.`);
  }

  private removeAbandonedLock(lockPath: string): void {
    try {
      const owner = Number(fs.readFileSync(lockPath, 'utf8'));
      if (!Number.isInteger(owner) || owner <= 0) return;
      try {
        process.kill(owner, 0);
      } catch (error) {
        if ((error as NodeJS.ErrnoException).code === 'ESRCH') fs.rmSync(lockPath, { force: true });
      }
    } catch (error) {
      if ((error as NodeJS.ErrnoException).code !== 'ENOENT') throw error;
    }
  }

  private readJson<T>(filePath: string, initial: T): T {
    if (!fs.existsSync(filePath)) return structuredClone(initial);
    return JSON.parse(fs.readFileSync(filePath, 'utf8')) as T;
  }

  private emptyCommerce(): CommerceData {
    return { mappings: {}, carts: {}, addresses: {}, inventory: {}, paymentMethods: {}, orderPaymentMethods: {}, reservations: {}, releases: {}, payments: {}, evidence: [] };
  }
}

export { OrderStore };
