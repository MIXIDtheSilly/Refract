// Build tool for the Java side of refract_native (run with `java ShimBuilder.java ...`).
//
//   base  <android.jar> <out.jar>
//       The Android SDK's android.jar with every "Stub!" body replaced by a default
//       return (0/false/null/void); constructors keep their super() call. This gives
//       the full Android API surface as no-ops.
//   merge <base.jar> <overlay-classes-dir> <out.jar>
//       Adds Refract's real implementations: for a class present in both, the overlay
//       class wins and keeps every base member it doesn't define.
//   app   <in.jar> <out.jar>
//       Rewrites an app's converted bytecode: System.loadLibrary/load go to the guest
//       linker (refract.Runtime) and guest file paths given to java.io map to host paths.
import java.io.*;
import java.lang.classfile.*;
import java.lang.reflect.AccessFlag;
import java.lang.classfile.attribute.*;
import java.lang.classfile.constantpool.*;
import java.lang.classfile.instruction.*;
import java.lang.constant.*;
import java.nio.file.*;
import java.util.*;
import java.util.jar.*;
import java.util.zip.*;

public class ShimBuilder {
    // Stack maps are regenerated where code changes; that needs the class hierarchy
    // of everything involved (framework, firmware, overlay and app classes + JDK).
    static final Map<String, byte[]> hierarchy = new HashMap<>();
    static ClassFile CF = ClassFile.of();
    static ClassFile CF_GEN = ClassFile.of(ClassFile.StackMapsOption.GENERATE_STACK_MAPS);

    static void addHierarchy(Path jarOrDir) throws IOException {
        if (Files.isDirectory(jarOrDir)) {
            try (var walk = Files.walk(jarOrDir)) {
                for (Path p : (Iterable<Path>) walk::iterator)
                    if (p.toString().endsWith(".class"))
                        hierarchy.putIfAbsent(jarOrDir.relativize(p).toString().replace('\\', '/'), Files.readAllBytes(p));
            }
            return;
        }
        if (!Files.exists(jarOrDir)) return;
        try (ZipFile z = new ZipFile(jarOrDir.toFile())) {
            for (var it = z.entries(); it.hasMoreElements(); ) {
                ZipEntry e = it.nextElement();
                if (e.getName().endsWith(".class")) hierarchy.putIfAbsent(e.getName(), z.getInputStream(e).readAllBytes());
            }
        }
    }

    static void useHierarchy() {
        ClassHierarchyResolver ours = ClassHierarchyResolver.ofResourceParsing(desc -> {
            byte[] b = hierarchy.get(desc.descriptorString().substring(1, desc.descriptorString().length() - 1) + ".class");
            return b == null ? null : new ByteArrayInputStream(b);
        });
        var resolver = ClassFile.ClassHierarchyResolverOption.of(ours.orElse(ClassHierarchyResolver.defaultResolver()));
        CF = ClassFile.of(resolver);
        CF_GEN = ClassFile.of(resolver, ClassFile.StackMapsOption.GENERATE_STACK_MAPS, ClassFile.DebugElementsOption.DROP_DEBUG, ClassFile.LineNumbersOption.DROP_LINE_NUMBERS);
    }

    public static void main(String[] args) throws Exception {
        switch (args[0]) {
            case "base" -> base(Path.of(args[1]), Path.of(args[2]));
            case "real" -> real(Path.of(args[1]), Path.of(args[2]), Arrays.copyOfRange(args, 3, args.length));
            case "merge" -> merge(Path.of(args[1]), Path.of(args[2]), Path.of(args[3]), Path.of(args[4]));
            case "app" -> app(Path.of(args[1]), Path.of(args[2]));
            default -> throw new IllegalArgumentException(args[0]);
        }
    }

    // --- base ---------------------------------------------------------------------

    static boolean keepForBase(String name) {
        return name.startsWith("android/") || name.startsWith("com/android/") || name.startsWith("dalvik/")
                || name.startsWith("org/xmlpull/") || name.startsWith("org/json/");
    }

    static boolean isStub(CodeModel code) {
        for (CodeElement e : code) {
            if (e instanceof ConstantInstruction ci && "Stub!".equals(ci.constantValue()))
                return true;
        }
        return false;
    }

    static void emitDefaultReturn(CodeBuilder cb, MethodTypeDesc type) {
        ClassDesc r = type.returnType();
        switch (r.descriptorString()) {
            case "V" -> cb.return_();
            case "Z", "B", "C", "S", "I" -> cb.iconst_0().ireturn();
            case "J" -> cb.lconst_0().lreturn();
            case "F" -> cb.fconst_0().freturn();
            case "D" -> cb.dconst_0().dreturn();
            default -> cb.aconst_null().areturn();
        }
    }

    // android.jar class hierarchy: class -> superclass, class -> instance method keys.
    static final Map<String, String> superOf = new HashMap<>();
    static final Map<String, Set<String>> methodsOf = new HashMap<>();
    static final Set<String> OBJECT_METHODS = Set.of("toString()Ljava/lang/String;", "hashCode()I",
            "equals(Ljava/lang/Object;)Z");

