// VoiceFX v5 for SA-MP Nezuko (AML 1.3.0, armeabi-v7a)
// Pitch / formant voice changer: hooks libBASS BASS_RecordStart (Dobby) and runs a BASS DSP on the mic channel.
//
// Differences from the original libvoicefx v4:
//  * __GetModInfo returns a real 0xB0-byte ModInfo (AML >= 1.0.x struct ABI), not a "guid|ver|name|author" string.
//  * No aml-> calls at all (the IAML vtable differs between AML versions), so nothing here can hit the vtable trap.
//  * Chunk-size independent: FIFO in front of RubberBand's fixed block size (original zero-filled remainders).
//  * Parameters are atomics applied inside the DSP thread (RubberBand forbids setPitchScale during shift()).
//  * Extras: preserve-formant, mic gain, noise gate, dry/wet, robot ring-mod, config file, live stats for the menu.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <math.h>
#include <atomic>
#include <mutex>
#include <vector>
#include <memory>
#include "rubberband/RubberBandLiveShifter.h"

#ifndef VFX_HOST_TEST
#include <dlfcn.h>
#include <android/log.h>
#endif

#define VFX_EXPORT extern "C" __attribute__((visibility("default")))
#define LOG_PATH "/storage/emulated/0/voicefx_log.txt"
#define CFG_PATH "/storage/emulated/0/voicefx.cfg"

// ------------------------------------------------------------------ logging
static std::atomic<int> g_logBudget{400};
static void vlog(const char* fmt, ...)
{
    if (g_logBudget.fetch_sub(1) <= 0) return;
    char buf[512];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
#ifndef VFX_HOST_TEST
    __android_log_print(ANDROID_LOG_INFO, "VoiceFX", "%s", buf);
    FILE* f = fopen(LOG_PATH, "a");
    if (f) { fprintf(f, "[VFX] %s\n", buf); fclose(f); }
#else
    fprintf(stderr, "[VFX] %s\n", buf);
#endif
}

// ------------------------------------------------------------------ parameters (written by menu, read by DSP thread)
enum { P_ENABLED, P_PITCH, P_FORMANT, P_PRESERVE, P_GAIN_DB, P_GATE_DB, P_MIX, P_ROBOT, P_ROBOT_HZ, P_COUNT };
static std::atomic<float> g_par[P_COUNT];
static void params_default()
{
    g_par[P_ENABLED] = 0.f;  g_par[P_PITCH] = 1.f;   g_par[P_FORMANT] = 1.f; g_par[P_PRESERVE] = 0.f;
    g_par[P_GAIN_DB] = 0.f;  g_par[P_GATE_DB] = -80.f; g_par[P_MIX] = 1.f;    g_par[P_ROBOT] = 0.f;
    g_par[P_ROBOT_HZ] = 80.f;
}
static struct ParamInit { ParamInit() { params_default(); } } g_paramInit;   // sane defaults even before OnModLoad
static float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
static float sanitize(int id, float v)
{
    if (!(v == v)) v = 0.f;   // NaN
    switch (id) {
        case P_ENABLED: case P_PRESERVE: case P_ROBOT: return v >= 0.5f ? 1.f : 0.f;
        case P_PITCH: case P_FORMANT: return clampf(v, 0.25f, 4.f);
        case P_GAIN_DB: return clampf(v, -24.f, 24.f);
        case P_GATE_DB: return clampf(v, -80.f, -20.f);
        case P_MIX: return clampf(v, 0.f, 1.f);
        case P_ROBOT_HZ: return clampf(v, 20.f, 500.f);
    }
    return v;
}
static const char* kParName[P_COUNT] = {"enabled","pitch","formant","preserve","gain_db","gate_db","mix","robot","robot_hz"};

