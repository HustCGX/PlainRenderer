#include "pch.h"
#include "RenderFrontend.h"
#include "Runtime/ImageLoader.h"

//disable ImGui warning
#pragma warning( push )
#pragma warning( disable : 26495)

#include <imgui/imgui.h>

//reenable warning
#pragma warning( pop )

#include <Utilities/MathUtils.h>
#include "Runtime/Timer.h"
#include "Culling.h"
#include "Utilities/GeneralUtils.h"
#include "Common/MeshProcessing.h"

#define GLFW_INCLUDE_VULKAN
#include "GLFW/glfw3.h"

//definition of extern variable from header
RenderFrontend gRenderFrontend;

const uint32_t shadowMapRes = 2048;
const uint32_t skyTextureRes = 1024;
const uint32_t specularProbeRes = 512;
const uint32_t diffuseProbeRes = 256;
const uint32_t skyTextureMipCount = 8;
const uint32_t brdfLutRes = 512;
const uint32_t nHistogramBins = 128;
const uint32_t shadowCascadeCount = 4;

const uint32_t histogramTileSizeX = 32;
const uint32_t histogramTileSizeY = 32;

const uint32_t  skyShadowMapRes = 1024;
const uint32_t  skyOcclusionVolumeMaxRes = 256;
const float     skyOcclusionTargetDensity = 0.5f; //meter/texel
const uint32_t  skyOcclusionSampleCount = 1024;


void resizeCallback(GLFWwindow* window, int width, int height) {
    RenderFrontend* frontEnd = reinterpret_cast<RenderFrontend*>(glfwGetWindowUserPointer(window));
    frontEnd->setResolution(width, height);
}

DefaultTextures createDefaultTextures() {
    DefaultTextures defaultTextures;
    //albedo
    {
        ImageDescription defaultDiffuseDesc;
        defaultDiffuseDesc.autoCreateMips = true;
        defaultDiffuseDesc.depth = 1;
        defaultDiffuseDesc.format = ImageFormat::RGBA8;
        defaultDiffuseDesc.initialData = { 255, 255, 255, 255 };
        defaultDiffuseDesc.manualMipCount = 1;
        defaultDiffuseDesc.mipCount = MipCount::FullChain;
        defaultDiffuseDesc.type = ImageType::Type2D;
        defaultDiffuseDesc.usageFlags = ImageUsageFlags::Sampled;
        defaultDiffuseDesc.width = 1;
        defaultDiffuseDesc.height = 1;

        defaultTextures.diffuse = gRenderBackend.createImage(defaultDiffuseDesc);
    }
    //specular
    {
        ImageDescription defaultSpecularDesc;
        defaultSpecularDesc.autoCreateMips = true;
        defaultSpecularDesc.depth = 1;
        defaultSpecularDesc.format = ImageFormat::RGBA8;
        defaultSpecularDesc.initialData = { 0, 128, 255, 0 };
        defaultSpecularDesc.manualMipCount = 1;
        defaultSpecularDesc.mipCount = MipCount::FullChain;
        defaultSpecularDesc.type = ImageType::Type2D;
        defaultSpecularDesc.usageFlags = ImageUsageFlags::Sampled;
        defaultSpecularDesc.width = 1;
        defaultSpecularDesc.height = 1;

        defaultTextures.specular = gRenderBackend.createImage(defaultSpecularDesc);
    }
    //normal
    {
        ImageDescription defaultNormalDesc;
        defaultNormalDesc.autoCreateMips = true;
        defaultNormalDesc.depth = 1;
        defaultNormalDesc.format = ImageFormat::RG8;
        defaultNormalDesc.initialData = { 128, 128 };
        defaultNormalDesc.manualMipCount = 1;
        defaultNormalDesc.mipCount = MipCount::FullChain;
        defaultNormalDesc.type = ImageType::Type2D;
        defaultNormalDesc.usageFlags = ImageUsageFlags::Sampled;
        defaultNormalDesc.width = 1;
        defaultNormalDesc.height = 1;

        defaultTextures.normal = gRenderBackend.createImage(defaultNormalDesc);
    }
    //sky
    {
        ImageDescription defaultCubemapDesc;
        defaultCubemapDesc.autoCreateMips = true;
        defaultCubemapDesc.depth = 1;
        defaultCubemapDesc.format = ImageFormat::RGBA8;
        defaultCubemapDesc.initialData = { 255, 255, 255, 255 };
        defaultCubemapDesc.manualMipCount = 1;
        defaultCubemapDesc.mipCount = MipCount::FullChain;
        defaultCubemapDesc.type = ImageType::Type2D;
        defaultCubemapDesc.usageFlags = ImageUsageFlags::Sampled;
        defaultCubemapDesc.width = 1;
        defaultCubemapDesc.height = 1;

        defaultTextures.sky = gRenderBackend.createImage(defaultCubemapDesc);
    }
    return defaultTextures;
}

glm::vec2 computeProjectionMatrixJitter(const float pixelSizeX, const float pixelSizeY) {
    static uint32_t jitterIndex;
    glm::vec2 offset = hammersley2D(jitterIndex) - glm::vec2(0.5f);
    offset.x *= pixelSizeX;
    offset.y *= pixelSizeY;

    jitterIndex++;
    const uint32_t sampleCount = 16;
    jitterIndex %= sampleCount;

    return offset;
}

glm::mat4 applyProjectionMatrixJitter(const glm::mat4& projectionMatrix, const glm::vec2& offset) {

    glm::mat4 jitteredProjection = projectionMatrix;
    jitteredProjection[2][0] = offset.x;
    jitteredProjection[2][1] = offset.y;

    return jitteredProjection;
}

void RenderFrontend::setup(GLFWwindow* window) {
    m_window = window;

    int width, height;
    glfwGetWindowSize(window, &width, &height);
    m_screenWidth = width;
    m_screenHeight = height;
    m_camera.intrinsic.aspectRatio = (float)width / (float)height;

    glfwSetWindowUserPointer(window, this);
    glfwSetFramebufferSizeCallback(window, resizeCallback);

    m_defaultTextures = createDefaultTextures();
    initSamplers();
    initImages();

    const auto histogramSettings = createHistogramSettings();
    initBuffers(histogramSettings);
    initRenderpasses(histogramSettings);

    initMeshs();
    
    //IBL preprocessing
    gRenderBackend.newFrame();
    computeBRDFLut();
    skyCubemapFromTexture();
    skyCubemapIBLPreProcessing(m_cubemapMipPasses);
    gRenderBackend.renderFrame(false);
}

void RenderFrontend::shutdown() {
    
}

void RenderFrontend::prepareNewFrame() {
    if (m_didResolutionChange) {
        gRenderBackend.recreateSwapchain(m_screenWidth, m_screenHeight, m_window);
        gRenderBackend.resizeImages( { m_colorBuffer, m_depthBuffer, m_motionVectorBuffer, 
            m_historyBuffer }, m_screenWidth, m_screenHeight);
        gRenderBackend.resizeImages({ m_minMaxDepthPyramid}, m_screenWidth / 2, m_screenHeight / 2);
        m_didResolutionChange = false;

        uint32_t threadgroupCount = 0;
        gRenderBackend.updateComputePassShaderDescription(m_depthPyramidPass, createDepthPyramidShaderDescription(&threadgroupCount));
    }
    if (m_minimized) {
        return;
    }

    m_currentMeshCount = 0;
    m_currentMainPassDrawcallCount = 0;
    m_currentShadowPassDrawcallCount = 0;

    if (m_isMainPassShaderDescriptionStale) {
        gRenderBackend.updateGraphicPassShaderDescription(m_mainPass, createForwardPassShaderDescription(m_shadingConfig));
        m_isMainPassShaderDescriptionStale = false;
    }

    if (m_isBRDFLutShaderDescriptionStale) {
        gRenderBackend.updateComputePassShaderDescription(m_brdfLutPass, createBRDFLutShaderDescription(m_shadingConfig));
        //don't reset m_isMainPassShaderDescriptionStale, this is done when rendering as it's used to trigger lut recreation
    }

    if (m_isTAAShaderDescriptionStale) {
        gRenderBackend.updateComputePassShaderDescription(m_taaPass, createTAAShaderDescription());
        m_isTAAShaderDescriptionStale = false;
    }

    gRenderBackend.updateShaderCode();
    gRenderBackend.newFrame();
    m_bbsToDebugDraw.clear(); 

    prepareRenderpasses();
    updateGlobalShaderInfo();
}

void RenderFrontend::prepareRenderpasses(){

    std::vector<RenderPassHandle> preparationPasses;

    if (m_isBRDFLutShaderDescriptionStale) {
        computeBRDFLut();
        preparationPasses.push_back(m_brdfLutPass);
        m_isBRDFLutShaderDescriptionStale = false;
    }

    renderSunShadowCascades();
    computeColorBufferHistogram();
    computeExposure();
    renderDepthPrepass();
    computeDepthPyramid();
    computeSunLightMatrices();
    renderForwardShading(preparationPasses);

    //for sky and debug models, first matrix is mvp with identity model matrix, secondary is unused
    const std::array<glm::mat4, 2> defaultTransform = { m_viewProjectionMatrix, glm::mat4(1.f) };

    //update debug geo
    if (m_freezeAndDrawCameraFrustum) {
        gRenderBackend.drawDynamicMeshes({ m_cameraFrustumModel }, { defaultTransform }, m_debugGeoPass);
    }
    if (m_drawShadowFrustum) {
        gRenderBackend.drawDynamicMeshes({ m_shadowFrustumModel }, { defaultTransform }, m_debugGeoPass);
    }
    if (m_drawBBs) {
        updateBoundingBoxDebugGeo();
    }

    const bool drawDebugPass =
        m_freezeAndDrawCameraFrustum ||
        m_drawShadowFrustum ||
        m_drawBBs;

    //debug pass
    if (drawDebugPass) {
        renderDebugGeometry();
    }
    renderSky(drawDebugPass);
    computeTAA();
    copyColorToHistoryBuffer();
    computeTonemapping();
}

void RenderFrontend::setResolution(const uint32_t width, const uint32_t height) {
    m_screenWidth = width;
    m_screenHeight = height;
    m_camera.intrinsic.aspectRatio = (float)width / (float)height;
    if (width == 0 || height == 0) {
        m_minimized = true;
        return;
    }
    else {
        m_minimized = false;
    }
    m_didResolutionChange = true;
}

void RenderFrontend::setCameraExtrinsic(const CameraExtrinsic& extrinsic) {

    m_previousViewProjectionMatrix = m_viewProjectionMatrix;
    m_globalShaderInfo.previousFrameCameraJitter = m_globalShaderInfo.currentFrameCameraJitter;

    m_camera.extrinsic = extrinsic;
    const glm::mat4 viewMatrix = viewMatrixFromCameraExtrinsic(extrinsic);
    const glm::mat4 projectionMatrix = projectionMatrixFromCameraIntrinsic(m_camera.intrinsic);

    //jitter matrix for TAA
    {
        const float pixelSizeX = 1.f / m_screenWidth;
        const float pixelSizeY = 1.f / m_screenHeight;

        m_globalShaderInfo.currentFrameCameraJitter = computeProjectionMatrixJitter(pixelSizeX, pixelSizeY);
        const glm::mat4 jitteredProjection = applyProjectionMatrixJitter(projectionMatrix, m_globalShaderInfo.currentFrameCameraJitter);

        m_viewProjectionMatrix = jitteredProjection * viewMatrix;
    }    

    if (!m_freezeAndDrawCameraFrustum) {
        updateCameraFrustum();
    }

    updateShadowFrustum();
}

