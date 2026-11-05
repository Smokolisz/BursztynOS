#include "display.h"
#include "errno.h"
#include "kernel_api.h"
#include "mm.h"
#include "vfs.h"

namespace lx {

namespace {

int64_t read_null(File*, void*, uint64_t) { return 0; }
int64_t write_discard(File*, const void*, uint64_t length) { return static_cast<int64_t>(length); }

int64_t read_zero(File*, void* buffer, uint64_t length) {
    memset(buffer, 0, length);
    return static_cast<int64_t>(length);
}

int64_t read_random(File*, void* buffer, uint64_t length) {
    static uint64_t state;
    state ^= read_tsc();
    auto* out = static_cast<uint8_t*>(buffer);
    for (uint64_t i = 0; i < length; ++i) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        if (!state) state = 0x9E3779B97F4A7C15ULL;
        out[i] = static_cast<uint8_t>(state);
    }
    return static_cast<int64_t>(length);
}

int64_t write_console(File*, const void* buffer, uint64_t length) {
    constexpr uint64_t kChunk = 400;
    auto* text = static_cast<const char*>(buffer);
    char line[kChunk + 1];
    for (uint64_t done = 0; done < length;) {
        const uint64_t count = min(length - done, kChunk);
        memcpy(line, text + done, count);
        line[count] = 0;
        log("%s", line);
        done += count;
    }
    return static_cast<int64_t>(length);
}

uint32_t poll_console(File*) { return kPollOut; }

const DeviceOps kNullOps = {read_null, write_discard, nullptr, nullptr, nullptr, nullptr, nullptr};
const DeviceOps kZeroOps = {read_zero, write_discard, nullptr, nullptr, nullptr, nullptr, nullptr};
const DeviceOps kRandomOps = {read_random, write_discard, nullptr, nullptr, nullptr, nullptr, nullptr};
const DeviceOps kConsoleOps = {read_null, write_console, nullptr, poll_console, nullptr, nullptr, nullptr};

constexpr uint64_t kFbGetVarScreenInfo = 0x4600;
constexpr uint64_t kFbGetFixScreenInfo = 0x4602;
constexpr uint32_t kFbTypePackedPixels = 0;
constexpr uint32_t kFbVisualTrueColor = 2;

struct FbBitfield {
    uint32_t offset;
    uint32_t length;
    uint32_t msb_right;
};

struct FbVarScreenInfo {
    uint32_t xres, yres, xres_virtual, yres_virtual, xoffset, yoffset;
    uint32_t bits_per_pixel, grayscale;
    FbBitfield red, green, blue, transp;
    uint32_t nonstd, activate, height, width, accel_flags;
    uint32_t pixclock, left_margin, right_margin, upper_margin, lower_margin;
    uint32_t hsync_len, vsync_len, sync, vmode, rotate, colorspace;
    uint32_t reserved[4];
};

struct FbFixScreenInfo {
    char id[16];
    uint64_t smem_start;
    uint32_t smem_len;
    uint32_t type;
    uint32_t type_aux;
    uint32_t visual;
    uint16_t xpanstep, ypanstep, ywrapstep;
    uint32_t line_length;
    uint64_t mmio_start;
    uint32_t mmio_len;
    uint32_t accel;
    uint16_t capabilities;
    uint16_t reserved[2];
};

constexpr uint16_t kEventSync = 0;
constexpr uint16_t kEventKey = 1;
constexpr uint16_t kEventRelative = 2;
constexpr uint16_t kEventAbsolute = 3;
constexpr uint16_t kAbsoluteX = 0;
constexpr uint16_t kAbsoluteY = 1;
constexpr uint16_t kRelativeWheel = 8;
constexpr uint16_t kButtonLeft = 0x110;
constexpr uint16_t kButtonRight = 0x111;
constexpr uint16_t kButtonMiddle = 0x112;
constexpr uint16_t kKeyClose = 206;

struct InputEvent {
    int64_t seconds;
    int64_t microseconds;
    uint16_t type;
    uint16_t code;
    int32_t value;
};

constexpr uint32_t kInputQueueCapacity = 4096;

struct {
    uint64_t pixels_phys;
    uint64_t capacity;
    uint32_t width;
    uint32_t height;
} surface;

InputEvent input_queue[kInputQueueCapacity];
uint32_t input_head;
uint32_t input_tail;
WaitQueue input_waiters;
bool surface_open;
int last_pointer_x = -1;
int last_pointer_y = -1;
uint8_t last_buttons;
bool scancode_extended;

void push_event(uint16_t type, uint16_t code, int32_t value) {
    const uint32_t next = (input_head + 1) % kInputQueueCapacity;
    if (next == input_tail) return;
    const uint64_t now = wall_clock_ns();
    input_queue[input_head] = InputEvent{static_cast<int64_t>(now / 1000000000ULL),
                                         static_cast<int64_t>((now % 1000000000ULL) / 1000), type, code, value};
    input_head = next;
}

void finish_event_batch() {
    push_event(kEventSync, 0, 0);
    wake_all(&input_waiters);
    poll_notify();
}

int64_t input_read(File* file, void* buffer, uint64_t length) {
    if (length < sizeof(InputEvent)) return -kEINVAL;
    while (input_head == input_tail) {
        if (file->flags & kOpenNonBlocking) return -kEAGAIN;
        const int status = block_on(&input_waiters, 0);
        if (status < 0) return status;
    }
    auto* out = static_cast<InputEvent*>(buffer);
    uint64_t count = 0;
    const uint64_t capacity = length / sizeof(InputEvent);
    while (count < capacity && input_head != input_tail) {
        out[count++] = input_queue[input_tail];
        input_tail = (input_tail + 1) % kInputQueueCapacity;
    }
    return static_cast<int64_t>(count * sizeof(InputEvent));
}

uint32_t input_poll(File*) { return input_head != input_tail ? kPollIn : 0; }

const DeviceOps kInputOps = {input_read, nullptr, nullptr, input_poll, nullptr, nullptr, nullptr};

int64_t surface_ioctl(File*, uint64_t command, uint64_t argument) {
    if (command == kFbGetVarScreenInfo) {
        FbVarScreenInfo info{};
        info.xres = info.xres_virtual = surface.width;
        info.yres = info.yres_virtual = surface.height;
        info.bits_per_pixel = 32;
        info.red = {16, 8, 0};
        info.green = {8, 8, 0};
        info.blue = {0, 8, 0};
        info.transp = {24, 8, 0};
        return copy_to_user(reinterpret_cast<void*>(argument), &info, sizeof(info)) ? 0 : -kEFAULT;
    }
    if (command == kFbGetFixScreenInfo) {
        FbFixScreenInfo info{};
        format(info.id, sizeof(info.id), "bursztynfb");
        info.smem_len = surface.width * surface.height * 4;
        info.type = kFbTypePackedPixels;
        info.visual = kFbVisualTrueColor;
        info.line_length = surface.width * 4;
        return copy_to_user(reinterpret_cast<void*>(argument), &info, sizeof(info)) ? 0 : -kEFAULT;
    }
    return -kENOTTY;
}

uint64_t surface_page_phys(uint64_t offset) { return offset < surface.capacity ? surface.pixels_phys + offset : 0; }

void surface_sync(uint64_t offset, uint64_t length) {
    if (!surface_open || !length) return;
    const uint64_t pitch = surface.width * 4ULL;
    const uint64_t first = offset / pitch;
    const uint64_t last = (offset + length + pitch - 1) / pitch;
    if (first >= surface.height) return;
    gui_surface_present(static_cast<uint32_t>(first), static_cast<uint32_t>(min<uint64_t>(last, surface.height) - first));
}

const DeviceOps kSurfaceOps = {nullptr, nullptr, surface_ioctl, nullptr, surface_page_phys, surface_sync, nullptr};

uint16_t linux_key_for_extended(uint8_t code) {
    switch (code) {
        case 0x1C: return 96;
        case 0x1D: return 97;
        case 0x35: return 98;
        case 0x37: return 99;
        case 0x38: return 100;
        case 0x47: return 102;
        case 0x48: return 103;
        case 0x49: return 104;
        case 0x4B: return 105;
        case 0x4D: return 106;
        case 0x4F: return 107;
        case 0x50: return 108;
        case 0x51: return 109;
        case 0x52: return 110;
        case 0x53: return 111;
        case 0x5B: return 125;
        case 0x5C: return 126;
        case 0x5D: return 127;
        default: return 0;
    }
}

}