// ------------------------------------------------------------------ stats
static std::atomic<int>   g_hookState{0};      // 0 none, 1 installed, -1 failed
static std::atomic<int>   g_recStarts{0}, g_dspCalls{0}, g_lastBytes{0}, g_rate{0}, g_fmtOK{0}, g_block{0};
static std::atomic<float> g_peakIn{0.f}, g_peakOut{0.f}, g_latMs{0.f};

// ------------------------------------------------------------------ DSP engine
namespace {
struct Ring {
    std::vector<float> b; size_t r = 0, w = 0, n = 0;
    void init(size_t cap) { b.assign(cap, 0.f); r = w = n = 0; }
    void clear() { r = w = n = 0; }
    void push(float v) { if (n == b.size()) { r = (r + 1) % b.size(); n--; } b[w] = v; w = (w + 1) % b.size(); n++; }
    float pop() { float v = b[r]; r = (r + 1) % b.size(); n--; return v; }
};

struct Engine {
    std::mutex mtx;
    std::unique_ptr<RubberBand::RubberBandLiveShifter> rb;
    size_t B = 0, rate = 0, delayD = 0;
    Ring inq, outq, dry;
    std::vector<float> inBlk, outBlk;
    float lastPitch = -1.f, lastForm = -1.f;
    bool shiftActive = false;
    float env = 0.f, gateG = 1.f, phase = 0.f;

    // (re)build for a sample rate. Called from the RecordStart hook thread, never from the DSP path.
    bool rebuild(size_t sr)
    {
        std::lock_guard<std::mutex> lk(mtx);
        rb.reset();
        rb.reset(new RubberBand::RubberBandLiveShifter(sr, 1,
                 RubberBand::RubberBandLiveShifter::OptionWindowShort |
                 RubberBand::RubberBandLiveShifter::OptionFormantShifted |
                 RubberBand::RubberBandLiveShifter::OptionChannelsApart));
        rate = sr; B = rb->getBlockSize();
        inBlk.assign(B, 0.f); outBlk.assign(B, 0.f);
        inq.init(65536); outq.init(65536);
        size_t sd = rb->getStartDelay(); delayD = B + sd; if (delayD > 16384) delayD = 16384;
        dry.init(delayD + 1);
        resetState();
        g_block = (int)B; g_rate = (int)sr;
        g_latMs = 1000.f * (float)delayD / (float)sr;
        return true;
    }
    void resetState()   // caller holds mtx (or is single-threaded)
    {
        if (rb) rb->reset();
        inq.clear(); outq.clear(); dry.clear();
        for (size_t i = 0; i < B; i++) outq.push(0.f);          // pre-roll so output never underruns
        for (size_t i = 0; i < delayD; i++) dry.push(0.f);
        lastPitch = lastForm = -1.f; shiftActive = false; env = 0.f; gateG = 1.f; phase = 0.f;
    }
    void reset() { std::lock_guard<std::mutex> lk(mtx); resetState(); }