void RenderFrontend::addStaticMeshes(const std::vector<MeshBinary>& meshData, const std::vector<glm::mat4>& transforms) {

    assert(meshData.size() == transforms.size());
    
    std::vector<Material> materials;
    materials.reserve(meshData.size());

    for (const auto& data : meshData) {
        Material material;

        if (!loadImageFromPath(data.texturePaths.albedoTexturePath, &material.diffuseTexture)) {
            material.diffuseTexture = m_defaultTextures.diffuse;
        }
        if (!loadImageFromPath(data.texturePaths.normalTexturePath, &material.normalTexture)) {
            material.normalTexture = m_defaultTextures.normal;
        }
        if (!loadImageFromPath(data.texturePaths.specularTexturePath, &material.specularTexture)) {
            material.specularTexture = m_defaultTextures.specular;
        }

        materials.push_back(material);
    }
    
    const auto backendHandles = gRenderBackend.createMeshes(meshData, materials);
    
    //compute and store bounding boxes    
    const uint32_t meshCount = glm::min(backendHandles.size(), transforms.size());

    for (uint32_t i = 0; i < meshCount; i++) {

        StaticMesh staticMesh;
        staticMesh.backendHandle = backendHandles[i];
        staticMesh.modelMatrix = transforms[i];
        staticMesh.bbWorldSpace = axisAlignedBoundingBoxTransformed(meshData[i].boundingBox, staticMesh.modelMatrix);

        m_staticMeshes.push_back(staticMesh);

        //create debug mesh for rendering
        const auto debugMesh = gRenderBackend.createDynamicMeshes(
            { axisAlignedBoundingBoxPositionsPerMesh }, { axisAlignedBoundingBoxIndicesPerMesh }).back();
        m_bbDebugMeshes.push_back(debugMesh);
    }
}

void RenderFrontend::renderStaticMeshes() {

    //if we prepare render commands without consuming them we will save up a huge amount of commands
    //so commands are not recorded if minmized in the first place
    if (m_minimized) {
        return;
    }

    m_currentMeshCount += (uint32_t)m_staticMeshes.size();

    //main and prepass
    {
        std::vector<MeshHandle> culledMeshes;
        std::vector<std::array<glm::mat4, 2>> culledTransformsMainPass; //contains MVP and model matrix
        std::vector<std::array<glm::mat4, 2>> culledTransformsPrepass;  //contains MVP and previous mvp

        //frustum culling
        for (const StaticMesh& mesh : m_staticMeshes) {

            const auto mvp = m_viewProjectionMatrix * mesh.modelMatrix;

            const bool renderMesh = isAxisAlignedBoundingBoxIntersectingViewFrustum(m_cameraFrustum, mesh.bbWorldSpace);

            if (renderMesh) {
                m_currentMainPassDrawcallCount++;

                culledMeshes.push_back(mesh.backendHandle);

                const std::array<glm::mat4, 2> mainPassTransforms = { mvp, mesh.modelMatrix };
                culledTransformsMainPass.push_back(mainPassTransforms);

                const glm::mat4 previousMVP = m_previousViewProjectionMatrix * mesh.modelMatrix;
                const std::array<glm::mat4, 2> prePassTransforms = { mvp, previousMVP };
                culledTransformsPrepass.push_back(prePassTransforms);

                if (m_drawBBs) {
                    m_bbsToDebugDraw.push_back(mesh.bbWorldSpace);
                }
            }
        }
        gRenderBackend.drawMeshes(culledMeshes, culledTransformsMainPass, m_mainPass);
        gRenderBackend.drawMeshes(culledMeshes, culledTransformsPrepass, m_depthPrePass);
    }
    
    //shadow pass
    {
        std::vector<MeshHandle> culledMeshes;
        std::vector<std::array<glm::mat4, 2>> culledTransforms; //model matrix and secondary unused for now 

        const glm::vec3 sunDirection = directionToVector(m_sunDirection);
        //we must not cull behind the shadow frustum near plane, as objects there cast shadows into the visible area
        //for now we simply offset the near plane points very far into the light direction
        //this means that all objects in that direction within the moved distance will intersect our frustum and aren't culled
        const float nearPlaneExtensionLength = 10000.f;
        const glm::vec3 nearPlaneOffset = sunDirection * nearPlaneExtensionLength;
        m_sunShadowFrustum.points.l_l_n += nearPlaneOffset;
        m_sunShadowFrustum.points.r_l_n += nearPlaneOffset;
        m_sunShadowFrustum.points.l_u_n += nearPlaneOffset;
        m_sunShadowFrustum.points.r_u_n += nearPlaneOffset;

        //coarse frustum culling for shadow rendering, assuming shadow frustum if fitted to camera frustum
        //actual frustum is fitted tightly to depth buffer values, but that is done on the GPU
        for (const StaticMesh& mesh : m_staticMeshes) {

            const std::array<glm::mat4, 2> transforms = { glm::mat4(1.f), mesh.modelMatrix };
            const bool renderMesh = isAxisAlignedBoundingBoxIntersectingViewFrustum(m_sunShadowFrustum, mesh.bbWorldSpace);

            if (renderMesh) {
                m_currentShadowPassDrawcallCount++;

                culledMeshes.push_back(mesh.backendHandle);
                culledTransforms.push_back(transforms);
            }
        }
        for (uint32_t shadowPass = 0; shadowPass < m_shadowPasses.size(); shadowPass++) {
            gRenderBackend.drawMeshes(culledMeshes, culledTransforms, m_shadowPasses[shadowPass]);
        }
    }  
}

void RenderFrontend::renderFrame() {

    if (m_minimized) {
        return;
    }

    drawUi();
    const std::array<glm::mat4, 2> defaultTransform = { m_viewProjectionMatrix, glm::mat4(1.f) };
    gRenderBackend.drawMeshes(std::vector<MeshHandle> {m_skyCube}, { defaultTransform }, m_skyPass);
    gRenderBackend.renderFrame(true);
}

void RenderFrontend::computeColorBufferHistogram() const {

    StorageBufferResource histogramPerTileResource(m_histogramPerTileBuffer, false, 0);
    StorageBufferResource histogramResource(m_histogramBuffer, false, 1);

    //histogram per tile
    {
        ImageResource colorTextureResource(m_colorBuffer, 0, 2);
        SamplerResource texelSamplerResource(m_defaultTexelSampler, 4);
        StorageBufferResource lightBufferResource(m_lightBuffer, true, 3);

        RenderPassExecution histogramPerTileExecution;
        histogramPerTileExecution.handle = m_histogramPerTilePass;
        histogramPerTileExecution.resources.storageBuffers = { histogramPerTileResource, lightBufferResource };
        histogramPerTileExecution.resources.samplers = { texelSamplerResource };
        histogramPerTileExecution.resources.sampledImages = { colorTextureResource };
        histogramPerTileExecution.dispatchCount[0] = uint32_t(std::ceilf((float)m_screenWidth / float(histogramTileSizeX)));
        histogramPerTileExecution.dispatchCount[1] = uint32_t(std::ceilf((float)m_screenHeight / float(histogramTileSizeY)));
        histogramPerTileExecution.dispatchCount[2] = 1;

        gRenderBackend.setRenderPassExecution(histogramPerTileExecution);
    }

    const float binsPerDispatch = 64.f;
    //reset global tile
    {
        RenderPassExecution histogramResetExecution;
        histogramResetExecution.handle = m_histogramResetPass;
        histogramResetExecution.resources.storageBuffers = { histogramResource };
        histogramResetExecution.dispatchCount[0] = uint32_t(std::ceilf(float(nHistogramBins) / binsPerDispatch));
        histogramResetExecution.dispatchCount[1] = 1;
        histogramResetExecution.dispatchCount[2] = 1;

        gRenderBackend.setRenderPassExecution(histogramResetExecution);
    }
    //combine tiles
    {
        RenderPassExecution histogramCombineTilesExecution;
        histogramCombineTilesExecution.handle = m_histogramCombinePass;
        histogramCombineTilesExecution.resources.storageBuffers = { histogramPerTileResource, histogramResource };
        uint32_t tileCount =
            (uint32_t)std::ceilf(m_screenWidth / float(histogramTileSizeX)) *
            (uint32_t)std::ceilf(m_screenHeight / float(histogramTileSizeY));
        histogramCombineTilesExecution.dispatchCount[0] = tileCount;
        histogramCombineTilesExecution.dispatchCount[1] = uint32_t(std::ceilf(float(nHistogramBins) / binsPerDispatch));
        histogramCombineTilesExecution.dispatchCount[2] = 1;
        histogramCombineTilesExecution.parents = { m_histogramPerTilePass, m_histogramResetPass };

        gRenderBackend.setRenderPassExecution(histogramCombineTilesExecution);
    }
}
   
void RenderFrontend::renderSky(const bool drewDebugPasses) const {
    const auto skyTextureResource = ImageResource(m_skyTexture, 0, 0);
    const auto skySamplerResource = SamplerResource(m_cubeSampler, 1);
    const auto lightBufferResource = StorageBufferResource(m_lightBuffer, true, 2);

    RenderPassExecution skyPassExecution;
    skyPassExecution.handle = m_skyPass;
    skyPassExecution.resources.storageBuffers = { lightBufferResource };
    skyPassExecution.resources.sampledImages = { skyTextureResource };
    skyPassExecution.resources.samplers = { skySamplerResource };
    skyPassExecution.parents = { m_mainPass };
    if (drewDebugPasses) {
        skyPassExecution.parents.push_back(m_debugGeoPass);
    }
    gRenderBackend.setRenderPassExecution(skyPassExecution);
}

void RenderFrontend::renderSunShadowCascades() const {
    for (uint32_t i = 0; i < shadowCascadeCount; i++) {
        RenderPassExecution shadowPassExecution;
        shadowPassExecution.handle = m_shadowPasses[i];
        shadowPassExecution.parents = { m_lightMatrixPass };

        StorageBufferResource lightMatrixBufferResource(m_sunShadowInfoBuffer, true, 0);
        shadowPassExecution.resources.storageBuffers = { lightMatrixBufferResource };

        gRenderBackend.setRenderPassExecution(shadowPassExecution);
    }
}

void RenderFrontend::computeExposure() const {
    StorageBufferResource lightBufferResource(m_lightBuffer, false, 0);
    StorageBufferResource histogramResource(m_histogramBuffer, false, 1);

    RenderPassExecution preExposeLightsExecution;
    preExposeLightsExecution.handle = m_preExposeLightsPass;
    preExposeLightsExecution.resources.storageBuffers = { histogramResource, lightBufferResource };
    preExposeLightsExecution.parents = { m_histogramCombinePass };
    preExposeLightsExecution.dispatchCount[0] = 1;
    preExposeLightsExecution.dispatchCount[1] = 1;
    preExposeLightsExecution.dispatchCount[2] = 1;

    gRenderBackend.setRenderPassExecution(preExposeLightsExecution);
}

