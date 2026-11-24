/* 2A03 APU. See apu.h for the two layers (FCEUX-matching state, cycle-clocked synthesis). */
#include "apu.h"
#include <math.h>
#include <string.h>

#define TWO_PI 6.283185307179586
#define STEP_PERIOD (14915 * 24) /* frame counter step, in 1/48 CPU cycles (7457.5 cycles) */

static const uint8_t length_table[32] = {
    10, 254, 20, 2,  40, 4,  80, 6,  160, 8,  60, 10, 14, 12, 26, 14,
    12, 16,  24, 18, 48, 20, 96, 22, 192, 24, 72, 26, 16, 28, 32, 30,
};
static const uint16_t noise_periods[16] = {
    4, 8, 16, 32, 64, 96, 128, 160, 202, 254, 380, 508, 762, 1016, 2034, 4068,
};
static const uint16_t dmc_periods[16] = {
    428, 380, 340, 320, 286, 254, 226, 214, 190, 160, 142, 128, 106, 84, 72, 54,
};
static const uint8_t duty_wave[4][8] = {
    {0, 1, 0, 0, 0, 0, 0, 0},
    {0, 1, 1, 0, 0, 0, 0, 0},
    {0, 1, 1, 1, 1, 0, 0, 0},
    {1, 0, 0, 1, 1, 1, 1, 1},
};

static float pulse_mix[31];
static float tnd_mix[203];
static int mix_ready;

static void init_mix(void)
{
    int i;
    if (mix_ready)
        return;
    pulse_mix[0] = 0;
    for (i = 1; i < 31; i++)
        pulse_mix[i] = (float)(95.52 / (8128.0 / i + 100.0));
    tnd_mix[0] = 0;
    for (i = 1; i < 203; i++)
        tnd_mix[i] = (float)(163.67 / (24329.0 / i + 100.0));
    mix_ready = 1;
}

/* ---- helpers shared by both layers ---- */

static int env_volume(const ApuEnvelope *e)
{
    return (e->mode & 1) ? e->speed : e->decay;
}

/* Sweep unit silences the channel when the target period would overflow (only for "add" sweeps). */
static int sweep_ok(int32_t period, uint8_t sweep_reg)
{
    if (!(sweep_reg & 0x08)) {
        uint32_t mod = (uint32_t)period >> (sweep_reg & 7);
        if ((mod + (uint32_t)period) & 0x800)
            return 0;
    }
    return 1;
}

static int pulse_audible(const Apu *a, int x)
{
    return a->period[x] >= 8 && a->period[x] <= 0x7FF && sweep_ok(a->period[x], a->regs[x * 4 + 1]) &&
           a->length[x] != 0;
}

static void update_irq(Apu *a)
{
    if (a->host.irq)
        a->host.irq(a->host.ctx, (a->irq_status & 0x40) != 0, (a->irq_status & 0x80) != 0);
}

/* ---- synthesis ---- */

static float mix_now(const Apu *a)
{
    int p = 0, t = 0, n = 0, x;
    for (x = 0; x < 2; x++)
        if (pulse_audible(a, x) && duty_wave[a->regs[x * 4] >> 6][a->pulse_pos[x]])
            p += env_volume(&a->env[x]);
    {
        int pos = a->tri_pos;
        t = pos < 16 ? 15 - pos : pos - 16;
    }
    if (a->length[3] && !(a->noise_lfsr & 1))
        n = env_volume(&a->env[2]);
    return pulse_mix[p] + tnd_mix[3 * t + 2 * n + a->dmc_level];
}

static void emit(Apu *a, float x)
{
    /* console output stage: high-pass 90 Hz, high-pass 440 Hz, low-pass 14 kHz */
    float y1 = a->hp1_k * (a->hp1_prev_out + x - a->hp1_prev_in);
    float y2, y3;
    a->hp1_prev_in = x;
    a->hp1_prev_out = y1;
    y2 = a->hp2_k * (a->hp2_prev_out + y1 - a->hp2_prev_in);
    a->hp2_prev_in = y1;
    a->hp2_prev_out = y2;
    y3 = a->lp_prev + a->lp_k * (y2 - a->lp_prev);
    a->lp_prev = y3;
    y3 *= 1.75f; /* full-scale game music peaks around 0.6 before this gain */
    if (y3 > 1.0f)
        y3 = 1.0f;
    if (y3 < -1.0f)
        y3 = -1.0f;
    if (a->out_len < a->out_cap)
        a->out[a->out_len++] = y3;
    else
        a->samples_dropped++;
}