    /** True if an ancestor (in android.jar, or Object) declares this instance method. */
    static boolean inherited(String cls, String key) {
        if (OBJECT_METHODS.contains(key)) return true;
        String s = superOf.get(cls);
        while (s != null) {
            Set<String> m = methodsOf.get(s);
            if (m != null && m.contains(key)) return true;
            s = superOf.get(s);
        }
        return false;
    }

    // Methods of android.content.Context (name+descriptor), for ContextWrapper delegation.
    static final Set<String> contextMethods = new HashSet<>();
    static final ClassDesc CONTEXT = ClassDesc.ofInternalName("android/content/Context");

    static void loadParameters(CodeBuilder cob, MethodTypeDesc t) {
        int slot = 1;
        for (ClassDesc p : t.parameterList()) {
            TypeKind k = TypeKind.from(p);
            cob.loadLocal(k, slot);
            slot += k.slotSize();
        }
    }

    static void returnValue(CodeBuilder cob, MethodTypeDesc t) {
        cob.return_(TypeKind.from(t.returnType()));
    }

    static byte[] destub(byte[] bytes) {
        ClassModel cm = CF.parse(bytes);
        final boolean isInterface = cm.flags().has(AccessFlag.INTERFACE);
        final boolean isContextWrapper = cm.thisClass().asInternalName().equals("android/content/ContextWrapper");
        boolean hasNoArgCtor = false;
        for (MethodModel m : cm.methods())
            if (m.methodName().stringValue().equals("<init>") && m.methodType().stringValue().equals("()V"))
                hasNoArgCtor = true;
        // Android hides most constructors of framework services; give every class one.
        final boolean addCtor = !isInterface && !hasNoArgCtor && cm.superclass().isPresent();
        ClassTransform addCtorTransform = ClassTransform.endHandler(clb -> {
            if (!addCtor) return;
            ClassDesc sup = cm.superclass().get().asSymbol();
            clb.withMethodBody("<init>", MethodTypeDesc.of(ConstantDescs.CD_void), ClassFile.ACC_PUBLIC,
                    cob -> cob.aload(0).invokespecial(sup, "<init>", MethodTypeDesc.of(ConstantDescs.CD_void)).return_());
        });
        return CF.transformClass(cm, addCtorTransform.andThen((clb, ce) -> {
            if (ce instanceof MethodModel mm && !isInterface && mm.methodName().stringValue().equals("<init>")
                    && mm.methodType().stringValue().equals("()V") && !mm.flags().has(AccessFlag.PUBLIC)) {
                // Hidden no-arg constructors become public (see addCtor).
                int flags = (mm.flags().flagsMask() & ~(ClassFile.ACC_PRIVATE | ClassFile.ACC_PROTECTED)) | ClassFile.ACC_PUBLIC;
                boolean stub = mm.code().isPresent() && isStub(mm.code().get());
                clb.withMethod(mm.methodName(), mm.methodType(), flags, mb -> {
                    for (MethodElement me : mm) {
                        if (me instanceof CodeModel code && stub) {
                            mb.withCode(cob -> {
                                for (CodeElement e : code) {
                                    if (e instanceof Instruction ins) {
                                        if (ins instanceof NewObjectInstruction) break;
                                        cob.with(e);
                                        if (ins instanceof InvokeInstruction ii && ii.opcode() == Opcode.INVOKESPECIAL
                                                && ii.name().stringValue().equals("<init>"))
                                            break;
                                    }
                                }
                                cob.return_();
                            });
                        } else if (!(me instanceof AccessFlags)) {
                            mb.with(me);
                        }
                    }
                });
                return;
            }
            if (ce instanceof MethodModel mm && isContextWrapper && mm.code().isPresent()
                    && !mm.flags().has(AccessFlag.STATIC) && !mm.methodName().stringValue().startsWith("<")
                    && contextMethods.contains(key(mm))) {
                // ContextWrapper: forward to mBase, like the real one.
                clb.withMethod(mm.methodName(), mm.methodType(), mm.flags().flagsMask(), mb -> {
                    for (MethodElement me : mm)
                        if (!(me instanceof CodeModel)) mb.with(me);
                    mb.withCode(cob -> {
                        cob.aload(0).getfield(cm.thisClass().asSymbol(), "mBase", CONTEXT);
                        loadParameters(cob, mm.methodTypeSymbol());
                        cob.invokevirtual(CONTEXT, mm.methodName().stringValue(), mm.methodTypeSymbol());
                        returnValue(cob, mm.methodTypeSymbol());
                    });
                });
                return;
            }
            if (ce instanceof MethodModel mm && !isInterface && mm.flags().has(AccessFlag.ABSTRACT)) {
                // Abstract framework methods become no-ops so Refract classes implement only what they need.
                int flags = mm.flags().flagsMask() & ~ClassFile.ACC_ABSTRACT;
                clb.withMethod(mm.methodName(), mm.methodType(), flags, mb -> {
                    for (MethodElement me : mm)
                        if (!(me instanceof AccessFlags)) mb.with(me);
                    mb.withCode(cob -> emitDefaultReturn(cob, mm.methodTypeSymbol()));
                });
                return;
            }
            if (ce instanceof MethodModel mm && mm.code().isPresent() && isStub(mm.code().get())
                    && !mm.flags().has(AccessFlag.STATIC) && !mm.methodName().stringValue().startsWith("<")
                    && inherited(cm.thisClass().asInternalName(), key(mm))) {
                // A stubbed override would hide the parent's (possibly real) implementation.
                return;
            }
            if (ce instanceof MethodModel mm && mm.code().isPresent() && isStub(mm.code().get())) {
                CodeModel code = mm.code().get();
                String name = mm.methodName().stringValue();
                clb.withMethod(mm.methodName(), mm.methodType(), mm.flags().flagsMask(), mb -> {
                    for (MethodElement me : mm)
                        if (!(me instanceof CodeModel)) mb.with(me);
                    mb.withCode(cob -> {
                        if (name.equals("<init>")) {
                            // Keep the delegating constructor call but pass our own arguments on
                            // (the stub passes nulls, which leaves e.g. View.mContext unset).
                            for (CodeElement e : code) {
                                if (e instanceof InvokeInstruction ii && ii.opcode() == Opcode.INVOKESPECIAL
                                        && ii.name().stringValue().equals("<init>")) {
                                    List<ClassDesc> mine = mm.methodTypeSymbol().parameterList();
                                    cob.aload(0);
                                    int slot = 1;
                                    int[] slots = new int[mine.size()];
                                    for (int i = 0; i < mine.size(); i++) {
                                        slots[i] = slot;
                                        slot += TypeKind.from(mine.get(i)).slotSize();
                                    }
                                    List<ClassDesc> sup = ii.typeSymbol().parameterList();
                                    for (int i = 0; i < sup.size(); i++) {
                                        TypeKind k = TypeKind.from(sup.get(i));
                                        if (i < mine.size() && mine.get(i).equals(sup.get(i))) {
                                            cob.loadLocal(k, slots[i]);
                                        } else {
                                            switch (k) {
                                                case REFERENCE -> cob.aconst_null();
                                                case LONG -> cob.lconst_0();
                                                case FLOAT -> cob.fconst_0();
                                                case DOUBLE -> cob.dconst_0();
                                                default -> cob.iconst_0();
                                            }
                                        }
                                    }
                                    cob.invokespecial(ii.owner().asSymbol(), "<init>", ii.typeSymbol());
                                    break;
                                }
                                if (e instanceof NewObjectInstruction) break;
                            }
                            cob.return_();
                        } else if (name.equals("<clinit>")) {
                            cob.return_();
                        } else {
                            emitDefaultReturn(cob, mm.methodTypeSymbol());
                        }
                    });
                });
            } else {
                clb.with(ce);
            }
        }));
    }