void RenderFrontend::renderDepthPrepass() const {
    RenderPassExecution prepassExe;
    prepassExe.handle = m_depthPrePass;
    gRenderBackend.setRenderPassExecution(prepassExe);
}

void RenderFrontend::computeDepthPyramid() const {
    RenderPassExecution exe;
    exe.handle = m_depthPyramidPass;
    exe.parents = { m_depthPrePass };
    const glm::ivec2 dispatchCount = computeDepthPyramidDispatchCount();
    exe.dispatchCount[0] = dispatchCount.x;
    exe.dispatchCount[1] = dispatchCount.y;
    exe.dispatchCount[2] = 1;

    ImageResource depthBufferResource(m_depthBuffer, 0, 13);
    ImageResource depthPyramidResource(m_minMaxDepthPyramid, 0, 15);

    exe.resources.sampledImages = { depthBufferResource, depthPyramidResource };

    SamplerResource clampedDepthSamplerResource(m_clampedDepthSampler, 14);
    exe.resources.samplers = { clampedDepthSamplerResource };

    StorageBufferResource syncBuffer(m_depthPyramidSyncBuffer, false, 16);
    exe.resources.storageBuffers = { syncBuffer };

    const uint32_t mipCount = mipCountFromResolution(m_screenWidth / 2, m_screenHeight / 2, 1);
    const uint32_t maxMipCount = 11; //see shader for details
    if (mipCount > maxMipCount) {
        std::cout << "Warning: depth pyramid mip count exceeds calculation shader max\n";
    }
    exe.resources.storageImages.reserve(maxMipCount);
    const uint32_t unusedMipCount = maxMipCount - mipCount;
    for (uint32_t i = 0; i < maxMipCount; i++) {
        const uint32_t mipLevel = i >= unusedMipCount ? i - unusedMipCount : 0;
        ImageResource pyramidMip(m_minMaxDepthPyramid, mipLevel, i);
        exe.resources.storageImages.push_back(pyramidMip);
    }
    gRenderBackend.setRenderPassExecution(exe);
}

void RenderFrontend::computeSunLightMatrices() const{
    RenderPassExecution exe;
    exe.handle = m_lightMatrixPass;
    exe.parents = { m_depthPyramidPass };
    exe.dispatchCount[0] = 1;
    exe.dispatchCount[1] = 1;
    exe.dispatchCount[2] = 1;

    const uint32_t depthPyramidMipCount = mipCountFromResolution(m_screenWidth / 2, m_screenHeight / 2, 1);
    ImageResource depthPyramidLowestMipResource(m_minMaxDepthPyramid, depthPyramidMipCount - 1, 1);
    exe.resources.storageImages = { depthPyramidLowestMipResource };

    StorageBufferResource lightMatrixBuffer(m_sunShadowInfoBuffer, false, 0);
    exe.resources.storageBuffers = { lightMatrixBuffer };

    gRenderBackend.setRenderPassExecution(exe);
}

void RenderFrontend::renderForwardShading(const std::vector<RenderPassHandle>& externalDependencies) const {
    const auto shadowSamplerResource = SamplerResource(m_shadowSampler, 0);
    const auto diffuseProbeResource = ImageResource(m_diffuseProbe, 0, 1);
    const auto cubeSamplerResource = SamplerResource(m_cubeSampler, 2);
    const auto brdfLutResource = ImageResource(m_brdfLut, 0, 3);
    const auto specularProbeResource = ImageResource(m_specularProbe, 0, 4);
    const auto cubeSamplerMipsResource = SamplerResource(m_skySamplerWithMips, 5);
    const auto lustSamplerResource = SamplerResource(m_lutSampler, 6);
    const auto lightBufferResource = StorageBufferResource(m_lightBuffer, true, 7);
    const auto lightMatrixBuffer = StorageBufferResource(m_sunShadowInfoBuffer, true, 8);

    const ImageResource occlusionVolumeResource(m_skyOcclusionVolume, 0, 13);
    const UniformBufferResource skyOcclusionInfoBuffer(m_skyOcclusionDataBuffer, 14);
    const SamplerResource occlusionSamplerResource(m_skyOcclusionSampler, 15);

    RenderPassExecution mainPassExecution;
    mainPassExecution.handle = m_mainPass;
    mainPassExecution.resources.storageBuffers = { lightBufferResource, lightMatrixBuffer };
    mainPassExecution.resources.sampledImages = { diffuseProbeResource, brdfLutResource, specularProbeResource, occlusionVolumeResource };
    mainPassExecution.resources.uniformBuffers = { skyOcclusionInfoBuffer };
    mainPassExecution.resources.samplers = { shadowSamplerResource, cubeSamplerResource,
        cubeSamplerMipsResource, lustSamplerResource, occlusionSamplerResource };

    //add shadow map cascade resources
    for (uint32_t i = 0; i < shadowCascadeCount; i++) {
        const auto shadowMapResource = ImageResource(m_shadowMaps[i], 0, 9 + i);
        mainPassExecution.resources.sampledImages.push_back(shadowMapResource);
    }

    mainPassExecution.parents = { m_preExposeLightsPass, m_depthPrePass, m_lightMatrixPass };
    mainPassExecution.parents.insert(mainPassExecution.parents.end(), m_shadowPasses.begin(), m_shadowPasses.end());
    mainPassExecution.parents.insert(mainPassExecution.parents.begin(), externalDependencies.begin(), externalDependencies.end());

    gRenderBackend.setRenderPassExecution(mainPassExecution);
}

void RenderFrontend::computeTAA() const {
    ImageResource colorBufferResource(m_colorBuffer, 0, 0);
    ImageResource previousFrameResource(m_historyBuffer, 0, 1);
    ImageResource motionBufferResource(m_motionVectorBuffer, 0, 2);
    ImageResource depthBufferResource(m_depthBuffer, 0, 3);
    SamplerResource samplerResource(m_colorSampler, 4);

    RenderPassExecution taaExecution;
    taaExecution.handle = m_taaPass;
    taaExecution.resources.storageImages = { colorBufferResource };
    taaExecution.resources.sampledImages = { previousFrameResource, motionBufferResource, depthBufferResource };
    taaExecution.resources.samplers = { samplerResource };
    taaExecution.dispatchCount[0] = (uint32_t)std::ceil(m_screenWidth / 8.f);
    taaExecution.dispatchCount[1] = (uint32_t)std::ceil(m_screenHeight / 8.f);
    taaExecution.dispatchCount[2] = 1;
    taaExecution.parents = { m_skyPass };

    gRenderBackend.setRenderPassExecution(taaExecution);
}

void RenderFrontend::computeTonemapping() const {
    const auto swapchainInput = gRenderBackend.getSwapchainInputImage();
    ImageResource targetResource(swapchainInput, 0, 0);
    ImageResource colorBufferResource(m_colorBuffer, 0, 1);
    SamplerResource samplerResource(m_defaultTexelSampler, 2);

    RenderPassExecution tonemappingExecution;
    tonemappingExecution.handle = m_tonemappingPass;
    tonemappingExecution.resources.storageImages = { targetResource };
    tonemappingExecution.resources.sampledImages = { colorBufferResource };
    tonemappingExecution.resources.samplers = { samplerResource };
    tonemappingExecution.dispatchCount[0] = (uint32_t)std::ceil(m_screenWidth / 8.f);
    tonemappingExecution.dispatchCount[1] = (uint32_t)std::ceil(m_screenHeight / 8.f);
    tonemappingExecution.dispatchCount[2] = 1;
    tonemappingExecution.parents = { m_taaPass };

    gRenderBackend.setRenderPassExecution(tonemappingExecution);
}

void RenderFrontend::renderDebugGeometry() const {
    RenderPassExecution debugPassExecution;
    debugPassExecution.handle = m_debugGeoPass;
    debugPassExecution.parents = { m_mainPass };
    gRenderBackend.setRenderPassExecution(debugPassExecution);
}

void RenderFrontend::updateBoundingBoxDebugGeo() {
    std::vector<std::vector<glm::vec3>> positionsPerMesh;
    std::vector<std::vector<uint32_t>>  indicesPerMesh;

    positionsPerMesh.reserve(m_bbDebugMeshes.size());
    indicesPerMesh.reserve(m_bbDebugMeshes.size());
    for (const auto& bb : m_bbsToDebugDraw) {
        std::vector<glm::vec3> vertices;
        std::vector<uint32_t> indices;

        axisAlignedBoundingBoxToLineMesh(bb, &vertices, &indices);

        positionsPerMesh.push_back(vertices);
        indicesPerMesh.push_back(indices);
    }

    //subvector with correct handle count
    std::vector<DynamicMeshHandle> bbMeshHandles(&m_bbDebugMeshes[0], &m_bbDebugMeshes[positionsPerMesh.size()]);

    gRenderBackend.updateDynamicMeshes(bbMeshHandles, positionsPerMesh, indicesPerMesh);

    const std::array<glm::mat4, 2> defaultTransform = { m_viewProjectionMatrix, glm::mat4(1.f) };
    std::vector<std::array<glm::mat4, 2>> debugMeshTransforms(m_bbsToDebugDraw.size(), defaultTransform);
    gRenderBackend.drawDynamicMeshes(bbMeshHandles, debugMeshTransforms, m_debugGeoPass);
}

void RenderFrontend::copyColorToHistoryBuffer() const {
    ImageResource lastFrameResource(m_historyBuffer, 0, 0);
    ImageResource colorBufferResource(m_colorBuffer, 0, 1);
    SamplerResource samplerResource(m_defaultTexelSampler, 2);

    RenderPassExecution copyNextFrameExecution;
    copyNextFrameExecution.handle = m_imageCopyHDRPass;
    copyNextFrameExecution.resources.storageImages = { lastFrameResource };
    copyNextFrameExecution.resources.sampledImages = { colorBufferResource };
    copyNextFrameExecution.resources.samplers = { samplerResource };
    copyNextFrameExecution.dispatchCount[0] = (uint32_t)std::ceil(m_screenWidth / 8.f);
    copyNextFrameExecution.dispatchCount[1] = (uint32_t)std::ceil(m_screenHeight / 8.f);
    copyNextFrameExecution.dispatchCount[2] = 1;
    copyNextFrameExecution.parents = { m_taaPass };

    gRenderBackend.setRenderPassExecution(copyNextFrameExecution);
}

bool RenderFrontend::loadImageFromPath(std::filesystem::path path, ImageHandle* outImageHandle) {

    if (path == "") {
        return false;
    }

    if (m_textureMap.find(path) == m_textureMap.end()) {
        ImageDescription image;
        if (loadImage(path, true, &image)) {
            *outImageHandle = gRenderBackend.createImage(image);
            m_textureMap[path] = *outImageHandle;
            return true;
        }
        else {
            return false;
        }
    }
    else {
        *outImageHandle = m_textureMap[path];
        return true;
    }
}

