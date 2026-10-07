/* DirectX 12 presenting to a window through Wine. MIT license.
 * d3d12.dll and d3d12core.dll (vkd3d-proton) and dxgi.dll (DXVK's) beside this
 * program: a borderless window the size of the display, a flip-model swapchain
 * on it, each frame cleared to a changing colour and presented with vsync.
 *   win32-d3d12-present-test.exe [seconds]      (default 5)
 * Run with WINEDLLOVERRIDES=d3d12,d3d12core,dxgi=n. The exit status is 0 only
 * when frames were presented. */
#define COBJMACROS
#define WIDL_C_INLINE_WRAPPERS
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <stdio.h>
#include <stdlib.h>

static int failures;
static int check(HRESULT result, const char* what) {
    if(FAILED(result)) failures++;
    printf("%s: %s (0x%08lx)\n", FAILED(result) ? "FAIL" : "PASS", what, (unsigned long)result);
    fflush(stdout);
    return SUCCEEDED(result);
}
static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM w, LPARAM l) { return DefWindowProcA(window, message, w, l); }
static void pump(void) { MSG message; while(PeekMessageA(&message, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageA(&message); } }

int main(int argc, char** argv) {
    const DWORD run = (argc > 1 ? (DWORD)atoi(argv[1]) : 5) * 1000;
    WNDCLASSA class = { .lpfnWndProc = procedure, .hInstance = GetModuleHandleA(NULL), .lpszClassName = "WoWPS5D3D12" };
    RegisterClassA(&class);
    const int width = GetSystemMetrics(SM_CXSCREEN), height = GetSystemMetrics(SM_CYSCREEN);
    HWND window = CreateWindowExA(0, class.lpszClassName, "WoWPS5 D3D12", WS_POPUP | WS_VISIBLE, 0, 0, width, height, NULL, NULL, class.hInstance, NULL);
    printf("info display %d x %d\n", width, height);
    if(!check(window ? S_OK : HRESULT_FROM_WIN32(GetLastError()), "a window the size of the display")) goto done;
    pump();

    ID3D12Device* device = NULL; IDXGIFactory4* factory = NULL;
    HRESULT result = D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void**)&device);
    if(!check(result, "D3D12CreateDevice")) goto done;
    if(!check(CreateDXGIFactory1(&IID_IDXGIFactory4, (void**)&factory), "CreateDXGIFactory1")) goto done;
    IDXGIAdapter1* adapter = NULL; DXGI_ADAPTER_DESC1 description;
    if(SUCCEEDED(IDXGIFactory4_EnumAdapters1(factory, 0, &adapter)) && SUCCEEDED(IDXGIAdapter1_GetDesc1(adapter, &description)))
        printf("info adapter \"%ls\", %llu MiB dedicated\n", description.Description, (unsigned long long)(description.DedicatedVideoMemory >> 20));

    ID3D12CommandQueue* queue = NULL; ID3D12CommandAllocator* allocator = NULL; ID3D12GraphicsCommandList* list = NULL;
    D3D12_COMMAND_QUEUE_DESC queueDesc = { .Type = D3D12_COMMAND_LIST_TYPE_DIRECT };
    result = ID3D12Device_CreateCommandQueue(device, &queueDesc, &IID_ID3D12CommandQueue, (void**)&queue);
    if(SUCCEEDED(result)) result = ID3D12Device_CreateCommandAllocator(device, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void**)&allocator);
    if(SUCCEEDED(result)) result = ID3D12Device_CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, NULL, &IID_ID3D12GraphicsCommandList, (void**)&list);
    if(SUCCEEDED(result)) result = ID3D12GraphicsCommandList_Close(list);
    if(!check(result, "a command queue, allocator and list")) goto done;

    enum { BUFFERS = 3 };
    DXGI_SWAP_CHAIN_DESC1 swapDesc = { .Width = (UINT)width, .Height = (UINT)height, .Format = DXGI_FORMAT_R8G8B8A8_UNORM, .SampleDesc = { 1, 0 },
        .BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT, .BufferCount = BUFFERS, .SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD };
    IDXGISwapChain1* first = NULL; IDXGISwapChain3* swapchain = NULL;
    result = IDXGIFactory4_CreateSwapChainForHwnd(factory, (IUnknown*)queue, window, &swapDesc, NULL, NULL, &first);
    if(SUCCEEDED(result)) result = IDXGISwapChain1_QueryInterface(first, &IID_IDXGISwapChain3, (void**)&swapchain);
    if(!check(result, "a flip-model swapchain on the window")) goto done;

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = { .Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV, .NumDescriptors = BUFFERS };
    ID3D12DescriptorHeap* heap = NULL; ID3D12Resource* buffers[BUFFERS] = { 0 }; D3D12_CPU_DESCRIPTOR_HANDLE views[BUFFERS];
    result = ID3D12Device_CreateDescriptorHeap(device, &heapDesc, &IID_ID3D12DescriptorHeap, (void**)&heap);
    const UINT step = ID3D12Device_GetDescriptorHandleIncrementSize(device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    for(UINT i = 0; SUCCEEDED(result) && i < BUFFERS; i++) {
        result = IDXGISwapChain3_GetBuffer(swapchain, i, &IID_ID3D12Resource, (void**)&buffers[i]);
        views[i] = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(heap); views[i].ptr += i * step;
        if(SUCCEEDED(result)) ID3D12Device_CreateRenderTargetView(device, buffers[i], NULL, views[i]);
    }
    ID3D12Fence* fence = NULL; HANDLE event = CreateEventA(NULL, FALSE, FALSE, NULL); UINT64 value = 0;
    if(SUCCEEDED(result)) result = ID3D12Device_CreateFence(device, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void**)&fence);
    if(!check(result, "the swapchain's buffers as render targets")) goto done;

    const DWORD start = GetTickCount(); DWORD report = start; unsigned frames = 0;
    while(GetTickCount() - start < run) {
        pump();
        const UINT index = IDXGISwapChain3_GetCurrentBackBufferIndex(swapchain);
        const float phase = (float)((GetTickCount() - start) % 3000) / 3000.0f;
        const float colour[4] = { phase, 0.25f, 1.0f - phase, 1.0f };
        D3D12_RESOURCE_BARRIER barrier = { .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
            .Transition = { buffers[index], D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET } };
        result = ID3D12CommandAllocator_Reset(allocator);
        if(SUCCEEDED(result)) result = ID3D12GraphicsCommandList_Reset(list, allocator, NULL);
        if(FAILED(result)) break;
        ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &barrier);
        ID3D12GraphicsCommandList_ClearRenderTargetView(list, views[index], colour, 0, NULL);
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET; barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &barrier);
        result = ID3D12GraphicsCommandList_Close(list);
        if(FAILED(result)) break;
        ID3D12CommandList* lists[] = { (ID3D12CommandList*)list };
        ID3D12CommandQueue_ExecuteCommandLists(queue, 1, lists);
        result = IDXGISwapChain3_Present(swapchain, 1, 0);
        if(FAILED(result)) break;
        result = ID3D12CommandQueue_Signal(queue, fence, ++value);
        if(SUCCEEDED(result) && ID3D12Fence_GetCompletedValue(fence) < value) {
            result = ID3D12Fence_SetEventOnCompletion(fence, value, event);
            if(SUCCEEDED(result) && WaitForSingleObject(event, 20000) != WAIT_OBJECT_0) result = HRESULT_FROM_WIN32(WAIT_TIMEOUT);
        }
        if(FAILED(result)) break;
        frames++;
        if(GetTickCount() - report >= 1000) { report = GetTickCount(); printf("info %u frames after %lu ms\n", frames, report - start); fflush(stdout); }
    }
    const DWORD elapsed = GetTickCount() - start;
    printf("info %u frames in %lu ms (%.1f per second)\n", frames, elapsed, elapsed ? frames * 1000.0 / elapsed : 0.0);
    check(FAILED(result) ? result : frames > 10 ? S_OK : E_FAIL, "frames presented");
done:
    printf(failures ? "WoWPS5 Win32 d3d12 present test FAIL\n" : "WoWPS5 Win32 d3d12 present test PASS\n");
    return failures;
}
