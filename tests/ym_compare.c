/* Per-test comparison for probes/ym/ymtest.gen: test k sounds from frame
 * 60 + 60k for 40 frames. Prints, per test: peak level of each core (dB
 * full scale), the level difference, envelope correlation and spectral
 * band correlation of the sounding part, and the zero-crossing rate (pitch).
 *   ym_compare md.raw ref.raw tests.txt */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define RATE 44100
#define FPS 59.9227
static float *load(const char *p, double rate, long *n)
{
    FILE *f = fopen(p, "rb"); long bytes, frames, i, on; short *raw; float *out;
    fseek(f, 0, SEEK_END); bytes = ftell(f); fseek(f, 0, SEEK_SET);
    frames = bytes / 4; raw = malloc(bytes); if (fread(raw, 4, frames, f) != (size_t)frames) exit(2); fclose(f);
    on = (long)(frames * RATE / rate); out = malloc(on * sizeof(float));
    for (i = 0; i < on; i++) {
        double p2 = i * rate / RATE; long k = (long)p2; double fr = p2 - k;
        double a = k < frames ? (raw[2 * k] + raw[2 * k + 1]) * 0.5 : 0, b = k + 1 < frames ? (raw[2 * k + 2] + raw[2 * k + 3]) * 0.5 : a;
        out[i] = (float)((a + (b - a) * fr) / 32768.0);
    }
    *n = on; return out;
}
static void bands(const float *x, double *out)
{
    int b, k, N = 882;
    for (b = 0; b < 24; b++) {
        double lo = 60 * pow(16000.0 / 60, b / 24.0), hi = 60 * pow(16000.0 / 60, (b + 1) / 24.0), s = 0;
        int k0 = (int)(lo * N / RATE), k1 = (int)(hi * N / RATE); if (k1 <= k0) k1 = k0 + 1;
        for (k = k0; k < k1; k++) { double re = 0, im = 0; int t;
            for (t = 0; t < N; t++) { double w = 0.5 - 0.5 * cos(2 * M_PI * t / N); re += x[t] * w * cos(2 * M_PI * k * t / N); im -= x[t] * w * sin(2 * M_PI * k * t / N); }
            s += re * re + im * im; }
        out[b] = log10(1e-12 + s);
    }
}
static double c2(const double *a, const double *b, int n)
{ double ma = 0, mb = 0, s = 0, aa = 0, bb = 0; int i; for (i = 0; i < n; i++) { ma += a[i]; mb += b[i]; } ma /= n; mb /= n;
  for (i = 0; i < n; i++) { s += (a[i]-ma)*(b[i]-mb); aa += (a[i]-ma)*(a[i]-ma); bb += (b[i]-mb)*(b[i]-mb); } return s / sqrt(aa*bb + 1e-30); }
int main(int argc, char **argv)
{
    long na, nb; float *a = load(argv[1], 53267.04, &na), *b = load(argv[2], 44100, &nb);
    FILE *tf = fopen(argv[3], "r"); char name[64]; int k = 0;
    {   /* align the two streams: cross-correlate 1 ms envelopes over +-100 ms */
        long ne = (na < nb ? na : nb) / 44 - 200, w, i, lag, best = 0; double bc = -2;
        double *ea = malloc(ne * sizeof(double)), *eb = malloc(ne * sizeof(double));
        for (w = 0; w < ne; w++) { double x = 0, y = 0; for (i = 0; i < 44; i++) { x += a[w*44+i]*a[w*44+i]; y += b[w*44+i]*b[w*44+i]; } ea[w] = sqrt(x); eb[w] = sqrt(y); }
        for (lag = -100; lag <= 100; lag++) { double c = c2(ea + 100, eb + 100 + lag, (int)(ne - 200)); if (c > bc) { bc = c; best = lag; } }
        printf("streams aligned: reference lags by %+ld ms (correlation %.4f)\n", best, bc);
        if (best > 0) { b += best * 44; nb -= best * 44; } else { a -= best * 44; na += best * 44; }
    }
    printf("%-10s %8s %8s %7s %7s %7s %6s %6s\n", "test", "md dBFS", "ref dBFS", "diff", "envcor", "spec", "zc md", "zc ref");
    while (fscanf(tf, "%63s", name) == 1) {
        long s0 = (long)((60 + 60 * k) / FPS * RATE), len = (long)(58 / FPS * RATE), w, i;
        int nw = (int)(len / 220); double ea[400], eb[400], pa = 0, pb = 0; long za = 0, zb = 0;
        if (s0 + len >= na || s0 + len >= nb) break;
        for (w = 0; w < nw; w++) { double sa = 0, sb = 0; for (i = 0; i < 220; i++) { double x = a[s0 + w*220 + i], y = b[s0 + w*220 + i]; sa += x*x; sb += y*y; }
            ea[w] = sqrt(sa / 220); eb[w] = sqrt(sb / 220); if (ea[w] > pa) pa = ea[w]; if (eb[w] > pb) pb = eb[w]; }
        for (i = 1; i < (long)(0.5 * RATE); i++) { if ((a[s0+i] > 0) != (a[s0+i-1] > 0)) za++; if ((b[s0+i] > 0) != (b[s0+i-1] > 0)) zb++; }
        { double sa[24], sb[24], sp = 0; int q, cnt = 0;
          for (q = 0; q < 4; q++) { long o = s0 + (long)(0.05 * RATE) + q * (long)(0.1 * RATE); bands(a + o, sa); bands(b + o, sb); sp += c2(sa, sb, 24); cnt++; }
          printf("%-10s %8.1f %8.1f %+7.2f %7.4f %7.4f %6ld %6ld\n", name, 20*log10(pa+1e-9), 20*log10(pb+1e-9), 20*log10((pa+1e-9)/(pb+1e-9)), c2(ea, eb, nw), sp / cnt, za, zb); }
        k++;
    }
    return 0;
}