void RenderFrontend::skyCubemapFromTexture() {
    //write to sky texture
    {
        const auto skyTextureResource = ImageResource(m_skyTexture, 0, 0);
        const auto hdrCaptureResource = ImageResource(m_environmentMapSrc, 0, 1);
        const auto hdrSamplerResource = SamplerResource(m_cubeSampler, 2);

        RenderPassExecution cubeWriteExecution;
        cubeWriteExecution.handle = m_toCubemapPass;
        cubeWriteExecution.resources.storageImages = { skyTextureResource };
        cubeWriteExecution.resources.sampledImages = { hdrCaptureResource };
        cubeWriteExecution.resources.samplers = { hdrSamplerResource };
        cubeWriteExecution.dispatchCount[0] = skyTextureRes / 8;
        cubeWriteExecution.dispatchCount[1] = skyTextureRes / 8;
        cubeWriteExecution.dispatchCount[2] = 6;
        gRenderBackend.setRenderPassExecution(cubeWriteExecution);
    }
    //mips
    for (uint32_t i = 1; i < skyTextureMipCount; i++) {
        const uint32_t srcMip = i - 1;
        const auto skyMipSrcResource = ImageResource(m_skyTexture, srcMip, 0);
        const auto skyMipDstResource = ImageResource(m_skyTexture, i, 1);

        RenderPassExecution skyMipExecution;
        skyMipExecution.handle = m_cubemapMipPasses[srcMip];
        skyMipExecution.resources.storageImages = { skyMipSrcResource, skyMipDstResource };
        if (srcMip == 0) {
            skyMipExecution.parents = { m_toCubemapPass };
        }
        else {
            skyMipExecution.parents = { m_cubemapMipPasses[srcMip - 1] };
        }
        skyMipExecution.dispatchCount[0] = skyTextureRes / 8 / (uint32_t)glm::pow(2, i);
        skyMipExecution.dispatchCount[1] = skyTextureRes / 8 / (uint32_t)glm::pow(2, i);
        skyMipExecution.dispatchCount[2] = 6;
        gRenderBackend.setRenderPassExecution(skyMipExecution);
    }
}

void RenderFrontend::skyCubemapIBLPreProcessing(const std::vector<RenderPassHandle>& dependencies) {
    //diffuse convolution
    {
        const auto diffuseProbeResource = ImageResource(m_diffuseProbe, 0, 0);
        const auto diffuseConvolutionSrcResource = ImageResource(m_skyTexture, 0, 1);
        const auto cubeSamplerResource = SamplerResource(m_skySamplerWithMips, 2);

        RenderPassExecution diffuseConvolutionExecution;
        diffuseConvolutionExecution.handle = m_diffuseConvolutionPass;
        diffuseConvolutionExecution.parents = dependencies;
        diffuseConvolutionExecution.resources.storageImages = { diffuseProbeResource };
        diffuseConvolutionExecution.resources.sampledImages = { diffuseConvolutionSrcResource };
        diffuseConvolutionExecution.resources.samplers = { cubeSamplerResource };
        diffuseConvolutionExecution.dispatchCount[0] = diffuseProbeRes / 8;
        diffuseConvolutionExecution.dispatchCount[1] = diffuseProbeRes / 8;
        diffuseConvolutionExecution.dispatchCount[2] = 6;
        gRenderBackend.setRenderPassExecution(diffuseConvolutionExecution);
    }
    //specular probe convolution
    for (uint32_t mipLevel = 0; mipLevel < m_specularProbeMipCount; mipLevel++) {

        const auto specularProbeResource = ImageResource(m_specularProbe, mipLevel, 0);
        const auto specularConvolutionSrcResource = ImageResource(m_skyTexture, 0, 1);
        const auto specCubeSamplerResource = SamplerResource(m_skySamplerWithMips, 2);

        RenderPassExecution specularConvolutionExecution;
        specularConvolutionExecution.handle = m_specularConvolutionPerMipPasses[mipLevel];
        specularConvolutionExecution.parents = dependencies;
        specularConvolutionExecution.resources.storageImages = { specularProbeResource };
        specularConvolutionExecution.resources.sampledImages = { specularConvolutionSrcResource };
        specularConvolutionExecution.resources.samplers = { specCubeSamplerResource };
        specularConvolutionExecution.dispatchCount[0] = specularProbeRes / uint32_t(pow(2, mipLevel)) / 8;
        specularConvolutionExecution.dispatchCount[1] = specularProbeRes / uint32_t(pow(2, mipLevel)) / 8;
        specularConvolutionExecution.dispatchCount[2] = 6;
        gRenderBackend.setRenderPassExecution(specularConvolutionExecution);
    }
}

void RenderFrontend::computeBRDFLut() {

    const auto brdfLutStorageResource = ImageResource(m_brdfLut, 0, 0);

    RenderPassExecution brdfLutExecution;
    brdfLutExecution.handle = m_brdfLutPass;
    brdfLutExecution.resources.storageImages = { brdfLutStorageResource };
    brdfLutExecution.dispatchCount[0] = brdfLutRes / 8;
    brdfLutExecution.dispatchCount[1] = brdfLutRes / 8;
    brdfLutExecution.dispatchCount[2] = 1;
    gRenderBackend.setRenderPassExecution(brdfLutExecution);
}

void RenderFrontend::bakeSkyOcclusion() {
    
    std::vector<AxisAlignedBoundingBox> meshBoundingBoxes;
    for (const auto& mesh : m_staticMeshes) {
        meshBoundingBoxes.push_back(mesh.bbWorldSpace);
    }

    SkyOcclusionRenderData occlusionData;
    auto sceneBB = combineAxisAlignedBoundingBoxes(meshBoundingBoxes);

    const float bbBias = 1.f;
    sceneBB.max += bbBias;
    sceneBB.min -= bbBias;

    occlusionData.offset = glm::vec4((sceneBB.max + sceneBB.min) * 0.5f, 0.f);
    occlusionData.extends = glm::vec4((sceneBB.max - sceneBB.min), 0.f);

    occlusionData.weight = 1.f / skyOcclusionSampleCount;

    m_skyOcclusionVolumeRes = glm::ivec3(
        pow(2, int(std::ceil(log2f(occlusionData.extends.x / skyOcclusionTargetDensity)))),
        pow(2, int(std::ceil(log2f(occlusionData.extends.y / skyOcclusionTargetDensity)))),
        pow(2, int(std::ceil(log2f(occlusionData.extends.z / skyOcclusionTargetDensity))))
    );
    m_skyOcclusionVolumeRes = glm::min(m_skyOcclusionVolumeRes, glm::ivec3(skyOcclusionVolumeMaxRes));

    std::cout << "\nSky occlusion resolution:\n"
        << "x-axis: " << std::to_string(m_skyOcclusionVolumeRes.x) << "\n"
        << "y-axis: " << std::to_string(m_skyOcclusionVolumeRes.y) << "\n"
        << "z-axis: " << std::to_string(m_skyOcclusionVolumeRes.z) << "\n";

    const glm::vec3 density = glm::vec3(occlusionData.extends) / glm::vec3(m_skyOcclusionVolumeRes);

    std::cout << "\nSky occlusion density:\n" 
        << "x-axis: " << std::to_string(density.x) << " texel/meter\n"
        << "y-axis: " << std::to_string(density.y) << " texel/meter\n"
        << "z-axis: " << std::to_string(density.z) << " texel/meter\n";

    //create sky shadow volume
    {
        ImageDescription desc;
        desc.width = m_skyOcclusionVolumeRes.x;
        desc.height = m_skyOcclusionVolumeRes.y;
        desc.depth = m_skyOcclusionVolumeRes.z;
        desc.type = ImageType::Type3D;
        desc.format = ImageFormat::RGBA16_sNorm;
        desc.usageFlags = ImageUsageFlags::Storage | ImageUsageFlags::Sampled;
        desc.mipCount = MipCount::One;
        desc.manualMipCount = 1;
        desc.autoCreateMips = false;

        m_skyOcclusionVolume = gRenderBackend.createImage(desc);
    }

    RenderPassExecution skyShadowExecution;
    //configure shadow pass
    {
        skyShadowExecution.handle = m_skyShadowPass;
    }

    RenderPassExecution gatherExecution;
    //configure gather pass
    {
        gatherExecution.handle = m_skyOcclusionGatherPass;
        gatherExecution.parents = { m_skyShadowPass };

        const uint32_t threadgroupSize = 4;
        const glm::ivec3 dispatchCount = glm::ivec3(glm::ceil(glm::vec3(m_skyOcclusionVolumeRes) / float(threadgroupSize)));

        gatherExecution.dispatchCount[0] = dispatchCount.x;
        gatherExecution.dispatchCount[1] = dispatchCount.y;
        gatherExecution.dispatchCount[2] = dispatchCount.z;

        const ImageResource occlusionVolume(m_skyOcclusionVolume, 0, 0);
        const ImageResource skyShadowMap(m_skyShadowMap, 0, 1);
        const SamplerResource shadowSamplerResource(m_shadowSampler, 2);
        const UniformBufferResource skyShadowInfo(m_skyOcclusionDataBuffer, 3);

        gatherExecution.resources.storageImages = { occlusionVolume };
        gatherExecution.resources.sampledImages = { skyShadowMap };
        gatherExecution.resources.samplers = { shadowSamplerResource };
        gatherExecution.resources.uniformBuffers = { skyShadowInfo };
    }

    for (int i = 0; i < skyOcclusionSampleCount; i++) {
        //compute sample
        {
            glm::vec2 sample = hammersley2D(i);

            //using uniform distributed samples
            //AO should use cosine weighing with respect to normal
            //however the volume is used by surfaces with arbitrary normals, so use uniform distribution instead
            //reference: http://holger.dammertz.org/stuff/notes_HammersleyOnHemisphere.html
            float cosTheta = 1.f - sample.x;
            float sinTheta = sqrt(1 - cosTheta * cosTheta);
            float phi = 2.f * 3.1415f * sample.y;
            occlusionData.sampleDirection = glm::vec4(cos(phi) * sinTheta, cosTheta, sin(phi) * sinTheta, 0.f);
        }
        //compute shadow matrix
        occlusionData.shadowMatrix = viewProjectionMatrixAroundBB(sceneBB, glm::vec3(occlusionData.sampleDirection));

        gRenderBackend.newFrame();

        //sky shadow pass mesh commands
        {
            std::vector<MeshHandle> meshHandles;
            std::vector<std::array<glm::mat4, 2>> transforms;

            for (const StaticMesh& mesh : m_staticMeshes) {

                const std::array<glm::mat4, 2> t = {
                    occlusionData.shadowMatrix * mesh.modelMatrix,
                    glm::mat4(1.f) }; //unused

                meshHandles.push_back(mesh.backendHandle);
                transforms.push_back(t);
            }
            gRenderBackend.drawMeshes(meshHandles, transforms, m_skyShadowPass);
        }

        gRenderBackend.setUniformBufferData(m_skyOcclusionDataBuffer, &occlusionData, sizeof(SkyOcclusionRenderData));
        gRenderBackend.setRenderPassExecution(skyShadowExecution);
        gRenderBackend.setRenderPassExecution(gatherExecution);
        gRenderBackend.renderFrame(false);
    }
}

void RenderFrontend::updateCameraFrustum() {
    m_cameraFrustum = computeViewFrustum(m_camera);

    //debug geo
    std::vector<glm::vec3> frustumPoints;
    std::vector<uint32_t> frustumIndices;

    frustumToLineMesh(m_cameraFrustum, &frustumPoints, &frustumIndices);
    gRenderBackend.updateDynamicMeshes({ m_cameraFrustumModel }, { frustumPoints }, { frustumIndices });
}