    // in-place 16-bit mono processing. Any n.
    void process(int16_t* s, int n)
    {
        std::unique_lock<std::mutex> lk(mtx, std::try_to_lock);
        if (!lk.owns_lock() || !rb) return;                      // being rebuilt: pass audio through untouched
        const float pitch = g_par[P_PITCH], form = g_par[P_FORMANT];
        const bool preserve = g_par[P_PRESERVE] >= 0.5f;
        const float effForm = clampf(form * (preserve ? 1.f / pitch : 1.f), 0.1f, 8.f);
        const float gainLin = powf(10.f, g_par[P_GAIN_DB] / 20.f);
        const float gateOn = g_par[P_GATE_DB] > -79.5f ? 1.f : 0.f;
        const float gateThr = powf(10.f, g_par[P_GATE_DB] / 20.f);
        const float mix = g_par[P_MIX];
        const bool robot = g_par[P_ROBOT] >= 0.5f;
        const float robotInc = 2.f * 3.14159265f * g_par[P_ROBOT_HZ] / (float)rate;
        const bool needShift = fabsf(pitch - 1.f) > 1e-3f || fabsf(effForm - 1.f) > 1e-3f;

        if (needShift && !shiftActive) { rb->reset(); inq.clear(); outq.clear(); for (size_t i=0;i<B;i++) outq.push(0.f); shiftActive = true; }
        else if (!needShift && shiftActive) { shiftActive = false; }
        if (needShift) {
            if (pitch != lastPitch) { rb->setPitchScale(pitch); lastPitch = pitch; }
            if (effForm != lastForm) { rb->setFormantScale(effForm); lastForm = effForm; }
        }

        float pin = 0.f, pout = 0.f;
        // 1) input stage: int16 -> float, gate (smoothed), gain; push to FIFO
        for (int i = 0; i < n; i++) {
            float x = s[i] * (1.f / 32768.f);
            float ax = fabsf(x); if (ax > pin) pin = ax;
            if (gateOn > 0.f) {
                env = ax > env ? ax : env * 0.9995f;
                float tgt = env > gateThr ? 1.f : 0.f;
                gateG += (tgt - gateG) * (tgt > gateG ? 0.05f : 0.002f);
                x *= gateG;
            }
            x *= gainLin;
            dry.push(x);
            if (needShift) inq.push(x);
        }
        // 2) fixed-size RubberBand blocks
        if (needShift) {
            while (inq.n >= B) {
                for (size_t i = 0; i < B; i++) inBlk[i] = inq.pop();
                const float* ip[1] = { inBlk.data() }; float* op[1] = { outBlk.data() };
                rb->shift(ip, op);
                for (size_t i = 0; i < B; i++) outq.push(outBlk[i]);
            }
        }
        // 3) output stage
        for (int i = 0; i < n; i++) {
            float d = dry.pop();                                  // dry delayed to match wet latency
            float y;
            if (needShift) { float w = outq.n ? outq.pop() : 0.f; y = d * (1.f - mix) + w * mix; }
            else y = d;
            if (robot) { y *= sinf(phase); phase += robotInc; if (phase > 6.2831853f) phase -= 6.2831853f; }
            float ay = fabsf(y);
            if (ay > 0.9f) y = copysignf(0.9f + 0.1f * tanhf((ay - 0.9f) * 10.f), y);   // soft limiter
            if (fabsf(y) > pout) pout = fabsf(y);
            int v = (int)lrintf(y * 32767.f); if (v > 32767) v = 32767; if (v < -32768) v = -32768;
            s[i] = (int16_t)v;
        }
        if (!needShift) { /* keep dry delay line length constant: dry.pop above already balanced pushes */ }
        float pi = g_peakIn, po = g_peakOut;
        g_peakIn = pin > pi * 0.85f ? pin : pi * 0.85f;
        g_peakOut = pout > po * 0.85f ? pout : po * 0.85f;
    }
};
Engine g_eng;
}  // namespace

// Test hook / used by the BASS DSP callback.
extern "C" __attribute__((visibility("default"))) void vfx_process_i16(int16_t* buf, int n) { g_eng.process(buf, n); }
extern "C" __attribute__((visibility("default"))) void vfx_prepare(int rate) { g_eng.rebuild((size_t)rate); }

// ------------------------------------------------------------------ config file
static void cfg_save()
{
    FILE* f = fopen(CFG_PATH, "w"); if (!f) return;
    for (int i = 0; i < P_COUNT; i++) fprintf(f, "%s=%.4f\n", kParName[i], (double)g_par[i].load());
    fclose(f);
}
static void cfg_load()
{
    FILE* f = fopen(CFG_PATH, "r"); if (!f) return;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        char* eq = strchr(line, '='); if (!eq) continue; *eq = 0;
        for (int i = 0; i < P_COUNT; i++) if (!strcmp(line, kParName[i])) g_par[i] = sanitize(i, (float)atof(eq + 1));
    }
    fclose(f);
}

