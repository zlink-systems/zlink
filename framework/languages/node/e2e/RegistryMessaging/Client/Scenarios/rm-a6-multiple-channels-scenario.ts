// RM-A6: 서로 다른 RouteMesh를 격리한다 시나리오를 검증한다.
import type { ProfileRes, WorkflowRes } from '../../Shared/messages';
import { postJson } from '../../../http-client';
import { ensure, uniqueMarker } from '../Support/scenario-assert';

export async function runRmA6(providerAUrl: string, providerBUrl: string, workflowUrl: string): Promise<void> {
  const profileMarker = uniqueMarker('rm-a6-profile');
  const workflowMarker = uniqueMarker('rm-a6-workflow');

  const profileReply = await postJson<ProfileRes>(providerAUrl, '/profile/request', { value: profileMarker });
  ensure(profileReply.value === `profile:${profileMarker}`, 'RM-A6 profile reply value mismatch.');
  ensure(profileReply.providerRid === 'api-a' || profileReply.providerRid === 'api-b', 'RM-A6 profile request reached an unexpected provider.');

  const workflowReply = await postJson<WorkflowRes>(workflowUrl, '/workflow/request', { value: workflowMarker });
  ensure(workflowReply.value === `workflow:${workflowMarker}`, 'RM-A6 workflow reply value mismatch.');
  ensure(workflowReply.providerRid === 'workflow-a', 'RM-A6 workflow request should reach workflow-a.');

  const providerEvidence = await firstEvidence(providerAUrl, providerBUrl, profileMarker);
  const workflowEvidence = await postJson<string[]>(workflowUrl, '/evidence/wait', { contains: workflowMarker });
  ensure(providerEvidence.some((line) => line.includes('profile-request|') && line.includes(profileMarker)), 'RM-A6 profile evidence missing.');
  ensure(workflowEvidence.some((line) => line.includes('workflow-request|rid=workflow-a') && line.includes(workflowMarker)), 'RM-A6 workflow evidence missing.');
  ensure(!providerEvidence.some((line) => line.includes('workflow-request|') && line.includes(workflowMarker)), 'RM-A6 workflow request was recorded on profile providers.');
  console.log('scenario RM-A6 passed');
}

async function firstEvidence(providerAUrl: string, providerBUrl: string, marker: string): Promise<string[]> {
  return await Promise.race([
    postJson<string[]>(providerAUrl, '/evidence/wait', { contains: marker }),
    postJson<string[]>(providerBUrl, '/evidence/wait', { contains: marker })
  ]);
}
