# Digitalis prebuilts — ARM64-to-x86_64 translator (binary-only)

This bundle lets an **x86_64 Android (AOSP) product** run **arm64-v8a-only** apps by
dropping in the prebuilt Digitalis translator — no need to build Berberis from
source. It is binaries + configs + one makefile; you integrate it into your product
source with a single `inherit-product` line.

See `MANIFEST.txt` for this bundle's build date, builder identity, source revisions,
and artifact count, and `SHA256SUMS` for per-file integrity.

## What's in here

```
system/lib64/libberberis_arm64.so                 # the translator / native bridge (x86-64 host lib)
system/lib64/libberberis_exec_region.so
system/lib64/libberberis_proxy_*.so               # proxy libs (libc, libm, libvulkan, …)
system/bin/berberis_program_runner_arm64          # arm64 program runners
system/bin/berberis_program_runner_binfmt_misc_arm64
system/lib64/arm64/*.so                           # arm64 GUEST libraries (libc, libm, libvulkan, …)
system/bin/arm64/{app_process64,linker64}         # arm64 guest app_process + linker
system/etc/ld.config.arm64.txt                    # arm64 linker namespace config
system/etc/init/berberis.rc                       # binfmt_misc registration
system/etc/binfmt_misc/arm64_{dyn,exe}
digitalis-prebuilts.mk                            # the integration makefile (below)
```

The host-side libraries/executables are `ELF x86-64`; everything under
`system/lib64/arm64/` and `system/bin/arm64/` is `ELF aarch64` (the guest payload the
translator loads). Verify with `readelf -h`.

## Requirements

- An **x86_64** AOSP product you build from source (this provides the framework,
  zygote, GPU stack, etc.). Digitalis is the translator only.
- For **Vulkan** apps, a working host GPU path (e.g. GFXStream / ANGLE) in that
  product. Digitalis forwards guest Vulkan to the host via `libberberis_proxy_libvulkan`;
  it does not provide the GPU stack.
- Build the product for API level / platform matching the bundle (see `MANIFEST.txt`).

## Integrate into your x86_64 product source

1. **Copy this whole directory into your tree**, for example:

   ```
   vendor/digitalis/prebuilts/         <- copy `digitalis-prebuilts.mk` and `system/` here
   ```

2. **Inherit the makefile from your product `.mk`** (e.g. your
   `device/<vendor>/<product>/<product>.mk`):

   ```make
   $(call inherit-product, vendor/digitalis/prebuilts/digitalis-prebuilts.mk)
   ```

3. **Build your product as usual** (`source build/envsetup.sh && lunch <your_product> && m`).
   arm64-v8a-only apps now run under translation.

That's all — `digitalis-prebuilts.mk` does the rest:

- Sets the native-bridge system properties:
  ```
  ro.dalvik.vm.native.bridge=libberberis_arm64.so
  ro.dalvik.vm.isa.arm64=x86_64
  ro.enable.native.bridge.exec=1
  ```
- Copies every artifact into the system image at its canonical `system/...` path
  (via `PRODUCT_COPY_FILES`).
- Allow-lists those paths (`PRODUCT_ARTIFACT_PATH_REQUIREMENT_ALLOWED_LIST`) so the
  image-builder accepts them.

## Verify what you received

```bash
# from this bundle's root directory:
sha256sum -c SHA256SUMS                                        # integrity of every file

# arch spot-check:
readelf -h system/lib64/libberberis_arm64.so | grep Machine   # -> X86-64
readelf -h system/lib64/arm64/libc.so        | grep Machine   # -> AArch64
```

## Notes

- The native-bridge property override **replaces** any
  `ro.dalvik.vm.native.bridge?=0` set by `build/make/target/product/runtime_libart.mk`;
  inherit this makefile after that base so the override wins.
- This bundle targets the `system` partition. If your product uses a different
  partition layout, adjust the `system/...` destinations in `digitalis-prebuilts.mk`.
- Digitalis is licensed under Apache-2.0 (see the project `LICENSE`).
