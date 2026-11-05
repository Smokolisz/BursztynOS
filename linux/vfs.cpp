#include "vfs.h"

#include "errno.h"
#include "mm.h"

namespace lx {

WaitQueue poll_waiters;

namespace {

constexpr int kMaxSymlinkDepth = 16;
constexpr char kImageMagic[8] = {'B', 'R', 'S', 'Z', 'R', 'F', 'S', '1'};

struct ImageHeader {
    char magic[8];
    uint64_t entry_count;
};

struct ImageEntry {
    uint64_t path_offset;
    uint64_t data_offset;
    uint64_t size;
    uint32_t mode;
    uint32_t reserved;
};

Node* root_node;
bool image_mounted;
uint64_t next_inode = 1;

Node* allocate_node(const char* name, size_t name_length, NodeKind kind, uint32_t mode) {
    auto* node = kernel_new<Node>();
    node->name = string_duplicate_n(name, name_length);
    node->kind = kind;
    node->mode = mode;
    node->links = 1;
    node->inode = next_inode++;
    node->modified_ns = wall_clock_ns();
    return node;
}

void attach_child(Node* directory, Node* child) {
    child->parent = directory;
    child->next_sibling = directory->first_child;
    directory->first_child = child;
}

void detach_child(Node* child) {
    Node* parent = child->parent;
    if (!parent) return;
    for (Node** link = &parent->first_child; *link; link = &(*link)->next_sibling) {
        if (*link == child) {
            *link = child->next_sibling;
            break;
        }
    }
    child->parent = nullptr;
    child->next_sibling = nullptr;
}

void destroy_node(Node* node) {
    if (node->pages) {
        for (uint64_t i = 0; i < node->page_capacity; ++i) {
            if (node->pages[i]) free_frame(node->pages[i]);
        }
        kernel_free(node->pages);
    }
    kernel_free(node->link_target);
    kernel_free(node->name);
    kernel_free(node);
}

Node* ensure_directory(Node* base, const char* path, size_t length) {
    Node* directory = base;
    size_t position = 0;
    while (position < length) {
        while (position < length && path[position] == '/') ++position;
        size_t end = position;
        while (end < length && path[end] != '/') ++end;
        if (end == position) break;
        Node* next = vfs_lookup_child(directory, path + position, end - position);
        if (!next) next = vfs_create_child(directory, path + position, end - position, NodeKind::kDirectory, 0755);
        directory = next;
        position = end;
    }
    return directory;
}

int resolve_at(Node* base, const char* path, bool follow_last, int depth, Node** result) {
    if (depth > kMaxSymlinkDepth) return -kELOOP;
    if (!*path) return -kENOENT;
    Node* node = path[0] == '/' ? root_node : base;
    const char* cursor = path;
    for (;;) {
        while (*cursor == '/') ++cursor;
        if (!*cursor) break;
        const char* end = cursor;
        while (*end && *end != '/') ++end;
        const size_t length = end - cursor;
        if (node->kind != NodeKind::kDirectory) return -kENOTDIR;
        Node* next;
        if (length == 1 && cursor[0] == '.') {
            next = node;
        } else if (length == 2 && cursor[0] == '.' && cursor[1] == '.') {
            next = node->parent ? node->parent : node;
        } else {
            next = vfs_lookup_child(node, cursor, length);
            if (!next) return -kENOENT;
        }
        const char* rest = end;
        while (*rest == '/') ++rest;
        if (next->kind == NodeKind::kSymlink && (*rest || follow_last)) {
            Node* target;
            const int status = resolve_at(node, next->link_target, true, depth + 1, &target);
            if (status < 0) return status;
            next = target;
        }
        node = next;
        cursor = end;
    }
    *result = node;
    return 0;
}

int64_t read_pipe(File* file, void* buffer, uint64_t length);
int64_t write_pipe(File* file, const void* buffer, uint64_t length);
uint32_t poll_pipe(File* file);
void release_pipe(File* file);

const DeviceOps kPipeOps = {read_pipe, write_pipe, nullptr, poll_pipe, nullptr, nullptr, release_pipe};

constexpr uint64_t kPipeCapacity = 65536;

struct Pipe {
    uint8_t* data;
    uint64_t read_position;
    uint64_t used;
    uint32_t readers;
    uint32_t writers;
    WaitQueue readable;
    WaitQueue writable;
};

Pipe* pipe_of(File* file) { return static_cast<Pipe*>(file->node->device_data); }

int64_t read_pipe(File* file, void* buffer, uint64_t length) {
    Pipe* pipe = pipe_of(file);
    while (!pipe->used) {
        if (!pipe->writers) return 0;
        if (file->flags & kOpenNonBlocking) return -kEAGAIN;
        const int status = block_on(&pipe->readable, 0);
        if (status < 0) return status;
    }
    const uint64_t count = min(length, pipe->used);
    auto* out = static_cast<uint8_t*>(buffer);
    for (uint64_t i = 0; i < count; ++i) {
        out[i] = pipe->data[(pipe->read_position + i) % kPipeCapacity];
    }
    pipe->read_position = (pipe->read_position + count) % kPipeCapacity;
    pipe->used -= count;
    wake_all(&pipe->writable);
    poll_notify();
    return static_cast<int64_t>(count);
}

int64_t write_pipe(File* file, const void* buffer, uint64_t length) {
    Pipe* pipe = pipe_of(file);
    auto* in = static_cast<const uint8_t*>(buffer);
    uint64_t written = 0;
    while (written < length) {
        if (!pipe->readers) return written ? static_cast<int64_t>(written) : -kEPIPE;
        if (pipe->used == kPipeCapacity) {
            if (file->flags & kOpenNonBlocking) return written ? static_cast<int64_t>(written) : -kEAGAIN;
            const int status = block_on(&pipe->writable, 0);
            if (status < 0) return written ? static_cast<int64_t>(written) : status;
            continue;
        }
        const uint64_t count = min(length - written, kPipeCapacity - pipe->used);
        for (uint64_t i = 0; i < count; ++i) {
            pipe->data[(pipe->read_position + pipe->used + i) % kPipeCapacity] = in[written + i];
        }
        pipe->used += count;
        written += count;
        wake_all(&pipe->readable);
        poll_notify();
    }
    return static_cast<int64_t>(written);
}

uint32_t poll_pipe(File* file) {
    Pipe* pipe = pipe_of(file);
    uint32_t events = 0;
    if ((file->flags & kOpenAccessMask) == 0) {
        if (pipe->used) events |= kPollIn;
        if (!pipe->writers) events |= kPollHangup;
    } else {
        if (pipe->used < kPipeCapacity) events |= kPollOut;
        if (!pipe->readers) events |= kPollErr;
    }
    return events;
}

void release_pipe(File* file) {
    Pipe* pipe = pipe_of(file);
    if ((file->flags & kOpenAccessMask) == 0) {
        --pipe->readers;
    } else {
        --pipe->writers;
    }
    wake_all(&pipe->readable);
    wake_all(&pipe->writable);
    poll_notify();
    if (!pipe->readers && !pipe->writers) {
        kernel_free(pipe->data);
        kernel_free(pipe);
    }
}

}

bool vfs_mount_image(uint64_t phys_start, uint64_t phys_end) {
    auto* base = static_cast<const uint8_t*>(phys_to_virt(phys_start));
    auto* header = reinterpret_cast<const ImageHeader*>(base);
    if (phys_end - phys_start < sizeof(ImageHeader) || memcmp(header->magic, kImageMagic, sizeof(kImageMagic))) {
        return false;
    }
    auto* entries = reinterpret_cast<const ImageEntry*>(header + 1);
    for (uint64_t i = 0; i < header->entry_count; ++i) {
        const ImageEntry& entry = entries[i];
        const char* path = reinterpret_cast<const char*>(base + entry.path_offset);
        size_t split = string_length(path);
        const size_t path_length = split;
        while (split && path[split - 1] != '/') --split;
        Node* directory = ensure_directory(root_node, path, split);
        const char* leaf = path + split;
        const size_t leaf_length = path_length - split;
        const uint32_t type = entry.mode & kModeTypeMask;
        const uint32_t permissions = entry.mode & 07777;
        if (type == kModeDirectory) {
            ensure_directory(directory, leaf, leaf_length);
        } else if (type == kModeSymlink) {
            Node* node = vfs_create_child(directory, leaf, leaf_length, NodeKind::kSymlink, 0777);
            node->link_target = string_duplicate_n(reinterpret_cast<const char*>(base + entry.data_offset), entry.size);
            node->size = entry.size;
        } else {
            Node* node = vfs_create_child(directory, leaf, leaf_length, NodeKind::kFile, permissions);
            node->image_phys = phys_start + entry.data_offset;
            node->size = entry.size;
        }
    }
    image_mounted = true;
    log("rootfs: mounted %lu entries, %lu MiB\n", header->entry_count, (phys_end - phys_start) >> 20);
    return true;
}

bool vfs_has_image() { return image_mounted; }

Node* vfs_root() {
    if (!root_node) {
        root_node = allocate_node("", 0, NodeKind::kDirectory, 0755);
        root_node->references = 1;
    }
    return root_node;
}

Node* vfs_lookup_child(Node* directory, const char* name, size_t length) {
    for (Node* child = directory->first_child; child; child = child->next_sibling) {
        if (!string_compare_n(child->name, name, length) && !child->name[length]) return child;
    }
    return nullptr;
}

Node* vfs_create_child(Node* directory, const char* name, size_t length, NodeKind kind, uint32_t mode) {
    Node* node = allocate_node(name, length, kind, mode);
    attach_child(directory, node);
    return node;
}

int vfs_resolve(Node* base, const char* path, bool follow_last, Node** result) {
    return resolve_at(base, path, follow_last, 0, result);
}

int vfs_resolve_parent(Node* base, const char* path, Node** parent, const char** leaf, size_t* leaf_length) {
    size_t end = string_length(path);
    while (end > 1 && path[end - 1] == '/') --end;
    size_t split = end;
    while (split && path[split - 1] != '/') --split;
    *leaf = path + split;
    *leaf_length = end - split;
    if (!split) {
        *parent = base;
        return 0;
    }
    char* prefix = string_duplicate_n(path, split);
    const int status = resolve_at(base, prefix, true, 0, parent);
    kernel_free(prefix);
    if (status < 0) return status;
    return (*parent)->kind == NodeKind::kDirectory ? 0 : -kENOTDIR;
}

int vfs_unlink(Node* node) {
    if (node->kind == NodeKind::kDirectory && node->first_child) return -kENOTEMPTY;
    detach_child(node);
    node->links = 0;
    if (!node->references) destroy_node(node);
    return 0;
}

int vfs_rename(Node* node, Node* new_parent, const char* name, size_t length) {
    Node* existing = vfs_lookup_child(new_parent, name, length);
    if (existing == node) return 0;
    if (existing) {
        const int status = vfs_unlink(existing);
        if (status < 0) return status;
    }
    detach_child(node);
    kernel_free(node->name);
    node->name = string_duplicate_n(name, length);
    attach_child(new_parent, node);
    return 0;
}

Node* vfs_register_device(const char* path, const DeviceOps* ops, uint64_t size, uint32_t mode) {
    size_t split = string_length(path);
    const size_t length = split;
    while (split && path[split - 1] != '/') --split;
    Node* directory = ensure_directory(vfs_root(), path, split);
    Node* node = vfs_create_child(directory, path + split, length - split, NodeKind::kDevice, mode);
    node->device = ops;
    node->device_size = size;
    return node;
}

size_t vfs_path_of(Node* node, char* buffer, size_t capacity) {
    if (node == root_node || !node->parent) {
        if (capacity < 2) return 0;
        buffer[0] = '/';
        buffer[1] = 0;
        return 1;
    }
    const size_t prefix = vfs_path_of(node->parent, buffer, capacity);
    size_t length = (prefix == 1 && buffer[0] == '/') ? 0 : prefix;
    const size_t name_length = string_length(node->name);
    if (length + name_length + 2 > capacity) return 0;
    buffer[length++] = '/';
    memcpy(buffer + length, node->name, name_length);
    length += name_length;
    buffer[length] = 0;
    return length;
}

void node_retain(Node* node) { ++node->references; }

void node_release(Node* node) {
    if (--node->references == 0 && node->links == 0) destroy_node(node);
}

int node_make_writable(Node* node) {
    if (!node->image_phys) return 0;
    const uint64_t page_count = page_ceil(node->size) / kPageSize;
    const uint64_t capacity = max<uint64_t>(page_count, 16);
    node->pages = static_cast<uint64_t*>(kernel_alloc_zeroed(capacity * sizeof(uint64_t)));
    node->page_capacity = capacity;
    for (uint64_t i = 0; i < page_count; ++i) {
        const uint64_t frame = alloc_frame();
        if (!frame) return -kENOMEM;
        memcpy(phys_to_virt(frame), phys_to_virt(node->image_phys + i * kPageSize), kPageSize);
        node->pages[i] = frame;
    }
    node->image_phys = 0;
    return 0;
}

uint64_t node_page_phys(Node* node, uint64_t page_index, bool allocate) {
    if (node->kind == NodeKind::kDevice) {
        return node->device->page_phys ? node->device->page_phys(page_index * kPageSize) : 0;
    }
    if (node->image_phys) {
        return page_index * kPageSize < page_ceil(node->size) ? node->image_phys + page_index * kPageSize : 0;
    }
    if (page_index >= node->page_capacity) {
        if (!allocate) return 0;
        const uint64_t capacity = max<uint64_t>(page_index + 1, max<uint64_t>(node->page_capacity * 2, 16));
        auto* pages = static_cast<uint64_t*>(kernel_alloc_zeroed(capacity * sizeof(uint64_t)));
        if (node->pages) {
            memcpy(pages, node->pages, node->page_capacity * sizeof(uint64_t));
            kernel_free(node->pages);
        }
        node->pages = pages;
        node->page_capacity = capacity;
    }
    if (!node->pages[page_index] && allocate) node->pages[page_index] = alloc_frame_zeroed();
    return node->pages[page_index];
}

int node_truncate(Node* node, uint64_t size) {
    if (node->kind != NodeKind::kFile) return -kEINVAL;
    const int status = node_make_writable(node);
    if (status < 0) return status;
    if (size < node->size && node->pages) {
        const uint64_t keep = page_ceil(size) / kPageSize;
        for (uint64_t i = keep; i < node->page_capacity; ++i) {
            if (node->pages[i]) {
                free_frame(node->pages[i]);
                node->pages[i] = 0;
            }
        }
        const uint64_t tail = size & kPageMask;
        if (tail && keep && node->pages[keep - 1]) {
            memset(static_cast<uint8_t*>(phys_to_virt(node->pages[keep - 1])) + tail, 0, kPageSize - tail);
        }
    }
    node->size = size;
    node->modified_ns = wall_clock_ns();
    return 0;
}

int64_t node_read(Node* node, void* buffer, uint64_t length, uint64_t offset) {
    if (node->kind == NodeKind::kDirectory) return -kEISDIR;
    if (offset >= node->size) return 0;
    const uint64_t total = min(length, node->size - offset);
    auto* out = static_cast<uint8_t*>(buffer);
    if (node->image_phys) {
        memcpy(out, static_cast<const uint8_t*>(phys_to_virt(node->image_phys)) + offset, total);
        return static_cast<int64_t>(total);
    }
    uint64_t done = 0;
    while (done < total) {
        const uint64_t position = offset + done;
        const uint64_t in_page = position & kPageMask;
        const uint64_t count = min(total - done, kPageSize - in_page);
        const uint64_t frame = node_page_phys(node, position / kPageSize, false);
        if (frame) {
            memcpy(out + done, static_cast<const uint8_t*>(phys_to_virt(frame)) + in_page, count);
        } else {
            memset(out + done, 0, count);
        }
        done += count;
    }
    return static_cast<int64_t>(total);
}

int64_t node_write(Node* node, const void* buffer, uint64_t length, uint64_t offset) {
    if (node->kind != NodeKind::kFile) return -kEINVAL;
    const int status = node_make_writable(node);
    if (status < 0) return status;
    auto* in = static_cast<const uint8_t*>(buffer);
    uint64_t done = 0;
    while (done < length) {
        const uint64_t position = offset + done;
        const uint64_t in_page = position & kPageMask;
        const uint64_t count = min(length - done, kPageSize - in_page);
        const uint64_t frame = node_page_phys(node, position / kPageSize, true);
        if (!frame) return done ? static_cast<int64_t>(done) : -kENOSPC;
        memcpy(static_cast<uint8_t*>(phys_to_virt(frame)) + in_page, in + done, count);
        done += count;
    }
    if (offset + length > node->size) node->size = offset + length;
    node->modified_ns = wall_clock_ns();
    return static_cast<int64_t>(length);
}

File* file_open(Node* node, uint32_t flags) {
    auto* file = kernel_new<File>();
    file->node = node;
    file->flags = flags;
    file->references = 1;
    node_retain(node);
    return file;
}

File* file_retain(File* file) {
    ++file->references;
    return file;
}

void file_release(File* file) {
    if (--file->references) return;
    if (file->node->device && file->node->device->release) file->node->device->release(file);
    node_release(file->node);
    kernel_free(file);
}

int64_t file_read(File* file, void* buffer, uint64_t length) {
    Node* node = file->node;
    if (node->device) {
        return node->device->read ? node->device->read(file, buffer, length) : -kEINVAL;
    }
    const int64_t count = node_read(node, buffer, length, file->offset);
    if (count > 0) file->offset += count;
    return count;
}

int64_t file_write(File* file, const void* buffer, uint64_t length) {
    Node* node = file->node;
    if (node->device) {
        return node->device->write ? node->device->write(file, buffer, length) : -kEINVAL;
    }
    if (file->flags & kOpenAppend) file->offset = node->size;
    const int64_t count = node_write(node, buffer, length, file->offset);
    if (count > 0) file->offset += count;
    return count;
}

uint32_t file_poll(File* file) {
    Node* node = file->node;
    if (node->device && node->device->poll) return node->device->poll(file);
    return kPollIn | kPollOut;
}

int pipe_create(File** read_end, File** write_end) {
    auto* pipe = kernel_new<Pipe>();
    pipe->data = static_cast<uint8_t*>(kernel_alloc(kPipeCapacity));
    pipe->readers = 1;
    pipe->writers = 1;
    auto* node = allocate_node("pipe", 4, NodeKind::kDevice, kModeFifo | 0600);
    node->links = 0;
    node->device = &kPipeOps;
    node->device_data = pipe;
    *read_end = file_open(node, 0);
    *write_end = file_open(node, kOpenWriteOnly);
    return 0;
}

void poll_notify() { wake_all(&poll_waiters); }

}
