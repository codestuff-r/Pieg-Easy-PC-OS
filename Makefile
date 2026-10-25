
CC := gcc
ASM := nasm

BUILD := build
ISO_DIR := iso
GRUB_EFI_DIR := /usr/lib/grub/x86_64-efi
GRUB_BIOS_DIR := /usr/lib/grub/i386-pc

CFLAGS := -std=gnu11 -ffreestanding -fno-stack-protector -fno-pic -fno-pie -m64 -mno-red-zone -Wall -Wextra -O2 -Ikernel
LDFLAGS := -T linker.ld -nostdlib -no-pie -Wl,--build-id=none -Wl,-z,max-page-size=0x1000

C_SOURCES := \
	kernel/kernel.c \
	kernel/vga.c \
	kernel/framebuffer.c \
	kernel/driverinit.c \
	kernel/fsinit.c \
	kernel/scheduler.c \
	kernel/terminal.c \
	kernel/keyboard.c \
	kernel/blockdev.c \
	kernel/fat.c \
	kernel/panic.c \
	kernel/exceptions.c \
	kernel/pci.c \
	kernel/ata.c

ASM_SOURCES := kernel/entry.asm

OBJECTS := $(patsubst kernel/%.c,$(BUILD)/%.o,$(C_SOURCES)) \
           $(patsubst kernel/%.asm,$(BUILD)/%.o,$(ASM_SOURCES))

.PHONY: all clean iso iso-uefi iso-legacy run disk check-tools

all: $(BUILD)/pieg.elf

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: kernel/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: kernel/%.asm | $(BUILD)
	$(ASM) -f elf64 $< -o $@

$(BUILD)/pieg.elf: $(OBJECTS) linker.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJECTS) -lgcc

check-tools:
	@test -f $(GRUB_EFI_DIR)/modinfo.sh || \
		(echo "Missing GRUB x86_64-efi modules. Install grub-efi-amd64-bin."; exit 1)
	@test -f $(GRUB_BIOS_DIR)/modinfo.sh || \
		(echo "Missing GRUB i386-pc modules. Install grub-pc-bin."; exit 1)
	@command -v grub-mkrescue >/dev/null || \
		(echo "Missing grub-mkrescue. Install grub-common."; exit 1)
	@command -v xorriso >/dev/null || \
		(echo "Missing xorriso."; exit 1)

iso: iso-uefi iso-legacy

iso-uefi: $(BUILD)/pieg.elf check-tools
	mkdir -p $(ISO_DIR)/boot/grub
	cp $(BUILD)/pieg.elf $(ISO_DIR)/boot/pieg.elf
	grub-mkrescue --directory=$(GRUB_EFI_DIR) -o $(BUILD)/pieg-uefi.iso $(ISO_DIR)

iso-legacy: $(BUILD)/pieg.elf check-tools
	mkdir -p $(ISO_DIR)/boot/grub
	cp $(BUILD)/pieg.elf $(ISO_DIR)/boot/pieg.elf
	grub-mkrescue --directory=$(GRUB_BIOS_DIR) -o $(BUILD)/pieg-legacy.iso $(ISO_DIR)

disk: $(BUILD)/pieg-disk.img

$(BUILD)/pieg-disk.img: | $(BUILD)
	dd if=/dev/zero of=$@ bs=1M count=64 status=none
	mkfs.fat -F 16 $@

run: iso-legacy disk
	@printf '\nLaunch PIEG in QEMU? [y/N] '; \
	read answer; \
	case "$$answer" in \
		y|Y|yes|YES) \
			qemu-system-x86_64 \
				-no-reboot \
				-no-shutdown \
				-boot order=d \
				-drive file=$(BUILD)/pieg-disk.img,format=raw,if=ide,index=0 \
				-cdrom $(BUILD)/pieg-legacy.iso \
				-monitor stdio ;; \
		*) echo "Cancelled."; exit 0 ;; \
	esac

clean:
	rm -f $(BUILD)/*.o \
		$(BUILD)/pieg.elf \
		$(BUILD)/pieg.iso \
		$(BUILD)/pieg-uefi.iso \
		$(BUILD)/pieg-legacy.iso
