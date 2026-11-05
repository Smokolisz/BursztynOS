#pragma once

#include "base.h"
#include "sched.h"

namespace lx {

enum class NodeKind : uint8_t { kDirectory, kFile, kSymlink, kDevice };

struct File;

struct DeviceOps {
    int64_t (*read)(File* file, void* buffer, uint64_t length);
    int64_t (*write)(File* file, const void* buffer, uint64_t length);
    int64_t (*ioctl)(File* file, uint64_t command, uint64_t argument);
    uint32_t (*poll)(File* file);
    uint64_t (*page_phys)(uint64_t offset);
    void (*sync)(uint64_t offset, uint64_t length);
    void (*release)(File* file);
};

struct Node {
    Node* parent;
    Node* first_child;
    Node* next_sibling;
    char* name;
    NodeKind kind;
    uint32_t mode;
    uint32_t references;
    uint32_t links;
    uint64_t inode;
    uint64_t size;
    uint64_t modified_ns;
    uint64_t image_phys;
    uint64_t* pages;
    uint64_t page_capacity;
    char* link_target;
    const DeviceOps* device;
    uint64_t device_size;
    void* device_data;
};

struct File {
    Node* node;
    uint64_t offset;
    uint32_t flags;
    uint32_t references;
};

constexpr uint32_t kOpenAccessMask = 3;
constexpr uint32_t kOpenWriteOnly = 1;
constexpr uint32_t kOpenReadWrite = 2;
constexpr uint32_t kOpenCreate = 0x40;
constexpr uint32_t kOpenExclusive = 0x80;
constexpr uint32_t kOpenTruncate = 0x200;
constexpr uint32_t kOpenAppend = 0x400;
constexpr uint32_t kOpenNonBlocking = 0x800;
constexpr uint32_t kOpenDirectory = 0x10000;
constexpr uint32_t kOpenNoFollow = 0x20000;

constexpr uint32_t kPollIn = 0x001;
constexpr uint32_t kPollOut = 0x004;
constexpr uint32_t kPollErr = 0x008;
constexpr uint32_t kPollHangup = 0x010;

constexpr uint32_t kModeDirectory = 0040000;
constexpr uint32_t kModeCharDevice = 0020000;
constexpr uint32_t kModeFifo = 0010000;
constexpr uint32_t kModeRegular = 0100000;
constexpr uint32_t kModeSymlink = 0120000;
constexpr uint32_t kModeTypeMask = 0170000;

extern WaitQueue poll_waiters;

bool vfs_mount_image(uint64_t phys_start, uint64_t phys_end);
bool vfs_has_image();
Node* vfs_root();

int vfs_resolve(Node* base, const char* path, bool follow_last, Node** result);
int vfs_resolve_parent(Node* base, const char* path, Node** parent, const char** leaf, size_t* leaf_length);
Node* vfs_lookup_child(Node* directory, const char* name, size_t length);
Node* vfs_create_child(Node* directory, const char* name, size_t length, NodeKind kind, uint32_t mode);
int vfs_unlink(Node* node);
int vfs_rename(Node* node, Node* new_parent, const char* name, size_t length);
Node* vfs_register_device(const char* path, const DeviceOps* ops, uint64_t size, uint32_t mode);
size_t vfs_path_of(Node* node, char* buffer, size_t capacity);

void node_retain(Node* node);
void node_release(Node* node);
int node_truncate(Node* node, uint64_t size);
int64_t node_read(Node* node, void* buffer, uint64_t length, uint64_t offset);
int64_t node_write(Node* node, const void* buffer, uint64_t length, uint64_t offset);
uint64_t node_page_phys(Node* node, uint64_t page_index, bool allocate);
int node_make_writable(Node* node);

File* file_open(Node* node, uint32_t flags);
File* file_retain(File* file);
void file_release(File* file);
int64_t file_read(File* file, void* buffer, uint64_t length);
int64_t file_write(File* file, const void* buffer, uint64_t length);
uint32_t file_poll(File* file);

int pipe_create(File** read_end, File** write_end);
void poll_notify();

void devices_init();

}
