#pragma once
#include "lumen.h"

// Both backends expose the same tightly packed ARGB rendering surface.
typedef struct {
    Canvas *(*init)(void);
    Canvas *(*present)(void);
} DisplayBackend;

extern const DisplayBackend display_backend;
