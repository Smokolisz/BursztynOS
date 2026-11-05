#pragma once

#include <stdint.h>

namespace lx {

bool display_start();
void display_on_process_exit();

}

extern "C" {
bool gui_surface_geometry(uint32_t* width, uint32_t* height);
void gui_surface_attach(uint32_t* pixels, uint32_t width, uint32_t height);
void gui_surface_detach();
void gui_surface_raise();
void gui_surface_present(uint32_t first_row, uint32_t row_count);
bool gui_surface_has_keyboard();
}
