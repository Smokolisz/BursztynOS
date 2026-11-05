CROSS_PREFIX := $(if $(shell command -v x86_64-linux-gnu-as 2>/dev/null),x86_64-linux-gnu-,)
CXX := $(CROSS_PREFIX)g++
AS := $(CROSS_PREFIX)as
LD := $(CROSS_PREFIX)ld
OBJCOPY := $(CROSS_PREFIX)objcopy

BUILD := build
KERNEL := $(BUILD)/system_operacyjny.bin
DESKTOP_ISO := $(BUILD)/BursztynOS.iso
REAPER_ISO := $(BUILD)/BursztynOS-REAPER.iso
ROOTFS := $(BUILD)/rootfs.img
DISK := $(BUILD)/wirtualny_dysk.img

KERNEL_FLAGS := -ffreestanding -O2 -fno-exceptions -fno-rtti -fno-threadsafe-statics \
	-fno-stack-protector -fno-pie -fno-pic -fcf-protection=none -fno-asynchronous-unwind-tables \
	-mcmodel=large -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -I.
LEGACY_FLAGS := $(KERNEL_FLAGS) -std=gnu++20 -w
LINUX_FLAGS := $(KERNEL_FLAGS) -std=c++20 -Wall -Wextra -Wno-unused-parameter -Ilinux

LEGACY_OBJECTS := $(addprefix $(BUILD)/, boot.o gdt.o tss.o apic.o idt.o przerwania.o klawiatura.o mysz.o \
	zegar-rtc.o pmm.o vmm.o psf.o grafika.o syscalls.o pci.o ahci.o ring3.o loader.o kernel.o shell_blob.o)
LINUX_OBJECTS := $(addprefix $(BUILD)/linux/, base.o sched.o vfs.o mm.o process.o futex.o signal.o syscalls.o \
	devices.o kernel_api.o entry.o)
HEADERS := $(wildcard *.h linux/*.h)

QEMU := qemu-system-x86_64
QEMU_MACHINE := -m 3G -vga std -serial stdio -device ahci,id=ahci
QEMU_ACCEL := $(if $(wildcard /dev/kvm),-enable-kvm -cpu host,-cpu max)

.PHONY: all iso run reaper reaper-iso clear clean cdysk

all: $(KERNEL)

$(BUILD)/%.o: %.cpp $(HEADERS) | $(BUILD)/linux
	$(CXX) $(LEGACY_FLAGS) -c $< -o $@

$(BUILD)/%.o: %.S | $(BUILD)/linux
	$(AS) -c $< -o $@

$(BUILD)/linux/%.o: linux/%.cpp $(HEADERS) | $(BUILD)/linux
	$(CXX) $(LINUX_FLAGS) -c $< -o $@

$(BUILD)/linux/%.o: linux/%.S | $(BUILD)/linux
	$(AS) -c $< -o $@

$(BUILD)/linux:
	mkdir -p $@

$(BUILD)/shell_tmp.o: shell.cpp | $(BUILD)/linux
	$(CXX) $(LEGACY_FLAGS) -c $< -o $@

$(BUILD)/shell.bin: $(BUILD)/shell_tmp.o shell_linker.ld
	$(LD) -T shell_linker.ld -nostdlib -no-pie $< -o $(BUILD)/shell.elf
	$(OBJCOPY) -O binary $(BUILD)/shell.elf $@

$(BUILD)/shell_blob.o: $(BUILD)/shell.bin
	cd $(BUILD) && $(LD) -r -b binary shell.bin -o shell_blob.o

$(KERNEL): $(LEGACY_OBJECTS) $(LINUX_OBJECTS) linker.ld
	$(CXX) -T linker.ld -nostdlib -no-pie -z noexecstack -o $@ $(LEGACY_OBJECTS) $(LINUX_OBJECTS) -lgcc

define make_iso
	rm -rf $(BUILD)/isodir
	mkdir -p $(BUILD)/isodir/boot/grub
	cp $(KERNEL) $(BUILD)/isodir/boot/
	$(if $(2),ln $(ROOTFS) $(BUILD)/isodir/boot/rootfs.img)
	printf '%s\n' 'set timeout=0' 'set default=0' 'menuentry "Bursztyn OS" {' \
		'    insmod all_video' '    set gfxpayload=keep' \
		'    multiboot2 /boot/system_operacyjny.bin $(2)' \
		$(if $(2),'    module2 /boot/rootfs.img') '    boot' '}' > $(BUILD)/isodir/boot/grub/grub.cfg
	grub-mkrescue -o $(1) $(BUILD)/isodir --xorriso=xorriso
endef

iso: $(DESKTOP_ISO)

$(DESKTOP_ISO): $(KERNEL)
	$(call make_iso,$@,)

$(DISK): | $(BUILD)/linux
	qemu-img create -f raw $@ 40M
	dd if=tapeta.bmp of=$@ bs=512 seek=10 conv=notrunc

QEMU_DISK = -drive id=disk,file=$(DISK),format=raw,if=none -device ide-hd,drive=disk,bus=ahci.0

run: $(DESKTOP_ISO) $(DISK)
	$(QEMU) $(QEMU_ACCEL) $(QEMU_MACHINE) $(QEMU_DISK) -cdrom $(DESKTOP_ISO)

include reaper/reaper.mk

reaper-iso: $(REAPER_ISO)

$(REAPER_ISO): $(KERNEL) $(ROOTFS)
	$(call make_iso,$@,autostart=reaper $(KERNEL_ARGS))

reaper: $(REAPER_ISO) $(DISK)
	$(QEMU) $(QEMU_ACCEL) $(QEMU_MACHINE) $(QEMU_DISK) -cdrom $(REAPER_ISO)

clean clear:
	rm -rf $(BUILD)/*.o $(BUILD)/linux $(BUILD)/*.bin $(BUILD)/*.elf $(BUILD)/*.iso $(BUILD)/isodir

cdysk:
	rm -f $(DISK)
