#include "cpu.h"

#include <cstdio>
#include <cstring>

namespace {
constexpr uint64_t kApicBase = 0xFEE00000ull;
constexpr int BYTE = 1, WORD = 2, DWORD = 4, QWORD = 8;

uint64_t maskFor(int s) { return s == 1 ? 0xFFull : s == 2 ? 0xFFFFull : s == 4 ? 0xFFFFFFFFull : ~0ull; }
uint64_t signFor(int s) {
    return s == 1 ? 0x80ull : s == 2 ? 0x8000ull : s == 4 ? 0x80000000ull : 0x8000000000000000ull;
}
int64_t sext(uint64_t v, int s) {
    switch (s) {
    case 1: return int8_t(uint8_t(v));
    case 2: return int16_t(uint16_t(v));
    case 4: return int32_t(uint32_t(v));
    default: return int64_t(v);
    }
}
// Writes an address or count register at the given address size. 16-bit writes keep the upper bits;
// 32-bit writes clear them, as the hardware does.
void setAddr(uint64_t& reg, uint64_t v, int asz) {
    if (asz == 8) reg = v;
    else if (asz == 4) reg = v & 0xFFFFFFFFull;
    else reg = (reg & ~0xFFFFull) | (v & 0xFFFF);
}
}  // namespace

// ----------------------------------------------------------------------------- bus

uint64_t Bus::read(uint32_t phys, int size) {
    uint64_t v = 0;
    for (int i = 0; i < size; ++i) {
        const uint64_t p = uint64_t(phys) + i;
        uint64_t b;
        if (p < ram.size()) b = ram[p];
        else if (p - kApicBase < apic.size()) b = apic[p - kApicBase];
        else if (p >= romBase && p - romBase < rom.size()) b = rom[p - romBase];
        else {
            b = 0xFF;
            ++badAccesses;
        }
        v |= b << (8 * i);
    }
    return v;
}

void Bus::write(uint32_t phys, uint64_t value, int size) {
    for (int i = 0; i < size; ++i) {
        uint64_t p = uint64_t(phys) + i;
        if (p < ram.size()) ram[p] = uint8_t(value >> (8 * i));
        else if (p - kApicBase < apic.size()) apic[p - kApicBase] = uint8_t(value >> (8 * i));
        else ++badAccesses;
    }
}

// ----------------------------------------------------------------------------- setup

Cpu::Cpu(Bus& b) : bus(b) { reset(); }

void Cpu::reset() {
    for (auto& x : r) x = 0;
    eip = 0xFFF0;
    eflags = 0x2;
    cr0 = 0x60000010;
    cr2 = cr3 = 0;
    cr4 = 0;
    efer = 0;
    seg[CS].sel = 0xF000;
    seg[CS].base = 0xFFFF0000;
    seg[CS].limit = 0xFFFF;
    seg[CS].big = false;
    seg[CS].longMode = false;
    const int others[] = {ES, SS, DS, FS, GS};
    for (int s : others) seg[s] = Segment{};
    gdtBase = gdtLimit = 0;
    idtBase = 0;
    idtLimit = 0x3FF;
    halted = stopped = false;
    cycles = 0;
    pendingVector = -1;
    rex = 0;
    stopInfo[0] = 0;
    fpuCw = 0x37F;
    fpuSw = 0;
    mxcsr = 0x1F80;
    for (auto& x : xmm) x[0] = x[1] = 0;
}

void Cpu::raiseInterrupt(int vector) { pendingVector = vector; }

void Cpu::stop(const char* what) {
    stopped = true;
    std::snprintf(stopInfo, sizeof stopInfo, "%s at %04X:%08X", what, unsigned(instrCs), unsigned(instrEip));
}

// ----------------------------------------------------------------------------- fetch and operands

uint8_t Cpu::fetch8() {
    uint8_t v = uint8_t(readLin(linear(CS, eip), 1));
    const bool wide = seg[CS].big || in64();
    eip = wide ? eip + 1 : ((eip + 1) & 0xFFFF);
    return v;
}

uint16_t Cpu::fetch16() {
    uint16_t lo = fetch8();
    uint16_t hi = fetch8();
    return uint16_t(lo | (hi << 8));
}

uint32_t Cpu::fetch32() {
    uint32_t lo = fetch16();
    uint32_t hi = fetch16();
    return lo | (hi << 16);
}

uint64_t Cpu::fetch64() {
    uint64_t lo = fetch32();
    uint64_t hi = fetch32();
    return lo | (hi << 32);
}

uint64_t Cpu::fetchImm(int size) {
    switch (size) {
    case 1: return fetch8();
    case 2: return fetch16();
    case 4: return fetch32();
    default: return uint64_t(int64_t(int32_t(fetch32())));
    }
}

uint64_t Cpu::getReg(int idx, int size) const {
    if (size == 1) return (idx < 4 || rex) ? (r[idx] & 0xFF) : ((r[idx - 4] >> 8) & 0xFF);
    return r[idx] & maskFor(size);
}

void Cpu::setReg(int idx, uint64_t v, int size) {
    if (size == 1) {
        if (idx < 4 || rex) r[idx] = (r[idx] & ~0xFFull) | (v & 0xFF);
        else r[idx - 4] = (r[idx - 4] & ~0xFF00ull) | ((v & 0xFF) << 8);
    } else if (size == 2) {
        r[idx] = (r[idx] & ~0xFFFFull) | (v & 0xFFFF);
    } else if (size == 4) {
        r[idx] = v & 0xFFFFFFFFull;  // 32-bit writes clear the upper half
    } else {
        r[idx] = v;
    }
}

// The linear address of an offset in a segment. In 64-bit code the segment bases are ignored.
uint64_t Cpu::linear(int s, uint64_t off) const {
    if (in64()) return off;
    return uint32_t(seg[s].base + uint32_t(off));
}

uint64_t Cpu::memRead(int s, uint64_t off, int size) { return readLin(linear(s, off), size); }

void Cpu::memWrite(int s, uint64_t off, uint64_t v, int size) { writeLin(linear(s, off), v, size); }

// The effective address of a memory operand. RIP-relative operands add the address of the next
// instruction, which is the current eip because the whole instruction has been decoded.
uint64_t Cpu::effAddr(const Modrm& m) const { return m.ripRel ? m.ea + eip : m.ea; }

// Translates a linear address through the page tables. Returns the address unchanged when paging is
// off. Sets the accessed and dirty bits, and throws PageFault when the page is not usable.
uint32_t Cpu::translate(uint64_t linAddr, bool write) {
    if (!(cr0 & 0x80000000u)) return uint32_t(linAddr);
    if (longActive()) return translate4(linAddr, write);
    if (cr4 & 0x20u) {
        stop("PAE paging without long mode is not implemented yet");
        return 0;
    }
    const uint32_t lin = uint32_t(linAddr);
    const bool wp = (cr0 & 0x10000u) != 0;  // WP: supervisor writes respect read-only pages
    const uint32_t pdeAddr = uint32_t(cr3 & 0xFFFFF000u) + (lin >> 22) * 4;
    const uint32_t pde = uint32_t(bus.read(pdeAddr, 4));
    if (!(pde & 1)) throw PageFault{lin, write ? 2u : 0u};
    if ((cr4 & 0x10u) && (pde & 0x80u)) {  // 4 MB page
        if (write && !(pde & 2) && wp) throw PageFault{lin, 3u};
        const uint32_t upd = pde | 0x20u | (write ? 0x40u : 0u);
        if (upd != pde) bus.write(pdeAddr, upd, 4);
        return (pde & 0xFFC00000u) | (lin & 0x3FFFFFu);
    }
    if (!(pde & 0x20u)) bus.write(pdeAddr, pde | 0x20u, 4);
    const uint32_t pteAddr = (pde & 0xFFFFF000u) + (((lin >> 12) & 0x3FFu) * 4);
    const uint32_t pte = uint32_t(bus.read(pteAddr, 4));
    if (!(pte & 1)) throw PageFault{lin, write ? 2u : 0u};
    if (write && !(pte & 2) && wp) throw PageFault{lin, 3u};
    const uint32_t upd = pte | 0x20u | (write ? 0x40u : 0u);
    if (upd != pte) bus.write(pteAddr, upd, 4);
    return (pte & 0xFFFFF000u) | (lin & 0xFFFu);
}

