#include "pci.h"
#include "ata.h"

extern void vga_write(const char *text);

void driverinit(void)
{
    vga_write("\nInitializing hardware drivers...\n");

    pci_init();
    ata_init();

    vga_write("Hardware driver initialization complete.\n");
}