    static void base(Path androidJar, Path out) throws IOException {
        addHierarchy(androidJar);
        useHierarchy();
        int n = 0;
        try (ZipFile in = new ZipFile(androidJar.toFile())) {
            ClassModel ctx = CF.parse(in.getInputStream(in.getEntry("android/content/Context.class")).readAllBytes());
            for (MethodModel m : ctx.methods()) contextMethods.add(key(m));
            for (var it = in.entries(); it.hasMoreElements(); ) {
                ZipEntry e = it.nextElement();
                if (!e.getName().endsWith(".class")) continue;
                ClassModel c = CF.parse(in.getInputStream(e).readAllBytes());
                String name = c.thisClass().asInternalName();
                c.superclass().ifPresent(sc -> superOf.put(name, sc.asInternalName()));
                Set<String> keys = new HashSet<>();
                for (MethodModel m : c.methods())
                    if (!m.flags().has(AccessFlag.STATIC) && !m.flags().has(AccessFlag.PRIVATE)) keys.add(key(m));
                methodsOf.put(name, keys);
            }
        }
        try (ZipFile in = new ZipFile(androidJar.toFile());
             JarOutputStream jo = new JarOutputStream(Files.newOutputStream(out))) {
            for (var it = in.entries(); it.hasMoreElements(); ) {
                ZipEntry e = it.nextElement();
                if (!e.getName().endsWith(".class") || !keepForBase(e.getName())) continue;
                byte[] b = in.getInputStream(e).readAllBytes();
                jo.putNextEntry(new JarEntry(e.getName()));
                jo.write(destub(b));
                jo.closeEntry();
                n++;
            }
        }
        System.out.println("base: " + n + " classes -> " + out);
    }

    // --- real -----------------------------------------------------------------------

