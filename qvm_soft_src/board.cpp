#include "board.h"

#include <ctime>

namespace {
uint8_t bcd(int v) { return uint8_t(((v / 10) << 4) | (v % 10)); }
}  // namespace

Board::Board(uint32_t ramMB) {
    const std::time_t now = std::time(nullptr);
    if (const std::tm* t = std::localtime(&now)) {
        const int year = t->tm_year + 1900;
        cmos[0x00] = bcd(t->tm_sec);
        cmos[0x02] = bcd(t->tm_min);
        cmos[0x04] = bcd(t->tm_hour);
        cmos[0x06] = bcd(t->tm_wday + 1);  // 1 = Sunday
        cmos[0x07] = bcd(t->tm_mday);
        cmos[0x08] = bcd(t->tm_mon + 1);
        cmos[0x09] = bcd(year % 100);
        cmos[0x32] = bcd(year / 100);
    }
    cmos[0x0A] = 0x26;  // clock running, not updating
    cmos[0x0B] = 0x02;  // BCD values, 24-hour clock
    cmos[0x0D] = 0x80;  // battery good
    cmos[0x15] = 0x80;  // base memory: 640 KB
    cmos[0x16] = 0x02;
    const uint32_t extKb = ramMB > 1 ? (ramMB - 1) * 1024 : 0;  // memory above 1 MB, in KB
    const uint32_t ext = extKb > 0xFFFF ? 0xFFFF : extKb;
    cmos[0x17] = cmos[0x30] = uint8_t(ext);
    cmos[0x18] = cmos[0x31] = uint8_t(ext >> 8);
    const uint32_t above16 = ramMB > 16 ? (ramMB - 16) * 16 : 0;  // memory above 16 MB, in 64 KB units
    cmos[0x34] = uint8_t(above16);
    cmos[0x35] = uint8_t(above16 >> 8);
}

bool Board::in(uint16_t port, int size, uint32_t& v) {
    switch (port) {
    case 0x3F8: case 0x3F9: case 0x3FA: case 0x3FB:
    case 0x3FC: case 0x3FD: case 0x3FE: case 0x3FF:  // COM1
        if (port == 0x3FD) v = 0x60;        // line status: transmit holding and shift registers empty
        else if (port == 0x3FA) v = 0x01;   // interrupt id: none pending
        else if (port == 0x3FE) v = 0xB0;   // modem status: CTS, DSR, DCD
        else v = 0;
        return true;
    case 0x608: case 0x609: case 0x60A: case 0x60B: {  // ACPI PM timer (PMBASE 0x600 + 8, as QEMU lays it out)
        const uint32_t t = clock ? clock() : 0;
        const int sh = 8 * (port - 0x608);
        v = t >> sh;
        if (size == 1) v &= 0xFF;
        else if (size == 2) v &= 0xFFFF;
        return true;
    }
    case 0x70:
        v = cmosIndex;
        return true;
    case 0x71:
        v = cmos[cmosIndex & 0x7F];
        return true;
    case 0x64:  // keyboard controller status: output empty, input empty, unlocked
        v = 0x10;
        return true;
    case 0x60:
        v = 0;
        return true;
    case 0x92:
        v = a20 ? 0x02 : 0;
        return true;
    case 0xCF8:
        v = pciAddress;
        return true;
    case 0xCFC: case 0xCFD: case 0xCFE: case 0xCFF:  // no device on the bus: reads return all ones
        v = size == 1 ? 0xFF : size == 2 ? 0xFFFF : 0xFFFFFFFF;
        return true;
    default:
        return false;
    }
}

bool Board::out(uint16_t port, uint32_t value) {
    switch (port) {
    case 0x3F8:
        if (serialOut) serialOut(uint8_t(value));
        return true;
    case 0x3F9: case 0x3FA: case 0x3FB: case 0x3FC:
    case 0x3FD: case 0x3FE: case 0x3FF:
        return true;  // other COM1 registers are accepted and ignored
    case 0x70:
        cmosIndex = uint8_t(value & 0x7F);
        return true;
    case 0x71:
        cmos[cmosIndex] = uint8_t(value);
        return true;
    case 0x92:
        a20 = (value & 0x02) != 0;
        return true;
    case 0x402:  // debugcon: OVMF debug builds print here
        if (serialOut) serialOut(uint8_t(value));
        return true;
    case 0x80:  // POST code port
    case 0x60: case 0x64:
        return true;
    case 0xCF8:
        pciAddress = value;
        return true;
    case 0xCFC: case 0xCFD: case 0xCFE: case 0xCFF:
        return true;  // writes to an empty bus go nowhere
    default:
        return false;
    }
}
