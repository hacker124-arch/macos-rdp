// Interrupt controller and timer for the software machine.
#pragma once
#include <cstdint>
#include <functional>
#include <utility>

// Intel 8259A interrupt controller, two chips in cascade. Master on ports 0x20/0x21, slave on
// 0xA0/0xA1 with its output on master IRQ2. Fixed priority: IRQ0 highest, IRQ7 lowest.
class Pic {
public:
    void reset();
    void raise(int irq);  // a device asserts IRQ 0-15; the request stays latched until acknowledged
    int acknowledge();    // the CPU takes the highest-priority request: returns its vector, or -1
    bool ioOut(uint16_t port, uint8_t value);  // false when the port is not a PIC port
    bool ioIn(uint16_t port, uint8_t& value);

private:
    struct Chip {
        uint8_t irr = 0;     // requests pending
        uint8_t isr = 0;     // requests being serviced
        uint8_t imr = 0xFF;  // mask bits
        uint8_t base = 0;    // vector of IRQ0
        int init = 0;        // initialization: 0 when finished, otherwise the next ICW number (2..4)
        bool needIcw4 = false;
        bool readIsr = false;
    };
    Chip chips[2];  // [0] master, [1] slave

    int pick(const Chip& c) const;
    void endOfInterrupt(int chip, int irq);  // irq -1 means the highest-priority one in service
};

// Intel 8253 interval timer, channel 0 only (drives IRQ0). Port 0x40 is the data port, 0x43 the mode
// command. Modes 0, 2 and 3 are supported. The counter moves once per call to tick(); the machine
// calls it once per emulated instruction, so time is counted in instructions, not seconds.
class Pit {
public:
    explicit Pit(std::function<void(int irq)> irqLine);
    void reset();
    void tick();
    bool ioOut(uint16_t port, uint8_t value);
    bool ioIn(uint16_t port, uint8_t& value);

private:
    void setDivisor(uint16_t divisor);
    std::function<void(int)> irqLine;
    uint16_t reload = 0;  // programmed divisor (0 means 65536)
    uint32_t count = 0;
    bool running = false;
    uint8_t mode = 0;
    uint8_t access = 3;  // 1 low byte only, 2 high byte only, 3 low byte then high byte
    uint8_t writeStep = 0;
    uint8_t loByte = 0;
    bool latched = false;
    uint16_t latchValue = 0;
    uint8_t readStep = 0;
};
