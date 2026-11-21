#include "devices.h"

// ----------------------------------------------------------------------------- 8259A

void Pic::reset() {
    chips[0] = Chip{};
    chips[1] = Chip{};
}

void Pic::raise(int irq) {
    if (irq < 0 || irq > 15) return;
    if (irq < 8) {
        chips[0].irr |= uint8_t(1u << irq);
    } else {
        chips[1].irr |= uint8_t(1u << (irq - 8));
        chips[0].irr |= 0x04;  // the cascade line
    }
}

// Highest-priority request on this chip that may be serviced now, or -1.
int Pic::pick(const Chip& c) const {
    const uint8_t pending = uint8_t(c.irr & ~c.imr);
    for (int i = 0; i < 8; ++i) {
        const uint8_t bit = uint8_t(1u << i);
        if (c.isr & bit) return -1;  // an equal or higher priority interrupt is in service
        if (pending & bit) return i;
    }
    return -1;
}

int Pic::acknowledge() {
    const int irq = pick(chips[0]);
    if (irq < 0) return -1;
    if (irq == 2) {  // cascade: the slave supplies the vector
        const int s = pick(chips[1]);
        if (s < 0) {  // the slave's request went away or is masked
            chips[0].irr &= uint8_t(~0x04u);
            return acknowledge();
        }
        chips[1].irr &= uint8_t(~(1u << s));
        chips[1].isr |= uint8_t(1u << s);
        chips[0].isr |= 0x04;
        return chips[1].base + s;
    }
    chips[0].irr &= uint8_t(~(1u << irq));
    chips[0].isr |= uint8_t(1u << irq);
    return chips[0].base + irq;
}

void Pic::endOfInterrupt(int chip, int irq) {
    Chip& c = chips[chip];
    if (irq < 0) {
        for (int i = 0; i < 8; ++i) {
            if (c.isr & (1u << i)) {
                irq = i;
                break;
            }
        }
        if (irq < 0) return;
    }
    c.isr &= uint8_t(~(1u << irq));
    if (chip == 1 && c.isr == 0) chips[0].isr &= uint8_t(~0x04u);  // slave finished: release the cascade
}

bool Pic::ioOut(uint16_t port, uint8_t v) {
    int chip;
    if (port == 0x20 || port == 0x21) chip = 0;
    else if (port == 0xA0 || port == 0xA1) chip = 1;
    else return false;

    Chip& c = chips[chip];
    if ((port & 1) == 0) {  // command port
        if (v & 0x10) {     // ICW1 starts initialization; the mask is cleared
            c = Chip{};
            c.imr = 0;
            c.needIcw4 = (v & 1) != 0;
            c.init = 2;
        } else if ((v & 0x18) == 0x08) {  // OCW3: choose what a read returns
            if (v & 0x02) c.readIsr = (v & 0x01) != 0;
        } else {  // OCW2: end of interrupt
            switch (v >> 5) {
            case 1: endOfInterrupt(chip, -1); break;     // non-specific
            case 3: endOfInterrupt(chip, v & 7); break;  // specific
            default: break;
            }
        }
    } else {  // data port: initialization words, or the mask
        if (c.init) {
            if (c.init == 2) {
                c.base = v & 0xF8;
                c.init = 3;
            } else if (c.init == 3) {
                c.init = c.needIcw4 ? 4 : 0;  // ICW3 (cascade wiring) is accepted and ignored
            } else {
                c.init = 0;  // ICW4: 8086 mode is the only one used
            }
        } else {
            c.imr = v;
        }
    }
    return true;
}

bool Pic::ioIn(uint16_t port, uint8_t& v) {
    int chip;
    if (port == 0x20 || port == 0x21) chip = 0;
    else if (port == 0xA0 || port == 0xA1) chip = 1;
    else return false;
    const Chip& c = chips[chip];
    if ((port & 1) == 0) v = c.readIsr ? c.isr : c.irr;
    else v = c.imr;
    return true;
}

// ----------------------------------------------------------------------------- 8253

Pit::Pit(std::function<void(int)> line) : irqLine(std::move(line)) { reset(); }

void Pit::reset() {
    reload = 0;
    count = 0;
    running = false;
    mode = 0;
    access = 3;
    writeStep = 0;
    loByte = 0;
    latched = false;
    latchValue = 0;
    readStep = 0;
}

void Pit::setDivisor(uint16_t divisor) {
    reload = divisor;
    count = divisor ? divisor : 0x10000;
    running = true;
}

void Pit::tick() {
    if (!running) return;
    if (--count != 0) return;
    irqLine(0);
    if (mode == 2 || mode == 3) count = reload ? reload : 0x10000;  // periodic
    else running = false;                                           // mode 0: once
}

bool Pit::ioOut(uint16_t port, uint8_t v) {
    if (port == 0x43) {
        if (((v >> 6) & 3) != 0) return true;  // channels 1 and 2 are not modelled
        const int rw = (v >> 4) & 3;
        if (rw == 0) {  // counter latch
            latched = true;
            latchValue = uint16_t(count);
            readStep = 0;
        } else {
            access = uint8_t(rw);
            mode = (v >> 1) & 7;
            writeStep = 0;
            running = false;
        }
        return true;
    }
    if (port == 0x40) {
        if (access == 1) {
            setDivisor(v);
        } else if (access == 2) {
            setDivisor(uint16_t(v) << 8);
        } else if (writeStep == 0) {
            loByte = v;
            writeStep = 1;
        } else {
            writeStep = 0;
            setDivisor(uint16_t(loByte | (uint16_t(v) << 8)));
        }
        return true;
    }
    return false;
}

bool Pit::ioIn(uint16_t port, uint8_t& v) {
    if (port != 0x40) return false;
    const uint16_t val = latched ? latchValue : uint16_t(count);
    const bool hi = (access == 2) || (access == 3 && readStep == 1);
    v = uint8_t(hi ? (val >> 8) : (val & 0xFF));
    if (access == 3) readStep ^= 1;
    if (latched && (access != 3 || readStep == 0)) latched = false;
    return true;
}
