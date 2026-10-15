// SPDX-License-Identifier: GPL-3.0-or-later
import { render } from 'preact';

import './app/theme.css';
import { App } from './app/App';
import { connect } from './app/protocol';
import { catalog, conn, live, proj, samples, ui, userPresets } from './app/state';

// read-only handle for browser tests and debugging (window.__daw)
(window as unknown as Record<string, unknown>).__daw = { conn, proj, catalog, live, ui, samples, userPresets };
connect();
render(<App />, document.getElementById('app')!);
