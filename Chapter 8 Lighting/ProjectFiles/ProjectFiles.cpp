#define TINYOBJLOADER_IMPLEMENTATION
#define NOMINMAX

#include "../../Common/d3dApp.h"
#include "../../Common/MathHelper.h"
#include "../../Common/UploadBuffer.h"
#include "../../Common/GeometryGenerator.h"
#include "../../Common/tiny_obj_loader.h"
#include "../../Common/Camera.h"
#include "FrameResource.h"

using Microsoft::WRL::ComPtr;
using namespace DirectX;
using namespace DirectX::PackedVector;

#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "D3D12.lib")

const int gNumFrameResources = 3;

struct RenderItem
{
    RenderItem() = default;

    XMFLOAT4X4 World = MathHelper::Identity4x4();
    XMFLOAT4X4 TexTransform = MathHelper::Identity4x4();
    int NumFramesDirty = gNumFrameResources;
    UINT ObjCBIndex = -1;
    Material* Mat = nullptr;
    MeshGeometry* Geo = nullptr;
    D3D12_PRIMITIVE_TOPOLOGY PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    UINT IndexCount = 0;
    UINT StartIndexLocation = 0;
    int BaseVertexLocation = 0;
};

class CarApp : public D3DApp
{
public:
    CarApp(HINSTANCE hInstance);
    CarApp(const CarApp& rhs) = delete;
    CarApp& operator=(const CarApp& rhs) = delete;
    ~CarApp();

    virtual bool Initialize()override;

private:
    virtual void OnResize()override;
    virtual void Update(const GameTimer& gt)override;
    virtual void Draw(const GameTimer& gt)override;

    virtual void OnMouseDown(WPARAM btnState, int x, int y)override;
    virtual void OnMouseUp(WPARAM btnState, int x, int y)override;
    virtual void OnMouseMove(WPARAM btnState, int x, int y)override;

    void UpdateCamera(const GameTimer& gt);
    void UpdateObjectCBs(const GameTimer& gt);
    void UpdateMaterialCBs(const GameTimer& gt);
    void UpdateMainPassCB(const GameTimer& gt);

    void BuildRootSignature();
    void BuildShadersAndInputLayout();
    void BuildCarGeometry();
    void BuildRoadGeometry();
    void BuildSkyGeometry();
    void BuildWaterGeometry();
    void BuildPSOs();
    void BuildFrameResources();
    void BuildMaterials();
    void BuildRenderItems();
    void DrawRenderItems(ID3D12GraphicsCommandList* cmdList, const std::vector<RenderItem*>& ritems);

    void OnKeyboardInput(const GameTimer& gt);

private:
    RenderItem* mCarRitem = nullptr;
    RenderItem* mRoadRitem = nullptr;
    RenderItem* mSkyRitem = nullptr;
    RenderItem* mWaterRitem = nullptr;
    RenderItem* mCarOutlineRitem = nullptr;

    std::vector<std::unique_ptr<FrameResource>> mFrameResources;
    FrameResource* mCurrFrameResource = nullptr;
    int mCurrFrameResourceIndex = 0;

    UINT mCbvSrvDescriptorSize = 0;

    ComPtr<ID3D12RootSignature> mRootSignature = nullptr;
    ComPtr<ID3D12DescriptorHeap> mSrvDescriptorHeap = nullptr;

    std::unordered_map<std::string, std::unique_ptr<MeshGeometry>> mGeometries;
    std::unordered_map<std::string, std::unique_ptr<Material>> mMaterials;
    std::unordered_map<std::string, ComPtr<ID3DBlob>> mShaders;

    std::vector<D3D12_INPUT_ELEMENT_DESC> mInputLayout;
    ComPtr<ID3D12PipelineState> mOpaquePSO = nullptr;
    ComPtr<ID3D12PipelineState> mSkyPSO = nullptr;
    ComPtr<ID3D12PipelineState> mTransparentPSO = nullptr;
    ComPtr<ID3D12PipelineState> mOutlinePSO = nullptr;

    std::vector<std::unique_ptr<RenderItem>> mAllRitems;
    std::vector<RenderItem*> mOpaqueRitems;
    std::vector<RenderItem*> mTransparentRitems;
    std::vector<RenderItem*> mOutlineRitems;

    PassConstants mMainPassCB;

    Camera mCamera;

    POINT mLastMousePos;

    std::unique_ptr<Texture> mSkyTex;
    std::unique_ptr<Texture> mWaterTex;
};

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE prevInstance,
    PSTR cmdLine, int showCmd)
{
#if defined(DEBUG) | defined(_DEBUG)
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif

    try
    {
        CarApp theApp(hInstance);
        if (!theApp.Initialize())
            return 0;
        return theApp.Run();
    }
    catch (DxException& e)
    {
        MessageBox(nullptr, e.ToString().c_str(), L"HR Failed", MB_OK);
        return 0;
    }
}

CarApp::CarApp(HINSTANCE hInstance)
    : D3DApp(hInstance)
{
}

CarApp::~CarApp()
{
    if (md3dDevice != nullptr)
        FlushCommandQueue();
}

