// Extended instructions: the SSE and SSE2 subset firmware uses, the integer instructions the main
// table lacks (shifts through carry, double shifts, XADD, CMPXCHG, bit tests, NOP forms) and the
// x87 control instructions.
#include "cpu.h"

namespace {
constexpr int BYTE = 1, WORD = 2, DWORD = 4, QWORD = 8;

// Mask for one element of esz bytes.
uint64_t laneMask(int esz) { return esz == 8 ? ~0ull : ((1ull << (esz * 8)) - 1); }

// Element i of a 128-bit value, element size esz bytes.
uint64_t laneGet(const uint64_t v[2], int i, int esz) {
    const int bit = i * esz * 8;
    return (v[bit / 64] >> (bit % 64)) & laneMask(esz);
}

void laneSet(uint64_t v[2], int i, int esz, uint64_t val) {
    const int bit = i * esz * 8;
    const int sh = bit % 64;
    const uint64_t m = laneMask(esz) << sh;
    v[bit / 64] = (v[bit / 64] & ~m) | ((val << sh) & m);
}

// Writes only the low `size` bytes of a register (MOVSS and MOVSD register forms keep the rest).
void setLow(uint64_t& dst, uint64_t src, int size) {
    const uint64_t m = size == 8 ? ~0ull : 0xFFFFFFFFull;
    dst = (dst & ~m) | (src & m);
}
}  // namespace

void Cpu::readX(const Modrm& m, uint64_t out[2]) {
    if (m.mod == 3) {
        out[0] = xmm[m.rmx][0];
        out[1] = xmm[m.rmx][1];
        return;
    }
    const uint64_t ea = effAddr(m);
    out[0] = memRead(m.seg, ea, QWORD);
    out[1] = memRead(m.seg, ea + 8, QWORD);
}

void Cpu::writeX(const Modrm& m, const uint64_t in[2]) {
    if (m.mod == 3) {
        xmm[m.rmx][0] = in[0];
        xmm[m.rmx][1] = in[1];
        return;
    }
    const uint64_t ea = effAddr(m);
    memWrite(m.seg, ea, in[0], QWORD);
    memWrite(m.seg, ea + 8, in[1], QWORD);
}

