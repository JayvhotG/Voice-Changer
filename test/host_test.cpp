#define VFX_HOST_TEST 1
#include "../jni/voicefx.cpp"
#include <random>
static std::vector<int16_t> run(const std::vector<int16_t>& in, const std::vector<int>& chunks)
{
    std::vector<int16_t> out = in; size_t pos = 0, ci = 0;
    vfx_prepare(48000);
    while (pos < out.size()) {
        int n = chunks[ci++ % chunks.size()]; if (pos + n > out.size()) n = (int)(out.size() - pos);
        vfx_process_i16(out.data() + pos, n); pos += n;
    }
    return out;
}
static double freqOf(const std::vector<int16_t>& x, size_t a, size_t b)  // zero-crossing estimate
{
    int zc = 0; for (size_t i = a + 1; i < b; i++) if ((x[i-1] < 0) != (x[i] < 0)) zc++;
    return zc / 2.0 / ((b - a) / 48000.0);
}
int main()
{
    params_default();
    const int N = 48000 * 3; std::vector<int16_t> in(N);
    for (int i = 0; i < N; i++) in[i] = (int16_t)(12000 * sinf(2 * 3.14159265f * 440.f * i / 48000.f));

    // 1) disabled path is not tested here (dsp_proc gates it); process() with no shift = delayed passthrough
    vfx_set(P_PITCH, 2.0f);
    auto a = run(in, {960});
    auto b = run(in, {137, 480, 1000, 251, 7, 2048, 333});
    size_t diff = 0; for (int i = 0; i < N; i++) if (a[i] != b[i]) diff++;
    printf("chunk-invariance: %zu/%d samples differ (960 vs random chunks)\n", diff, N);
    printf("pitch x2.0 : 440 Hz in -> %.1f Hz out (expect ~880)\n", freqOf(a, 48000, 48000 * 3 - 4800));
    // dropouts: find 5ms windows of near-silence after 1s warmup
    int silent = 0, wins = 0;
    for (size_t i = 48000; i + 240 < (size_t)N; i += 240) { int mx = 0; for (int k = 0; k < 240; k++) mx = std::max(mx, abs((int)b[i+k])); wins++; if (mx < 800) silent++; }
    printf("dropout windows (5ms, random chunks): %d / %d silent\n", silent, wins);
    // tiny chunks smaller than a RubberBand block
    auto c = run(in, {100});
    silent = wins = 0;
    for (size_t i = 48000; i + 240 < (size_t)N; i += 240) { int mx = 0; for (int k = 0; k < 240; k++) mx = std::max(mx, abs((int)c[i+k])); wins++; if (mx < 800) silent++; }
    printf("tiny 100-sample chunks: %d / %d silent windows, freq %.1f Hz\n", silent, wins, freqOf(c, 48000, 48000*3-4800));

    vfx_set(P_PITCH, 0.5f); auto d = run(in, {960});
    printf("pitch x0.5 : -> %.1f Hz (expect ~220)\n", freqOf(d, 48000, 48000*3-4800));

    // 2) pure passthrough/gain/limiter
    vfx_set(P_PITCH, 1.f); vfx_set(P_GAIN_DB, 6.f); auto e = run(in, {960});
    int pk = 0; for (size_t i = 48000; i < (size_t)N; i++) pk = std::max(pk, abs((int)e[i]));
    printf("+6 dB, no shift: peak %d (in 12000 -> ~24000 pre-limiter, limiter above 0.9=29491)\n", pk);
    // 3) gate closes on quiet signal
    vfx_set(P_GAIN_DB, 0.f); vfx_set(P_GATE_DB, -30.f);
    std::vector<int16_t> q(N); for (int i = 0; i < N; i++) q[i] = (int16_t)(300 * sinf(2*3.14159265f*440.f*i/48000.f)); // ~ -40 dBFS
    auto g = run(q, {960}); int gp = 0; for (size_t i = 24000; i < (size_t)N; i++) gp = std::max(gp, abs((int)g[i]));
    printf("gate -30 dB on -40 dB tone: out peak %d (expect ~0)\n", gp);
    // 4) formant / preserve doesn't crash and stays in range
    vfx_set(P_GATE_DB, -80.f); vfx_set(P_PITCH, 1.5f); vfx_set(P_PRESERVE, 1.f); vfx_set(P_FORMANT, 1.2f);
    auto h = run(in, {960}); pk = 0; for (size_t i = 48000; i < (size_t)N; i++) pk = std::max(pk, abs((int)h[i]));
    printf("pitch1.5 preserve formant1.2: peak %d, freq %.1f (expect ~660)\n", pk, freqOf(h, 48000, 48000*3-4800));
    printf("block=%d latency=%.1f ms\n", vfx_stat(6), (double)vfx_statf(2));
    return 0;
}
