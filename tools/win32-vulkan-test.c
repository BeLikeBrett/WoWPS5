/* Step c of the single-process desktop work: a window, a Vulkan surface and
 * swapchain on it through vulkan-1.dll (winevulkan), frames cleared to a
 * changing colour and presented. MIT license.
 *
 *   win32-vulkan-test.exe [seconds] [device-index] [fullscreen]     (default 3 seconds, first usable device, a 640x360 window)
 *
 * vulkan-1.dll is loaded at run time, so a missing loader is a reported
 * failure and not a failed program start. Every check prints PASS or FAIL
 * with the VkResult; the first failure ends the run. The exit status is 0
 * only when frames were presented.
 *
 * Build: needs the Vulkan headers (any platform's copy), see
 * tools/wine-ps5/build-desktop-tests.sh. */
#define VK_USE_PLATFORM_WIN32_KHR
#define VK_NO_PROTOTYPES
#include <windows.h>
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int paints, sizes, closes;

static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch(message) {
    case WM_PAINT: paints++; ValidateRect(window, NULL); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: sizes++; return 0;
    case WM_CLOSE: closes++; return 0;
    }
    return DefWindowProcA(window, message, wparam, lparam);
}

static void pump(void) {
    MSG message;
    while(PeekMessageA(&message, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageA(&message);
    }
}

static int step(VkResult result, const char* what) {
    printf("%s: %s (VkResult %d)\n", result >= 0 ? "PASS" : "FAIL", what, (int)result);
    fflush(stdout);
    return result >= 0;
}

static int fail(const char* what) {
    printf("FAIL: %s (GetLastError=%lu)\nWoWPS5 Vulkan test FAIL\n", what, GetLastError());
    fflush(stdout);
    return 1;
}

/* every Vulkan entry point the test calls, resolved in three stages */
#define GLOBAL_FUNCTIONS(F) F(vkCreateInstance) F(vkEnumerateInstanceExtensionProperties)
#define INSTANCE_FUNCTIONS(F) F(vkCreateWin32SurfaceKHR) F(vkEnumeratePhysicalDevices) F(vkGetPhysicalDeviceProperties) F(vkGetPhysicalDeviceQueueFamilyProperties) F(vkGetPhysicalDeviceSurfaceSupportKHR) F(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) F(vkGetPhysicalDeviceSurfaceFormatsKHR) F(vkCreateDevice) F(vkGetDeviceProcAddr) F(vkDestroySurfaceKHR) F(vkDestroyInstance)
#define DEVICE_FUNCTIONS(F) F(vkGetDeviceQueue) F(vkCreateSwapchainKHR) F(vkGetSwapchainImagesKHR) F(vkCreateCommandPool) F(vkAllocateCommandBuffers) F(vkCreateSemaphore) F(vkCreateFence) F(vkAcquireNextImageKHR) F(vkWaitForFences) F(vkResetFences) F(vkBeginCommandBuffer) F(vkEndCommandBuffer) F(vkCmdPipelineBarrier) F(vkCmdClearColorImage) F(vkQueueSubmit) F(vkQueuePresentKHR) F(vkDeviceWaitIdle) F(vkDestroySwapchainKHR) F(vkDestroyCommandPool) F(vkDestroySemaphore) F(vkDestroyFence) F(vkDestroyDevice)
#define DECLARE(name) static PFN_##name name;
GLOBAL_FUNCTIONS(DECLARE) INSTANCE_FUNCTIONS(DECLARE) DEVICE_FUNCTIONS(DECLARE)
#define LOAD_INSTANCE(name) if(!(name = (PFN_##name)get_instance_proc(instance, #name))) missing = #name;
#define LOAD_DEVICE(name) if(!(name = (PFN_##name)vkGetDeviceProcAddr(device, #name))) missing = #name;
#define STEP(call, what) do { if(!step((call), what)) goto failed; } while(0)

int main(int argc, char** argv) {
    DWORD run_ms = (argc > 1 ? (DWORD)atoi(argv[1]) : 3) * 1000, start, last_report;
    int wanted_device = argc > 2 ? atoi(argv[2]) : -1;
    WNDCLASSEXA window_class = {sizeof window_class};
    RECT rect = {0, 0, 640, 360};
    HMODULE loader;
    HWND window;
    PFN_vkGetInstanceProcAddr get_instance_proc;
    VkInstance instance = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkPhysicalDevice physical_devices[8], physical = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties properties;
    VkQueueFamilyProperties families[16];
    VkSurfaceCapabilitiesKHR caps;
    VkSurfaceFormatKHR formats[64], format;
    VkImage images[16];
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer commands;
    VkSemaphore acquired = VK_NULL_HANDLE, rendered = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkQueue queue;
    VkResult result;
    uint32_t count, i, family = UINT32_MAX, image_count, frames = 0, suboptimal = 0, out_of_date = 0, index;
    static const char* const instance_extensions[] = {"VK_KHR_surface", "VK_KHR_win32_surface"};
    static const char* const device_extensions[] = {"VK_KHR_swapchain"};
    const char* missing = NULL;
    int status = 1;

    printf("WoWPS5 Vulkan test: pid %lu thread %lu\n", GetCurrentProcessId(), GetCurrentThreadId());
    fflush(stdout);

    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = GetModuleHandleA(NULL);
    window_class.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    window_class.lpszClassName = "WoWPS5VulkanTest";
    if(!RegisterClassExA(&window_class)) return fail("RegisterClassEx");
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    /* "fullscreen" as the third argument: a borderless window the size of the display, as a
     * game's is. A display that is a fixed-size output only takes a swapchain of its own size. */
    if(argc > 3 && !strcmp(argv[3], "fullscreen"))
        window = CreateWindowExA(0, window_class.lpszClassName, "WoWPS5 Vulkan test", WS_POPUP, 0, 0,
                                 GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), NULL, NULL, window_class.hInstance, NULL);
    else
    window = CreateWindowExA(0, window_class.lpszClassName, "WoWPS5 Vulkan test", WS_OVERLAPPEDWINDOW, 120, 120,
                             rect.right - rect.left, rect.bottom - rect.top, NULL, NULL, window_class.hInstance, NULL);
    if(!window) return fail("CreateWindowEx");
    printf("PASS: CreateWindowEx\n");
    ShowWindow(window, SW_SHOWNOACTIVATE);
    pump();

    loader = LoadLibraryA("vulkan-1.dll");
    if(!loader) return fail("LoadLibrary vulkan-1.dll");
    get_instance_proc = (PFN_vkGetInstanceProcAddr)(void*)GetProcAddress(loader, "vkGetInstanceProcAddr");
    if(!get_instance_proc) return fail("vkGetInstanceProcAddr export");
    printf("PASS: vulkan-1.dll loaded\n");
    fflush(stdout);

    GLOBAL_FUNCTIONS(LOAD_INSTANCE)   /* instance is still null here */
    if(missing) STEP(VK_ERROR_INITIALIZATION_FAILED, missing);
    {
        VkApplicationInfo application = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "win32-vulkan-test", 1, "none", 0, VK_API_VERSION_1_1};
        VkInstanceCreateInfo info = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &application, 0, NULL, 2, instance_extensions};
        VkExtensionProperties extensions[64];
        int has_win32 = 0;
        count = 64;
        result = vkEnumerateInstanceExtensionProperties(NULL, &count, extensions);
        if(result < 0) STEP(result, "vkEnumerateInstanceExtensionProperties");
        for(i = 0; i < count; i++) if(!strcmp(extensions[i].extensionName, "VK_KHR_win32_surface")) has_win32 = 1;
        printf("INFO: %u instance extensions, VK_KHR_win32_surface %s\n", count, has_win32 ? "present" : "ABSENT");
        STEP(vkCreateInstance(&info, NULL, &instance), "vkCreateInstance with VK_KHR_surface, VK_KHR_win32_surface");
    }
    INSTANCE_FUNCTIONS(LOAD_INSTANCE)
    if(missing) STEP(VK_ERROR_INITIALIZATION_FAILED, missing);
    {
        VkWin32SurfaceCreateInfoKHR surface_info = {VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR, NULL, 0, window_class.hInstance, window};
        float priority = 1.0f;
        VkDeviceQueueCreateInfo queue_info = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, 0, 1, &priority};
        VkDeviceCreateInfo device_info = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &queue_info, 0, NULL, 1, device_extensions, NULL};

        STEP(vkCreateWin32SurfaceKHR(instance, &surface_info, NULL, &surface), "vkCreateWin32SurfaceKHR on the window");

        count = 8;
        result = vkEnumeratePhysicalDevices(instance, &count, physical_devices);
        if(result < 0 || !count) STEP(result < 0 ? result : VK_ERROR_INITIALIZATION_FAILED, "vkEnumeratePhysicalDevices");
        for(i = 0; i < count; i++) {
            uint32_t family_count = 16, j, usable = UINT32_MAX;
            vkGetPhysicalDeviceProperties(physical_devices[i], &properties);
            vkGetPhysicalDeviceQueueFamilyProperties(physical_devices[i], &family_count, families);
            for(j = 0; j < family_count; j++) {
                VkBool32 supported = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(physical_devices[i], j, surface, &supported);
                if(supported && (families[j].queueFlags & VK_QUEUE_GRAPHICS_BIT)) { usable = j; break; }
            }
            printf("INFO: device %u: %s (type %d), %s\n", i, properties.deviceName, (int)properties.deviceType,
                   usable == UINT32_MAX ? "cannot present to this surface" : "can present to this surface");
            if(usable != UINT32_MAX && physical == VK_NULL_HANDLE && (wanted_device < 0 || wanted_device == (int)i)) {
                physical = physical_devices[i];
                family = usable;
            }
        }
        if(physical == VK_NULL_HANDLE) STEP(VK_ERROR_FEATURE_NOT_PRESENT, "a device with a graphics queue that can present to the surface");
        vkGetPhysicalDeviceProperties(physical, &properties);
        printf("PASS: using %s, queue family %u\n", properties.deviceName, family);

        queue_info.queueFamilyIndex = family;
        STEP(vkCreateDevice(physical, &device_info, NULL, &device), "vkCreateDevice with VK_KHR_swapchain");
        STEP(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &caps), "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
        count = 64;
        result = vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count, formats);
        if(result < 0 || !count) STEP(result < 0 ? result : VK_ERROR_FORMAT_NOT_SUPPORTED, "vkGetPhysicalDeviceSurfaceFormatsKHR");
        format = formats[0];
        for(i = 0; i < count; i++) if(formats[i].format == VK_FORMAT_B8G8R8A8_UNORM) format = formats[i];
        printf("INFO: surface extent %ux%u (min %ux%u, max %ux%u), images %u..%u, usage %#x, format %d\n",
               caps.currentExtent.width, caps.currentExtent.height, caps.minImageExtent.width, caps.minImageExtent.height,
               caps.maxImageExtent.width, caps.maxImageExtent.height, caps.minImageCount, caps.maxImageCount,
               (unsigned)caps.supportedUsageFlags, (int)format.format);
        if(!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT))
            STEP(VK_ERROR_FEATURE_NOT_PRESENT, "swapchain images usable as a clear target (TRANSFER_DST)");

        DEVICE_FUNCTIONS(LOAD_DEVICE)
        if(missing) STEP(VK_ERROR_INITIALIZATION_FAILED, missing);
        {
            VkSwapchainCreateInfoKHR swapchain_info = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
            VkCommandPoolCreateInfo pool_info = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, family};
            VkCommandBufferAllocateInfo allocate = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, VK_NULL_HANDLE, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
            VkSemaphoreCreateInfo semaphore_info = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            VkFenceCreateInfo fence_info = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, VK_FENCE_CREATE_SIGNALED_BIT};
            VkExtent2D extent = caps.currentExtent;

            if(extent.width == UINT32_MAX) { extent.width = 640; extent.height = 360; }
            swapchain_info.surface = surface;
            swapchain_info.minImageCount = caps.minImageCount < 2 ? 2 : caps.minImageCount;
            if(caps.maxImageCount && swapchain_info.minImageCount > caps.maxImageCount) swapchain_info.minImageCount = caps.maxImageCount;
            swapchain_info.imageFormat = format.format;
            swapchain_info.imageColorSpace = format.colorSpace;
            swapchain_info.imageExtent = extent;
            swapchain_info.imageArrayLayers = 1;
            swapchain_info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            swapchain_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
            swapchain_info.preTransform = caps.currentTransform;
            swapchain_info.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
                                                ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
            swapchain_info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
            swapchain_info.clipped = VK_TRUE;
            STEP(vkCreateSwapchainKHR(device, &swapchain_info, NULL, &swapchain), "vkCreateSwapchainKHR (FIFO)");
            image_count = 16;
            STEP(vkGetSwapchainImagesKHR(device, swapchain, &image_count, images), "vkGetSwapchainImagesKHR");
            printf("INFO: swapchain %ux%u with %u images\n", extent.width, extent.height, image_count);

            vkGetDeviceQueue(device, family, 0, &queue);
            STEP(vkCreateCommandPool(device, &pool_info, NULL, &pool), "vkCreateCommandPool");
            allocate.commandPool = pool;
            STEP(vkAllocateCommandBuffers(device, &allocate, &commands), "vkAllocateCommandBuffers");
            STEP(vkCreateSemaphore(device, &semaphore_info, NULL, &acquired), "vkCreateSemaphore");
            STEP(vkCreateSemaphore(device, &semaphore_info, NULL, &rendered), "vkCreateSemaphore");
            STEP(vkCreateFence(device, &fence_info, NULL, &fence), "vkCreateFence");

            start = last_report = GetTickCount();
            while(GetTickCount() - start < run_ms) {
                VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
                VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, VK_NULL_HANDLE, range};
                VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
                VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 1, &acquired, &wait_stage, 1, &commands, 1, &rendered};
                VkPresentInfoKHR present = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, NULL, 1, &rendered, 1, &swapchain, &index, NULL};
                float phase = (float)(frames % 120) / 120.0f;
                VkClearColorValue color = {{phase, 0.25f, 1.0f - phase, 1.0f}};

                pump();
                result = vkWaitForFences(device, 1, &fence, VK_TRUE, 2000000000ull);
                if(result != VK_SUCCESS) STEP(result == VK_TIMEOUT ? VK_ERROR_DEVICE_LOST : result, "vkWaitForFences (previous frame)");
                result = vkAcquireNextImageKHR(device, swapchain, 2000000000ull, acquired, VK_NULL_HANDLE, &index);
                if(result == VK_SUBOPTIMAL_KHR) suboptimal++;
                else if(result == VK_ERROR_OUT_OF_DATE_KHR) { out_of_date++; if(out_of_date > 100) STEP(result, "vkAcquireNextImageKHR"); Sleep(10); continue; }
                else if(result != VK_SUCCESS) STEP(result == VK_TIMEOUT || result == VK_NOT_READY ? VK_ERROR_DEVICE_LOST : result, "vkAcquireNextImageKHR");
                vkResetFences(device, 1, &fence);

                barrier.image = images[index];
                vkBeginCommandBuffer(commands, &begin);
                vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
                vkCmdClearColorImage(commands, images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
                barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                barrier.dstAccessMask = 0;
                barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
                vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
                vkEndCommandBuffer(commands);
                result = vkQueueSubmit(queue, 1, &submit, fence);
                if(result != VK_SUCCESS) STEP(result, "vkQueueSubmit");
                result = vkQueuePresentKHR(queue, &present);
                if(result == VK_SUBOPTIMAL_KHR) suboptimal++;
                else if(result == VK_ERROR_OUT_OF_DATE_KHR) out_of_date++;
                else if(result != VK_SUCCESS) STEP(result, "vkQueuePresentKHR");
                if(!frames) { printf("PASS: first frame submitted and presented\n"); fflush(stdout); }
                frames++;
                if(GetTickCount() - last_report >= 1000) {
                    printf("INFO: %u frames after %lu ms\n", frames, GetTickCount() - start);
                    fflush(stdout);
                    last_report = GetTickCount();
                }
            }
            vkDeviceWaitIdle(device);
            printf("INFO: %u frames in %lu ms (%.1f per second), %u suboptimal, %u out of date; WM_PAINT %d, WM_SIZE %d\n",
                   frames, GetTickCount() - start, frames * 1000.0 / (GetTickCount() - start + 1), suboptimal, out_of_date, paints, sizes);
            if(frames >= 10) { printf("PASS: frames presented\n"); status = 0; }
            else printf("FAIL: fewer than 10 frames presented\n");

        }
    }
failed:
    if(device && vkDeviceWaitIdle) vkDeviceWaitIdle(device);
    if(fence) vkDestroyFence(device, fence, NULL);
    if(acquired) vkDestroySemaphore(device, acquired, NULL);
    if(rendered) vkDestroySemaphore(device, rendered, NULL);
    if(pool) vkDestroyCommandPool(device, pool, NULL);
    if(swapchain) vkDestroySwapchainKHR(device, swapchain, NULL);
    if(device && vkDestroyDevice) vkDestroyDevice(device, NULL);
    if(surface) vkDestroySurfaceKHR(instance, surface, NULL);
    if(instance) vkDestroyInstance(instance, NULL);
    DestroyWindow(window);
    pump();
    printf("WoWPS5 Vulkan test %s\n", status ? "FAIL" : "PASS");
    fflush(stdout);
    return status;
}
