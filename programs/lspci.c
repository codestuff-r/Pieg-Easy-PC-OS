#include "libpieg.h"

/*
 * lspci [-v]
 *
 * Lists every PCI function the kernel found at boot. -v adds revision,
 * prog-if, IRQ line and the decoded BAR registers.
 */

static void put_hex(uint64_t value, unsigned int digits)
{
    static const char hex[] = "0123456789ABCDEF";

    while (digits--)
        p_putc(hex[(value >> (digits * 4)) & 0xF]);
}

static void pad(const char *s, unsigned int width)
{
    unsigned int n = (unsigned int)p_strlen(s);

    p_puts(s);

    while (n++ < width)
        p_putc(' ');
}

static const char *vendor_name(uint16_t id)
{
    switch (id) {
    case 0x8086: return "Intel";
    case 0x1022: return "AMD";
    case 0x1002: return "AMD/ATI";
    case 0x10DE: return "NVIDIA";
    case 0x10EC: return "Realtek";
    case 0x14E4: return "Broadcom";
    case 0x168C: return "Atheros";
    case 0x17CB: return "Qualcomm";
    case 0x14C3: return "MediaTek";
    case 0x1B4B: return "Marvell";
    case 0x1106: return "VIA";
    case 0x1B21: return "ASMedia";
    case 0x1912: return "Renesas";
    case 0x104C: return "TI";
    case 0x197B: return "JMicron";
    case 0x144D: return "Samsung";
    case 0x1987: return "Phison";
    case 0x1C5C: return "SK hynix";
    case 0x1E0F: return "KIOXIA";
    case 0x15B7: return "SanDisk";
    case 0x1344: return "Micron";
    case 0x126F: return "Silicon Motion";
    case 0x1B36: return "Red Hat/QEMU";
    case 0x1AF4: return "virtio";
    case 0x1234: return "QEMU/Bochs";
    case 0x15AD: return "VMware";
    case 0x80EE: return "VirtualBox";
    case 0x1414: return "Microsoft";
    case 0x1AE0: return "Google";
    case 0x1D0F: return "Amazon";
    }

    return "unknown";
}

static const char *class_name(uint8_t cls, uint8_t sub)
{
    switch (cls) {
    case 0x00:
        return sub == 0x01 ? "VGA-compatible (legacy)" : "Unclassified";
    case 0x01:
        switch (sub) {
        case 0x00: return "SCSI controller";
        case 0x01: return "IDE controller";
        case 0x02: return "Floppy controller";
        case 0x05: return "ATA controller";
        case 0x06: return "SATA controller";
        case 0x07: return "SAS controller";
        case 0x08: return "NVMe controller";
        }
        return "Storage controller";
    case 0x02:
        switch (sub) {
        case 0x00: return "Ethernet controller";
        case 0x80: return "Network controller";
        }
        return "Network controller";
    case 0x03:
        switch (sub) {
        case 0x00: return "VGA controller";
        case 0x01: return "XGA controller";
        case 0x02: return "3D controller";
        }
        return "Display controller";
    case 0x04:
        switch (sub) {
        case 0x00: return "Video device";
        case 0x01: return "Audio device";
        case 0x03: return "HD Audio";
        }
        return "Multimedia device";
    case 0x05: return "Memory controller";
    case 0x06:
        switch (sub) {
        case 0x00: return "Host bridge";
        case 0x01: return "ISA bridge";
        case 0x04: return "PCI bridge";
        case 0x07: return "CardBus bridge";
        case 0x80: return "Other bridge";
        }
        return "Bridge";
    case 0x07:
        switch (sub) {
        case 0x00: return "Serial controller";
        case 0x80: return "Communication ctrl";
        }
        return "Communication ctrl";
    case 0x08:
        switch (sub) {
        case 0x00: return "PIC";
        case 0x01: return "DMA controller";
        case 0x02: return "Timer";
        case 0x03: return "RTC";
        case 0x05: return "SD host controller";
        }
        return "System peripheral";
    case 0x09: return "Input device";
    case 0x0A: return "Docking station";
    case 0x0B: return "Processor";
    case 0x0C:
        switch (sub) {
        case 0x03: return "USB controller";
        case 0x05: return "SMBus controller";
        }
        return "Serial bus controller";
    case 0x0D: return "Wireless controller";
    case 0x0E: return "Intelligent I/O";
    case 0x0F: return "Satellite comm";
    case 0x10: return "Encryption controller";
    case 0x11: return "Signal processing";
    case 0x12: return "Processing accelerator";
    }

    return "Unknown class";
}

static void print_bars(const pieg_pciinfo_t *d)
{
    unsigned int count = d->header_type == 0 ? 6 : d->header_type == 1 ? 2 : 0;

    for (unsigned int i = 0; i < count; i++) {
        uint32_t bar = d->bar[i];

        if (bar == 0)
            continue;

        p_puts("          BAR");
        p_u64(i);
        p_puts(": ");

        if (bar & 1) {
            p_puts("io   0x");
            put_hex(bar & ~3u, 4);
        } else {
            unsigned int type = (bar >> 1) & 3;
            uint64_t base = bar & ~0xFu;

            if (type == 2 && i + 1 < count) {
                base |= (uint64_t)d->bar[i + 1] << 32;
                i++;
                p_puts("mem64");
            } else {
                p_puts("mem32");
            }

            p_puts((bar & 8) ? " pf 0x" : "    0x");
            put_hex(base, type == 2 ? 16 : 8);
        }

        p_putc('\n');
    }
}

int main(int argc, char **argv)
{
    int verbose = 0;

    for (int i = 1; i < argc; i++) {
        if (p_streq(argv[i], "-v")) {
            verbose = 1;
        } else {
            p_putln("usage: lspci [-v]");
            return 1;
        }
    }

    if (pieg->version < 2 || !pieg->pci_count) {
        p_putln("lspci: this kernel has no PCI API (needs API version 2)");
        return 1;
    }

    unsigned int count = pieg->pci_count();

    for (unsigned int i = 0; i < count; i++) {
        pieg_pciinfo_t d;

        if (!pieg->pci_info(i, &d))
            continue;

        put_hex(d.bus, 2);
        p_putc(':');
        put_hex(d.device, 2);
        p_putc('.');
        put_hex(d.function, 1);
        p_putc(' ');
        put_hex(d.vendor_id, 4);
        p_putc(':');
        put_hex(d.device_id, 4);
        p_putc(' ');
        pad(vendor_name(d.vendor_id), 14);
        p_putln(class_name(d.class_code, d.subclass));

        if (verbose) {
            p_puts("          class ");
            put_hex(d.class_code, 2);
            put_hex(d.subclass, 2);
            p_puts("  prog-if ");
            put_hex(d.prog_if, 2);
            p_puts("  rev ");
            put_hex(d.revision, 2);
            p_puts("  irq ");

            if (d.irq_line == 0xFF)
                p_putc('-');
            else
                p_u64(d.irq_line);

            p_putc('\n');
            print_bars(&d);
        }
    }

    p_u64(count);
    p_putln(" PCI functions");

    return 0;
}
