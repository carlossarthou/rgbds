/* SPDX-License-Identifier: MIT */

#ifndef RGBDS_MAIN_H
#define RGBDS_MAIN_H

#include <stdint.h>
#include <stdio.h>
#include <string>

#include "helpers.hpp"

extern bool haltNop;
extern bool warnOnHaltNop;
extern bool optimizeLoads;
extern bool warnOnLdOpt;
extern bool verbose;
extern bool warnings; // True to enable warnings, false to disable them.

extern FILE *dependfile;
extern std::string targetFileName;
extern bool generatedMissingIncludes;
extern bool failedOnMissingInclude;
extern bool generatePhonyDeps;

#endif // RGBDS_MAIN_H