// ------------------------------------------------------------------ public C API for the menu (Lua FFI)
VFX_EXPORT float vfx_get(int id)  { return (id >= 0 && id < P_COUNT) ? g_par[id].load() : 0.f; }
VFX_EXPORT void  vfx_set(int id, float v) { if (id >= 0 && id < P_COUNT) g_par[id] = sanitize(id, v); }
VFX_EXPORT void  vfx_save(void) { cfg_save(); }
VFX_EXPORT void  vfx_load(void) { cfg_load(); }
// stat ids: 0 hook(1/0/-1) 1 recordStarts 2 dspCalls 3 lastChunkBytes 4 rate 5 formatOK 6 blockSize
VFX_EXPORT int   vfx_stat(int id)
{
    switch (id) { case 0: return g_hookState; case 1: return g_recStarts; case 2: return g_dspCalls;
                  case 3: return g_lastBytes; case 4: return g_rate; case 5: return g_fmtOK; case 6: return g_block; }
    return 0;
}
// float stat ids: 0 peakIn 1 peakOut 2 latencyMs
VFX_EXPORT float vfx_statf(int id) { return id == 0 ? g_peakIn.load() : id == 1 ? g_peakOut.load() : id == 2 ? g_latMs.load() : 0.f; }

// Backwards-compatible table used by the original v4 menus.
struct vc_api_t {
    void (*set_pitch)(float); void (*enable)(void); void (*disable)(void); int (*is_enabled)(void);
    float (*get_pitch)(void); void (*set_formant)(float); float (*get_formant)(void);
};
static void  _set_pitch(float v)   { vfx_set(P_PITCH, v); }
static void  _enable(void)         { vfx_set(P_ENABLED, 1.f); }
static void  _disable(void)        { vfx_set(P_ENABLED, 0.f); }
static int   _is_enabled(void)     { return g_par[P_ENABLED] >= 0.5f; }
static float _get_pitch(void)      { return g_par[P_PITCH]; }
static void  _set_formant(float v) { vfx_set(P_FORMANT, v); }
static float _get_formant(void)    { return g_par[P_FORMANT]; }
VFX_EXPORT vc_api_t vc_api = { _set_pitch, _enable, _disable, _is_enabled, _get_pitch, _set_formant, _get_formant };

// ------------------------------------------------------------------ AML entry points (ModInfo ABI)
struct ModInfo {                       // identical to AndroidModLoader mod/amlmod.h `class ModInfo` (0xB0 bytes)
    char szGUID[48]; char szName[48]; char szVersion[24]; char szAuthor[48];
    uint16_t major, minor, revision, build;
};
static_assert(sizeof(ModInfo) == 0xB0, "ModInfo layout");
static ModInfo g_modinfo = { "com.brruham.voicefx", "VoiceFX (RubberBand)", "5.0.0", "brruham", 5, 0, 0, 0 };
VFX_EXPORT ModInfo* __GetModInfo() { return &g_modinfo; }

#ifndef VFX_HOST_TEST
typedef uint32_t DWORD;
typedef void (*DSPPROC)(DWORD handle, DWORD channel, void* buffer, DWORD length, void* user);
typedef DWORD (*RecordStartFn)(DWORD freq, DWORD chans, DWORD flags, void* proc, void* user);
typedef DWORD (*SetDSPFn)(DWORD handle, DSPPROC proc, void* user, int priority);
typedef int   (*DobbyHookFn)(void* addr, void* replace, void** origin);
typedef void* (*DobbyResolveFn)(const char* image, const char* sym);

static RecordStartFn g_origRecStart = nullptr;
static SetDSPFn      g_setDSP = nullptr;
static std::atomic<int> g_hookBusy{0};

static void dsp_proc(DWORD, DWORD, void* buffer, DWORD length, void*)
{
    g_dspCalls++; g_lastBytes = (int)length;
    if (g_par[P_ENABLED] < 0.5f || !g_fmtOK.load() || length < 2 || length > 65536) return;
    g_eng.process((int16_t*)buffer, (int)(length / 2));
}

