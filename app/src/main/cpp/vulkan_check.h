#pragma once

#include "native_self_test.h"
#include "volk.h"

// Sanity check for the volk + VMA + Vulkan 1.3 plumbing, kept around for the days
// when nothing works and the cause is not obvious: it proves the loader, the
// dynamic function pointers and the allocator are fine, so the bug is elsewhere.
//
// It borrows the application's instance (putorana::graphics::instance_holder)
// rather than creating one of its own, then builds a throwaway logical device and
// VMA allocator on top of it, allocates one small buffer, and tears the device and
// the allocator back down. The instance is left untouched.
//
// Deliberately dumb where the real renderer is careful: it takes the first
// physical device and queue family 0 without asking whether they can present.
// That is the point -- it has to keep working when the real path is broken.
SelfTestResult RunVulkanVmaSelfTest(VkInstance instance);