void RenderFrontend::updateShadowFrustum() {
    m_sunShadowFrustum = computeOrthogonalFrustumFittedToCamera(m_cameraFrustum, directionToVector(m_sunDirection));

    //debug geo
    std::vector<glm::vec3> frustumPoints;
    std::vector<uint32_t> frustumIndices;
    frustumToLineMesh(m_sunShadowFrustum, &frustumPoints, &frustumIndices);
    gRenderBackend.updateDynamicMeshes({ m_shadowFrustumModel }, { frustumPoints }, { frustumIndices });
}

HistogramSettings RenderFrontend::createHistogramSettings() {
    HistogramSettings settings;

    settings.minValue = 0.001f;
    settings.maxValue = 200000.f;

    uint32_t pixelsPerTile = histogramTileSizeX * histogramTileSizeX;
    settings.maxTileCount = 1920 * 1080 / pixelsPerTile; //FIXME: update buffer on rescale

    return settings;
}

GraphicPassShaderDescriptions RenderFrontend::createForwardPassShaderDescription(const ShadingConfig& config) {

    GraphicPassShaderDescriptions shaderDesc;
    shaderDesc.vertex.srcPathRelative = "triangle.vert";
    shaderDesc.fragment.srcPathRelative = "triangle.frag";

    //specialisation constants
    {
        auto& constants = shaderDesc.fragment.specialisationConstants;

        //diffuse BRDF
        constants.push_back({
            0,                                                                      //location
            dataToCharArray((void*)&config.diffuseBRDF, sizeof(config.diffuseBRDF)) //value
            });
        //direct specular multiscattering
        constants.push_back({
            1,                                                                                      //location
            dataToCharArray((void*)&config.directMultiscatter, sizeof(config.directMultiscatter))   //value
            });
        //use indirect multiscattering
        constants.push_back({
            2,                                                                                              //location
            dataToCharArray((void*)&config.useIndirectMultiscatter, sizeof(config.useIndirectMultiscatter)) //value
            });
        //use geometry AA
        constants.push_back({
            3,                                                                          //location
            dataToCharArray((void*)&config.useGeometryAA, sizeof(config.useGeometryAA)) //value
            });
        //specular probe mip count
        constants.push_back({
            4,                                                                                  //location
            dataToCharArray((void*)&m_specularProbeMipCount, sizeof(m_specularProbeMipCount))   //value
            });
        //texture LoD bias
        constants.push_back({
            5,                                                                                          //location
            dataToCharArray((void*)&m_taaSettings.textureLoDBias, sizeof(m_taaSettings.textureLoDBias)) //value
            });
        //sky occlusion
        constants.push_back({
            6,                                                                              //location
            dataToCharArray((void*)&config.useSkyOcclusion, sizeof(config.useSkyOcclusion)) //value
            });
        //sky occlusion direction
        constants.push_back({
            7,                                                                                                  //location
            dataToCharArray((void*)&config.useSkyOcclusionDirection, sizeof(config.useSkyOcclusionDirection))   //value
            });
    }

    return shaderDesc;
}

ShaderDescription RenderFrontend::createBRDFLutShaderDescription(const ShadingConfig& config) {

    ShaderDescription desc;
    desc.srcPathRelative = "brdfLut.comp";

    //diffuse brdf specialisation constant
    desc.specialisationConstants.push_back({
        0,                                                                      //location
        dataToCharArray((void*)&config.diffuseBRDF, sizeof(config.diffuseBRDF)) //value
        });
    
    return desc;
}

ShaderDescription RenderFrontend::createTAAShaderDescription() {
    ShaderDescription desc;
    desc.srcPathRelative = "taa.comp";
    
    //specialisation constants
    {
        //use clipping
        desc.specialisationConstants.push_back({
            0,                                                                              //location
            dataToCharArray(&m_taaSettings.useClipping, sizeof(m_taaSettings.useClipping))  //value
            });
        //use variance clipping
        desc.specialisationConstants.push_back({
            1,                                                                                              //location
            dataToCharArray(&m_taaSettings.useVarianceClipping, sizeof(m_taaSettings.useVarianceClipping))  //value
            });
        //use YCoCg color space
        desc.specialisationConstants.push_back({
            2,                                                                          //location
            dataToCharArray(&m_taaSettings.useYCoCg, sizeof(m_taaSettings.useYCoCg))    //value
            });
        //use use motion vector dilation
        desc.specialisationConstants.push_back({
            3,                                                                                                      //location
            dataToCharArray(&m_taaSettings.useMotionVectorDilation, sizeof(m_taaSettings.useMotionVectorDilation))  //value
            });
    }

    return desc;
}

void RenderFrontend::updateGlobalShaderInfo() {
    m_globalShaderInfo.sunDirection = glm::vec4(directionToVector(m_sunDirection), 0.f);
    m_globalShaderInfo.cameraPos = glm::vec4(m_camera.extrinsic.position, 1.f);

    m_globalShaderInfo.deltaTime = Timer::getDeltaTimeFloat();
    m_globalShaderInfo.nearPlane = m_camera.intrinsic.near;
    m_globalShaderInfo.farPlane = m_camera.intrinsic.far;

    m_globalShaderInfo.cameraRight      = glm::vec4(m_camera.extrinsic.right, 0);
    m_globalShaderInfo.cameraUp         = glm::vec4(m_camera.extrinsic.up, 0);
    m_globalShaderInfo.cameraForward    = glm::vec4(m_camera.extrinsic.forward, 0);
    m_globalShaderInfo.cameraTanFovHalf = glm::tan(glm::radians(m_camera.intrinsic.fov) * 0.5f);
    m_globalShaderInfo.cameraAspectRatio = m_camera.intrinsic.aspectRatio;

    gRenderBackend.setGlobalShaderInfo(m_globalShaderInfo);
}

void RenderFrontend::initImages() {
    //load skybox
    {
        ImageDescription hdrCapture;
        if (loadImage("textures\\sunset_in_the_chalk_quarry_2k.hdr", false, &hdrCapture)) {
            m_environmentMapSrc = gRenderBackend.createImage(hdrCapture);
        }
        else {
            m_environmentMapSrc = m_defaultTextures.sky;
        }
    }
    //main color buffer
    {
        ImageDescription desc;
        desc.initialData = std::vector<uint8_t>{};
        desc.width = m_screenWidth;
        desc.height = m_screenHeight;
        desc.depth = 1;
        desc.type = ImageType::Type2D;
        desc.format = ImageFormat::R11G11B10_uFloat;
        desc.usageFlags = ImageUsageFlags::Attachment | ImageUsageFlags::Sampled | ImageUsageFlags::Storage;
        desc.mipCount = MipCount::One;
        desc.manualMipCount = 0;
        desc.autoCreateMips = false;

        m_colorBuffer = gRenderBackend.createImage(desc);
    }
    //depth buffer
    {
        ImageDescription desc;
        desc.initialData = std::vector<uint8_t>{};
        desc.width = m_screenWidth;
        desc.height = m_screenHeight;
        desc.depth = 1;
        desc.type = ImageType::Type2D;
        desc.format = ImageFormat::Depth32;
        desc.usageFlags = ImageUsageFlags::Attachment | ImageUsageFlags::Sampled;
        desc.mipCount = MipCount::One;
        desc.manualMipCount = 0;
        desc.autoCreateMips = false;

        m_depthBuffer = gRenderBackend.createImage(desc);
    }
    //motion vector buffer
    {
        ImageDescription desc;
        desc.width = m_screenWidth;
        desc.height = m_screenHeight;
        desc.depth = 1;
        desc.format = ImageFormat::RG16_sFloat;
        desc.autoCreateMips = false;
        desc.manualMipCount = 1;
        desc.mipCount = MipCount::One;
        desc.type = ImageType::Type2D;
        desc.usageFlags = ImageUsageFlags::Attachment | ImageUsageFlags::Sampled;

        m_motionVectorBuffer = gRenderBackend.createImage(desc);
    }
    //history buffer for TAA
    {
        ImageDescription desc;
        desc.width = m_screenWidth;
        desc.height = m_screenHeight;
        desc.depth = 1;
        desc.type = ImageType::Type2D;
        desc.format = ImageFormat::R11G11B10_uFloat;
        desc.usageFlags = ImageUsageFlags::Storage | ImageUsageFlags::Sampled;
        desc.mipCount = MipCount::One;
        desc.manualMipCount = 1;
        desc.autoCreateMips = false;

        m_historyBuffer = gRenderBackend.createImage(desc);
    }
    //shadow map cascades
    {
        ImageDescription desc;
        desc.width = shadowMapRes;
        desc.height = shadowMapRes;
        desc.depth = 1;
        desc.type = ImageType::Type2D;
        desc.format = ImageFormat::Depth16;
        desc.usageFlags = ImageUsageFlags::Attachment | ImageUsageFlags::Sampled;
        desc.mipCount = MipCount::One;
        desc.manualMipCount = 1;
        desc.autoCreateMips = false;

        m_shadowMaps.reserve(shadowCascadeCount);
        for (uint32_t i = 0; i < shadowCascadeCount; i++) {
            const auto shadowMap = gRenderBackend.createImage(desc);
            m_shadowMaps.push_back(shadowMap);
        }
    }
    //specular probe
    {
        m_specularProbeMipCount = mipCountFromResolution(specularProbeRes, specularProbeRes, 1);

        ImageDescription desc;
        desc.width = specularProbeRes;
        desc.height = specularProbeRes;
        desc.depth = 1;
        desc.type = ImageType::TypeCube;
        desc.format = ImageFormat::R11G11B10_uFloat;
        desc.usageFlags = ImageUsageFlags::Sampled | ImageUsageFlags::Storage;
        desc.mipCount = MipCount::Manual;
        desc.manualMipCount = m_specularProbeMipCount;
        desc.autoCreateMips = false;

        m_specularProbe = gRenderBackend.createImage(desc);
    }
    //diffuse probe
    {
        ImageDescription desc;
        desc.width = diffuseProbeRes;
        desc.height = diffuseProbeRes;
        desc.depth = 1;
        desc.type = ImageType::TypeCube;
        desc.format = ImageFormat::R11G11B10_uFloat;
        desc.usageFlags = ImageUsageFlags::Sampled | ImageUsageFlags::Storage;
        desc.mipCount = MipCount::One;
        desc.manualMipCount = 0;
        desc.autoCreateMips = false;

        m_diffuseProbe = gRenderBackend.createImage(desc);
    }
    //sky cubemap
    {
        ImageDescription desc;
        desc.width = skyTextureRes;
        desc.height = skyTextureRes;
        desc.depth = 1;
        desc.type = ImageType::TypeCube;
        desc.format = ImageFormat::R11G11B10_uFloat;
        desc.usageFlags = ImageUsageFlags::Sampled | ImageUsageFlags::Storage;
        desc.mipCount = MipCount::Manual;
        desc.manualMipCount = 8;
        desc.autoCreateMips = false;

        m_skyTexture = gRenderBackend.createImage(desc);
    }
    //brdf LUT
    {
        ImageDescription desc;
        desc.width = brdfLutRes;
        desc.height = brdfLutRes;
        desc.depth = 1;
        desc.type = ImageType::Type2D;
        desc.format = ImageFormat::RGBA16_sFloat;
        desc.usageFlags = ImageUsageFlags::Sampled | ImageUsageFlags::Storage;
        desc.mipCount = MipCount::One;
        desc.manualMipCount = 1;
        desc.autoCreateMips = false;

        m_brdfLut = gRenderBackend.createImage(desc);
    }
    //min/max depth pyramid
    {
        ImageDescription desc;
        desc.autoCreateMips = false;
        desc.width = m_screenWidth / 2;
        desc.height = m_screenHeight / 2;
        desc.depth = 1;
        desc.mipCount = MipCount::FullChain;
        desc.type = ImageType::Type2D;
        desc.format = ImageFormat::RG32_sFloat;
        desc.usageFlags = ImageUsageFlags::Sampled | ImageUsageFlags::Storage;

        m_minMaxDepthPyramid = gRenderBackend.createImage(desc);
    }
    //sky shadow map
    {
        ImageDescription desc;
        desc.width = skyShadowMapRes;
        desc.height = skyShadowMapRes;
        desc.depth = 1;
        desc.type = ImageType::Type2D;
        desc.format = ImageFormat::Depth16;
        desc.usageFlags = ImageUsageFlags::Attachment | ImageUsageFlags::Sampled;
        desc.mipCount = MipCount::One;
        desc.manualMipCount = 1;
        desc.autoCreateMips = false;

        m_skyShadowMap = gRenderBackend.createImage(desc);
    }
    //sky occlusion volume is created later
    //its resolution is dependent on scene size in order to fit desired texel density
}