static DWORD hook_RecordStart(DWORD freq, DWORD chans, DWORD flags, void* proc, void* user)
{
    DWORD h = g_origRecStart(freq, chans, flags, proc, user);
    int n = ++g_recStarts;
    bool ok = (chans == 1) && !(flags & 0x101);              // mono, 16-bit (no BASS_SAMPLE_8BITS / FLOAT)
    g_fmtOK = ok ? 1 : 0;
    if (n <= 20) vlog("RecordStart #%d freq=%u chans=%u flags=0x%x proc=%p -> handle=%u fmtOK=%d", n, freq, chans, flags, proc, h, (int)ok);
    if (ok && (int)freq != g_rate.load()) g_eng.rebuild(freq);
    else if (ok) g_eng.reset();
    if (h && g_setDSP) {
        DWORD d = g_setDSP(h, dsp_proc, nullptr, 1);
        if (n <= 20) vlog("BASS_ChannelSetDSP(handle=%u) -> %u", h, d);
    }
    return h;
}

static void install_hook()
{
    if (g_hookState.load() == 1 || g_hookBusy.exchange(1)) return;
    void* dob = dlopen("libdobby.so", RTLD_NOW);
    if (!dob) { vlog("ERROR: dlopen libdobby.so failed: %s", dlerror()); g_hookState = -1; g_hookBusy = 0; return; }
    DobbyHookFn hookFn = (DobbyHookFn)dlsym(dob, "DobbyHook");
    DobbyResolveFn resolve = (DobbyResolveFn)dlsym(dob, "DobbySymbolResolver");
    // Use the same path Nezuko uses so we get the very same loaded copy of libBASS.
    void* bass = dlopen("/data/data/com.nezukosamp.game/lib/libBASS.so", RTLD_NOW);
    const char* how = "absolute path";
    if (!bass) { bass = dlopen("libBASS.so", RTLD_NOW); how = "soname"; }
    if (!bass || !hookFn) { vlog("ERROR: libBASS=%p DobbyHook=%p", bass, (void*)hookFn); g_hookState = -1; g_hookBusy = 0; return; }
    g_setDSP = (SetDSPFn)dlsym(bass, "BASS_ChannelSetDSP");
    void* target = dlsym(bass, "BASS_RecordStart");
    if (!target && resolve) target = resolve("libBASS.so", "BASS_RecordStart");
    vlog("libBASS via %s, BASS_RecordStart=%p BASS_ChannelSetDSP=%p", how, target, (void*)g_setDSP);
    if (!target || !g_setDSP) { g_hookState = -1; g_hookBusy = 0; return; }
    int r = hookFn(target, (void*)hook_RecordStart, (void**)&g_origRecStart);
    if (r != 0 || !g_origRecStart) { vlog("ERROR: DobbyHook returned %d", r); g_hookState = -1; g_hookBusy = 0; return; }
    g_hookState = 1; g_hookBusy = 0;
    vlog("RecordStart hook installed");
}
VFX_EXPORT int vfx_hook_retry(void) { install_hook(); return g_hookState; }

VFX_EXPORT void OnModPreLoad() { remove(LOG_PATH); vlog("OnModPreLoad v5.0.0"); }
VFX_EXPORT void OnModLoad()
{
    params_default(); cfg_load();
    vlog("OnModLoad (enabled=%.0f pitch=%.3f formant=%.3f)", (double)g_par[P_ENABLED].load(), (double)g_par[P_PITCH].load(), (double)g_par[P_FORMANT].load());
    g_eng.rebuild(48000);            // Nezuko records at 48000 Hz mono; rebuilt automatically if that ever changes
    install_hook();
}
VFX_EXPORT void OnAllModsLoaded() { if (g_hookState.load() != 1) { vlog("retrying hook after all mods loaded"); install_hook(); } }
#endif
