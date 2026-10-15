// SPDX-License-Identifier: GPL-3.0-or-later
// Client-side undo / redo. Each entry restores an entity to its state before the edit (notes of one
// pattern row, a step row, the playlist's clips) by sending the full value back. Policy for several
// clients: undo restores what THIS client saw before its edit; concurrent edits by others to the
// same entity are overwritten by that undo (docs/PROTOCOL.md, consistency).
import { Store } from './store';

interface Entry {
  label: string;
  undo: () => void;
  redo: () => void;
}
export const history = new Store({ stack: [] as Entry[], pos: 0 });

export function record(label: string, undo: () => void, redo: () => void) {
  history.set((h) => {
    const stack = h.stack.slice(0, h.pos);
    stack.push({ label, undo, redo });
    if (stack.length > 200) stack.shift();
    return { stack, pos: stack.length };
  });
}
export function undo() {
  const h = history.state;
  if (h.pos === 0) return;
  h.stack[h.pos - 1].undo();
  history.set({ pos: h.pos - 1 });
}
export function redo() {
  const h = history.state;
  if (h.pos >= h.stack.length) return;
  h.stack[h.pos].redo();
  history.set({ pos: h.pos + 1 });
}
/** forget every entry (after the whole project was replaced: they refer to what is gone) */
export const clearHistory = () => history.set({ stack: [], pos: 0 });
export const canUndo = () => history.state.pos > 0;
export const canRedo = () => history.state.pos < history.state.stack.length;
export const undoLabel = () => (history.state.pos > 0 ? history.state.stack[history.state.pos - 1].label : '');
