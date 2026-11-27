const golden = require('../../../../../runtime/protocol/golden/authority-aggregate-relocation-v1.json');

// Other language jobs still use the shared fixture for the old completion schema.
module.exports = golden.cases.map((vector) => {
  if (vector.expect !== 'accept' || vector.aggregate.relocation?.completion === undefined)
    return vector;
  const aggregate = structuredClone(vector.aggregate);
  delete aggregate.relocation.completion;
  return { ...vector, aggregate, name: vector.name.replace(/-with-completion$/, '') };
});
