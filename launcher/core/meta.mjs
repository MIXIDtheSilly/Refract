// Native SSO protocol adapted from RiftLift meta_auth.py (GPL-3.0-or-later).
// See THIRD_PARTY_NOTICES.md for source revision and API references.
import { createHash, timingSafeEqual } from 'node:crypto';

const FRL_APP = '512466987071624';
const CLIENT = `FRL|${FRL_APP}|01d4a1f7fd0682aea7ee8ae987704d63`;
const META = 'https://meta.graph.meta.com';
const GRAPH = 'https://graph.oculus.com/graphql';
export const nodes = value => Array.isArray(value) ? value : value?.nodes ?? value?.edges?.map(e => e.node) ?? [];
export function appId(value) {
  if (/^\d{5,30}$/.test(String(value))) return String(value);
  let url; try { url = new URL(value); } catch { throw new Error('Enter a Meta Quest store URL or app ID.'); }
  if (url.protocol !== 'https:' || !['www.meta.com', 'meta.com', 'www.oculus.com', 'oculus.com'].includes(url.hostname)) throw new Error('Use a Meta Quest store URL.');
  const id = url.pathname.match(/\/(\d{5,30})(?:\/|$)/)?.[1];
  if (!id || /\/rift\//i.test(url.pathname)) throw new Error('Choose a Quest app, not a Rift app.');
  return id;
}
export function questApp(item) {
  const platform = String(item?.platform ?? item?.latest_supported_binary?.platform ?? '').toUpperCase();
  const questDevices = (item?.supported_hmd_platforms ?? item?.supported_hmd_types ?? item?.targeted_devices ?? []).some(p => /QUEST|MONTEREY|HOLLYWOOD|SEACLIFF|EUREKA|PANTHER/i.test(p));
  return platform === 'ANDROID_6DOF' || (platform === 'ANDROID' && questDevices) ||
    (!platform && questDevices && item?.latest_supported_binary?.__typename === 'AndroidBinary');
}
export function card(item, owned = false) {
  return { id: String(item.id), name: item.display_name || item.displayName || item.name || `Quest app ${item.id}`,
    description: item.display_long_description || item.display_short_description || '',
    image: item.cover_landscape_image?.uri || item.cover_square_image?.uri || '',
    genres: item.genre_names || [], publisher: item.publisher_name || item.developer_name || '',
    platform: item.platform || '', price: item.current_offer?.price?.formatted || '', owned, source: 'meta' };
}
// retries: how many more times a request is sent after Meta's server fails it (HTTP 5xx), which it often does
// for one request and not the next. Only for queries; sign-in steps are sent once.
export async function post(url, fields, request = fetch, retries = 0, wait = attempt => new Promise(resolve => setTimeout(resolve, 1500 * attempt))) {
  for (let attempt = 1; ; attempt++) {
    try {
      return await read(await request(url, { method: 'POST', body: new URLSearchParams(fields),
        signal: AbortSignal.timeout(30000), headers: { Accept: 'application/json' } }));
    } catch (error) {
      if (!error.serverError || attempt > retries) throw error;
      await wait(attempt);
    }
  }
}
async function read(response) {
  if (!response.ok) {
    // Meta's error body names the reason (an expired session is code 190); read it, never show it.
    const code = await response.json().then(data => data?.error?.code ?? data?.errors?.[0]?.code, () => undefined);
    if (code === 190 || response.status === 401) throw new Error('Your Meta session expired. Sign in again.');
    if (response.status >= 500) throw Object.assign(new Error(`Meta's server could not answer this request (HTTP ${response.status}${code ? `, code ${code}` : ''}). Try again later; signing in again does not help with this one.`), { serverError: true });
    throw new Error(`Meta request failed (${response.status}). Try signing in again.`);
  }
  const raw = await response.text();
  if (raw.length > 16 * 1024 * 1024) throw new Error('Meta returned an oversized response.');
  let data; try { data = JSON.parse(raw); } catch { throw new Error('Meta returned an unexpected response. Try again later.'); }
  if (data.error || data.errors?.length) {
    // Server messages can echo credentials/queries. Do not expose raw responses.
    const code = data.error?.code ?? data.errors?.[0]?.code;
    throw new Error(code === 190 ? 'Your Meta session expired. Sign in again.' : `Meta could not complete this request${code ? ` (code ${code})` : ''}. Its API may have changed.`);
  }
  return data;
}
export class MetaAuth {
  constructor(request = fetch) { this.request = request; }
  async begin() {
    const response = await post(`${META}/webview_tokens_query`, { access_token: CLIENT }, this.request);
    if (!response.native_sso_token || !response.native_sso_etoken) throw new Error('Meta did not return a sign-in challenge.');
    this.challenge = response.native_sso_token;
    this.expires = Date.now() + 10 * 60 * 1000;
    return `https://auth.meta.com/native_sso/confirm?${new URLSearchParams({ native_app_id: FRL_APP, source_app_id: FRL_APP, native_sso_etoken: response.native_sso_etoken, utm_source: 'refract' })}`;
  }
  async complete(callback) {
    const challenge = this.challenge;
    if (!challenge || Date.now() > this.expires) throw new Error('Sign-in expired. Please start again.');
    const url = new URL(callback);
    const expected = createHash('sha256').update(challenge).digest('hex').slice(0, 16);
    const token = url.searchParams.get('token') || '';
    if (!['oculus:', 'oculus-client:'].includes(url.protocol) || token.length !== expected.length || !timingSafeEqual(Buffer.from(token), Buffer.from(expected))) throw new Error('Sign-in callback did not match this session.');
    const blob = url.searchParams.get('blob');
    if (!blob || blob.length > 2 * 1024 * 1024) throw new Error('Invalid sign-in response.');
    this.challenge = null;
    const decrypted = await post(`${META}/webview_blobs_decrypt`, { access_token: CLIENT, blob, request_token: challenge }, this.request);
    const result = await post(`${META}/graphql`, { access_token: decrypted.access_token,
      doc_id: '24112177345042346', variables: JSON.stringify({ app_id: '1582076955407037' }) }, this.request);
    const access = result.data?.xfr_create_profile_token?.profile_tokens?.[0]?.access_token;
    if (typeof access !== 'string' || !access) throw new Error('Meta did not return a Quest account token.');
    return access;
  }
}

export class QuestStore {
  constructor(token, request = fetch, wait) { this.token = token; this.request = request; this.wait = wait; }
  async search(text) {
    if (typeof text !== 'string' || text.trim().length < 2 || text.length > 160) throw new Error('Enter at least two characters to search the Quest store.');
    const result = await post('https://www.meta.com/ocapi/graphql', { doc_id: '24633449332970329',
      variables: JSON.stringify({ query: text.trim(), hmdType: 'MONTEREY', firstSearchResultItems: 50 }) }, this.request);
    const categories = result.data?.viewer?.contextual_search?.all_category_results;
    if (!Array.isArray(categories)) throw new Error('The Meta storefront is unavailable. Try again later.');
    return categories.flatMap(c => nodes(c.search_results)).map(n => n.target_object)
      .filter(i => i?.__typename === 'Application').map(i => card(i));
  }
  async query(doc, variables = {}) {
    if (!this.token) throw new Error('Sign in to Meta first.');
    return post(GRAPH, { access_token: this.token, ...(/^\d+$/.test(doc) ? { doc_id: doc } : { doc }), variables: JSON.stringify(variables) }, this.request, 2, this.wait);
  }
  async artwork(id) {
    id = appId(id);
    const result = await post('https://www.meta.com/ocapi/graphql', {
      doc_id: '6549406941839522', variables: JSON.stringify({ itemId: id, hmdType: 'MONTEREY' })
    }, this.request);
    const item = result.data?.item;
    if (String(item?.id) !== id) throw new Error('Meta did not return the requested artwork.');
    // Listing metadata only: never infer ownership from this public response.
    return item.cover_landscape_image?.uri || item.hero?.uri || item.cover_square_image?.uri || item.icon_image?.uri || '';
  }
  async library() {
    const result = await this.query('4850747515044496');
    const viewer = result.data?.viewer;
    const connection = viewer?.user?.active_entitlements ?? viewer?.active_entitlements;
    if (!connection) throw new Error('Meta did not return your library. Try signing in again.');
    // Never silently treat Rift entitlements as Quest ownership.
    return { games: nodes(connection).map(e => e.item).filter(questApp).map(i => card(i, true)),
      partial: Boolean(connection.page_info?.has_next_page) };
  }
  // The library query only returns the user's id; name and picture come from the REST node.
  async profile() {
    if (!this.token) throw new Error('Sign in to Meta first.');
    const me = await read(await this.request('https://graph.oculus.com/me?fields=alias,display_name,profile_url', {
      signal: AbortSignal.timeout(30000), headers: { Accept: 'application/json', Authorization: `OAuth ${this.token}` } }));
    let image = '';
    try {
      const url = new URL(me.profile_url);
      // Only hosts the UI's img-src allows. The signed URL expires within days, so it is never stored.
      if (url.protocol === 'https:' && /\.(oculuscdn\.com|fbcdn\.net)$/.test(url.hostname)) image = url.href;
    } catch { /* No picture. */ }
    return { name: String(me.alias || me.display_name || ''), image };
  }
  async details(id) {
    const result = await this.query('6549406941839522', { itemId: appId(id), hmdType: 'EUREKA' });
    const item = result.data?.item ?? result.data?.node;
    if (!item || !questApp(item)) throw new Error('This listing did not return a Quest build.');
    return card(item);
  }
  async builds(id) {
    id = appId(id);
    // The version list fails on Meta's side for some apps (HTTP 500, every time). The store listing still names
    // the current release, so the game downloads; only older versions are missing then.
    let failed = null;
    const item = await this.query('2885322071572384', { applicationID: id }).then(result => result.data?.node, error => { failed = error; return null; });
    let application = item, released;
    if (!questApp(application)) {
      const listing = await this.query('6549406941839522', { itemId: id, hmdType: 'EUREKA' }).catch(error => { throw failed || error; });
      application = listing.data?.item;
      if (failed && String(application?.id) !== id) throw failed;
      if (String(application?.id) !== id || !questApp(application)) throw new Error('Meta did not return Quest platform information for this app.');
      released = application.latest_supported_binary;
    }
    const builds = nodes(item?.primary_binaries ?? item?.binaries).filter(b =>
      b.platform !== 'PC' && b.__typename !== 'RiftBinary' &&
      (questApp(b) || (questApp(application) && (!b.__typename || b.__typename === 'AndroidBinary'))));
    builds.sort((a, b) => Number(b.version_code ?? b.versionCode ?? 0) - Number(a.version_code ?? a.versionCode ?? 0));
    if (released?.id) {
      const index = builds.findIndex(b => String(b.id) === String(released.id));
      const current = index >= 0 ? builds.splice(index, 1)[0] : released;
      builds.unshift({ ...current, ...released });
    }
    if (!builds.length) throw failed || new Error('Meta returned no downloadable Android versions for this app.');
    return builds;
  }
  async plan(id, binaryId) {
    id = appId(id);
    const builds = await this.builds(id);
    const selected = binaryId ? builds.find(b => String(b.id) === String(binaryId)) : builds[0];
    if (!selected) throw new Error('Choose an available Quest build.');
    const result = await this.query('4734929166632773', { binaryID: String(selected.id) })
      .catch(error => { throw error.serverError ? Object.assign(new Error(`Meta could not list this build's files. ${error.message}`), { serverError: true }) : error; });
    const binary = { ...selected, ...result.data?.node };
    if (binary.platform === 'PC' || !binary.package_name || (binary.binary_application?.id && String(binary.binary_application.id) !== id)) throw new Error('The selected build is not an Android APK for this app.');
    const assets = nodes(binary.asset_files);
    if (Number(binary.asset_files?.count || 0) > assets.length) throw new Error('Meta returned an incomplete asset list. Download was not started.');
    const files = [{ id: String(binary.id), name: 'base.apk', uri: binary.uri, size: Number(binary.size || 0), kind: 'apk' }];
    if (binary.obb_binary?.id) files.push({ id: String(binary.obb_binary.id), name: binary.obb_binary.file_name, uri: binary.obb_binary.uri, size: Number(binary.obb_binary.size || 0), kind: 'obb' });
    for (const asset of assets) {
      // Optional store DLC is handled separately by the entitlement-checked add-on flow.
      if (asset.is_required === false || asset.iap_item) continue;
      if (!asset.file_name) throw new Error('Meta omitted a content filename. Download was not started.');
      if (files.some(f => (asset.id && f.id === String(asset.id)) || f.name === asset.file_name)) continue;
      files.push({ id: asset.id ? String(asset.id) : '', name: asset.file_name, uri: asset.uri,
        size: Number(asset.size || 0), kind: asset.file_name.endsWith('.obb') ? 'obb' : 'asset' });
    }
    return { appId: id, binaryId: String(binary.id), package: binary.package_name,
      version: binary.version || '', files };
  }
  async dlc(id) {
    const result = await this.query('3853229151363174', { id: appId(id), first: 200, last: null, after: null, before: null, forward: true });
    const app = result.data?.node;
    if (!app) throw new Error('Meta did not return add-on information.');
    // Meta now returns the add-on list on the current build rather than the app node.
    const binary = app.latest_supported_binary;
    const listed = nodes(app.firstIapItems ?? app.iap_items ?? binary?.firstIapItems ?? binary?.iap_items).filter(Boolean);
    const entitled = nodes(app.active_dlc_entitlements).map(e => e.item).filter(Boolean);
    const owned = new Set(entitled.map(i => String(i.id)));
    // Ownership comes only from Meta's per-viewer flag; an owned bundle grants each item it contains.
    for (const i of listed) if (i.is_viewer_entitled === true) {
      owned.add(String(i.id));
      for (const part of nodes(i.bundle_items)) if (part?.id) owned.add(String(part.id));
    }
    // Bundles only group add-ons; their contents are listed and downloaded individually.
    const items = [...listed.filter(i => i.__typename !== 'AppItemBundle'), ...entitled];
    return [...new Map(items.filter(Boolean).map(i => [String(i.id), i])).values()].map(i => ({
      id: String(i.id), name: i.display_name || i.sku || 'Add-on', owned: owned.has(String(i.id)),
      files: [i.latest_supported_asset_file, ...nodes(i.asset_files)].filter(f => f?.id || f?.uri).map(f => ({
        id: String(f.id || ''), uri: f.uri, name: f.file_name || i.file_name, size: Number(f.size || 0), kind: 'asset' }))
    }));
  }
  fileUrl(file) {
    if (file.uri) {
      const url = new URL(file.uri);
      // Metadata returns a regional securecdn endpoint without authentication.
      // Attach the token only to Meta's binary download endpoint, never to art
      // URLs or arbitrary hosts. Preserve already-signed asset URLs unchanged.
      if (url.protocol === 'https:' && /^securecdn(?:-[a-z0-9-]+)?\.oculus\.com$/.test(url.hostname) && url.pathname === '/binaries/download/') {
        url.searchParams.set('access_token', this.token);
      }
      return url.href;
    }
    if (!/^\d+$/.test(file.id)) throw new Error('Meta returned an invalid asset ID.');
    return `https://securecdn.oculus.com/binaries/download/?${new URLSearchParams({ id: file.id, access_token: this.token })}`;
  }
  async downloadUrl(file) {
    // Native SSO yields a PC profile token. Quest delivery requires a separate
    // application-scoped token; retain the original token for library queries.
    // Share the exchange across all APK/OBB/DLC files in this download job.
    if (!this.questDelivery) {
      this.questDelivery = (async () => {
        if (!this.token) throw new Error('Sign in to Meta first.');
        const url = new URL('https://graph.oculus.com/authenticate_application');
        url.searchParams.set('app_id', '1481000308606657');
        url.searchParams.set('access_token', this.token);
        const result = await post(url.href, {}, this.request);
        if (typeof result.access_token !== 'string' || !result.access_token) throw new Error('Meta did not return a Quest download token. Try reconnecting Meta.');
        return new QuestStore(result.access_token, this.request);
      })().catch(error => { this.questDelivery = null; throw error; });
    }
    return (await this.questDelivery).fileUrl(file);
  }
}
