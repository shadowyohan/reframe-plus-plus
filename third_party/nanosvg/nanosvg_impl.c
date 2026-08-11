/* Translation unit for the header-only nanosvg implementation.
 *
 * It lives with the other vendored code so it is built with the vendored
 * warning settings, instead of dragging nanosvg's narrowing conversions into
 * one of our own source files. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define NANOSVG_IMPLEMENTATION
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvg.h"
#include "nanosvgrast.h"