bool CarApp::Initialize()
{
    if (!D3DApp::Initialize())
        return false;

    ThrowIfFailed(mCommandList->Reset(mDirectCmdListAlloc.Get(), nullptr));

    mCbvSrvDescriptorSize = md3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // Sky Texture Load
    mSkyTex = std::make_unique<Texture>();
    mSkyTex->Name = "skyTex";
    mSkyTex->Filename = L"../../Textures/sunsetcube1024.dds";
    ThrowIfFailed(DirectX::CreateDDSTextureFromFile12(md3dDevice.Get(),
        mCommandList.Get(), mSkyTex->Filename.c_str(),
        mSkyTex->Resource, mSkyTex->UploadHeap));

    // Water Texture Load
    mWaterTex = std::make_unique<Texture>();
    mWaterTex->Name = "waterTex";
    mWaterTex->Filename = L"../../Textures/water1.dds";
    ThrowIfFailed(DirectX::CreateDDSTextureFromFile12(md3dDevice.Get(),
        mCommandList.Get(), mWaterTex->Filename.c_str(),
        mWaterTex->Resource, mWaterTex->UploadHeap));

    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
    srvHeapDesc.NumDescriptors = 2;
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(md3dDevice->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&mSrvDescriptorHeap)));

    CD3DX12_CPU_DESCRIPTOR_HANDLE hDescriptor(mSrvDescriptorHeap->GetCPUDescriptorHandleForHeapStart());

    // Skybox SRV
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = mSkyTex->Resource->GetDesc().Format;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    srvDesc.TextureCube.MostDetailedMip = 0;
    srvDesc.TextureCube.MipLevels = mSkyTex->Resource->GetDesc().MipLevels;
    srvDesc.TextureCube.ResourceMinLODClamp = 0.0f;
    md3dDevice->CreateShaderResourceView(mSkyTex->Resource.Get(), &srvDesc, hDescriptor);

    // Water SRV
    hDescriptor.Offset(1, mCbvSrvDescriptorSize);
    srvDesc.Format = mWaterTex->Resource->GetDesc().Format;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.MipLevels = mWaterTex->Resource->GetDesc().MipLevels;
    srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
    md3dDevice->CreateShaderResourceView(mWaterTex->Resource.Get(), &srvDesc, hDescriptor);

    BuildRootSignature();
    BuildShadersAndInputLayout();
    BuildCarGeometry();
    BuildRoadGeometry();
    BuildWaterGeometry();
    BuildSkyGeometry();
    BuildMaterials();
    BuildRenderItems();
    BuildFrameResources();
    BuildPSOs();

    ThrowIfFailed(mCommandList->Close());
    ID3D12CommandList* cmdsLists[] = { mCommandList.Get() };
    mCommandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);

    FlushCommandQueue();

    mCamera.SetPosition(0.0f, 2.0f, -15.0f);

    return true;
}

void CarApp::OnResize()
{
    D3DApp::OnResize();
    mCamera.SetLens(0.25f * MathHelper::Pi, AspectRatio(), 1.0f, 1000.0f);
}

void CarApp::Update(const GameTimer& gt)
{
    UpdateCamera(gt);

    mCurrFrameResourceIndex = (mCurrFrameResourceIndex + 1) % gNumFrameResources;
    mCurrFrameResource = mFrameResources[mCurrFrameResourceIndex].get();

    if (mCurrFrameResource->Fence != 0 && mFence->GetCompletedValue() < mCurrFrameResource->Fence)
    {
        HANDLE eventHandle = CreateEventEx(nullptr, false, false, EVENT_ALL_ACCESS);
        ThrowIfFailed(mFence->SetEventOnCompletion(mCurrFrameResource->Fence, eventHandle));
        WaitForSingleObject(eventHandle, INFINITE);
        CloseHandle(eventHandle);
    }

    UpdateObjectCBs(gt);
    UpdateMaterialCBs(gt);
    UpdateMainPassCB(gt);
}

void CarApp::OnKeyboardInput(const GameTimer& gt)
{
    const float dt = gt.DeltaTime();
    float speed = 10.0f;

    if (GetAsyncKeyState('W') & 0x8000)
        mCamera.Walk(speed * dt);
    if (GetAsyncKeyState('S') & 0x8000)
        mCamera.Walk(-speed * dt);
    if (GetAsyncKeyState('A') & 0x8000)
        mCamera.Strafe(-speed * dt);
    if (GetAsyncKeyState('D') & 0x8000)
        mCamera.Strafe(speed * dt);

    mCamera.UpdateViewMatrix();
}

void CarApp::Draw(const GameTimer& gt)
{
    auto cmdListAlloc = mCurrFrameResource->CmdListAlloc;

    ThrowIfFailed(cmdListAlloc->Reset());
    ThrowIfFailed(mCommandList->Reset(cmdListAlloc.Get(), mOpaquePSO.Get()));

    mCommandList->RSSetViewports(1, &mScreenViewport);
    mCommandList->RSSetScissorRects(1, &mScissorRect);

    mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
        D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET));

    mCommandList->ClearRenderTargetView(CurrentBackBufferView(), Colors::LightSteelBlue, 0, nullptr);
    mCommandList->ClearDepthStencilView(DepthStencilView(), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);

    mCommandList->OMSetRenderTargets(1, &CurrentBackBufferView(), true, &DepthStencilView());

    mCommandList->SetGraphicsRootSignature(mRootSignature.Get());

    ID3D12DescriptorHeap* descriptorHeaps[] = { mSrvDescriptorHeap.Get() };
    mCommandList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);

    CD3DX12_GPU_DESCRIPTOR_HANDLE skySrvHandle(mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
    mCommandList->SetGraphicsRootDescriptorTable(3, skySrvHandle);

    auto passCB = mCurrFrameResource->PassCB->Resource();
    mCommandList->SetGraphicsRootConstantBufferView(2, passCB->GetGPUVirtualAddress());

    std::vector<RenderItem*> roadItem;
    roadItem.push_back(mRoadRitem);
    mCommandList->OMSetStencilRef(0);
    DrawRenderItems(mCommandList.Get(), roadItem);

    std::vector<RenderItem*> carItem;
    carItem.push_back(mCarRitem);
    mCommandList->OMSetStencilRef(1);
    DrawRenderItems(mCommandList.Get(), carItem);

    if (mOutlinePSO != nullptr)
    {
        mCommandList->SetPipelineState(mOutlinePSO.Get());
        mCommandList->OMSetStencilRef(1);
        DrawRenderItems(mCommandList.Get(), mOutlineRitems);
    }

    if (mSkyPSO != nullptr)
    {
        mCommandList->SetPipelineState(mSkyPSO.Get());
        std::vector<RenderItem*> skyRitem;
        skyRitem.push_back(mSkyRitem);
        DrawRenderItems(mCommandList.Get(), skyRitem);
    }

    if (mTransparentPSO != nullptr)
    {
        mCommandList->SetPipelineState(mTransparentPSO.Get());

        CD3DX12_GPU_DESCRIPTOR_HANDLE waterSrvHandle(mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
        waterSrvHandle.Offset(1, mCbvSrvDescriptorSize);
        mCommandList->SetGraphicsRootDescriptorTable(3, waterSrvHandle);

        DrawRenderItems(mCommandList.Get(), mTransparentRitems);
    }

    mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
        D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT));

    ThrowIfFailed(mCommandList->Close());

    ID3D12CommandList* cmdsLists[] = { mCommandList.Get() };
    mCommandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);

    ThrowIfFailed(mSwapChain->Present(0, 0));
    mCurrBackBuffer = (mCurrBackBuffer + 1) % SwapChainBufferCount;

    mCurrFrameResource->Fence = ++mCurrentFence;
    mCommandQueue->Signal(mFence.Get(), mCurrentFence);
}

