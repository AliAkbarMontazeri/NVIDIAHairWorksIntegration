﻿#include "pch.h"
#include "hwInternal.h"
#include "hwContext.h"

#if defined(_M_IX86)
    #define hwSDKDLL "GFSDK_HairWorks.win32.dll"
#elif defined(_M_X64)
    #define hwSDKDLL "GFSDK_HairWorks.win64.dll"
#endif


bool operator==(const hwConversionSettings &a, const hwConversionSettings &b)
{
#define cmp(V) a.V==b.V
    return cmp(m_targetUpAxisHint) && cmp(m_targetHandednessHint) && cmp(m_pConversionMatrix) && cmp(m_targetSceneUnit);
#undef cmp
}

bool hwFileToString(std::string &o_buf, const char *path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) { return false; }
    f.seekg(0, std::ios::end);
    o_buf.resize(f.tellg());
    f.seekg(0, std::ios::beg);
    f.read(&o_buf[0], o_buf.size());
    return true;
}


hwSDK *g_hw_sdk = nullptr;

hwSDK* hwContext::loadSDK()
{
    if (g_hw_sdk) {
        return g_hw_sdk;
    }

    char path[MAX_PATH] = {0};
    if(path[0] == 0) {
        // get path to this module
        HMODULE mod = 0;
        ::GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCSTR)&hwInitialize, &mod);
        DWORD size = ::GetModuleFileNameA(mod, path, sizeof(path));
        for (int i = size - 1; i >= 0; --i) {
            if (path[i] == '\\') {
                path[i + 1] = '\0';
                std::strncat(path, hwSDKDLL, MAX_PATH);
                break;
            }
        }
    }
    g_hw_sdk = GFSDK_LoadHairSDK(path, GFSDK_HAIRWORKS_VERSION);
    hwLog("hwContext::loadSDK(): %s (%s)\n", g_hw_sdk ? "succeeded" : "failed", path);
    return g_hw_sdk;
}

void hwContext::unloadSDK()
{
    if (g_hw_sdk) {
        g_hw_sdk->Release();
        g_hw_sdk = nullptr;
        hwLog("hwContext::unloadSDK()\n");
    }
}

hwContext::hwContext()
{
}

hwContext::~hwContext()
{
    finalize();
}

bool hwContext::valid() const
{
    return m_d3ddev!=nullptr && m_d3dctx!=nullptr && g_hw_sdk!=nullptr;
}

bool hwContext::initialize(hwDevice *d3d_device)
{
    if (d3d_device == nullptr) { return false; }

    g_hw_sdk = loadSDK();
    if (g_hw_sdk != nullptr) {
        hwLog("GFSDK_LoadHairSDK() succeeded.\n");
    }
    else {
        hwLog("GFSDK_LoadHairSDK() failed.\n");
        return false;
    }

    if (g_hw_sdk->InitRenderResources((ID3D11Device*)d3d_device) == GFSDK_HAIR_RETURN_OK) {
        hwLog("GFSDK_HairSDK::InitRenderResources() succeeded.\n");
    }
    else {
        hwLog("GFSDK_HairSDK::InitRenderResources() failed.\n");
        finalize();
        return false;
    }

    m_d3ddev = (ID3D11Device*)d3d_device;
    m_d3ddev->GetImmediateContext(&m_d3dctx);
    if (g_hw_sdk->SetCurrentContext(m_d3dctx) == GFSDK_HAIR_RETURN_OK) {
        hwLog("GFSDK_HairSDK::SetCurrentContext() succeeded.\n");
    }
    else {
        hwLog("GFSDK_HairSDK::SetCurrentContext() failed.\n");
        finalize();
        return false;
    }

    {
        CD3D11_DEPTH_STENCIL_DESC desc;
        m_d3ddev->CreateDepthStencilState(&desc, &m_rs_enable_depth);
    }
    {
        // create constant buffer for hair rendering pixel shader
        D3D11_BUFFER_DESC desc;
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.ByteWidth = sizeof(hwConstantBuffer);
        desc.StructureByteStride = 0;
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        desc.MiscFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        m_d3ddev->CreateBuffer(&desc, 0, &m_rs_constant_buffer);
    }

    return true;
}

