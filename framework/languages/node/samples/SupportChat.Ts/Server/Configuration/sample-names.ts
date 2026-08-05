const SampleNames = {
  apiChannel: 'supportchat.api',
  sessionStreamNode: 'supportchat-session-stream',
  conversationSpotMesh: 'supportchat-conversations',
  meshName: 'supportchat-conversations',
  conversationSpotType: 'supportchat.conversation',
  supportActorType: 'support.user',
  conversationIdMetadataKey: 'conversation-id'
} as const;

const SampleTimings = {
  requestTimeout: 5000,
  idleTimeout: 3000,
  closeGraceTimeout: 1000,
  clientTimeout: 10000
} as const;

export {
  SampleNames,
  SampleTimings
};