void devices_init() {
    vfs_register_device("/dev/null", &kNullOps, 0, 0666);
    vfs_register_device("/dev/zero", &kZeroOps, 0, 0666);
    vfs_register_device("/dev/random", &kRandomOps, 0, 0666);
    vfs_register_device("/dev/urandom", &kRandomOps, 0, 0666);
    vfs_register_device("/dev/console", &kConsoleOps, 0, 0600);
    vfs_register_device("/dev/tty", &kConsoleOps, 0, 0666);
    vfs_register_device("/dev/input/event0", &kInputOps, 0, 0660);

    uint32_t width = 0;
    uint32_t height = 0;
    if (gui_surface_geometry(&width, &height)) {
        surface.width = width;
        surface.height = height;
        surface.capacity = page_ceil(static_cast<uint64_t>(width) * height * 4);
        vfs_register_device("/dev/fb0", &kSurfaceOps, surface.capacity, 0660);
    }
}

bool display_start() {
    if (!surface.capacity) return false;
    if (!surface.pixels_phys) {
        surface.pixels_phys = alloc_contiguous_frames(surface.capacity / kPageSize);
        if (!surface.pixels_phys) return false;
    }
    memset(phys_to_virt(surface.pixels_phys), 0, surface.capacity);
    input_head = input_tail = 0;
    last_pointer_x = last_pointer_y = -1;
    last_buttons = 0;
    scancode_extended = false;
    surface_open = true;
    gui_surface_attach(static_cast<uint32_t*>(phys_to_virt(surface.pixels_phys)), surface.width, surface.height);
    return true;
}

