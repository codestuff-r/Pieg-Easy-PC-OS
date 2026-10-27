#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "ahci.h"
#include "mem.h"
#include "pci.h"

/*
 * Minimal AHCI (SATA) driver.
 *
 *  - polled, no interrupts, one command slot (slot 0)
 *  - IDENTIFY, READ DMA EXT, WRITE DMA EXT, FLUSH CACHE EXT (48-bit LBA)
 *  - 512-byte logical sectors only (4Kn disks are skipped)
 *  - all DMA memory is static kernel memory below 4 GiB (identity mapped),
 *    data goes through one bounce buffer so callers can pass any pointer
 *
 * Not handled: port multipliers, ATAPI, hot plug, NCQ, 64-bit-only HBAs
 * with memory above 4 GiB (we never put DMA memory there anyway).
 */

extern void vga_write(const char *text);

/* entry.asm: the 4 x 512 page-directory entries (2 MiB pages, identity). */
extern uint64_t pd_table[];

/* ---- HBA registers (byte offsets) ---- */
#define HBA_CAP    0x00
#define HBA_GHC    0x04
#define HBA_IS     0x08
#define HBA_PI     0x0C
#define HBA_CAP2   0x24
#define HBA_BOHC   0x28
#define HBA_PORT(n) (0x100u + 0x80u * (n))

#define GHC_IE     (1u << 1)
#define GHC_AE     (1u << 31)

/* ---- port registers (byte offsets from the port base) ---- */
#define P_CLB      0x00
#define P_CLBU     0x04
#define P_FB       0x08
#define P_FBU      0x0C
#define P_IS       0x10
#define P_IE       0x14
#define P_CMD      0x18
#define P_TFD      0x20
#define P_SIG      0x24
#define P_SSTS     0x28
#define P_SERR     0x30
#define P_CI       0x38

#define CMD_ST     (1u << 0)
#define CMD_FRE    (1u << 4)
#define CMD_FR     (1u << 14)
#define CMD_CR     (1u << 15)

#define TFD_ERR    (1u << 0)
#define TFD_DRQ    (1u << 3)
#define TFD_DF     (1u << 5)
#define TFD_BSY    (1u << 7)

#define IS_TFES    (1u << 30)      /* task file error */

#define SIG_SATA   0x00000101u

/* ---- ATA commands ---- */
#define ATA_IDENTIFY       0xEC
#define ATA_READ_DMA_EXT   0x25
#define ATA_WRITE_DMA_EXT  0x35
#define ATA_FLUSH_EXT      0xEA

#define SPIN_SHORT   2000000u
#define SPIN_LONG    60000000u

#define BOUNCE_SECTORS 64u

/* ---- DMA structures ---- */
typedef struct {
    uint16_t flags;         /* CFL[4:0], W = bit 6 */
    uint16_t prdtl;
    volatile uint32_t prdbc;
    uint32_t ctba;
    uint32_t ctbau;
    uint32_t reserved[4];
} __attribute__((packed)) cmd_header_t;

typedef struct {
    uint32_t dba;
    uint32_t dbau;
    uint32_t reserved;
    uint32_t dbc;           /* byte count - 1 */
} __attribute__((packed)) prdt_t;

typedef struct {
    uint8_t cfis[64];
    uint8_t acmd[16];
    uint8_t reserved[48];
    prdt_t prdt[1];
} __attribute__((packed)) cmd_table_t;

typedef struct {
    cmd_header_t list[32];  /* 1 KiB, 1 KiB aligned */
    uint8_t fis[256];       /* received-FIS area, 256 aligned */
    cmd_table_t table;      /* 128 aligned */
} __attribute__((aligned(1024))) port_mem_t;

_Static_assert(sizeof(cmd_header_t) == 32, "command header size");
_Static_assert(offsetof(port_mem_t, fis) % 256 == 0, "FIS alignment");
_Static_assert(offsetof(port_mem_t, table) % 128 == 0, "table alignment");

typedef struct {
    volatile uint32_t *regs;    /* port register block */
    port_mem_t *mem;
    bool alive;
    char name[4];
    blockdev_t dev;
} disk_t;

static disk_t disks[AHCI_MAX_DISKS];
static port_mem_t port_mem[AHCI_MAX_DISKS];
static unsigned int disk_total;

static uint8_t bounce[BOUNCE_SECTORS * 512u] __attribute__((aligned(4096)));

