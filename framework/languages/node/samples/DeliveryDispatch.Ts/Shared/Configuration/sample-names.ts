const SampleNames = {
  dispatchChannel: 'deliverydispatch.dispatch',
  courierMeshName: 'deliverydispatch.courier',
  customerMeshName: 'deliverydispatch.customer',
  courierStreamNode: 'delivery-courier-stream',
  trackingChannel: 'deliverydispatch.tracking',
  customerStreamNode: 'delivery-customer-stream',
  courierActorType: 'delivery-courier',
  customerActorType: 'delivery-customer'
} as const;

const SampleTimings = {
  requestTimeout: 10000,
  offerDecisionTimeout: 700,
  offerSweepInterval: 50,
  clientTimeout: 30000
} as const;

export {
  SampleNames,
  SampleTimings
};