void display_on_process_exit() {
    surface_open = false;
    gui_surface_detach();
}

}

using namespace lx;

extern "C" void lx_display_post_pointer(int x, int y, uint8_t buttons, int wheel) {
    if (!surface_open) return;
    if (x != last_pointer_x) push_event(kEventAbsolute, kAbsoluteX, x);
    if (y != last_pointer_y) push_event(kEventAbsolute, kAbsoluteY, y);
    last_pointer_x = x;
    last_pointer_y = y;
    const uint8_t changed = buttons ^ last_buttons;
    if (changed & 1) push_event(kEventKey, kButtonLeft, buttons & 1);
    if (changed & 2) push_event(kEventKey, kButtonRight, (buttons >> 1) & 1);
    if (changed & 4) push_event(kEventKey, kButtonMiddle, (buttons >> 2) & 1);
    last_buttons = buttons;
    if (wheel) push_event(kEventRelative, kRelativeWheel, -wheel);
    finish_event_batch();
}

extern "C" void lx_display_post_close() {
    if (!surface_open) return;
    push_event(kEventKey, kKeyClose, 1);
    push_event(kEventKey, kKeyClose, 0);
    finish_event_batch();
}

extern "C" void lx_display_post_scancode(uint8_t scancode) {
    if (!surface_open) return;
    if (scancode == 0xE0) {
        scancode_extended = true;
        return;
    }
    const bool released = scancode & 0x80;
    const uint8_t code = scancode & 0x7F;
    const uint16_t key = scancode_extended ? linux_key_for_extended(code) : code;
    scancode_extended = false;
    if (!key) return;
    push_event(kEventKey, key, released ? 0 : 1);
    finish_event_batch();
}