    static void real(Path whitelist, Path out, String[] jars) throws IOException {
        List<String> include = new ArrayList<>(), exclude = new ArrayList<>();
        for (String line : Files.readAllLines(whitelist)) {
            line = line.trim();
            if (line.isEmpty() || line.startsWith("#")) continue;
            if (line.startsWith("!")) exclude.add(line.substring(1));
            else include.add(line);
        }
        int n = 0;
        Set<String> seen = new HashSet<>();
        try (JarOutputStream jo = new JarOutputStream(Files.newOutputStream(out))) {
            for (String jar : jars) {
                try (ZipFile in = new ZipFile(jar)) {
                    for (var it = in.entries(); it.hasMoreElements(); ) {
                        ZipEntry e = it.nextElement();
                        String name = e.getName();
                        if (!name.endsWith(".class") || !seen.add(name)) continue;
                        String cls = name.substring(0, name.length() - 6);
                        boolean in_ = false;
                        for (String p : include)
                            if (p.endsWith("/") ? cls.startsWith(p) : (cls.equals(p) || cls.startsWith(p + "$"))) in_ = true;
                        for (String p : exclude)
                            if (cls.equals(p) || cls.startsWith(p + "$")) in_ = false;
                        if (!in_) continue;
                        jo.putNextEntry(new JarEntry(name));
                        jo.write(in.getInputStream(e).readAllBytes());
                        jo.closeEntry();
                        n++;
                    }
                }
            }
        }
        System.out.println("real: " + n + " firmware classes -> " + out);
    }

    // --- merge --------------------------------------------------------------------

    static String key(MethodModel m) {
        return m.methodName().stringValue() + m.methodType().stringValue();
    }

    static byte[] mergeClass(byte[] baseBytes, byte[] overlayBytes) {
        ClassModel base = CF.parse(baseBytes);
        ClassModel over = CF.parse(overlayBytes);
        if (!base.superclass().map(c -> c.asInternalName()).equals(over.superclass().map(c -> c.asInternalName())))
            System.err.println("warning: " + over.thisClass().asInternalName() + " changes its superclass");
        Set<String> overMethods = new HashSet<>();
        for (MethodModel m : over.methods()) overMethods.add(key(m));
        Set<String> overFields = new HashSet<>();
        for (FieldModel f : over.fields()) overFields.add(f.fieldName().stringValue());
        Set<String> overIfaces = new HashSet<>();
        for (ClassEntry c : over.interfaces()) overIfaces.add(c.asInternalName());
        List<ClassEntry> extraIfaces = new ArrayList<>();
        for (ClassEntry c : base.interfaces())
            if (!overIfaces.contains(c.asInternalName())) extraIfaces.add(c);
        boolean old = (baseBytes[7] & 0xff) == 49;
        return CF.transformClass(over, ClassTransform.endHandler(clb -> {
            for (MethodModel m : base.methods())
                if (!overMethods.contains(key(m))) clb.with(m);
            for (FieldModel f : base.fields())
                if (!overFields.contains(f.fieldName().stringValue())) clb.with(f);
        }).andThen((clb, ce) -> {
            if (old && ce instanceof ClassFileVersion) {
                clb.withVersion(49, 0);
            } else if (ce instanceof Interfaces ifs && !extraIfaces.isEmpty()) {
                List<ClassEntry> all = new ArrayList<>(ifs.interfaces());
                all.addAll(extraIfaces);
                clb.withInterfaces(all);
            } else {
                clb.with(ce);
            }
        }));
    }

    /** dex2jar output has no StackMapTable and a wrong max_stack: recompute it and use class version 49
     *  (HotSpot verifies those by type inference). */
    static byte[] fixReal(byte[] b) {
        if (b.length < 8 || b[6] != 0 || (b[7] & 0xff) <= 49 || CF.parse(b).flags().has(AccessFlag.INTERFACE)) return b;
        // dex2jar's max_stack is unreliable (and ASM's recomputation undercounts methods that keep
        // uninitialized objects in locals), so be generous: the old verifier only checks the bound.
        org.objectweb.asm.ClassReader cr = new org.objectweb.asm.ClassReader(b);
        org.objectweb.asm.ClassWriter cw = new org.objectweb.asm.ClassWriter(0);
        cr.accept(new org.objectweb.asm.ClassVisitor(org.objectweb.asm.Opcodes.ASM9, cw) {
            @Override
            public org.objectweb.asm.MethodVisitor visitMethod(int acc, String n, String d, String sig, String[] ex) {
                return new org.objectweb.asm.MethodVisitor(org.objectweb.asm.Opcodes.ASM9, super.visitMethod(acc, n, d, sig, ex)) {
                    @Override
                    public void visitMaxs(int maxStack, int maxLocals) {
                        super.visitMaxs(Math.max(maxStack + 24, 32), maxLocals);
                    }
                };
            }
        }, org.objectweb.asm.ClassReader.SKIP_FRAMES | org.objectweb.asm.ClassReader.SKIP_DEBUG);
        b = cw.toByteArray();
        b[7] = 49;
        return b;
    }

