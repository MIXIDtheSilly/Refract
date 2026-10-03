import test from 'node:test';
import assert from 'node:assert/strict';
import { remoteZipNames, vrSdk } from '../core/apk_sdk.mjs';

// A stored (uncompressed) zip with these files, as an APK would be laid out.
function zip(names) {
  const local = [], central = [];
  let offset = 0;
  for (const name of names) {
    const file = Buffer.from(name), data = Buffer.from(`contents of ${name}`);
    const header = Buffer.alloc(30); header.writeUInt32LE(0x04034b50, 0); header.writeUInt32LE(data.length, 18); header.writeUInt32LE(data.length, 22); header.writeUInt16LE(file.length, 26);
    const entry = Buffer.alloc(46); entry.writeUInt32LE(0x02014b50, 0); entry.writeUInt32LE(data.length, 20); entry.writeUInt32LE(data.length, 24); entry.writeUInt16LE(file.length, 28); entry.writeUInt32LE(offset, 42);
    local.push(header, file, data); central.push(entry, file);
    offset += header.length + file.length + data.length;
  }
  const directory = Buffer.concat(central), end = Buffer.alloc(22);
  end.writeUInt32LE(0x06054b50, 0); end.writeUInt16LE(names.length, 8); end.writeUInt16LE(names.length, 10); end.writeUInt32LE(directory.length, 12); end.writeUInt32LE(offset, 16);
  return Buffer.concat([...local, directory, end]);
}
// A CDN that answers Range requests like Meta's.
const cdn = (file, seen = []) => async (url, { headers }) => {
  seen.push(headers.Range);
  const [, from, to] = headers.Range.match(/^bytes=(\d*)-(\d*)$/);
  const start = from === '' ? Math.max(0, file.length - Number(to)) : Number(from), end = from === '' || to === '' ? file.length - 1 : Number(to);
  return new Response(file.subarray(start, end + 1), { status: 206, headers: { 'content-range': `bytes ${start}-${end}/${file.length}` } });
};

test('the VR SDK comes from the loader libraries; the OpenXR loader wins when both are there', () => {
  assert.equal(vrSdk(['lib/arm64-v8a/libvrapi.so', 'lib/arm64-v8a/libunity.so']), 'vrapi');
  assert.equal(vrSdk(['lib/arm64-v8a/libvrapi.so', 'lib/arm64-v8a/libopenxr_loader.so']), 'openxr');
  assert.equal(vrSdk(['lib/arm64-v8a/libUE4.so', 'lib/arm64-v8a/libopenxr_loader.so']), 'openxr');
  assert.equal(vrSdk(['assets/lib/arm64-v8a/libvrapi.so', 'lib/arm64-v8a/libgame.so']), 'unknown');
  assert.equal(vrSdk(), 'unknown');
});

test('an APK on the CDN is read from its end only: its file list, never its contents', async () => {
  const names = ['AndroidManifest.xml', 'classes.dex', 'lib/arm64-v8a/libvrapi.so', 'lib/arm64-v8a/libunity.so', 'assets/bin/Data/data.unity3d'];
  const file = zip(names), seen = [];
  assert.deepEqual(await remoteZipNames('https://securecdn.oculus.com/binaries/download/?id=1', { request: cdn(file, seen) }), names);
  assert.equal(seen.length, 1);
  // A directory that starts before the last 64 KB needs a second range.
  const big = zip([...Array.from({ length: 2000 }, (_, i) => `assets/a-long-folder-name-for-padding/file-${i}.bin`), 'lib/arm64-v8a/libopenxr_loader.so']);
  seen.length = 0;
  const listed = await remoteZipNames('https://securecdn.oculus.com/binaries/download/?id=1', { request: cdn(big, seen) });
  assert.equal(listed.length, 2001);
  assert.equal(vrSdk(listed), 'openxr');
  assert.equal(seen.length, 2);
  assert.match(seen[1], /^bytes=\d+-\d+$/);
});

test('a server without ranges or a host outside Meta gives no answer', async () => {
  assert.equal(await remoteZipNames('https://securecdn.oculus.com/x', { request: async () => new Response('whole file') }), null);
  await assert.rejects(remoteZipNames('https://example.com/game.apk', { request: async () => assert.fail('requested') }), /Meta delivery/);
});