void apu_render(Apu *a, uint64_t to)
{
    if (!a->out) { /* no audio wanted: only keep the clock */
        a->synth_cycle = to;
        return;
    }
    while (a->synth_cycle < to) {
        int x;
        /* clock timers for this CPU cycle */
        for (x = 0; x < 2; x++) {
            if (--a->pulse_timer[x] <= 0) {
                a->pulse_timer[x] = (a->period[x] + 1) * 2;
                a->pulse_pos[x] = (uint8_t)((a->pulse_pos[x] + 1) & 7);
            }
        }
        if (--a->tri_timer <= 0) {
            int32_t per = a->regs[0xA] | ((a->regs[0xB] & 7) << 8);
            a->tri_timer = per + 1;
            if (a->length[2] && a->linear && per >= 2) /* hold ultrasonic periods instead of aliasing */
                a->tri_pos = (uint8_t)((a->tri_pos + 1) & 31);
        }
        if (--a->noise_timer <= 0) {
            int tap = (a->regs[0xE] & 0x80) ? 6 : 1;
            uint16_t fb = (uint16_t)((a->noise_lfsr ^ (a->noise_lfsr >> tap)) & 1);
            a->noise_timer = noise_periods[a->regs[0xE] & 15];
            a->noise_lfsr = (uint16_t)((a->noise_lfsr >> 1) | (fb << 14));
        }
        a->sum += mix_now(a);
        a->sum_n++;
        a->out_acc += 1.0;
        a->synth_cycle++;
        if (a->out_acc >= a->out_step) {
            /* box filter over the output period: averages away content above the output rate */
            emit(a, (float)(a->sum / a->sum_n));
            a->out_acc -= a->out_step;
            a->sum = 0;
            a->sum_n = 0;
        }
    }
}

/* ---- frame counter (FCEUX order) ---- */

static void quarter_and_half(Apu *a, int step)
{
    int x;
    if (!(step & 1)) { /* length counters and sweeps on steps 0 and 2 */
        if (!(a->regs[8] & 0x80) && a->length[2] > 0)
            a->length[2]--;
        if (!(a->regs[0xC] & 0x20) && a->length[3] > 0)
            a->length[3]--;
        for (x = 0; x < 2; x++) {
            uint8_t sw = a->regs[x * 4 + 1];
            if (!(a->regs[x * 4] & 0x20) && a->length[x] > 0)
                a->length[x]--;
            if (a->sweep_count[x] > 0)
                a->sweep_count[x]--;
            if (a->sweep_count[x] == 0) {
                int shift = sw & 7;
                if (a->sweep_on[x] && shift && a->period[x] >= 8) {
                    int32_t mod = a->period[x] >> shift;
                    if (sw & 0x08)
                        a->period[x] -= mod + (x ^ 1);
                    else if (mod + a->period[x] < 0x800)
                        a->period[x] += mod;
                }
                a->sweep_count[x] = (uint8_t)(((sw >> 4) & 7) + 1);
            }
            if (a->sweep_reload[x]) {
                a->sweep_count[x] = (uint8_t)(((sw >> 4) & 7) + 1);
                a->sweep_reload[x] = 0;
            }
        }
    }
    /* linear counter and envelopes on every step */
    if (a->linear_reload)
        a->linear = a->regs[8] & 0x7F;
    else if (a->linear)
        a->linear--;
    if (!(a->regs[8] & 0x80))
        a->linear_reload = 0;
    for (x = 0; x < 3; x++) {
        ApuEnvelope *e = &a->env[x];
        if (e->restart) {
            e->decay = 15;
            e->divider = (uint8_t)(e->speed + 1);
            e->restart = 0;
            continue;
        }
        if (e->divider > 0)
            e->divider--;
        if (e->divider == 0) {
            e->divider = (uint8_t)(e->speed + 1);
            if (e->decay || (e->mode & 2))
                e->decay = (uint8_t)((e->decay - 1) & 15);
        }
    }
}

static void frame_step(Apu *a)
{
    if (a->step == 0 && !(a->frame_mode & 3)) {
        a->irq_status |= 0x40;
        update_irq(a);
    }
    if (a->step == 3 && (a->frame_mode & 2))
        a->step_timer += STEP_PERIOD; /* 5-step mode: one extra step's worth of delay */
    quarter_and_half(a, a->step);
    a->step = (uint8_t)((a->step + 1) & 3);
}

