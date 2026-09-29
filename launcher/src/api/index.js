// Inside Tauri the UI talks to the launcher backend; in a normal browser it runs
// against sample data so the interface can be previewed without Windows.
export const native = typeof window !== 'undefined' && '__TAURI_INTERNALS__' in window;

const backend = native ? await import('./tauri.js') : await import('./mock.js');

export const call = backend.call;
export const onChange = backend.onChange;
export const onLaunchError = backend.onLaunchError;
export const windowControls = backend.windowControls;
