/* 6502 interpreter. See cpu.h for the timing model. */
#include "cpu.h"

const uint8_t cpu_base_cycles[256] = {
    /*        0 1 2 3 4 5 6 7 8 9 A B C D E F */
    /* 0x00 */ 7,6,2,8,3,3,5,5,3,2,2,2,4,4,6,6,
    /* 0x10 */ 2,5,2,8,4,4,6,6,2,4,2,7,4,4,7,7,
    /* 0x20 */ 6,6,2,8,3,3,5,5,4,2,2,2,4,4,6,6,
    /* 0x30 */ 2,5,2,8,4,4,6,6,2,4,2,7,4,4,7,7,
    /* 0x40 */ 6,6,2,8,3,3,5,5,3,2,2,2,3,4,6,6,
    /* 0x50 */ 2,5,2,8,4,4,6,6,2,4,2,7,4,4,7,7,
    /* 0x60 */ 6,6,2,8,3,3,5,5,4,2,2,2,5,4,6,6,
    /* 0x70 */ 2,5,2,8,4,4,6,6,2,4,2,7,4,4,7,7,
    /* 0x80 */ 2,6,2,6,3,3,3,3,2,2,2,2,4,4,4,4,
    /* 0x90 */ 2,6,2,6,4,4,4,4,2,5,2,5,5,5,5,5,
    /* 0xA0 */ 2,6,2,6,3,3,3,3,2,2,2,2,4,4,4,4,
    /* 0xB0 */ 2,5,2,5,4,4,4,4,2,4,2,4,4,4,4,4,
    /* 0xC0 */ 2,6,2,8,3,3,5,5,2,2,2,2,4,4,6,6,
    /* 0xD0 */ 2,5,2,8,4,4,6,6,2,4,2,7,4,4,7,7,
    /* 0xE0 */ 2,6,3,8,3,3,5,5,2,2,2,2,4,4,6,6,
    /* 0xF0 */ 2,5,2,8,4,4,6,6,2,4,2,7,4,4,7,7,
};

const uint8_t cpu_official[256] = {
    /*        0 1 2 3 4 5 6 7 8 9 A B C D E F */
    /* 0x00 */ 1,1,0,0,0,1,1,0,1,1,1,0,0,1,1,0,
    /* 0x10 */ 1,1,0,0,0,1,1,0,1,1,0,0,0,1,1,0,
    /* 0x20 */ 1,1,0,0,1,1,1,0,1,1,1,0,1,1,1,0,
    /* 0x30 */ 1,1,0,0,0,1,1,0,1,1,0,0,0,1,1,0,
    /* 0x40 */ 1,1,0,0,0,1,1,0,1,1,1,0,1,1,1,0,
    /* 0x50 */ 1,1,0,0,0,1,1,0,1,1,0,0,0,1,1,0,
    /* 0x60 */ 1,1,0,0,0,1,1,0,1,1,1,0,1,1,1,0,
    /* 0x70 */ 1,1,0,0,0,1,1,0,1,1,0,0,0,1,1,0,
    /* 0x80 */ 0,1,0,0,1,1,1,0,1,0,1,0,1,1,1,0,
    /* 0x90 */ 1,1,0,0,1,1,1,0,1,1,1,0,0,1,0,0,
    /* 0xA0 */ 1,1,1,0,1,1,1,0,1,1,1,0,1,1,1,0,
    /* 0xB0 */ 1,1,0,0,1,1,1,0,1,1,1,0,1,1,1,0,
    /* 0xC0 */ 1,1,0,0,1,1,1,0,1,1,1,0,1,1,1,0,
    /* 0xD0 */ 1,1,0,0,0,1,1,0,1,1,0,0,0,1,1,0,
    /* 0xE0 */ 1,1,0,0,1,1,1,0,1,1,1,0,1,1,1,0,
    /* 0xF0 */ 1,1,0,0,0,1,1,0,1,1,0,0,0,1,1,0,
};

/* ---- bus and time helpers ---- */

static void add_cycles(Cpu *c, int n)
{
    c->cycles += (uint64_t)n;
    c->budget -= n * CPU_CYCLE_UNITS;
}

void cpu_add_cycles(Cpu *cpu, int n)
{
    add_cycles(cpu, n);
}