// Long mode paging: four levels of 8-byte entries, with 2 MB pages at the PD level and 1 GB pages at
// the PDPT level. The R/W bit is the AND of the bits at every level.
uint32_t Cpu::translate4(uint64_t lin, bool write) {
    const bool wp = (cr0 & 0x10000u) != 0;
    const uint64_t err = write ? 2u : 0u;
    auto rd = [&](uint64_t a) { return bus.read(uint32_t(a), 8); };
    auto wr = [&](uint64_t a, uint64_t v) { bus.write(uint32_t(a), v, 8); };
    // A table entry: it must be present, and gets the accessed bit.
    auto entry = [&](uint64_t addr, uint64_t& e) {
        e = rd(addr);
        if (!(e & 1)) throw PageFault{lin, uint32_t(err)};
        if (!(e & 0x20)) {
            e |= 0x20;
            wr(addr, e);
        }
    };
    // A page: the write check, the accessed and dirty bits, and the physical address.
    auto leaf = [&](uint64_t addr, uint64_t e, bool writable, uint64_t offMask, uint64_t physMask) -> uint32_t {
        if (write && !writable && wp) throw PageFault{lin, uint32_t(err | 1)};
        const uint64_t upd = e | 0x20 | (write ? 0x40 : 0);
        if (upd != e) wr(addr, upd);
        return uint32_t((e & physMask) | (lin & offMask));
    };
    uint64_t e4 = 0, e3 = 0, e2 = 0, e1 = 0;
    const uint64_t a4 = (cr3 & 0x000FFFFFFFFFF000ull) + ((lin >> 39) & 0x1FF) * 8;
    entry(a4, e4);
    const uint64_t a3 = (e4 & 0x000FFFFFFFFFF000ull) + ((lin >> 30) & 0x1FF) * 8;
    entry(a3, e3);
    bool w = (e4 & 2) && (e3 & 2);
    if (e3 & 0x80) return leaf(a3, e3, w, 0x3FFFFFFFull, 0x000FFFFFC0000000ull);  // 1 GB page
    const uint64_t a2 = (e3 & 0x000FFFFFFFFFF000ull) + ((lin >> 21) & 0x1FF) * 8;
    entry(a2, e2);
    w = w && (e2 & 2);
    if (e2 & 0x80) return leaf(a2, e2, w, 0x1FFFFFull, 0x000FFFFFFFE00000ull);  // 2 MB page
    const uint64_t a1 = (e2 & 0x000FFFFFFFFFF000ull) + ((lin >> 12) & 0x1FF) * 8;
    entry(a1, e1);
    w = w && (e1 & 2);
    return leaf(a1, e1, w, 0xFFFull, 0x000FFFFFFFFFF000ull);
}

uint64_t Cpu::readLin(uint64_t lin, int size) {
    if (!(cr0 & 0x80000000u)) return bus.read(uint32_t(lin), size);
    if ((lin & 0xFFFu) + size <= 0x1000u) return bus.read(translate(lin, false), size);
    uint64_t v = 0;  // the access crosses a page boundary: translate each byte
    for (int i = 0; i < size; ++i) v |= bus.read(translate(lin + i, false), 1) << (8 * i);
    return v;
}

void Cpu::writeLin(uint64_t lin, uint64_t v, int size) {
    if (!(cr0 & 0x80000000u)) {
        bus.write(uint32_t(lin), v, size);
        return;
    }
    if ((lin & 0xFFFu) + size <= 0x1000u) {
        bus.write(translate(lin, true), v, size);
        return;
    }
    for (int i = 0; i < size; ++i) bus.write(translate(lin + i, true), v >> (8 * i), 1);
}

uint64_t Cpu::readRM(const Modrm& m, int size) {
    if (m.mod == 3) return getReg(m.rmx, size);
    return memRead(m.seg, effAddr(m), size);
}

void Cpu::writeRM(const Modrm& m, uint64_t v, int size) {
    if (m.mod == 3) setReg(m.rmx, v, size);
    else memWrite(m.seg, effAddr(m), v, size);
}

// Reads the ModRM byte, the SIB byte and any displacement. Computes the offset and default segment.
// The REX prefix extends the register numbers (REX.R for reg, REX.B for rm and the SIB base,
// REX.X for the SIB index).
Cpu::Modrm Cpu::decodeModrm(int asz, int segOv) {
    Modrm m{};
    uint8_t b = fetch8();
    m.mod = b >> 6;
    m.reg = (b >> 3) & 7;
    m.rm = b & 7;
    m.regx = m.reg | ((rex & 4) ? 8 : 0);
    m.rmx = m.rm | ((rex & 1) ? 8 : 0);
    m.ea = 0;
    m.seg = DS;
    m.ripRel = false;
    if (m.mod == 3) return m;

    if (asz == 2) {
        uint64_t ea = 0;
        bool bpBase = false;
        switch (m.rm) {
        case 0: ea = (r[EBX] & 0xFFFF) + (r[ESI] & 0xFFFF); break;
        case 1: ea = (r[EBX] & 0xFFFF) + (r[EDI] & 0xFFFF); break;
        case 2: ea = (r[EBP] & 0xFFFF) + (r[ESI] & 0xFFFF); bpBase = true; break;
        case 3: ea = (r[EBP] & 0xFFFF) + (r[EDI] & 0xFFFF); bpBase = true; break;
        case 4: ea = r[ESI] & 0xFFFF; break;
        case 5: ea = r[EDI] & 0xFFFF; break;
        case 6:
            if (m.mod == 0) ea = fetch16();
            else { ea = r[EBP] & 0xFFFF; bpBase = true; }
            break;
        case 7: ea = r[EBX] & 0xFFFF; break;
        }
        if (m.mod == 1) ea += uint64_t(int64_t(int8_t(fetch8())));
        else if (m.mod == 2) ea += fetch16();
        m.ea = ea & 0xFFFF;
        m.seg = bpBase ? SS : DS;
    } else {
        int base = m.rm | ((rex & 1) ? 8 : 0);
        int index = -1;
        int scale = 0;
        bool hasBase = true;
        if (m.rm == 4) {
            uint8_t sib = fetch8();
            scale = sib >> 6;
            const int idxRaw = (sib >> 3) & 7;
            if (idxRaw != 4 || (rex & 2)) index = idxRaw | ((rex & 2) ? 8 : 0);
            base = sib & 7;
            if (base == 5 && m.mod == 0) hasBase = false;  // no base, disp32 only
            else base |= (rex & 1) ? 8 : 0;
        } else if (m.rm == 5 && m.mod == 0) {
            // 64-bit code: RIP-relative. Otherwise an absolute disp32.
            if (asz == 8) m.ripRel = true;
            hasBase = false;
        }
        uint64_t ea = 0;
        if (hasBase) ea = r[base];
        if (index >= 0) ea += r[index] << scale;
        if (m.mod == 1) ea += uint64_t(int64_t(int8_t(fetch8())));
        else if (m.mod == 2 || !hasBase) ea += uint64_t(int64_t(int32_t(fetch32())));
        if (asz == 4) ea &= 0xFFFFFFFFull;
        m.ea = ea;
        m.seg = (hasBase && ((base & 7) == ESP || (base & 7) == EBP)) ? SS : DS;
    }
    if (segOv >= 0) m.seg = segOv;
    return m;
}

// ----------------------------------------------------------------------------- flags and arithmetic

void Cpu::setFlag(uint32_t f, bool on) {
    if (on) eflags |= f;
    else eflags &= ~f;
}

void Cpu::setSZP(uint64_t res, int size) {
    setFlag(F_ZF, (res & maskFor(size)) == 0);
    setFlag(F_SF, (res & signFor(size)) != 0);
    setFlag(F_PF, __builtin_parity(uint32_t(res & 0xFF)) == 0);
}

// The eight ALU operations in the order the opcodes use: ADD OR ADC SBB AND SUB XOR CMP.
uint64_t Cpu::alu(int op, uint64_t a, uint64_t b, int size) {
    const uint64_t m = maskFor(size), sb = signFor(size);
    a &= m;
    b &= m;
    uint64_t res = 0;
    switch (op) {
    case 0:
    case 2: {  // ADD, ADC
        uint64_t c = (op == 2 && flag(F_CF)) ? 1 : 0;
        unsigned __int128 full = (unsigned __int128)a + b + c;
        res = uint64_t(full) & m;
        setFlag(F_CF, full > m);
        setFlag(F_OF, ((~(a ^ b)) & (a ^ res) & sb) != 0);
        setFlag(F_AF, ((a ^ b ^ res) & 0x10) != 0);
        break;
    }
    case 3:
    case 5:
    case 7: {  // SBB, SUB, CMP
        uint64_t c = (op == 3 && flag(F_CF)) ? 1 : 0;
        res = (a - b - c) & m;
        setFlag(F_CF, (unsigned __int128)a < (unsigned __int128)b + c);
        setFlag(F_OF, ((a ^ b) & (a ^ res) & sb) != 0);
        setFlag(F_AF, ((a ^ b ^ res) & 0x10) != 0);
        break;
    }
    case 1:  // OR
        res = a | b;
        setFlag(F_CF, false);
        setFlag(F_OF, false);
        setFlag(F_AF, false);
        break;
    case 4:  // AND
        res = a & b;
        setFlag(F_CF, false);
        setFlag(F_OF, false);
        setFlag(F_AF, false);
        break;
    default:  // XOR
        res = a ^ b;
        setFlag(F_CF, false);
        setFlag(F_OF, false);
        setFlag(F_AF, false);
        break;
    }
    setSZP(res, size);
    return res;
}

