import { render } from 'preact';
import { GamePage } from '../pages/game/game-page';
import { loadClientEndpoints } from '../shared/config/runtime';
import '../shared/ui/theme.css';

const endpoints = await loadClientEndpoints();
render(<GamePage gateway={endpoints.gateway} />, document.getElementById('app')!);