static uint8_t rd(Cpu *c, uint16_t addr)
{
    c->bus = c->read(c->ctx, addr);
    return c->bus;
}

static void wr(Cpu *c, uint16_t addr, uint8_t v)
{
    c->write(c->ctx, addr, v);
    c->bus = v;
}

static void push(Cpu *c, uint8_t v)
{
    wr(c, (uint16_t)(0x100 | c->s), v);
    c->s--;
}

static uint8_t pop(Cpu *c)
{
    c->s++;
    return rd(c, (uint16_t)(0x100 | c->s));
}

static void set_zn(Cpu *c, uint8_t v)
{
    c->p = (uint8_t)((c->p & ~(CPU_Z | CPU_N)) | (v == 0 ? CPU_Z : 0) | (v & CPU_N));
}

/* ---- addressing modes (return the effective address; PC advances past the operand) ---- */

static uint8_t fetch(Cpu *c)
{
    return rd(c, c->pc++);
}

static uint16_t am_abs(Cpu *c)
{
    uint16_t lo = fetch(c);
    return (uint16_t)(lo | fetch(c) << 8);
}

/* Indexed absolute for reads: an extra cycle and a dummy read only when the page changes. */
static uint16_t am_abs_idx_rd(Cpu *c, uint8_t index)
{
    uint16_t base = am_abs(c);
    uint16_t ea = (uint16_t)(base + index);
    if ((ea ^ base) & 0x100) {
        rd(c, (uint16_t)(ea ^ 0x100));
        add_cycles(c, 1);
    }
    return ea;
}

/* Indexed absolute for stores and read-modify-writes: always a dummy read at the unfixed address. */
static uint16_t am_abs_idx_wr(Cpu *c, uint8_t index)
{
    uint16_t base = am_abs(c);
    uint16_t ea = (uint16_t)(base + index);
    rd(c, (uint16_t)((ea & 0x00FF) | (base & 0xFF00)));
    return ea;
}

static uint16_t am_zp(Cpu *c)
{
    return fetch(c);
}

static uint16_t am_zp_idx(Cpu *c, uint8_t index)
{
    return (uint8_t)(fetch(c) + index);
}

static uint16_t am_ind_x(Cpu *c)
{
    uint8_t zp = (uint8_t)(fetch(c) + c->x);
    uint16_t lo = rd(c, zp);
    return (uint16_t)(lo | rd(c, (uint8_t)(zp + 1)) << 8);
}

static uint16_t ind_y_base(Cpu *c)
{
    uint8_t zp = fetch(c);
    uint16_t lo = rd(c, zp);
    return (uint16_t)(lo | rd(c, (uint8_t)(zp + 1)) << 8);
}

static uint16_t am_ind_y_rd(Cpu *c)
{
    uint16_t base = ind_y_base(c);
    uint16_t ea = (uint16_t)(base + c->y);
    if ((ea ^ base) & 0x100) {
        rd(c, (uint16_t)(ea ^ 0x100));
        add_cycles(c, 1);
    }
    return ea;
}

static uint16_t am_ind_y_wr(Cpu *c)
{
    uint16_t base = ind_y_base(c);
    uint16_t ea = (uint16_t)(base + c->y);
    rd(c, (uint16_t)((ea & 0x00FF) | (base & 0xFF00)));
    return ea;
}

/* ---- operations ---- */

static void op_adc(Cpu *c, uint8_t v)
{
    unsigned sum = (unsigned)c->a + v + (c->p & CPU_C);
    c->p &= (uint8_t)~(CPU_C | CPU_V);
    if (~(c->a ^ v) & (c->a ^ sum) & 0x80)
        c->p |= CPU_V;
    if (sum > 0xFF)
        c->p |= CPU_C;
    c->a = (uint8_t)sum;
    set_zn(c, c->a);
}

static void op_sbc(Cpu *c, uint8_t v)
{
    op_adc(c, (uint8_t)~v);
}

static void op_cmp(Cpu *c, uint8_t reg, uint8_t v)
{
    c->p = (uint8_t)((c->p & ~CPU_C) | (reg >= v ? CPU_C : 0));
    set_zn(c, (uint8_t)(reg - v));
}

static void op_bit(Cpu *c, uint8_t v)
{
    c->p = (uint8_t)((c->p & ~(CPU_Z | CPU_V | CPU_N)) | (v & (CPU_V | CPU_N)) | ((c->a & v) ? 0 : CPU_Z));
}

