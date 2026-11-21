// A software x86 CPU (no hypervisor needed). Real mode, 32-bit protected mode, 32-bit paging (4 KB and
// 4 MB pages), long mode (EFER.LME with 4-level paging, 4 KB, 2 MB and 1 GB pages), 64-bit code with
// REX prefixes and RIP-relative addressing, the integer instruction set, interrupts, exceptions and halt.
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

enum GpReg { EAX = 0, ECX, EDX, EBX, ESP, EBP, ESI, EDI, R8, R9, R10, R11, R12, R13, R14, R15 };
enum SegReg { ES = 0, CS, SS, DS, FS, GS };
enum : uint32_t {
    F_CF = 1u << 0,
    F_PF = 1u << 2,
    F_AF = 1u << 4,
    F_ZF = 1u << 6,
    F_SF = 1u << 7,
    F_TF = 1u << 8,
    F_IF = 1u << 9,
    F_DF = 1u << 10,
    F_OF = 1u << 11,
};
// IA32_EFER bits
constexpr uint64_t EFER_SCE = 1ull << 0;   // SYSCALL enable (stored only)
constexpr uint64_t EFER_LME = 1ull << 8;   // long mode enable (set by software)
constexpr uint64_t EFER_LMA = 1ull << 10;  // long mode active (set by the CPU when paging turns on)
constexpr uint64_t EFER_NXE = 1ull << 11;  // no-execute enable (stored only)

struct Segment {
    uint16_t sel = 0;
    uint32_t base = 0;
    uint32_t limit = 0xFFFF;
    bool big = false;      // 32-bit code and stack (the D/B bit of the descriptor)
    bool longMode = false; // 64-bit code (the L bit of the descriptor)
};

// Physical memory plus port I/O. Memory beyond the RAM size reads as 0xFF and is counted.
class Bus {
public:
    explicit Bus(size_t bytes) : ram(bytes, 0) {}
    uint64_t read(uint32_t phys, int size);  // size 1, 2, 4 or 8
    void write(uint32_t phys, uint64_t value, int size);

    std::vector<uint8_t> ram;
    std::vector<uint8_t> rom;  // firmware image, read-only: it occupies [romBase, romBase + rom.size())
    uint64_t romBase = 0;
    std::vector<uint8_t> apic;  // local APIC registers at 0xFEE00000, one 4 KB page (empty: not present)
    std::function<uint32_t(uint16_t port, int size)> ioIn;
    std::function<void(uint16_t port, uint32_t value, int size)> ioOut;
    uint64_t badAccesses = 0;
};

class Cpu {
public:
    explicit Cpu(Bus& bus);
    void reset();
    // Runs one instruction, or delivers one interrupt, or passes one cycle while halted.
    // Returns false only after the CPU has stopped on an error (see stopInfo).
    bool step();
    // Queues an interrupt vector. It is delivered when IF is set.
    void raiseInterrupt(int vector);
    // Asked on every step while IF is set: returns the vector of an interrupt to deliver, or -1.
    // The interrupt controller connects here.
    std::function<int()> pollInterrupt;

    Bus& bus;
    uint64_t r[16] = {};  // general registers; 16/32-bit operations keep the upper bits as the hardware does
    uint32_t eip = 0;     // instruction pointer; 32 bits (code below 4 GB)
    uint32_t eflags = 0x2;
    Segment seg[6];
    uint32_t cr0 = 0, cr4 = 0;
    uint64_t cr2 = 0, cr3 = 0;
    uint32_t gdtLimit = 0, idtLimit = 0x3FF;
    uint64_t gdtBase = 0, idtBase = 0;
    uint64_t efer = 0;
    bool halted = false;
    bool stopped = false;
    char stopInfo[200] = {};
    uint64_t cycles = 0;
    uint64_t xmm[16][2] = {};  // SSE registers: low and high 64 bits
    uint32_t mxcsr = 0x1F80;
    uint16_t fpuCw = 0x37F;    // x87 control word (stored only)
    uint16_t fpuSw = 0;        // x87 status word (stored only)

    // True when long mode is active (EFER.LMA).
    bool longActive() const { return (efer & EFER_LMA) != 0; }
    // True when the code now running is 64-bit code.
    bool in64() const { return longActive() && seg[CS].longMode; }

private:
    struct Modrm {
        int mod, reg, rm;  // raw fields: reg is also the opcode extension of group instructions
        int regx, rmx;     // with REX.R and REX.B added: the register operands
        uint64_t ea;       // effective address, or the displacement when ripRel is set
        int seg;
        bool ripRel;       // RIP-relative: the address is ea plus the next instruction address
    };
    // Thrown by the memory access path when a page is not usable. The instruction is restarted
    // after the page fault handler returns.
    struct PageFault {
        uint64_t addr;
        uint32_t err;
    };

    int pendingVector = -1;
    uint32_t instrEip = 0;
    uint16_t instrCs = 0;
    uint8_t rex = 0;  // the REX prefix of the current instruction, or 0

    void execute();
    uint8_t fetch8();
    uint16_t fetch16();
    uint32_t fetch32();
    uint64_t fetch64();
    uint64_t fetchImm(int size);  // an immediate of an operand of this size; 8 means sign-extended 32 bits
    Modrm decodeModrm(int asz, int segOv);  // asz: address size, 2, 4 or 8
    uint64_t effAddr(const Modrm& m) const;

    uint64_t getReg(int idx, int size) const;
    void setReg(int idx, uint64_t v, int size);
    uint64_t linear(int s, uint64_t off) const;
    uint64_t memRead(int s, uint64_t off, int size);
    void memWrite(int s, uint64_t off, uint64_t v, int size);
    uint64_t readRM(const Modrm& m, int size);
    void writeRM(const Modrm& m, uint64_t v, int size);

    uint32_t translate(uint64_t lin, bool write);
    uint32_t translate4(uint64_t lin, bool write);
    uint64_t readLin(uint64_t lin, int size);
    void writeLin(uint64_t lin, uint64_t v, int size);

    bool flag(uint32_t f) const { return (eflags & f) != 0; }
    void setFlag(uint32_t f, bool on);
    void setSZP(uint64_t res, int size);
    uint64_t alu(int op, uint64_t a, uint64_t b, int size);
    uint64_t shift(int op, uint64_t v, int count, int size);
    uint64_t incdec(uint64_t v, int size, bool dec);
    bool condition(int cc) const;

    void push(uint64_t v, int size);
    uint64_t pop(int size);
    void jumpRel(int32_t d);
    void loadSeg(int s, uint16_t sel);
    void deliver(int vector, bool hasError, uint32_t error);
    void deliver64(int vector, bool hasError, uint32_t error);
    void raise(int vector, bool hasError = false, uint32_t error = 0);
    void stop(const char* what);
    void stringOp(uint8_t op, int segOv, int asz, int osz, uint8_t rep);
    bool muldiv(int reg, uint64_t v, int size);
    bool extended0F(uint8_t op, bool p66, uint8_t rep, int asz, int segOv, int osz);
    bool x87(uint8_t op, int asz, int segOv);
    void readX(const Modrm& m, uint64_t out[2]);
    void writeX(const Modrm& m, const uint64_t in[2]);
};
