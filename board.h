// Board devices that firmware touches during boot: COM1 (status and transmit register), the CMOS
// clock and memory counters, the keyboard controller status, port 0x92, and PCI configuration
// space with an empty bus. The caller routes every other port.
#pragma once
#include <cstdint>
#include <functional>
#include "fwcfg.h"

class Board {
public:
    explicit Board(uint32_t ramMB);
    // Returns false when the port is not one of the board's.
    bool in(uint16_t port, int size, uint32_t& value);
    bool out(uint16_t port, uint32_t value);

    std::function<void(uint8_t)> serialOut;  // bytes written to COM1's transmit register
    std::function<uint32_t()> clock;         // the time the ACPI PM timer shows (one count per instruction)

private:
    uint8_t cmos[128] = {};
    uint8_t cmosIndex = 0;
    uint32_t pciAddress = 0;
    bool a20 = false;
    FwCfg fw;
};