void CarApp::OnMouseDown(WPARAM btnState, int x, int y)
{
    mLastMousePos.x = x;
    mLastMousePos.y = y;
    SetCapture(mhMainWnd);
}

void CarApp::OnMouseUp(WPARAM btnState, int x, int y)
{
    ReleaseCapture();
}

void CarApp::OnMouseMove(WPARAM btnState, int x, int y)
{
    if ((btnState & MK_LBUTTON) != 0)
    {
        float dx = XMConvertToRadians(0.25f * static_cast<float>(x - mLastMousePos.x));
        float dy = XMConvertToRadians(0.25f * static_cast<float>(y - mLastMousePos.y));

        mCamera.Pitch(dy);
        mCamera.RotateY(dx);
    }

    mLastMousePos.x = x;
    mLastMousePos.y = y;
}

void CarApp::UpdateCamera(const GameTimer& gt)
{
    OnKeyboardInput(gt);
}

void CarApp::UpdateObjectCBs(const GameTimer& gt)
{
    if (mWaterRitem != nullptr)
    {
        float waterY = -0.5f + 0.15f * sinf(gt.TotalTime() * 1.5f);
        XMMATRIX waterWorld = XMMatrixTranslation(0.0f, waterY, 0.0f);
        XMStoreFloat4x4(&mWaterRitem->World, waterWorld);
        mWaterRitem->NumFramesDirty = gNumFrameResources;
    }

    auto currObjectCB = mCurrFrameResource->ObjectCB.get();
    for (auto& e : mAllRitems)
    {
        if (e->NumFramesDirty > 0)
        {
            XMMATRIX world = XMLoadFloat4x4(&e->World);
            XMMATRIX texTransform = XMLoadFloat4x4(&e->TexTransform);

            ObjectConstants objConstants;
            XMStoreFloat4x4(&objConstants.World, XMMatrixTranspose(world));
            XMStoreFloat4x4(&objConstants.TexTransform, XMMatrixTranspose(texTransform));

            currObjectCB->CopyData(e->ObjCBIndex, objConstants);
            e->NumFramesDirty--;
        }
    }

    if (mCarRitem == nullptr) return;

    static const XMFLOAT3 waypoints[] =
    {
        { 8.00f, 2.0f, -30.0f },
        { 6.0f, 2.0f, -20.0f },
        { 3.0f, 1.4f, -10.0f },
        { 0.0f, 1.4f,  0.0f },
        { -2.8f, 1.4f,  10.0f },
        { -4.0f, 1.4f,  20.0f },
        { -6.8f, 1.4f,  30.0f },
    };
    static const int numWaypoints = _countof(waypoints);

    float loopTime = 6.0f;
    float u = fmodf(gt.TotalTime(), loopTime) / loopTime;
    float scaledU = u * (numWaypoints - 1);
    int i0 = (int)scaledU;
    int i1 = MathHelper::Min(i0 + 1, numWaypoints - 1);
    float t = scaledU - (float)i0;

    XMVECTOR p0 = XMLoadFloat3(&waypoints[i0]);
    XMVECTOR p1 = XMLoadFloat3(&waypoints[i1]);
    XMVECTOR pos = XMVectorLerp(p0, p1, t);

    XMVECTOR dir = XMVector3Normalize(XMVectorSubtract(p1, p0));
    float yaw = atan2f(XMVectorGetX(dir), XMVectorGetZ(dir));

    XMMATRIX world =
        XMMatrixScaling(0.2f, 0.2f, 0.2f) *
        XMMatrixRotationY(yaw) *
        XMMatrixTranslation(XMVectorGetX(pos), XMVectorGetY(pos), XMVectorGetZ(pos));

    XMStoreFloat4x4(&mCarRitem->World, world);
    mCarRitem->NumFramesDirty = gNumFrameResources;

    if (mCarOutlineRitem != nullptr)
    {
        XMMATRIX outlineWorld =
            XMMatrixScaling(0.208f, 0.208f, 0.208f) *
            XMMatrixRotationY(yaw) *
            XMMatrixTranslation(XMVectorGetX(pos), XMVectorGetY(pos), XMVectorGetZ(pos));

        XMStoreFloat4x4(&mCarOutlineRitem->World, outlineWorld);
        mCarOutlineRitem->NumFramesDirty = gNumFrameResources;
    }

    if (mSkyRitem != nullptr)
    {
        XMFLOAT3 eyePos = mCamera.GetPosition3f();
        XMMATRIX skyWorld = XMMatrixScaling(5000.0f, 5000.0f, 5000.0f) * XMMatrixTranslation(eyePos.x, eyePos.y, eyePos.z);
        XMStoreFloat4x4(&mSkyRitem->World, skyWorld);
        mSkyRitem->NumFramesDirty = gNumFrameResources;
    }
}

void CarApp::UpdateMaterialCBs(const GameTimer& gt)
{
    auto currMaterialCB = mCurrFrameResource->MaterialCB.get();
    for (auto& e : mMaterials)
    {
        Material* mat = e.second.get();
        if (mat->NumFramesDirty > 0)
        {
            XMMATRIX matTransform = XMLoadFloat4x4(&mat->MatTransform);

            MaterialConstants matConstants;
            matConstants.DiffuseAlbedo = mat->DiffuseAlbedo;
            matConstants.FresnelR0 = mat->FresnelR0;
            matConstants.Roughness = mat->Roughness;
            XMStoreFloat4x4(&matConstants.MatTransform, XMMatrixTranspose(matTransform));

            currMaterialCB->CopyData(mat->MatCBIndex, matConstants);
            mat->NumFramesDirty--;
        }
    }
}

