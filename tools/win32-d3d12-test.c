/* DirectX 12 through Wine with no window. MIT license.
 * d3d12.dll (vkd3d-proton, placed beside this program) -> Vulkan -> the GPU:
 * a device, a render target cleared on the GPU, copied to a readback buffer
 * and checked. No swapchain, so no display is needed.
 * Each check prints PASS or FAIL with the HRESULT; the exit status is the
 * number of failures. Run with WINEDLLOVERRIDES=d3d12,d3d12core=n. */
#define COBJMACROS
#define WIDL_C_INLINE_WRAPPERS   /* for the calls that return a structure */
#include <windows.h>
#include <d3d12.h>
#include <stdio.h>
#include <string.h>

static int failures;
static int check(HRESULT result, const char* what) {
    if(FAILED(result)) failures++;
    printf("%s: %s (0x%08lx)\n", FAILED(result) ? "FAIL" : "PASS", what, (unsigned long)result);
    fflush(stdout);
    return SUCCEEDED(result);
}

int main(void) {
    HMODULE library = LoadLibraryA("d3d12.dll");
    typedef HRESULT (WINAPI *CreateDevice)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
    CreateDevice create = library ? (CreateDevice)GetProcAddress(library, "D3D12CreateDevice") : NULL;
    char path[MAX_PATH] = "";
    if(library) GetModuleFileNameA(library, path, sizeof(path));
    printf("info d3d12.dll is %s\n", path);
    if(!check(create ? S_OK : HRESULT_FROM_WIN32(GetLastError()), "d3d12.dll loads")) goto done;

    ID3D12Device* device = NULL;
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_12_0;
    HRESULT result = create(NULL, level, &IID_ID3D12Device, (void**)&device);
    if(FAILED(result)) { level = D3D_FEATURE_LEVEL_11_0; result = create(NULL, level, &IID_ID3D12Device, (void**)&device); }
    if(!check(result, "D3D12CreateDevice")) goto done;
    static const D3D_FEATURE_LEVEL wanted[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_2 };
    D3D12_FEATURE_DATA_FEATURE_LEVELS levels = { sizeof(wanted) / sizeof(*wanted), wanted, 0 };
    if(SUCCEEDED(ID3D12Device_CheckFeatureSupport(device, D3D12_FEATURE_FEATURE_LEVELS, &levels, sizeof(levels))))
        printf("info highest feature level 0x%x\n", levels.MaxSupportedFeatureLevel);
    D3D12_FEATURE_DATA_SHADER_MODEL model = { D3D_SHADER_MODEL_6_6 };
    if(SUCCEEDED(ID3D12Device_CheckFeatureSupport(device, D3D12_FEATURE_SHADER_MODEL, &model, sizeof(model))))
        printf("info highest shader model 0x%x\n", model.HighestShaderModel);

    ID3D12CommandQueue* queue = NULL; ID3D12CommandAllocator* allocator = NULL; ID3D12GraphicsCommandList* list = NULL;
    D3D12_COMMAND_QUEUE_DESC queueDesc = { .Type = D3D12_COMMAND_LIST_TYPE_DIRECT };
    result = ID3D12Device_CreateCommandQueue(device, &queueDesc, &IID_ID3D12CommandQueue, (void**)&queue);
    if(SUCCEEDED(result)) result = ID3D12Device_CreateCommandAllocator(device, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void**)&allocator);
    if(SUCCEEDED(result)) result = ID3D12Device_CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, NULL, &IID_ID3D12GraphicsCommandList, (void**)&list);
    if(!check(result, "a command queue, allocator and list")) goto done;

    const UINT size = 64;
    D3D12_HEAP_PROPERTIES defaultHeap = { .Type = D3D12_HEAP_TYPE_DEFAULT }, readbackHeap = { .Type = D3D12_HEAP_TYPE_READBACK };
    D3D12_RESOURCE_DESC targetDesc = { .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D, .Width = size, .Height = size, .DepthOrArraySize = 1, .MipLevels = 1,
        .Format = DXGI_FORMAT_R8G8B8A8_UNORM, .SampleDesc = { 1, 0 }, .Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET };
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint; UINT64 bytes = 0;
    ID3D12Device_GetCopyableFootprints(device, &targetDesc, 0, 1, 0, &footprint, NULL, NULL, &bytes);
    D3D12_RESOURCE_DESC bufferDesc = { .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER, .Width = bytes, .Height = 1, .DepthOrArraySize = 1, .MipLevels = 1,
        .SampleDesc = { 1, 0 }, .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR };
    const float colour[4] = { 1.0f, 0.5f, 0.25f, 1.0f };
    D3D12_CLEAR_VALUE clear = { .Format = DXGI_FORMAT_R8G8B8A8_UNORM, .Color = { 1.0f, 0.5f, 0.25f, 1.0f } };
    ID3D12Resource* target = NULL; ID3D12Resource* readback = NULL; ID3D12DescriptorHeap* heap = NULL;
    result = ID3D12Device_CreateCommittedResource(device, &defaultHeap, D3D12_HEAP_FLAG_NONE, &targetDesc, D3D12_RESOURCE_STATE_RENDER_TARGET, &clear, &IID_ID3D12Resource, (void**)&target);
    if(SUCCEEDED(result)) result = ID3D12Device_CreateCommittedResource(device, &readbackHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc, D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void**)&readback);
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = { .Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV, .NumDescriptors = 1 };
    if(SUCCEEDED(result)) result = ID3D12Device_CreateDescriptorHeap(device, &heapDesc, &IID_ID3D12DescriptorHeap, (void**)&heap);
    if(!check(result, "a render target, a readback buffer and a descriptor heap")) goto done;

    D3D12_CPU_DESCRIPTOR_HANDLE view = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(heap);
    ID3D12Device_CreateRenderTargetView(device, target, NULL, view);
    ID3D12GraphicsCommandList_ClearRenderTargetView(list, view, colour, 0, NULL);
    D3D12_RESOURCE_BARRIER barrier = { .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
        .Transition = { target, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE } };
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &barrier);
    D3D12_TEXTURE_COPY_LOCATION from = { .pResource = target, .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, .SubresourceIndex = 0 };
    D3D12_TEXTURE_COPY_LOCATION to = { .pResource = readback, .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, .PlacedFootprint = footprint };
    ID3D12GraphicsCommandList_CopyTextureRegion(list, &to, 0, 0, 0, &from, NULL);
    result = ID3D12GraphicsCommandList_Close(list);
    ID3D12Fence* fence = NULL; HANDLE event = CreateEventA(NULL, FALSE, FALSE, NULL);
    if(SUCCEEDED(result)) result = ID3D12Device_CreateFence(device, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void**)&fence);
    if(SUCCEEDED(result)) {
        ID3D12CommandList* lists[] = { (ID3D12CommandList*)list };
        ID3D12CommandQueue_ExecuteCommandLists(queue, 1, lists);
        result = ID3D12CommandQueue_Signal(queue, fence, 1);
        if(SUCCEEDED(result)) result = ID3D12Fence_SetEventOnCompletion(fence, 1, event);
        if(SUCCEEDED(result) && WaitForSingleObject(event, 20000) != WAIT_OBJECT_0) result = HRESULT_FROM_WIN32(WAIT_TIMEOUT);
    }
    if(!check(result, "commands executed and the fence signalled")) goto done;

    unsigned char* pixels = NULL; unsigned right = 0;
    D3D12_RANGE whole = { 0, (SIZE_T)bytes };
    result = ID3D12Resource_Map(readback, 0, &whole, (void**)&pixels);
    for(UINT y = 0; SUCCEEDED(result) && y < size; y++) for(UINT x = 0; x < size; x++) {
        const unsigned char* pixel = pixels + footprint.Offset + y * footprint.Footprint.RowPitch + x * 4;
        right += pixel[0] == 255 && pixel[1] >= 127 && pixel[1] <= 128 && pixel[2] >= 63 && pixel[2] <= 64 && pixel[3] == 255;
    }
    if(pixels) printf("info first pixel %u %u %u %u\n", pixels[footprint.Offset], pixels[footprint.Offset + 1], pixels[footprint.Offset + 2], pixels[footprint.Offset + 3]);
    check(SUCCEEDED(result) && right == size * size ? S_OK : E_FAIL, "the render target the GPU cleared reads back correct");
done:
    printf(failures ? "WoWPS5 Win32 d3d12 test FAIL\n" : "WoWPS5 Win32 d3d12 test PASS\n");
    return failures;
}