void hwContext::finalize()
{
    for (auto &i : m_instances) { instanceRelease(i.handle); }
    m_instances.clear();

    for (auto &i : m_assets) { assetRelease(i.handle); }
    m_assets.clear();

    for (auto &i : m_shaders) { shaderRelease(i.handle); }
    m_shaders.clear();

    for (auto &i : m_srvtable) { i.second->Release(); }
    m_srvtable.clear();

    for (auto &i : m_rtvtable) { i.second->Release(); }
    m_rtvtable.clear();

    if (m_rs_enable_depth) {
        m_rs_enable_depth->Release();
        m_rs_enable_depth = nullptr;
    }

    if (m_d3dctx)
    {
        m_d3dctx->Release();
        m_d3dctx = nullptr;
    }
}

void hwContext::move(hwContext &from)
{
#define mov(V) V=from.V; from.V=decltype(V)();

    mov(m_d3dctx);
    mov(m_d3ddev);

    mov(m_shaders);
    mov(m_assets);
    mov(m_instances);
    mov(m_srvtable);
    mov(m_rtvtable);
    //mov(m_commands);

    mov(m_rs_enable_depth);
    mov(m_rs_constant_buffer);
    //mov(m_cb);

#undef mov
}


hwShaderData& hwContext::newShaderData()
{
    auto i = std::find_if(m_shaders.begin(), m_shaders.end(), [](const hwShaderData &v) { return !v; });
    if (i != m_shaders.end()) { return *i; }

    hwShaderData tmp;
    tmp.handle = m_shaders.size();
    m_shaders.push_back(tmp);
    return m_shaders.back();
}

hwHShader hwContext::shaderLoadFromFile(const std::string &path)
{
    {
        auto i = std::find_if(m_shaders.begin(), m_shaders.end(), [&](const hwShaderData &v) { return v.path == path; });
        if (i != m_shaders.end() && i->ref_count > 0) {
            ++i->ref_count;
            return i->handle;
        }
    }

    std::string bin;
    if (!hwFileToString(bin, path.c_str())) {
        hwLog("failed to load shader (%s)\n", path.c_str());
        return hwNullHandle;
    }

    hwShaderData& v = newShaderData();
    v.path = path;
    if (SUCCEEDED(m_d3ddev->CreatePixelShader(&bin[0], bin.size(), nullptr, &v.shader))) {
        v.ref_count = 1;
        hwLog("CreatePixelShader(%s) : %d succeeded.\n", path.c_str(), v.handle);
        return v.handle;
    }
    else {
        hwLog("CreatePixelShader(%s) failed.\n", path.c_str());
    }
    return hwNullHandle;
}

void hwContext::shaderRelease(hwHShader hs)
{
    if (hs >= m_shaders.size()) { return; }

    auto &v = m_shaders[hs];
    if (v.ref_count > 0 && --v.ref_count == 0) {
        v.shader->Release();
        v.invalidate();
        hwLog("shaderRelease(%d)\n", hs);
    }
}

void hwContext::shaderReload(hwHShader hs)
{
    if (hs >= m_shaders.size()) { return; }

    auto &v = m_shaders[hs];
    // release existing shader
    if (v.shader) {
        v.shader->Release();
        v.shader = nullptr;
    }

    // reload
    std::string bin;
    if (!hwFileToString(bin, v.path.c_str())) {
        hwLog("failed to reload shader (%s)\n", v.path.c_str());
        return;
    }
    if (SUCCEEDED(m_d3ddev->CreatePixelShader(&bin[0], bin.size(), nullptr, &v.shader))) {
        hwLog("CreatePixelShader(%s) : %d reloaded.\n", v.path.c_str(), v.handle);
    }
    else {
        hwLog("CreatePixelShader(%s) failed to reload.\n", v.path.c_str());
    }
}


hwAssetData& hwContext::newAssetData()
{
    auto i = std::find_if(m_assets.begin(), m_assets.end(), [](const hwAssetData &v) { return !v; });
    if (i != m_assets.end()) { return *i; }

    hwAssetData tmp;
    tmp.handle = m_assets.size();
    m_assets.push_back(tmp);
    return m_assets.back();
}

