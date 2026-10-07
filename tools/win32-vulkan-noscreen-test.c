/* Vulkan through Wine with no window. MIT license.
 * vulkan-1.dll (Wine's loader) -> winevulkan -> the console's driver: an
 * instance, a device, an image cleared on the GPU, copied to a buffer and read
 * back through a mapping. No surface, so no display driver is needed.
 * Each check prints PASS or FAIL; the exit status is the number of failures. */
#define VK_NO_PROTOTYPES
#include <windows.h>
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <string.h>

static int failures;
static void check(int passed, const char* what, int result) {
    if(!passed) failures++;
    printf("%s: %s (%d)\n", passed ? "PASS" : "FAIL", what, result);
    fflush(stdout);
}
#define FUNCTION(owner, name) PFN_##name name = (PFN_##name)owner(handle, #name)

int main(void) {
    HMODULE loader = LoadLibraryA("vulkan-1.dll");
    PFN_vkGetInstanceProcAddr getInstance = loader ? (PFN_vkGetInstanceProcAddr)GetProcAddress(loader, "vkGetInstanceProcAddr") : NULL;
    check(getInstance != NULL, "vulkan-1.dll loads", (int)GetLastError());
    if(!getInstance) goto done;
    PFN_vkCreateInstance createInstance = (PFN_vkCreateInstance)getInstance(NULL, "vkCreateInstance");
    VkApplicationInfo application = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "wowps5", .apiVersion = VK_API_VERSION_1_1 };
    VkInstanceCreateInfo instanceInfo = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &application };
    VkInstance instance = VK_NULL_HANDLE;
    VkResult result = createInstance ? createInstance(&instanceInfo, NULL, &instance) : VK_ERROR_INITIALIZATION_FAILED;
    check(result == VK_SUCCESS, "vkCreateInstance", result);
    if(result) goto done;
    {
        VkInstance handle = instance;
        FUNCTION(getInstance, vkEnumeratePhysicalDevices); FUNCTION(getInstance, vkGetPhysicalDeviceProperties);
        FUNCTION(getInstance, vkGetPhysicalDeviceQueueFamilyProperties); FUNCTION(getInstance, vkGetPhysicalDeviceMemoryProperties);
        FUNCTION(getInstance, vkCreateDevice); FUNCTION(getInstance, vkGetDeviceProcAddr); FUNCTION(getInstance, vkDestroyInstance);
        VkPhysicalDevice physical[8]; uint32_t count = 8;
        result = vkEnumeratePhysicalDevices(instance, &count, physical);
        check(result >= 0 && count >= 1, "a physical device", result);
        if(result < 0 || !count) goto done;
        VkPhysicalDeviceProperties properties; vkGetPhysicalDeviceProperties(physical[0], &properties);
        printf("info device \"%s\", Vulkan %u.%u.%u, vendor 0x%x\n", properties.deviceName, VK_API_VERSION_MAJOR(properties.apiVersion),
               VK_API_VERSION_MINOR(properties.apiVersion), VK_API_VERSION_PATCH(properties.apiVersion), properties.vendorID);
        VkQueueFamilyProperties families[16]; uint32_t familyCount = 16, family = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical[0], &familyCount, families);
        while(family < familyCount && !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) family++;
        float priority = 1;
        VkDeviceQueueCreateInfo queueInfo = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority };
        VkDeviceCreateInfo deviceInfo = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &queueInfo };
        VkDevice device = VK_NULL_HANDLE;
        result = family < familyCount ? vkCreateDevice(physical[0], &deviceInfo, NULL, &device) : VK_ERROR_FEATURE_NOT_PRESENT;
        check(result == VK_SUCCESS, "vkCreateDevice with a graphics queue", result);
        if(result) goto done;
        {
            VkDevice handle = device;
            FUNCTION(vkGetDeviceProcAddr, vkGetDeviceQueue); FUNCTION(vkGetDeviceProcAddr, vkCreateImage); FUNCTION(vkGetDeviceProcAddr, vkCreateBuffer);
            FUNCTION(vkGetDeviceProcAddr, vkGetImageMemoryRequirements); FUNCTION(vkGetDeviceProcAddr, vkGetBufferMemoryRequirements);
            FUNCTION(vkGetDeviceProcAddr, vkAllocateMemory); FUNCTION(vkGetDeviceProcAddr, vkBindImageMemory); FUNCTION(vkGetDeviceProcAddr, vkBindBufferMemory);
            FUNCTION(vkGetDeviceProcAddr, vkCreateCommandPool); FUNCTION(vkGetDeviceProcAddr, vkAllocateCommandBuffers);
            FUNCTION(vkGetDeviceProcAddr, vkBeginCommandBuffer); FUNCTION(vkGetDeviceProcAddr, vkCmdPipelineBarrier);
            FUNCTION(vkGetDeviceProcAddr, vkCmdClearColorImage); FUNCTION(vkGetDeviceProcAddr, vkCmdCopyImageToBuffer);
            FUNCTION(vkGetDeviceProcAddr, vkEndCommandBuffer); FUNCTION(vkGetDeviceProcAddr, vkQueueSubmit); FUNCTION(vkGetDeviceProcAddr, vkQueueWaitIdle);
            FUNCTION(vkGetDeviceProcAddr, vkMapMemory); FUNCTION(vkGetDeviceProcAddr, vkDeviceWaitIdle); FUNCTION(vkGetDeviceProcAddr, vkDestroyDevice);
            VkPhysicalDeviceMemoryProperties memory; vkGetPhysicalDeviceMemoryProperties(physical[0], &memory);
            VkQueue queue; vkGetDeviceQueue(device, family, 0, &queue);
            const uint32_t size = 64;
            VkImageCreateInfo imageInfo = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
                .extent = { size, size, 1 }, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT };
            VkBufferCreateInfo bufferInfo = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size * size * 4, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT };
            VkImage image; VkBuffer buffer; VkMemoryRequirements need; VkDeviceMemory imageMemory = VK_NULL_HANDLE, bufferMemory = VK_NULL_HANDLE;
            result = vkCreateImage(device, &imageInfo, NULL, &image);
            if(!result) result = vkCreateBuffer(device, &bufferInfo, NULL, &buffer);
            check(result == VK_SUCCESS, "an image and a buffer", result);
            if(result) goto done;
            for(int pass = 0; pass < 2 && !result; pass++) {
                const VkMemoryPropertyFlags wanted = pass ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT : 0;
                if(pass) vkGetBufferMemoryRequirements(device, buffer, &need); else vkGetImageMemoryRequirements(device, image, &need);
                uint32_t type = 0;
                while(type < memory.memoryTypeCount && !((need.memoryTypeBits >> type & 1) && (memory.memoryTypes[type].propertyFlags & wanted) == wanted)) type++;
                VkMemoryAllocateInfo allocate = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = need.size, .memoryTypeIndex = type };
                result = type < memory.memoryTypeCount ? vkAllocateMemory(device, &allocate, NULL, pass ? &bufferMemory : &imageMemory) : VK_ERROR_OUT_OF_DEVICE_MEMORY;
                if(!result) result = pass ? vkBindBufferMemory(device, buffer, bufferMemory, 0) : vkBindImageMemory(device, image, imageMemory, 0);
            }
            check(result == VK_SUCCESS, "device memory for both, the buffer's visible to the CPU", result);
            if(result) goto done;
            VkCommandPoolCreateInfo poolInfo = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = family };
            VkCommandPool pool; VkCommandBuffer commands;
            result = vkCreateCommandPool(device, &poolInfo, NULL, &pool);
            VkCommandBufferAllocateInfo commandInfo = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
            if(!result) result = vkAllocateCommandBuffers(device, &commandInfo, &commands);
            VkCommandBufferBeginInfo begin = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
            if(!result) result = vkBeginCommandBuffer(commands, &begin);
            if(!result) {
                const VkImageSubresourceRange whole = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
                VkImageMemoryBarrier barrier = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = image, .subresourceRange = whole };
                vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
                const VkClearColorValue colour = { .float32 = { 1.0f, 0.5f, 0.25f, 1.0f } };
                vkCmdClearColorImage(commands, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &colour, 1, &whole);
                barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
                const VkBufferImageCopy region = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { size, size, 1 } };
                vkCmdCopyImageToBuffer(commands, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
                result = vkEndCommandBuffer(commands);
            }
            VkSubmitInfo submit = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &commands };
            if(!result) result = vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE);
            if(!result) result = vkQueueWaitIdle(queue);
            check(result == VK_SUCCESS, "commands recorded, submitted and finished on the GPU", result);
            unsigned char* pixels = NULL; unsigned right = 0;
            if(!result) result = vkMapMemory(device, bufferMemory, 0, VK_WHOLE_SIZE, 0, (void**)&pixels);
            for(unsigned i = 0; !result && pixels && i < size * size; i++)
                right += pixels[i * 4] == 255 && pixels[i * 4 + 1] >= 127 && pixels[i * 4 + 1] <= 128 && pixels[i * 4 + 2] >= 63 && pixels[i * 4 + 2] <= 64 && pixels[i * 4 + 3] == 255;
            if(pixels) printf("info first pixel %u %u %u %u, mapping at %p\n", pixels[0], pixels[1], pixels[2], pixels[3], (void*)pixels);
            check(!result && right == size * size, "the image the GPU cleared reads back correct through a mapping", (int)right);
            vkDeviceWaitIdle(device); vkDestroyDevice(device, NULL);
        }
        vkDestroyInstance(instance, NULL);
    }
done:
    printf(failures ? "WoWPS5 Win32 vulkan noscreen test FAIL\n" : "WoWPS5 Win32 vulkan noscreen test PASS\n");
    return failures;
}
