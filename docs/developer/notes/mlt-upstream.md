# MLT upstream reports

[Docs home](../../README.md) › [Developer docs](../README.md) › [Implementation notes](README.md) › MLT upstream reports

MLT behaviour we work around that looks like a bug upstream: what it is,
what we do instead, and its upstream report. Each filed report has a
standalone repro (a `melt` script or a short C program against libmlt
alone) in [`tools/upstream-repros/mlt/`](../../../tools/upstream-repros/README.md),
and was checked against MLT master (0926a75) before filing. Filed
2026-09-28 from the owner's GitHub account; check the issue for the
current status. When upstream fixes one, note the MLT version here and
decide whether the workaround can go once that version is our floor.

## Filed

| Behaviour | Our workaround | Upstream |
|---|---|---|
| `composite` blends 4:2:2 YUV with each pixel's own alpha, so where a pair's alphas differ U and V mix by different amounts: anti-aliased edges fringe (white at alpha 5 over red: (60,63,5), not (255,5,5)). | `attachAlphaPairing()` evens out each pair before the compositor ([Engine sync](engine-sync.md)) | [#1311](https://github.com/mltframework/mlt/issues/1311) |
| `composite`'s `halign`/`valign` are right only for frames read at the profile's size; read smaller, the picture shifts right. | Centre only when frames are read at profile size (`EngineSync::FrameReads`) | [#1312](https://github.com/mltframework/mlt/issues/1312) |
| `producer_colour` tags its YUV BT.601 whatever the profile, and `composite` keeps the A frame's tag: BT.709 red 253 → 231 over `color:black`. | Re-tag the black master; other colours as RGBA ([Engine sync](engine-sync.md)) | [#1313](https://github.com/mltframework/mlt/issues/1313) |
| In MLT XML a `resource` of `color:…` loads as black, with or without `mlt_service`: `producer_xml` copies the prefixed resource back over the producer's. | Write `mlt_service=color` and a bare `0xRRGGBBAA` | [#1314](https://github.com/mltframework/mlt/issues/1314) |
| The `avformat` consumer drops alpha for a yuva `pix_fmt` unless `mlt_image_format=rgba` is set. | Set `mlt_image_format=rgba` for alpha exports | [#1315](https://github.com/mltframework/mlt/issues/1315) |
| Every `mix` transition is a 9.2 MB `calloc()`; once glibc's mmap threshold rises, each is zeroed in full. | `M_MMAP_THRESHOLD` pinned in `FactoryPolicy` ([Engine sync](engine-sync.md)) | [#1316](https://github.com/mltframework/mlt/issues/1316) |
| The loader's `dictionary` and `normalizers`, `get_cache()`'s global `caches` and avformat's init are lazy and unlocked: the first parallel opens crash (31 of 200 runs). | Warm them up on the main thread in `FactoryPolicy` | [#1317](https://github.com/mltframework/mlt/issues/1317) |
| `mlt_cache` keeps 4 avformat decoders process-wide and evicts one another thread is decoding with. | `FactoryPolicy::raiseAvformatDecoderLimit()` | [#1318](https://github.com/mltframework/mlt/issues/1318) |
| `_unique_id` is a non-atomic `++` on a static int: concurrent services share ids, which key per-frame data. | None needed so far | [#1319](https://github.com/mltframework/mlt/issues/1319) |
| `sdl2_audio`'s paused path checks `running` outside `refresh_mutex`, then waits without re-checking it: a stop landing between the two hangs `pthread_join()`. From code reading; never reproduced. | `prefill` 1 while paused ([Playback engine](playback-engine.md)) | [#1320](https://github.com/mltframework/mlt/issues/1320) |
| `movit.luma_mix` squares the progress, so a wipe with a luma map stands still for its first frames (4 of 20); its map reaches movit at 8 bits (from the source). Filed as a question. | The GPU pipeline plays every recipe as a plain dissolve ([Effects](effects.md)) | [#1321](https://github.com/mltframework/mlt/issues/1321) |
| `movit.convert` leaks a `movit::Input` per input per frame whenever it reuses a chain (~10 MB a minute of GPU playback). | The Flatpak's MLT carries `packaging/flatpak/patches/mlt-movit-convert-input-leak.patch`; distro builds leak ([GPU](gpu.md)) | [#1322](https://github.com/mltframework/mlt/issues/1322); fix in pull request [#1323](https://github.com/mltframework/mlt/pull/1323) (from the `iDoMeteor/mlt` fork, now `unicorntearsproject/mlt`), verified on master |

## Not filed

Each needs a standalone repro before it goes upstream.

| Behaviour | Why not |
|---|---|
| `sdl2_audio` handed `on_consumer_frame_show` a freed frame right after a consumer restart (a SEGV in `mlt_frame_get_position`, seen once under ASan; [Playback engine](playback-engine.md)). | 600 restarts under ASan, standalone: clean |
| `play()` after a pause sometimes never started: stale speed-0 frames used up the refresh wake-ups (worked around in `PlaybackController::play()`). | 0 of 120 in two standalone variants |
| `sdl2_audio` shows frames the read-ahead thread skipped without checking `rendered` (the stop path and the paused refresh), so a GL user renders them off the render thread ([GPU](gpu.md)). | Arguably by design; seen once in 2.5 minutes under load |
| The `avfilter` filter leaks a properties object (with its strings) each time it sets up a filter graph: `init_image_filtergraph()` closes `p` only on its failure path, and the success path returns first (`filter_avfilter.c:431`, `:711`). Once per graph, not per frame; LSan suppression `leak:filter_get_image` (`tests/sanitizers/lsan.supp`), seen in the effects drop-in's `test_engine` LUT case under `just asan`. | Found 2026-09-29, after this round; not filed yet (needs a standalone repro and a check against master) |
| `pixbuf` ignores `video_index=-1`. We mark such frames `test_image` (`attachHideVideo()`). | Not a bug: `video_index` is an `avformat` property |
