/* SPDX-License-Identifier: MIT */

#ifndef RGBDS_ERROR_H
#define RGBDS_ERROR_H

#include "helpers.hpp"
#include "platform.hpp"

extern "C" {

void warn(char const *fmt...) format_(printf, 1, 2);
void warnx(char const *fmt, ...) format_(printf, 1, 2);

[[noreturn]] void err(char const *fmt, ...) format_(printf, 1, 2);
[[noreturn]] void errx(char const *fmt, ...) format_(printf, 1, 2);
}

#endif // RGBDS_ERROR_H