/* ================================================================
 * Helpers
 * ================================================================ */

static inline uint32_t rd(volatile uint32_t *base, uint32_t off)
{
    return base[off / 4];
}

static inline void wr(volatile uint32_t *base, uint32_t off, uint32_t value)
{
    base[off / 4] = value;
}

static inline void cpu_relax(void)
{
    __asm__ volatile ("pause");
}

static bool wait_clear(volatile uint32_t *base, uint32_t off, uint32_t mask,
                       uint32_t spins)
{
    while (spins--) {
        if (!(rd(base, off) & mask))
            return true;

        cpu_relax();
    }

    return false;
}

static void print_u64(uint64_t value)
{
    char text[21];
    unsigned int n = 0;

    if (value == 0) {
        vga_write("0");
        return;
    }

    while (value) {
        text[n++] = (char)('0' + value % 10);
        value /= 10;
    }

    char out[21];
    unsigned int i = 0;

    while (n)
        out[i++] = text[--n];

    out[i] = '\0';
    vga_write(out);
}

/*
 * HBA registers live in PCI MMIO space, which entry.asm maps like RAM
 * (write-back). Cached register reads would hang the driver on real
 * hardware, so mark the covering 2 MiB pages uncached (PWT+PCD).
 * PCI MMIO lives below 4 GiB here: AHCI BAR5 is a 32-bit BAR.
 */
static void map_uncached(uint32_t phys, uint32_t size)
{
    uint32_t first = phys >> 21;
    uint32_t last = (phys + size - 1u) >> 21;

    for (uint32_t i = first; i <= last && i < 2048; i++)
        pd_table[i] |= (1ull << 3) | (1ull << 4);

    uint64_t cr3;

    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile ("mov %0, %%cr3" : : "r"(cr3) : "memory");
}

/* ================================================================
 * Port control
 * ================================================================ */

static bool port_stop(volatile uint32_t *p)
{
    uint32_t cmd = rd(p, P_CMD);

    if (cmd & (CMD_ST | CMD_CR)) {
        wr(p, P_CMD, cmd & ~CMD_ST);

        if (!wait_clear(p, P_CMD, CMD_CR, SPIN_SHORT))
            return false;
    }

    cmd = rd(p, P_CMD);

    if (cmd & (CMD_FRE | CMD_FR)) {
        wr(p, P_CMD, cmd & ~CMD_FRE);

        if (!wait_clear(p, P_CMD, CMD_FR, SPIN_SHORT))
            return false;
    }

    return true;
}

static bool port_start(volatile uint32_t *p)
{
    if (!wait_clear(p, P_TFD, TFD_BSY | TFD_DRQ, SPIN_SHORT))
        return false;

    wr(p, P_CMD, rd(p, P_CMD) | CMD_FRE);
    wr(p, P_CMD, rd(p, P_CMD) | CMD_ST);
    return true;
}

static bool port_setup(disk_t *d)
{
    volatile uint32_t *p = d->regs;

    if (!port_stop(p))
        return false;

    wr(p, P_CLB, (uint32_t)(uintptr_t)d->mem->list);
    wr(p, P_CLBU, 0);
    wr(p, P_FB, (uint32_t)(uintptr_t)d->mem->fis);
    wr(p, P_FBU, 0);
    wr(p, P_SERR, 0xFFFFFFFFu);
    wr(p, P_IS, 0xFFFFFFFFu);
    wr(p, P_IE, 0);

    return port_start(p);
}

/* ================================================================
 * Command execution (slot 0, polled)
 * ================================================================ */