void CarApp::UpdateMainPassCB(const GameTimer& gt)
{
    XMMATRIX view = mCamera.GetView();
    XMMATRIX proj = mCamera.GetProj();

    XMMATRIX viewProj = XMMatrixMultiply(view, proj);
    XMMATRIX invView = XMMatrixInverse(&XMMatrixDeterminant(view), view);
    XMMATRIX invProj = XMMatrixInverse(&XMMatrixDeterminant(proj), proj);
    XMMATRIX invViewProj = XMMatrixInverse(&XMMatrixDeterminant(viewProj), viewProj);

    XMStoreFloat4x4(&mMainPassCB.View, XMMatrixTranspose(view));
    XMStoreFloat4x4(&mMainPassCB.InvView, XMMatrixTranspose(invView));
    XMStoreFloat4x4(&mMainPassCB.Proj, XMMatrixTranspose(proj));
    XMStoreFloat4x4(&mMainPassCB.InvProj, XMMatrixTranspose(invProj));
    XMStoreFloat4x4(&mMainPassCB.ViewProj, XMMatrixTranspose(viewProj));
    XMStoreFloat4x4(&mMainPassCB.InvViewProj, XMMatrixTranspose(invViewProj));

    mMainPassCB.EyePosW = mCamera.GetPosition3f();

    mMainPassCB.RenderTargetSize = XMFLOAT2((float)mClientWidth, (float)mClientHeight);
    mMainPassCB.InvRenderTargetSize = XMFLOAT2(1.0f / mClientWidth, 1.0f / mClientHeight);
    mMainPassCB.NearZ = 1.0f;
    mMainPassCB.FarZ = 1000.0f;
    mMainPassCB.TotalTime = gt.TotalTime();
    mMainPassCB.DeltaTime = gt.DeltaTime();
    mMainPassCB.AmbientLight = XMFLOAT4(0.02f, 0.03f, 0.06f, 1.0f);
    mMainPassCB.Lights[0].Direction = { 0.57735f, -0.57735f, 0.57735f };
    mMainPassCB.Lights[0].Strength = { 0.05f, 0.05f, 0.1f };
    mMainPassCB.Lights[1].Direction = { -0.57735f, -0.57735f, 0.57735f };
    mMainPassCB.Lights[1].Strength = { 0.03f, 0.03f, 0.06f };
    mMainPassCB.Lights[2].Direction = { 0.0f, -0.707f, -0.707f };
    mMainPassCB.Lights[2].Strength = { 0.6f, 0.6f, 0.6f };

    XMFLOAT3 carPos = {
        mCarRitem->World._41,
        mCarRitem->World._42,
        mCarRitem->World._43
    };

    XMFLOAT3 forward = {
        mCarRitem->World._31,
        mCarRitem->World._32,
        mCarRitem->World._33
    };

    XMVECTOR f = XMVector3Normalize(XMLoadFloat3(&forward));
    XMStoreFloat3(&forward, f);

    XMFLOAT3 leftOffset = { -0.3f, 0.2f, 0.05f };
    XMFLOAT3 rightOffset = { 0.3f, 0.2f, 0.05f };

    mMainPassCB.Lights[3].Position = {
        carPos.x + leftOffset.x,
        carPos.y + leftOffset.y,
        carPos.z + leftOffset.z
    };

    mMainPassCB.Lights[3].Direction = forward;
    mMainPassCB.Lights[3].Strength = { 25.0f, 25.0f, 25.0f };
    mMainPassCB.Lights[3].SpotPower = 64.0f;

    mMainPassCB.Lights[4].Position = {
        carPos.x + rightOffset.x,
        carPos.y + rightOffset.y,
        carPos.z + rightOffset.z
    };

    mMainPassCB.Lights[4].Direction = forward;
    mMainPassCB.Lights[4].Strength = { 25.0f, 25.0f, 25.0f };
    mMainPassCB.Lights[4].SpotPower = 64.0f;

    auto currPassCB = mCurrFrameResource->PassCB.get();
    currPassCB->CopyData(0, mMainPassCB);
}

void CarApp::BuildRootSignature()
{
    CD3DX12_DESCRIPTOR_RANGE texTable;
    texTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);

    CD3DX12_ROOT_PARAMETER slotRootParameter[4];
    slotRootParameter[0].InitAsConstantBufferView(0);
    slotRootParameter[1].InitAsConstantBufferView(1);
    slotRootParameter[2].InitAsConstantBufferView(2);
    slotRootParameter[3].InitAsDescriptorTable(1, &texTable, D3D12_SHADER_VISIBILITY_PIXEL);

    const CD3DX12_STATIC_SAMPLER_DESC linearWrap(
        0,
        D3D12_FILTER_MIN_MAG_MIP_LINEAR,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP);

    CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(4, slotRootParameter, 1, &linearWrap,
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

    ComPtr<ID3DBlob> serializedRootSig = nullptr;
    ComPtr<ID3DBlob> errorBlob = nullptr;
    HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
        serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

    if (errorBlob != nullptr)
        ::OutputDebugStringA((char*)errorBlob->GetBufferPointer());

    ThrowIfFailed(hr);

    ThrowIfFailed(md3dDevice->CreateRootSignature(
        0,
        serializedRootSig->GetBufferPointer(),
        serializedRootSig->GetBufferSize(),
        IID_PPV_ARGS(mRootSignature.GetAddressOf())));
}

void CarApp::BuildShadersAndInputLayout()
{
    mShaders["standardVS"] = d3dUtil::CompileShader(L"Shaders\\Default.hlsl", nullptr, "VS", "vs_5_1");
    mShaders["opaquePS"] = d3dUtil::CompileShader(L"Shaders\\Default.hlsl", nullptr, "PS", "ps_5_1");
    mShaders["standardGS"] = d3dUtil::CompileShader(
        L"Shaders\\Default.hlsl",
        nullptr,
        "MyGS",
        "gs_5_1"
    );

    mShaders["skyVS"] = d3dUtil::CompileShader(L"Shaders\\Sky.hlsl", nullptr, "VS", "vs_5_1");
    mShaders["skyPS"] = d3dUtil::CompileShader(L"Shaders\\Sky.hlsl", nullptr, "PS", "ps_5_1");

    mInputLayout =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };
}

