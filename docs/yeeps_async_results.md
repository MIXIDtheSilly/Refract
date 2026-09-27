# Yeeps 64 FPS cap investigation

Date: 2026-09-26. Six-core emulator, TSC clock, Berberis `two-gear`, GPU shared-texture stream.

The previous viewer showed a repeatable plateau near 64 FPS. At 19:45:33, `xrEndFrame` was 64.5 frames/s and spent 10.398 ms on average; `image-ack-wait` alone averaged 9.793 ms. A later viewer probe at the plateau reported 62.9 copies/s and 15.249 ms median GPU-copy completion time. Copying just one eye did not improve it, so that experiment was reverted.

The viewer now queues GPU copies, receives later frames while earlier copies complete, and acknowledges each frame in order only after its query signals. The old synchronous transport behavior remains available to callers that omit the deferred-ack callback. A Windows transport smoke test verifies two received frames can overlap the first delayed acknowledgment and that acknowledgments remain ordered.

After a full emulator restart and relaunch, Yeeps ran above the old plateau for several consecutive five-second windows: 121.0, 133.3, 137.9, 133.8, 136.3, 138.0, and 138.3 frames/s at 20:08:40–20:09:10. It was still at 129–137 frames/s in the ten windows through 20:11:40. `image-ack-wait` averaged 0.006–0.014 ms in the first group, and the viewer displayed frames at similar rates. No GPU-consumer loss or game crash appeared in this run's log.

This before/after comparison includes an emulator restart, so it does not isolate the code change's exact FPS gain. It does verify that the new path preserves copy-before-ack safety and that the 64 FPS plateau was absent in this sustained Yeeps run. The game remains running in `runs/yeeps-async-fresh`.
