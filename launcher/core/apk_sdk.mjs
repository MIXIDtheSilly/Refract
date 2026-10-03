// Which VR SDK a Quest APK is built on, from the native libraries it carries. Refract runs OpenXR games; games on
// Meta's older VrApi SDK load libvrapi.so (the "VrApi Loader") and stop at start. Engines that support both ship
// both loaders and pick OpenXR, so the OpenXR loader decides.
import path from 'node:path';
import { allowedDownload, fetchFile } from './download.mjs';

export function vrSdk(names = []) {
  const libraries = new Set(names.filter(n => /^lib\/[^/]+\/[^/]+\.so$/.test(n)).map(n => path.posix.basename(n)));
  if ([...libraries].some(n => /^libopenxr_loader/.test(n))) return 'openxr';
  if (libraries.has('libvrapi.so')) return 'vrapi';
  return 'unknown';
}

// The file names in a zip (an APK) on Meta's CDN, read without downloading it: the zip's directory sits at its
// end, so one ranged request finds it and at most one more reads it. null when the server does not do ranges.
const TAIL = 64 * 1024 + 22, MAX_DIRECTORY = 32 * 1024 * 1024;
export async function remoteZipNames(url, { request = fetch, validate = allowedDownload, signal } = {}) {
  const get = async range => {
    const response = await fetchFile(url, { headers: { Range: `bytes=${range}` }, signal: AbortSignal.any([signal || new AbortController().signal, AbortSignal.timeout(60000)]) }, request, validate);
    if (response.status !== 206) { await response.body?.cancel(); return null; }
    const total = Number(response.headers.get('content-range')?.match(/\/(\d+)$/)?.[1]);
    const data = Buffer.from(await response.arrayBuffer());
    return { data, total };
  };
  const tail = await get(`-${TAIL}`);
  if (!tail || !tail.total) return null;
  const start = tail.total - tail.data.length, buffer = tail.data;
  let end = -1;
  for (let i = buffer.length - 22; i >= 0; i--) if (buffer.readUInt32LE(i) === 0x06054b50) { end = i; break; }
  if (end < 0) throw new Error('The APK has no zip directory.');
  let count = buffer.readUInt16LE(end + 10), size = buffer.readUInt32LE(end + 12), offset = buffer.readUInt32LE(end + 16);
  // Zip64 (APKs over 4 GB): the locator right before the end record points at the larger record.
  if ((count === 0xffff || size === 0xffffffff || offset === 0xffffffff) && end >= 20 && buffer.readUInt32LE(end - 20) === 0x07064b50) {
    const at = Number(buffer.readBigUInt64LE(end - 12)) - start;
    if (at < 0 || buffer.readUInt32LE(at) !== 0x06064b50) throw new Error('The APK has an unreadable zip directory.');
    count = Number(buffer.readBigUInt64LE(at + 32)); size = Number(buffer.readBigUInt64LE(at + 40)); offset = Number(buffer.readBigUInt64LE(at + 48));
  }
  if (size > MAX_DIRECTORY || offset + size > tail.total) throw new Error('The APK has an unreadable zip directory.');
  let directory;
  if (offset >= start) directory = buffer.subarray(offset - start, offset - start + size);
  else {
    const part = await get(`${offset}-${offset + size - 1}`);
    if (!part || part.data.length !== size) return null;
    directory = part.data;
  }
  const names = [];
  for (let at = 0; at + 46 <= directory.length && names.length < count; ) {
    if (directory.readUInt32LE(at) !== 0x02014b50) throw new Error('The APK has an unreadable zip directory.');
    const nameLength = directory.readUInt16LE(at + 28), extra = directory.readUInt16LE(at + 30), comment = directory.readUInt16LE(at + 32);
    names.push(directory.toString('utf8', at + 46, at + 46 + nameLength));
    at += 46 + nameLength + extra + comment;
  }
  return names;
}