hwHAsset hwContext::assetLoadFromFile(const std::string &path, const hwConversionSettings *_settings)
{
    hwConversionSettings settings;
    if (_settings != nullptr) { settings = *_settings; }

    {
        auto i = std::find_if(m_assets.begin(), m_assets.end(),
            [&](const hwAssetData &v) { return v.path == path && v.settings==settings; });
        if (i != m_assets.end() && i->ref_count > 0) {
            ++i->ref_count;
            return i->aid;
        }
    }

    hwAssetData& v = newAssetData();
    v.settings = settings;
    v.path = path;
    if (g_hw_sdk->LoadHairAssetFromFile(path.c_str(), &v.aid, nullptr, &settings) == GFSDK_HAIR_RETURN_OK) {
        v.ref_count = 1;

        hwLog("GFSDK_HairSDK::LoadHairAssetFromFile(\"%s\") : %d succeeded.\n", path.c_str(), v.handle);
        return v.handle;
    }
    else {
        hwLog("GFSDK_HairSDK::LoadHairAssetFromFile(\"%s\") failed.\n", path.c_str());
    }
    return hwNullHandle;
}

void hwContext::assetRelease(hwHAsset ha)
{
    if (ha >= m_assets.size()) { return; }

    auto &v = m_assets[ha];
    if (v.ref_count > 0 && --v.ref_count==0) {
        if (g_hw_sdk->FreeHairAsset(v.aid) == GFSDK_HAIR_RETURN_OK) {
            hwLog("GFSDK_HairSDK::FreeHairAsset(%d) succeeded.\n", ha);
        }
        else {
            hwLog("GFSDK_HairSDK::FreeHairAsset(%d) failed.\n", ha);
        }
        v.invalidate();
    }
}

void hwContext::assetReload(hwHAsset ha)
{
    if (ha >= m_assets.size()) { return; }

    auto &v = m_assets[ha];
    // release existing asset
    if (g_hw_sdk->FreeHairAsset(v.aid)) {
        v.aid = hwNullAssetID;
    }

    // reload
    if (g_hw_sdk->LoadHairAssetFromFile(v.path.c_str(), &v.aid, nullptr, &v.settings) == GFSDK_HAIR_RETURN_OK) {
        hwLog("GFSDK_HairSDK::LoadHairAssetFromFile(\"%s\") : %d reloaded.\n", v.path.c_str(), v.handle);
    }
    else {
        hwLog("GFSDK_HairSDK::LoadHairAssetFromFile(\"%s\") failed to reload.\n", v.path.c_str());
    }
}

int hwContext::assetGetNumBones(hwHAsset ha) const
{
    uint32_t r = 0;
    if (ha >= m_assets.size()) { return r; }

    if (g_hw_sdk->GetNumBones(m_assets[ha].aid, &r) != GFSDK_HAIR_RETURN_OK) {
        hwLog("GFSDK_HairSDK::GetNumBones(%d) failed.\n", ha);
    }
    return r;
}

const char* hwContext::assetGetBoneName(hwHAsset ha, int nth) const
{
    static char tmp[256];
    if (ha >= m_assets.size()) { tmp[0] = '\0'; return tmp; }

    if (g_hw_sdk->GetBoneName(m_assets[ha].aid, nth, tmp) != GFSDK_HAIR_RETURN_OK) {
        hwLog("GFSDK_HairSDK::GetBoneName(%d) failed.\n", ha);
    }
    return tmp;
}

void hwContext::assetGetBoneIndices(hwHAsset ha, hwFloat4 &o_indices) const
{
    if (ha >= m_assets.size()) { return; }

    if (g_hw_sdk->GetBoneIndices(m_assets[ha].aid, &o_indices) != GFSDK_HAIR_RETURN_OK) {
        hwLog("GFSDK_HairSDK::GetBoneIndices(%d) failed.\n", ha);
    }
}

