#pragma once

#include <stdint.h>

namespace lx {
struct InterruptFrame;
}

extern "C" {
void lx_init(uint64_t kernel_cr3);
void lx_start_shell(void (*entry)(void*));
[[noreturn]] void lx_run();

bool lx_launch_reaper();

bool lx_handle_exception(lx::InterruptFrame* frame);
void lx_on_timer_interrupt(lx::InterruptFrame* frame);

void lx_wait_for_keyboard();
void lx_keyboard_ready();

void lx_display_post_pointer(int x, int y, uint8_t buttons, int wheel);
void lx_display_post_close();
void lx_display_post_scancode(uint8_t scancode);

uint32_t lx_timer_ticks_per_ms(volatile uint32_t* lapic);
}
