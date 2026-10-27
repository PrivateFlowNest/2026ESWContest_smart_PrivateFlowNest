#pragma once

#include <stddef.h>
#include <stdint.h>

namespace hm {

typedef uint32_t (*Random32)();

// Writes a random (version 4, RFC 4122 variant) UUID, lowercase, 36 chars + NUL.
void makeUuidV4(Random32 random32, char out[37]);

// True for the canonical 8-4-4-4-12 hexadecimal form (either case).
bool isUuid(const char* text);

}  // namespace hm
