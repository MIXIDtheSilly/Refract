package android.view;

import android.content.Context;
import android.content.res.Resources;
import android.content.res.TypedArray;
import android.util.AttributeSet;
import java.lang.reflect.Constructor;
import java.util.HashMap;
import org.xmlpull.v1.XmlPullParser;

/**
 * Inflates compiled layouts (refract.app.BinaryXml): views by class name with their
 * (Context, AttributeSet) constructor, &lt;include&gt;, &lt;merge&gt;, factories and android:theme.
 */
public abstract class LayoutInflater {
    private static final int ATTR_THEME = 0x01010000, ATTR_ID = 0x010100d0, ATTR_VISIBILITY = 0x010100dc;
    private static final String[] PREFIXES = {"android.widget.", "android.webkit.", "android.app.", "android.view."};
    private static final HashMap<String, Constructor<? extends View>> sConstructors = new HashMap<>();

    protected final Context mContext;
    private Factory mFactory;
    private Factory2 mFactory2;
    private Factory2 mPrivateFactory;
    private Filter mFilter;

    public interface Factory {
        View onCreateView(String name, Context context, AttributeSet attrs);
    }

    public interface Factory2 extends Factory {
        View onCreateView(View parent, String name, Context context, AttributeSet attrs);
    }

    public interface Filter {
        boolean onLoadClass(Class clazz);
    }

    protected LayoutInflater(Context context) { mContext = context; }

    protected LayoutInflater(LayoutInflater original, Context newContext) {
        mContext = newContext;
        mFactory = original.mFactory;
        mFactory2 = original.mFactory2;
        mPrivateFactory = original.mPrivateFactory;
        mFilter = original.mFilter;
    }

    public static LayoutInflater from(Context context) {
        LayoutInflater li = (LayoutInflater) context.getSystemService(Context.LAYOUT_INFLATER_SERVICE);
        if (li == null) throw new AssertionError("LayoutInflater not found.");
        return li;
    }

    public abstract LayoutInflater cloneInContext(Context newContext);

    public Context getContext() { return mContext; }
    public final Factory getFactory() { return mFactory; }
    public final Factory2 getFactory2() { return mFactory2; }
    public void setFactory(Factory factory) {
        if (mFactory != null) throw new IllegalStateException("A factory has already been set on this LayoutInflater");
        mFactory = factory;
    }
    public void setFactory2(Factory2 factory) {
        if (mFactory != null) throw new IllegalStateException("A factory has already been set on this LayoutInflater");
        mFactory = mFactory2 = factory;
    }
    public void setPrivateFactory(Factory2 factory) { mPrivateFactory = factory; }
    public Filter getFilter() { return mFilter; }
    public void setFilter(Filter filter) { mFilter = filter; }

    public View inflate(int resource, ViewGroup root) { return inflate(resource, root, root != null); }
    public View inflate(XmlPullParser parser, ViewGroup root) { return inflate(parser, root, root != null); }
    public View inflate(int resource, ViewGroup root, boolean attachToRoot) {
        return inflate(mContext.getResources().getLayout(resource), root, attachToRoot);
    }

    public View inflate(XmlPullParser parser, ViewGroup root, boolean attachToRoot) {
        try {
            AttributeSet attrs = (AttributeSet) parser;
            int type;
            while ((type = parser.next()) != XmlPullParser.START_TAG && type != XmlPullParser.END_DOCUMENT) {}
            if (type != XmlPullParser.START_TAG)
                throw new InflateException(parser.getPositionDescription() + ": No start tag found!");
            String name = parser.getName();
            if ("merge".equals(name)) {
                if (root == null || !attachToRoot)
                    throw new InflateException("<merge /> can be used only with a valid ViewGroup root and attachToRoot=true");
                rInflate(parser, root, mContext, attrs, false);
                return root;
            }
            View temp = createViewFromTag(root, name, mContext, attrs);
            ViewGroup.LayoutParams params = null;
            if (root != null) {
                params = root.generateLayoutParams(attrs);
                if (!attachToRoot) temp.setLayoutParams(params);
            }
            rInflate(parser, temp, temp.getContext(), attrs, true);
            if (root != null && attachToRoot) {
                root.addView(temp, params);
                return root;
            }
            return temp;
        } catch (InflateException e) {
            throw e;
        } catch (Exception e) {
            throw new InflateException(parser.getPositionDescription() + ": " + e, e);
        }
    }