/* ---- DMC ---- */

static void dmc_restart(Apu *a)
{
    a->dmc_addr = 0x4000u + ((uint32_t)a->dmc_addr_latch << 6);
    a->dmc_remaining = ((int32_t)a->dmc_size_latch << 4) + 1;
}

static void dmc_fetch(Apu *a)
{
    if (a->dmc_remaining && !a->dmc_buffer_full) {
        uint16_t addr = (uint16_t)(0x8000 + a->dmc_addr);
        if (a->host.stall)
            a->host.stall(a->host.ctx, 4); /* FCEUX: four bus reads, one cycle each */
        a->dmc_buffer = a->host.read ? a->host.read(a->host.ctx, addr) : 0;
        a->dmc_buffer_full = 1;
        a->dmc_addr = (a->dmc_addr + 1) & 0x7FFF;
        a->dmc_remaining--;
        if (!a->dmc_remaining) {
            if (a->dmc_format & 0x40) {
                dmc_restart(a);
            } else if (a->dmc_format & 0x80) {
                a->irq_status |= 0x80;
                update_irq(a);
            }
        }
    }
}

static void dmc_clock(Apu *a, int32_t cycles)
{
    a->dmc_timer -= cycles;
    while (a->dmc_timer <= 0) {
        if (a->dmc_playing) {
            uint8_t before = a->dmc_level;
            a->dmc_level = (uint8_t)(a->dmc_level + ((a->dmc_shift & 1) << 2) - 2);
            if (a->dmc_level & 0x80)
                a->dmc_level = before;
        }
        a->dmc_timer += a->dmc_period;
        a->dmc_bits = (uint8_t)((a->dmc_bits + 1) & 7);
        a->dmc_shift >>= 1;
        if (a->dmc_bits == 0) {
            if (!a->dmc_buffer_full) {
                a->dmc_playing = 0;
            } else {
                a->dmc_playing = 1;
                a->dmc_shift = a->dmc_buffer;
                a->dmc_buffer_full = 0;
            }
        }
    }
}

/* ---- public ---- */

void apu_power(Apu *a, int sample_rate, const ApuHost *host)
{
    double dt;
    init_mix();
    memset(a, 0, sizeof *a);
    if (host)
        a->host = *host;
    a->step_timer = STEP_PERIOD;
    a->dmc_timer = 1;
    a->dmc_period = dmc_periods[0];
    a->noise_lfsr = 1;
    a->pulse_timer[0] = a->pulse_timer[1] = 2;
    a->tri_timer = a->noise_timer = 1;
    a->sample_rate = sample_rate > 0 ? sample_rate : 48000;
    a->out_step = APU_CPU_HZ / a->sample_rate;
    dt = 1.0 / a->sample_rate;
    a->hp1_k = (float)((1.0 / (TWO_PI * 90)) / (1.0 / (TWO_PI * 90) + dt));
    a->hp2_k = (float)((1.0 / (TWO_PI * 440)) / (1.0 / (TWO_PI * 440) + dt));
    a->lp_k = (float)(dt / (1.0 / (TWO_PI * 14000) + dt));
}

void apu_set_buffer(Apu *a, float *buf, size_t cap)
{
    a->out = buf;
    a->out_cap = cap;
    a->out_len = 0;
}

size_t apu_take(Apu *a)
{
    size_t n = a->out_len;
    a->out_len = 0;
    return n;
}

void apu_tick(Apu *a, uint64_t now)
{
    int32_t cycles = (int32_t)(now - a->last_tick);
    a->last_tick = now;
    apu_render(a, now);
    a->step_timer -= cycles * 48;
    if (a->step_timer <= 0) {
        frame_step(a);
        a->step_timer += STEP_PERIOD;
    }
    dmc_fetch(a);
    dmc_clock(a, cycles);
}

