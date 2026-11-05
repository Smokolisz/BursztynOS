#include "mm.h"

#include "errno.h"
#include "process.h"

namespace lx {

namespace {

constexpr uint64_t kPtePresent = 1ULL << 0;
constexpr uint64_t kPteWrite = 1ULL << 1;
constexpr uint64_t kPteUser = 1ULL << 2;
constexpr uint64_t kPteBorrowed = 1ULL << 9;
constexpr uint64_t kPteAddressMask = 0x000FFFFFFFFFF000ULL;
constexpr uint64_t kTableFlags = kPtePresent | kPteWrite | kPteUser;

constexpr uint64_t kUserFloor = 0x400000ULL;
constexpr uint64_t kKernelHoleStart = 0x40000000ULL;
constexpr uint64_t kKernelHoleEnd = 0x100000000ULL;
constexpr uint64_t kUserCeiling = 0x00007FFFFFFFF000ULL;
constexpr uint64_t kMmapBase = 0x00007FFFF0000000ULL;
constexpr int kKernelImagePdEntries = 2;
constexpr int kDirectMapPml4Index = 256;
constexpr int kSharedKernelPdptEntries = 4;
constexpr uint64_t kMremapMayMove = 1;
constexpr uint64_t kMremapFixed = 2;

uint64_t* table_at(uint64_t phys) { return static_cast<uint64_t*>(phys_to_virt(phys)); }

bool in_user_window(uint64_t start, uint64_t end) {
    if (start < kUserFloor || end > kUserCeiling || start >= end) return false;
    return end <= kKernelHoleStart || start >= kKernelHoleEnd;
}

uint64_t pte_flags_for(uint32_t prot) {
    if (!(prot & (kProtRead | kProtWrite | kProtExec))) return 0;
    return kPtePresent | kPteUser | ((prot & kProtWrite) ? kPteWrite : 0);
}

void reload_tlb_if_active(uint64_t cr3) {
    if (read_cr3() == cr3) write_cr3(cr3);
}

}

AddressSpace* AddressSpace::create(uint64_t kernel_pml4_phys) {
    auto* space = kernel_new<AddressSpace>();
    space->pml4_phys_ = alloc_frame_zeroed();
    const uint64_t pdpt_phys = alloc_frame_zeroed();
    const uint64_t pd_phys = alloc_frame_zeroed();
    uint64_t* pml4 = table_at(space->pml4_phys_);
    uint64_t* pdpt = table_at(pdpt_phys);
    uint64_t* pd = table_at(pd_phys);
    const uint64_t* kernel_pml4 = table_at(kernel_pml4_phys);
    const uint64_t* kernel_pdpt = table_at(kernel_pml4[0] & kPteAddressMask);
    const uint64_t* kernel_pd = table_at(kernel_pdpt[0] & kPteAddressMask);

    pml4[0] = pdpt_phys | kTableFlags;
    pml4[kDirectMapPml4Index] = kernel_pml4[kDirectMapPml4Index];
    pdpt[0] = pd_phys | kTableFlags;
    for (int i = 1; i < kSharedKernelPdptEntries; ++i) pdpt[i] = kernel_pdpt[i];
    for (int i = 0; i < kKernelImagePdEntries; ++i) pd[i] = kernel_pd[i];

    space->vma_capacity_ = 256;
    space->vmas_ = static_cast<Vma*>(kernel_alloc(space->vma_capacity_ * sizeof(Vma)));
    return space;
}

void AddressSpace::free_tables() {
    uint64_t* pml4 = table_at(pml4_phys_);
    for (int l4 = 0; l4 < kDirectMapPml4Index; ++l4) {
        if (!(pml4[l4] & kPtePresent)) continue;
        uint64_t* pdpt = table_at(pml4[l4] & kPteAddressMask);
        for (int l3 = 0; l3 < 512; ++l3) {
            if (l4 == 0 && l3 >= 1 && l3 < kSharedKernelPdptEntries) continue;
            if (!(pdpt[l3] & kPtePresent)) continue;
            uint64_t* pd = table_at(pdpt[l3] & kPteAddressMask);
            for (int l2 = 0; l2 < 512; ++l2) {
                if (l4 == 0 && l3 == 0 && l2 < kKernelImagePdEntries) continue;
                if (pd[l2] & kPtePresent) free_frame(pd[l2] & kPteAddressMask);
            }
            free_frame(pdpt[l3] & kPteAddressMask);
        }
        free_frame(pml4[l4] & kPteAddressMask);
    }
    free_frame(pml4_phys_);
}

void AddressSpace::destroy() {
    for (size_t i = 0; i < vma_count_; ++i) {
        release_pages(vmas_[i].start, vmas_[i].end);
        if (vmas_[i].node) node_release(vmas_[i].node);
    }
    free_tables();
    kernel_free(vmas_);
    kernel_free(this);
}

size_t AddressSpace::lower_bound(uint64_t address) const {
    size_t low = 0;
    size_t high = vma_count_;
    while (low < high) {
        const size_t middle = (low + high) / 2;
        if (vmas_[middle].end <= address) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return low;
}

const Vma* AddressSpace::find(uint64_t address) const {
    const size_t index = lower_bound(address);
    if (index < vma_count_ && vmas_[index].start <= address) return &vmas_[index];
    return nullptr;
}

void AddressSpace::reserve_vma_slot() {
    if (vma_count_ < vma_capacity_) return;
    const size_t capacity = vma_capacity_ * 2;
    auto* grown = static_cast<Vma*>(kernel_alloc(capacity * sizeof(Vma)));
    memcpy(grown, vmas_, vma_count_ * sizeof(Vma));
    kernel_free(vmas_);
    vmas_ = grown;
    vma_capacity_ = capacity;
}

void AddressSpace::insert_vma(const Vma& vma) {
    size_t index = lower_bound(vma.start);
    if (index > 0) {
        Vma& previous = vmas_[index - 1];
        const bool adjacent_anonymous =
            previous.end == vma.start && !previous.node && !vma.node && previous.prot == vma.prot && previous.flags == vma.flags;
        if (adjacent_anonymous) {
            previous.end = vma.end;
            return;
        }
    }
    reserve_vma_slot();
    memmove(&vmas_[index + 1], &vmas_[index], (vma_count_ - index) * sizeof(Vma));
    vmas_[index] = vma;
    ++vma_count_;
}

void AddressSpace::erase_vma(size_t index) {
    memmove(&vmas_[index], &vmas_[index + 1], (vma_count_ - index - 1) * sizeof(Vma));
    --vma_count_;
}

void AddressSpace::split_at(uint64_t address) {
    const size_t index = lower_bound(address);
    if (index >= vma_count_ || vmas_[index].start >= address) return;
    Vma upper = vmas_[index];
    upper.start = address;
    if (upper.node) {
        upper.offset += address - vmas_[index].start;
        node_retain(upper.node);
    }
    vmas_[index].end = address;
    reserve_vma_slot();
    memmove(&vmas_[index + 2], &vmas_[index + 1], (vma_count_ - index - 1) * sizeof(Vma));
    vmas_[index + 1] = upper;
    ++vma_count_;
}

uint64_t* AddressSpace::walk(uint64_t address, bool create) {
    uint64_t* table = table_at(pml4_phys_);
    for (int shift = 39; shift > 12; shift -= 9) {
        uint64_t& entry = table[(address >> shift) & 0x1FF];
        if (!(entry & kPtePresent)) {
            if (!create) return nullptr;
            const uint64_t frame = alloc_frame_zeroed();
            if (!frame) return nullptr;
            entry = frame | kTableFlags;
        }
        table = table_at(entry & kPteAddressMask);
    }
    return &table[(address >> 12) & 0x1FF];
}

void AddressSpace::release_pages(uint64_t start, uint64_t end) {
    constexpr uint64_t kLargeRangePages = 64;
    const bool flush_all = (end - start) / kPageSize > kLargeRangePages;
    const bool active = read_cr3() == pml4_phys_;
    uint64_t address = start;
    while (address < end) {
        uint64_t* pte = walk(address, false);
        if (!pte) {
            constexpr uint64_t kPtCoverage = 512 * kPageSize;
            address = (address + kPtCoverage) & ~(kPtCoverage - 1);
            continue;
        }
        const uint64_t entry = *pte;
        if (entry) {
            if (!(entry & kPteBorrowed)) free_frame(entry & kPteAddressMask);
            *pte = 0;
            if (active && !flush_all) flush_tlb_page(address);
        }
        address += kPageSize;
    }
    if (flush_all) reload_tlb_if_active(pml4_phys_);
}

void AddressSpace::remove_range(uint64_t start, uint64_t end) {
    split_at(start);
    split_at(end);
    size_t index = lower_bound(start);
    while (index < vma_count_ && vmas_[index].start < end) {
        release_pages(vmas_[index].start, vmas_[index].end);
        if (vmas_[index].node) node_release(vmas_[index].node);
        erase_vma(index);
    }
}

bool AddressSpace::range_is_free(uint64_t start, uint64_t end) const {
    const size_t index = lower_bound(start);
    return index >= vma_count_ || vmas_[index].start >= end;
}

uint64_t AddressSpace::find_free_range(uint64_t length) const {
    uint64_t ceiling = kMmapBase;
    for (size_t index = vma_count_; index > 0; --index) {
        const Vma& vma = vmas_[index - 1];
        if (vma.start >= ceiling) continue;
        if (vma.end <= ceiling && ceiling - vma.end >= length) return ceiling - length;
        ceiling = min(ceiling, vma.start);
        if (ceiling < kKernelHoleEnd + length) return 0;
    }
    return ceiling >= kKernelHoleEnd + length ? ceiling - length : 0;
}

int64_t AddressSpace::map(uint64_t hint, uint64_t length, uint32_t prot, uint32_t flags, Node* node, uint64_t offset) {
    if (!length || (offset & kPageMask)) return -kEINVAL;
    length = page_ceil(length);
    uint64_t start;
    if (flags & (kMapFixed | kMapFixedNoReplace)) {
        if (hint & kPageMask) return -kEINVAL;
        start = hint;
        if (!in_user_window(start, start + length)) return -kENOMEM;
        if ((flags & kMapFixedNoReplace) && !range_is_free(start, start + length)) return -kEEXIST;
        remove_range(start, start + length);
    } else {
        start = page_floor(hint);
        if (!start || !in_user_window(start, start + length) || !range_is_free(start, start + length)) {
            start = find_free_range(length);
            if (!start) return -kENOMEM;
        }
    }
    if (node) {
        if ((flags & kMapShared) && (prot & kProtWrite) && node->kind == NodeKind::kFile) {
            const int status = node_make_writable(node);
            if (status < 0) return status;
        }
        node_retain(node);
    }
    insert_vma(Vma{start, start + length, prot, flags & (kMapShared | kMapPrivate | kMapAnonymous), node, offset});
    return static_cast<int64_t>(start);
}

int AddressSpace::unmap(uint64_t start, uint64_t length) {
    if ((start & kPageMask) || !length) return -kEINVAL;
    remove_range(start, start + page_ceil(length));
    return 0;
}

int AddressSpace::protect(uint64_t start, uint64_t length, uint32_t prot) {
    if (start & kPageMask) return -kEINVAL;
    const uint64_t end = start + page_ceil(length);
    split_at(start);
    split_at(end);
    uint64_t cursor = start;
    for (size_t index = lower_bound(start); index < vma_count_ && vmas_[index].start < end; ++index) {
        Vma& vma = vmas_[index];
        if (vma.start > cursor) return -kENOMEM;
        vma.prot = prot;
        cursor = vma.end;
        const uint64_t base_flags = pte_flags_for(prot);
        for (uint64_t address = vma.start; address < vma.end;) {
            uint64_t* pte = walk(address, false);
            if (!pte) {
                constexpr uint64_t kPtCoverage = 512 * kPageSize;
                address = (address + kPtCoverage) & ~(kPtCoverage - 1);
                continue;
            }
            uint64_t entry = *pte;
            if (entry & (kPtePresent | kPteBorrowed)) {
                const bool borrowed = entry & kPteBorrowed;
                uint64_t flags = base_flags;
                if (borrowed && !(vma.flags & kMapShared)) flags &= ~kPteWrite;
                const uint64_t phys = entry & kPteAddressMask;
                if (flags) {
                    *pte = phys | flags | (borrowed ? kPteBorrowed : 0);
                } else if (borrowed) {
                    *pte = 0;
                } else {
                    *pte = phys | kPteUser;
                }
                flush_tlb_page(address);
            }
            address += kPageSize;
        }
    }
    return cursor >= end ? 0 : -kENOMEM;
}

int64_t AddressSpace::remap(uint64_t old_start, uint64_t old_length, uint64_t new_length, uint32_t flags,
                            uint64_t new_address) {
    if ((old_start & kPageMask) || !new_length) return -kEINVAL;
    old_length = page_ceil(old_length);
    new_length = page_ceil(new_length);
    const Vma* source = find(old_start);
    if (!source || source->end < old_start + old_length) return -kEINVAL;

    if (!(flags & kMremapFixed)) {
        if (new_length <= old_length) {
            if (new_length < old_length) remove_range(old_start + new_length, old_start + old_length);
            return static_cast<int64_t>(old_start);
        }
        const uint64_t grown_end = old_start + new_length;
        if (source->end == old_start + old_length && in_user_window(old_start, grown_end) &&
            range_is_free(source->end, grown_end) && !source->node) {
            const size_t index = lower_bound(old_start);
            vmas_[index].end = grown_end;
            return static_cast<int64_t>(old_start);
        }
        if (!(flags & kMremapMayMove)) return -kENOMEM;
    }

    Vma moved = *source;
    uint64_t destination;
    if (flags & kMremapFixed) {
        destination = new_address;
        if (!in_user_window(destination, destination + new_length)) return -kEINVAL;
        remove_range(destination, destination + new_length);
    } else {
        destination = find_free_range(new_length);
        if (!destination) return -kENOMEM;
    }
    const uint64_t move_length = min(old_length, new_length);
    const uint64_t source_offset = moved.node ? moved.offset + (old_start - moved.start) : 0;
    for (uint64_t delta = 0; delta < move_length; delta += kPageSize) {
        uint64_t* from = walk(old_start + delta, false);
        if (!from || !*from) continue;
        uint64_t* to = walk(destination + delta, true);
        if (!to) return -kENOMEM;
        *to = *from;
        *from = 0;
    }
    reload_tlb_if_active(pml4_phys_);
    if (moved.node) node_retain(moved.node);
    insert_vma(Vma{destination, destination + new_length, moved.prot, moved.flags, moved.node, source_offset});
    remove_range(old_start, old_start + old_length);
    return static_cast<int64_t>(destination);
}

int AddressSpace::discard_pages(uint64_t start, uint64_t length) {
    const uint64_t end = start + page_ceil(length);
    for (size_t index = lower_bound(start); index < vma_count_ && vmas_[index].start < end; ++index) {
        const Vma& vma = vmas_[index];
        if (vma.flags & kMapShared) continue;
        release_pages(max(vma.start, start), min(vma.end, end));
    }
    return 0;
}

int AddressSpace::sync(uint64_t start, uint64_t length) {
    const uint64_t end = start + length;
    for (size_t index = lower_bound(start); index < vma_count_ && vmas_[index].start < end; ++index) {
        const Vma& vma = vmas_[index];
        if (!vma.node || !vma.node->device || !vma.node->device->sync) continue;
        const uint64_t from = max(vma.start, start);
        const uint64_t to = min(vma.end, end);
        vma.node->device->sync(vma.offset + (from - vma.start), to - from);
    }
    return 0;
}

void AddressSpace::init_break(uint64_t start) {
    break_start_ = page_ceil(start);
    break_end_ = break_start_;
}

uint64_t AddressSpace::set_break(uint64_t requested) {
    if (requested < break_start_) return break_end_;
    const uint64_t new_end = page_ceil(requested);
    const uint64_t old_end = page_ceil(break_end_);
    if (new_end > old_end) {
        if (new_end > kKernelHoleStart || !range_is_free(old_end, new_end)) return break_end_;
        insert_vma(Vma{old_end, new_end, kProtRead | kProtWrite, kMapPrivate | kMapAnonymous, nullptr, 0});
    } else if (new_end < old_end) {
        remove_range(new_end, old_end);
    }
    break_end_ = requested;
    return break_end_;
}

bool AddressSpace::handle_fault(uint64_t address, bool write) {
    const Vma* vma = find(address);
    if (!vma) return false;
    if (write && !(vma->prot & kProtWrite)) return false;
    if (!write && !(vma->prot & (kProtRead | kProtExec))) return false;

    const uint64_t page = page_floor(address);
    uint64_t* pte = walk(page, true);
    if (!pte) return false;
    const uint64_t entry = *pte;
    const uint64_t flags = pte_flags_for(vma->prot);

    if (entry & kPtePresent) {
        if (!write || (entry & kPteWrite)) {
            flush_tlb_page(page);
            return true;
        }
        if (entry & kPteBorrowed) {
            const uint64_t frame = alloc_frame();
            if (!frame) return false;
            memcpy(phys_to_virt(frame), phys_to_virt(entry & kPteAddressMask), kPageSize);
            *pte = frame | flags;
        } else {
            *pte = (entry & kPteAddressMask) | flags;
        }
        flush_tlb_page(page);
        return true;
    }

    if (entry & kPteUser) {
        *pte = (entry & kPteAddressMask) | flags;
        flush_tlb_page(page);
        return true;
    }

    uint64_t source = 0;
    if (vma->node) {
        const uint64_t file_offset = vma->offset + (page - vma->start);
        const bool allocate = vma->flags & kMapShared;
        source = node_page_phys(vma->node, file_offset / kPageSize, allocate);
    }

    if (source && (vma->flags & kMapShared)) {
        *pte = source | flags | kPteBorrowed;
    } else if (source && !write) {
        *pte = source | (flags & ~kPteWrite) | kPteBorrowed;
    } else {
        const uint64_t frame = source ? alloc_frame() : alloc_frame_zeroed();
        if (!frame) return false;
        if (source) memcpy(phys_to_virt(frame), phys_to_virt(source), kPageSize);
        *pte = frame | flags;
    }
    flush_tlb_page(page);
    return true;
}

bool AddressSpace::range_accessible(uint64_t start, uint64_t length, bool write) const {
    if (!length) return true;
    const uint64_t end = start + length;
    if (end < start) return false;
    uint64_t cursor = start;
    for (size_t index = lower_bound(start); index < vma_count_ && cursor < end; ++index) {
        const Vma& vma = vmas_[index];
        if (vma.start > cursor) return false;
        if (write ? !(vma.prot & kProtWrite) : !(vma.prot & (kProtRead | kProtExec))) return false;
        cursor = vma.end;
    }
    return cursor >= end;
}

bool user_range_ok(const void* pointer, uint64_t length, bool write) {
    Process* process = current()->process;
    return process && process->address_space->range_accessible(reinterpret_cast<uint64_t>(pointer), length, write);
}

bool copy_from_user(void* destination, const void* source, uint64_t length) {
    if (!user_range_ok(source, length, false)) return false;
    memcpy(destination, source, length);
    return true;
}

bool copy_to_user(void* destination, const void* source, uint64_t length) {
    if (!user_range_ok(destination, length, true)) return false;
    memcpy(destination, source, length);
    return true;
}

int copy_string_from_user(char* destination, const char* source, size_t capacity) {
    for (size_t i = 0; i < capacity; ++i) {
        if (!user_range_ok(source + i, 1, false)) return -kEFAULT;
        destination[i] = source[i];
        if (!destination[i]) return static_cast<int>(i);
    }
    return -kENAMETOOLONG;
}

}