void hwContext::assetGetBoneWeights(hwHAsset ha, hwFloat4 &o_weight) const
{
    if (ha >= m_assets.size()) { return; }

    if (g_hw_sdk->GetBoneWeights(m_assets[ha].aid, &o_weight) != GFSDK_HAIR_RETURN_OK) {
        hwLog("GFSDK_HairSDK::GetBoneWeights(%d) failed.\n", ha);
    }
}

void hwContext::assetGetBindPose(hwHAsset ha, int nth, hwMatrix &o_mat)
{
    if (ha >= m_assets.size()) { return; }

    if (g_hw_sdk->GetBindPose(m_assets[ha].aid, nth, &o_mat) != GFSDK_HAIR_RETURN_OK) {
        hwLog("GFSDK_HairSDK::GetBindPose(%d, %d) failed.\n", ha, nth);
    }
}

void hwContext::assetGetDefaultDescriptor(hwHAsset ha, hwHairDescriptor &o_desc) const
{
    if (ha >= m_assets.size()) { return; }

    if (g_hw_sdk->CopyInstanceDescriptorFromAsset(m_assets[ha].aid, o_desc) != GFSDK_HAIR_RETURN_OK) {
        hwLog("GFSDK_HairSDK::CopyInstanceDescriptorFromAsset(%d) failed.\n", ha);
    }
}



hwInstanceData& hwContext::newInstanceData()
{
    auto i = std::find_if(m_instances.begin(), m_instances.end(), [](const hwInstanceData &v) { return !v; });
    if (i != m_instances.end()) { return *i; }

    hwInstanceData tmp;
    tmp.handle = m_instances.size();
    m_instances.push_back(tmp);
    return m_instances.back();
}

hwHInstance hwContext::instanceCreate(hwHAsset ha)
{
    if (ha >= m_assets.size()) { return hwNullHandle; }

    hwInstanceData& v = newInstanceData();
    v.hasset = ha;
    if (g_hw_sdk->CreateHairInstance(m_assets[ha].aid, &v.iid) == GFSDK_HAIR_RETURN_OK) {
        hwLog("GFSDK_HairSDK::CreateHairInstance(%d) : %d succeeded.\n", ha, v.handle);
    }
    else
    {
        hwLog("GFSDK_HairSDK::CreateHairInstance(%d) failed.\n", ha);
    }
    return v.handle;
}

void hwContext::instanceRelease(hwHInstance hi)
{
    if (hi >= m_instances.size()) { return; }
    auto &v = m_instances[hi];

    if (g_hw_sdk->FreeHairInstance(v.iid) == GFSDK_HAIR_RETURN_OK) {
        hwLog("GFSDK_HairSDK::FreeHairInstance(%d) succeeded.\n", hi);
    }
    else {
        hwLog("GFSDK_HairSDK::FreeHairInstance(%d) failed.\n", hi);
    }
    v.invalidate();
}

void hwContext::instanceGetBounds(hwHInstance hi, hwFloat3 &o_min, hwFloat3 &o_max) const
{
    if (hi >= m_instances.size()) { return; }
    auto &v = m_instances[hi];

    if (g_hw_sdk->GetBounds(v.iid, &o_min, &o_max) != GFSDK_HAIR_RETURN_OK)
    {
        hwLog("GFSDK_HairSDK::GetBounds(%d) failed.\n", hi);
    }
}

void hwContext::instanceGetDescriptor(hwHInstance hi, hwHairDescriptor &desc) const
{
    if (hi >= m_instances.size()) { return; }
    auto &v = m_instances[hi];

    if (g_hw_sdk->CopyCurrentInstanceDescriptor(v.iid, desc) != GFSDK_HAIR_RETURN_OK)
    {
        hwLog("GFSDK_HairSDK::CopyCurrentInstanceDescriptor(%d) failed.\n", hi);
    }
}

void hwContext::instanceSetDescriptor(hwHInstance hi, const hwHairDescriptor &desc)
{
    if (hi >= m_instances.size()) { return; }
    auto &v = m_instances[hi];

    if (g_hw_sdk->UpdateInstanceDescriptor(v.iid, desc) != GFSDK_HAIR_RETURN_OK)
    {
        hwLog("GFSDK_HairSDK::UpdateInstanceDescriptor(%d) failed.\n", hi);
    }
}

