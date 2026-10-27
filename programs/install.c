#include "libpieg.h"

/*
 * install            list the disks and what can be done with them
 * install DISK [MiB] create the persistent PIEG volume on DISK
 *
 * Writes a PIEG volume (FAT32, label PIEG) into free space on a GPT disk
 * (or on a blank disk, which gets a fresh GPT first), then copies the
 * running system onto it. Next boot, the kernel finds and mounts that
 * volume on its own, so everything you write there survives a reboot.
 *
 * Existing partitions are never modified; disks that are neither blank
 * nor GPT are refused.
 */

static void put_mib(uint64_t bytes)
{
    p_u64(bytes >> 20);
    p_puts(" MiB");
}

static const char *state_text(int state)
{
    switch (state) {
    case 1: return "blank (a new GPT will be written)";
    case 2: return "GPT";
    default: return "not supported (not blank, no valid GPT)";
    }
}

static int parse_u32(const char *s, uint32_t *out)
{
    uint32_t v = 0;

    if (!*s)
        return 0;

    for (; *s; s++) {
        if (*s < '0' || *s > '9' || v > 4000000u)
            return 0;

        v = v * 10u + (uint32_t)(*s - '0');
    }

    *out = v;
    return 1;
}

static void read_line(char *dst, size_t capacity)
{
    size_t n = 0;

    for (;;) {
        char c = p_getc();

        if (c == '\r' || c == '\n') {
            p_putc('\n');
            break;
        }

        if ((c == '\b' || (unsigned char)c == 127) && n) {
            n--;
            p_putc('\b');
            continue;
        }

        if (c >= 32 && c < 127 && n + 1 < capacity) {
            dst[n++] = c;
            p_putc(c);
        }
    }

    dst[n] = '\0';
}

static void list_disks(void)
{
    unsigned int count = pieg->disk_count();

    if (!count) {
        p_putln("No ATA/SATA disk found.");
        return;
    }

    for (unsigned int i = 0; i < count; i++) {
        pieg_diskinfo_t d;

        if (!pieg->disk_info(i, &d))
            continue;

        p_puts(d.name);
        p_puts("  ");
        put_mib(d.bytes);
        p_puts("  ");
        p_puts(state_text(d.state));

        if (d.state) {
            p_puts(", free ");
            put_mib(d.free_bytes);
        }

        p_putc('\n');
    }

    p_putln("");
    p_putln("Usage: install DISK [MiB]    (default size: free space, max 4096)");
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        list_disks();
        return 0;
    }

    unsigned int count = pieg->disk_count();
    unsigned int index = count;
    pieg_diskinfo_t d;

    for (unsigned int i = 0; i < count; i++) {
        if (pieg->disk_info(i, &d) && p_ieq(d.name, argv[1])) {
            index = i;
            break;
        }
    }

    if (index == count) {
        p_puts("No such disk: ");
        p_putln(argv[1]);
        return 1;
    }

    uint32_t mib = 0;

    if (argc > 2 && !parse_u32(argv[2], &mib)) {
        p_putln("Size must be a number of MiB.");
        return 1;
    }

    if (!d.state) {
        p_puts(d.name);
        p_putln(" is not blank and has no valid GPT; PIEG will not touch it.");
        return 1;
    }

    p_puts("Disk ");
    p_puts(d.name);
    p_puts(": ");
    p_putln(state_text(d.state));
    p_puts("A new PIEG volume of ");

    if (mib)
        p_u64(mib);
    else
        p_u64((d.free_bytes >> 20) < 4096 ? (d.free_bytes >> 20) : 4096);

    p_putln(" MiB will be added in free space.");
    p_putln("This edits the partition table of the disk. Back up anything");
    p_putln("important on it first. Existing partitions are not modified.");
    p_puts("Type YES to continue: ");

    char answer[8];

    read_line(answer, sizeof(answer));

    if (!p_streq(answer, "YES")) {
        p_putln("Cancelled.");
        return 1;
    }

    p_putln("Working...");

    const char *err = pieg->disk_install(index, mib);

    if (err) {
        p_puts("install: ");
        p_putln(err);
        return 1;
    }

    p_putln("Done. Type 'reboot' to start from the persistent volume.");
    return 0;
}
