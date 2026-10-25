// Tests the SPI flash model itself (hw/sim/spi_flash_model.h): it answers
// like the real chip, and it notices the mistakes a controller can make.
#include <cstdio>
#include <vector>
#include "spi_flash_model.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

// Drives the model like a mode-0 controller: 2 system clocks per SCK half.
struct Bus {
    SpiFlashModel &f;
    int miso = 1;
    explicit Bus(SpiFlashModel &m) : f(m) {}
    void idle(int n) { for (int i = 0; i < n; i++) miso = f.step(1, 0, 0); }
    void select() { miso = f.step(0, 0, 0); miso = f.step(0, 0, 0); }
    void deselect() { miso = f.step(0, 0, 0); miso = f.step(1, 0, 0); miso = f.step(1, 0, 0); }
    int byte(int out)
    {
        int in = 0;
        for (int b = 7; b >= 0; b--) {
            int mosi = (out >> b) & 1;
            miso = f.step(0, 0, mosi);
            miso = f.step(0, 0, mosi);
            in = (in << 1) | miso;           // sample as SCK rises
            miso = f.step(0, 1, mosi);
            miso = f.step(0, 1, mosi);
        }
        miso = f.step(0, 0, 0);              // SCK back low: the falling edge
        return in;
    }
    std::vector<int> command(std::vector<int> out, int reply_bytes)
    {
        std::vector<int> in;
        select();
        for (int b : out) byte(b);
        for (int i = 0; i < reply_bytes; i++) in.push_back(byte(0));
        deselect();
        return in;
    }
    void wait_ready()
    {
        for (int i = 0; i < 1000; i++) {
            if (!(command({0x05}, 1)[0] & 1)) return;
            idle(1000);
        }
        printf("FAIL: flash stayed busy\n");
        failures++;
    }
};

static void answers_like_the_chip()
{
    SpiFlashModel f;
    Bus bus(f);
    bus.idle(10);
    std::vector<int> id = bus.command({0x9F}, 3);
    CHECK(id[0] == 0xC2 && id[1] == 0x20 && id[2] == 0x17);
    CHECK(bus.command({0x05}, 1)[0] == 0x00);
    bus.command({0x06}, 0);
    CHECK(bus.command({0x05}, 1)[0] == 0x02);           // write enabled
    bus.command({0x02, 0x01, 0x23, 0x40, 0xA5, 0x5A, 0x0F}, 0);
    CHECK(bus.command({0x05}, 1)[0] & 1);               // busy programming
    bus.wait_ready();
    CHECK(bus.command({0x05}, 1)[0] == 0x00);           // write enable cleared
    std::vector<int> r = bus.command({0x03, 0x01, 0x23, 0x3F}, 5);
    CHECK(r[0] == 0xFF && r[1] == 0xA5 && r[2] == 0x5A && r[3] == 0x0F && r[4] == 0xFF);
    // programming only clears bits
    bus.command({0x06}, 0);
    bus.command({0x02, 0x01, 0x23, 0x40, 0xF0}, 0);
    bus.wait_ready();
    CHECK(bus.command({0x03, 0x01, 0x23, 0x40}, 1)[0] == 0xA0);
    // a sector erase brings it back
    bus.command({0x06}, 0);
    bus.command({0x20, 0x01, 0x2F, 0xFF}, 0);
    bus.wait_ready();
    CHECK(bus.command({0x03, 0x01, 0x23, 0x40}, 1)[0] == 0xFF);
    CHECK(f.failures == 0);
}

static int failures_after(void (*mistake)(Bus &))
{
    SpiFlashModel f;
    Bus bus(f);
    bus.idle(10);
    mistake(bus);
    return f.failures;
}

static void notices_mistakes()
{
    CHECK(failures_after([](Bus &b) { b.command({0x02, 0, 0, 0, 0x12}, 0); }) > 0);   // no write enable
    CHECK(failures_after([](Bus &b) { b.command({0x20, 0, 0, 0}, 0); }) > 0);         // no write enable
    CHECK(failures_after([](Bus &b) {                                                 // crosses a page
        b.command({0x06}, 0);
        b.command({0x02, 0, 0, 0xFF, 1, 2}, 0);
    }) > 0);
    CHECK(failures_after([](Bus &b) {                                                 // command while busy
        b.command({0x06}, 0);
        b.command({0x02, 0, 0, 0, 1}, 0);
        b.command({0x9F}, 3);
    }) > 0);
    CHECK(failures_after([](Bus &b) { b.command({0x77}, 0); }) > 0);                  // unknown command
    CHECK(failures_after([](Bus &b) {                                                 // CS rises mid-byte
        b.select();
        b.f.step(0, 1, 1); b.f.step(0, 1, 1); b.f.step(0, 0, 1); b.f.step(0, 0, 1);
        b.deselect();
    }) > 0);
    CHECK(failures_after([](Bus &b) {                                                 // powered down
        b.command({0xB9}, 0);
        b.command({0x9F}, 3);
    }) > 0);
}

int main()
{
    answers_like_the_chip();
    notices_mistakes();
    printf("spi_flash_model: %s\n", failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}
