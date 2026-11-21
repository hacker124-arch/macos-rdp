// Software machine: CPU, interrupt controller, timer and the board devices firmware touches.
//
//   qvm_soft <image.bin> [loadAddr=0x7C00] [cs=0] [ip=0x7C00] [maxSteps=10000000] [ramMB=16]
//       Copies the image to loadAddr and starts at cs:ip in real mode (the test programs).
//   qvm_soft --firmware <firmware.fd> [ramMB=64] [maxSteps=100000000]
//       Maps the firmware so that it ends at 4 GB and starts at the reset vector, FFFF:FFF0.
//
// Ports: 0x20/0x21 and 0xA0/0xA1 interrupt controller, 0x40/0x43 timer, 0x70/0x71 CMOS,
//        0x3F8 COM1 (text output to stdout), 0x64 and 0x92 board, 0xCF8/0xCFC PCI config space,
//        0xE9 text output to stdout, 0x501 exit code (the test programs end with it).
#include "board.h"
#include "cpu.h"
#include "devices.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>



int main(int argc, char** argv) {
    const bool firmware = argc >= 2 && std::strcmp(argv[1], "--firmware") == 0;
    const char* path = firmware ? (argc >= 3 ? argv[2] : nullptr) : (argc >= 2 ? argv[1] : nullptr);
    if (!path) {
        std::fprintf(stderr, "usage: qvm_soft <image.bin> [loadAddr] [cs] [ip] [maxSteps] [ramMB]\n"
                             "       qvm_soft --firmware <firmware.fd> [ramMB] [maxSteps]\n");
        return 2;
    }
    auto arg = [&](int i, unsigned long long def) {
        return i < argc ? std::strtoull(argv[i], nullptr, 0) : def;
    };
    uint32_t load = 0x7C00;
    uint16_t cs = 0;
    uint32_t ip = 0x7C00;
    uint64_t maxSteps = 10000000;
    size_t ramBytes = size_t(16) << 20;
    if (firmware) {
        ramBytes = size_t(arg(3, 64)) << 20;
        maxSteps = arg(4, 100000000);
    } else {
        load = uint32_t(arg(2, 0x7C00));
        cs = uint16_t(arg(3, 0));
        ip = uint32_t(arg(4, 0x7C00));
        maxSteps = arg(5, 10000000);
        ramBytes = size_t(arg(6, 16)) << 20;
    }

    std::ifstream f(path, std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "cannot open %s\n", path);
        return 2;
    }
    std::vector<uint8_t> image((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

    Bus bus(ramBytes);
    if (firmware) {
        if (image.size() > (size_t(1) << 32)) {
            std::fprintf(stderr, "firmware is larger than 4 GB\n");
            return 2;
        }
        bus.romBase = (uint64_t(1) << 32) - image.size();
        bus.rom = image;
        bus.apic.assign(0x1000, 0);
        bus.apic[0x30] = 0x14;  // version 0x00050014 (xAPIC, 6 LVT entries)
        bus.apic[0x31] = 0x00;
        bus.apic[0x32] = 0x05;
    } else {
        if (uint64_t(load) + image.size() > bus.ram.size()) {
            std::fprintf(stderr, "image does not fit in RAM\n");
            return 2;
        }
        std::copy(image.begin(), image.end(), bus.ram.begin() + load);
    }

    Board board(uint32_t(ramBytes >> 20));
    board.serialOut = [](uint8_t b) {
        std::putchar(b);
        std::fflush(stdout);
    };
    Pic pic;
    Pit pit([&](int irq) { pic.raise(irq); });
    int exitCode = -1;

    bus.ioIn = [&](uint16_t port, int size) -> uint32_t {
        uint8_t v8 = 0xFF;
        if (pic.ioIn(port, v8) || pit.ioIn(port, v8)) return v8;
        uint32_t v = 0;
        if (board.in(port, size, v)) return v;
        return size == 1 ? 0xFF : size == 2 ? 0xFFFF : 0xFFFFFFFF;
    };
    bus.ioOut = [&](uint16_t port, uint32_t value, int size) {
        const uint8_t b = uint8_t(value);
        if (pic.ioOut(port, b) || pit.ioOut(port, b)) return;
        if (board.out(port, value)) return;
        (void)size;
        if (port == 0xE9) {
            std::putchar(b);
            std::fflush(stdout);
        } else if (port == 0x501) {
            exitCode = b;
        }
    };

    Cpu cpu(bus);
    cpu.pollInterrupt = [&]() { return pic.acknowledge(); };
    board.clock = [&]() { return uint32_t(cpu.cycles); };
    if (!firmware) {  // the test programs start in real mode with flat-ish segments
        cpu.cr0 = 0;
        cpu.seg[CS].sel = cs;
        cpu.seg[CS].base = uint32_t(cs) << 4;
        cpu.seg[CS].limit = 0xFFFF;
        cpu.seg[CS].big = false;
        for (int s : {ES, SS, DS, FS, GS}) {
            cpu.seg[s].sel = 0;
            cpu.seg[s].base = 0;
            cpu.seg[s].limit = 0xFFFF;
            cpu.seg[s].big = false;
        }
        cpu.r[ESP] = 0xFFFE;
        cpu.eip = ip;
        cpu.idtBase = 0;
        cpu.idtLimit = 0x3FF;
    }

    uint64_t n = 0;
    while (n < maxSteps && exitCode < 0) {
        pit.tick();
        if (!cpu.step()) break;
        ++n;
        if (cpu.halted && !(cpu.eflags & F_IF)) break;  // halted with interrupts off: nothing can wake it
    }

    if (firmware) std::fprintf(stderr, "\n[unmapped memory accesses: %llu]", (unsigned long long)bus.badAccesses);
    if (cpu.stopped) {
        std::fprintf(stderr, "\n[stopped] %s after %llu steps\n", cpu.stopInfo, (unsigned long long)n);
        return 3;
    }
    if (exitCode >= 0) {
        std::fprintf(stderr, "\n[exit %d] after %llu steps\n", exitCode, (unsigned long long)n);
        return exitCode;
    }
    if (cpu.halted) {
        std::fprintf(stderr, "\n[halted] at %04X:%08X after %llu steps\n", unsigned(cpu.seg[CS].sel),
                     unsigned(cpu.eip), (unsigned long long)n);
        return 4;
    }
    std::fprintf(stderr, "\n[step limit] after %llu steps at %04X:%08X\n", (unsigned long long)n,
                 unsigned(cpu.seg[CS].sel), unsigned(cpu.eip));
    return 5;
}
