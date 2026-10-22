// Unknown-sources methods for the launcher backend. server.mjs merges these into its method table with one line;
// everything else (install, play, importAssets, openFolder) is the normal game flow, which works on these entries as is.
import fs from 'node:fs/promises';
import { diskStatus, entryFor, planFolder, siblingObbs, stampOf } from '../core/sideload.mjs';

// ctx: { state, runtime, persist, getGame, exclusive, absolute, methods } (state and runtime are getters).
export function sideloadMethods(ctx) {
  const unknown = id => { const game = ctx.getGame(id); if (!game.unknownSource) throw new Error('This game is not from an unknown source.'); return game; };

  // Adds or refreshes one inspected APK. A Meta library game is left to the Meta flow, so the two never mix.
  async function add(info, obbs) {
    const existing = ctx.state.data.games.find(g => g.package === info.package);
    if (existing?.source === 'meta') throw new Error(`${existing.name} is in your Meta library. Download it there, or use Import APK on its page.`);
    const apkStat = await fs.stat(info.apk);
    // installedStamp is kept (entryFor spreads the old entry), so a rebuilt APK still shows as not yet installed.
    return ctx.state.put(entryFor(info, obbs, existing, apkStat));
  }

  return {
    // The shell shows the file picker and passes the chosen APK path(s).
    sideloadAdd: (...files) => ctx.exclusive(async () => {
      const added = [], problems = [];
      for (const file of files) {
        try {
          const info = await ctx.runtime.inspect(ctx.absolute(file));
          added.push((await add(info, await siblingObbs(info.apk, info.package))).name);
        } catch (error) { problems.push(`${String(file).split(/[\\/]/).pop()}: ${error.message}`); }
      }
      await ctx.persist();
      return { added, problems };
    }),
    // A folder of builds: every APK in it (newest per package) with the expansion files that belong to it.
    sideloadScan: folder => ctx.exclusive(async () => {
      const { games, problems } = await planFolder(ctx.absolute(folder), apk => ctx.runtime.inspect(apk));
      const added = [];
      for (const { info, obbs } of games) {
        try { added.push((await add(info, obbs)).name); } catch (error) { problems.push(`${info.name}: ${error.message}`); }
      }
      await ctx.persist();
      return { added, problems };
    }),
    // Per game: is the APK gone, rebuilt since the last install, or an expansion file missing? Read-only; the page asks on open and on focus.
    sideloadStatus: async () => Object.fromEntries(await Promise.all(ctx.state.data.games.filter(g => g.unknownSource)
      .map(async g => [g.id, await diskStatus(g)]))),
    // Reads the APK again (a rebuild may change the version or launch activity), then installs it over the old one; saves stay.
    sideloadInstall: async id => {
      const game = unknown(id);
      if (!game.apk) throw new Error('This entry has no APK. Add it again.');
      const info = await ctx.runtime.inspect(game.apk);
      if (info.package !== game.package) throw new Error(`The APK is now ${info.package}, not ${game.package}. Remove this entry and add the new APK.`);
      const obbs = await siblingObbs(game.apk, game.package);
      await ctx.exclusive(async () => { Object.assign(game, entryFor(info, obbs, game, await fs.stat(game.apk))); await ctx.persist(); });
      await ctx.methods.install(id);
      game.installedStamp = await stampOf(game.apk);
      await ctx.persist();
    },
    // Whether the Meta Platform stand-in tells this game its entitlement check passed. Off by default: it is for your own
    // builds that check for an owner; a game with no check does not need it.
    sideloadOwned: async (id, owned) => {
      const game = unknown(id);
      if (typeof owned !== 'boolean') throw new Error('Choose on or off.');
      game.owned = owned; await ctx.persist();
    },
    // Takes the game out of Refract's library. Files stay on disk, and Android keeps the installed copy (Emulator > Storage removes it).
    sideloadRemove: async id => {
      const game = unknown(id);
      if (ctx.runtime.game === game.id) throw new Error('Close the game first.');
      ctx.state.data.games = ctx.state.data.games.filter(g => g !== game);
      await ctx.persist();
    },
  };
}