void CarApp::BuildCarGeometry()
{
    tinyobj::attrib_t attrib;
    std::vector<tinyobj::shape_t> shapes;
    std::vector<tinyobj::material_t> materials;
    std::string warn, err;

    bool ret = tinyobj::LoadObj(&attrib, &shapes, &materials, &warn, &err, "Models/car.obj");

    if (!ret || !err.empty())
    {
        MessageBoxA(0, err.c_str(), "OBJ Load Error", MB_OK);
        return;
    }

    std::vector<Vertex> vertices;
    std::vector<std::int32_t> indices;

    for (auto& shape : shapes)
    {
        for (auto& index : shape.mesh.indices)
        {
            Vertex v = {};
            v.Pos = {
                attrib.vertices[3 * index.vertex_index + 0],
                attrib.vertices[3 * index.vertex_index + 1],
                attrib.vertices[3 * index.vertex_index + 2]
            };
            if (index.normal_index >= 0)
            {
                v.Normal = {
                    attrib.normals[3 * index.normal_index + 0],
                    attrib.normals[3 * index.normal_index + 1],
                    attrib.normals[3 * index.normal_index + 2]
                };
            }
            indices.push_back((std::int32_t)vertices.size());
            vertices.push_back(v);
        }
    }

    const UINT vbByteSize = (UINT)vertices.size() * sizeof(Vertex);
    const UINT ibByteSize = (UINT)indices.size() * sizeof(std::int32_t);

    auto geo = std::make_unique<MeshGeometry>();
    geo->Name = "carGeo";

    ThrowIfFailed(D3DCreateBlob(vbByteSize, &geo->VertexBufferCPU));
    CopyMemory(geo->VertexBufferCPU->GetBufferPointer(), vertices.data(), vbByteSize);

    ThrowIfFailed(D3DCreateBlob(ibByteSize, &geo->IndexBufferCPU));
    CopyMemory(geo->IndexBufferCPU->GetBufferPointer(), indices.data(), ibByteSize);

    geo->VertexBufferGPU = d3dUtil::CreateDefaultBuffer(md3dDevice.Get(),
        mCommandList.Get(), vertices.data(), vbByteSize, geo->VertexBufferUploader);

    geo->IndexBufferGPU = d3dUtil::CreateDefaultBuffer(md3dDevice.Get(),
        mCommandList.Get(), indices.data(), ibByteSize, geo->IndexBufferUploader);

    geo->VertexByteStride = sizeof(Vertex);
    geo->VertexBufferByteSize = vbByteSize;
    geo->IndexFormat = DXGI_FORMAT_R32_UINT;
    geo->IndexBufferByteSize = ibByteSize;

    SubmeshGeometry submesh;
    submesh.IndexCount = (UINT)indices.size();
    submesh.StartIndexLocation = 0;
    submesh.BaseVertexLocation = 0;

    geo->DrawArgs["car"] = submesh;
    mGeometries[geo->Name] = std::move(geo);
}

void CarApp::BuildRoadGeometry()
{
    tinyobj::attrib_t attrib;
    std::vector<tinyobj::shape_t> shapes;
    std::vector<tinyobj::material_t> materials;
    std::string warn, err;

    bool ret = tinyobj::LoadObj(&attrib, &shapes, &materials, &warn, &err, "Models/road.obj");

    if (!ret || !err.empty())
    {
        MessageBoxA(0, err.c_str(), "OBJ Load Error", MB_OK);
        return;
    }

    std::vector<Vertex> vertices;
    std::vector<std::int32_t> indices;

    for (auto& shape : shapes)
    {
        for (auto& index : shape.mesh.indices)
        {
            Vertex v = {};
            v.Pos = {
                attrib.vertices[3 * index.vertex_index + 0],
                attrib.vertices[3 * index.vertex_index + 1],
                attrib.vertices[3 * index.vertex_index + 2]
            };
            if (index.normal_index >= 0)
            {
                v.Normal = {
                    attrib.normals[3 * index.normal_index + 0],
                    attrib.normals[3 * index.normal_index + 1],
                    attrib.normals[3 * index.normal_index + 2]
                };
            }
            indices.push_back((std::int32_t)vertices.size());
            vertices.push_back(v);
        }
    }

    const UINT vbByteSize = (UINT)vertices.size() * sizeof(Vertex);
    const UINT ibByteSize = (UINT)indices.size() * sizeof(std::int32_t);

    auto geo = std::make_unique<MeshGeometry>();
    geo->Name = "roadGeo";

    ThrowIfFailed(D3DCreateBlob(vbByteSize, &geo->VertexBufferCPU));
    CopyMemory(geo->VertexBufferCPU->GetBufferPointer(), vertices.data(), vbByteSize);

    ThrowIfFailed(D3DCreateBlob(ibByteSize, &geo->IndexBufferCPU));
    CopyMemory(geo->IndexBufferCPU->GetBufferPointer(), indices.data(), ibByteSize);

    geo->VertexBufferGPU = d3dUtil::CreateDefaultBuffer(md3dDevice.Get(),
        mCommandList.Get(), vertices.data(), vbByteSize, geo->VertexBufferUploader);

    geo->IndexBufferGPU = d3dUtil::CreateDefaultBuffer(md3dDevice.Get(),
        mCommandList.Get(), indices.data(), ibByteSize, geo->IndexBufferUploader);

    geo->VertexByteStride = sizeof(Vertex);
    geo->VertexBufferByteSize = vbByteSize;
    geo->IndexFormat = DXGI_FORMAT_R32_UINT;
    geo->IndexBufferByteSize = ibByteSize;

    SubmeshGeometry submesh;
    submesh.IndexCount = (UINT)indices.size();
    submesh.StartIndexLocation = 0;
    submesh.BaseVertexLocation = 0;

    geo->DrawArgs["road"] = submesh;
    mGeometries[geo->Name] = std::move(geo);
}

