#pragma once

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

namespace lx {

constexpr uint64_t kPageSize = 4096;
constexpr uint64_t kPageMask = kPageSize - 1;
constexpr uint64_t kDirectMapBase = 0xFFFF800000000000ULL;

constexpr uint64_t page_floor(uint64_t value) { return value & ~kPageMask; }
constexpr uint64_t page_ceil(uint64_t value) { return (value + kPageMask) & ~kPageMask; }

template <typename T>
constexpr T min(T a, T b) {
    return a < b ? a : b;
}

template <typename T>
constexpr T max(T a, T b) {
    return a > b ? a : b;
}

inline void* phys_to_virt(uint64_t phys) { return reinterpret_cast<void*>(kDirectMapBase + phys); }
inline uint64_t virt_to_phys(const void* virt) { return reinterpret_cast<uint64_t>(virt) - kDirectMapBase; }

inline uint64_t read_msr(uint32_t msr) {
    uint32_t low, high;
    asm volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return (static_cast<uint64_t>(high) << 32) | low;
}

inline void write_msr(uint32_t msr, uint64_t value) {
    asm volatile("wrmsr" : : "a"(static_cast<uint32_t>(value)), "d"(static_cast<uint32_t>(value >> 32)), "c"(msr));
}

inline uint64_t read_tsc() {
    uint32_t low, high;
    asm volatile("rdtsc" : "=a"(low), "=d"(high));
    return (static_cast<uint64_t>(high) << 32) | low;
}

inline void port_out8(uint16_t port, uint8_t value) { asm volatile("outb %0, %1" : : "a"(value), "Nd"(port)); }

inline uint8_t port_in8(uint16_t port) {
    uint8_t value;
    asm volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

inline uint64_t read_cr3() {
    uint64_t value;
    asm volatile("mov %%cr3, %0" : "=r"(value));
    return value;
}

inline void write_cr3(uint64_t value) { asm volatile("mov %0, %%cr3" : : "r"(value) : "memory"); }

inline void flush_tlb_page(uint64_t address) { asm volatile("invlpg (%0)" : : "r"(address) : "memory"); }

size_t string_length(const char* text);
int string_compare(const char* a, const char* b);
int string_compare_n(const char* a, const char* b, size_t count);
const char* string_find(const char* haystack, const char* needle);
char* string_duplicate(const char* text);
char* string_duplicate_n(const char* text, size_t count);

void format(char* buffer, size_t capacity, const char* pattern, ...);
void log(const char* pattern, ...);
[[noreturn]] void panic(const char* pattern, ...);

uint64_t alloc_frame();
uint64_t alloc_frame_zeroed();
void free_frame(uint64_t phys);
uint64_t alloc_contiguous_frames(uint64_t count);
uint64_t free_frame_count();
uint64_t total_frame_count();

void memory_init();
void* kernel_alloc(size_t size);
void* kernel_alloc_zeroed(size_t size);
void kernel_free(void* pointer);

template <typename T>
T* kernel_new() {
    return static_cast<T*>(kernel_alloc_zeroed(sizeof(T)));
}

void boot_capture(uint64_t multiboot_info);
const void* boot_info();
bool boot_module(uint64_t* start, uint64_t* end);
bool boot_cmdline_contains(const char* option);

void cpu_init();
void clock_init();

}

extern "C" {
void* memcpy(void* destination, const void* source, size_t count);
void* memmove(void* destination, const void* source, size_t count);
void* memset(void* destination, int value, size_t count);
int memcmp(const void* a, const void* b, size_t count);
}
