/* ==========================================================================
 *  GT-DOS ATA PIO 驱动 (Primary 通道: Master + Slave 双设备)
 *  ---------------------------------------------------------------------------
 *  28 位 LBA 模式. QEMU 映射: -hda => dev0(master), -hdb => dev1(slave).
 *  对外保留 dev0 的兼容接口 (ata_read/ata_write/...), 多盘场景用 *_dev。
 * ========================================================================== */
#include "gt.h"

#define ATA_IO      0x1F0
#define ATA_CTRL    0x3F6

#define ATA_DATA    (ATA_IO + 0)     /* 16 位数据寄存器 */
#define ATA_FEATURE (ATA_IO + 1)
#define ATA_SECCNT  (ATA_IO + 2)
#define ATA_LBA_LO  (ATA_IO + 3)
#define ATA_LBA_MID (ATA_IO + 4)
#define ATA_LBA_HI  (ATA_IO + 5)
#define ATA_DRIVE   (ATA_IO + 6)
#define ATA_STATUS  (ATA_IO + 7)
#define ATA_COMMAND (ATA_IO + 7)

#define CMD_READ     0x20
#define CMD_WRITE    0x30
#define CMD_IDENTIFY 0xEC

#define ST_ERR 0x01
#define ST_DRQ 0x08
#define ST_BSY 0x80

#define ATA_NDEV 2

struct ata_dev {
    bool ok;
    u8   drive_sel;                  /* 0xA0 master / 0xB0 slave */
    u32  sectors;
    char model[41];
};

static struct ata_dev devs[ATA_NDEV];
static int cur_dev;                  /* 兼容接口的默认设备 */

static u8 status(void) { return inb(ATA_STATUS); }

static bool wait_bsy_clear(void)
{
    for (int i = 0; i < 200000; i++)
        if (!(status() & ST_BSY))
            return true;
    return false;
}

static bool wait_drq(void)
{
    for (int i = 0; i < 200000; i++) {
        u8 s = status();
        if (s & ST_ERR)
            return false;
        if ((s & (ST_BSY | ST_DRQ)) == ST_DRQ)
            return true;
    }
    return false;
}

static void select_drive(const struct ata_dev *d)
{
    outb(ATA_DRIVE, d->drive_sel);
    io_wait();
    io_wait();
}

/* 探测单个设备; 成功返回 true 并填好 sectors/model */
static bool probe(struct ata_dev *d)
{
    d->ok = false;
    d->sectors = 0;
    d->model[0] = 0;

    select_drive(d);
    if (!wait_bsy_clear())
        return false;

    outb(ATA_SECCNT, 0);
    outb(ATA_LBA_LO, 0);
    outb(ATA_LBA_MID, 0);
    outb(ATA_LBA_HI, 0);
    outb(ATA_COMMAND, CMD_IDENTIFY);
    io_wait();

    u8 s = status();
    if (s == 0 || s == 0xFF)
        return false;               /* 无设备 */
    if (!wait_bsy_clear())
        return false;
    if (!wait_drq())
        return false;

    u16 ident[256];
    for (int i = 0; i < 256; i++)
        ident[i] = inw(ATA_DATA);

    d->sectors = (u32)ident[60] | ((u32)ident[61] << 16);

    for (int i = 0; i < 40; i++) {
        u16 w = ident[27 + i / 2];
        char c = (i & 1) ? (char)(w & 0xFF) : (char)(w >> 8);
        d->model[i] = c;
    }
    d->model[40] = 0;
    for (int i = 39; i >= 0 && d->model[i] == ' '; i--)
        d->model[i] = 0;

    if (d->sectors > 0)
        d->ok = true;
    return d->ok;
}

bool ata_init(void)
{
    outb(ATA_CTRL, 0x04);           /* 软复位 */
    io_wait();
    outb(ATA_CTRL, 0x00);
    io_wait();

    devs[0].drive_sel = 0xA0;       /* master */
    devs[1].drive_sel = 0xB0;       /* slave  */
    cur_dev = 0;

    bool any = false;
    for (int i = 0; i < ATA_NDEV; i++)
        if (probe(&devs[i]))
            any = true;
    return any;
}

int ata_ndev(void)
{
    int n = 0;
    for (int i = 0; i < ATA_NDEV; i++)
        if (devs[i].ok)
            n++;
    return n;
}

bool ata_present_dev(int dev)
{
    return dev >= 0 && dev < ATA_NDEV && devs[dev].ok;
}

u32 ata_sectors_dev(int dev)
{
    return (dev >= 0 && dev < ATA_NDEV) ? devs[dev].sectors : 0;
}

const char *ata_model_dev(int dev)
{
    static const char none[] = "(none)";
    return (dev >= 0 && dev < ATA_NDEV) ? devs[dev].model : none;
}

static bool transfer(struct ata_dev *d, u32 lba, u32 count, void *buf,
                     bool write)
{
    if (!d->ok)
        return false;
    if (count == 0)
        return true;

    if (!wait_bsy_clear())
        return false;

    select_drive(d);
    outb(ATA_FEATURE, 0x00);
    outb(ATA_SECCNT, (u8)count);
    outb(ATA_LBA_LO, (u8)(lba & 0xFF));
    outb(ATA_LBA_MID, (u8)((lba >> 8) & 0xFF));
    outb(ATA_LBA_HI, (u8)((lba >> 16) & 0xFF));
    outb(ATA_DRIVE, (u8)(0xE0 | ((lba >> 24) & 0x0F) |
                         (d->drive_sel == 0xB0 ? 0x10 : 0x00)));
    outb(ATA_COMMAND, write ? CMD_WRITE : CMD_READ);
    io_wait();

    u16 *dst = (u16 *)buf;
    for (u32 blk = 0; blk < count; blk++) {
        if (!wait_drq())
            return false;
        if (write) {
            for (int i = 0; i < 256; i++)
                outw(ATA_DATA, *dst++);
            io_wait();
            if (!wait_bsy_clear())
                return false;
        } else {
            for (int i = 0; i < 256; i++)
                *dst++ = inw(ATA_DATA);
            io_wait();
        }
        if (status() & ST_ERR)
            return false;
    }
    return true;
}

bool ata_read_dev(int dev, u32 lba, u32 count, void *buf)
{
    if (dev < 0 || dev >= ATA_NDEV)
        return false;
    return transfer(&devs[dev], lba, count, buf, false);
}

bool ata_write_dev(int dev, u32 lba, u32 count, const void *buf)
{
    if (dev < 0 || dev >= ATA_NDEV)
        return false;
    return transfer(&devs[dev], lba, count, (void *)buf, true);
}

/* -------------------------------------------------- 兼容接口 (默认 dev0) */
bool ata_present(void) { return devs[0].ok; }
u32  ata_sectors(void) { return devs[0].sectors; }
const char *ata_model_string(void) { return devs[0].model; }
bool ata_read(u32 lba, u32 count, void *buf)
{
    return transfer(&devs[cur_dev], lba, count, buf, false);
}
bool ata_write(u32 lba, u32 count, const void *buf)
{
    return transfer(&devs[cur_dev], lba, count, (void *)buf, true);
}