    static void merge(Path baseJar, Path realJar, Path overlayDir, Path out) throws IOException {
        addHierarchy(overlayDir);
        addHierarchy(realJar);
        addHierarchy(baseJar);
        useHierarchy();
        Map<String, byte[]> real = new HashMap<>();
        if (Files.exists(realJar)) {
            try (ZipFile in = new ZipFile(realJar.toFile())) {
                for (var it = in.entries(); it.hasMoreElements(); ) {
                    ZipEntry e = it.nextElement();
                    real.put(e.getName(), in.getInputStream(e).readAllBytes());
                }
            }
        }
        Map<String, byte[]> overlay = new TreeMap<>();
        try (var walk = Files.walk(overlayDir)) {
            for (Path p : (Iterable<Path>) walk::iterator) {
                if (!p.toString().endsWith(".class")) continue;
                String name = overlayDir.relativize(p).toString().replace('\\', '/');
                overlay.put(name, Files.readAllBytes(p));
            }
        }
        int merged = 0;
        try (ZipFile in = new ZipFile(baseJar.toFile());
             JarOutputStream jo = new JarOutputStream(Files.newOutputStream(out))) {
            Set<String> done = new HashSet<>();
            for (var it = in.entries(); it.hasMoreElements(); ) {
                ZipEntry e = it.nextElement();
                byte[] b = in.getInputStream(e).readAllBytes();
                byte[] r = real.remove(e.getName());
                if (r != null) {
                    b = r;
                    b = fixReal(b);
                }
                byte[] o = overlay.get(e.getName());
                if (o != null) {
                    b = mergeClass(b, o);
                    merged++;
                    done.add(e.getName());
                }
                jo.putNextEntry(new JarEntry(e.getName()));
                jo.write(b);
                jo.closeEntry();
            }
            for (var re : real.entrySet()) {
                // Firmware classes without an android.jar counterpart (internal helpers).
                byte[] b = fixReal(re.getValue());
                byte[] o = overlay.get(re.getKey());
                if (o != null) {
                    b = mergeClass(b, o);
                    merged++;
                    done.add(re.getKey());
                }
                jo.putNextEntry(new JarEntry(re.getKey()));
                jo.write(b);
                jo.closeEntry();
            }
            for (var oe : overlay.entrySet()) {
                if (done.contains(oe.getKey())) continue;
                jo.putNextEntry(new JarEntry(oe.getKey()));
                jo.write(oe.getValue());
                jo.closeEntry();
            }
            System.out.println("merge: " + merged + " classes merged, " + (overlay.size() - merged) + " added -> " + out);
        }
    }

    // --- app ----------------------------------------------------------------------

    static final ClassDesc RUNTIME = ClassDesc.of("refract.Runtime");
    static final ClassDesc STRING = ClassDesc.of("java.lang.String");
    static final MethodTypeDesc S_TO_S = MethodTypeDesc.of(STRING, STRING);

    static int rewrites = 0;

    // --- byte-level invoke patching ----------------------------------------------

