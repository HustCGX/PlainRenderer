#pragma once
#include "pch.h"
#include <vulkan/vulkan.h>
#include "Runtime/Rendering/ResourceDescriptions.h"

VkPipelineInputAssemblyStateCreateInfo                  createInputAssemblyInfo(const RasterizationeMode rasterMode);
VkPipelineDynamicStateCreateInfo                        createDynamicStateInfo(const std::vector<VkDynamicState>& states);
VkPipelineRasterizationStateCreateInfo                  createRasterizationState(const RasterizationConfig& raster);
VkPipelineRasterizationConservativeStateCreateInfoEXT   createConservativeRasterCreateInfo();
VkPipelineViewportStateCreateInfo                       createDynamicViewportCreateInfo();
VkPipelineTessellationStateCreateInfo                   createTesselationState(const uint32_t patchControlPoints);
VkPipelineMultisampleStateCreateInfo                    createDefaultMultisamplingInfo();
VkPipelineDepthStencilStateCreateInfo                   createDepthStencilState(const DepthTest& depthTest);
VkStencilOpState                                        createStencilOpStateDummy();
VkCompareOp                                             depthFunctionToVulkanCompareOp(const DepthFunction function);