void CarApp::BuildWaterGeometry()
{
    GeometryGenerator geoGen;
    GeometryGenerator::MeshData grid = geoGen.CreateGrid(160.0f, 160.0f, 50, 50);

    std::vector<Vertex> vertices(grid.Vertices.size());
    for (size_t i = 0; i < grid.Vertices.size(); ++i)
    {
        vertices[i].Pos = grid.Vertices[i].Position;
        vertices[i].Normal = grid.Vertices[i].Normal;
    }

    const UINT vbByteSize = (UINT)vertices.size() * sizeof(Vertex);

    std::vector<std::uint16_t> indices = grid.GetIndices16();
    const UINT ibByteSize = (UINT)indices.size() * sizeof(std::uint16_t);

    auto geo = std::make_unique<MeshGeometry>();
    geo->Name = "waterGeo";

    ThrowIfFailed(D3DCreateBlob(vbByteSize, &geo->VertexBufferCPU));
    CopyMemory(geo->VertexBufferCPU->GetBufferPointer(), vertices.data(), vbByteSize);

    ThrowIfFailed(D3DCreateBlob(ibByteSize, &geo->IndexBufferCPU));
    CopyMemory(geo->IndexBufferCPU->GetBufferPointer(), indices.data(), ibByteSize);

    geo->VertexBufferGPU = d3dUtil::CreateDefaultBuffer(md3dDevice.Get(),
        mCommandList.Get(), vertices.data(), vbByteSize, geo->VertexBufferUploader);

    geo->IndexBufferGPU = d3dUtil::CreateDefaultBuffer(md3dDevice.Get(),
        mCommandList.Get(), indices.data(), ibByteSize, geo->IndexBufferUploader);

    geo->VertexByteStride = sizeof(Vertex);
    geo->VertexBufferByteSize = vbByteSize;
    geo->IndexFormat = DXGI_FORMAT_R16_UINT;
    geo->IndexBufferByteSize = ibByteSize;

    SubmeshGeometry submesh;
    submesh.IndexCount = (UINT)indices.size();
    submesh.StartIndexLocation = 0;
    submesh.BaseVertexLocation = 0;

    geo->DrawArgs["water"] = submesh;
    mGeometries[geo->Name] = std::move(geo);
}

void CarApp::BuildSkyGeometry()
{
    GeometryGenerator geoGen;
    GeometryGenerator::MeshData sphere = geoGen.CreateSphere(0.5f, 20, 20);

    std::vector<Vertex> vertices(sphere.Vertices.size());
    for (size_t i = 0; i < sphere.Vertices.size(); ++i)
    {
        vertices[i].Pos = sphere.Vertices[i].Position;
        vertices[i].Normal = sphere.Vertices[i].Normal;
    }

    const UINT vbByteSize = (UINT)vertices.size() * sizeof(Vertex);

    std::vector<std::uint16_t> indices = sphere.GetIndices16();
    const UINT ibByteSize = (UINT)indices.size() * sizeof(std::uint16_t);

    auto geo = std::make_unique<MeshGeometry>();
    geo->Name = "skyGeo";

    ThrowIfFailed(D3DCreateBlob(vbByteSize, &geo->VertexBufferCPU));
    CopyMemory(geo->VertexBufferCPU->GetBufferPointer(), vertices.data(), vbByteSize);

    ThrowIfFailed(D3DCreateBlob(ibByteSize, &geo->IndexBufferCPU));
    CopyMemory(geo->IndexBufferCPU->GetBufferPointer(), indices.data(), ibByteSize);

    geo->VertexBufferGPU = d3dUtil::CreateDefaultBuffer(md3dDevice.Get(),
        mCommandList.Get(), vertices.data(), vbByteSize, geo->VertexBufferUploader);

    geo->IndexBufferGPU = d3dUtil::CreateDefaultBuffer(md3dDevice.Get(),
        mCommandList.Get(), indices.data(), ibByteSize, geo->IndexBufferUploader);

    geo->VertexByteStride = sizeof(Vertex);
    geo->VertexBufferByteSize = vbByteSize;
    geo->IndexFormat = DXGI_FORMAT_R16_UINT;
    geo->IndexBufferByteSize = ibByteSize;

    SubmeshGeometry submesh;
    submesh.IndexCount = (UINT)indices.size();
    submesh.StartIndexLocation = 0;
    submesh.BaseVertexLocation = 0;

    geo->DrawArgs["sky"] = submesh;
    mGeometries[geo->Name] = std::move(geo);
}