    /**
     * Redirects System.loadLibrary/load and Runtime.loadLibrary/load to refract.Runtime
     * by patching invoke instructions in place (same length, same stack effect), so
     * StackMapTables stay valid. Returns the input when there is nothing to patch.
     */
    static byte[] patchLoadLibrary(byte[] b) {
        java.nio.ByteBuffer in = java.nio.ByteBuffer.wrap(b);
        in.position(8);
        int cpCount = in.getShort() & 0xffff;
        String[] utf = new String[cpCount];
        int[] tag = new int[cpCount], a = new int[cpCount], c = new int[cpCount];
        for (int i = 1; i < cpCount; i++) {
            int t = in.get() & 0xff;
            tag[i] = t;
            switch (t) {
                case 1 -> {
                    int len = in.getShort() & 0xffff;
                    byte[] sb = new byte[len];
                    in.get(sb);
                    utf[i] = new String(sb, java.nio.charset.StandardCharsets.UTF_8);
                }
                case 3, 4 -> in.position(in.position() + 4);
                case 5, 6 -> { in.position(in.position() + 8); i++; }
                case 7, 8, 16, 19, 20 -> a[i] = in.getShort() & 0xffff;
                case 9, 10, 11, 12, 17, 18 -> { a[i] = in.getShort() & 0xffff; c[i] = in.getShort() & 0xffff; }
                case 15 -> in.position(in.position() + 3);
                default -> throw new IllegalStateException("cp tag " + t);
            }
        }
        int cpEnd = in.position();
        Map<Integer, String[]> targets = new HashMap<>();
        for (int i = 1; i < cpCount; i++) {
            if (tag[i] != 10) continue;
            String owner = utf[a[a[i]]];
            String name = utf[a[c[i]]], desc = utf[c[c[i]]];
            if (!"(Ljava/lang/String;)V".equals(desc) || !(name.equals("loadLibrary") || name.equals("load"))) continue;
            if (owner.equals("java/lang/System")) targets.put(i, new String[] {name, desc});
            else if (owner.equals("java/lang/Runtime"))
                targets.put(i, new String[] {"runtime" + Character.toUpperCase(name.charAt(0)) + name.substring(1),
                        "(Ljava/lang/Runtime;Ljava/lang/String;)V"});
        }
        if (targets.isEmpty()) return b;
        try {
            ByteArrayOutputStream extra = new ByteArrayOutputStream();
            DataOutputStream eo = new DataOutputStream(extra);
            int next = cpCount;
            eo.writeByte(1);
            eo.writeUTF("refract/Runtime");
            int clsName = next++;
            eo.writeByte(7);
            eo.writeShort(clsName);
            int cls = next++;
            Map<Integer, Integer> remap = new HashMap<>();
            for (var t : targets.entrySet()) {
                eo.writeByte(1);
                eo.writeUTF(t.getValue()[0]);
                int n = next++;
                eo.writeByte(1);
                eo.writeUTF(t.getValue()[1]);
                int d = next++;
                eo.writeByte(12);
                eo.writeShort(n);
                eo.writeShort(d);
                int nat = next++;
                eo.writeByte(10);
                eo.writeShort(cls);
                eo.writeShort(nat);
                remap.put(t.getKey(), next++);
            }
            byte[] out = b.clone();
            in.position(cpEnd + 6);
            int ifaces = in.getShort() & 0xffff;
            in.position(in.position() + ifaces * 2);
            int fields = in.getShort() & 0xffff;
            for (int i = 0; i < fields; i++) {
                in.position(in.position() + 6);
                skipAttributes(in);
            }
            int methods = in.getShort() & 0xffff;
            int patched = 0;
            for (int i = 0; i < methods; i++) {
                in.position(in.position() + 6);
                int na = in.getShort() & 0xffff;
                for (int k = 0; k < na; k++) {
                    int nameIdx = in.getShort() & 0xffff;
                    int len = in.getInt();
                    int start = in.position();
                    if ("Code".equals(utf[nameIdx])) {
                        int codeLen = in.getInt(start + 4);
                        patched += patchCode(out, start + 8, codeLen, remap);
                    }
                    in.position(start + len);
                }
            }
            if (patched == 0) return b;
            rewrites += patched;
            ByteArrayOutputStream res = new ByteArrayOutputStream(out.length + extra.size());
            res.write(out, 0, 8);
            res.write((next >> 8) & 0xff);
            res.write(next & 0xff);
            res.write(out, 10, cpEnd - 10);
            res.write(extra.toByteArray());
            res.write(out, cpEnd, out.length - cpEnd);
            return res.toByteArray();
        } catch (IOException e) {
            throw new UncheckedIOException(e);
        }
    }

    static void skipAttributes(java.nio.ByteBuffer in) {
        int n = in.getShort() & 0xffff;
        for (int i = 0; i < n; i++) {
            in.getShort();
            int len = in.getInt();
            in.position(in.position() + len);
        }
    }

    // Lengths of fixed-size opcodes (0 = variable: tableswitch, lookupswitch, wide).
    static final int[] OPLEN = new int[256];
    static {
        Arrays.fill(OPLEN, 1);
        for (int op : new int[] {0x10, 0x12, 0x15, 0x16, 0x17, 0x18, 0x19, 0x36, 0x37, 0x38, 0x39, 0x3a, 0xa9, 0xbc})
            OPLEN[op] = 2;
        for (int op : new int[] {0x11, 0x13, 0x14, 0x84, 0x99, 0x9a, 0x9b, 0x9c, 0x9d, 0x9e, 0x9f, 0xa0, 0xa1, 0xa2,
                0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xbb, 0xbd, 0xc0, 0xc1,
                0xc6, 0xc7})
            OPLEN[op] = 3;
        OPLEN[0xc5] = 4;
        for (int op : new int[] {0xb9, 0xba, 0xc8, 0xc9}) OPLEN[op] = 5;
        OPLEN[0xaa] = OPLEN[0xab] = OPLEN[0xc4] = 0;
    }

    static int patchCode(byte[] code, int start, int len, Map<Integer, Integer> remap) {
        int pc = 0, patched = 0;
        while (pc < len) {
            int op = code[start + pc] & 0xff;
            int l = OPLEN[op];
            if (op == 0xaa || op == 0xab) {
                int p = (pc + 4) & ~3;
                java.nio.ByteBuffer bb = java.nio.ByteBuffer.wrap(code);
                if (op == 0xaa) {
                    int lo = bb.getInt(start + p + 4);
                    int hi = bb.getInt(start + p + 8);
                    l = (p - pc) + 12 + (hi - lo + 1) * 4;
                } else {
                    int npairs = bb.getInt(start + p + 4);
                    l = (p - pc) + 8 + npairs * 8;
                }
            } else if (op == 0xc4) {
                l = (code[start + pc + 1] & 0xff) == 0x84 ? 6 : 4;
            } else if (op == 0xb8 || op == 0xb6) {
                int idx = ((code[start + pc + 1] & 0xff) << 8) | (code[start + pc + 2] & 0xff);
                Integer to = remap.get(idx);
                if (to != null) {
                    code[start + pc] = (byte) 0xb8;
                    code[start + pc + 1] = (byte) (to >> 8);
                    code[start + pc + 2] = (byte) (int) to;
                    patched++;
                }
            }
            pc += l;
        }
        return patched;
    }

