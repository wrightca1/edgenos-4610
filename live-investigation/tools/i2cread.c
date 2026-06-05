/* i2cread — read-only CPLD register dump over /dev/i2c-N via SMBus read_byte_data.
 * For verifying ONL's AS4610 CPLD register map against the live box.
 * Usage: i2cread <i2c-bus-dev> <slave-hex> <first-reg-hex> <last-reg-hex>
 *   e.g. i2cread /dev/i2c-0 0x30 0x00 0x3f
 * READ-ONLY: only issues SMBus read_byte_data (no writes to CPLD state).
 */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/ioctl.h>
#include <linux/types.h>

#define I2C_SLAVE        0x0703
#define I2C_SMBUS        0x0720
#define I2C_SMBUS_READ   1
#define I2C_SMBUS_BYTE_DATA 2

union i2c_smbus_data { __u8 byte; __u16 word; __u8 block[34]; };
struct i2c_smbus_ioctl_data {
    __u8 read_write; __u8 command; __u32 size; union i2c_smbus_data *data;
};

static int rd(int fd, __u8 cmd) {
    union i2c_smbus_data d;
    struct i2c_smbus_ioctl_data a = { I2C_SMBUS_READ, cmd, I2C_SMBUS_BYTE_DATA, &d };
    if (ioctl(fd, I2C_SMBUS, &a) < 0) return -1;
    return d.byte;
}

int main(int argc, char **argv) {
    if (argc != 5) { fprintf(stderr, "usage: %s /dev/i2c-N 0xADDR 0xFIRST 0xLAST\n", argv[0]); return 2; }
    const char *dev = argv[1];
    int addr = strtol(argv[2], 0, 16), first = strtol(argv[3], 0, 16), last = strtol(argv[4], 0, 16);
    int fd = open(dev, O_RDWR);
    if (fd < 0) { perror("open"); return 1; }
    if (ioctl(fd, I2C_SLAVE, addr) < 0) { perror("I2C_SLAVE"); return 1; }
    printf("# CPLD %s @ 0x%02x  regs 0x%02x..0x%02x (read-only)\n", dev, addr, first, last);
    for (int r = first; r <= last; r++) {
        int v = rd(fd, (__u8)r);
        if (v < 0) printf("0x%02x: --\n", r);
        else       printf("0x%02x: 0x%02x  %d%d%d%d%d%d%d%d\n", r, v,
                          (v>>7)&1,(v>>6)&1,(v>>5)&1,(v>>4)&1,(v>>3)&1,(v>>2)&1,(v>>1)&1,v&1);
    }
    close(fd);
    return 0;
}