    void rInflate(XmlPullParser parser, View parent, Context context, AttributeSet attrs, boolean finishInflate)
            throws Exception {
        int depth = parser.getDepth(), type;
        while (((type = parser.next()) != XmlPullParser.END_TAG || parser.getDepth() > depth)
                && type != XmlPullParser.END_DOCUMENT) {
            if (type != XmlPullParser.START_TAG) continue;
            String name = parser.getName();
            if ("requestFocus".equals(name) || "tag".equals(name)) {
                skip(parser);
            } else if ("include".equals(name)) {
                parseInclude(parser, context, parent, attrs);
            } else if ("merge".equals(name)) {
                throw new InflateException("<merge /> must be the root element");
            } else {
                View view = createViewFromTag(parent, name, context, attrs);
                ViewGroup group = (ViewGroup) parent;
                ViewGroup.LayoutParams params = group.generateLayoutParams(attrs);
                rInflate(parser, view, view.getContext(), attrs, true);
                group.addView(view, params);
            }
        }
        if (finishInflate) parent.onFinishInflate();
    }

    private static void skip(XmlPullParser parser) throws Exception {
        int depth = parser.getDepth(), type;
        while (((type = parser.next()) != XmlPullParser.END_TAG || parser.getDepth() > depth)
                && type != XmlPullParser.END_DOCUMENT) {}
    }

    private void parseInclude(XmlPullParser parser, Context context, View parent, AttributeSet attrs) throws Exception {
        int layout = attrs.getAttributeResourceValue(null, "layout", 0);
        if (layout == 0) {
            String value = attrs.getAttributeValue(null, "layout");
            if (value != null && value.startsWith("?")) {
                android.util.TypedValue tv = new android.util.TypedValue();
                if (context.getTheme().resolveAttribute(Integer.parseInt(value.substring(1)), tv, true)) layout = tv.resourceId;
            }
            if (layout == 0) throw new InflateException("You must specify a valid layout reference in <include />");
        }
        int id = attrs.getIdAttributeResourceValue(View.NO_ID);
        int visibility = visibilityOf(attrs);
        XmlPullParser child = context.getResources().getLayout(layout);
        AttributeSet childAttrs = (AttributeSet) child;
        int type;
        while ((type = child.next()) != XmlPullParser.START_TAG && type != XmlPullParser.END_DOCUMENT) {}
        if (type == XmlPullParser.START_TAG) {
            String name = child.getName();
            if ("merge".equals(name)) {
                rInflate(child, parent, context, childAttrs, false);
            } else {
                View view = createViewFromTag(parent, name, context, childAttrs);
                ViewGroup group = (ViewGroup) parent;
                ViewGroup.LayoutParams params = group.generateLayoutParams(childAttrs);
                rInflate(child, view, view.getContext(), childAttrs, true);
                if (id != View.NO_ID) view.setId(id);
                if (visibility >= 0) view.setVisibility(visibility);
                group.addView(view, params);
            }
        }
        skip(parser);
    }

    private static int visibilityOf(AttributeSet attrs) {
        for (int i = 0; i < attrs.getAttributeCount(); ++i)
            if (attrs.getAttributeNameResource(i) == ATTR_VISIBILITY)
                return switch (attrs.getAttributeIntValue(i, 0)) {
                    case 1 -> View.INVISIBLE;
                    case 2 -> View.GONE;
                    default -> View.VISIBLE;
                };
        return -1;
    }