void RenderFrontend::initSamplers(){
    //shadow sampler
    {
        SamplerDescription desc;
        desc.interpolation = SamplerInterpolation::Nearest;
        desc.wrapping = SamplerWrapping::Color;
        desc.useAnisotropy = false;
        desc.maxAnisotropy = 0;
        desc.borderColor = SamplerBorderColor::White;
        desc.maxMip = 0;

        m_shadowSampler = gRenderBackend.createSampler(desc);
    }
    //cube map sampler
    {
        SamplerDescription desc;
        desc.interpolation = SamplerInterpolation::Linear;
        desc.wrapping = SamplerWrapping::Clamp;
        desc.useAnisotropy = false;
        desc.maxAnisotropy = 0;
        desc.borderColor = SamplerBorderColor::White;
        desc.maxMip = 0;

        m_cubeSampler = gRenderBackend.createSampler(desc);
    }
    //lut sampler
    {
        SamplerDescription desc;
        desc.interpolation = SamplerInterpolation::Linear;
        desc.wrapping = SamplerWrapping::Clamp;
        desc.useAnisotropy = false;
        desc.maxAnisotropy = 0;
        desc.borderColor = SamplerBorderColor::White;
        desc.maxMip = 0;

        m_lutSampler = gRenderBackend.createSampler(desc);
    }
    //color sampler
    {
        SamplerDescription desc;
        desc.interpolation = SamplerInterpolation::Linear;
        desc.wrapping = SamplerWrapping::Clamp;
        desc.useAnisotropy = false;
        desc.maxAnisotropy = 0;
        desc.borderColor = SamplerBorderColor::White;
        desc.maxMip = 0;

        m_colorSampler = gRenderBackend.createSampler(desc);
    }
    //hdri sampler
    {
        SamplerDescription desc;
        desc.interpolation = SamplerInterpolation::Linear;
        desc.wrapping = SamplerWrapping::Clamp;
        desc.useAnisotropy = false;
        desc.maxAnisotropy = 0;
        desc.borderColor = SamplerBorderColor::Black;
        desc.maxMip = 0;

        m_hdriSampler = gRenderBackend.createSampler(desc);
    }
    //cubemap sampler
    {
        SamplerDescription desc;
        desc.interpolation = SamplerInterpolation::Linear;
        desc.wrapping = SamplerWrapping::Clamp;
        desc.useAnisotropy = false;
        desc.maxAnisotropy = 0;
        desc.borderColor = SamplerBorderColor::Black;
        desc.maxMip = 0;

        m_cubeSampler = gRenderBackend.createSampler(desc);
    }
    //sky sampler
    {
        SamplerDescription desc;
        desc.interpolation = SamplerInterpolation::Linear;
        desc.wrapping = SamplerWrapping::Clamp;
        desc.useAnisotropy = false;
        desc.maxAnisotropy = 0;
        desc.borderColor = SamplerBorderColor::Black;
        desc.maxMip = skyTextureMipCount;

        m_skySamplerWithMips = gRenderBackend.createSampler(desc);
    }
    //texel sampler
    {
        SamplerDescription desc;
        desc.interpolation = SamplerInterpolation::Nearest;
        desc.wrapping = SamplerWrapping::Clamp;
        desc.useAnisotropy = false;
        desc.maxAnisotropy = 0;
        desc.borderColor = SamplerBorderColor::Black;
        desc.maxMip = 0;

        m_defaultTexelSampler = gRenderBackend.createSampler(desc);
    }
    //depth sampler
    {
        SamplerDescription desc;
        desc.interpolation = SamplerInterpolation::Nearest;
        desc.maxMip = 11;
        desc.useAnisotropy = false;
        desc.wrapping = SamplerWrapping::Clamp;

        m_clampedDepthSampler = gRenderBackend.createSampler(desc);
    }
    //sky occlusion sampler
    {
        SamplerDescription desc;
        desc.interpolation = SamplerInterpolation::Linear;
        desc.maxMip = 0;
        desc.useAnisotropy = false;
        desc.wrapping = SamplerWrapping::Color;
        desc.borderColor = SamplerBorderColor::White;

        m_skyOcclusionSampler = gRenderBackend.createSampler(desc);
    }
}

void RenderFrontend::initBuffers(const HistogramSettings& histogramSettings) {
    //histogram buffer
    {
        StorageBufferDescription histogramBufferDesc;
        histogramBufferDesc.size = nHistogramBins * sizeof(uint32_t);
        m_histogramBuffer = gRenderBackend.createStorageBuffer(histogramBufferDesc);
    }
    //light buffer 
    {
        float initialLightBufferData[3] = { 0.f, 0.f, 0.f };
        StorageBufferDescription lightBufferDesc;
        lightBufferDesc.size = 3 * sizeof(uint32_t);
        lightBufferDesc.initialData = initialLightBufferData;
        m_lightBuffer = gRenderBackend.createStorageBuffer(lightBufferDesc);
    }
    //per tile histogram
    {
        StorageBufferDescription histogramPerTileBufferDesc;
        histogramPerTileBufferDesc.size = (size_t)histogramSettings.maxTileCount * nHistogramBins * sizeof(uint32_t);
        m_histogramPerTileBuffer = gRenderBackend.createStorageBuffer(histogramPerTileBufferDesc);
    }
    //depth pyramid syncing buffer
    {
        StorageBufferDescription desc;
        desc.size = sizeof(uint32_t);
        desc.initialData = { (uint32_t)0 };
        m_depthPyramidSyncBuffer = gRenderBackend.createStorageBuffer(desc);
    }
    //light matrix buffer
    {
        StorageBufferDescription desc;
        const size_t splitSize = sizeof(glm::vec4);
        const size_t lightMatrixSize = sizeof(glm::mat4) * shadowCascadeCount;
        desc.size = splitSize + lightMatrixSize;
        m_sunShadowInfoBuffer = gRenderBackend.createStorageBuffer(desc);
    }
    //sky shadow info buffer
    {
        UniformBufferDescription desc;
        desc.size = sizeof(SkyOcclusionRenderData);
        m_skyOcclusionDataBuffer = gRenderBackend.createUniformBuffer(desc);
    }
}

void RenderFrontend::initMeshs() {
    //dynamic meshes for frustum debugging
    {
        m_cameraFrustumModel = gRenderBackend.createDynamicMeshes(
            { positionsInViewFrustumLineMesh }, { indicesInViewFrustumLineMesh }).front();

        m_shadowFrustumModel = gRenderBackend.createDynamicMeshes(
            { positionsInViewFrustumLineMesh }, { indicesInViewFrustumLineMesh }).front();
    }
    //skybox cube
    {
        MeshData cubeData;
        cubeData.positions = {
            glm::vec3(-1.f, -1.f, -1.f),
            glm::vec3(1.f, -1.f, -1.f),
            glm::vec3(1.f, 1.f, -1.f),
            glm::vec3(-1.f, 1.f, -1.f),
            glm::vec3(-1.f, -1.f, 1.f),
            glm::vec3(1.f, -1.f, 1.f),
            glm::vec3(1.f, 1.f, 1.f),
            glm::vec3(-1.f, 1.f, 1.f)
        };
        cubeData.uvs = {
            glm::vec2(),
            glm::vec2(),
            glm::vec2(),
            glm::vec2(),
            glm::vec2(),
            glm::vec2(),
            glm::vec2(),
            glm::vec2()
        };
        cubeData.normals = {
            glm::vec3(),
            glm::vec3(),
            glm::vec3(),
            glm::vec3(),
            glm::vec3(),
            glm::vec3(),
            glm::vec3(),
            glm::vec3()
        };
        cubeData.tangents = {
            glm::vec3(),
            glm::vec3(),
            glm::vec3(),
            glm::vec3(),
            glm::vec3(),
            glm::vec3(),
            glm::vec3(),
            glm::vec3()
        };
        cubeData.bitangents = {
            glm::vec3(),
            glm::vec3(),
            glm::vec3(),
            glm::vec3(),
            glm::vec3(),
            glm::vec3(),
            glm::vec3(),
            glm::vec3()
        };
        cubeData.indices = {
            0, 1, 3, 3, 1, 2,
            1, 5, 2, 2, 5, 6,
            5, 4, 6, 6, 4, 7,
            4, 0, 7, 7, 0, 3,
            3, 2, 7, 7, 2, 6,
            4, 5, 0, 0, 5, 1
        };
        const std::vector<MeshBinary> cubeBinary = meshesToBinary(std::vector<MeshData>{cubeData});

        Material cubeMaterial;
        cubeMaterial.diffuseTexture = m_defaultTextures.diffuse;
        cubeMaterial.normalTexture = m_defaultTextures.normal;
        cubeMaterial.specularTexture = m_defaultTextures.specular;

        m_skyCube = gRenderBackend.createMeshes(cubeBinary, std::vector<Material> {cubeMaterial}).back();
    }
}

