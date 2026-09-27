const { contextBridge, ipcRenderer } = require('electron');
const channels = ['state', 'sync', 'login', 'logout', 'search', 'add', 'lookup', 'builds', 'download', 'dlc', 'downloadDlc', 'cancel', 'retry', 'import', 'importAssets', 'install', 'patch', 'play', 'stop', 'settings', 'chooseFolder', 'chooseCli', 'openFolder', 'openStore'];
contextBridge.exposeInMainWorld('refract', Object.fromEntries([
  ...channels.map(name => [name, (...args) => ipcRenderer.invoke(`refract:${name}`, ...args)]),
  ['onChange', callback => { const listener = (_event, state) => callback(state); ipcRenderer.on('refract:changed', listener); return () => ipcRenderer.removeListener('refract:changed', listener); }],
  ['onLaunchError', callback => { const listener = (_event, error) => callback(error); ipcRenderer.on('refract:launch-error', listener); return () => ipcRenderer.removeListener('refract:launch-error', listener); }]
]));