// Returns false for an opcode it does not know; the caller then stops the machine.
bool Cpu::extended0F(uint8_t op, bool p66, uint8_t rep, int asz, int segOv, int osz) {
    switch (op) {
    case 0x08: case 0x09:  // INVD, WBINVD: there is no cache to invalidate or write back
        return true;

    case 0x0D:  // PREFETCH
    case 0x18: case 0x19: case 0x1A: case 0x1B:  // hint forms and multi-byte NOP
    case 0x1C: case 0x1D: case 0x1E: case 0x1F:
        decodeModrm(asz, segOv);
        return true;

    case 0xAE: {  // fences, LDMXCSR, STMXCSR, CLFLUSH
        Modrm m = decodeModrm(asz, segOv);
        if (m.mod == 3) return true;  // LFENCE, MFENCE, SFENCE
        switch (m.reg) {
        case 2:  // LDMXCSR
            mxcsr = uint32_t(memRead(m.seg, effAddr(m), DWORD));
            return true;
        case 3:  // STMXCSR
            memWrite(m.seg, effAddr(m), mxcsr, DWORD);
            return true;
        case 7:  // CLFLUSH: there is no cache to flush
            return true;
        default:
            return false;
        }
    }

    case 0xA4: case 0xA5:  // SHLD r/m, r, imm8 / CL
    case 0xAC: case 0xAD: {  // SHRD r/m, r, imm8 / CL
        Modrm m = decodeModrm(asz, segOv);
        const int count0 = (op & 1) ? int(getReg(ECX, BYTE)) : int(fetch8());
        const int bits = osz * 8;
        const int count = count0 & (osz == 8 ? 0x3F : 0x1F);
        if (count == 0 || count > bits) return true;
        const uint64_t dst = readRM(m, osz);
        const uint64_t src = getReg(m.regx, osz);
        uint64_t res;
        bool cf;
        if (op <= 0xA5) {  // SHLD: the high bits of dst shift out
            res = ((dst << count) | (src >> (bits - count))) & laneMask(osz);
            cf = ((dst >> (bits - count)) & 1) != 0;
        } else {           // SHRD: the low bits of dst shift out
            res = ((dst >> count) | (src << (bits - count))) & laneMask(osz);
            cf = ((dst >> (count - 1)) & 1) != 0;
        }
        setFlag(F_CF, cf);
        setSZP(res, osz);
        writeRM(m, res, osz);
        return true;
    }

    case 0xAB: case 0xB3: case 0xBB:  // BTS, BTR, BTC r/m, r
    case 0xBA: {                       // group 8: BT, BTS, BTR, BTC r/m, imm8
        Modrm m = decodeModrm(asz, segOv);
        int kind;  // 0 BT, 1 BTS, 2 BTR, 3 BTC
        uint64_t bit;
        if (op == 0xBA) {
            bit = fetch8();
            if (m.reg < 4) return false;
            kind = m.reg - 4;
        } else {
            bit = getReg(m.regx, osz);
            kind = op == 0xAB ? 1 : op == 0xB3 ? 2 : 3;
        }
        const int b = int(bit % uint64_t(osz * 8));
        const uint64_t v = readRM(m, osz);
        setFlag(F_CF, ((v >> b) & 1) != 0);
        if (kind == 0) return true;
        const uint64_t bitMask = 1ull << b;
        const uint64_t res = kind == 1 ? (v | bitMask) : kind == 2 ? (v & ~bitMask) : (v ^ bitMask);
        writeRM(m, res & laneMask(osz), osz);
        return true;
    }

    case 0xB0: case 0xB1: {  // CMPXCHG r/m, r
        const int sz = op == 0xB0 ? BYTE : osz;
        Modrm m = decodeModrm(asz, segOv);
        const uint64_t dst = readRM(m, sz);
        const uint64_t acc = getReg(EAX, sz);
        alu(7, acc, dst, sz);  // flags as for CMP accumulator, destination
        if (acc == dst) writeRM(m, getReg(m.regx, sz), sz);
        else setReg(EAX, dst, sz);
        return true;
    }

    case 0xC0: case 0xC1: {  // XADD r/m, r
        const int sz = op == 0xC0 ? BYTE : osz;
        Modrm m = decodeModrm(asz, segOv);
        const uint64_t dst = readRM(m, sz);
        const uint64_t src = getReg(m.regx, sz);
        const uint64_t sum = alu(0, dst, src, sz);
        setReg(m.regx, dst, sz);
        writeRM(m, sum, sz);
        return true;
    }

    case 0xC7: {  // group 9: CMPXCHG8B, CMPXCHG16B (with REX.W)
        Modrm m = decodeModrm(asz, segOv);
        if (m.reg != 1 || m.mod == 3) return false;
        const int half = (rex & 8) ? QWORD : DWORD;
        const uint64_t ea = effAddr(m);
        const uint64_t lo = memRead(m.seg, ea, half);
        const uint64_t hi = memRead(m.seg, ea + half, half);
        if (lo == getReg(EAX, half) && hi == getReg(EDX, half)) {
            setFlag(F_ZF, true);
            memWrite(m.seg, ea, getReg(EBX, half), half);
            memWrite(m.seg, ea + half, getReg(ECX, half), half);
        } else {
            setFlag(F_ZF, false);
            setReg(EAX, lo, half);
            setReg(EDX, hi, half);
        }
        return true;
    }

    // ----- moves
    case 0x10: case 0x11:  // MOVUPS/MOVUPD; with F3 MOVSS, with F2 MOVSD
    case 0x28: case 0x29:  // MOVAPS/MOVAPD (no alignment check)
    case 0x6F: case 0x7F: {  // MOVDQA (66) and MOVDQU (F3)
        if ((op == 0x28 || op == 0x29) && rep != 0) return false;
        if ((op == 0x6F || op == 0x7F) && !p66 && rep != 0xF3) return false;  // MMX forms not supported
        const bool store = op == 0x11 || op == 0x29 || op == 0x7F;
        const bool scalar = (op == 0x10 || op == 0x11) && (rep == 0xF2 || rep == 0xF3);
        Modrm m = decodeModrm(asz, segOv);
        if (scalar) {
            const int sz = rep == 0xF3 ? DWORD : QWORD;
            if (!store) {
                if (m.mod == 3) setLow(xmm[m.regx][0], xmm[m.rmx][0], sz);
                else {
                    xmm[m.regx][0] = memRead(m.seg, effAddr(m), sz);  // a load clears the high part
                    xmm[m.regx][1] = 0;
                }
            } else {
                if (m.mod == 3) setLow(xmm[m.rmx][0], xmm[m.regx][0], sz);
                else memWrite(m.seg, effAddr(m), xmm[m.regx][0], sz);
            }
            return true;
        }
        if (store) {
            const uint64_t v[2] = {xmm[m.regx][0], xmm[m.regx][1]};
            writeX(m, v);
        } else {
            uint64_t v[2];
            readX(m, v);
            xmm[m.regx][0] = v[0];
            xmm[m.regx][1] = v[1];
        }
        return true;
    }

    case 0x6E: {  // MOVD / MOVQ xmm, r/m
        if (!p66) return false;
        Modrm m = decodeModrm(asz, segOv);
        xmm[m.regx][0] = readRM(m, (rex & 8) ? QWORD : DWORD);
        xmm[m.regx][1] = 0;
        return true;
    }
    case 0x7E: {  // F3: MOVQ xmm, xmm/m64.  66: MOVD / MOVQ r/m, xmm
        Modrm m = decodeModrm(asz, segOv);
        if (rep == 0xF3) {
            xmm[m.regx][0] = (m.mod == 3) ? xmm[m.rmx][0] : memRead(m.seg, effAddr(m), QWORD);
            xmm[m.regx][1] = 0;
            return true;
        }
        if (!p66) return false;
        writeRM(m, xmm[m.regx][0], (rex & 8) ? QWORD : DWORD);
        return true;
    }
    case 0xD6: {  // MOVQ xmm/m64, xmm
        if (!p66) return false;
        Modrm m = decodeModrm(asz, segOv);
        if (m.mod == 3) {
            xmm[m.rmx][0] = xmm[m.regx][0];
            xmm[m.rmx][1] = 0;
        } else {
            memWrite(m.seg, effAddr(m), xmm[m.regx][0], QWORD);
        }
        return true;
    }

    // ----- logic
    case 0x54: case 0x55: case 0x56: case 0x57:  // ANDPS, ANDNPS, ORPS, XORPS (and the PD forms)
    case 0xDB: case 0xDF: case 0xEB: case 0xEF: {  // PAND, PANDN, POR, PXOR (66 only)
        if (op >= 0xDB && !p66) return false;
        Modrm m = decodeModrm(asz, segOv);
        uint64_t s[2];
        readX(m, s);
        const int kind = (op == 0x54 || op == 0xDB) ? 0 : (op == 0x55 || op == 0xDF) ? 1
                       : (op == 0x56 || op == 0xEB) ? 2 : 3;
        for (int i = 0; i < 2; ++i) {
            const uint64_t d = xmm[m.regx][i];
            xmm[m.regx][i] = kind == 0 ? (d & s[i]) : kind == 1 ? (~d & s[i])
                           : kind == 2 ? (d | s[i]) : (d ^ s[i]);
        }
        return true;
    }

    // ----- packed integer arithmetic and compares (66 only)
    case 0xFC: case 0xFD: case 0xFE: case 0xD4:  // PADDB, PADDW, PADDD, PADDQ
    case 0xF8: case 0xF9: case 0xFA: case 0xFB:  // PSUBB, PSUBW, PSUBD, PSUBQ
    case 0x74: case 0x75: case 0x76: {           // PCMPEQB, PCMPEQW, PCMPEQD
        if (!p66) return false;
        Modrm m = decodeModrm(asz, segOv);
        uint64_t s[2];
        readX(m, s);
        const uint64_t d[2] = {xmm[m.regx][0], xmm[m.regx][1]};
        int esz = 1, kind = 0;  // kind: 0 add, 1 subtract, 2 compare equal
        switch (op) {
        case 0xFC: esz = 1; kind = 0; break;
        case 0xFD: esz = 2; kind = 0; break;
        case 0xFE: esz = 4; kind = 0; break;
        case 0xD4: esz = 8; kind = 0; break;
        case 0xF8: esz = 1; kind = 1; break;
        case 0xF9: esz = 2; kind = 1; break;
        case 0xFA: esz = 4; kind = 1; break;
        case 0xFB: esz = 8; kind = 1; break;
        case 0x74: esz = 1; kind = 2; break;
        case 0x75: esz = 2; kind = 2; break;
        default:   esz = 4; kind = 2; break;
        }
        uint64_t out[2] = {0, 0};
        for (int i = 0; i < 16 / esz; ++i) {
            const uint64_t a = laneGet(d, i, esz), b = laneGet(s, i, esz);
            const uint64_t res = kind == 0 ? a + b : kind == 1 ? a - b : (a == b ? laneMask(esz) : 0);
            laneSet(out, i, esz, res);
        }
        xmm[m.regx][0] = out[0];
        xmm[m.regx][1] = out[1];
        return true;
    }
    case 0xD7: {  // PMOVMSKB r32, xmm
        if (!p66) return false;
        Modrm m = decodeModrm(asz, segOv);
        uint64_t mask = 0;
        for (int i = 0; i < 16; ++i) mask |= ((laneGet(xmm[m.rmx], i, 1) >> 7) & 1) << i;
        setReg(m.regx, mask, DWORD);
        return true;
    }
    case 0x70: {  // PSHUFD xmm, xmm/m128, imm8
        if (!p66) return false;
        Modrm m = decodeModrm(asz, segOv);
        const int imm = fetch8();  // fetched before the operand: a RIP-relative address counts it
        uint64_t s[2];
        readX(m, s);
        uint64_t out[2] = {0, 0};
        for (int i = 0; i < 4; ++i) laneSet(out, i, 4, laneGet(s, (imm >> (2 * i)) & 3, 4));
        xmm[m.regx][0] = out[0];
        xmm[m.regx][1] = out[1];
        return true;
    }
    case 0x71: case 0x72: case 0x73: {  // shifts by immediate (66 only)
        if (!p66) return false;
        Modrm m = decodeModrm(asz, segOv);
        const int imm = fetch8();
        if (m.mod != 3) return false;
        uint64_t* d = xmm[m.rmx];
        if (op == 0x73 && (m.reg == 3 || m.reg == 7)) {  // PSRLDQ, PSLLDQ: whole-register byte shifts
            const uint64_t src[2] = {d[0], d[1]};
            uint64_t out[2] = {0, 0};
            for (int i = 0; i < 16; ++i) {
                const int j = m.reg == 7 ? i - imm : i + imm;
                if (j >= 0 && j < 16) laneSet(out, i, 1, laneGet(src, j, 1));
            }
            d[0] = out[0];
            d[1] = out[1];
            return true;
        }
        const int esz = op == 0x71 ? 2 : op == 0x72 ? 4 : 8;
        const bool left = m.reg == 6;
        const bool arith = m.reg == 4;
        if (!(m.reg == 2 || m.reg == 6 || (m.reg == 4 && op != 0x73))) return false;
        const uint64_t src[2] = {d[0], d[1]};
        uint64_t out[2] = {0, 0};
        const int sh = 64 - esz * 8;
        for (int i = 0; i < 16 / esz; ++i) {
            const uint64_t a = laneGet(src, i, esz);
            uint64_t res;
            if (imm >= esz * 8) {
                const bool neg = arith && ((a >> (esz * 8 - 1)) & 1) != 0;
                res = neg ? laneMask(esz) : 0;
            } else if (left) {
                res = a << imm;
            } else if (arith) {
                const int64_t sv = int64_t(a << sh) >> sh;
                res = uint64_t(sv >> imm);
            } else {
                res = a >> imm;
            }
            laneSet(out, i, esz, res);
        }
        d[0] = out[0];
        d[1] = out[1];
        return true;
    }

    default:
        return false;
    }
}