void hwContext::instanceSetTexture(hwHInstance hi, hwTextureType type, hwTexture *tex)
{
    if (hi >= m_instances.size()) { return; }
    auto &v = m_instances[hi];

    auto *srv = getSRV(tex);
    if (!srv || g_hw_sdk->SetTextureSRV(v.iid, type, srv) != GFSDK_HAIR_RETURN_OK)
    {
        hwLog("GFSDK_HairSDK::SetTextureSRV(%d, %d) failed.\n", hi, type);
    }
}

void hwContext::instanceUpdateSkinningMatrices(hwHInstance hi, int num_bones, hwMatrix *matrices)
{
    if (matrices == nullptr) { return; }
    if (hi >= m_instances.size()) { return; }
    auto &v = m_instances[hi];

    if (g_hw_sdk->UpdateSkinningMatrices(v.iid, num_bones, matrices) != GFSDK_HAIR_RETURN_OK)
    {
        hwLog("GFSDK_HairSDK::UpdateSkinningMatrices(%d) failed.\n", hi);
    }
}

void hwContext::instanceUpdateSkinningDQs(hwHInstance hi, int num_bones, hwDQuaternion *dqs)
{
    if (dqs == nullptr) { return; }
    if (hi >= m_instances.size()) { return; }
    auto &v = m_instances[hi];

    if (g_hw_sdk->UpdateSkinningDQs(v.iid, num_bones, dqs) != GFSDK_HAIR_RETURN_OK)
    {
        hwLog("GFSDK_HairSDK::UpdateSkinningDQs(%d) failed.\n", hi);
    }
}


void hwContext::beginScene()
{
    m_mutex.lock();
}

void hwContext::endScene()
{
    m_mutex.unlock();
}

void hwContext::pushDeferredCall(const DeferredCall &c)
{
    m_commands.push_back(c);
}

void hwContext::setRenderTarget(hwTexture *framebuffer, hwTexture *depthbuffer)
{
    pushDeferredCall([=]() {
        setRenderTargetImpl(framebuffer, depthbuffer);
    });
}

void hwContext::setViewProjection(const hwMatrix &view, const hwMatrix &proj, float fov)
{
    pushDeferredCall([=]() {
        setViewProjectionImpl(view, proj, fov);
    });
}

void hwContext::setShader(hwHShader hs)
{
    pushDeferredCall([=]() {
        setShaderImpl(hs);
    });
}

void hwContext::setLights(int num_lights, const hwLightData *lights_)
{
    std::array<hwLightData, hwMaxLights> lights;
    num_lights = std::min<int>(num_lights, hwMaxLights);
    std::copy(lights_, lights_ + num_lights, &lights[0]);
    pushDeferredCall([=]() {
        setLightsImpl(num_lights, &lights[0]);
    });

}

void hwContext::render(hwHInstance hi)
{
    pushDeferredCall([=]() {
        renderImpl(hi);
    });
}

void hwContext::renderShadow(hwHInstance hi)
{
    pushDeferredCall([=]() {
        renderShadowImpl(hi);
    });
}

void hwContext::stepSimulation(float dt)
{
    std::unique_lock<std::mutex> lock(m_mutex);

    pushDeferredCall([=]() {
        stepSimulationImpl(dt);
    });
}


hwSRV* hwContext::getSRV(hwTexture *tex)
{
    {
        auto i = m_srvtable.find(tex);
        if (i != m_srvtable.end()) {
            return i->second;
        }
    }

    hwSRV *ret = nullptr;
    if (SUCCEEDED(m_d3ddev->CreateShaderResourceView(tex, nullptr, &ret))) {
        m_srvtable[tex] = ret;
    }
    return ret;
}

hwRTV* hwContext::getRTV(hwTexture *tex)
{
    {
        auto i = m_rtvtable.find(tex);
        if (i != m_rtvtable.end()) {
            return i->second;
        }
    }

    hwRTV *ret = nullptr;
    if (SUCCEEDED(m_d3ddev->CreateRenderTargetView(tex, nullptr, &ret))) {
        m_rtvtable[tex] = ret;
    }
    return ret;
}