static uint8_t op_asl(Cpu *c, uint8_t v)
{
    c->p = (uint8_t)((c->p & ~CPU_C) | (v >> 7));
    v = (uint8_t)(v << 1);
    set_zn(c, v);
    return v;
}

static uint8_t op_lsr(Cpu *c, uint8_t v)
{
    c->p = (uint8_t)((c->p & ~CPU_C) | (v & 1));
    v >>= 1;
    set_zn(c, v);
    return v;
}

static uint8_t op_rol(Cpu *c, uint8_t v)
{
    uint8_t carry = c->p & CPU_C;
    c->p = (uint8_t)((c->p & ~CPU_C) | (v >> 7));
    v = (uint8_t)(v << 1 | carry);
    set_zn(c, v);
    return v;
}

static uint8_t op_ror(Cpu *c, uint8_t v)
{
    uint8_t carry = c->p & CPU_C;
    c->p = (uint8_t)((c->p & ~CPU_C) | (v & 1));
    v = (uint8_t)(v >> 1 | carry << 7);
    set_zn(c, v);
    return v;
}

static uint8_t op_inc(Cpu *c, uint8_t v)
{
    v++;
    set_zn(c, v);
    return v;
}

static uint8_t op_dec(Cpu *c, uint8_t v)
{
    v--;
    set_zn(c, v);
    return v;
}

static void branch(Cpu *c, int taken)
{
    if (taken) {
        int8_t disp = (int8_t)fetch(c);
        uint16_t from = c->pc;
        add_cycles(c, 1);
        c->pc = (uint16_t)(c->pc + disp);
        if ((from ^ c->pc) & 0x100)
            add_cycles(c, 1);
    } else {
        c->pc++;
    }
}

/* Read-modify-write on memory: read, write the old value back (dummy write), write the result. */
typedef uint8_t (*RmwOp)(Cpu *c, uint8_t v);

static void rmw_mem(Cpu *c, uint16_t ea, RmwOp f)
{
    uint8_t v = rd(c, ea);
    wr(c, ea, v);
    wr(c, ea, f(c, v));
}

/* Zero-page RMW has no dummy write (the address can only be RAM). */
static void rmw_zp(Cpu *c, uint16_t ea, RmwOp f)
{
    wr(c, ea, f(c, rd(c, ea)));
}

/* ---- interrupts ---- */

static void interrupt(Cpu *c, uint16_t vector)
{
    add_cycles(c, 7);
    push(c, (uint8_t)(c->pc >> 8));
    push(c, (uint8_t)c->pc);
    push(c, (uint8_t)((c->p & ~CPU_B) | CPU_U));
    c->p |= CPU_I;
    c->pc = rd(c, vector);
    c->pc |= (uint16_t)(rd(c, (uint16_t)(vector + 1)) << 8);
}

/* Handles pending interrupt state before an instruction (FCEUX order: reset, delayed NMI, NMI, IRQ). */
static void poll_interrupts(Cpu *c)
{
    if (c->irq & CPU_INT_RESET) {
        c->pc = rd(c, 0xFFFC);
        c->pc |= (uint16_t)(rd(c, 0xFFFD) << 8);
        c->jammed = 0;
        c->p = c->p_before = CPU_I;
        c->irq &= (uint8_t)~CPU_INT_RESET;
        return;
    }
    if (c->irq & CPU_INT_NMI2) {
        c->irq = (uint8_t)((c->irq & ~CPU_INT_NMI2) | CPU_INT_NMI);
        return;
    }
    if (c->irq & CPU_INT_NMI) {
        if (!c->jammed) {
            interrupt(c, 0xFFFA);
            c->irq &= (uint8_t)~CPU_INT_NMI;
        }
        return;
    }
    if (!(c->p_before & CPU_I) && !c->jammed)
        interrupt(c, 0xFFFE);
}

/* ---- one instruction ---- */