    /** True if the class constructs java.io/java.nio paths from strings (cheap constant-pool check). */
    static boolean needsPathRewrite(byte[] b) {
        String s = new String(b, java.nio.charset.StandardCharsets.ISO_8859_1);
        return s.contains("java/io/File") || s.contains("java/io/RandomAccessFile") || s.contains("java/nio/file/Path");
    }

    static byte[] rewriteApp(byte[] bytes) {
        ClassModel cm = CF.parse(bytes);
        CodeTransform ct = (cob, e) -> {
            if (e instanceof InvokeInstruction ii) {
                String owner = ii.owner().asInternalName();
                String name = ii.name().stringValue();
                String desc = ii.type().stringValue();
                if (owner.equals("java/lang/System") && (name.equals("loadLibrary") || name.equals("load"))
                        && desc.equals("(Ljava/lang/String;)V")) {
                    cob.invokestatic(RUNTIME, name, MethodTypeDesc.ofDescriptor(desc));
                    rewrites++;
                    return;
                }
                if (owner.equals("java/lang/Runtime") && (name.equals("loadLibrary") || name.equals("load"))
                        && desc.equals("(Ljava/lang/String;)V")) {
                    cob.invokestatic(RUNTIME, "runtime" + Character.toUpperCase(name.charAt(0)) + name.substring(1),
                            MethodTypeDesc.ofDescriptor("(Ljava/lang/Runtime;Ljava/lang/String;)V"));
                    rewrites++;
                    return;
                }
                if (ii.opcode() == Opcode.INVOKESTATIC
                        && ((owner.equals("java/nio/file/Paths") && name.equals("get"))
                            || (owner.equals("java/nio/file/Path") && name.equals("of")))
                        && desc.equals("(Ljava/lang/String;[Ljava/lang/String;)Ljava/nio/file/Path;")) {
                    cob.swap().invokestatic(RUNTIME, "hostPath", S_TO_S).swap();
                    rewrites++;
                }
                if (ii.opcode() == Opcode.INVOKESPECIAL && name.equals("<init>")) {
                    // Path argument on top of the stack: map it. Path one below: swap around it.
                    boolean top = false, second = false;
                    switch (owner + desc) {
                        case "java/io/File(Ljava/lang/String;)V",
                             "java/io/FileInputStream(Ljava/lang/String;)V",
                             "java/io/FileOutputStream(Ljava/lang/String;)V",
                             "java/io/FileReader(Ljava/lang/String;)V",
                             "java/io/FileWriter(Ljava/lang/String;)V" -> top = true;
                        case "java/io/File(Ljava/lang/String;Ljava/lang/String;)V",
                             "java/io/FileOutputStream(Ljava/lang/String;Z)V",
                             "java/io/FileWriter(Ljava/lang/String;Z)V",
                             "java/io/RandomAccessFile(Ljava/lang/String;Ljava/lang/String;)V" -> second = true;
                        default -> { }
                    }
                    if (top) {
                        cob.invokestatic(RUNTIME, "hostPath", S_TO_S);
                        rewrites++;
                    } else if (second) {
                        cob.swap().invokestatic(RUNTIME, "hostPath", S_TO_S).swap();
                        rewrites++;
                    }
                }
            }
            cob.with(e);
        };
        try {
            byte[] out = CF.transformClass(cm, ClassTransform.transformingMethodBodies(ct));
            return hasIndy(cm) ? out : fixReal(out);
        } catch (IllegalArgumentException e) {
            // dex2jar output the stack map generator can't handle: drop stack maps and
            // mark the class version 50 so HotSpot verifies it by type inference.
            for (MethodModel m : cm.methods())
                if (m.code().isPresent())
                    for (CodeElement ce : m.code().get())
                        if (ce instanceof InvokeDynamicInstruction) throw e;
            ClassFile drop = ClassFile.of(ClassFile.StackMapsOption.DROP_STACK_MAPS);
            return fixReal(drop.transformClass(drop.parse(stripStackMaps(bytes)), ClassTransform.transformingMethodBodies(ct).andThen((clb, ce) -> {
                if (ce instanceof ClassFileVersion) clb.withVersion(52, 0);
                else clb.with(ce);
            })));
        }
    }

    static boolean hasIndy(ClassModel cm) {
        for (MethodModel m : cm.methods())
            if (m.code().isPresent())
                for (CodeElement ce : m.code().get())
                    if (ce instanceof InvokeDynamicInstruction) return true;
        return false;
    }