void RenderFrontend::initRenderpasses(const HistogramSettings& histogramSettings) {
    //main shading pass
    {
        const auto colorAttachment = Attachment(
            m_colorBuffer,
            0,
            0,
            AttachmentLoadOp::Clear);

        const auto depthAttachment = Attachment(
            m_depthBuffer,
            0,
            0,
            AttachmentLoadOp::Load);

        GraphicPassDescription mainPassDesc;
        mainPassDesc.name = "Forward shading";
        mainPassDesc.shaderDescriptions = createForwardPassShaderDescription(m_shadingConfig);
        mainPassDesc.attachments = { colorAttachment, depthAttachment };
        mainPassDesc.depthTest.function = DepthFunction::Equal;
        mainPassDesc.depthTest.write = true;
        mainPassDesc.rasterization.cullMode = CullMode::Back;
        mainPassDesc.rasterization.mode = RasterizationeMode::Fill;
        mainPassDesc.blending = BlendState::None;
        mainPassDesc.vertexFormat = VertexFormat::Full;

        m_mainPass = gRenderBackend.createGraphicPass(mainPassDesc);
    }
    //shadow cascade passes
    for (uint32_t cascade = 0; cascade < shadowCascadeCount; cascade++) {

        const auto shadowMapAttachment = Attachment(
            m_shadowMaps[cascade],
            0,
            0,
            AttachmentLoadOp::Clear);

        GraphicPassDescription shadowPassConfig;
        shadowPassConfig.name = "Shadow map cascade " + std::to_string(cascade);
        shadowPassConfig.attachments = { shadowMapAttachment };
        shadowPassConfig.shaderDescriptions.vertex.srcPathRelative = "sunShadow.vert";
        shadowPassConfig.shaderDescriptions.fragment.srcPathRelative = "sunShadow.frag";
        shadowPassConfig.depthTest.function = DepthFunction::LessEqual;
        shadowPassConfig.depthTest.write = true;
        shadowPassConfig.rasterization.cullMode = CullMode::Front;
        shadowPassConfig.rasterization.mode = RasterizationeMode::Fill;
        shadowPassConfig.rasterization.clampDepth = true;
        shadowPassConfig.blending = BlendState::None;
        shadowPassConfig.vertexFormat = VertexFormat::Full;

        //cascade index specialisation constant
        shadowPassConfig.shaderDescriptions.vertex.specialisationConstants = { {
            0,                                                  //location
            dataToCharArray((void*)&cascade, sizeof(cascade))   //value
            }};
        

        const auto shadowPass = gRenderBackend.createGraphicPass(shadowPassConfig);
        m_shadowPasses.push_back(shadowPass);
    }
    //sky copy pass
    {
        ComputePassDescription cubeWriteDesc;
        cubeWriteDesc.name = "Copy sky to cubemap";
        cubeWriteDesc.shaderDescription.srcPathRelative = "copyToCube.comp";
        m_toCubemapPass = gRenderBackend.createComputePass(cubeWriteDesc);
    }
    //cubemap mip creation pass
    {
        ComputePassDescription cubemapMipPassDesc;
        cubemapMipPassDesc.name = "Sky mip creation";
        cubemapMipPassDesc.shaderDescription.srcPathRelative = "cubemapMip.comp";

        //first map is written to by different shader        
        for (uint32_t i = 0; i < skyTextureMipCount - 1; i++) {
            m_cubemapMipPasses.push_back(gRenderBackend.createComputePass(cubemapMipPassDesc));
        }
    }
    //specular convolution pass
    {
        //don't use the last few mips as they are too small
        const uint32_t mipsTooSmallCount = 4;
        if (m_specularProbeMipCount > mipsTooSmallCount) {
            m_specularProbeMipCount -= mipsTooSmallCount;
        }

        for (uint32_t i = 0; i < m_specularProbeMipCount; i++) {
            ComputePassDescription specularConvolutionDesc;
            specularConvolutionDesc.name = "Specular probe convolution";
            specularConvolutionDesc.shaderDescription.srcPathRelative = "specularCubeConvolution.comp";

            //specialisation constants
            {
                auto& constants = specularConvolutionDesc.shaderDescription.specialisationConstants;

                //mip count specialisation constant
                constants.push_back({
                    0,                                                                                  //location
                    dataToCharArray((void*)&m_specularProbeMipCount, sizeof(m_specularProbeMipCount))   //value
                    });
                //mip level
                constants.push_back({
                    1,                                      //location
                    dataToCharArray((void*)&i, sizeof(i))   //value
                    });
            }
            m_specularConvolutionPerMipPasses.push_back(gRenderBackend.createComputePass(specularConvolutionDesc));
        }
    }
    //diffuse convolution pass
    {
        ComputePassDescription diffuseConvolutionDesc;
        diffuseConvolutionDesc.name = "Diffuse probe convolution";
        diffuseConvolutionDesc.shaderDescription.srcPathRelative = "diffuseCubeConvolution.comp";
        m_diffuseConvolutionPass = gRenderBackend.createComputePass(diffuseConvolutionDesc);
    }
    //sky pass
    {
        const auto colorAttachment = Attachment(m_colorBuffer, 0, 0, AttachmentLoadOp::Load);
        const auto depthAttachment = Attachment(m_depthBuffer, 0, 0, AttachmentLoadOp::Load);

        GraphicPassDescription skyPassConfig;
        skyPassConfig.name = "Skybox render";
        skyPassConfig.attachments = { colorAttachment, depthAttachment };
        skyPassConfig.shaderDescriptions.vertex.srcPathRelative = "sky.vert";
        skyPassConfig.shaderDescriptions.fragment.srcPathRelative = "sky.frag";
        skyPassConfig.depthTest.function = DepthFunction::LessEqual;
        skyPassConfig.depthTest.write = false;
        skyPassConfig.rasterization.cullMode = CullMode::None;
        skyPassConfig.rasterization.mode = RasterizationeMode::Fill;
        skyPassConfig.blending = BlendState::None;
        skyPassConfig.vertexFormat = VertexFormat::Full;

        m_skyPass = gRenderBackend.createGraphicPass(skyPassConfig);
    }
    //BRDF Lut creation pass
    {
        ComputePassDescription brdfLutPassDesc;
        brdfLutPassDesc.name = "BRDF Lut creation";
        brdfLutPassDesc.shaderDescription = createBRDFLutShaderDescription(m_shadingConfig);
        m_brdfLutPass = gRenderBackend.createComputePass(brdfLutPassDesc);
    }
    //geometry debug pass
    {
        const auto colorAttachment = Attachment(m_colorBuffer, 0, 0, AttachmentLoadOp::Load);
        const auto depthAttachment = Attachment(m_depthBuffer, 0, 0, AttachmentLoadOp::Load);

        GraphicPassDescription debugPassConfig;
        debugPassConfig.name = "Debug geometry";
        debugPassConfig.attachments = { colorAttachment, depthAttachment };
        debugPassConfig.shaderDescriptions.vertex.srcPathRelative = "debug.vert";
        debugPassConfig.shaderDescriptions.fragment.srcPathRelative = "debug.frag";
        debugPassConfig.depthTest.function = DepthFunction::LessEqual;
        debugPassConfig.depthTest.write = true;
        debugPassConfig.rasterization.cullMode = CullMode::None;
        debugPassConfig.rasterization.mode = RasterizationeMode::Line;
        debugPassConfig.blending = BlendState::None;
        debugPassConfig.vertexFormat = VertexFormat::PositionOnly;

        m_debugGeoPass = gRenderBackend.createGraphicPass(debugPassConfig);
    }
    //histogram per tile pass
    {
        ComputePassDescription histogramPerTileDesc;
        histogramPerTileDesc.name = "Histogram per tile";
        histogramPerTileDesc.shaderDescription.srcPathRelative = "histogramPerTile.comp";

        const uint32_t maxTilesSpecialisationConstantID = 4;

        //specialisation constants
        {
            auto& constants = histogramPerTileDesc.shaderDescription.specialisationConstants;

            //bin count
            constants.push_back({
                0,                                                                  //location
                dataToCharArray((void*)&nHistogramBins, sizeof(nHistogramBins)) //value
                });
            //min luminance constant
            constants.push_back({
                1,                                                                                      //location
                dataToCharArray((void*)&histogramSettings.minValue, sizeof(histogramSettings.minValue)) //value
                });
            //max luminance constant
            constants.push_back({
                2,                                                                                      //location
                dataToCharArray((void*)&histogramSettings.maxValue, sizeof(histogramSettings.maxValue)) //value
                });
            constants.push_back({
                3,                                                                                              //location
                dataToCharArray((void*)&histogramSettings.maxTileCount, sizeof(histogramSettings.maxTileCount)) //value
                });
        }
        m_histogramPerTilePass = gRenderBackend.createComputePass(histogramPerTileDesc);
    }
    //histogram reset pass
    {
        ComputePassDescription resetDesc;
        resetDesc.name = "Histogram reset";
        resetDesc.shaderDescription.srcPathRelative = "histogramReset.comp";

        //bin count constant
        resetDesc.shaderDescription.specialisationConstants.push_back({
            0,                                                                  //location
            dataToCharArray((void*)&nHistogramBins, sizeof(nHistogramBins)) //value
            });

        m_histogramResetPass = gRenderBackend.createComputePass(resetDesc);
    }
    //histogram combine tiles pass
    {
        const uint32_t maxTilesSpecialisationConstantID = 1;

        ComputePassDescription histogramCombineDesc;
        histogramCombineDesc.name = "Histogram combine tiles";
        histogramCombineDesc.shaderDescription.srcPathRelative = "histogramCombineTiles.comp";

        auto& constants = histogramCombineDesc.shaderDescription.specialisationConstants;

        //bin count
        constants.push_back({
            0,                                                                  //location
            dataToCharArray((void*)&nHistogramBins, sizeof(nHistogramBins)) //value
                });
        //max luminance constant
        constants.push_back({
            1,                                                                                              //location
            dataToCharArray((void*)&histogramSettings.maxTileCount, sizeof(histogramSettings.maxTileCount)) //value
                });

        m_histogramCombinePass = gRenderBackend.createComputePass(histogramCombineDesc);
    }
    //pre-expose lights pass
    {
        ComputePassDescription preExposeLightsDesc;
        preExposeLightsDesc.name = "Pre-expose lights";
        preExposeLightsDesc.shaderDescription.srcPathRelative = "preExposeLights.comp";

        //specialisation constants
        {
            auto& constants = preExposeLightsDesc.shaderDescription.specialisationConstants;

            //bin count
            constants.push_back({
                0,                                                                  //location
                dataToCharArray((void*)&nHistogramBins, sizeof(nHistogramBins)) //value
                });
            //min luminance constant
            constants.push_back({
                1,                                                                                      //location
                dataToCharArray((void*)&histogramSettings.minValue,sizeof(histogramSettings.minValue))  //value
                });
            //max luminance constant
            constants.push_back({
                2,                                                                                      //location
                dataToCharArray((void*)&histogramSettings.maxValue, sizeof(histogramSettings.maxValue)) //value
                });
        }
        m_preExposeLightsPass = gRenderBackend.createComputePass(preExposeLightsDesc);
    }
    //depth prepass
    {
        Attachment depthAttachment(m_depthBuffer, 0, 0, AttachmentLoadOp::Clear);
        Attachment velocityAttachment(m_motionVectorBuffer, 0, 1, AttachmentLoadOp::Clear);

        GraphicPassDescription desc;
        desc.attachments = { depthAttachment, velocityAttachment };
        desc.blending = BlendState::None;
        desc.depthTest.function = DepthFunction::LessEqual;
        desc.depthTest.write = true;
        desc.name = "Depth prepass";
        desc.rasterization.cullMode = CullMode::Back;
        desc.shaderDescriptions.vertex.srcPathRelative = "depthPrepass.vert";
        desc.shaderDescriptions.fragment.srcPathRelative = "depthPrepass.frag";
        desc.vertexFormat = VertexFormat::Full;

        m_depthPrePass = gRenderBackend.createGraphicPass(desc);
    }
    //depth pyramid pass
    {
        ComputePassDescription desc;
        desc.name = "Depth min/max pyramid creation";

        uint32_t threadgroupCount = 0;
        desc.shaderDescription = createDepthPyramidShaderDescription(&threadgroupCount);

        m_depthPyramidPass = gRenderBackend.createComputePass(desc);
    }
    //light matrix pass
    {
        ComputePassDescription desc;
        desc.name = "Compute light matrix";
        desc.shaderDescription.srcPathRelative = "lightMatrix.comp";

        m_lightMatrixPass = gRenderBackend.createComputePass(desc);
    }
    //tonemapping pass
    {
        ComputePassDescription desc;
        desc.name = "Tonemapping";
        desc.shaderDescription.srcPathRelative = "tonemapping.comp";

        m_tonemappingPass = gRenderBackend.createComputePass(desc);
    }
    //image copy pass
    {
        ComputePassDescription desc;
        desc.name = "Image copy";
        desc.shaderDescription.srcPathRelative = "imageCopyHDR.comp";

        m_imageCopyHDRPass = gRenderBackend.createComputePass(desc);
    }
    //TAA pass
    {
        ComputePassDescription desc;
        desc.name = "TAA";
        desc.shaderDescription = createTAAShaderDescription();
        m_taaPass = gRenderBackend.createComputePass(desc);
    }
    //sky shadow pass
    {
        const auto shadowMapAttachment = Attachment(
            m_skyShadowMap,
            0,
            0,
            AttachmentLoadOp::Clear);

        GraphicPassDescription config;
        config.name = "Sky shadow map";
        config.attachments = { shadowMapAttachment };
        config.shaderDescriptions.vertex.srcPathRelative = "depthOnlySimple.vert";
        config.shaderDescriptions.fragment.srcPathRelative = "depthOnlySimple.frag";
        config.depthTest.function = DepthFunction::LessEqual;
        config.depthTest.write = true;
        config.rasterization.cullMode = CullMode::Back;
        config.rasterization.mode = RasterizationeMode::Fill;
        config.rasterization.clampDepth = true;
        config.blending = BlendState::None;
        config.vertexFormat = VertexFormat::Full;

        m_skyShadowPass = gRenderBackend.createGraphicPass(config);
    }
    //sky occlusion pass
    {
        ComputePassDescription desc;
        desc.name = "Sky occlusion gather";
        desc.shaderDescription.srcPathRelative = "skyOcclusionGather.comp";
        m_skyOcclusionGatherPass = gRenderBackend.createComputePass(desc);
    }
}