static void execute(Cpu *c)
{
    uint8_t op;
    uint16_t ea;

    c->p_before = c->p;
    op = rd(c, c->pc);
    add_cycles(c, cpu_base_cycles[op]);
    if (c->exec_hook && c->exec_watch[c->pc])
        c->exec_hook(c->ctx, c);
    c->pc++;
    c->instructions++;
    c->last_op = op;

    switch (op) {
    /* loads */
    case 0xA9: c->a = fetch(c); set_zn(c, c->a); break;
    case 0xA5: c->a = rd(c, am_zp(c)); set_zn(c, c->a); break;
    case 0xB5: c->a = rd(c, am_zp_idx(c, c->x)); set_zn(c, c->a); break;
    case 0xAD: c->a = rd(c, am_abs(c)); set_zn(c, c->a); break;
    case 0xBD: c->a = rd(c, am_abs_idx_rd(c, c->x)); set_zn(c, c->a); break;
    case 0xB9: c->a = rd(c, am_abs_idx_rd(c, c->y)); set_zn(c, c->a); break;
    case 0xA1: c->a = rd(c, am_ind_x(c)); set_zn(c, c->a); break;
    case 0xB1: c->a = rd(c, am_ind_y_rd(c)); set_zn(c, c->a); break;

    case 0xA2: c->x = fetch(c); set_zn(c, c->x); break;
    case 0xA6: c->x = rd(c, am_zp(c)); set_zn(c, c->x); break;
    case 0xB6: c->x = rd(c, am_zp_idx(c, c->y)); set_zn(c, c->x); break;
    case 0xAE: c->x = rd(c, am_abs(c)); set_zn(c, c->x); break;
    case 0xBE: c->x = rd(c, am_abs_idx_rd(c, c->y)); set_zn(c, c->x); break;

    case 0xA0: c->y = fetch(c); set_zn(c, c->y); break;
    case 0xA4: c->y = rd(c, am_zp(c)); set_zn(c, c->y); break;
    case 0xB4: c->y = rd(c, am_zp_idx(c, c->x)); set_zn(c, c->y); break;
    case 0xAC: c->y = rd(c, am_abs(c)); set_zn(c, c->y); break;
    case 0xBC: c->y = rd(c, am_abs_idx_rd(c, c->x)); set_zn(c, c->y); break;

    /* stores */
    case 0x85: wr(c, am_zp(c), c->a); break;
    case 0x95: wr(c, am_zp_idx(c, c->x), c->a); break;
    case 0x8D: wr(c, am_abs(c), c->a); break;
    case 0x9D: wr(c, am_abs_idx_wr(c, c->x), c->a); break;
    case 0x99: wr(c, am_abs_idx_wr(c, c->y), c->a); break;
    case 0x81: wr(c, am_ind_x(c), c->a); break;
    case 0x91: wr(c, am_ind_y_wr(c), c->a); break;
    case 0x86: wr(c, am_zp(c), c->x); break;
    case 0x96: wr(c, am_zp_idx(c, c->y), c->x); break;
    case 0x8E: wr(c, am_abs(c), c->x); break;
    case 0x84: wr(c, am_zp(c), c->y); break;
    case 0x94: wr(c, am_zp_idx(c, c->x), c->y); break;
    case 0x8C: wr(c, am_abs(c), c->y); break;

    /* register transfers */
    case 0xAA: c->x = c->a; set_zn(c, c->x); break;
    case 0x8A: c->a = c->x; set_zn(c, c->a); break;
    case 0xA8: c->y = c->a; set_zn(c, c->y); break;
    case 0x98: c->a = c->y; set_zn(c, c->a); break;
    case 0xBA: c->x = c->s; set_zn(c, c->x); break;
    case 0x9A: c->s = c->x; break;

    /* stack */
    case 0x48: push(c, c->a); break;
    case 0x08: push(c, (uint8_t)(c->p | CPU_U | CPU_B)); break;
    case 0x68: c->a = pop(c); set_zn(c, c->a); break;
    case 0x28: c->p = pop(c); break;

    /* logic and arithmetic */
#define ALU_GROUP(base, OPEXPR)                                              \
    case base + 0x09: { uint8_t v = fetch(c); OPEXPR; } break;               \
    case base + 0x05: { uint8_t v = rd(c, am_zp(c)); OPEXPR; } break;        \
    case base + 0x15: { uint8_t v = rd(c, am_zp_idx(c, c->x)); OPEXPR; } break; \
    case base + 0x0D: { uint8_t v = rd(c, am_abs(c)); OPEXPR; } break;       \
    case base + 0x1D: { uint8_t v = rd(c, am_abs_idx_rd(c, c->x)); OPEXPR; } break; \
    case base + 0x19: { uint8_t v = rd(c, am_abs_idx_rd(c, c->y)); OPEXPR; } break; \
    case base + 0x01: { uint8_t v = rd(c, am_ind_x(c)); OPEXPR; } break;     \
    case base + 0x11: { uint8_t v = rd(c, am_ind_y_rd(c)); OPEXPR; } break;
    ALU_GROUP(0x00, c->a |= v; set_zn(c, c->a))
    ALU_GROUP(0x20, c->a &= v; set_zn(c, c->a))
    ALU_GROUP(0x40, c->a ^= v; set_zn(c, c->a))
    ALU_GROUP(0x60, op_adc(c, v))
    ALU_GROUP(0xC0, op_cmp(c, c->a, v))
    ALU_GROUP(0xE0, op_sbc(c, v))
#undef ALU_GROUP

    case 0xE0: op_cmp(c, c->x, fetch(c)); break;
    case 0xE4: op_cmp(c, c->x, rd(c, am_zp(c))); break;
    case 0xEC: op_cmp(c, c->x, rd(c, am_abs(c))); break;
    case 0xC0: op_cmp(c, c->y, fetch(c)); break;
    case 0xC4: op_cmp(c, c->y, rd(c, am_zp(c))); break;
    case 0xCC: op_cmp(c, c->y, rd(c, am_abs(c))); break;
    case 0x24: op_bit(c, rd(c, am_zp(c))); break;
    case 0x2C: op_bit(c, rd(c, am_abs(c))); break;

    /* shifts, rotates, inc/dec */
#define RMW_GROUP(base, F)                                                    \
    case base + 0x06: rmw_zp(c, am_zp(c), F); break;                          \
    case base + 0x16: rmw_zp(c, am_zp_idx(c, c->x), F); break;                \
    case base + 0x0E: rmw_mem(c, am_abs(c), F); break;                        \
    case base + 0x1E: rmw_mem(c, am_abs_idx_wr(c, c->x), F); break;
    RMW_GROUP(0x00, op_asl)
    RMW_GROUP(0x20, op_rol)
    RMW_GROUP(0x40, op_lsr)
    RMW_GROUP(0x60, op_ror)
    RMW_GROUP(0xC0, op_dec)
    RMW_GROUP(0xE0, op_inc)
#undef RMW_GROUP
    case 0x0A: c->a = op_asl(c, c->a); break;
    case 0x2A: c->a = op_rol(c, c->a); break;
    case 0x4A: c->a = op_lsr(c, c->a); break;
    case 0x6A: c->a = op_ror(c, c->a); break;
    case 0xE8: c->x++; set_zn(c, c->x); break;
    case 0xCA: c->x--; set_zn(c, c->x); break;
    case 0xC8: c->y++; set_zn(c, c->y); break;
    case 0x88: c->y--; set_zn(c, c->y); break;

    /* flags */
    case 0x18: c->p &= (uint8_t)~CPU_C; break;
    case 0x38: c->p |= CPU_C; break;
    case 0x58: c->p &= (uint8_t)~CPU_I; break;
    case 0x78: c->p |= CPU_I; break;
    case 0xB8: c->p &= (uint8_t)~CPU_V; break;
    case 0xD8: c->p &= (uint8_t)~CPU_D; break;
    case 0xF8: c->p |= CPU_D; break;

    /* branches */
    case 0x10: branch(c, !(c->p & CPU_N)); break;
    case 0x30: branch(c, c->p & CPU_N); break;
    case 0x50: branch(c, !(c->p & CPU_V)); break;
    case 0x70: branch(c, c->p & CPU_V); break;
    case 0x90: branch(c, !(c->p & CPU_C)); break;
    case 0xB0: branch(c, c->p & CPU_C); break;
    case 0xD0: branch(c, !(c->p & CPU_Z)); break;
    case 0xF0: branch(c, c->p & CPU_Z); break;

    /* jumps and subroutines */
    case 0x4C: c->pc = am_abs(c); break;
    case 0x6C: /* JMP (ind): the high byte is read without carrying into the page */
        ea = am_abs(c);
        c->pc = rd(c, ea);
        c->pc |= (uint16_t)(rd(c, (uint16_t)(((ea + 1) & 0x00FF) | (ea & 0xFF00))) << 8);
        break;
    case 0x20: {
        uint8_t lo = fetch(c);
        push(c, (uint8_t)(c->pc >> 8));
        push(c, (uint8_t)c->pc);
        c->pc = (uint16_t)(rd(c, c->pc) << 8 | lo);
        break;
    }
    case 0x60:
        c->pc = pop(c);
        c->pc |= (uint16_t)(pop(c) << 8);
        c->pc++;
        break;
    case 0x40: /* RTI: the restored I flag applies to IRQ polling at once */
        c->p = pop(c);
        c->p_before = c->p;
        c->pc = pop(c);
        c->pc |= (uint16_t)(pop(c) << 8);
        break;
    case 0x00: /* BRK */
        c->pc++;
        push(c, (uint8_t)(c->pc >> 8));
        push(c, (uint8_t)c->pc);
        push(c, (uint8_t)(c->p | CPU_U | CPU_B));
        c->p |= CPU_I;
        c->p_before |= CPU_I;
        c->pc = rd(c, 0xFFFE);
        c->pc |= (uint16_t)(rd(c, 0xFFFF) << 8);
        break;

    case 0xEA: break; /* NOP */

    default:
        if (!c->trap || !c->trap(c->ctx, c, (uint16_t)(c->pc - 1), op)) {
            c->jammed = 1;
            c->pc--;
        }
        break;
    }
}