    View createViewFromTag(View parent, String name, Context context, AttributeSet attrs) throws Exception {
        if ("view".equals(name)) name = attrs.getAttributeValue(null, "class");
        for (int i = 0; i < attrs.getAttributeCount(); ++i) {
            if (attrs.getAttributeNameResource(i) == ATTR_THEME) {
                int theme = attrs.getAttributeResourceValue(i, 0);
                if (theme != 0) context = new ContextThemeWrapper(context, theme);
            }
        }
        View view = null;
        if (mFactory2 != null) view = mFactory2.onCreateView(parent, name, context, attrs);
        else if (mFactory != null) view = mFactory.onCreateView(name, context, attrs);
        if (view == null && mPrivateFactory != null) view = mPrivateFactory.onCreateView(parent, name, context, attrs);
        if (view == null) {
            view = name.indexOf('.') < 0 ? onCreateView(context, parent, name, attrs)
                                          : createView(context, name, null, attrs);
        }
        // What View's constructor reads from the XML on Android.
        if (view.getId() == View.NO_ID) {
            int id = attrs.getIdAttributeResourceValue(View.NO_ID);
            if (id == View.NO_ID)
                for (int i = 0; i < attrs.getAttributeCount(); ++i)
                    if (attrs.getAttributeNameResource(i) == ATTR_ID) id = attrs.getAttributeResourceValue(i, View.NO_ID);
            if (id != View.NO_ID) view.setId(id);
        }
        int visibility = visibilityOf(attrs);
        if (visibility >= 0 && view.getVisibility() != visibility) view.setVisibility(visibility);
        return view;
    }

    public final View createView(String name, String prefix, AttributeSet attrs) throws ClassNotFoundException {
        return createView(mContext, name, prefix, attrs);
    }

    public final View createView(Context viewContext, String name, String prefix, AttributeSet attrs)
            throws ClassNotFoundException {
        String cls = prefix != null ? prefix + name : name;
        Constructor<? extends View> ctor;
        synchronized (sConstructors) {
            ctor = sConstructors.get(cls);
        }
        try {
            if (ctor == null) {
                ClassLoader loader = viewContext.getClassLoader();
                Class<? extends View> c = Class.forName(cls, false, loader != null ? loader : LayoutInflater.class.getClassLoader())
                        .asSubclass(View.class);
                if (mFilter != null && !mFilter.onLoadClass(c))
                    throw new InflateException("Class not allowed to be inflated " + cls);
                ctor = c.getConstructor(Context.class, AttributeSet.class);
                ctor.setAccessible(true);
                synchronized (sConstructors) {
                    sConstructors.put(cls, ctor);
                }
            }
            return ctor.newInstance(viewContext, attrs);
        } catch (ClassNotFoundException e) {
            throw e;
        } catch (java.lang.reflect.InvocationTargetException e) {
            throw new InflateException(attrs.getPositionDescription() + ": Error inflating class " + cls, e.getCause());
        } catch (ReflectiveOperationException | ClassCastException e) {
            throw new InflateException(attrs.getPositionDescription() + ": Error inflating class " + cls, e);
        }
    }

    protected View onCreateView(String name, AttributeSet attrs) throws ClassNotFoundException {
        for (String prefix : PREFIXES) {
            try {
                return createView(name, prefix, attrs);
            } catch (ClassNotFoundException e) {
                // next prefix
            }
        }
        throw new ClassNotFoundException(name);
    }

    protected View onCreateView(View parent, String name, AttributeSet attrs) throws ClassNotFoundException {
        return onCreateView(name, attrs);
    }

    public View onCreateView(Context viewContext, View parent, String name, AttributeSet attrs)
            throws ClassNotFoundException {
        for (String prefix : PREFIXES) {
            try {
                return createView(viewContext, name, prefix, attrs);
            } catch (ClassNotFoundException e) {
                // next prefix
            }
        }
        throw new ClassNotFoundException(name);
    }
}
