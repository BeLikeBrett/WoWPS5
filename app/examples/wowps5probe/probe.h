// WoWPS5 capability and RGBA16F compute probe. SPDX-License-Identifier: MIT
#pragma once
#include "platform.h"
#include <cstdio>
#include <stdexcept>
#include <vector>
#ifndef PS5_APP_ROOT
#define PS5_APP_ROOT "/app0"
#endif

struct ProbeResult {
    bool storageFormat = false;
    bool computeQueue = false;
    bool extendedFormats = false;
    bool computePassed = false;
    uint32_t apiVersion = 0;
    uint32_t mismatches = 0;
    VkResult result = VK_SUCCESS;
};

static ProbeResult runProbe(vks::VulkanDevice* gpu, VkQueue queue,
                           VkShaderModule shader, bool extendedEnabled)
{
    ProbeResult out;
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(gpu->physicalDevice, &props);
    out.apiVersion = props.apiVersion;
    say("probe identity wowps5-rgba16f-v2; device %s; Vulkan %u.%u.%u", props.deviceName,
        VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion),
        VK_API_VERSION_PATCH(props.apiVersion));
    VkFormatProperties format{};
    vkGetPhysicalDeviceFormatProperties(gpu->physicalDevice, VK_FORMAT_R16G16B16A16_SFLOAT, &format);
    const auto required = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
    out.storageFormat = (format.optimalTilingFeatures & required) == required;
    out.extendedFormats = extendedEnabled;
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(gpu->physicalDevice, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(gpu->physicalDevice, &count, families.data());
    out.computeQueue = gpu->queueFamilyIndices.graphics < count &&
        (families[gpu->queueFamilyIndices.graphics].queueFlags & VK_QUEUE_COMPUTE_BIT);
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    f11.pNext = &f12; f12.pNext = &f13;
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features.pNext = &f11;
    vkGetPhysicalDeviceFeatures2(gpu->physicalDevice, &features);
    VkPhysicalDeviceDescriptorIndexingProperties indexing{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES};
    VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    properties.pNext = &indexing;
    vkGetPhysicalDeviceProperties2(gpu->physicalDevice, &properties);
    say("probe feature shaderDrawParameters=%u samplerMirrorClampToEdge=%u dynamicRendering=%u synchronization2=%u",
        f11.shaderDrawParameters, f12.samplerMirrorClampToEdge, f13.dynamicRendering, f13.synchronization2);
    say("probe feature descriptorIndexing=%u runtimeDescriptorArray=%u descriptorBindingPartiallyBound=%u descriptorBindingVariableDescriptorCount=%u",
        f12.descriptorIndexing, f12.runtimeDescriptorArray, f12.descriptorBindingPartiallyBound, f12.descriptorBindingVariableDescriptorCount);
    say("probe feature sampledImageNonUniform=%u storageImageNonUniform=%u storageBufferNonUniform=%u uniformTexelBufferNonUniform=%u storageTexelBufferNonUniform=%u",
        f12.shaderSampledImageArrayNonUniformIndexing, f12.shaderStorageImageArrayNonUniformIndexing,
        f12.shaderStorageBufferArrayNonUniformIndexing, f12.shaderUniformTexelBufferArrayNonUniformIndexing,
        f12.shaderStorageTexelBufferArrayNonUniformIndexing);
    say("probe feature sampledImageUpdateAfterBind=%u storageImageUpdateAfterBind=%u storageBufferUpdateAfterBind=%u uniformTexelBufferUpdateAfterBind=%u storageTexelBufferUpdateAfterBind=%u updateUnusedWhilePending=%u",
        f12.descriptorBindingSampledImageUpdateAfterBind, f12.descriptorBindingStorageImageUpdateAfterBind,
        f12.descriptorBindingStorageBufferUpdateAfterBind, f12.descriptorBindingUniformTexelBufferUpdateAfterBind,
        f12.descriptorBindingStorageTexelBufferUpdateAfterBind, f12.descriptorBindingUpdateUnusedWhilePending);
    say("probe limit sampledImageUpdateAfterBind=%u storageImageUpdateAfterBind=%u storageBufferUpdateAfterBind=%u allPoolsUpdateAfterBind=%u",
        indexing.maxDescriptorSetUpdateAfterBindSampledImages, indexing.maxDescriptorSetUpdateAfterBindStorageImages,
        indexing.maxDescriptorSetUpdateAfterBindStorageBuffers, indexing.maxUpdateAfterBindDescriptorsInAllPools);
    uint32_t extensionCount = 0;
    vkEnumerateDeviceExtensionProperties(gpu->physicalDevice, nullptr, &extensionCount, nullptr);
    std::vector<VkExtensionProperties> extensions(extensionCount);
    vkEnumerateDeviceExtensionProperties(gpu->physicalDevice, nullptr, &extensionCount, extensions.data());
    for (const auto& extension : extensions)
        say("probe extension %s", extension.extensionName);
    say("probe RGBA16F storage+transfer=%d compute_queue=%d extended_formats=%d",
        out.storageFormat, out.computeQueue, out.extendedFormats);
    if (!out.storageFormat || !out.computeQueue || !out.extendedFormats) return out;

    VkDevice device = gpu->logicalDevice;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    vks::Buffer readback{};
    VkCommandBuffer command = VK_NULL_HANDLE;
    auto check = [&](VkResult result) {
        out.result = result;
        if (result != VK_SUCCESS) throw std::runtime_error("probe Vulkan call failed");
    };
    try {
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        info.extent = {16, 16, 1};
        info.mipLevels = info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        check(vkCreateImage(device, &info, nullptr, &image));
        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(device, image, &requirements);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = gpu->getMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        check(vkAllocateMemory(device, &allocation, nullptr, &memory));
        check(vkBindImageMemory(device, image, memory, 0));
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = image; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = info.format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        check(vkCreateImageView(device, &vi, nullptr, &view));
        VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.bindingCount = 1; li.pBindings = &binding;
        check(vkCreateDescriptorSetLayout(device, &li, nullptr, &layout));
        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1};
        VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pi.maxSets = 1; pi.poolSizeCount = 1; pi.pPoolSizes = &size;
        check(vkCreateDescriptorPool(device, &pi, nullptr, &pool));
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = pool; ai.descriptorSetCount = 1; ai.pSetLayouts = &layout;
        VkDescriptorSet set;
        check(vkAllocateDescriptorSets(device, &ai, &set));
        VkDescriptorImageInfo ii{VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = set; write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; write.pImageInfo = &ii;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 1; pli.pSetLayouts = &layout;
        check(vkCreatePipelineLayout(device, &pli, nullptr, &pipelineLayout));
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, shader, "main", nullptr};
        ci.layout = pipelineLayout;
        check(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline));
        check(gpu->createBuffer(VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &readback, 16 * 16 * 8));
        command = gpu->createCommandBuffer(VK_COMMAND_BUFFER_LEVEL_PRIMARY, true);
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image; barrier.subresourceRange = vi.subresourceRange;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &set, 0, nullptr);
        vkCmdDispatch(command, 2, 2, 1);
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL; barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; copy.imageExtent = info.extent;
        vkCmdCopyImageToBuffer(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &copy);
        VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        host.srcQueueFamilyIndex = host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        host.buffer = readback.buffer; host.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &host, 0, nullptr);
        check(vkEndCommandBuffer(command));
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        check(vkCreateFence(device, &fi, nullptr, &fence));
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
        check(vkQueueSubmit(queue, 1, &submit, fence));
        VkResult waited = vkWaitForFences(device, 1, &fence, VK_TRUE, 10000000000ULL);
        if (waited != VK_SUCCESS) {
            out.result = waited;
            // The shell must reclaim in-flight objects; never free them under the GPU.
            say("probe compute fence failed: %d; stopping title", waited);
            throw std::runtime_error("probe fence timeout; title must close");
        }
        check(readback.map());
        const auto* pixels = static_cast<const uint16_t*>(readback.mapped);
        const uint16_t expected[4] = {0x3400, 0x3800, 0x3a00, 0x3c00};
        for (uint32_t i = 0; i < 16 * 16 * 4; ++i)
            if (pixels[i] != expected[i % 4]) ++out.mismatches;
        out.computePassed = out.mismatches == 0;
        say("probe RGBA16F compute readback %s; mismatches=%u", out.computePassed ? "PASS" : "FAIL", out.mismatches);
    } catch (const std::runtime_error& error) {
        say("probe error: %s; VkResult=%d", error.what(), out.result);
        if (fence && vkGetFenceStatus(device, fence) == VK_NOT_READY) throw;
    }
    if (command) vkFreeCommandBuffers(device, gpu->commandPool, 1, &command);
    if (fence) vkDestroyFence(device, fence, nullptr);
    readback.unmap();
    readback.destroy();
    if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
    if (pipelineLayout) vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
    if (pool) vkDestroyDescriptorPool(device, pool, nullptr);
    if (layout) vkDestroyDescriptorSetLayout(device, layout, nullptr);
    if (view) vkDestroyImageView(device, view, nullptr);
    if (image) vkDestroyImage(device, image, nullptr);
    if (memory) vkFreeMemory(device, memory, nullptr);
    return out;
}
