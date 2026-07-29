#pragma once

#include "native_self_test.h"

// Brings up a minimal Vulkan 1.3 instance/device, creates a VMA allocator
// configured for Vulkan 1.3, and performs a tiny buffer allocation through it.
// Used as a self-test to prove the Vulkan + VMA integration works on a real
// device. The whole thing is torn down before returning.
SelfTestResult RunVulkanVmaSelfTest();