// Group 2: ROL ROR (RCL RCR not yet) SHL SHR SAR.
uint64_t Cpu::shift(int op, uint64_t v, int count, int size) {
    const uint64_t m = maskFor(size), sb = signFor(size);
    const int bits = size * 8;
    v &= m;
    count &= (size == 8) ? 0x3F : 0x1F;
    if (count == 0) return v;
    uint64_t res = v;
    switch (op) {
    case 0: {  // ROL
        int c = count % bits;
        res = c ? (((v << c) | (v >> (bits - c))) & m) : v;
        bool cf = (res & 1) != 0;
        setFlag(F_CF, cf);
        setFlag(F_OF, ((res & sb) != 0) != cf);
        break;
    }
    case 1: {  // ROR
        int c = count % bits;
        res = c ? (((v >> c) | (v << (bits - c))) & m) : v;
        setFlag(F_CF, (res & sb) != 0);
        setFlag(F_OF, ((res & sb) != 0) != ((res & (sb >> 1)) != 0));
        break;
    }
    case 4:
    case 6: {  // SHL
        bool cf = count <= bits ? (((v >> (bits - count)) & 1) != 0) : false;
        res = (v << count) & m;
        setFlag(F_CF, cf);
        setFlag(F_OF, ((res & sb) != 0) != cf);
        setSZP(res, size);
        break;
    }
    case 5: {  // SHR
        bool cf = ((v >> (count - 1)) & 1) != 0;
        res = v >> count;
        setFlag(F_CF, cf);
        setFlag(F_OF, (v & sb) != 0);
        setSZP(res, size);
        break;
    }
    case 7: {  // SAR
        int64_t sv = sext(v, size);
        int c = count > 63 ? 63 : count;
        bool cf = ((sv >> (c - 1)) & 1) != 0;
        res = uint64_t(sv >> c) & m;
        setFlag(F_CF, cf);
        setFlag(F_OF, false);
        setSZP(res, size);
        break;
    }
    case 2: {  // RCL: rotate left through carry
        bool cf = flag(F_CF);
        const int n = size < 4 ? count % (bits + 1) : count;
        for (int i = 0; i < n; ++i) {
            const bool out = (v & sb) != 0;
            v = ((v << 1) | (cf ? 1 : 0)) & m;
            cf = out;
        }
        setFlag(F_CF, cf);
        setFlag(F_OF, ((v & sb) != 0) != cf);
        return v;
    }
    case 3: {  // RCR: rotate right through carry
        bool cf = flag(F_CF);
        const int n = size < 4 ? count % (bits + 1) : count;
        for (int i = 0; i < n; ++i) {
            const bool out = (v & 1) != 0;
            v = (v >> 1) | (cf ? sb : 0);
            cf = out;
        }
        setFlag(F_CF, cf);
        setFlag(F_OF, ((v & sb) != 0) != ((v & (sb >> 1)) != 0));
        return v;
    }
    default:
        stop("unknown shift operation");
        break;
    }
    return res;
}

// INC and DEC leave CF alone.
uint64_t Cpu::incdec(uint64_t v, int size, bool dec) {
    const uint64_t m = maskFor(size), sb = signFor(size);
    v &= m;
    uint64_t res = dec ? ((v - 1) & m) : ((v + 1) & m);
    setFlag(F_OF, dec ? (v == sb) : (v == sb - 1));
    setFlag(F_AF, ((v ^ res) & 0x10) != 0);
    setSZP(res, size);
    return res;
}

bool Cpu::condition(int cc) const {
    bool res;
    switch (cc >> 1) {
    case 0: res = flag(F_OF); break;
    case 1: res = flag(F_CF); break;
    case 2: res = flag(F_ZF); break;
    case 3: res = flag(F_CF) || flag(F_ZF); break;
    case 4: res = flag(F_SF); break;
    case 5: res = flag(F_PF); break;
    case 6: res = flag(F_SF) != flag(F_OF); break;
    default: res = flag(F_ZF) || (flag(F_SF) != flag(F_OF)); break;
    }
    return (cc & 1) ? !res : res;
}

// MUL, IMUL, DIV and IDIV on AL, AX, EAX or RAX. Returns false if a divide error was raised.
bool Cpu::muldiv(int reg, uint64_t v, int size) {
    v &= maskFor(size);
    switch (reg) {
    case 4: {  // MUL
        unsigned __int128 p = (unsigned __int128)getReg(EAX, size) * v;
        if (size == 1) {
            setReg(EAX, uint64_t(p), WORD);
            setFlag(F_CF, (p >> 8) != 0);
        } else {
            setReg(EAX, uint64_t(p), size);
            setReg(EDX, uint64_t(p >> (8 * size)), size);
            setFlag(F_CF, (p >> (8 * size)) != 0);
        }
        setFlag(F_OF, flag(F_CF));
        return true;
    }
    case 5: {  // IMUL
        __int128 p = (__int128)sext(getReg(EAX, size), size) * sext(v, size);
        const bool over = (__int128)sext(uint64_t(p), size) != p;
        if (size == 1) {
            setReg(EAX, uint64_t(p), WORD);
        } else {
            setReg(EAX, uint64_t(p), size);
            setReg(EDX, uint64_t(p >> (8 * size)), size);
        }
        setFlag(F_CF, over);
        setFlag(F_OF, over);
        return true;
    }
    case 6: {  // DIV
        if (v == 0) { raise(0); return false; }
        if (size == 1) {
            uint64_t n = getReg(EAX, WORD);
            uint64_t q = n / v;
            if (q > 0xFF) { raise(0); return false; }
            setReg(EAX, (q & 0xFF) | ((n % v) << 8), WORD);
        } else if (size == 2) {
            uint64_t n = (getReg(EDX, WORD) << 16) | getReg(EAX, WORD);
            uint64_t q = n / v;
            if (q > 0xFFFF) { raise(0); return false; }
            setReg(EAX, q, WORD);
            setReg(EDX, n % v, WORD);
        } else if (size == 4) {
            uint64_t n = (getReg(EDX, DWORD) << 32) | getReg(EAX, DWORD);
            uint64_t q = n / v;
            if (q > 0xFFFFFFFFull) { raise(0); return false; }
            setReg(EAX, q, DWORD);
            setReg(EDX, n % v, DWORD);
        } else {
            unsigned __int128 n = ((unsigned __int128)getReg(EDX, QWORD) << 64) | getReg(EAX, QWORD);
            unsigned __int128 q = n / v;
            if (q > ~0ull) { raise(0); return false; }
            setReg(EAX, uint64_t(q), QWORD);
            setReg(EDX, uint64_t(n % v), QWORD);
        }
        return true;
    }
    default: {  // IDIV
        const int64_t d = sext(v, size);
        if (d == 0) { raise(0); return false; }
        __int128 n;
        if (size == 1) n = int16_t(getReg(EAX, WORD));
        else if (size == 2) n = int32_t((getReg(EDX, WORD) << 16) | getReg(EAX, WORD));
        else if (size == 4) n = int64_t((getReg(EDX, DWORD) << 32) | getReg(EAX, DWORD));
        else n = (__int128)(((unsigned __int128)getReg(EDX, QWORD) << 64) | getReg(EAX, QWORD));
        const __int128 q = n / d;
        const __int128 rem = n % d;
        const __int128 lim = size == 1 ? 127 : size == 2 ? 32767 : size == 4 ? 2147483647 : (__int128)INT64_MAX;
        if (q > lim || q < -lim - 1) { raise(0); return false; }
        if (size == 1) {
            setReg(EAX, (uint64_t(q) & 0xFF) | ((uint64_t(rem) & 0xFF) << 8), WORD);
        } else {
            setReg(EAX, uint64_t(q), size);
            setReg(EDX, uint64_t(rem), size);
        }
        return true;
    }
    }
}

// ----------------------------------------------------------------------------- stack, jumps, segments

// In 64-bit code the stack pointer is RSP and pushes and pops move it by their size.
void Cpu::push(uint64_t v, int size) {
    if (in64()) {
        r[ESP] -= size;
        memWrite(SS, r[ESP], v, size);
    } else if (seg[SS].big) {
        r[ESP] = uint32_t(r[ESP] - size);
        memWrite(SS, r[ESP], v, size);
    } else {
        uint16_t sp = uint16_t(r[ESP] - size);
        r[ESP] = (r[ESP] & 0xFFFF0000ull) | sp;
        memWrite(SS, sp, v, size);
    }
}

uint64_t Cpu::pop(int size) {
    if (in64()) {
        uint64_t v = memRead(SS, r[ESP], size);
        r[ESP] += size;
        return v;
    }
    if (seg[SS].big) {
        uint64_t v = memRead(SS, r[ESP], size);
        r[ESP] = uint32_t(r[ESP] + size);
        return v;
    }
    uint16_t sp = uint16_t(r[ESP]);
    uint64_t v = memRead(SS, sp, size);
    r[ESP] = (r[ESP] & 0xFFFF0000ull) | uint16_t(sp + size);
    return v;
}