    /** Removes StackMapTable attributes from a class file (byte level, no parsing of the code). */
    static byte[] stripStackMaps(byte[] b) {
        java.nio.ByteBuffer in = java.nio.ByteBuffer.wrap(b);
        ByteArrayOutputStream out = new ByteArrayOutputStream(b.length);
        DataOutputStream o = new DataOutputStream(out);
        try {
            in.position(8);
            int cpCount = in.getShort() & 0xffff;
            String[] utf = new String[cpCount];
            for (int i = 1; i < cpCount; i++) {
                int tag = in.get() & 0xff;
                switch (tag) {
                    case 1 -> {
                        int len = in.getShort() & 0xffff;
                        byte[] s = new byte[len];
                        in.get(s);
                        utf[i] = new String(s, java.nio.charset.StandardCharsets.UTF_8);
                    }
                    case 3, 4 -> in.position(in.position() + 4);
                    case 5, 6 -> { in.position(in.position() + 8); i++; }
                    case 7, 8, 16, 19, 20 -> in.position(in.position() + 2);
                    case 9, 10, 11, 12, 17, 18 -> in.position(in.position() + 4);
                    case 15 -> in.position(in.position() + 3);
                    default -> throw new IllegalStateException("cp tag " + tag);
                }
            }
            int cpEnd = in.position();
            o.write(b, 0, cpEnd);
            o.write(b, cpEnd, 6);  // access, this, super
            in.position(cpEnd + 6);
            int ifaces = in.getShort() & 0xffff;
            o.writeShort(ifaces);
            for (int i = 0; i < ifaces; i++) o.writeShort(in.getShort());
            for (int pass = 0; pass < 2; pass++) {  // fields, methods
                int n = in.getShort() & 0xffff;
                o.writeShort(n);
                for (int i = 0; i < n; i++) {
                    o.writeShort(in.getShort());
                    o.writeShort(in.getShort());
                    o.writeShort(in.getShort());
                    copyAttributes(in, o, utf, true);
                }
            }
            copyAttributes(in, o, utf, false);
            o.flush();
            return out.toByteArray();
        } catch (IOException e) {
            throw new UncheckedIOException(e);
        }
    }

    static void copyAttributes(java.nio.ByteBuffer in, DataOutputStream o, String[] utf, boolean memberLevel) throws IOException {
        int n = in.getShort() & 0xffff;
        o.writeShort(n);
        for (int i = 0; i < n; i++) {
            int nameIdx = in.getShort() & 0xffff;
            int len = in.getInt();
            byte[] body = new byte[len];
            in.get(body);
            if (memberLevel && "Code".equals(utf[nameIdx])) {
                java.nio.ByteBuffer c = java.nio.ByteBuffer.wrap(body);
                ByteArrayOutputStream cb = new ByteArrayOutputStream();
                DataOutputStream co = new DataOutputStream(cb);
                co.writeShort(c.getShort());  // max_stack
                co.writeShort(c.getShort());  // max_locals
                int codeLen = c.getInt();
                co.writeInt(codeLen);
                byte[] code = new byte[codeLen];
                c.get(code);
                co.write(code);
                int ex = c.getShort() & 0xffff;
                co.writeShort(ex);
                for (int k = 0; k < ex * 4; k++) co.writeShort(c.getShort());
                int an = c.getShort() & 0xffff;
                List<byte[]> kept = new ArrayList<>();
                for (int k = 0; k < an; k++) {
                    int ani = c.getShort() & 0xffff;
                    int al = c.getInt();
                    byte[] ab = new byte[al];
                    c.get(ab);
                    if ("StackMapTable".equals(utf[ani])) continue;
                    ByteArrayOutputStream one = new ByteArrayOutputStream();
                    DataOutputStream oo = new DataOutputStream(one);
                    oo.writeShort(ani);
                    oo.writeInt(al);
                    oo.write(ab);
                    kept.add(one.toByteArray());
                }
                co.writeShort(kept.size());
                for (byte[] k : kept) co.write(k);
                co.flush();
                body = cb.toByteArray();
            }
            o.writeShort(nameIdx);
            o.writeInt(body.length);
            o.write(body);
        }
    }

    static void app(Path in, Path out) throws IOException {
        addHierarchy(in);
        String shim = System.getProperty("refract.shim");
        if (shim != null) addHierarchy(Path.of(shim));
        useHierarchy();
        int n = 0;
        try (ZipFile zin = new ZipFile(in.toFile());
             JarOutputStream jo = new JarOutputStream(Files.newOutputStream(out))) {
            for (var it = zin.entries(); it.hasMoreElements(); ) {
                ZipEntry e = it.nextElement();
                if (e.isDirectory()) continue;
                byte[] b = zin.getInputStream(e).readAllBytes();
                if (e.getName().endsWith(".class")) {
                    b = patchLoadLibrary(b);
                    if (needsPathRewrite(b)) {
                        try {
                            b = rewriteApp(b);
                        } catch (RuntimeException ex) {
                            System.err.println("warning: guest paths in " + e.getName()
                                    + " stay unmapped for java.io (" + ex.getMessage() + ")");
                        }
                    }
                    if (!hasIndy(CF.parse(b))) b = fixReal(b);
                    n++;
                }
                jo.putNextEntry(new JarEntry(e.getName()));
                jo.write(b);
                jo.closeEntry();
            }
        }
        System.out.println("app: " + n + " classes, " + rewrites + " call sites rewritten -> " + out);
    }
}
