// Single translation unit that compiles the Vulkan Memory Allocator (VMA)
// implementation. VMA is a single-header library, so VMA_IMPLEMENTATION must be
// defined in exactly one .cpp file across the whole target.
//
// Configuration macros (VMA_VULKAN_VERSION, VMA_STATIC/DYNAMIC_VULKAN_FUNCTIONS)
// are provided centrally via CMake target_compile_definitions so that this unit
// and every consumer of the header agree on the same settings.
#define VMA_IMPLEMENTATION
#include "vk_mem_alloc.h"