void hwContext::setRenderTargetImpl(hwTexture *framebuffer, hwTexture *depthbuffer)
{
    // todo
}

void hwContext::setViewProjectionImpl(const hwMatrix &view, const hwMatrix &proj, float fov)
{
    if (g_hw_sdk->SetViewProjection((const gfsdk_float4x4*)&view, (const gfsdk_float4x4*)&proj, GFSDK_HAIR_RIGHT_HANDED, fov) != GFSDK_HAIR_RETURN_OK)
    {
        hwLog("GFSDK_HairSDK::SetViewProjection() failed.\n");
    }
}

void hwContext::setShaderImpl(hwHShader hs)
{
    if (hs >= m_shaders.size()) { return; }

    auto &v = m_shaders[hs];
    if (v.shader) {
        m_d3dctx->PSSetShader(v.shader, nullptr, 0);
    }
}

void hwContext::setLightsImpl(int num_lights, const hwLightData *lights)
{
    m_cb.num_lights = num_lights;
    std::copy(lights, lights + num_lights, m_cb.lights);
}

void hwContext::renderImpl(hwHInstance hi)
{
    if (hi >= m_instances.size()) { return; }
    auto &v = m_instances[hi];

    // update constant buffer
    {
        g_hw_sdk->PrepareShaderConstantBuffer(v.iid, &m_cb.hw);

        D3D11_MAPPED_SUBRESOURCE MappedResource;
        m_d3dctx->Map(m_rs_constant_buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &MappedResource);
        *((hwConstantBuffer*)MappedResource.pData) = m_cb;
        m_d3dctx->Unmap(m_rs_constant_buffer, 0);

        m_d3dctx->PSSetConstantBuffers(0, 1, &m_rs_constant_buffer);
    }

    // set shader resource views
    {
        ID3D11ShaderResourceView* SRVs[GFSDK_HAIR_NUM_SHADER_RESOUCES];
        g_hw_sdk->GetShaderResources(v.iid, SRVs);
        m_d3dctx->PSSetShaderResources(0, GFSDK_HAIR_NUM_SHADER_RESOUCES, SRVs);
    }

    // render
    auto settings = GFSDK_HairShaderSettings(true, false);
    if (g_hw_sdk->RenderHairs(v.iid, &settings) != GFSDK_HAIR_RETURN_OK)
    {
        hwLog("GFSDK_HairSDK::RenderHairs(%d) failed.\n", hi);
    }

    // render indicators
    g_hw_sdk->RenderVisualization(v.iid);
}

void hwContext::renderShadowImpl(hwHInstance hi)
{
    if (hi >= m_instances.size()) { return; }
    auto &v = m_instances[hi];

    // set shader resource views
    {
        ID3D11ShaderResourceView* SRVs[GFSDK_HAIR_NUM_SHADER_RESOUCES];
        g_hw_sdk->GetShaderResources(v.iid, SRVs);
        m_d3dctx->PSSetShaderResources(0, GFSDK_HAIR_NUM_SHADER_RESOUCES, SRVs);
    }

    auto settings = GFSDK_HairShaderSettings(false, true);
    if (g_hw_sdk->RenderHairs(v.iid, &settings) != GFSDK_HAIR_RETURN_OK)
    {
        hwLog("GFSDK_HairSDK::RenderHairs(%d) failed.\n", hi);
    }
}

void hwContext::stepSimulationImpl(float dt)
{
    if (g_hw_sdk->StepSimulation(dt) != GFSDK_HAIR_RETURN_OK)
    {
        hwLog("GFSDK_HairSDK::StepSimulation(%f) failed.\n", dt);
    }
}

void hwContext::flush()
{
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_commands_back = m_commands;
        m_commands.clear();
    }

    m_d3dctx->OMSetDepthStencilState(m_rs_enable_depth, 0);
    for (auto& c : m_commands_back) {
        c();
    }
    m_commands_back.clear();
}