/* ---- public ---- */

void cpu_init(Cpu *cpu, void *ctx, CpuRead read, CpuWrite write, CpuTrap trap)
{
    *cpu = (Cpu){0};
    cpu->ctx = ctx;
    cpu->read = read;
    cpu->write = write;
    cpu->trap = trap;
}

void cpu_power(Cpu *cpu)
{
    cpu->pc = 0;
    cpu->a = cpu->x = cpu->y = cpu->p = cpu->p_before = cpu->bus = 0;
    cpu->jammed = 0;
    cpu->s = 0xFD;
    cpu->budget = 0;
    cpu->cycles = 0;
    cpu->instructions = 0;
    cpu->irq = CPU_INT_RESET;
}

void cpu_reset(Cpu *cpu)
{
    cpu->irq = CPU_INT_RESET;
}

void cpu_step(Cpu *cpu)
{
    if (cpu->irq) {
        poll_interrupts(cpu);
        return;
    }
    if (!cpu->jammed)
        execute(cpu);
    else
        add_cycles(cpu, 2);
}

void cpu_run(Cpu *cpu, int32_t dots)
{
    cpu->budget += dots * 16;
    while (cpu->budget > 0) {
        if (cpu->irq) {
            poll_interrupts(cpu);
            if (cpu->budget <= 0) {
                cpu->p_before = cpu->p;
                return;
            }
        }
        if (cpu->jammed) {
            /* FCEUX keeps fetching the jam opcode; time still passes. */
            add_cycles(cpu, cpu_base_cycles[0x02]);
            continue;
        }
        execute(cpu);
    }
}

