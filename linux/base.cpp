#include "base.h"

extern "C" {
void* ZaalokujRamkeC();
void ZwolnijRamkeC(void* phys);
uint64_t pmm_alloc_contiguous(uint64_t count);
uint64_t pmm_free_frames();
uint64_t pmm_total_frames();
}

extern "C" void* memcpy(void* destination, const void* source, size_t count) {
    void* result = destination;
    asm volatile("rep movsb" : "+D"(destination), "+S"(source), "+c"(count) : : "memory");
    return result;
}

extern "C" void* memmove(void* destination, const void* source, size_t count) {
    auto* to = static_cast<uint8_t*>(destination);
    auto* from = static_cast<const uint8_t*>(source);
    if (to <= from || to >= from + count) {
        return memcpy(destination, source, count);
    }
    to += count - 1;
    from += count - 1;
    asm volatile("std; rep movsb; cld" : "+D"(to), "+S"(from), "+c"(count) : : "memory");
    return destination;
}

extern "C" void* memset(void* destination, int value, size_t count) {
    void* result = destination;
    asm volatile("rep stosb" : "+D"(destination), "+c"(count) : "a"(value) : "memory");
    return result;
}

extern "C" int memcmp(const void* a, const void* b, size_t count) {
    auto* left = static_cast<const uint8_t*>(a);
    auto* right = static_cast<const uint8_t*>(b);
    for (size_t i = 0; i < count; ++i) {
        if (left[i] != right[i]) return left[i] - right[i];
    }
    return 0;
}

namespace lx {

size_t string_length(const char* text) {
    size_t length = 0;
    while (text[length]) ++length;
    return length;
}

int string_compare(const char* a, const char* b) {
    while (*a && *a == *b) {
        ++a;
        ++b;
    }
    return static_cast<uint8_t>(*a) - static_cast<uint8_t>(*b);
}

int string_compare_n(const char* a, const char* b, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        if (a[i] != b[i]) return static_cast<uint8_t>(a[i]) - static_cast<uint8_t>(b[i]);
        if (!a[i]) return 0;
    }
    return 0;
}

const char* string_find(const char* haystack, const char* needle) {
    const size_t needle_length = string_length(needle);
    for (; *haystack; ++haystack) {
        if (!string_compare_n(haystack, needle, needle_length)) return haystack;
    }
    return nullptr;
}

char* string_duplicate_n(const char* text, size_t count) {
    auto* copy = static_cast<char*>(kernel_alloc(count + 1));
    memcpy(copy, text, count);
    copy[count] = 0;
    return copy;
}

char* string_duplicate(const char* text) { return string_duplicate_n(text, string_length(text)); }

namespace {

struct FormatSink {
    char* buffer;
    size_t capacity;
    size_t length;

