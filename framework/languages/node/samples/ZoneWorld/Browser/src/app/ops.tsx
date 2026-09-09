import { render } from 'preact';
import { OpsPage } from '../pages/ops/ops-page';
import { loadClientEndpoints } from '../shared/config/runtime';
import '../shared/ui/theme.css';

const endpoints = await loadClientEndpoints();
render(<OpsPage ops={endpoints.ops} />, document.getElementById('app')!);
