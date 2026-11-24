/* Checks the APU's sound output: pitch of pulse and triangle, noise activity, $4011 steps, silence. */
#include "nes/apu.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define RATE 48000
static float buf[RATE * 2];
static int fails;

static void check(int ok, const char *what, double got)
{
    printf("%s: %s (%.2f)\n", ok ? "ok  " : "FAIL", what, got);
    fails += !ok;
}

/* Runs the APU for `seconds` from cycle *t, ticking every 4 cycles like a stream of short instructions. */
static size_t run(Apu *a, uint64_t *t, double seconds)
{
    uint64_t end = *t + (uint64_t)(seconds * APU_CPU_HZ);
    apu_set_buffer(a, buf, sizeof buf / sizeof buf[0]);
    for (; *t < end; *t += 4)
        apu_tick(a, *t);
    apu_render(a, *t);
    return apu_take(a);
}

static double crossing_freq(size_t n)
{
    size_t i, c = 0, first = 0, last = 0;
    for (i = 1 + RATE / 10; i < n; i++) /* skip the filters' settling time */
        if (buf[i - 1] < 0 && buf[i] >= 0) {
            if (!c)
                first = i;
            last = i;
            c++;
        }
    return c > 1 ? (double)(c - 1) * RATE / (double)(last - first) : 0;
}

/* Fundamental by autocorrelation: robust against the output high-pass filters' ringing on square waves. */
/* Fundamental by autocorrelation (robust against the output high-pass filters' ringing on square waves):
 * the shortest lag whose correlation is within 90% of the best one. */
static double autocorr_freq(size_t n)
{
    static double corr[RATE / 50 + 1];
    size_t lag, i, start = RATE / 10, len = RATE / 4;
    double best = 0;
    if (n < start + len + RATE / 50)
        return 0;
    for (lag = RATE / 2000; lag <= RATE / 50; lag++) {
        double s = 0;
        for (i = start; i < start + len; i++)
            s += (double)buf[i] * buf[i + lag];
        corr[lag] = s;
        if (s > best)
            best = s;
    }
    for (lag = RATE / 2000 + 1; lag < RATE / 50; lag++)
        if (corr[lag] >= 0.9 * best && corr[lag] >= corr[lag - 1] && corr[lag] >= corr[lag + 1])
            return (double)RATE / (double)lag;
    return 0;
}

static double rms(size_t n)
{
    double s = 0;
    size_t i;
    for (i = RATE / 10; i < n; i++)
        s += buf[i] * buf[i];
    return sqrt(s / (double)(n - RATE / 10));
}

int main(void)
{
    static Apu a;
    uint64_t t = 0;
    size_t n;
    int period = (int)(APU_CPU_HZ / (16.0 * 440.0) - 1 + 0.5); /* pulse: f = CPU / (16 (p + 1)) */

    apu_power(&a, RATE, NULL);
    n = run(&a, &t, 0.5);
    check(rms(n) < 1e-3, "silent after power-on", rms(n));

    apu_write(&a, t, 0x4015, 0x0F);
    apu_write(&a, t, 0x4000, 0xBF); /* 50% duty, constant volume 15 */
    apu_write(&a, t, 0x4002, (uint8_t)period);
    apu_write(&a, t, 0x4003, (uint8_t)(0xF8 | (period >> 8)));
    n = run(&a, &t, 1.0);
    check(fabs(autocorr_freq(n) - 440.0) < 5.0, "pulse 1 at 440 Hz (autocorrelation)", autocorr_freq(n));
    check(rms(n) > 0.05, "pulse is audible", rms(n));

    apu_write(&a, t, 0x4000, 0xB0); /* volume 0 */
    period = (int)(APU_CPU_HZ / (32.0 * 220.0) - 1 + 0.5); /* triangle: f = CPU / (32 (p + 1)) */
    apu_write(&a, t, 0x4008, 0xFF);
    apu_write(&a, t, 0x400A, (uint8_t)period);
    apu_write(&a, t, 0x400B, (uint8_t)(0xF8 | (period >> 8)));
    n = run(&a, &t, 1.0);
    check(fabs(crossing_freq(n) - 220.0) < 2.0, "triangle at 220 Hz", crossing_freq(n));

    apu_write(&a, t, 0x4008, 0x00);   /* linear counter 0: triangle stops */
    apu_write(&a, t, 0x400B, 0x08);
    apu_write(&a, t, 0x400C, 0x3F);   /* noise, constant volume 15 */
    apu_write(&a, t, 0x400E, 0x04);
    apu_write(&a, t, 0x400F, 0xF8);
    n = run(&a, &t, 0.5);
    check(rms(n) > 0.02, "noise is audible", rms(n));

    apu_write(&a, t, 0x4015, 0x00);   /* all channels off */
    n = run(&a, &t, 0.5);
    check(rms(n) < 0.002, "channels off: silence", rms(n));

    /* raw PCM: a $4011 square wave at 1 kHz from timed writes */
    {
        uint64_t end = t + (uint64_t)APU_CPU_HZ, half = (uint64_t)(APU_CPU_HZ / 2000.0), next = t;
        int hi = 0;
        apu_set_buffer(&a, buf, sizeof buf / sizeof buf[0]);
        for (; t < end; t += 4) {
            if (t >= next) {
                apu_write(&a, t, 0x4011, hi ? 0x60 : 0x20);
                hi ^= 1;
                next += half;
            }
            apu_tick(&a, t);
        }
        apu_render(&a, t);
        n = apu_take(&a);
        check(fabs(crossing_freq(n) - 1000.0) < 5.0, "$4011 writes at 1 kHz", crossing_freq(n));
    }
    return fails ? 1 : 0;
}
