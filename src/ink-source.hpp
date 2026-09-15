// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "canvas.hpp"
#include <memory>
#include <obs-module.h>
namespace program_draw {
inline constexpr const char *SourceId = "program_draw_poc_overlay";
void registerInkSource(std::shared_ptr<Canvas> canvas);
} // namespace program_draw