void apu_write(Apu *a, uint64_t now, uint16_t addr, uint8_t v)
{
    unsigned r = addr - 0x4000u;

    apu_render(a, now);
    if (r < 0x10) {
        switch (r) {
        case 0x0:
        case 0x4:
        case 0xC: {
            ApuEnvelope *e = &a->env[r == 0xC ? 2 : r >> 2];
            e->mode = (uint8_t)((v & 0x30) >> 4);
            e->speed = v & 0x0F;
            break;
        }
        case 0x1:
        case 0x5:
            a->sweep_on[r >> 2] = v & 0x80;
            a->sweep_reload[r >> 2] = 1;
            break;
        case 0x2:
        case 0x6:
            a->period[r >> 2] = (a->period[r >> 2] & 0xFF00) | v;
            break;
        case 0x3:
        case 0x7: {
            int x = (int)(r >> 2);
            if (a->enabled & (1 << x))
                a->length[x] = length_table[v >> 3];
            a->period[x] = (a->period[x] & 0xFF) | ((v & 7) << 8);
            a->pulse_pos[x] = 0;
            a->env[x].restart = 1;
            break;
        }
        case 0xB:
            if (a->enabled & 0x04)
                a->length[2] = length_table[v >> 3];
            a->linear_reload = 1;
            break;
        case 0xF:
            if (a->enabled & 0x08)
                a->length[3] = length_table[v >> 3];
            a->env[2].restart = 1;
            break;
        default:
            break;
        }
        a->regs[r] = v;
        return;
    }
    switch (addr) {
    case 0x4010:
        a->dmc_period = dmc_periods[v & 0x0F];
        if (a->irq_status & 0x80) {
            if (!(v & 0x80))
                a->irq_status &= (uint8_t)~0x80;
            update_irq(a);
        }
        a->dmc_format = v;
        break;
    case 0x4011:
        a->dmc_level = a->dmc_level_written = v & 0x7F;
        break;
    case 0x4012:
        a->dmc_addr_latch = v;
        break;
    case 0x4013:
        a->dmc_size_latch = v;
        break;
    case 0x4015: {
        int x;
        for (x = 0; x < 4; x++)
            if (!(v & (1 << x)))
                a->length[x] = 0;
        if (v & 0x10) {
            if (!a->dmc_remaining)
                dmc_restart(a);
        } else {
            a->dmc_remaining = 0;
        }
        a->irq_status &= (uint8_t)~0x80;
        update_irq(a);
        a->enabled = v & 0x1F;
        break;
    }
    case 0x4017:
        v = (uint8_t)((v & 0xC0) >> 6);
        a->step = 0;
        if (v & 2)
            frame_step(a);
        a->step = 1;
        a->step_timer = STEP_PERIOD;
        a->irq_status &= (uint8_t)~0x40;
        update_irq(a);
        a->frame_mode = v;
        break;
    default:
        break;
    }
}

uint8_t apu_read_status(Apu *a, uint64_t now)
{
    uint8_t ret = a->irq_status;
    int x;
    (void)now;
    for (x = 0; x < 4; x++)
        if (a->length[x])
            ret |= (uint8_t)(1 << x);
    if (a->dmc_remaining)
        ret |= 0x10;
    a->irq_status &= (uint8_t)~0x40;
    update_irq(a);
    return ret;
}

static double midikey(double freq)
{
    return log(freq / 440.0) * 12 / log(2.0) + 69;
}

void apu_view(const Apu *a, ApuView *v)
{
    int x;
    for (x = 0; x < 2; x++) {
        v->sq_vol[x] = pulse_audible(a, x) ? env_volume(&a->env[x]) / 15.0 : 0.0;
        v->sq_key[x] = midikey(APU_CPU_HZ / 16.0 / (a->period[x] + 1));
        v->sq_duty[x] = a->regs[x * 4] >> 6;
    }
    v->tri_vol = (a->length[2] == 0 || a->linear == 0) ? 0.0 : 1.0;
    v->tri_key = midikey(APU_CPU_HZ / 32.0 / ((a->regs[0xA] | ((a->regs[0xB] & 7) << 8)) + 1));
    v->noise_vol = a->length[3] == 0 ? 0.0 : env_volume(&a->env[2]) / 15.0;
    v->noise_short = (a->regs[0xE] & 0x80) != 0;
    v->noise_key = midikey(APU_CPU_HZ / noise_periods[a->regs[0xE] & 15] / (v->noise_short ? 93.0 : 1.0));
    v->dmc_vol = a->dmc_playing ? 1 : 0;
    v->dmc_addr = 0xC000 + (a->dmc_addr_latch << 6);
    v->dmc_size = (a->dmc_size_latch << 4) + 1;
}
