const assert = require('node:assert/strict');
const { getHeapSnapshot } = require('node:v8');

// Inspect ownership without calling or replacing the pump's private methods.
async function assertClaimState(batch, record) {
  const marker = 'mesh-single-claim-receive-batch';
  batch.claimStateMarker = marker;
  batch.undefinedProbe = undefined;
  batch.recordProbe = record;
  const chunks = [];
  try {
    for await (const chunk of getHeapSnapshot()) chunks.push(chunk);
    const snapshot = JSON.parse(Buffer.concat(chunks).toString());
    const { nodes, edges, strings } = snapshot;
    const fields = snapshot.snapshot.meta.node_fields;
    const edgeFields = snapshot.snapshot.meta.edge_fields;
    const nodeSize = fields.length;
    const edgeSize = edgeFields.length;
    const edgeCounts = fields.indexOf('edge_count');
    const edgeName = edgeFields.indexOf('name_or_index');
    const edgeTarget = edgeFields.indexOf('to_node');
    const properties = new Map();
    let edgeOffset = 0;
    for (let index = 0; index < nodes.length; index += nodeSize) {
      const props = new Map();
      const end = edgeOffset + nodes[index + edgeCounts] * edgeSize;
      for (; edgeOffset < end; edgeOffset += edgeSize) {
        props.set(strings[edges[edgeOffset + edgeName]], edges[edgeOffset + edgeTarget]);
      }
      properties.set(index, props);
    }
    const batches = [...properties].filter(([, props]) => {
      const target = props.get('claimStateMarker');
      return target !== undefined && strings[nodes[target + fields.indexOf('name')]] === marker;
    });
    assert.equal(batches.length, 1);
    const [batchIndex, probes] = batches[0];
    const states = [...properties.values()].filter((props) => props.get('suppliedReceiveBatch') === batchIndex);
    assert.equal(states.length, 1, 'one claim state belongs to the shared worker batch');
    const state = states[0];
    if (record !== undefined) {
      // Compare the array element with the exact record retained by the probe.
      const recordsIndex = state.get('records');
      let offset = 0;
      for (let index = 0; index < recordsIndex; index += nodeSize) offset += nodes[index + edgeCounts] * edgeSize;
      const end = offset + nodes[recordsIndex + edgeCounts] * edgeSize;
      let firstRecord;
      for (; offset < end; offset += edgeSize) {
        const type = snapshot.snapshot.meta.edge_types[0][edges[offset]];
        if (type === 'element' && edges[offset + edgeName] === 0) firstRecord = edges[offset + edgeTarget];
      }
      assert.equal(firstRecord, probes.get('recordProbe'));
    } else {
      for (const key of ['records', 'owner', 'claim', 'claimPermit']) {
        assert.equal(state.get(key), probes.get('undefinedProbe'), `${key} must be cleared while idle`);
      }
    }
  } finally {
    delete batch.claimStateMarker;
    delete batch.undefinedProbe;
    delete batch.recordProbe;
  }
}

module.exports = { assertClaimState };