void Cpu::jumpRel(int32_t d) {
    const bool wide = seg[CS].big || in64();
    eip = wide ? (eip + uint32_t(d)) : ((eip + uint32_t(d)) & 0xFFFF);
}

// Loads a segment register. In real mode the selector is the paragraph number. In protected mode the
// descriptor comes from the GDT.
void Cpu::loadSeg(int s, uint16_t sel) {
    Segment& d = seg[s];
    if (!(cr0 & 1)) {
        d.sel = sel;
        d.base = uint32_t(sel) << 4;
        d.limit = 0xFFFF;
        d.big = false;
        d.longMode = false;
        return;
    }
    if (sel < 8 && s != CS) {  // null selector
        d.sel = sel;
        d.base = 0;
        d.limit = 0;
        d.big = false;
        d.longMode = false;
        return;
    }
    uint32_t off = sel & ~7u;
    if (off + 7 > gdtLimit) {
        stop("selector is outside the GDT");
        return;
    }
    uint32_t lo = uint32_t(readLin(gdtBase + off, 4));
    uint32_t hi = uint32_t(readLin(gdtBase + off + 4, 4));
    uint32_t base = (lo >> 16) | ((hi & 0xFF) << 16) | (hi & 0xFF000000u);
    uint32_t limit = (lo & 0xFFFF) | (hi & 0xF0000u);
    if (hi & 0x800000u) limit = (limit << 12) | 0xFFF;  // granularity bit
    d.sel = sel;
    d.base = base;
    d.limit = limit;
    d.big = (hi & 0x400000u) != 0;
    d.longMode = (hi & 0x200000u) != 0;
}

// Delivers an interrupt or exception. Real mode uses the vector table at address 0. Protected mode
// uses the IDT, and long mode uses the 64-bit IDT (see deliver64).
void Cpu::deliver(int vec, bool hasError, uint32_t error) {
    if (!(cr0 & 1)) {
        push(eflags & 0xFFFF, WORD);
        push(seg[CS].sel, WORD);
        push(eip & 0xFFFF, WORD);
        setFlag(F_IF, false);
        setFlag(F_TF, false);
        const uint64_t entry = idtBase + uint64_t(vec) * 4;
        uint16_t newIp = uint16_t(readLin(entry, 2));
        uint16_t newCs = uint16_t(readLin(entry + 2, 2));
        loadSeg(CS, newCs);
        eip = newIp;
        return;
    }
    if (longActive()) {
        deliver64(vec, hasError, error);
        return;
    }
    if (uint32_t(vec) * 8 + 7 > idtLimit) {
        stop("interrupt number is outside the IDT");
        return;
    }
    const uint64_t g = idtBase + uint64_t(vec) * 8;
    uint32_t lo = uint32_t(readLin(g, 4));
    uint32_t hi = uint32_t(readLin(g + 4, 4));
    if (!(hi & 0x8000u)) {
        stop("interrupt gate is not present");
        return;
    }
    uint16_t sel = uint16_t(lo >> 16);
    uint32_t off = (lo & 0xFFFF) | (hi & 0xFFFF0000u);
    uint32_t type = (hi >> 8) & 0xF;  // 0xE interrupt gate, 0xF trap gate
    push(eflags, DWORD);
    push(seg[CS].sel, DWORD);
    push(eip, DWORD);
    if (hasError) push(error, DWORD);
    if (type == 0xE) setFlag(F_IF, false);
    setFlag(F_TF, false);
    loadSeg(CS, sel);
    eip = off;
}

// Long mode interrupt: 16-byte gates, and the frame is SS, RSP, RFLAGS, CS, RIP (and the error code).
// The stack pointer is aligned to 16 bytes first. There is no privilege change yet.
void Cpu::deliver64(int vec, bool hasError, uint32_t error) {
    if (uint32_t(vec) * 16 + 15 > idtLimit) {
        stop("interrupt number is outside the IDT");
        return;
    }
    const uint64_t g = idtBase + uint64_t(vec) * 16;
    const uint64_t lo = readLin(g, 8);
    const uint64_t hi = readLin(g + 8, 4);
    if (!((lo >> 47) & 1)) {
        stop("interrupt gate is not present");
        return;
    }
    const uint16_t sel = uint16_t(lo >> 16);
    const uint64_t off = (lo & 0xFFFF) | (((lo >> 48) & 0xFFFF) << 16) | (hi << 32);
    const uint32_t type = uint32_t((lo >> 40) & 0xF);  // 0xE interrupt gate, 0xF trap gate
    const uint64_t oldSs = seg[SS].sel, oldRsp = r[ESP], oldFlags = eflags;
    const uint64_t oldCs = seg[CS].sel, oldRip = eip;
    r[ESP] &= ~0xFull;
    push(oldSs, QWORD);
    push(oldRsp, QWORD);
    push(oldFlags, QWORD);
    push(oldCs, QWORD);
    push(oldRip, QWORD);
    if (hasError) push(error, QWORD);
    if (type == 0xE) setFlag(F_IF, false);
    setFlag(F_TF, false);
    loadSeg(CS, sel);
    eip = uint32_t(off);
}

void Cpu::raise(int vector, bool hasError, uint32_t error) { deliver(vector, hasError, error); }

// String instructions: MOVS CMPS STOS LODS SCAS, with REP, REPE and REPNE.
void Cpu::stringOp(uint8_t op, int segOv, int asz, int osz, uint8_t rep) {
    const int sz = (op & 1) ? osz : BYTE;
    const int srcSeg = segOv >= 0 ? segOv : DS;
    const int stepBy = flag(F_DF) ? -sz : sz;
    const uint64_t amask = asz == 8 ? ~0ull : asz == 4 ? 0xFFFFFFFFull : 0xFFFFull;
    auto addr = [&](uint64_t v) { return v & amask; };
    auto count = [&]() -> uint64_t { return r[ECX] & amask; };
    auto setCount = [&](uint64_t v) { setAddr(r[ECX], v, asz); };
    auto bump = [&](int reg, int by) { setAddr(r[reg], r[reg] + uint64_t(int64_t(by)), asz); };
    const uint8_t base = op & 0xFE;
    for (;;) {
        if (rep && count() == 0) break;
        switch (base) {
        case 0xA4: {  // MOVS
            uint64_t v = memRead(srcSeg, addr(r[ESI]), sz);
            memWrite(ES, addr(r[EDI]), v, sz);
            bump(ESI, stepBy);
            bump(EDI, stepBy);
            break;
        }
        case 0xAA:  // STOS
            memWrite(ES, addr(r[EDI]), getReg(EAX, sz), sz);
            bump(EDI, stepBy);
            break;
        case 0xAC:  // LODS
            setReg(EAX, memRead(srcSeg, addr(r[ESI]), sz), sz);
            bump(ESI, stepBy);
            break;
        case 0xA6: {  // CMPS
            uint64_t a = memRead(srcSeg, addr(r[ESI]), sz);
            uint64_t b = memRead(ES, addr(r[EDI]), sz);
            alu(7, a, b, sz);
            bump(ESI, stepBy);
            bump(EDI, stepBy);
            break;
        }
        case 0xAE: {  // SCAS
            uint64_t b = memRead(ES, addr(r[EDI]), sz);
            alu(7, getReg(EAX, sz), b, sz);
            bump(EDI, stepBy);
            break;
        }
        default:
            stop("unsupported string instruction");
            return;
        }
        if (!rep) break;
        setCount(count() - 1);
        if (base == 0xA6 || base == 0xAE) {
            if (rep == 0xF3 && !flag(F_ZF)) break;  // REPE stops on not-equal
            if (rep == 0xF2 && flag(F_ZF)) break;   // REPNE stops on equal
        }
    }
}

// ----------------------------------------------------------------------------- one instruction

// Runs one step: an interrupt if one is waiting, otherwise one instruction. A page fault anywhere in
// it becomes exception 14, and the instruction is restarted when the handler returns.
bool Cpu::step() {
    if (stopped) return false;
    try {
        instrEip = eip;
        instrCs = seg[CS].sel;
        int vec = -1;
        if (flag(F_IF)) {
            if (pendingVector >= 0) {
                vec = pendingVector;
                pendingVector = -1;
            } else if (pollInterrupt) {
                vec = pollInterrupt();
            }
        }
        if (vec >= 0) {
            halted = false;
            deliver(vec, false, 0);
            return !stopped;
        }
        if (halted) return true;  // waiting for an interrupt; time still passes
        ++cycles;
        execute();
    } catch (const PageFault& pf) {
        cr2 = pf.addr;
        eip = instrEip;
        halted = false;
        try {
            deliver(14, true, pf.err);
        } catch (const PageFault&) {
            stop("double fault while delivering a page fault");
        }
    }
    return !stopped;
}

