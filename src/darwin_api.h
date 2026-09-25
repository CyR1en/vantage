#ifndef SB_DARWIN_API_H
#define SB_DARWIN_API_H
#ifdef __APPLE__
#include <sys/attr.h>
#elif defined(SB_TEST_DARWIN)
/* Contract-test facade, NOT a macOS SDK or an implementation of these syscalls. */
#include "darwin_shim.h"
#endif
#endif
