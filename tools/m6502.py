"""Official 6502 opcode table (shared by tools/export_meta.py and tools/meta_check.py).

Only the opcode -> (mnemonic, addressing mode) mapping, which is public CPU documentation. No game data here.
"""

SIZE = {'imp': 1, 'acc': 1, 'imm': 2, 'zp': 2, 'zpx': 2, 'zpy': 2, 'rel': 2, 'indx': 2, 'indy': 2,
        'abs': 3, 'absx': 3, 'absy': 3, 'ind': 3}

_GROUPS = {
    'adc': {0x69: 'imm', 0x65: 'zp', 0x75: 'zpx', 0x6D: 'abs', 0x7D: 'absx', 0x79: 'absy', 0x61: 'indx', 0x71: 'indy'},
    'and': {0x29: 'imm', 0x25: 'zp', 0x35: 'zpx', 0x2D: 'abs', 0x3D: 'absx', 0x39: 'absy', 0x21: 'indx', 0x31: 'indy'},
    'asl': {0x0A: 'acc', 0x06: 'zp', 0x16: 'zpx', 0x0E: 'abs', 0x1E: 'absx'},
    'bcc': {0x90: 'rel'}, 'bcs': {0xB0: 'rel'}, 'beq': {0xF0: 'rel'}, 'bmi': {0x30: 'rel'},
    'bne': {0xD0: 'rel'}, 'bpl': {0x10: 'rel'}, 'bvc': {0x50: 'rel'}, 'bvs': {0x70: 'rel'},
    'bit': {0x24: 'zp', 0x2C: 'abs'},
    'brk': {0x00: 'imp'}, 'clc': {0x18: 'imp'}, 'cld': {0xD8: 'imp'}, 'cli': {0x58: 'imp'}, 'clv': {0xB8: 'imp'},
    'cmp': {0xC9: 'imm', 0xC5: 'zp', 0xD5: 'zpx', 0xCD: 'abs', 0xDD: 'absx', 0xD9: 'absy', 0xC1: 'indx', 0xD1: 'indy'},
    'cpx': {0xE0: 'imm', 0xE4: 'zp', 0xEC: 'abs'}, 'cpy': {0xC0: 'imm', 0xC4: 'zp', 0xCC: 'abs'},
    'dec': {0xC6: 'zp', 0xD6: 'zpx', 0xCE: 'abs', 0xDE: 'absx'}, 'dex': {0xCA: 'imp'}, 'dey': {0x88: 'imp'},
    'eor': {0x49: 'imm', 0x45: 'zp', 0x55: 'zpx', 0x4D: 'abs', 0x5D: 'absx', 0x59: 'absy', 0x41: 'indx', 0x51: 'indy'},
    'inc': {0xE6: 'zp', 0xF6: 'zpx', 0xEE: 'abs', 0xFE: 'absx'}, 'inx': {0xE8: 'imp'}, 'iny': {0xC8: 'imp'},
    'jmp': {0x4C: 'abs', 0x6C: 'ind'}, 'jsr': {0x20: 'abs'},
    'lda': {0xA9: 'imm', 0xA5: 'zp', 0xB5: 'zpx', 0xAD: 'abs', 0xBD: 'absx', 0xB9: 'absy', 0xA1: 'indx', 0xB1: 'indy'},
    'ldx': {0xA2: 'imm', 0xA6: 'zp', 0xB6: 'zpy', 0xAE: 'abs', 0xBE: 'absy'},
    'ldy': {0xA0: 'imm', 0xA4: 'zp', 0xB4: 'zpx', 0xAC: 'abs', 0xBC: 'absx'},
    'lsr': {0x4A: 'acc', 0x46: 'zp', 0x56: 'zpx', 0x4E: 'abs', 0x5E: 'absx'}, 'nop': {0xEA: 'imp'},
    'ora': {0x09: 'imm', 0x05: 'zp', 0x15: 'zpx', 0x0D: 'abs', 0x1D: 'absx', 0x19: 'absy', 0x01: 'indx', 0x11: 'indy'},
    'pha': {0x48: 'imp'}, 'php': {0x08: 'imp'}, 'pla': {0x68: 'imp'}, 'plp': {0x28: 'imp'},
    'rol': {0x2A: 'acc', 0x26: 'zp', 0x36: 'zpx', 0x2E: 'abs', 0x3E: 'absx'},
    'ror': {0x6A: 'acc', 0x66: 'zp', 0x76: 'zpx', 0x6E: 'abs', 0x7E: 'absx'},
    'rti': {0x40: 'imp'}, 'rts': {0x60: 'imp'},
    'sbc': {0xE9: 'imm', 0xE5: 'zp', 0xF5: 'zpx', 0xED: 'abs', 0xFD: 'absx', 0xF9: 'absy', 0xE1: 'indx', 0xF1: 'indy'},
    'sec': {0x38: 'imp'}, 'sed': {0xF8: 'imp'}, 'sei': {0x78: 'imp'},
    'sta': {0x85: 'zp', 0x95: 'zpx', 0x8D: 'abs', 0x9D: 'absx', 0x99: 'absy', 0x81: 'indx', 0x91: 'indy'},
    'stx': {0x86: 'zp', 0x96: 'zpy', 0x8E: 'abs'}, 'sty': {0x84: 'zp', 0x94: 'zpx', 0x8C: 'abs'},
    'tax': {0xAA: 'imp'}, 'tay': {0xA8: 'imp'}, 'tsx': {0xBA: 'imp'}, 'txa': {0x8A: 'imp'}, 'txs': {0x9A: 'imp'},
    'tya': {0x98: 'imp'},
}

OPS = {}
for _m, _d in _GROUPS.items():
    for _op, _mode in _d.items():
        OPS[_op] = (_m, _mode)

BRANCHES = {'bcc', 'bcs', 'beq', 'bmi', 'bne', 'bpl', 'bvc', 'bvs'}
STORES = {'sta', 'stx', 'sty', 'inc', 'dec', 'asl', 'lsr', 'rol', 'ror'}


def decode(mem, addr, base=0x8000):
    """Decode one instruction at CPU address addr from a 32 KB bank image. Returns
    (mnemonic, mode, size, operand) or None for an undocumented opcode / running past the bank.
    operand: the immediate / address value; for 'rel' the absolute branch target."""
    o = addr - base
    if not 0 <= o < len(mem):
        return None
    op = mem[o]
    if op not in OPS:
        return None
    m, mode = OPS[op]
    n = SIZE[mode]
    if o + n > len(mem):
        return None
    if n == 1:
        v = None
    elif n == 2:
        v = mem[o + 1]
        if mode == 'rel':
            v = (addr + 2 + (v - 256 if v >= 128 else v)) & 0xFFFF
    else:
        v = mem[o + 1] | (mem[o + 2] << 8)
    return m, mode, n, v