void Cpu::execute() {
    // Legacy prefixes
    bool opOv = false, adOv = false;
    int segOv = -1;
    uint8_t rep = 0;
    uint8_t op;
    rex = 0;
    for (;;) {
        op = fetch8();
        switch (op) {
        case 0x66: opOv = true; continue;
        case 0x67: adOv = true; continue;
        case 0x26: segOv = ES; continue;
        case 0x2E: segOv = CS; continue;
        case 0x36: segOv = SS; continue;
        case 0x3E: segOv = DS; continue;
        case 0x64: segOv = FS; continue;
        case 0x65: segOv = GS; continue;
        case 0xF0: continue;  // LOCK: one CPU, nothing to lock against
        case 0xF2:
        case 0xF3: rep = op; continue;
        default: break;
        }
        break;
    }
    // In 64-bit code a REX prefix (0x40-0x4F) must come right before the opcode.
    const bool lm = in64();
    if (lm && (op & 0xF0) == 0x40) {
        rex = op;
        op = fetch8();
    }
    const bool rexW = (rex & 8) != 0;
    int osz;
    if (rexW) osz = QWORD;
    else if (lm) osz = opOv ? WORD : DWORD;
    else osz = (seg[CS].big != opOv) ? DWORD : WORD;
    const int asz = lm ? (adOv ? DWORD : QWORD) : ((seg[CS].big != adOv) ? DWORD : WORD);
    const int ssz = lm ? (opOv ? WORD : QWORD) : osz;  // stack operand size: 64-bit by default in 64-bit code
    const int oreg = (op & 7) | ((rex & 1) ? 8 : 0);   // register named in the low 3 bits of the opcode

    // ALU block 00-3F
    if (op < 0x40 && (op & 7) < 6) {
        const int aluOp = op >> 3;
        switch (op & 7) {
        case 0: {
            Modrm m = decodeModrm(asz, segOv);
            uint64_t res = alu(aluOp, readRM(m, BYTE), getReg(m.regx, BYTE), BYTE);
            if (aluOp != 7) writeRM(m, res, BYTE);
            break;
        }
        case 1: {
            Modrm m = decodeModrm(asz, segOv);
            uint64_t res = alu(aluOp, readRM(m, osz), getReg(m.regx, osz), osz);
            if (aluOp != 7) writeRM(m, res, osz);
            break;
        }
        case 2: {
            Modrm m = decodeModrm(asz, segOv);
            uint64_t res = alu(aluOp, getReg(m.regx, BYTE), readRM(m, BYTE), BYTE);
            if (aluOp != 7) setReg(m.regx, res, BYTE);
            break;
        }
        case 3: {
            Modrm m = decodeModrm(asz, segOv);
            uint64_t res = alu(aluOp, getReg(m.regx, osz), readRM(m, osz), osz);
            if (aluOp != 7) setReg(m.regx, res, osz);
            break;
        }
        case 4: {
            uint64_t imm = fetch8();
            uint64_t res = alu(aluOp, getReg(EAX, BYTE), imm, BYTE);
            if (aluOp != 7) setReg(EAX, res, BYTE);
            break;
        }
        default: {
            uint64_t imm = fetchImm(osz);
            uint64_t res = alu(aluOp, getReg(EAX, osz), imm, osz);
            if (aluOp != 7) setReg(EAX, res, osz);
            break;
        }
        }
        return;
    }

    switch (op) {
    case 0x06: push(seg[ES].sel, ssz); break;
    case 0x0E: push(seg[CS].sel, ssz); break;
    case 0x16: push(seg[SS].sel, ssz); break;
    case 0x1E: push(seg[DS].sel, ssz); break;
    case 0x07: loadSeg(ES, uint16_t(pop(ssz))); break;
    case 0x17: loadSeg(SS, uint16_t(pop(ssz))); break;
    case 0x1F: loadSeg(DS, uint16_t(pop(ssz))); break;

    case 0x0F: {  // two-byte opcodes
        uint8_t op2 = fetch8();
        switch (op2) {
        case 0x00: {  // group 6: task register and segment checks (no task state is modelled)
            Modrm m = decodeModrm(asz, segOv);
            switch (m.reg) {
            case 0: case 1: writeRM(m, 0, WORD); break;  // SLDT, STR: no task selector
            case 2: case 3: break;                       // LLDT, LTR
            case 4: case 5: setFlag(F_ZF, true); break;  // VERR, VERW: selector taken as valid
            default: stop("unsupported group 6 instruction"); break;
            }
            break;
        }
        case 0x01: {  // group 7: GDT, IDT, machine status
            Modrm m = decodeModrm(asz, segOv);
            const uint64_t ea = effAddr(m);
            const int bs = lm ? QWORD : DWORD;  // the base in a descriptor table pointer is 8 bytes in 64-bit code
            switch (m.reg) {
            case 0:  // SGDT
                memWrite(m.seg, ea, gdtLimit, WORD);
                memWrite(m.seg, ea + 2, gdtBase, bs);
                break;
            case 1:  // SIDT
                memWrite(m.seg, ea, idtLimit, WORD);
                memWrite(m.seg, ea + 2, idtBase, bs);
                break;
            case 2:  // LGDT
                gdtLimit = uint32_t(memRead(m.seg, ea, WORD));
                gdtBase = memRead(m.seg, ea + 2, bs);
                break;
            case 3:  // LIDT
                idtLimit = uint32_t(memRead(m.seg, ea, WORD));
                idtBase = memRead(m.seg, ea + 2, bs);
                break;
            case 4:  // SMSW
                writeRM(m, cr0 & 0xFFFF, WORD);
                break;
            case 6: {  // LMSW: can set PE, cannot clear it
                uint32_t v = uint32_t(readRM(m, WORD));
                cr0 = (cr0 & ~0xFu) | (v & 0xF) | (cr0 & 1);
                break;
            }
            default:
                stop("unsupported group 7 instruction");
                break;
            }
            break;
        }
        case 0x0B:  // UD2
            raise(6);
            break;
        case 0x20: {  // MOV r, CRn
            Modrm m = decodeModrm(asz, segOv);
            uint64_t v;
            switch (m.reg) {
            case 0: v = cr0; break;
            case 2: v = cr2; break;
            case 3: v = cr3; break;
            case 4: v = cr4; break;
            default: stop("unsupported control register"); return;
            }
            setReg(m.rmx, v, lm ? QWORD : DWORD);
            break;
        }
        case 0x22: {  // MOV CRn, r
            Modrm m = decodeModrm(asz, segOv);
            const uint64_t v = getReg(m.rmx, lm ? QWORD : DWORD);
            switch (m.reg) {
            case 0: {  // CR0: setting PG while EFER.LME is set turns on long mode (LMA)
                const bool pg = (v & 0x80000000ull) != 0;
                if (pg && (efer & EFER_LME)) {
                    if (!(cr4 & 0x20u)) { stop("long mode needs CR4.PAE"); return; }
                    efer |= EFER_LMA;
                } else if (!pg) {
                    efer &= ~EFER_LMA;
                }
                cr0 = uint32_t(v);
                break;
            }
            case 2: cr2 = v; break;
            case 3: cr3 = v; break;
            case 4: cr4 = uint32_t(v); break;
            default: stop("unsupported control register"); return;
            }
            break;
        }
        case 0x30: {  // WRMSR
            uint64_t val = (uint64_t(r[EDX] & 0xFFFFFFFFu) << 32) | (r[EAX] & 0xFFFFFFFFu);
            if ((r[ECX] & 0xFFFFFFFFu) == 0xC0000080u) {
                // LMA is set by the CPU when paging turns on, so software cannot write it.
                efer = (efer & EFER_LMA) | (val & (EFER_SCE | EFER_LME | EFER_NXE));
            }
            break;
        }
        case 0x31: {  // RDTSC
            r[EAX] = uint32_t(cycles);
            r[EDX] = uint32_t(cycles >> 32);
            break;
        }
        case 0x32: {  // RDMSR
            uint64_t val = 0;
            if ((r[ECX] & 0xFFFFFFFFu) == 0xC0000080u) val = efer;
            else if ((r[ECX] & 0xFFFFFFFFu) == 0x1B) val = 0xFEE00900u;  // APIC base
            r[EAX] = uint32_t(val);
            r[EDX] = uint32_t(val >> 32);
            break;
        }
        case 0xA2: {  // CPUID
            switch (uint32_t(r[EAX])) {
            case 0:
                r[EAX] = 1;
                r[EBX] = 0x756E6547;  // "Genu"
                r[EDX] = 0x49656E69;  // "ineI"
                r[ECX] = 0x6C65746E;  // "ntel"
                break;
            case 1:
                r[EAX] = 0x000006F1;
                r[EBX] = 0;
                r[ECX] = 0;
                // FPU, TSC, MSR, PAE, CX8, APIC, CMOV, MMX, FXSR, SSE, SSE2
                r[EDX] = (1u << 0) | (1u << 4) | (1u << 5) | (1u << 6) | (1u << 8) | (1u << 9) | (1u << 15) |
                         (1u << 23) | (1u << 24) | (1u << 25) | (1u << 26);
                break;
            case 0x80000000u:
                r[EAX] = 0x80000001u;
                r[EBX] = r[ECX] = r[EDX] = 0;
                break;
            case 0x80000001u:
                r[EAX] = r[EBX] = r[ECX] = 0;
                r[EDX] = 1u << 29;  // LM: long mode supported
                break;
            default:
                r[EAX] = r[EBX] = r[ECX] = r[EDX] = 0;
                break;
            }
            break;
        }
        case 0xA3: {  // BT r/m, r
            Modrm m = decodeModrm(asz, segOv);
            uint64_t bits = osz * 8;
            uint64_t v = readRM(m, osz);
            setFlag(F_CF, ((v >> (getReg(m.regx, osz) % bits)) & 1) != 0);
            break;
        }
        case 0xAF: {  // IMUL r, r/m
            Modrm m = decodeModrm(asz, segOv);
            __int128 p = (__int128)sext(getReg(m.regx, osz), osz) * sext(readRM(m, osz), osz);
            setReg(m.regx, uint64_t(p), osz);
            bool over = (__int128)sext(uint64_t(p), osz) != p;
            setFlag(F_CF, over);
            setFlag(F_OF, over);
            break;
        }
        case 0xB6:    // MOVZX r, r/m8
        case 0xB7: {  // MOVZX r, r/m16
            Modrm m = decodeModrm(asz, segOv);
            setReg(m.regx, readRM(m, op2 == 0xB6 ? BYTE : WORD), osz);
            break;
        }
        case 0xBE:    // MOVSX r, r/m8
        case 0xBF: {  // MOVSX r, r/m16
            Modrm m = decodeModrm(asz, segOv);
            int s = op2 == 0xBE ? BYTE : WORD;
            setReg(m.regx, uint64_t(sext(readRM(m, s), s)), osz);
            break;
        }
        case 0xBC:    // BSF
        case 0xBD: {  // BSR
            Modrm m = decodeModrm(asz, segOv);
            uint64_t v = readRM(m, osz) & maskFor(osz);
            if (v == 0) {
                setFlag(F_ZF, true);
            } else {
                setFlag(F_ZF, false);
                uint64_t idx = op2 == 0xBC ? uint64_t(__builtin_ctzll(v))
                                           : uint64_t(63 - __builtin_clzll(v));
                setReg(m.regx, idx, osz);
            }
            break;
        }
        case 0xC8: case 0xC9: case 0xCA: case 0xCB:
        case 0xCC: case 0xCD: case 0xCE: case 0xCF: {  // BSWAP
            const int reg = (op2 & 7) | ((rex & 1) ? 8 : 0);
            uint64_t v = r[reg];
            r[reg] = rexW ? __builtin_bswap64(v) : uint64_t(__builtin_bswap32(uint32_t(v)));
            break;
        }
        default:
            if (extended0F(op2, opOv, rep, asz, segOv, osz)) break;
            if (op2 >= 0x40 && op2 <= 0x4F) {  // CMOVcc
                Modrm m = decodeModrm(asz, segOv);
                uint64_t v = readRM(m, osz);
                if (condition(op2 & 0xF)) setReg(m.regx, v, osz);
            } else if (op2 >= 0x80 && op2 <= 0x8F) {  // Jcc rel16/rel32
                int32_t rel = osz == 2 ? int32_t(int16_t(fetch16())) : int32_t(fetch32());
                if (condition(op2 & 0xF)) jumpRel(rel);
            } else if (op2 >= 0x90 && op2 <= 0x9F) {  // SETcc
                Modrm m = decodeModrm(asz, segOv);
                writeRM(m, condition(op2 & 0xF) ? 1 : 0, BYTE);
            } else {
                char what[64];
                std::snprintf(what, sizeof what, "unsupported opcode 0F %02X", op2);
                stop(what);
            }
            break;
        }
        break;
    }

    case 0x40: case 0x41: case 0x42: case 0x43:
    case 0x44: case 0x45: case 0x46: case 0x47:
        setReg(oreg, incdec(getReg(oreg, osz), osz, false), osz);  // 32-bit code only: in 64-bit code 0x40-0x4F is REX
        break;
    case 0x48: case 0x49: case 0x4A: case 0x4B:
    case 0x4C: case 0x4D: case 0x4E: case 0x4F:
        setReg(oreg, incdec(getReg(oreg, osz), osz, true), osz);
        break;

    case 0x50: case 0x51: case 0x52: case 0x53:
    case 0x54: case 0x55: case 0x56: case 0x57:
        push(getReg(oreg, ssz), ssz);
        break;
    case 0x58: case 0x59: case 0x5A: case 0x5B:
    case 0x5C: case 0x5D: case 0x5E: case 0x5F:
        setReg(oreg, pop(ssz), ssz);
        break;

    case 0x60: {  // PUSHA
        uint64_t sp = r[ESP];
        const int order[] = {EAX, ECX, EDX, EBX, -1, EBP, ESI, EDI};
        for (int reg : order) push(reg < 0 ? sp : r[reg], osz);
        break;
    }
    case 0x61: {  // POPA
        const int order[] = {EDI, ESI, EBP, -1, EBX, EDX, ECX, EAX};
        for (int reg : order) {
            uint64_t v = pop(osz);
            if (reg >= 0) setReg(reg, v, osz);
        }
        break;
    }

    case 0x63: {
        Modrm m = decodeModrm(asz, segOv);
        if (lm) {  // MOVSXD r, r/m32
            setReg(m.regx, uint64_t(int64_t(int32_t(readRM(m, DWORD)))), rexW ? QWORD : DWORD);
        } else {   // ARPL r/m16, r16: raise the destination's RPL to the source's
            const uint64_t dst = readRM(m, WORD);
            const uint64_t src = getReg(m.regx, WORD);
            if ((dst & 3) < (src & 3)) {
                writeRM(m, (dst & ~3ull) | (src & 3), WORD);
                setFlag(F_ZF, true);
            } else {
                setFlag(F_ZF, false);
            }
        }
        break;
    }
    case 0x6C: case 0x6D: case 0x6E: case 0x6F: {  // INS, OUTS (port in DX)
        const bool isIn = op < 0x6E;
        const int sz = (op & 1) ? (osz == 2 ? WORD : DWORD) : BYTE;
        const int stepBy = flag(F_DF) ? -sz : sz;
        const uint64_t amask = asz == 8 ? ~0ull : asz == 4 ? 0xFFFFFFFFull : 0xFFFFull;
        const uint16_t port = uint16_t(r[EDX]);
        for (;;) {
            if (rep && (r[ECX] & amask) == 0) break;
            if (isIn) {
                const uint64_t v = bus.ioIn ? bus.ioIn(port, sz) : maskFor(sz);
                memWrite(ES, r[EDI] & amask, v, sz);
                setAddr(r[EDI], r[EDI] + uint64_t(int64_t(stepBy)), asz);
            } else {
                const uint64_t v = memRead(segOv >= 0 ? segOv : DS, r[ESI] & amask, sz);
                if (bus.ioOut) bus.ioOut(port, uint32_t(v), sz);
                setAddr(r[ESI], r[ESI] + uint64_t(int64_t(stepBy)), asz);
            }
            if (!rep) break;
            setAddr(r[ECX], (r[ECX] & amask) - 1, asz);
        }
        break;
    }
    case 0x68:  // PUSH imm16/32
        push(fetchImm(ssz), ssz);
        break;
    case 0x6A:  // PUSH imm8
        push(uint64_t(int64_t(int8_t(fetch8()))), ssz);
        break;
    case 0x69:    // IMUL r, r/m, imm16/32
    case 0x6B: {  // IMUL r, r/m, imm8
        Modrm m = decodeModrm(asz, segOv);
        int64_t imm = op == 0x69 ? sext(fetchImm(osz), osz)
                                 : int64_t(int8_t(fetch8()));
        __int128 p = (__int128)sext(readRM(m, osz), osz) * imm;
        setReg(m.regx, uint64_t(p), osz);
        bool over = (__int128)sext(uint64_t(p), osz) != p;
        setFlag(F_CF, over);
        setFlag(F_OF, over);
        break;
    }

    case 0x70: case 0x71: case 0x72: case 0x73:
    case 0x74: case 0x75: case 0x76: case 0x77:
    case 0x78: case 0x79: case 0x7A: case 0x7B:
    case 0x7C: case 0x7D: case 0x7E: case 0x7F: {
        int32_t rel = int32_t(int8_t(fetch8()));
        if (condition(op & 0xF)) jumpRel(rel);
        break;
    }

    case 0x80:
    case 0x81:
    case 0x83: {  // group 1: ALU with immediate
        int sz = op == 0x80 ? BYTE : osz;
        Modrm m = decodeModrm(asz, segOv);
        uint64_t imm;
        if (op == 0x80) imm = fetch8();
        else if (op == 0x81) imm = fetchImm(sz);
        else imm = uint64_t(int64_t(int8_t(fetch8())));
        uint64_t res = alu(m.reg, readRM(m, sz), imm, sz);
        if (m.reg != 7) writeRM(m, res, sz);
        break;
    }
    case 0x84:
    case 0x85: {  // TEST r/m, r
        int sz = op == 0x84 ? BYTE : osz;
        Modrm m = decodeModrm(asz, segOv);
        uint64_t res = readRM(m, sz) & getReg(m.regx, sz);
        setFlag(F_CF, false);
        setFlag(F_OF, false);
        setSZP(res, sz);
        break;
    }
    case 0x86:
    case 0x87: {  // XCHG r/m, r
        int sz = op == 0x86 ? BYTE : osz;
        Modrm m = decodeModrm(asz, segOv);
        uint64_t a = readRM(m, sz), b = getReg(m.regx, sz);
        writeRM(m, b, sz);
        setReg(m.regx, a, sz);
        break;
    }
    case 0x88: {  // MOV r/m8, r8
        Modrm m = decodeModrm(asz, segOv);
        writeRM(m, getReg(m.regx, BYTE), BYTE);
        break;
    }
    case 0x89: {  // MOV r/m, r
        Modrm m = decodeModrm(asz, segOv);
        writeRM(m, getReg(m.regx, osz), osz);
        break;
    }
    case 0x8A: {  // MOV r8, r/m8
        Modrm m = decodeModrm(asz, segOv);
        setReg(m.regx, readRM(m, BYTE), BYTE);
        break;
    }
    case 0x8B: {  // MOV r, r/m
        Modrm m = decodeModrm(asz, segOv);
        setReg(m.regx, readRM(m, osz), osz);
        break;
    }
    case 0x8C: {  // MOV r/m, Sreg
        Modrm m = decodeModrm(asz, segOv);
        if (m.reg > 5) { stop("bad segment register"); break; }
        writeRM(m, seg[m.reg].sel, WORD);
        break;
    }
    case 0x8D: {  // LEA
        Modrm m = decodeModrm(asz, segOv);
        setReg(m.regx, effAddr(m), osz);
        break;
    }
    case 0x8E: {  // MOV Sreg, r/m16
        Modrm m = decodeModrm(asz, segOv);
        if (m.reg > 5) { stop("bad segment register"); break; }
        loadSeg(m.reg, uint16_t(readRM(m, WORD)));
        break;
    }
    case 0x8F: {  // POP r/m
        Modrm m = decodeModrm(asz, segOv);
        writeRM(m, pop(ssz), ssz);
        break;
    }

    case 0x90:  // NOP, or XCHG r8, rax with REX.B
    case 0x91: case 0x92: case 0x93: case 0x94:
    case 0x95: case 0x96: case 0x97: {
        if (oreg == EAX) break;  // NOP (also PAUSE with F3)
        uint64_t t = getReg(EAX, osz);
        setReg(EAX, getReg(oreg, osz), osz);
        setReg(oreg, t, osz);
        break;
    }
    case 0x9B:  // WAIT
        break;
    case 0x98: {  // CBW / CWDE / CDQE
        int64_t v = sext(getReg(EAX, osz / 2), osz / 2);
        setReg(EAX, uint64_t(v), osz);
        break;
    }
    case 0x99: {  // CWD / CDQ / CQO
        bool neg = sext(getReg(EAX, osz), osz) < 0;
        setReg(EDX, neg ? maskFor(osz) : 0, osz);
        break;
    }
    case 0x9A: {  // CALL ptr16:16 / ptr16:32 (not valid in 64-bit code)
        uint32_t off = osz == 2 ? fetch16() : fetch32();
        uint16_t sel = fetch16();
        push(seg[CS].sel, osz);
        push(eip, osz);
        loadSeg(CS, sel);
        eip = off;
        break;
    }
    case 0x9C:  // PUSHF
        push(eflags, ssz);
        break;
    case 0x9D: {  // POPF
        uint64_t v = pop(ssz);
        if (ssz == 2) eflags = (eflags & 0xFFFF0000u) | (uint32_t(v) & 0xFFFF);
        else eflags = uint32_t(v);
        eflags |= 0x2;
        break;
    }
    case 0x9E: {  // SAHF
        uint32_t ah = uint32_t(r[EAX] >> 8) & 0xFF;
        eflags = (eflags & ~0xD5u) | (ah & 0xD5u) | 0x2;
        break;
    }
    case 0x9F:  // LAHF
        r[EAX] = (r[EAX] & ~0xFF00ull) | (uint64_t((eflags & 0xD5u) | 0x2) << 8);
        break;

    case 0xA0: case 0xA1: {  // MOV AL/eAX, moffs
        uint64_t off = asz == 8 ? fetch64() : asz == 4 ? fetch32() : fetch16();
        int sz = op == 0xA0 ? BYTE : osz;
        setReg(EAX, memRead(segOv >= 0 ? segOv : DS, off, sz), sz);
        break;
    }
    case 0xA2: case 0xA3: {  // MOV moffs, AL/eAX
        uint64_t off = asz == 8 ? fetch64() : asz == 4 ? fetch32() : fetch16();
        int sz = op == 0xA2 ? BYTE : osz;
        memWrite(segOv >= 0 ? segOv : DS, off, getReg(EAX, sz), sz);
        break;
    }
    case 0xA4: case 0xA5: case 0xA6: case 0xA7:
    case 0xAA: case 0xAB: case 0xAC: case 0xAD:
    case 0xAE: case 0xAF:
        stringOp(op, segOv, asz, osz, rep);
        break;
    case 0xA8: {  // TEST AL, imm8
        uint64_t res = getReg(EAX, BYTE) & fetch8();
        setFlag(F_CF, false);
        setFlag(F_OF, false);
        setSZP(res, BYTE);
        break;
    }
    case 0xA9: {  // TEST eAX, imm
        uint64_t imm = fetchImm(osz);
        uint64_t res = getReg(EAX, osz) & imm;
        setFlag(F_CF, false);
        setFlag(F_OF, false);
        setSZP(res, osz);
        break;
    }

    case 0xB0: case 0xB1: case 0xB2: case 0xB3:
    case 0xB4: case 0xB5: case 0xB6: case 0xB7:
        setReg(oreg, fetch8(), BYTE);
        break;
    case 0xB8: case 0xB9: case 0xBA: case 0xBB:
    case 0xBC: case 0xBD: case 0xBE: case 0xBF:
        if (osz == QWORD) setReg(oreg, fetch64(), QWORD);  // MOV r64, imm64
        else setReg(oreg, osz == WORD ? fetch16() : fetch32(), osz);
        break;

    case 0xC0:
    case 0xC1:
    case 0xD0:
    case 0xD1:
    case 0xD2:
    case 0xD3: {  // group 2: shifts and rotates
        int sz = (op & 1) ? osz : BYTE;
        Modrm m = decodeModrm(asz, segOv);
        int count;
        if (op <= 0xC1) count = fetch8();
        else if (op <= 0xD1) count = 1;
        else count = int(getReg(ECX, BYTE));
        uint64_t res = shift(m.reg, readRM(m, sz), count, sz);
        writeRM(m, res, sz);
        break;
    }
    case 0xC2: {  // RET imm16
        uint16_t n = fetch16();
        eip = uint32_t(pop(ssz));
        r[ESP] = lm ? r[ESP] + n : uint32_t(r[ESP] + n);
        break;
    }
    case 0xC3:  // RET
        eip = uint32_t(pop(ssz));
        break;
    case 0xC4:
    case 0xC5: {  // LES / LDS (not valid in 64-bit code)
        Modrm m = decodeModrm(asz, segOv);
        const uint64_t ea = effAddr(m);
        uint64_t off = memRead(m.seg, ea, osz);
        uint16_t sel = uint16_t(memRead(m.seg, ea + osz, WORD));
        loadSeg(op == 0xC4 ? ES : DS, sel);
        setReg(m.regx, off, osz);
        break;
    }
    case 0xC6:
    case 0xC7: {  // MOV r/m, imm
        int sz = op == 0xC6 ? BYTE : osz;
        Modrm m = decodeModrm(asz, segOv);
        uint64_t imm = fetchImm(sz);
        writeRM(m, imm, sz);
        break;
    }
    case 0xC8: {  // ENTER size, level (level 0 only)
        uint16_t size = fetch16();
        uint8_t level = fetch8();
        if (level != 0) { stop("ENTER with nesting level is not implemented"); break; }
        push(r[EBP], ssz);
        uint64_t frame = r[ESP];
        r[EBP] = frame;
        r[ESP] = lm ? frame - size : uint32_t(frame - size);
        break;
    }
    case 0xC9:  // LEAVE
        r[ESP] = r[EBP];
        setReg(EBP, pop(ssz), ssz);
        break;
    case 0xCA: {  // RETF imm16
        uint16_t n = fetch16();
        eip = uint32_t(pop(osz));
        loadSeg(CS, uint16_t(pop(osz)));
        r[ESP] += n;
        break;
    }
    case 0xCB: {  // RETF
        eip = uint32_t(pop(osz));
        loadSeg(CS, uint16_t(pop(osz)));
        break;
    }
    case 0xCC:  // INT3
        raise(3);
        break;
    case 0xCD:  // INT n
        raise(fetch8());
        break;
    case 0xCE:  // INTO
        if (flag(F_OF)) raise(4);
        break;
    case 0xCF: {  // IRET, or IRETQ in long mode
        if (longActive()) {  // RIP, CS, RFLAGS, RSP, SS
            eip = uint32_t(pop(QWORD));
            loadSeg(CS, uint16_t(pop(QWORD)));
            eflags = uint32_t(pop(QWORD)) | 0x2;
            const uint64_t newRsp = pop(QWORD);
            const uint16_t newSs = uint16_t(pop(QWORD));
            r[ESP] = newRsp;
            loadSeg(SS, newSs);
            break;
        }
        eip = uint32_t(pop(osz));
        loadSeg(CS, uint16_t(pop(osz)));
        uint32_t f = uint32_t(pop(osz));
        eflags = (osz == 2) ? ((eflags & 0xFFFF0000u) | (f & 0xFFFF)) : f;
        eflags |= 0x2;
        break;
    }

    case 0xD7: {  // XLAT
        uint64_t off = r[EBX] + getReg(EAX, BYTE);
        off = asz == 8 ? off : asz == 4 ? (off & 0xFFFFFFFFull) : (off & 0xFFFF);
        setReg(EAX, memRead(segOv >= 0 ? segOv : DS, off, BYTE), BYTE);
        break;
    }

    case 0xD8: case 0xD9: case 0xDA: case 0xDB:
    case 0xDC: case 0xDD: case 0xDE: case 0xDF:
        if (!x87(op, asz, segOv)) stop("this floating point instruction is not implemented yet");
        break;

    case 0xE0:    // LOOPNE
    case 0xE1:    // LOOPE
    case 0xE2: {  // LOOP
        int32_t rel = int32_t(int8_t(fetch8()));
        const uint64_t cmask = asz == 8 ? ~0ull : asz == 4 ? 0xFFFFFFFFull : 0xFFFFull;
        uint64_t c = ((r[ECX] & cmask) - 1) & cmask;
        setAddr(r[ECX], c, asz);
        bool take = c != 0;
        if (op == 0xE0) take = take && !flag(F_ZF);
        if (op == 0xE1) take = take && flag(F_ZF);
        if (take) jumpRel(rel);
        break;
    }
    case 0xE3: {  // JCXZ / JECXZ / JRCXZ
        int32_t rel = int32_t(int8_t(fetch8()));
        const uint64_t cmask = asz == 8 ? ~0ull : asz == 4 ? 0xFFFFFFFFull : 0xFFFFull;
        if ((r[ECX] & cmask) == 0) jumpRel(rel);
        break;
    }
    case 0xE4:    // IN AL, imm8
    case 0xE5: {  // IN eAX, imm8
        uint16_t port = fetch8();
        int sz = op == 0xE4 ? BYTE : (osz == 2 ? WORD : DWORD);
        uint64_t v = bus.ioIn ? bus.ioIn(port, sz) : maskFor(sz);
        setReg(EAX, v, sz);
        break;
    }
    case 0xE6:    // OUT imm8, AL
    case 0xE7: {  // OUT imm8, eAX
        uint16_t port = fetch8();
        int sz = op == 0xE6 ? BYTE : (osz == 2 ? WORD : DWORD);
        if (bus.ioOut) bus.ioOut(port, uint32_t(getReg(EAX, sz)), sz);
        break;
    }
    case 0xEC:    // IN AL, DX
    case 0xED: {  // IN eAX, DX
        uint16_t port = uint16_t(r[EDX]);
        int sz = op == 0xEC ? BYTE : (osz == 2 ? WORD : DWORD);
        uint64_t v = bus.ioIn ? bus.ioIn(port, sz) : maskFor(sz);
        setReg(EAX, v, sz);
        break;
    }
    case 0xEE:    // OUT DX, AL
    case 0xEF: {  // OUT DX, eAX
        uint16_t port = uint16_t(r[EDX]);
        int sz = op == 0xEE ? BYTE : (osz == 2 ? WORD : DWORD);
        if (bus.ioOut) bus.ioOut(port, uint32_t(getReg(EAX, sz)), sz);
        break;
    }
    case 0xE8: {  // CALL rel
        int32_t rel = osz == 2 ? int32_t(int16_t(fetch16())) : int32_t(fetch32());
        push(eip, ssz);
        jumpRel(rel);
        break;
    }
    case 0xE9: {  // JMP rel
        int32_t rel = osz == 2 ? int32_t(int16_t(fetch16())) : int32_t(fetch32());
        jumpRel(rel);
        break;
    }
    case 0xEA: {  // JMP ptr16:16 / ptr16:32 (not valid in 64-bit code)
        uint32_t off = osz == 2 ? fetch16() : fetch32();
        uint16_t sel = fetch16();
        loadSeg(CS, sel);
        eip = off;
        break;
    }
    case 0xEB: {  // JMP rel8
        int32_t rel = int32_t(int8_t(fetch8()));
        jumpRel(rel);
        break;
    }

    case 0xF4:  // HLT
        halted = true;
        break;
    case 0xF5:  // CMC
        setFlag(F_CF, !flag(F_CF));
        break;
    case 0xF6:
    case 0xF7: {  // group 3
        int sz = op == 0xF6 ? BYTE : osz;
        Modrm m = decodeModrm(asz, segOv);
        switch (m.reg) {
        case 0:
        case 1: {  // TEST r/m, imm
            uint64_t imm = fetchImm(sz);
            uint64_t res = readRM(m, sz) & imm;
            setFlag(F_CF, false);
            setFlag(F_OF, false);
            setSZP(res, sz);
            break;
        }
        case 2:  // NOT
            writeRM(m, ~readRM(m, sz) & maskFor(sz), sz);
            break;
        case 3: {  // NEG
            uint64_t v = readRM(m, sz) & maskFor(sz);
            uint64_t res = (0 - v) & maskFor(sz);
            setFlag(F_CF, v != 0);
            setFlag(F_OF, v == signFor(sz));
            setFlag(F_AF, ((v ^ res) & 0x10) != 0);
            setSZP(res, sz);
            writeRM(m, res, sz);
            break;
        }
        default:  // MUL, IMUL, DIV, IDIV
            muldiv(m.reg, readRM(m, sz), sz);
            break;
        }
        break;
    }
    case 0xF8: setFlag(F_CF, false); break;  // CLC
    case 0xF9: setFlag(F_CF, true); break;   // STC
    case 0xFA: setFlag(F_IF, false); break;  // CLI
    case 0xFB: setFlag(F_IF, true); break;   // STI
    case 0xFC: setFlag(F_DF, false); break;  // CLD
    case 0xFD: setFlag(F_DF, true); break;   // STD

    case 0xFE: {  // group 4: INC/DEC r/m8
        Modrm m = decodeModrm(asz, segOv);
        if (m.reg > 1) { stop("bad group 4 instruction"); break; }
        writeRM(m, incdec(readRM(m, BYTE), BYTE, m.reg == 1), BYTE);
        break;
    }
    case 0xFF: {  // group 5
        Modrm m = decodeModrm(asz, segOv);
        switch (m.reg) {
        case 0:
        case 1:  // INC/DEC r/m
            writeRM(m, incdec(readRM(m, osz), osz, m.reg == 1), osz);
            break;
        case 2: {  // CALL near r/m
            uint64_t target = readRM(m, lm ? QWORD : osz);
            push(eip, ssz);
            eip = uint32_t(target);
            break;
        }
        case 3: {  // CALL far m16:16 / m16:32
            const uint64_t ea = effAddr(m);
            uint64_t off = memRead(m.seg, ea, osz);
            uint16_t sel = uint16_t(memRead(m.seg, ea + osz, WORD));
            push(seg[CS].sel, osz);
            push(eip, osz);
            loadSeg(CS, sel);
            eip = uint32_t(off);
            break;
        }
        case 4:  // JMP near r/m
            eip = uint32_t(readRM(m, lm ? QWORD : osz));
            break;
        case 5: {  // JMP far m16:16 / m16:32
            const uint64_t ea = effAddr(m);
            uint64_t off = memRead(m.seg, ea, osz);
            uint16_t sel = uint16_t(memRead(m.seg, ea + osz, WORD));
            loadSeg(CS, sel);
            eip = uint32_t(off);
            break;
        }
        case 6:  // PUSH r/m
            push(readRM(m, ssz), ssz);
            break;
        default:
            stop("bad group 5 instruction");
            break;
        }
        break;
    }

    default: {
        char what[64];
        std::snprintf(what, sizeof what, "unsupported opcode %02X", op);
        stop(what);
        break;
    }
    }
    rex = 0;
}