static bool issue(disk_t *d, uint8_t command, uint64_t lba, uint32_t count,
                  bool write, uint32_t bytes)
{
    port_mem_t *m = d->mem;
    volatile uint32_t *p = d->regs;

    if (!d->alive)
        return false;

    if (!wait_clear(p, P_TFD, TFD_BSY | TFD_DRQ, SPIN_LONG))
        goto fail;

    memset(&m->list[0], 0, sizeof(m->list[0]));
    memset(&m->table, 0, sizeof(m->table));

    m->list[0].flags = (uint16_t)(5u | (write ? (1u << 6) : 0u));
    m->list[0].prdtl = bytes ? 1 : 0;
    m->list[0].prdbc = 0;
    m->list[0].ctba = (uint32_t)(uintptr_t)&m->table;
    m->list[0].ctbau = 0;

    uint8_t *f = m->table.cfis;

    f[0] = 0x27;                    /* register FIS, host to device */
    f[1] = 0x80;                    /* command bit */
    f[2] = command;
    f[4] = (uint8_t)lba;
    f[5] = (uint8_t)(lba >> 8);
    f[6] = (uint8_t)(lba >> 16);
    f[7] = (command == ATA_IDENTIFY) ? 0 : (1u << 6);    /* LBA mode */
    f[8] = (uint8_t)(lba >> 24);
    f[9] = (uint8_t)(lba >> 32);
    f[10] = (uint8_t)(lba >> 40);
    f[12] = (uint8_t)count;
    f[13] = (uint8_t)(count >> 8);

    if (bytes) {
        m->table.prdt[0].dba = (uint32_t)(uintptr_t)bounce;
        m->table.prdt[0].dbau = 0;
        m->table.prdt[0].dbc = bytes - 1u;
    }

    wr(p, P_IS, 0xFFFFFFFFu);
    __asm__ volatile ("" ::: "memory");
    wr(p, P_CI, 1);

    uint32_t spins = SPIN_LONG;

    for (;;) {
        if (!(rd(p, P_CI) & 1u))
            break;

        if (rd(p, P_IS) & IS_TFES)
            goto fail;

        if (!spins--)
            goto fail;

        cpu_relax();
    }

    if ((rd(p, P_IS) & IS_TFES) || (rd(p, P_TFD) & (TFD_ERR | TFD_DF)))
        goto fail;

    return true;

fail:
    /* Restart the port; if even that fails the disk is marked dead. */
    d->alive = port_setup(d);
    return false;
}

/* ================================================================
 * Block device glue
 * ================================================================ */

static bool ahci_block_read(blockdev_t *dev, uint64_t lba, uint32_t count,
                            void *buffer)
{
    disk_t *d = (disk_t *)dev->driver_data;
    uint8_t *out = (uint8_t *)buffer;

    while (count) {
        uint32_t n = count > BOUNCE_SECTORS ? BOUNCE_SECTORS : count;

        if (!issue(d, ATA_READ_DMA_EXT, lba, n, false, n * 512u))
            return false;

        memcpy(out, bounce, (size_t)n * 512u);

        out += (size_t)n * 512u;
        lba += n;
        count -= n;
    }

    return true;
}

static bool ahci_block_write(blockdev_t *dev, uint64_t lba, uint32_t count,
                             const void *buffer)
{
    disk_t *d = (disk_t *)dev->driver_data;
    const uint8_t *in = (const uint8_t *)buffer;

    while (count) {
        uint32_t n = count > BOUNCE_SECTORS ? BOUNCE_SECTORS : count;

        memcpy(bounce, in, (size_t)n * 512u);

        if (!issue(d, ATA_WRITE_DMA_EXT, lba, n, true, n * 512u))
            return false;

        in += (size_t)n * 512u;
        lba += n;
        count -= n;
    }

    /* Persistence matters more than speed: push the drive cache out. */
    return issue(d, ATA_FLUSH_EXT, 0, 0, false, 0);
}

/* ================================================================
 * Probing
 * ================================================================ */

static void print_model(const uint16_t *id)
{
    char model[41];
    unsigned int n = 0;

    for (unsigned int i = 0; i < 20; i++) {
        model[n++] = (char)(id[27 + i] >> 8);
        model[n++] = (char)(id[27 + i] & 0xFF);
    }

    while (n && (model[n - 1] == ' ' || model[n - 1] == '\0'))
        n--;

    model[n] = '\0';
    vga_write(n ? model : "(unnamed disk)");
}

static bool identify(disk_t *d, uint64_t *sectors)
{
    if (!issue(d, ATA_IDENTIFY, 0, 0, false, 512))
        return false;

    const uint16_t *id = (const uint16_t *)bounce;

    if (!(id[83] & (1u << 10))) {
        vga_write("[AHCI] disk lacks 48-bit LBA, skipped\n");
        return false;
    }

    if ((id[106] & 0xC000u) == 0x4000u && (id[106] & (1u << 12))) {
        uint32_t words = (uint32_t)id[117] | ((uint32_t)id[118] << 16);

        if (words * 2u != 512u) {
            vga_write("[AHCI] non-512-byte sectors, skipped\n");
            return false;
        }
    }

    *sectors = (uint64_t)id[100]
             | ((uint64_t)id[101] << 16)
             | ((uint64_t)id[102] << 32)
             | ((uint64_t)id[103] << 48);

    if (*sectors == 0)
        return false;

    vga_write(" ");
    print_model(id);
    return true;
}

