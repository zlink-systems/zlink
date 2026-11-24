function ownerLeaseClaimMutation(request) {
  if (
    !request.mutations.some(
      ({ kind, key }) => kind === 'put' && key.value === 'zlink:v11:owner-counter'
    )
  )
    return undefined;
  return request.mutations.find(
    ({ kind, key }) => kind === 'put' && key.value.startsWith('owner-lease\0')
  );
}

module.exports = { ownerLeaseClaimMutation };