ShaderDescription RenderFrontend::createDepthPyramidShaderDescription(uint32_t* outThreadgroupCount) {

    ShaderDescription desc;
    desc.srcPathRelative = "depthHiZPyramid.comp";

    const uint32_t depthMipCount = mipCountFromResolution(m_screenWidth / 2, m_screenHeight / 2, 1);
    const auto dispatchCount = computeDepthPyramidDispatchCount();

    //mip count
    desc.specialisationConstants.push_back({ 
        0,                                                              //location
        dataToCharArray((void*)&depthMipCount, sizeof(depthMipCount))   //value
            });
    //depth buffer width
    desc.specialisationConstants.push_back({
        1,                                                              //location
        dataToCharArray((void*)&m_screenWidth, sizeof(m_screenWidth))   //value
            });
    //depth buffer height
    desc.specialisationConstants.push_back({
        2,                                                              //location
        dataToCharArray((void*)&m_screenHeight, sizeof(m_screenHeight)) //value
            });
    //threadgroup count
    *outThreadgroupCount = dispatchCount.x * dispatchCount.y;
    desc.specialisationConstants.push_back({
        3,                                                                          //location
        dataToCharArray((void*)outThreadgroupCount, sizeof(*outThreadgroupCount))   //value
            });

    return desc;
}

glm::ivec2 RenderFrontend::computeDepthPyramidDispatchCount() const{
    glm::ivec2 count;

    //shader can process up to 11 mip levels
    //thread group extent ranges from 16 to 1 depending on how many mips are used
    const uint32_t mipCount = mipCountFromResolution(m_screenWidth / 2, m_screenHeight / 2, 1);
    const uint32_t maxMipCount = 11;
    const uint32_t unusedMips = maxMipCount - mipCount;

    //last 6 mips are processed by single thread group
    if (unusedMips >= 6) {
        return glm::ivec2(1, 1);
    }
    else {
        //group size of 16x16 can compute up to a 32x32 area in mip0
        const uint32_t localThreadGroupExtent = 32 / (uint32_t)pow((uint32_t)2, unusedMips);

        //pyramid mip0 is half screen resolution
        count.x = (uint32_t)std::ceil(m_screenWidth  * 0.5f / localThreadGroupExtent);
        count.y = (uint32_t)std::ceil(m_screenHeight * 0.5f / localThreadGroupExtent);

        return count;
    }
}

void RenderFrontend::drawUi() {
    //rendering stats
    {
        ImGui::Begin("Rendering stats");
        ImGui::Text(("DeltaTime: " + std::to_string(m_globalShaderInfo.deltaTime * 1000) + "ms").c_str());
        ImGui::Text(("Mesh count: " + std::to_string(m_currentMeshCount)).c_str());
        ImGui::Text(("Main pass drawcalls: " + std::to_string(m_currentMainPassDrawcallCount)).c_str());
        ImGui::Text(("Shadow map drawcalls: " + std::to_string(m_currentShadowPassDrawcallCount)).c_str());

        uint64_t allocatedMemorySizeByte;
        uint64_t usedMemorySizeByte;
        gRenderBackend.getMemoryStats(&allocatedMemorySizeByte, &usedMemorySizeByte);

        const float byteToMbDivider = 1048576;
        const float allocatedMemorySizeMegaByte = allocatedMemorySizeByte / byteToMbDivider;
        const float usedMemorySizeMegaByte = usedMemorySizeByte / byteToMbDivider;

        ImGui::Text(("Allocated memory: " + std::to_string(allocatedMemorySizeMegaByte) + "mb").c_str());
        ImGui::Text(("Used memory: " + std::to_string(usedMemorySizeMegaByte) + "mb").c_str());
    }

    //pass timings shown in columns
    {
        m_renderTimingTimeSinceLastUpdate += m_globalShaderInfo.deltaTime;
        if (m_renderTimingTimeSinceLastUpdate > m_renderTimingUpdateFrequency) {
            m_currentRenderTimings = gRenderBackend.getRenderpassTimings();
            m_renderTimingTimeSinceLastUpdate = 0.f;
        }
        
        ImGui::Separator();
        ImGui::Columns(2);
        for (const auto timing : m_currentRenderTimings) {
            ImGui::Text(timing.name.c_str());
        }
        ImGui::NextColumn();
        for (const auto timing : m_currentRenderTimings) {
            //limit number of decimal places to improve readability
            const size_t commaIndex = std::max(int(timing.timeMs) / 10, 1);
            const size_t decimalPlacesToKeep = 2;
            auto timeString = std::to_string(timing.timeMs);
            timeString = timeString.substr(0, commaIndex + 1 + decimalPlacesToKeep);
            ImGui::Text(timeString.c_str());
        }
    }
    ImGui::End();

    ImGui::Begin("Rendering");
    //TAA Settings
    if(ImGui::CollapsingHeader("TAA settings")){
        
        m_isTAAShaderDescriptionStale |= ImGui::Checkbox("Clipping", &m_taaSettings.useClipping);
        m_isTAAShaderDescriptionStale |= ImGui::Checkbox("Variance clipping", &m_taaSettings.useVarianceClipping);
        m_isTAAShaderDescriptionStale |= ImGui::Checkbox("YCoCg color space clipping", &m_taaSettings.useYCoCg);
        m_isTAAShaderDescriptionStale |= ImGui::Checkbox("Dilate motion vector", &m_taaSettings.useMotionVectorDilation);

        m_isMainPassShaderDescriptionStale |= ImGui::InputFloat("Texture LoD bias", &m_taaSettings.textureLoDBias);
    }

    //lighting settings
    if(ImGui::CollapsingHeader("Lighting settings")){
        ImGui::DragFloat2("Sun direction", &m_sunDirection.x);
        ImGui::ColorEdit4("Sun color", &m_globalShaderInfo.sunColor.x);
        ImGui::DragFloat("Exposure offset EV", &m_globalShaderInfo.exposureOffset, 0.1f);
        ImGui::DragFloat("Adaption speed EV/s", &m_globalShaderInfo.exposureAdaptionSpeedEvPerSec, 0.1f, 0.f);
        ImGui::InputFloat("Sun Illuminance Lux", &m_globalShaderInfo.sunIlluminanceLux);
        ImGui::InputFloat("Sky Illuminance Lux", &m_globalShaderInfo.skyIlluminanceLux);
    }
    
    //shading settings
    if (ImGui::CollapsingHeader("Shading settings")) {
        m_isMainPassShaderDescriptionStale |= ImGui::Checkbox("Indirect Multiscatter BRDF", &m_shadingConfig.useIndirectMultiscatter);

        //naming and values rely on enum values being ordered same as names and indices being [0,3]
        const char* diffuseBRDFOptions[] = { "Lambert", "Disney", "CoD WWII", "Titanfall 2" };
        const bool diffuseBRDFChanged = ImGui::Combo("Diffuse BRDF",
            (int*)&m_shadingConfig.diffuseBRDF,
            diffuseBRDFOptions, 4);
        m_isMainPassShaderDescriptionStale |= diffuseBRDFChanged;
        m_isBRDFLutShaderDescriptionStale = diffuseBRDFChanged;

        //naming and values rely on enum values being ordered same as names and indices being [0,3]
        const char* directMultiscatterBRDFOptions[] = { "McAuley", "Simplified", "Scaled GGX lobe", "None" };
        m_isMainPassShaderDescriptionStale |= ImGui::Combo("Direct Multiscatter BRDF",
            (int*)&m_shadingConfig.directMultiscatter,
            directMultiscatterBRDFOptions, 4);

        m_isMainPassShaderDescriptionStale |= ImGui::Checkbox("Geometric AA", &m_shadingConfig.useGeometryAA);
        m_isMainPassShaderDescriptionStale |= ImGui::Checkbox("Sky occlusion", &m_shadingConfig.useSkyOcclusion);
        m_isMainPassShaderDescriptionStale |= ImGui::Checkbox("Sky occlusion direction", &m_shadingConfig.useSkyOcclusionDirection);
    }
    //camera settings
    if (ImGui::CollapsingHeader("Camera settings")) {
        ImGui::InputFloat("Near plane", &m_camera.intrinsic.near);
        ImGui::InputFloat("Far plane", &m_camera.intrinsic.far);
    }
    
    //debug settings
    if (ImGui::CollapsingHeader("Debug settings")) {
        ImGui::Checkbox("Draw bounding boxes", &m_drawBBs);
        ImGui::Checkbox("Freeze and draw camera frustum", &m_freezeAndDrawCameraFrustum);
        ImGui::Checkbox("Draw shadow frustum", &m_drawShadowFrustum);
    }    

    ImGui::End();
}