// The x87 instructions that firmware uses to set up the FPU: control words and status words.
// The register stack is not modelled, so any other x87 instruction is not supported.
bool Cpu::x87(uint8_t op, int asz, int segOv) {
    const uint8_t next = uint8_t(readLin(linear(CS, eip), 1));
    if (next >= 0xC0) {  // register forms
        fetch8();
        if (op == 0xDB && next == 0xE2) return true;                                   // FNCLEX
        if (op == 0xDB && next == 0xE3) { fpuCw = 0x37F; fpuSw = 0; return true; }     // FNINIT
        if (op == 0xDF && next == 0xE0) { setReg(EAX, fpuSw, WORD); return true; }     // FNSTSW AX
        return false;
    }
    Modrm m = decodeModrm(asz, segOv);
    const uint64_t ea = effAddr(m);
    if (op == 0xD9 && m.reg == 5) { fpuCw = uint16_t(memRead(m.seg, ea, WORD)); return true; }  // FLDCW
    if (op == 0xD9 && m.reg == 7) { memWrite(m.seg, ea, fpuCw, WORD); return true; }            // FNSTCW
    if (op == 0xDD && m.reg == 7) { memWrite(m.seg, ea, fpuSw, WORD); return true; }            // FNSTSW m16
    return false;
}
