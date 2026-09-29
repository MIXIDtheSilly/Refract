import { invoke } from '@tauri-apps/api/core';
import { listen } from '@tauri-apps/api/event';
import { getCurrentWindow } from '@tauri-apps/api/window';

export const call = (method, ...args) => invoke('call', { method, args });

function subscribe(name, callback) {
  let stop = null, stopped = false;
  listen(name, event => callback(event.payload)).then(unlisten => { if (stopped) unlisten(); else stop = unlisten; });
  return () => { stopped = true; stop?.(); };
}
export const onChange = callback => subscribe('refract://changed', callback);
export const onLaunchError = callback => {
  const offError = subscribe('refract://launch-error', callback);
  const offExit = subscribe('refract://backend-exit', () => callback('The launcher backend stopped. Restart Refract.'));
  return () => { offError(); offExit(); };
};

const appWindow = getCurrentWindow();
export const windowControls = {
  minimize: () => appWindow.minimize(),
  toggleMaximize: () => appWindow.toggleMaximize(),
  close: () => appWindow.close(),
};
