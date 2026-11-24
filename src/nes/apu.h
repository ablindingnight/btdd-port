/* 2A03 APU: two pulse channels, triangle, noise, DMC, frame counter, $4015.
 *
 * Two layers:
 *  - Register/sequencer state that behaves like FCEUX 2.6 (envelopes, sweeps, length and linear counters,
 *    frame counter steps, DMC fetches and IRQ flags). It is advanced once per CPU instruction, at the
 *    point FCEUX does it: when an instruction starts, after its base cycles are charged (apu_tick).
 *    This is what tools/apudiff.py checks against FCEUX's per-frame channel log.
 *  - Sound synthesis: every channel's timer and waveform is clocked per CPU cycle between events
 *    (register writes, sequencer steps), mixed with the standard nonlinear NES mixer, low-passed by
 *    averaging each output period, then high-pass 90 Hz / 440 Hz and low-pass 14 kHz like the console's
 *    output stage. Raw $4011 writes (the game's PCM player) land on their exact CPU cycle.
 * Written from FCEUX's behaviour and the NESdev documentation, not from FCEUX code. */
#ifndef BTDD_APU_H
#define BTDD_APU_H
#include <stddef.h>
#include <stdint.h>

#define APU_CPU_HZ 1789772.7272727272 /* NTSC */

typedef struct ApuEnvelope {
    uint8_t speed;   /* $4000/$4004/$400C bits 0-3 */
    uint8_t mode;    /* bit 0 = constant volume, bit 1 = loop (bits 4-5 of the register) */
    uint8_t divider;
    uint8_t decay;   /* current decay level 0-15 */
    uint8_t restart;
} ApuEnvelope;

typedef struct ApuHost {
    void *ctx;
    uint8_t (*read)(void *ctx, uint16_t addr); /* DMC sample fetch (CPU bus) */
    void (*stall)(void *ctx, int cycles);       /* DMC fetch steals CPU cycles */
    void (*irq)(void *ctx, int frame_irq, int dmc_irq); /* IRQ line levels */
} ApuHost;

typedef struct Apu {
    /* ---- register / sequencer state (FCEUX-visible) ---- */
    uint8_t regs[0x10];          /* last values written to $4000-$400F */
    uint8_t dmc_format;          /* $4010 */
    uint8_t dmc_level;           /* 7-bit DAC */
    uint8_t dmc_level_written;   /* last $4011 value */
    uint8_t dmc_addr_latch, dmc_size_latch; /* $4012, $4013 */
    uint8_t enabled;             /* $4015 low 5 bits */
    uint8_t frame_mode;          /* $4017 bits 6-7 >> 6 */
    ApuEnvelope env[3];          /* pulse 1, pulse 2, noise */
    int32_t period[2];           /* pulse timer periods (11 bits, changed by sweeps) */
    uint8_t sweep_on[2], sweep_count[2], sweep_reload[2];
    int32_t length[4];
    uint8_t linear, linear_reload;
    uint8_t step;                /* frame counter step 0-3 */
    int32_t step_timer;          /* 1/48 CPU cycles until the next step */
    int32_t dmc_timer, dmc_period;
    uint8_t dmc_bits, dmc_shift, dmc_buffer;
    uint8_t dmc_buffer_full, dmc_playing;
    uint32_t dmc_addr;
    int32_t dmc_remaining;
    uint8_t irq_status;          /* bit 6 frame IRQ, bit 7 DMC IRQ */
    uint64_t last_tick;          /* CPU cycle of the previous apu_tick */

    /* ---- synthesis ---- */
    uint64_t synth_cycle;        /* audio has been produced up to here */
    int32_t pulse_timer[2];
    uint8_t pulse_pos[2];
    int32_t tri_timer;
    uint8_t tri_pos;
    int32_t noise_timer;
    uint16_t noise_lfsr;
    double out_step, out_acc;    /* CPU cycles per output sample, accumulated cycles */
    double sum;                  /* mixer output summed over the current output period */
    int sum_n;                   /* CPU cycles in sum */
    float hp1_prev_in, hp1_prev_out, hp2_prev_in, hp2_prev_out, lp_prev;
    float hp1_k, hp2_k, lp_k;
    float *out;                  /* caller's sample buffer */
    size_t out_cap, out_len;
    uint64_t samples_dropped;
    int sample_rate;

    ApuHost host;
} Apu;

void apu_power(Apu *apu, int sample_rate, const ApuHost *host);
/* Call at the start of every CPU instruction with the cycle count after its base cycles. */
void apu_tick(Apu *apu, uint64_t cpu_cycle);
void apu_write(Apu *apu, uint64_t cpu_cycle, uint16_t addr, uint8_t value); /* $4000-$4013, $4015, $4017 */
uint8_t apu_read_status(Apu *apu, uint64_t cpu_cycle);                       /* $4015 */
/* Produces audio up to cpu_cycle (e.g. at the end of a frame). */
void apu_render(Apu *apu, uint64_t cpu_cycle);
/* Output buffer for float samples in [-1, 1]; samples beyond capacity are counted and dropped. */
void apu_set_buffer(Apu *apu, float *buf, size_t cap);
size_t apu_take(Apu *apu); /* number of samples written since the last call; resets the fill */

/* Values FCEUX's Lua sound.get() reports, for logging. */
typedef struct ApuView {
    double sq_key[2], sq_vol[2];
    int sq_duty[2];
    double tri_key, tri_vol;
    double noise_key, noise_vol;
    int noise_short;
    int dmc_vol, dmc_addr, dmc_size;
} ApuView;
void apu_view(const Apu *apu, ApuView *v);

#endif