static void probe_port(volatile uint32_t *hba, unsigned int port)
{
    volatile uint32_t *p = hba + HBA_PORT(port) / 4;
    uint32_t ssts = rd(p, P_SSTS);

    if ((ssts & 0xF) != 3 || ((ssts >> 8) & 0xF) != 1)
        return;                         /* nothing attached / asleep */

    if (rd(p, P_SIG) != SIG_SATA)
        return;                         /* ATAPI, enclosure, port mult. */

    if (disk_total >= AHCI_MAX_DISKS)
        return;

    disk_t *d = &disks[disk_total];

    memset(d, 0, sizeof(*d));
    memset(&port_mem[disk_total], 0, sizeof(port_mem[disk_total]));

    d->regs = p;
    d->mem = &port_mem[disk_total];
    d->name[0] = 's';
    d->name[1] = 'd';
    d->name[2] = (char)('0' + disk_total);
    d->name[3] = '\0';

    if (!port_setup(d)) {
        vga_write("[AHCI] port ");
        print_u64(port);
        vga_write(": could not start, skipped\n");
        return;
    }

    d->alive = true;

    uint64_t sectors = 0;

    vga_write("[AHCI] ");
    vga_write(d->name);
    vga_write(":");

    if (!identify(d, &sectors)) {
        vga_write(" IDENTIFY failed\n");
        return;
    }

    vga_write(", ");
    print_u64(sectors / 2048u);
    vga_write(" MiB\n");

    d->dev.name = d->name;
    d->dev.sector_count = sectors;
    d->dev.sector_size = 512;
    d->dev.driver_data = d;
    d->dev.read = ahci_block_read;
    d->dev.write = ahci_block_write;

    disk_total++;
}

static void init_controller(const pci_device_t *dev)
{
    uint32_t bar5 = dev->bar[5];

    if ((bar5 & 1u) || (bar5 & ~0xFu) == 0) {
        vga_write("[AHCI] controller without a usable BAR5, skipped\n");
        return;
    }

    uint32_t base = bar5 & ~0xFu;

    /* Memory space + bus master on; keep the RW1C status bits untouched. */
    uint32_t command = pci_config_read32(dev->bus, dev->device,
                                         dev->function, 0x04);
    command = (command & 0xFFFFu) | 0x6u;
    pci_config_write32(dev->bus, dev->device, dev->function, 0x04, command);

    map_uncached(base, 0x1100u);

    volatile uint32_t *hba = (volatile uint32_t *)(uintptr_t)base;

    /* Take ownership from the firmware if it still claims the HBA. */
    if (rd(hba, HBA_CAP2) & 1u) {
        uint32_t bohc = rd(hba, HBA_BOHC);

        if (bohc & 1u) {
            wr(hba, HBA_BOHC, bohc | 2u);
            wait_clear(hba, HBA_BOHC, 1u, SPIN_SHORT);
        }
    }

    uint32_t ghc = rd(hba, HBA_GHC);

    wr(hba, HBA_GHC, (ghc | GHC_AE) & ~GHC_IE);

    uint32_t implemented = rd(hba, HBA_PI);

    for (unsigned int port = 0; port < 32; port++) {
        if (implemented & (1u << port))
            probe_port(hba, port);
    }

    wr(hba, HBA_IS, 0xFFFFFFFFu);
}

unsigned int ahci_init(void)
{
    disk_total = 0;

    vga_write("\n[AHCI] Looking for SATA controllers...\n");

    for (unsigned int i = 0; i < pci_device_count(); i++) {
        const pci_device_t *dev = pci_device_get(i);

        if (dev->class_code == 0x01 && dev->subclass == 0x06 &&
            dev->programming_interface == 0x01)
            init_controller(dev);
    }

    if (disk_total == 0)
        vga_write("[AHCI] No SATA disk found.\n");

    return disk_total;
}

unsigned int ahci_disk_count(void)
{
    return disk_total;
}

blockdev_t *ahci_disk_get(unsigned int index)
{
    return index < disk_total ? &disks[index].dev : 0;
}