    void put(char c) {
        if (length + 1 < capacity) buffer[length] = c;
        ++length;
    }
};

void format_number(FormatSink& sink, uint64_t value, unsigned base, bool negative, int width, char pad) {
    char digits[32];
    int count = 0;
    do {
        const unsigned digit = value % base;
        digits[count++] = static_cast<char>(digit < 10 ? '0' + digit : 'a' + digit - 10);
        value /= base;
    } while (value);
    if (negative) sink.put('-');
    for (int i = count + (negative ? 1 : 0); i < width; ++i) sink.put(pad);
    while (count) sink.put(digits[--count]);
}

void format_args(FormatSink& sink, const char* pattern, va_list args) {
    for (; *pattern; ++pattern) {
        if (*pattern != '%') {
            sink.put(*pattern);
            continue;
        }
        ++pattern;
        char pad = ' ';
        int width = 0;
        if (*pattern == '0') {
            pad = '0';
            ++pattern;
        }
        while (*pattern >= '0' && *pattern <= '9') width = width * 10 + (*pattern++ - '0');
        bool wide = false;
        while (*pattern == 'l') {
            wide = true;
            ++pattern;
        }
        switch (*pattern) {
            case 's': {
                const char* text = va_arg(args, const char*);
                for (text = text ? text : "(null)"; *text; ++text) sink.put(*text);
                break;
            }
            case 'd': {
                const int64_t value = wide ? va_arg(args, int64_t) : va_arg(args, int);
                format_number(sink, value < 0 ? -static_cast<uint64_t>(value) : value, 10, value < 0, width, pad);
                break;
            }
            case 'u':
                format_number(sink, wide ? va_arg(args, uint64_t) : va_arg(args, unsigned), 10, false, width, pad);
                break;
            case 'x':
                format_number(sink, wide ? va_arg(args, uint64_t) : va_arg(args, unsigned), 16, false, width, pad);
                break;
            case 'p':
                sink.put('0');
                sink.put('x');
                format_number(sink, va_arg(args, uint64_t), 16, false, 0, ' ');
                break;
            case '%':
                sink.put('%');
                break;
            default:
                sink.put(*pattern);
                break;
        }
    }
}

void serial_write(const char* text, size_t length) {
    for (size_t i = 0; i < length; ++i) {
        while (!(port_in8(0x3FD) & 0x20)) {
        }
        port_out8(0x3F8, text[i]);
    }
}

void log_args(const char* pattern, va_list args) {
    char line[512];
    FormatSink sink{line, sizeof(line), 0};
    format_args(sink, pattern, args);
    serial_write(line, min(sink.length, sizeof(line) - 1));
}

}

void format(char* buffer, size_t capacity, const char* pattern, ...) {
    va_list args;
    va_start(args, pattern);
    FormatSink sink{buffer, capacity, 0};
    format_args(sink, pattern, args);
    va_end(args);
    buffer[min(sink.length, capacity - 1)] = 0;
}

void log(const char* pattern, ...) {
    va_list args;
    va_start(args, pattern);
    log_args(pattern, args);
    va_end(args);
}

void panic(const char* pattern, ...) {
    va_list args;
    va_start(args, pattern);
    log("kernel panic: ");
    log_args(pattern, args);
    log("\n");
    va_end(args);
    for (;;) asm volatile("cli; hlt");
}

uint64_t alloc_frame() { return reinterpret_cast<uint64_t>(ZaalokujRamkeC()); }

uint64_t alloc_frame_zeroed() {
    const uint64_t phys = alloc_frame();
    if (phys) memset(phys_to_virt(phys), 0, kPageSize);
    return phys;
}

void free_frame(uint64_t phys) { ZwolnijRamkeC(reinterpret_cast<void*>(phys)); }
uint64_t alloc_contiguous_frames(uint64_t count) { return pmm_alloc_contiguous(count); }
uint64_t free_frame_count() { return pmm_free_frames(); }
uint64_t total_frame_count() { return pmm_total_frames(); }

namespace {

constexpr uint64_t kArenaFrames = 32768;
constexpr int kSmallestClassShift = 5;
constexpr int kLargestClassShift = 24;
constexpr size_t kHeaderSize = 16;

struct FreeBlock {
    FreeBlock* next;
};

struct BlockHeader {
    uint64_t size_class;
    uint64_t magic;
};

constexpr uint64_t kBlockMagic = 0x6C78616C6C6F6321ULL;

uint8_t* arena_cursor;
uint8_t* arena_end;
FreeBlock* free_lists[kLargestClassShift + 1];

int size_class_for(size_t total) {
    int shift = kSmallestClassShift;
    while ((1ULL << shift) < total) ++shift;
    return shift;
}

}

void memory_init() {
    const uint64_t phys = alloc_contiguous_frames(kArenaFrames);
    if (!phys) panic("cannot reserve kernel heap arena");
    arena_cursor = static_cast<uint8_t*>(phys_to_virt(phys));
    arena_end = arena_cursor + kArenaFrames * kPageSize;
}

