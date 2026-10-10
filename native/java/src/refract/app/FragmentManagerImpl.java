package refract.app;

import android.app.Fragment;
import android.app.FragmentManager;
import android.app.FragmentTransaction;
import java.util.ArrayList;
import java.util.List;

/**
 * The (deprecated) platform FragmentManager. Fragments are kept by tag/id but get no
 * lifecycle or views; androidx only adds its headless ReportFragment here, and on
 * API 29+ takes lifecycle events from ActivityLifecycleCallbacks instead.
 */
public final class FragmentManagerImpl extends FragmentManager {
    private final ArrayList<Fragment> fragments = new ArrayList<>();
    private final ArrayList<String> tags = new ArrayList<>();
    private final ArrayList<Integer> ids = new ArrayList<>();

    @Override public FragmentTransaction beginTransaction() { return new Transaction(); }
    @Override public boolean executePendingTransactions() { return false; }
    @Override public synchronized Fragment findFragmentById(int id) {
        int i = ids.indexOf(id);
        return i < 0 ? null : fragments.get(i);
    }
    @Override public synchronized Fragment findFragmentByTag(String tag) {
        int i = tag == null ? -1 : tags.indexOf(tag);
        return i < 0 ? null : fragments.get(i);
    }
    @Override public synchronized List<Fragment> getFragments() { return new ArrayList<>(fragments); }
    @Override public int getBackStackEntryCount() { return 0; }
    @Override public boolean isDestroyed() { return false; }
    @Override public boolean isStateSaved() { return false; }

    private synchronized void add(int id, Fragment f, String tag) {
        fragments.add(f);
        tags.add(tag);
        ids.add(id);
    }
    private synchronized void remove(Fragment f) {
        int i = fragments.indexOf(f);
        if (i < 0) return;
        fragments.remove(i);
        tags.remove(i);
        ids.remove(i);
    }

    private final class Transaction extends FragmentTransaction {
        private final ArrayList<Runnable> ops = new ArrayList<>();

        @Override public FragmentTransaction add(Fragment f, String tag) { return add(0, f, tag); }
        @Override public FragmentTransaction add(int id, Fragment f) { return add(id, f, null); }
        @Override public FragmentTransaction add(int id, Fragment f, String tag) {
            ops.add(() -> FragmentManagerImpl.this.add(id, f, tag));
            return this;
        }
        @Override public FragmentTransaction replace(int id, Fragment f) { return replace(id, f, null); }
        @Override public FragmentTransaction replace(int id, Fragment f, String tag) {
            ops.add(() -> {
                Fragment old;
                while ((old = findFragmentById(id)) != null) FragmentManagerImpl.this.remove(old);
                FragmentManagerImpl.this.add(id, f, tag);
            });
            return this;
        }
        @Override public FragmentTransaction remove(Fragment f) {
            ops.add(() -> FragmentManagerImpl.this.remove(f));
            return this;
        }
        @Override public FragmentTransaction hide(Fragment f) { return this; }
        @Override public FragmentTransaction show(Fragment f) { return this; }
        @Override public FragmentTransaction detach(Fragment f) { return this; }
        @Override public FragmentTransaction attach(Fragment f) { return this; }
        @Override public FragmentTransaction setPrimaryNavigationFragment(Fragment f) { return this; }
        @Override public boolean isEmpty() { return ops.isEmpty(); }
        @Override public FragmentTransaction setCustomAnimations(int enter, int exit) { return this; }
        @Override public FragmentTransaction setCustomAnimations(int enter, int exit, int popEnter, int popExit) { return this; }
        @Override public FragmentTransaction setTransition(int transit) { return this; }
        @Override public FragmentTransaction setTransitionStyle(int styleRes) { return this; }
        @Override public FragmentTransaction addToBackStack(String name) { return this; }
        @Override public boolean isAddToBackStackAllowed() { return false; }
        @Override public FragmentTransaction disallowAddToBackStack() { return this; }
        @Override public FragmentTransaction setReorderingAllowed(boolean reorderingAllowed) { return this; }
        @Override public FragmentTransaction runOnCommit(Runnable runnable) {
            ops.add(runnable);
            return this;
        }
        @Override public int commit() {
            for (Runnable r : ops) r.run();
            ops.clear();
            return 0;
        }
        @Override public int commitAllowingStateLoss() { return commit(); }
        @Override public void commitNow() { commit(); }
        @Override public void commitNowAllowingStateLoss() { commit(); }
    }
}
