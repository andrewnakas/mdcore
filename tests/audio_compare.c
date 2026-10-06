/* Compares two raw stereo s16 recordings of the same session numerically,
 * without playing anything: mdcore at 53267 Hz, the reference at 44100 Hz.
 *   audio_compare md.raw md_rate ref.raw ref_rate [skip_seconds]
 * Mono mixes are resampled to 44100 Hz, aligned by cross-correlating 5 ms
 * RMS envelopes, then compared: envelope correlation, level difference in
 * dB, and the cosine similarity of 24 log-spaced spectral bands per 20 ms. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RATE 44100
static float *load(const char *p, double rate, long *n)
{
    FILE *f = fopen(p, "rb"); long bytes, frames, i; short *raw; float *out; long on;
    if (!f) { perror(p); exit(2); }
    fseek(f, 0, SEEK_END); bytes = ftell(f); fseek(f, 0, SEEK_SET);
    frames = bytes / 4; raw = malloc(bytes);
    if (fread(raw, 4, frames, f) != (size_t)frames) exit(2);
    fclose(f);
    on = (long)(frames * RATE / rate); out = malloc(on * sizeof(float));
    for (i = 0; i < on; i++) {
        double p2 = i * rate / RATE; long k = (long)p2; double fr = p2 - k;
        double a = k < frames ? (raw[2 * k] + raw[2 * k + 1]) * 0.5 : 0;
        double b = k + 1 < frames ? (raw[2 * k + 2] + raw[2 * k + 3]) * 0.5 : a;
        out[i] = (float)((a + (b - a) * fr) / 32768.0);
    }
    free(raw); *n = on; return out;
}

static void env(const float *x, long n, int win, double *e, long ne)
{
    long w, i;
    for (w = 0; w < ne; w++) { double s = 0; for (i = 0; i < win; i++) { double v = x[w * win + i]; s += v * v; } e[w] = sqrt(s / win); }
    (void)n;
}

static double corr(const double *a, const double *b, long n)
{
    double ma = 0, mb = 0, sab = 0, saa = 0, sbb = 0; long i;
    for (i = 0; i < n; i++) { ma += a[i]; mb += b[i]; }
    ma /= n; mb /= n;
    for (i = 0; i < n; i++) { sab += (a[i] - ma) * (b[i] - mb); saa += (a[i] - ma) * (a[i] - ma); sbb += (b[i] - mb) * (b[i] - mb); }
    return sab / sqrt(saa * sbb + 1e-30);
}

static void bands(const float *x, double *out)
{
    /* 24 log-spaced bands from 60 Hz to 16 kHz, by direct DFT of an 882-sample window */
    int b, k, N = 882;
    for (b = 0; b < 24; b++) {
        double lo = 60 * pow(16000.0 / 60, b / 24.0), hi = 60 * pow(16000.0 / 60, (b + 1) / 24.0), s = 0;
        int k0 = (int)(lo * N / RATE), k1 = (int)(hi * N / RATE); if (k1 <= k0) k1 = k0 + 1;
        for (k = k0; k < k1; k++) {
            double re = 0, im = 0; int t;
            for (t = 0; t < N; t++) { double w = 0.5 - 0.5 * cos(2 * M_PI * t / N); re += x[t] * w * cos(2 * M_PI * k * t / N); im -= x[t] * w * sin(2 * M_PI * k * t / N); }
            s += re * re + im * im;
        }
        out[b] = log10(1e-9 + s);
    }
}

int main(int argc, char **argv)
{
    long na, nb, ne, w, best = 0, lag, n, i;
    double bestc = -2, skip = argc > 5 ? atof(argv[5]) : 0;
    float *a = load(argv[1], atof(argv[2]), &na), *b = load(argv[3], atof(argv[4]), &nb);
    int win = RATE / 200;
    long sk = (long)(skip * RATE);
    double *ea, *eb;
    a += sk; b += sk; na -= sk; nb -= sk;
    n = (na < nb ? na : nb) - RATE; ne = n / win;
    ea = malloc(ne * sizeof(double)); eb = malloc(ne * sizeof(double));
    env(a, n, win, ea, ne); env(b, n, win, eb, ne);
    for (lag = -100; lag <= 100; lag++) {
        long m = ne - 200; double c;
        if (m < 10) break;
        c = corr(ea + 100, eb + 100 + lag, m);
        if (c > bestc) { bestc = c; best = lag; }
    }
    printf("aligned at %+ld ms; envelope correlation %.4f\n", best * 5, bestc);
    {
        double sd = 0, worst = 0, off = 0; long cnt = 0, over1 = 0, c0 = 0;
        for (w = 100; w < ne - 100; w++) {
            double x = ea[w], y = eb[w + best];
            if (x < 0.003 && y < 0.003) continue;
            off += 20 * log10((x + 1e-9) / (y + 1e-9)); c0++;
        }
        off = c0 ? off / c0 : 0;
        printf("overall gain: mdcore is %+.2f dB relative to the reference (removed below)\n", off);
        for (w = 100; w < ne - 100; w++) {
            double x = ea[w], y = eb[w + best], d;
            if (x < 0.003 && y < 0.003) continue; /* below -50 dB: silence */
            d = 20 * log10((x + 1e-9) / (y + 1e-9)) - off; sd += fabs(d); cnt++;
            if (fabs(d) > 1) over1++;
            if (fabs(d) > fabs(worst)) worst = d;
        }
        printf("level: mean |diff| %.2f dB over %ld windows, %.1f%% of windows off by more than 1 dB, worst %+.1f dB\n",
               cnt ? sd / cnt : 0, cnt, cnt ? 100.0 * over1 / cnt : 0, worst);
    }
    {
        double tot = 0; long cnt = 0; long step = RATE / 5;  /* 5 spectra per second */
        for (i = RATE; i + 882 < n - RATE; i += step) {
            double sa[24], sb[24], dot = 0, aa = 0, bb = 0, ma = 0, mb = 0; int k;
            long j = i + best * win;
            if (j < 0 || j + 882 > nb) continue;
            bands(a + i, sa); bands(b + j, sb);
            for (k = 0; k < 24; k++) { ma += sa[k]; mb += sb[k]; }
            ma /= 24; mb /= 24;
            for (k = 0; k < 24; k++) { dot += (sa[k] - ma) * (sb[k] - mb); aa += (sa[k] - ma) * (sa[k] - ma); bb += (sb[k] - mb) * (sb[k] - mb); }
            if (aa < 1e-6 || bb < 1e-6) continue;
            tot += dot / sqrt(aa * bb); cnt++;
        }
        printf("spectral shape: mean band correlation %.4f over %ld windows\n", cnt ? tot / cnt : 0, cnt);
    }
    return 0;
}