void CarApp::BuildPSOs()
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC opaquePsoDesc;
    ZeroMemory(&opaquePsoDesc, sizeof(D3D12_GRAPHICS_PIPELINE_STATE_DESC));

    opaquePsoDesc.InputLayout = { mInputLayout.data(), (UINT)mInputLayout.size() };
    opaquePsoDesc.pRootSignature = mRootSignature.Get();
    opaquePsoDesc.VS =
    {
        reinterpret_cast<BYTE*>(mShaders["standardVS"]->GetBufferPointer()),
        mShaders["standardVS"]->GetBufferSize()
    };
    opaquePsoDesc.PS =
    {
        reinterpret_cast<BYTE*>(mShaders["opaquePS"]->GetBufferPointer()),
        mShaders["opaquePS"]->GetBufferSize()
    };
    opaquePsoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    opaquePsoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    opaquePsoDesc.SampleMask = UINT_MAX;
    opaquePsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    opaquePsoDesc.NumRenderTargets = 1;
    opaquePsoDesc.RTVFormats[0] = mBackBufferFormat;
    opaquePsoDesc.SampleDesc.Count = m4xMsaaState ? 4 : 1;
    opaquePsoDesc.SampleDesc.Quality = m4xMsaaState ? (m4xMsaaQuality - 1) : 0;
    opaquePsoDesc.DSVFormat = mDepthStencilFormat;

    opaquePsoDesc.GS =
    {
        reinterpret_cast<BYTE*>(mShaders["standardGS"]->GetBufferPointer()),
        mShaders["standardGS"]->GetBufferSize()
    };

    D3D12_DEPTH_STENCIL_DESC opaqueDSS;
    opaqueDSS.DepthEnable = true;
    opaqueDSS.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    opaqueDSS.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    opaqueDSS.StencilEnable = true;
    opaqueDSS.StencilReadMask = 0xff;
    opaqueDSS.StencilWriteMask = 0xff;
    opaqueDSS.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
    opaqueDSS.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
    opaqueDSS.FrontFace.StencilPassOp = D3D12_STENCIL_OP_REPLACE;
    opaqueDSS.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    opaqueDSS.BackFace = opaqueDSS.FrontFace;
    opaquePsoDesc.DepthStencilState = opaqueDSS;

    ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&opaquePsoDesc, IID_PPV_ARGS(&mOpaquePSO)));

    D3D12_GRAPHICS_PIPELINE_STATE_DESC outlinePsoDesc = opaquePsoDesc;
    D3D12_DEPTH_STENCIL_DESC outlineDSS = opaqueDSS;
    outlineDSS.StencilEnable = true;
    outlineDSS.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
    outlineDSS.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_NOT_EQUAL;
    outlineDSS.BackFace = outlineDSS.FrontFace;
    outlineDSS.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    outlinePsoDesc.DepthStencilState = outlineDSS;

    ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&outlinePsoDesc, IID_PPV_ARGS(&mOutlinePSO)));

    D3D12_RENDER_TARGET_BLEND_DESC transparencyBlendDesc;
    transparencyBlendDesc.BlendEnable = true;
    transparencyBlendDesc.LogicOpEnable = false;
    transparencyBlendDesc.SrcBlend = D3D12_BLEND_SRC_ALPHA;
    transparencyBlendDesc.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    transparencyBlendDesc.BlendOp = D3D12_BLEND_OP_ADD;
    transparencyBlendDesc.SrcBlendAlpha = D3D12_BLEND_ONE;
    transparencyBlendDesc.DestBlendAlpha = D3D12_BLEND_ZERO;
    transparencyBlendDesc.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    transparencyBlendDesc.LogicOp = D3D12_LOGIC_OP_NOOP;
    transparencyBlendDesc.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC transparentPsoDesc = opaquePsoDesc;
    transparentPsoDesc.BlendState.RenderTarget[0] = transparencyBlendDesc;
    transparentPsoDesc.DepthStencilState.StencilEnable = false;
    ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&transparentPsoDesc, IID_PPV_ARGS(&mTransparentPSO)));

    D3D12_GRAPHICS_PIPELINE_STATE_DESC skyPsoDesc = opaquePsoDesc;
    skyPsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    skyPsoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    skyPsoDesc.DepthStencilState.StencilEnable = false;
    skyPsoDesc.pRootSignature = mRootSignature.Get();

    if (mShaders.find("skyVS") != mShaders.end() && mShaders.find("skyPS") != mShaders.end())
    {
        skyPsoDesc.VS =
        {
            reinterpret_cast<BYTE*>(mShaders["skyVS"]->GetBufferPointer()),
            mShaders["skyVS"]->GetBufferSize()
        };
        skyPsoDesc.PS =
        {
            reinterpret_cast<BYTE*>(mShaders["skyPS"]->GetBufferPointer()),
            mShaders["skyPS"]->GetBufferSize()
        };
        skyPsoDesc.GS = {};
        ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&skyPsoDesc, IID_PPV_ARGS(&mSkyPSO)));
    }
}

void CarApp::BuildFrameResources()
{
    for (int i = 0; i < gNumFrameResources; ++i)
    {
        mFrameResources.push_back(std::make_unique<FrameResource>(md3dDevice.Get(),
            1, (UINT)mAllRitems.size(), (UINT)mMaterials.size()));
    }
}

void CarApp::BuildMaterials()
{
    auto carMat = std::make_unique<Material>();
    carMat->Name = "carMat";
    carMat->MatCBIndex = 0;
    carMat->DiffuseSrvHeapIndex = 0;
    carMat->DiffuseAlbedo = XMFLOAT4(0.8f, 0.1f, 0.1f, 1.0f);
    carMat->FresnelR0 = XMFLOAT3(0.1f, 0.1f, 0.1f);
    carMat->Roughness = 0.2f;

    auto roadMat = std::make_unique<Material>();
    roadMat->Name = "roadMat";
    roadMat->MatCBIndex = 1;
    roadMat->DiffuseSrvHeapIndex = 1;
    roadMat->DiffuseAlbedo = XMFLOAT4(0.15f, 0.15f, 0.15f, 1.0f);
    roadMat->FresnelR0 = XMFLOAT3(0.02f, 0.02f, 0.02f);
    roadMat->Roughness = 0.9f;

    auto skyMat = std::make_unique<Material>();
    skyMat->Name = "skyMat";
    skyMat->MatCBIndex = 2;
    skyMat->DiffuseSrvHeapIndex = 2;
    skyMat->DiffuseAlbedo = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
    skyMat->FresnelR0 = XMFLOAT3(0.1f, 0.1f, 0.1f);
    skyMat->Roughness = 1.0f;

    auto waterMat = std::make_unique<Material>();
    waterMat->Name = "waterMat";
    waterMat->MatCBIndex = 3;
    waterMat->DiffuseSrvHeapIndex = 3;
    waterMat->DiffuseAlbedo = XMFLOAT4(1.0f, 1.0f, 1.0f, 0.5f);
    waterMat->FresnelR0 = XMFLOAT3(0.1f, 0.1f, 0.1f);
    waterMat->Roughness = 0.0f;

    auto outlineMat = std::make_unique<Material>();
    outlineMat->Name = "outlineMat";
    outlineMat->MatCBIndex = 4;
    outlineMat->DiffuseSrvHeapIndex = 0;
    outlineMat->DiffuseAlbedo = XMFLOAT4(1.0f, 1.0f, 0.0f, 1.0f);
    outlineMat->FresnelR0 = XMFLOAT3(0.1f, 0.1f, 0.1f);
    outlineMat->Roughness = 1.0f;

    mMaterials["carMat"] = std::move(carMat);
    mMaterials["roadMat"] = std::move(roadMat);
    mMaterials["skyMat"] = std::move(skyMat);
    mMaterials["waterMat"] = std::move(waterMat);
    mMaterials["outlineMat"] = std::move(outlineMat);
}

