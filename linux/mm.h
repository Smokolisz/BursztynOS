#pragma once

#include "base.h"
#include "vfs.h"

namespace lx {

constexpr uint32_t kProtRead = 1;
constexpr uint32_t kProtWrite = 2;
constexpr uint32_t kProtExec = 4;

constexpr uint32_t kMapShared = 0x01;
constexpr uint32_t kMapPrivate = 0x02;
constexpr uint32_t kMapFixed = 0x10;
constexpr uint32_t kMapAnonymous = 0x20;
constexpr uint32_t kMapFixedNoReplace = 0x100000;

constexpr uint64_t kUserStackTop = 0x00007FFFFFFFF000ULL;
constexpr uint64_t kUserStackSize = 8ULL * 1024 * 1024;

struct Vma {
    uint64_t start;
    uint64_t end;
    uint32_t prot;
    uint32_t flags;
    Node* node;
    uint64_t offset;
};

class AddressSpace {
public:
    static AddressSpace* create(uint64_t kernel_pml4_phys);
    void destroy();

    uint64_t cr3() const { return pml4_phys_; }

    int64_t map(uint64_t hint, uint64_t length, uint32_t prot, uint32_t flags, Node* node, uint64_t offset);
    int unmap(uint64_t start, uint64_t length);
    int protect(uint64_t start, uint64_t length, uint32_t prot);
    int64_t remap(uint64_t old_start, uint64_t old_length, uint64_t new_length, uint32_t flags, uint64_t new_address);
    int discard_pages(uint64_t start, uint64_t length);
    int sync(uint64_t start, uint64_t length);
    uint64_t set_break(uint64_t requested);
    void init_break(uint64_t start);

    bool handle_fault(uint64_t address, bool write);
    bool range_accessible(uint64_t start, uint64_t length, bool write) const;
    const Vma* find(uint64_t address) const;

private:
    uint64_t pml4_phys_;
    Vma* vmas_;
    size_t vma_count_;
    size_t vma_capacity_;
    uint64_t break_start_;
    uint64_t break_end_;

    size_t lower_bound(uint64_t address) const;
    void reserve_vma_slot();
    void insert_vma(const Vma& vma);
    void erase_vma(size_t index);
    void split_at(uint64_t address);
    void remove_range(uint64_t start, uint64_t end);
    uint64_t* walk(uint64_t address, bool create);
    void release_pages(uint64_t start, uint64_t end);
    uint64_t find_free_range(uint64_t length) const;
    bool range_is_free(uint64_t start, uint64_t end) const;
    void free_tables();
};

bool user_range_ok(const void* pointer, uint64_t length, bool write);
bool copy_from_user(void* destination, const void* source, uint64_t length);
bool copy_to_user(void* destination, const void* source, uint64_t length);
int copy_string_from_user(char* destination, const char* source, size_t capacity);

}