void* kernel_alloc(size_t size) {
    const int shift = size_class_for(size + kHeaderSize);
    if (shift > kLargestClassShift) panic("kernel_alloc: %lu bytes exceeds largest class", size);
    uint8_t* block;
    if (free_lists[shift]) {
        block = reinterpret_cast<uint8_t*>(free_lists[shift]);
        free_lists[shift] = free_lists[shift]->next;
    } else {
        const uint64_t block_size = 1ULL << shift;
        const uint64_t start = (reinterpret_cast<uint64_t>(arena_cursor) + block_size - 1) & ~(block_size - 1);
        if (start + block_size > reinterpret_cast<uint64_t>(arena_end)) panic("kernel heap exhausted");
        block = reinterpret_cast<uint8_t*>(start);
        arena_cursor = block + block_size;
    }
    auto* header = reinterpret_cast<BlockHeader*>(block);
    header->size_class = shift;
    header->magic = kBlockMagic;
    return block + kHeaderSize;
}

void* kernel_alloc_zeroed(size_t size) {
    void* pointer = kernel_alloc(size);
    memset(pointer, 0, size);
    return pointer;
}

void kernel_free(void* pointer) {
    if (!pointer) return;
    auto* block = static_cast<uint8_t*>(pointer) - kHeaderSize;
    auto* header = reinterpret_cast<BlockHeader*>(block);
    if (header->magic != kBlockMagic) panic("kernel_free: corrupt block %p", pointer);
    const int shift = static_cast<int>(header->size_class);
    header->magic = 0;
    auto* entry = reinterpret_cast<FreeBlock*>(block);
    entry->next = free_lists[shift];
    free_lists[shift] = entry;
}

namespace {

constexpr uint32_t kTagCmdline = 1;
constexpr uint32_t kTagModule = 3;
constexpr size_t kBootInfoCapacity = 65536;

alignas(8) uint8_t boot_info_copy[kBootInfoCapacity];

struct Tag {
    uint32_t type;
    uint32_t size;
};

const Tag* find_tag(uint32_t type) {
    const auto total = *reinterpret_cast<const uint32_t*>(boot_info_copy);
    uint64_t offset = 8;
    while (offset + sizeof(Tag) <= total) {
        const auto* tag = reinterpret_cast<const Tag*>(boot_info_copy + offset);
        if (!tag->type) break;
        if (tag->type == type) return tag;
        offset += (tag->size + 7) & ~7ULL;
    }
    return nullptr;
}

}

void boot_capture(uint64_t multiboot_info) {
    const auto total = *reinterpret_cast<const uint32_t*>(multiboot_info);
    if (total > kBootInfoCapacity) panic("multiboot information too large");
    memcpy(boot_info_copy, reinterpret_cast<const void*>(multiboot_info), total);
}

const void* boot_info() { return boot_info_copy; }

bool boot_module(uint64_t* start, uint64_t* end) {
    const Tag* tag = find_tag(kTagModule);
    if (!tag) return false;
    const auto* fields = reinterpret_cast<const uint32_t*>(tag + 1);
    *start = fields[0];
    *end = fields[1];
    return true;
}

bool boot_cmdline_contains(const char* option) {
    const Tag* tag = find_tag(kTagCmdline);
    return tag && string_find(reinterpret_cast<const char*>(tag + 1), option);
}

void cpu_init() {
    constexpr uint64_t kCr0MonitorCoprocessor = 1ULL << 1;
    constexpr uint64_t kCr0Emulation = 1ULL << 2;
    constexpr uint64_t kCr0WriteProtect = 1ULL << 16;
    constexpr uint64_t kCr4Fxsr = 1ULL << 9;
    constexpr uint64_t kCr4XmmExceptions = 1ULL << 10;
    constexpr uint32_t kMsrPat = 0x277;
    constexpr uint64_t kPatWriteCombiningOnPwt = 0x0007040600070106ULL;

    uint64_t cr0, cr4;
    asm volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 = (cr0 & ~kCr0Emulation) | kCr0MonitorCoprocessor | kCr0WriteProtect;
    asm volatile("mov %0, %%cr0" : : "r"(cr0));

    asm volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= kCr4Fxsr | kCr4XmmExceptions;
    asm volatile("mov %0, %%cr4" : : "r"(cr4));

    asm volatile("fninit");
    write_msr(kMsrPat, kPatWriteCombiningOnPwt);
}

}