void CarApp::BuildRenderItems()
{
    auto roadRitem = std::make_unique<RenderItem>();
    XMStoreFloat4x4(&roadRitem->World, XMMatrixScaling(0.2f, 0.2f, 0.2f));
    roadRitem->TexTransform = MathHelper::Identity4x4();
    roadRitem->ObjCBIndex = 0;
    roadRitem->Mat = mMaterials["roadMat"].get();
    roadRitem->Geo = mGeometries["roadGeo"].get();
    roadRitem->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    roadRitem->IndexCount = roadRitem->Geo->DrawArgs["road"].IndexCount;
    roadRitem->StartIndexLocation = roadRitem->Geo->DrawArgs["road"].StartIndexLocation;
    roadRitem->BaseVertexLocation = roadRitem->Geo->DrawArgs["road"].BaseVertexLocation;
    mRoadRitem = roadRitem.get();
    mAllRitems.push_back(std::move(roadRitem));

    auto carRitem = std::make_unique<RenderItem>();
    XMStoreFloat4x4(&carRitem->World, XMMatrixScaling(0.2f, 0.2f, 0.2f) * XMMatrixTranslation(0.0f, 0.6f, 0.0f));
    carRitem->TexTransform = MathHelper::Identity4x4();
    carRitem->ObjCBIndex = 1;
    carRitem->Mat = mMaterials["carMat"].get();
    carRitem->Geo = mGeometries["carGeo"].get();
    carRitem->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    carRitem->IndexCount = carRitem->Geo->DrawArgs["car"].IndexCount;
    carRitem->StartIndexLocation = carRitem->Geo->DrawArgs["car"].StartIndexLocation;
    carRitem->BaseVertexLocation = carRitem->Geo->DrawArgs["car"].BaseVertexLocation;
    mCarRitem = carRitem.get();
    mAllRitems.push_back(std::move(carRitem));

    auto skyRitem = std::make_unique<RenderItem>();
    XMStoreFloat4x4(&skyRitem->World, XMMatrixScaling(5000.0f, 5000.0f, 5000.0f));
    skyRitem->TexTransform = MathHelper::Identity4x4();
    skyRitem->ObjCBIndex = 2;
    skyRitem->Mat = mMaterials["skyMat"].get();
    skyRitem->Geo = mGeometries["skyGeo"].get();
    skyRitem->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    skyRitem->IndexCount = skyRitem->Geo->DrawArgs["sky"].IndexCount;
    skyRitem->StartIndexLocation = skyRitem->Geo->DrawArgs["sky"].StartIndexLocation;
    skyRitem->BaseVertexLocation = skyRitem->Geo->DrawArgs["sky"].BaseVertexLocation;
    mSkyRitem = skyRitem.get();
    mAllRitems.push_back(std::move(skyRitem));

    auto waterRitem = std::make_unique<RenderItem>();
    XMStoreFloat4x4(&waterRitem->World, XMMatrixTranslation(0.0f, -0.5f, 0.0f));
    waterRitem->TexTransform = MathHelper::Identity4x4();
    waterRitem->ObjCBIndex = 3;
    waterRitem->Mat = mMaterials["waterMat"].get();
    waterRitem->Geo = mGeometries["waterGeo"].get();
    waterRitem->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    waterRitem->IndexCount = waterRitem->Geo->DrawArgs["water"].IndexCount;
    waterRitem->StartIndexLocation = waterRitem->Geo->DrawArgs["water"].StartIndexLocation;
    waterRitem->BaseVertexLocation = waterRitem->Geo->DrawArgs["water"].BaseVertexLocation;
    mWaterRitem = waterRitem.get();
    mTransparentRitems.push_back(waterRitem.get());
    mAllRitems.push_back(std::move(waterRitem));

    auto carOutlineRitem = std::make_unique<RenderItem>();
    carOutlineRitem->World = mCarRitem->World;
    carOutlineRitem->TexTransform = MathHelper::Identity4x4();
    carOutlineRitem->ObjCBIndex = 4;
    carOutlineRitem->Mat = mMaterials["outlineMat"].get();
    carOutlineRitem->Geo = mGeometries["carGeo"].get();
    carOutlineRitem->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    carOutlineRitem->IndexCount = carOutlineRitem->Geo->DrawArgs["car"].IndexCount;
    carOutlineRitem->StartIndexLocation = carOutlineRitem->Geo->DrawArgs["car"].StartIndexLocation;
    carOutlineRitem->BaseVertexLocation = carOutlineRitem->Geo->DrawArgs["car"].BaseVertexLocation;
    mCarOutlineRitem = carOutlineRitem.get();
    mOutlineRitems.push_back(carOutlineRitem.get());
    mAllRitems.push_back(std::move(carOutlineRitem));

    for (auto& e : mAllRitems)
    {
        if (e.get() != mSkyRitem && e.get() != mWaterRitem && e.get() != mCarOutlineRitem)
            mOpaqueRitems.push_back(e.get());
    }
}

void CarApp::DrawRenderItems(ID3D12GraphicsCommandList* cmdList, const std::vector<RenderItem*>& ritems)
{
    UINT objCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(ObjectConstants));
    UINT matCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(MaterialConstants));

    auto objectCB = mCurrFrameResource->ObjectCB->Resource();
    auto matCB = mCurrFrameResource->MaterialCB->Resource();

    for (size_t i = 0; i < ritems.size(); ++i)
    {
        auto ri = ritems[i];

        cmdList->IASetVertexBuffers(0, 1, &ri->Geo->VertexBufferView());
        cmdList->IASetIndexBuffer(&ri->Geo->IndexBufferView());
        cmdList->IASetPrimitiveTopology(ri->PrimitiveType);

        D3D12_GPU_VIRTUAL_ADDRESS objCBAddress = objectCB->GetGPUVirtualAddress() + ri->ObjCBIndex * objCBByteSize;
        D3D12_GPU_VIRTUAL_ADDRESS matCBAddress = matCB->GetGPUVirtualAddress() + ri->Mat->MatCBIndex * matCBByteSize;

        cmdList->SetGraphicsRootConstantBufferView(0, objCBAddress);
        cmdList->SetGraphicsRootConstantBufferView(1, matCBAddress);

        cmdList->DrawIndexedInstanced(ri->IndexCount, 1, ri->StartIndexLocation, ri->BaseVertexLocation, 0);
    }
}
