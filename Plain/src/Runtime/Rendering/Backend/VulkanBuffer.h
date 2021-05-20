#pragma once
#include "pch.h"
#include <vulkan/vulkan.h>
#include "VkMemoryAllocator.h"

VkBuffer createVulkanBuffer(const size_t size, const VkBufferUsageFlags usageFlags,
    const std::vector<uint32_t>& uniqueQueueFamilies);

VulkanAllocation allocateAndBindBufferMemory(const VkBuffer buffer, const VkMemoryAllocateFlags memoryFlags,
    VkMemoryAllocator& allocator);

std::vector<uint32_t> makeUniqueQueueFamilyList(const std::vector<uint32_t>& queueFamilies);