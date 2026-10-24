# VoiceFX v5 for SA-MP Nezuko (com.nezukosamp.game 1.0.3)

## What changed vs the original libvoicefx
| Problem in the original | Fix |
|---|---|
| `__GetModInfo` returned a string ("libvoicefx\|4.0\|..."), AML 1.3.0 expects a `ModInfo*` struct | Real 0xB0-byte `ModInfo` (guid/name/version/author + 4 x uint16) |
| Fixed 512-frame RubberBand blocks, remainder zero-filled (choppy/silent audio unless BASS chunk is a multiple of 512) | FIFO + pre-roll; tested with 100-sample and random chunks: 0 dropouts, output identical to fixed chunks |
| `setPitchScale` called from the menu thread while `shift()` runs (forbidden by RubberBand) | Parameters are atomics, applied inside the DSP callback |
| No format check | Only processes mono 16-bit (what Nezuko uses: `BASS_RecordStart(48000,1,0,NULL,0)`) |
| Only pitch/formant | + preserve-formant, mic gain, noise gate, dry/wet, robot, config file, live stats |

`vc_api` (7 function pointers, same order) is still exported for old menus.

## Build (GitHub Actions, same as your original)
1. Create/replace the repo contents with this folder (`jni/`, `.github/`). RubberBand 4.0.0 is downloaded by the workflow.
2. Push -> Actions -> `build-libvoicefx` -> download artifact `libvoicefx` (`libvoicefx.so`, armeabi-v7a).
Local alternative: `ndk-build NDK_PROJECT_PATH=. APP_BUILD_SCRIPT=jni/Android.mk NDK_APPLICATION_MK=jni/Application.mk`
(put RubberBand 4.0.0 at `jni/rubberband-src/`).

## Install
1. Launcher -> Settings -> **Modloader ON**.
2. Copy `libvoicefx.so` to `Android/data/com.nezukosamp.game/mods/` (delete the old libvoicefx.so).
3. Copy `VoiceFX_Menu.lua` to the same folder your other working Lua scripts use.
4. Start the game, join a server with voice, hold the mic button once.

## Test plan (what proves it works)
1. `/storage/emulated/0/voicefx_log.txt` contains `OnModLoad`, `libBASS via ...`, `RecordStart hook installed`.
2. Menu status shows **BASS hook: installed**. After pressing the mic: `Mic sessions` >= 1 and `DSP calls` keeps increasing, "Mic in/out" bars move.
3. Enable + preset: other players hear the changed voice.

## Not verified (static analysis only, nothing was run on a device)
* That BASS runs a DSP on a *polled* recording channel (Nezuko passes no RECORDPROC). If Mic sessions > 0 but DSP calls stays 0, that is the answer; send me the log.
* Menu API names (`imgui.*`, optional `sampRegisterChatCommand`) follow the skill's template; compare with one script that already works for you.
* Latency is ~55 ms (RubberBand live shifter), inherent to the library.
* Voice servers may reject/penalise voice changers; use within server rules.

RubberBand is GPL-2.0-or-later; the built library inherits that.