uint8_t cpu_bus_read(Cpu *cpu, uint16_t addr)
{
    return rd(cpu, addr);
}

void cpu_bus_write(Cpu *cpu, uint16_t addr, uint8_t value)
{
    wr(cpu, addr, value);
}

CpuCallResult cpu_call(Cpu *cpu, uint16_t addr, uint64_t max_instructions)
{
    uint8_t entry_s = cpu->s;
    uint64_t start = cpu->instructions;
    uint16_t ret = (uint16_t)(cpu->pc - 1);

    push(cpu, (uint8_t)(ret >> 8));
    push(cpu, (uint8_t)ret);
    cpu->pc = addr;
    for (;;) {
        cpu_step(cpu);
        if (cpu->jammed)
            return CPU_CALL_JAMMED;
        if (cpu->s == entry_s && cpu->last_op == 0x60)
            return cpu->pc == (uint16_t)(ret + 1) ? CPU_CALL_RETURNED : CPU_CALL_UNWOUND;
        /* S is 8-bit: "above the entry level" is measured as a small positive distance */
        if ((uint8_t)(cpu->s - entry_s) != 0 && (uint8_t)(cpu->s - entry_s) < 0x80)
            return CPU_CALL_UNWOUND;
        if (max_instructions && cpu->instructions - start >= max_instructions)
            return CPU_CALL_LIMIT;
    }
}
