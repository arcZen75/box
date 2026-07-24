#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wrl.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <string>
#include <vector>
#include <stdexcept>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

static const UINT FrameCount = 2;

struct Vertex {
    XMFLOAT3 pos;
    XMFLOAT4 color;
};

static const char* g_shaderCode = R"(
struct VSIn { float3 pos : POSITION; float4 color : COLOR; };
struct PSIn { float4 pos : SV_POSITION; float4 color : COLOR; };

PSIn VSMain(VSIn input) {
    PSIn o;
    o.pos = float4(input.pos, 1.0);
    o.color = input.color;
    return o;
}

float4 PSMain(PSIn input) : SV_TARGET {
    return input.color;
}
)";

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

static void ThrowIfFailed(HRESULT hr) {
    if (FAILED(hr)) throw std::runtime_error("HRESULT failed");
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int) {
    try {
        WNDCLASSEX wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = WndProc;
        wc.hInstance = hInstance;
        wc.lpszClassName = L"OverlayBoxClass";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassEx(&wc);

        HWND hwnd = CreateWindowEx(
            WS_EX_TOPMOST | WS_EX_LAYERED,
            wc.lpszClassName,
            L"OverlayBox",
            WS_POPUP,
            100, 100, 1280, 720,
            nullptr, nullptr, hInstance, nullptr
        );

        ThrowIfFailed(hwnd ? S_OK : E_FAIL);
        SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
        ShowWindow(hwnd, SW_SHOW);

        UINT dxgiFactoryFlags = 0;
#ifdef _DEBUG
        {
            ComPtr<ID3D12Debug> debug;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
                debug->EnableDebugLayer();
                dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
            }
        }
#endif

        ComPtr<IDXGIFactory4> factory;
        ThrowIfFailed(CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(&factory)));

        ComPtr<ID3D12Device> device;
        ThrowIfFailed(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));

        ComPtr<ID3D12CommandQueue> commandQueue;
        D3D12_COMMAND_QUEUE_DESC qdesc{};
        qdesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ThrowIfFailed(device->CreateCommandQueue(&qdesc, IID_PPV_ARGS(&commandQueue)));

        DXGI_SWAP_CHAIN_DESC1 scDesc{};
        scDesc.BufferCount = FrameCount;
        scDesc.Width = 1280;
        scDesc.Height = 720;
        scDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        scDesc.SampleDesc.Count = 1;
        scDesc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

        ComPtr<IDXGISwapChain1> swapChain1;
        ThrowIfFailed(factory->CreateSwapChainForHwnd(
            commandQueue.Get(),
            hwnd,
            &scDesc,
            nullptr,
            nullptr,
            &swapChain1
        ));

        ComPtr<IDXGISwapChain3> swapChain;
        ThrowIfFailed(swapChain1.As(&swapChain));

        ComPtr<ID3D12DescriptorHeap> rtvHeap;
        D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
        rtvHeapDesc.NumDescriptors = FrameCount;
        rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        ThrowIfFailed(device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&rtvHeap)));

        UINT rtvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

        ComPtr<ID3D12Resource> renderTargets[FrameCount];
        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        for (UINT i = 0; i < FrameCount; i++) {
            ThrowIfFailed(swapChain->GetBuffer(i, IID_PPV_ARGS(&renderTargets[i])));
            device->CreateRenderTargetView(renderTargets[i].Get(), nullptr, rtvHandle);
            rtvHandle.ptr += rtvDescriptorSize;
        }

        ComPtr<ID3D12CommandAllocator> commandAllocator;
        ThrowIfFailed(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&commandAllocator)));

        ComPtr<ID3D12GraphicsCommandList> commandList;
        ThrowIfFailed(device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, commandAllocator.Get(), nullptr, IID_PPV_ARGS(&commandList)));

        ThrowIfFailed(commandList->Close());

        ComPtr<ID3D12Fence> fence;
        ThrowIfFailed(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
        HANDLE fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

        UINT64 fenceValue = 1;
        UINT frameIndex = swapChain->GetCurrentBackBufferIndex();

        D3D12_ROOT_SIGNATURE_DESC rsDesc{};
        rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        ComPtr<ID3DBlob> sigBlob, errBlob;
        ThrowIfFailed(D3D12SerializeRootSignature(
            &rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &errBlob));

        ComPtr<ID3D12RootSignature> rootSignature;
        ThrowIfFailed(device->CreateRootSignature(
            0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(), IID_PPV_ARGS(&rootSignature)));

        ComPtr<ID3DBlob> vsBlob, psBlob, shaderErr;
        ThrowIfFailed(D3DCompile(g_shaderCode, strlen(g_shaderCode), nullptr, nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, &vsBlob, &shaderErr));
        ThrowIfFailed(D3DCompile(g_shaderCode, strlen(g_shaderCode), nullptr, nullptr, nullptr, "PSMain", "ps_5_0", 0, 0, &psBlob, &shaderErr));

        D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        };

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.InputLayout = { inputLayout, _countof(inputLayout) };
        psoDesc.pRootSignature = rootSignature.Get();
        psoDesc.VS = { vsBlob->GetBufferPointer(), vsBlob->GetBufferSize() };
        psoDesc.PS = { psBlob->GetBufferPointer(), psBlob->GetBufferSize() };
        psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        psoDesc.DepthStencilState.DepthEnable = FALSE;
        psoDesc.DepthStencilState.StencilEnable = FALSE;
        psoDesc.SampleMask = UINT_MAX;
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.NumRenderTargets = 1;
        psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        psoDesc.SampleDesc.Count = 1;

        ComPtr<ID3D12PipelineState> pipelineState;
        ThrowIfFailed(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pipelineState)));

        Vertex vertices[] = {
            {{-0.5f,  0.5f, 0.0f}, {0.0f, 1.0f, 0.0f, 1.0f}},
            {{ 0.5f,  0.5f, 0.0f}, {0.0f, 1.0f, 0.0f, 1.0f}},
            {{ 0.5f, -0.5f, 0.0f}, {0.0f, 1.0f, 0.0f, 1.0f}},
            {{-0.5f,  0.5f, 0.0f}, {0.0f, 1.0f, 0.0f, 1.0f}},
            {{ 0.5f, -0.5f, 0.0f}, {0.0f, 1.0f, 0.0f, 1.0f}},
            {{-0.5f, -0.5f, 0.0f}, {0.0f, 1.0f, 0.0f, 1.0f}},
        };

        ComPtr<ID3D12Resource> vertexBuffer;
        const UINT vbSize = sizeof(vertices);

        ThrowIfFailed(device->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
            D3D12_HEAP_FLAG_NONE,
            &CD3DX12_RESOURCE_DESC::Buffer(vbSize),
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&vertexBuffer)));

        void* mapped = nullptr;
        CD3DX12_RANGE readRange(0, 0);
        ThrowIfFailed(vertexBuffer->Map(0, &readRange, &mapped));
        memcpy(mapped, vertices, sizeof(vertices));
        vertexBuffer->Unmap(0, nullptr);

        D3D12_VERTEX_BUFFER_VIEW vbView{};
        vbView.BufferLocation = vertexBuffer->GetGPUVirtualAddress();
        vbView.StrideInBytes = sizeof(Vertex);
        vbView.SizeInBytes = vbSize;

        MSG msg{};
        while (msg.message != WM_QUIT) {
            while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }

            ThrowIfFailed(commandAllocator->Reset());
            ThrowIfFailed(commandList->Reset(commandAllocator.Get(), pipelineState.Get()));

            D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
                renderTargets[frameIndex].Get(),
                D3D12_RESOURCE_STATE_PRESENT,
                D3D12_RESOURCE_STATE_RENDER_TARGET);

            commandList->ResourceBarrier(1, &barrier);

            D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
            rtv.ptr += frameIndex * rtvDescriptorSize;

            FLOAT clearColor[] = { 0.0f, 0.0f, 0.0f, 0.0f };
            commandList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
            commandList->ClearRenderTargetView(rtv, clearColor, 0, nullptr);

            commandList->SetGraphicsRootSignature(rootSignature.Get());
            commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            commandList->IASetVertexBuffers(0, 1, &vbView);
            commandList->RSSetViewports(1, &CD3DX12_VIEWPORT(0.0f, 0.0f, 1280.0f, 720.0f));
            commandList->RSSetScissorRects(1, &CD3DX12_RECT(0, 0, 1280, 720));
            commandList->DrawInstanced(6, 1, 0, 0);

            barrier = CD3DX12_RESOURCE_BARRIER::Transition(
                renderTargets[frameIndex].Get(),
                D3D12_RESOURCE_STATE_RENDER_TARGET,
                D3D12_RESOURCE_STATE_PRESENT);
            commandList->ResourceBarrier(1, &barrier);

            ThrowIfFailed(commandList->Close());
            ID3D12CommandList* lists[] = { commandList.Get() };
            commandQueue->ExecuteCommandLists(1, lists);

            ThrowIfFailed(swapChain->Present(1, 0));

            const UINT64 currentFence = fenceValue;
            ThrowIfFailed(commandQueue->Signal(fence.Get(), currentFence));
            fenceValue++;

            if (fence->GetCompletedValue() < currentFence) {
                ThrowIfFailed(fence->SetEventOnCompletion(currentFence, fenceEvent));
                WaitForSingleObject(fenceEvent, INFINITE);
            }

            frameIndex = swapChain->GetCurrentBackBufferIndex();
        }

        CloseHandle(fenceEvent);
        return 0;
    } catch (...) {
        MessageBoxA(nullptr, "Failed to initialize D3D12 overlay box.", "Error", MB_OK);
        return -1;
    }
}
