#include "pch.h"
#include "RenderFrontend.h"
#include "ImageLoader.h"
#include <imgui/imgui.h>
#include <Utilities/MathUtils.h>
#include "Utilities/Timer.h"
#include "Culling.h"

#define GLFW_INCLUDE_VULKAN
#include "GLFW/glfw3.h"

/*
=========
resizeCallback
=========
*/
void resizeCallback(GLFWwindow* window, int width, int height) {
    RenderFrontend* frontEnd = reinterpret_cast<RenderFrontend*>(glfwGetWindowUserPointer(window));
    frontEnd->setResolution(width, height);
}

/*
=========
setup
=========
*/
void RenderFrontend::setup(GLFWwindow* window) {
    m_window = window;

    int width, height;
    glfwGetWindowSize(window, &width, &height);
    m_screenWidth = width;
    m_screenHeight = height;
    m_camera.intrinsic.aspectRatio = (float)width / (float)height;

    m_backend.setup(window);
    glfwSetWindowUserPointer(window, this);
    glfwSetFramebufferSizeCallback(window, resizeCallback);

    createDefaultTextures();

    //load skybox
    ImageDescription hdrCapture;
    if (loadImage("textures\\sunset_in_the_chalk_quarry_2k.hdr", false, &hdrCapture)) {
        m_environmentMapSrc = m_backend.createImage(hdrCapture);
    }
    else {
        m_environmentMapSrc = m_defaultSkyTexture;
    }
    
    createDefaultSamplers();

    createSkyTexturePreparationPasses();
    createSpecularConvolutionPass();
    createDiffuseConvolutionPass();
    createBRDFLutPreparationPass();
    createHistogramPasses();

    createShadowPass();
    createMainPass(width, height);
    createSkyPass();
    createSkyCubeMesh();
    createDebugGeoPass();
    createDepthPrePass();
    createDepthPyramidPass();
    createLightMatrixPass();
}

/*
=========
teardown
=========
*/
void RenderFrontend::teardown() {
    m_backend.teardown();
}

/*
=========
newFrame
=========
*/
void RenderFrontend::newFrame() {
    if (m_didResolutionChange) {
        m_backend.recreateSwapchain(m_screenWidth, m_screenHeight, m_window);
        m_backend.resizeImages( { m_colorBuffer, m_depthBuffer }, m_screenWidth, m_screenHeight);
        m_backend.resizeImages({ m_minMaxDepthPyramid}, m_screenWidth / 2, m_screenHeight / 2);
        m_didResolutionChange = false;

        updateDepthPyramidShaderDescription();
        m_backend.updateComputePassShaderDescription(m_depthPyramidPass, m_depthPyramidShaderConfig);
    }
    if (m_minimized) {
        return;
    }

    m_currentFrameMainPassDrawcallCount = 0;

    if (m_isMainPassShaderDescriptionStale) {
        m_backend.updateGraphicPassShaderDescription(m_mainPass, m_mainPassShaderConfig);
        m_isMainPassShaderDescriptionStale = false;
    }

    if (m_isBRDFLutShaderDescriptionStale) {
        m_brdfLutPassShaderConfig.specialisationConstants.values[m_brdfLutSpecilisationConstantDiffuseBRDFIndex] 
            = m_mainPassShaderConfig.fragment.specialisationConstants.values[m_mainPassSpecilisationConstantDiffuseBRDFIndex];
        m_backend.updateComputePassShaderDescription(m_brdfLutPass, m_brdfLutPassShaderConfig);
        //don't reset m_isMainPassShaderDescriptionStale, this is done when rendering as it's used to trigger lut recreation
    }

    m_backend.updateShaderCode();

    m_backend.newFrame();

    m_bbsToDebugDraw.clear();
}

/*
=========
setResolution
=========
*/
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

/*
=========
setCameraMatrix
=========
*/
void RenderFrontend::setCameraExtrinsic(const CameraExtrinsic& extrinsic) {
    m_camera.extrinsic = extrinsic;
    const glm::mat4 viewMatrix = viewMatrixFromCameraExtrinsic(extrinsic);
    const glm::mat4 projectionMatrix = projectionMatrixFromCameraIntrinsic(m_camera.intrinsic);
    const glm::mat4 viewProjectionMatrix = projectionMatrix * viewMatrix;
    m_backend.setViewProjectionMatrix(viewProjectionMatrix, m_mainPass);
    m_backend.setViewProjectionMatrix(viewProjectionMatrix, m_skyPass);
    m_backend.setViewProjectionMatrix(viewProjectionMatrix, m_debugGeoPass);
    m_backend.setViewProjectionMatrix(viewProjectionMatrix, m_depthPrePass);

    if (!m_freezeAndDrawCameraFrustum) {
        updateCameraFrustum();
    }
}

/*
=========
createMeshes
=========
*/
std::vector<MeshHandle> RenderFrontend::createMeshes(const std::vector<MeshData>& meshData) {

    //this is a lot of copying... improve later?
    std::vector<MeshDataInternal> dataInternal;
    dataInternal.reserve(meshData.size());

    for (const auto& data : meshData) {
        MeshDataInternal mesh;
        mesh.indices = data.indices;
        mesh.positions = data.positions;
        mesh.uvs = data.uvs;
        mesh.normals = data.normals;
        mesh.tangents = data.tangents;
        mesh.bitangents = data.bitangents;

        if (!getImageFromPath(data.material.albedoTexturePath, &mesh.diffuseTexture)) {
            mesh.diffuseTexture = m_defaultDiffuseTexture;
        }
        if (!getImageFromPath(data.material.normalTexturePath, &mesh.normalTexture)) {
            mesh.normalTexture = m_defaultNormalTexture;
        }
        if (!getImageFromPath(data.material.specularTexturePath, &mesh.specularTexture)) {
            mesh.specularTexture = m_defaultSpecularTexture;
        }

        dataInternal.push_back(mesh);
    }

    const auto handles = m_backend.createMeshes(dataInternal, std::vector<RenderPassHandle> { m_mainPass, m_shadowPass });

    //compute and store bounding boxes
    assert(handles.size() == dataInternal.size());
    for (uint32_t i = 0; i < handles.size(); i++) {
        const auto& meshData = dataInternal[i];
        const auto& handle = handles[i];
        const auto bb = axisAlignedBoundingBoxFromPositions(meshData.positions);
        m_meshHandleToBoundingBox[handle] = bb;

        //create debug mesh for rendering
        const auto debugMesh = m_backend.createDynamicMeshes(
            { axisAlignedBoundingBoxPositionsPerMesh }, { axisAlignedBoundingBoxIndicesPerMesh }).back();
        m_bbDebugMeshes.push_back(debugMesh);
    }

    return handles;
}

/*
=========
issueMeshDraw
=========
*/
void RenderFrontend::issueMeshDraws(const std::vector<MeshHandle>& meshes, const std::vector<glm::mat4>& modelMatrices) {
    if (meshes.size() != modelMatrices.size()) {
        std::cout << "Error: RenderFrontend::issueMeshDraws mesh and model matrix count do not match\n";
    }

    std::vector<MeshHandle> culledMainPassMeshes;
    std::vector<glm::mat4> culledMainPassTransforms;

    //frustum culling
    for (uint32_t i = 0; i < std::min(meshes.size(), modelMatrices.size()); i++) {
        const auto handle = meshes[i];
        const auto& bb = m_meshHandleToBoundingBox[handle];
        const auto& transform = modelMatrices[i];

        //account for transform
        const auto bbTransformed = axisAlignedBoundingBoxTransformed(bb, transform);
        const bool renderMesh = isAxisAlignedBoundingBoxIntersectingViewFrustum(m_cameraFrustum, bb);

        if (renderMesh) {
            m_currentFrameMainPassDrawcallCount++;

            culledMainPassMeshes.push_back(handle);
            culledMainPassTransforms.push_back(transform);

            //FIXME move draw BBs back here after debugging   
        }
        if (m_drawBBs) {
            m_bbsToDebugDraw.push_back(bbTransformed);
        }
    }

    //all meshes drawn in shadow pass for now
    m_backend.drawMeshes(meshes, modelMatrices, { m_shadowPass });

    //main pass is culled
    m_backend.drawMeshes(culledMainPassMeshes, culledMainPassTransforms, { m_mainPass, m_depthPrePass });
}

/*
=========
renderFrame
=========
*/
void RenderFrontend::renderFrame() {

    if (m_minimized) {
        return;
    }

    /*
    additional passes that have to be executed before the main pass
    */
    std::vector<RenderPassHandle> preparationPasses;

    if (m_firstFrame) {

        firstFramePreparation();

        preparationPasses.push_back(m_diffuseConvolutionPass);
        preparationPasses.push_back(m_brdfLutPass);
        for (uint32_t i = 0; i < m_specularConvolutionPerMipPasses.size(); i++) {
            preparationPasses.push_back(m_specularConvolutionPerMipPasses[i]);
        }

        m_firstFrame = false;
    }

    if (m_isBRDFLutShaderDescriptionStale) {
        computeBRDFLut();
        preparationPasses.push_back(m_brdfLutPass);
        m_isBRDFLutShaderDescriptionStale = false;
    }

    /*
    render sun shadow
    */
    {
        RenderPassExecution shadowPassExecution;
        shadowPassExecution.handle = m_shadowPass;
        shadowPassExecution.parents = { m_lightMatrixPass };

        StorageBufferResource lightMatrixBufferResource(m_sunLightMatrixBuffer, true, 0);
        shadowPassExecution.resources.storageBuffers = { lightMatrixBufferResource };

        m_backend.setRenderPassExecution(shadowPassExecution);
    }
    
    /*
    histogram and exposure computation
    */
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
        histogramPerTileExecution.dispatchCount[0] = uint32_t(std::ceilf((float)m_screenWidth  / float(m_histogramTileSizeX)));
        histogramPerTileExecution.dispatchCount[1] = uint32_t(std::ceilf((float)m_screenHeight / float(m_histogramTileSizeY)));
        histogramPerTileExecution.dispatchCount[2] = 1;

        m_backend.setRenderPassExecution(histogramPerTileExecution);
    }

    const float binsPerDispatch = 64.f;
    //reset global tile
    {
        RenderPassExecution histogramResetExecution;
        histogramResetExecution.handle = m_histogramResetPass;
        histogramResetExecution.resources.storageBuffers = { histogramResource };
        histogramResetExecution.dispatchCount[0] = uint32_t(std::ceilf(float(m_nHistogramBins) / binsPerDispatch));
        histogramResetExecution.dispatchCount[1] = 1;
        histogramResetExecution.dispatchCount[2] = 1;

        m_backend.setRenderPassExecution(histogramResetExecution);
    }

    //combine tiles
    {
        RenderPassExecution histogramCombineTilesExecution;
        histogramCombineTilesExecution.handle = m_histogramCombinePass;
        histogramCombineTilesExecution.resources.storageBuffers = { histogramPerTileResource, histogramResource };
        float nTiles = 
            std::ceilf(m_screenWidth  / float(m_histogramTileSizeX)) * 
            std::ceilf(m_screenHeight / float(m_histogramTileSizeY));
        histogramCombineTilesExecution.dispatchCount[0] = nTiles;
        histogramCombineTilesExecution.dispatchCount[1] = uint32_t(std::ceilf(float(m_nHistogramBins) / binsPerDispatch));
        histogramCombineTilesExecution.dispatchCount[2] = 1;
        histogramCombineTilesExecution.parents = { m_histogramPerTilePass, m_histogramResetPass };

        m_backend.setRenderPassExecution(histogramCombineTilesExecution);
    }

    //pre expose
    {
        StorageBufferResource lightBufferResource(m_lightBuffer, false, 0);

        RenderPassExecution preExposeLightsExecution;
        preExposeLightsExecution.handle = m_preExposeLightsPass;
        preExposeLightsExecution.resources.storageBuffers = { histogramResource, lightBufferResource };
        preExposeLightsExecution.parents = { m_histogramCombinePass };
        preExposeLightsExecution.dispatchCount[0] = 1;
        preExposeLightsExecution.dispatchCount[1] = 1;
        preExposeLightsExecution.dispatchCount[2] = 1;

        m_backend.setRenderPassExecution(preExposeLightsExecution);
    }
    
    //depth prepass
    {
        RenderPassExecution prepassExe;
        prepassExe.handle = m_depthPrePass;
        m_backend.setRenderPassExecution(prepassExe);
    }

    //depth pyramid
    {
        RenderPassExecution exe;
        exe.handle = m_depthPyramidPass;
        exe.parents = { m_depthPrePass };
        const auto dispatchCount = computeDepthPyramidDispatchCount();
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
            const uint32_t mipLevel = i >= unusedMipCount ?  i - unusedMipCount : 0;
            ImageResource pyramidMip(m_minMaxDepthPyramid, mipLevel, i);
            exe.resources.storageImages.push_back(pyramidMip);
        }
        m_backend.setRenderPassExecution(exe);
    }

    //compute light matrix
    {
        RenderPassExecution exe;
        exe.handle = m_lightMatrixPass;
        exe.parents = { m_depthPyramidPass };
        exe.dispatchCount[0] = 1;
        exe.dispatchCount[1] = 1;
        exe.dispatchCount[2] = 1;

        const uint32_t depthPyramidMipCount = mipCountFromResolution(m_screenWidth / 2, m_screenHeight / 2, 1);
        ImageResource depthPyramidLowestMipResource(m_minMaxDepthPyramid, depthPyramidMipCount-1, 1);
        exe.resources.storageImages = { depthPyramidLowestMipResource };

        StorageBufferResource lightMatrixBuffer(m_sunLightMatrixBuffer, false, 0);
        exe.resources.storageBuffers = { lightMatrixBuffer };

        m_backend.setRenderPassExecution(exe);
    }

    /*
    render scene geometry
    */
    {
        const auto shadowMapResource = ImageResource(m_shadowMap, 0, 0);
        const auto shadowSamplerResource = SamplerResource(m_shadowSampler, 1);
        const auto diffuseProbeResource = ImageResource(m_diffuseProbe, 0, 2);
        const auto cubeSamplerResource = SamplerResource(m_cubeSampler, 3);
        const auto brdfLutResource = ImageResource(m_brdfLut, 0, 4);
        const auto specularProbeResource = ImageResource(m_specularProbe, 0, 5);
        const auto cubeSamplerMipsResource = SamplerResource(m_skySamplerWithMips, 6);
        const auto lustSamplerResource = SamplerResource(m_lutSampler, 7);
        const auto lightBufferResource = StorageBufferResource(m_lightBuffer, true, 8);
        const auto lightMatrixBuffer = StorageBufferResource(m_sunLightMatrixBuffer, true, 9);

        RenderPassExecution mainPassExecution;
        mainPassExecution.handle = m_mainPass;
        mainPassExecution.resources.storageBuffers = { lightBufferResource, lightMatrixBuffer };
        mainPassExecution.resources.sampledImages = { shadowMapResource, diffuseProbeResource, brdfLutResource, specularProbeResource };
        mainPassExecution.resources.samplers = { shadowSamplerResource, cubeSamplerResource, cubeSamplerMipsResource, lustSamplerResource };
        mainPassExecution.parents = { m_shadowPass, m_preExposeLightsPass, m_depthPrePass, m_lightMatrixPass };
        mainPassExecution.parents.insert(mainPassExecution.parents.begin(), preparationPasses.begin(), preparationPasses.end());
        m_backend.setRenderPassExecution(mainPassExecution);
    }

    bool drawDebugPass = false;

    //update view frustum debug model
    if (m_freezeAndDrawCameraFrustum) {
        m_backend.drawDynamicMeshes({ m_cameraFrustumModel }, { glm::mat4(1.f) }, { m_debugGeoPass });
        drawDebugPass = true;
    }

    //update bounding box debug models
    if(m_drawBBs && m_bbsToDebugDraw.size() > 0) {
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

        m_backend.updateDynamicMeshes(m_bbDebugMeshes, positionsPerMesh, indicesPerMesh);
        m_backend.drawDynamicMeshes(m_bbDebugMeshes, std::vector<glm::mat4> (m_bbDebugMeshes.size(), glm::mat4(1.f)), {m_debugGeoPass});

        drawDebugPass = true;
    }

    //debug pass
    if (drawDebugPass) {
        RenderPassExecution debugPassExecution;
        debugPassExecution.handle = m_debugGeoPass;
        debugPassExecution.parents = { m_mainPass };
        m_backend.setRenderPassExecution(debugPassExecution);
    }

    /*
    render sky
    */
    {
        const auto skyTextureResource = ImageResource(m_skyTexture, 0, 0);
        const auto skySamplerResource = SamplerResource(m_cubeSampler, 1);
        const auto lightBufferResource = StorageBufferResource(m_lightBuffer, true, 2);

        RenderPassExecution skyPassExecution;
        skyPassExecution.handle = m_skyPass;
        skyPassExecution.resources.storageBuffers = { lightBufferResource };
        skyPassExecution.resources.sampledImages = { skyTextureResource };
        skyPassExecution.resources.samplers = { skySamplerResource };
        skyPassExecution.parents = { m_mainPass };
        if (m_firstFrame) {
            skyPassExecution.parents.push_back(m_toCubemapPass);
        }
        if (drawDebugPass) {
            skyPassExecution.parents.push_back(m_debugGeoPass);
        }
        m_backend.setRenderPassExecution(skyPassExecution);
    }

    /*
    update and final commands
    */
    drawUi();
    updateSun();
    updateGlobalShaderInfo();
    m_backend.drawMeshes(std::vector<MeshHandle> {m_skyCube}, std::vector<glm::mat4> { glm::mat4(1.f)}, std::vector<RenderPassHandle> { m_skyPass });
    m_backend.renderFrame();
}

/*
=========
getImageFromPath
=========
*/
bool RenderFrontend::getImageFromPath(std::filesystem::path path, ImageHandle* outImageHandle) {

    if (path == "") {
        return false;
    }

    if (m_textureMap.find(path) == m_textureMap.end()) {
        ImageDescription image;
        if (loadImage(path, true, &image)) {
            *outImageHandle = m_backend.createImage(image);
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

/*
=========
firstFramePreparation
=========
*/
void RenderFrontend::firstFramePreparation() {
    /*
    write to sky texture
    */
    const auto skyTextureResource = ImageResource(m_skyTexture, 0, 0);
    const auto hdrCaptureResource = ImageResource(m_environmentMapSrc, 0, 1);
    const auto hdrSamplerResource = SamplerResource(m_cubeSampler, 2);

    RenderPassExecution cubeWriteExecution;
    cubeWriteExecution.handle = m_toCubemapPass;
    cubeWriteExecution.resources.storageImages = { skyTextureResource };
    cubeWriteExecution.resources.sampledImages = { hdrCaptureResource };
    cubeWriteExecution.resources.samplers = { hdrSamplerResource };
    cubeWriteExecution.dispatchCount[0] = m_skyTextureRes / 8;
    cubeWriteExecution.dispatchCount[1] = m_skyTextureRes / 8;
    cubeWriteExecution.dispatchCount[2] = 6;
    m_backend.setRenderPassExecution(cubeWriteExecution);

    /*
    create sky texture mips
    */
    for (uint32_t i = 1; i < m_skyTextureMipCount; i++) {
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
        skyMipExecution.dispatchCount[0] = m_skyTextureRes / 8 / glm::pow(2, i);
        skyMipExecution.dispatchCount[1] = m_skyTextureRes / 8 / glm::pow(2, i);
        skyMipExecution.dispatchCount[2] = 6;
        m_backend.setRenderPassExecution(skyMipExecution);
    }

    /*
    diffuse convolution
    */
    const auto diffuseProbeResource = ImageResource(m_diffuseProbe, 0, 0);
    const auto diffuseConvolutionSrcResource = ImageResource(m_skyTexture, 0, 1);
    const auto cubeSamplerResource = SamplerResource(m_skySamplerWithMips, 2);

    RenderPassExecution diffuseConvolutionExecution;
    diffuseConvolutionExecution.handle = m_diffuseConvolutionPass;
    diffuseConvolutionExecution.parents = { m_toCubemapPass };
    diffuseConvolutionExecution.resources.storageImages = { diffuseProbeResource };
    diffuseConvolutionExecution.resources.sampledImages = { diffuseConvolutionSrcResource };
    diffuseConvolutionExecution.resources.samplers = { cubeSamplerResource };
    diffuseConvolutionExecution.dispatchCount[0] = m_diffuseProbeRes / 8;
    diffuseConvolutionExecution.dispatchCount[1] = m_diffuseProbeRes / 8;
    diffuseConvolutionExecution.dispatchCount[2] = 6;
    m_backend.setRenderPassExecution(diffuseConvolutionExecution);

    computeBRDFLut();

    /*
    specular probe convolution
    */
    for (uint32_t mipLevel = 0; mipLevel < m_specularProbeMipCount; mipLevel++) {

        const auto specularProbeResource = ImageResource(m_specularProbe, mipLevel, 0);
        const auto specularConvolutionSrcResource = ImageResource(m_skyTexture, 0, 1);
        const auto specCubeSamplerResource = SamplerResource(m_skySamplerWithMips, 2);

        RenderPassExecution specularConvolutionExecution;
        specularConvolutionExecution.handle = m_specularConvolutionPerMipPasses[mipLevel];
        specularConvolutionExecution.parents = { m_toCubemapPass, m_cubemapMipPasses.back() };
        specularConvolutionExecution.resources.storageImages = { specularProbeResource };
        specularConvolutionExecution.resources.sampledImages = { specularConvolutionSrcResource };
        specularConvolutionExecution.resources.samplers = { specCubeSamplerResource };
        specularConvolutionExecution.dispatchCount[0] = m_specularProbeRes / 8;
        specularConvolutionExecution.dispatchCount[1] = m_specularProbeRes / 8;
        specularConvolutionExecution.dispatchCount[2] = 6;
        m_backend.setRenderPassExecution(specularConvolutionExecution);
    }
}

/*
=========
computeBRDFLut
=========
*/
void RenderFrontend::computeBRDFLut() {
    /*
    create brdf lut for split sum approximation
    */
    const auto brdfLutStorageResource = ImageResource(m_brdfLut, 0, 0);

    RenderPassExecution brdfLutExecution;
    brdfLutExecution.handle = m_brdfLutPass;
    brdfLutExecution.resources.storageImages = { brdfLutStorageResource };
    brdfLutExecution.dispatchCount[0] = m_brdfLutRes / 8;
    brdfLutExecution.dispatchCount[1] = m_brdfLutRes / 8;
    brdfLutExecution.dispatchCount[2] = 1;
    m_backend.setRenderPassExecution(brdfLutExecution);
}

/*
=========
updateCameraFrustum
=========
*/
void RenderFrontend::updateCameraFrustum() {
    m_cameraFrustum = computeViewFrustum(m_camera);

    std::vector<glm::vec3> frustumPoints;
    std::vector<uint32_t> frustumIndices;

    frustumToLineMesh(m_cameraFrustum, &frustumPoints, &frustumIndices);
    m_backend.updateDynamicMeshes({ m_cameraFrustumModel }, { frustumPoints }, { frustumIndices });
}

/*
=========
updateGlobalShaderInfo
=========
*/
void RenderFrontend::updateGlobalShaderInfo() {
    m_globalShaderInfo.cameraPos = glm::vec4(m_camera.extrinsic.position, 1.f);

    Timer& timer = Timer::getReference();
    m_globalShaderInfo.deltaTime = timer.getDeltaTimeFloat();
    m_globalShaderInfo.nearPlane = m_camera.intrinsic.near;
    m_globalShaderInfo.farPlane = m_camera.intrinsic.far;

    m_globalShaderInfo.cameraRight      = glm::vec4(m_camera.extrinsic.right, 0);
    m_globalShaderInfo.cameraUp         = glm::vec4(m_camera.extrinsic.up, 0);
    m_globalShaderInfo.cameraForward    = glm::vec4(m_camera.extrinsic.forward, 0);
    m_globalShaderInfo.cameraTanFovHalf = glm::tan(glm::radians(m_camera.intrinsic.fov) * 0.5f);
    m_globalShaderInfo.cameraAspectRatio = m_camera.intrinsic.aspectRatio;

    m_backend.setGlobalShaderInfo(m_globalShaderInfo);
}

/*
=========
createMainPass
=========
*/
void RenderFrontend::createMainPass(const uint32_t width, const uint32_t height) {
    
    const auto colorBufferDescription = ImageDescription(
        std::vector<char>{},
        width,
        height,
        1,
        ImageType::Type2D,
        ImageFormat::R11G11B10_uFloat,
        (ImageUsageFlags)(IMAGE_USAGE_ATTACHMENT | IMAGE_USAGE_STORAGE | IMAGE_USAGE_SAMPLED),
        MipCount::One,
        0,
        false);
    m_colorBuffer = m_backend.createImage(colorBufferDescription);

    const auto depthBufferDescription = ImageDescription(
        std::vector<char>{},
        width,
        height,
        1,
        ImageType::Type2D,
        ImageFormat::Depth32,
        ImageUsageFlags(IMAGE_USAGE_ATTACHMENT | IMAGE_USAGE_SAMPLED),
        MipCount::One,
        0,
        false
    );
    m_depthBuffer = m_backend.createImage(depthBufferDescription);

    const auto shadowSamplerDesc = SamplerDescription(
        SamplerInterpolation::Nearest,
        SamplerWrapping::Color,
        false,
        0,
        SamplerBorderColor::White,
        0);
    m_shadowSampler = m_backend.createSampler(shadowSamplerDesc);

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

    
    m_mainPassShaderConfig.vertex.srcPathRelative   = "triangle.vert";
    m_mainPassShaderConfig.fragment.srcPathRelative = "triangle.frag";

    m_mainPassShaderConfig.fragment.specialisationConstants.locationIDs.push_back(m_mainPassSpecilisationConstantDiffuseBRDFIndex);
    m_mainPassShaderConfig.fragment.specialisationConstants.values.push_back(m_diffuseBRDFDefaultSelection);

    const int directMultiscatterBRDFDefaultSelection = 0;
    m_mainPassShaderConfig.fragment.specialisationConstants.locationIDs.push_back(m_mainPassSpecilisationConstantDirectMultiscatterBRDFIndex);
    m_mainPassShaderConfig.fragment.specialisationConstants.values.push_back(directMultiscatterBRDFDefaultSelection);

    const int indirectMultiscatterBRDFDefaultSelection = 1;
    m_mainPassShaderConfig.fragment.specialisationConstants.locationIDs.push_back(m_mainPassSpecilisationConstantUseIndirectMultiscatterBRDFIndex);
    m_mainPassShaderConfig.fragment.specialisationConstants.values.push_back(indirectMultiscatterBRDFDefaultSelection);

    const int useGeometricAADefaultSelection = 1;
    m_mainPassShaderConfig.fragment.specialisationConstants.locationIDs.push_back(m_mainPassSpecilisationConstantGeometricAAIndex);
    m_mainPassShaderConfig.fragment.specialisationConstants.values.push_back(useGeometricAADefaultSelection);

    const uint32_t specularProbeMipCountConstantIndex = 4;
    m_mainPassShaderConfig.fragment.specialisationConstants.locationIDs.push_back(specularProbeMipCountConstantIndex);
    m_mainPassShaderConfig.fragment.specialisationConstants.values.push_back(m_specularProbeMipCount);
    
    GraphicPassDescription mainPassDesc;
    mainPassDesc.name = "Forward shading";
    mainPassDesc.shaderDescriptions = m_mainPassShaderConfig;
    mainPassDesc.attachments = { colorAttachment, depthAttachment };
    mainPassDesc.depthTest.function = DepthFunction::Equal;
    mainPassDesc.depthTest.write = true;
    mainPassDesc.rasterization.cullMode = CullMode::Back;
    mainPassDesc.rasterization.mode = RasterizationeMode::Fill;
    mainPassDesc.blending = BlendState::None;

    m_mainPass = m_backend.createGraphicPass(mainPassDesc);
    m_backend.setSwapchainInputImage(m_colorBuffer);

    const auto cubeSamplerDesc = SamplerDescription(
        SamplerInterpolation::Linear,
        SamplerWrapping::Clamp,
        false,
        1,
        SamplerBorderColor::White,
        0);
    m_cubeSampler = m_backend.createSampler(cubeSamplerDesc);

    const auto lutSamplerDesc = SamplerDescription(
        SamplerInterpolation::Linear,
        SamplerWrapping::Clamp,
        false,
        1,
        SamplerBorderColor::White,
        0);
    m_lutSampler = m_backend.createSampler(lutSamplerDesc);
}

/*
=========
createShadowPass
=========
*/
void RenderFrontend::createShadowPass() {

    const auto shadowMapDesc = ImageDescription(
        std::vector<char>{},
        m_shadowMapRes,
        m_shadowMapRes,
        1,
        ImageType::Type2D,
        ImageFormat::Depth16,
        (ImageUsageFlags)(IMAGE_USAGE_ATTACHMENT | IMAGE_USAGE_SAMPLED),
        MipCount::One,
        1,
        false);
    m_shadowMap = m_backend.createImage(shadowMapDesc);

    const auto shadowMapAttachment = Attachment(
        m_shadowMap,
        0,
        0,
        AttachmentLoadOp::Clear);

    GraphicPassDescription shadowPassConfig;
    shadowPassConfig.name = "Shadow map";
    shadowPassConfig.attachments = { shadowMapAttachment };
    shadowPassConfig.shaderDescriptions.vertex.srcPathRelative   = "sunShadow.vert";
    shadowPassConfig.shaderDescriptions.fragment.srcPathRelative = "sunShadow.frag";
    shadowPassConfig.depthTest.function = DepthFunction::LessEqual;
    shadowPassConfig.depthTest.write = true;
    shadowPassConfig.rasterization.cullMode = CullMode::Front;
    shadowPassConfig.rasterization.mode = RasterizationeMode::Fill;
    shadowPassConfig.rasterization.clampDepth = true;
    shadowPassConfig.blending = BlendState::None;
    m_shadowPass = m_backend.createGraphicPass(shadowPassConfig);
}

/*
=========
createSkyTexturePreparationPasses
=========
*/
void RenderFrontend::createSkyTexturePreparationPasses() {

    ComputePassDescription cubeWriteDesc;
    cubeWriteDesc.name = "Copy sky to cubemap";
    cubeWriteDesc.shaderDescription.srcPathRelative = "copyToCube.comp";
    m_toCubemapPass = m_backend.createComputePass(cubeWriteDesc);

    ComputePassDescription cubemapMipPassDesc;
    cubemapMipPassDesc.name = "Sky mip creation";
    cubemapMipPassDesc.shaderDescription.srcPathRelative = "cubemapMip.comp";
    /*
    first map is written to by different shader
    */
    for (uint32_t i = 0; i < m_skyTextureMipCount - 1; i++) {
        m_cubemapMipPasses.push_back(m_backend.createComputePass(cubemapMipPassDesc));
    }
    
     const auto hdriSamplerDesc = SamplerDescription(
        SamplerInterpolation::Linear,
        SamplerWrapping::Clamp,
        false,
        0,
        SamplerBorderColor::Black,
        0);
     m_hdriSampler = m_backend.createSampler(hdriSamplerDesc);
}

/*
=========
createDiffuseConvolutionPass
=========
*/
void RenderFrontend::createSpecularConvolutionPass() {

    m_specularProbeMipCount = mipCountFromResolution(m_specularProbeRes, m_specularProbeRes, 1);
    //don't use the last few mips as they are too small
    const uint32_t mipsTooSmallCount = 4;
    if (m_specularProbeMipCount > mipsTooSmallCount) {
        m_specularProbeMipCount -= mipsTooSmallCount;
    }

    for (uint32_t i = 0; i < m_specularProbeMipCount; i++) {
        ComputePassDescription specularConvolutionDesc;
        specularConvolutionDesc.name = "Specular probe convolution";
        specularConvolutionDesc.shaderDescription.srcPathRelative = "specularCubeConvolution.comp";

        const uint32_t mipCountConstantIndex = 0;
        specularConvolutionDesc.shaderDescription.specialisationConstants.locationIDs.push_back(mipCountConstantIndex);
        specularConvolutionDesc.shaderDescription.specialisationConstants.values.push_back(m_specularProbeMipCount);

        const uint32_t mipLevelConstantIndex = 1;
        specularConvolutionDesc.shaderDescription.specialisationConstants.locationIDs.push_back(mipLevelConstantIndex);
        specularConvolutionDesc.shaderDescription.specialisationConstants.values.push_back(i);

        m_specularConvolutionPerMipPasses.push_back(m_backend.createComputePass(specularConvolutionDesc));
    }

    const auto specularProbeDesc = ImageDescription(
        std::vector<char>{},
        m_specularProbeRes,
        m_specularProbeRes,
        1,
        ImageType::TypeCube,
        ImageFormat::R11G11B10_uFloat,
        ImageUsageFlags(IMAGE_USAGE_SAMPLED | IMAGE_USAGE_STORAGE),
        MipCount::Manual,
        m_specularProbeMipCount,
        false);
    m_specularProbe = m_backend.createImage(specularProbeDesc);
}

/*
=========
createDiffuseConvolutionPass
=========
*/
void RenderFrontend::createDiffuseConvolutionPass() {
    ComputePassDescription diffuseConvolutionDesc;
    diffuseConvolutionDesc.name = "Diffuse probe convolution";
    diffuseConvolutionDesc.shaderDescription.srcPathRelative = "diffuseCubeConvolution.comp";
    m_diffuseConvolutionPass = m_backend.createComputePass(diffuseConvolutionDesc);

    const auto diffuseProbeDesc = ImageDescription(
        std::vector<char>{},
        m_diffuseProbeRes,
        m_diffuseProbeRes,
        1,
        ImageType::TypeCube,
        ImageFormat::R11G11B10_uFloat,
        ImageUsageFlags(IMAGE_USAGE_SAMPLED | IMAGE_USAGE_STORAGE),
        MipCount::One,
        0,
        false);
    m_diffuseProbe = m_backend.createImage(diffuseProbeDesc);
}

/*
=========
createSkyPass
=========
*/
void RenderFrontend::createSkyPass() {

    const auto skyCubeDesc = ImageDescription(
        std::vector<char>{},
        m_skyTextureRes,
        m_skyTextureRes,
        1,
        ImageType::TypeCube,
        ImageFormat::R11G11B10_uFloat,
        (ImageUsageFlags)(IMAGE_USAGE_SAMPLED | IMAGE_USAGE_STORAGE),
        MipCount::Manual,
        8,
        false);
    m_skyTexture = m_backend.createImage(skyCubeDesc);
    const auto colorAttachment = Attachment(m_colorBuffer, 0, 0, AttachmentLoadOp::Load);
    const auto depthAttachment = Attachment(m_depthBuffer, 0, 0, AttachmentLoadOp::Load);

    GraphicPassDescription skyPassConfig;
    skyPassConfig.name = "Skybox render";
    skyPassConfig.attachments = { colorAttachment, depthAttachment };
    skyPassConfig.shaderDescriptions.vertex.srcPathRelative   = "sky.vert";
    skyPassConfig.shaderDescriptions.fragment.srcPathRelative = "sky.frag";
    skyPassConfig.depthTest.function = DepthFunction::LessEqual;
    skyPassConfig.depthTest.write = false;
    skyPassConfig.rasterization.cullMode = CullMode::None;
    skyPassConfig.rasterization.mode = RasterizationeMode::Fill;
    skyPassConfig.blending = BlendState::None;
    
    m_skyPass = m_backend.createGraphicPass(skyPassConfig);
}

/*
=========
createBRDFLutPreparationPass
=========
*/
void RenderFrontend::createBRDFLutPreparationPass() {

    m_brdfLutPassShaderConfig.srcPathRelative = "brdfLut.comp";
    const int diffuseBRDFConstantID = 0;
    m_brdfLutPassShaderConfig.specialisationConstants.locationIDs.push_back(diffuseBRDFConstantID);
    m_brdfLutPassShaderConfig.specialisationConstants.values.push_back(m_diffuseBRDFDefaultSelection);

    ComputePassDescription brdfLutPassDesc;
    brdfLutPassDesc.name = "BRDF Lut creation";
    brdfLutPassDesc.shaderDescription = m_brdfLutPassShaderConfig;
    m_brdfLutPass = m_backend.createComputePass(brdfLutPassDesc);

    const auto brdfLutDesc = ImageDescription(
        std::vector<char>{},
        m_brdfLutRes,
        m_brdfLutRes,
        1,
        ImageType::Type2D,
        ImageFormat::RGBA16_sFloat,
        (ImageUsageFlags)(IMAGE_USAGE_SAMPLED | IMAGE_USAGE_STORAGE),
        MipCount::One,
        1,
        false);
    m_brdfLut = m_backend.createImage(brdfLutDesc);
}

/*
=========
createDebugGeoPass
=========
*/
void RenderFrontend::createDebugGeoPass() {
    //render pass
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

    m_debugGeoPass = m_backend.createGraphicPass(debugPassConfig);

    //meshes
    m_cameraFrustumModel = m_backend.createDynamicMeshes(
        { positionsInViewFrustumLineMesh }, { indicesInViewFrustumLineMesh }).front();
}

/*
=========
createHistogramPasses
=========
*/
void RenderFrontend::createHistogramPasses() {

    /*
    create ssbos
    */
    {
        BufferDescription histogramBufferDesc;
        histogramBufferDesc.size = m_nHistogramBins * sizeof(uint32_t);
        histogramBufferDesc.type = BufferType::Storage;
        m_histogramBuffer = m_backend.createStorageBuffer(histogramBufferDesc);
    }
   
    {
        float initialLightBufferData[3] = { 1.f, 1.f, 1.f };
        BufferDescription lightBufferDesc;
        lightBufferDesc.size = 3 * sizeof(uint32_t);
        lightBufferDesc.type = BufferType::Storage;
        lightBufferDesc.initialData = initialLightBufferData;
        m_lightBuffer = m_backend.createStorageBuffer(lightBufferDesc);
    }
    
    uint32_t pixelsPerTile = m_histogramTileSizeX * m_histogramTileSizeX;
    uint32_t nMaxTiles = 1920 * 1080 / pixelsPerTile; //FIXME: update buffer on rescale

    {
        BufferDescription histogramPerTileBufferDesc;
        histogramPerTileBufferDesc.size = nMaxTiles * m_nHistogramBins * sizeof(uint32_t);
        histogramPerTileBufferDesc.type = BufferType::Storage;
        m_histogramPerTileBuffer = m_backend.createStorageBuffer(histogramPerTileBufferDesc);
    }

    /*
    create renderpasses
    */
    const uint32_t nBinsSpecialisationConstantID = 0;
    const uint32_t minLumininanceSpecialisationConstantID = 1;
    const uint32_t maxLumininanceSpecialisationConstantID = 2;
    const uint32_t lumininanceFactorSpecialisationConstantID = 3;

    //range is remapped to avoid values < 0, due to problems with log()
    const uint32_t minHistogramValue = 1;
    const uint32_t maxHistogramValue = uint32_t(m_histogramMax / m_histogramMin);
    const uint32_t histogramFactor = uint32_t(1.f / m_histogramMin);

    {
        ComputePassDescription histogramPerTileDesc;
        histogramPerTileDesc.name = "Histogram per tile";
        histogramPerTileDesc.shaderDescription.srcPathRelative = "histogramPerTile.comp";

        const uint32_t maxTilesSpecialisationConstantID = 4;

        histogramPerTileDesc.shaderDescription.specialisationConstants.locationIDs.push_back(nBinsSpecialisationConstantID);
        histogramPerTileDesc.shaderDescription.specialisationConstants.locationIDs.push_back(minLumininanceSpecialisationConstantID);
        histogramPerTileDesc.shaderDescription.specialisationConstants.locationIDs.push_back(maxLumininanceSpecialisationConstantID);
        histogramPerTileDesc.shaderDescription.specialisationConstants.locationIDs.push_back(lumininanceFactorSpecialisationConstantID);
        histogramPerTileDesc.shaderDescription.specialisationConstants.locationIDs.push_back(maxTilesSpecialisationConstantID);

        histogramPerTileDesc.shaderDescription.specialisationConstants.values.push_back(m_nHistogramBins);
        histogramPerTileDesc.shaderDescription.specialisationConstants.values.push_back(minHistogramValue);
        histogramPerTileDesc.shaderDescription.specialisationConstants.values.push_back(maxHistogramValue);
        histogramPerTileDesc.shaderDescription.specialisationConstants.values.push_back(histogramFactor);
        histogramPerTileDesc.shaderDescription.specialisationConstants.values.push_back(nMaxTiles);

        m_histogramPerTilePass = m_backend.createComputePass(histogramPerTileDesc);
    }

    {
        ComputePassDescription resetDesc;
        resetDesc.name = "Histogram reset";
        resetDesc.shaderDescription.srcPathRelative = "histogramReset.comp";
        resetDesc.shaderDescription.specialisationConstants.locationIDs.push_back(nBinsSpecialisationConstantID);
        resetDesc.shaderDescription.specialisationConstants.values.push_back(m_nHistogramBins);
        m_histogramResetPass = m_backend.createComputePass(resetDesc);
    }

    {
        const uint32_t maxTilesSpecialisationConstantID = 1;

        ComputePassDescription histogramCombineDesc;
        histogramCombineDesc.name = "Histogram combine tiles";
        histogramCombineDesc.shaderDescription.srcPathRelative = "histogramCombineTiles.comp";

        histogramCombineDesc.shaderDescription.specialisationConstants.locationIDs.push_back(nBinsSpecialisationConstantID);
        histogramCombineDesc.shaderDescription.specialisationConstants.locationIDs.push_back(maxTilesSpecialisationConstantID);

        histogramCombineDesc.shaderDescription.specialisationConstants.values.push_back(m_nHistogramBins);
        histogramCombineDesc.shaderDescription.specialisationConstants.values.push_back(nMaxTiles);

        m_histogramCombinePass = m_backend.createComputePass(histogramCombineDesc);
    }

    {
        ComputePassDescription preExposeLightsDesc;
        preExposeLightsDesc.name = "Pre-expose lights";
        preExposeLightsDesc.shaderDescription.srcPathRelative = "preExposeLights.comp";

        preExposeLightsDesc.shaderDescription.specialisationConstants.locationIDs.push_back(nBinsSpecialisationConstantID);
        preExposeLightsDesc.shaderDescription.specialisationConstants.locationIDs.push_back(minLumininanceSpecialisationConstantID);
        preExposeLightsDesc.shaderDescription.specialisationConstants.locationIDs.push_back(maxLumininanceSpecialisationConstantID);
        preExposeLightsDesc.shaderDescription.specialisationConstants.locationIDs.push_back(lumininanceFactorSpecialisationConstantID);

        preExposeLightsDesc.shaderDescription.specialisationConstants.values.push_back(m_nHistogramBins);
        preExposeLightsDesc.shaderDescription.specialisationConstants.values.push_back(minHistogramValue);
        preExposeLightsDesc.shaderDescription.specialisationConstants.values.push_back(maxHistogramValue);
        preExposeLightsDesc.shaderDescription.specialisationConstants.values.push_back(histogramFactor);

        m_preExposeLightsPass = m_backend.createComputePass(preExposeLightsDesc);
    }
}

/*
=========
createDepthPrePass
=========
*/
void RenderFrontend::createDepthPrePass() {

    Attachment depthAttachment(m_depthBuffer, 0, 0, AttachmentLoadOp::Clear);

    GraphicPassDescription desc;
    desc.attachments = { depthAttachment };
    desc.blending = BlendState::None;
    desc.depthTest.function = DepthFunction::LessEqual;
    desc.depthTest.write = true;
    desc.name = "Depth prepass";
    desc.rasterization.cullMode = CullMode::Back;
    desc.shaderDescriptions.vertex.srcPathRelative = "depthPrepass.vert";
    desc.shaderDescriptions.fragment.srcPathRelative = "depthPrepass.frag";

    m_depthPrePass = m_backend.createGraphicPass(desc);
}

/*
=========
createDepthPyramidPass
=========
*/
void RenderFrontend::createDepthPyramidPass() {
    //texture
    {
        ImageDescription desc;
        desc.autoCreateMips = false;
        desc.width = m_screenWidth / 2;
        desc.height = m_screenHeight / 2;
        desc.depth = 1;
        desc.mipCount = MipCount::FullChain;
        desc.type = ImageType::Type2D;
        desc.format = ImageFormat::RG32_sFloat;
        desc.usageFlags = ImageUsageFlags(IMAGE_USAGE_SAMPLED | IMAGE_USAGE_STORAGE);
        m_minMaxDepthPyramid = m_backend.createImage(desc);
    }

    //render pass
    {
        ComputePassDescription desc;
        desc.name = "Depth min/max pyramid creation";

        updateDepthPyramidShaderDescription();
        desc.shaderDescription = m_depthPyramidShaderConfig;

        m_depthPyramidPass = m_backend.createComputePass(desc);
    }
    //sampler
    {
        SamplerDescription desc;
        desc.interpolation = SamplerInterpolation::Nearest;
        desc.maxMip = 11;
        desc.useAnisotropy = false;
        desc.wrapping = SamplerWrapping::Clamp;
        m_clampedDepthSampler = m_backend.createSampler(desc);
    }    
    //storage buffer for syncing
    {
        BufferDescription desc;
        desc.size = sizeof(uint32_t);
        desc.type = BufferType::Storage;
        desc.initialData = { (uint32_t)0 };
        m_depthPyramidSyncBuffer = m_backend.createStorageBuffer(desc);
    }
}

/*
=========
createLightMatrixPass
=========
*/
void RenderFrontend::createLightMatrixPass() {
    //pass
    {
        ComputePassDescription desc;
        desc.name = "Compute light matrix";
        desc.shaderDescription.srcPathRelative = "lightMatrix.comp";
        m_lightMatrixPass = m_backend.createComputePass(desc);
    }
    //buffer
    {
        BufferDescription desc;
        desc.size = sizeof(glm::mat4);
        desc.type = BufferType::Storage;
        m_sunLightMatrixBuffer = m_backend.createStorageBuffer(desc);
    }
}

/*
=========
createDefaultTextures
=========
*/
void RenderFrontend::createDefaultTextures() {
    {
        ImageDescription defaultDiffuseDesc;
        defaultDiffuseDesc.autoCreateMips = true;
        defaultDiffuseDesc.depth = 1;
        defaultDiffuseDesc.format = ImageFormat::RGBA8;
        defaultDiffuseDesc.initialData = { (char)255, (char)255, (char)255, (char)255 };
        defaultDiffuseDesc.manualMipCount = 1;
        defaultDiffuseDesc.mipCount = MipCount::FullChain;
        defaultDiffuseDesc.type = ImageType::Type2D;
        defaultDiffuseDesc.usageFlags = IMAGE_USAGE_SAMPLED;
        defaultDiffuseDesc.width = 1;
        defaultDiffuseDesc.height = 1;

        m_defaultDiffuseTexture = m_backend.createImage(defaultDiffuseDesc);
    }
    
    {
        ImageDescription defaultSpecularDesc;
        defaultSpecularDesc.autoCreateMips = true;
        defaultSpecularDesc.depth = 1;
        defaultSpecularDesc.format = ImageFormat::RGBA8;
        defaultSpecularDesc.initialData = { (char)0, (char)128, (char)255, (char)0 };
        defaultSpecularDesc.manualMipCount = 1;
        defaultSpecularDesc.mipCount = MipCount::FullChain;
        defaultSpecularDesc.type = ImageType::Type2D;
        defaultSpecularDesc.usageFlags = IMAGE_USAGE_SAMPLED;
        defaultSpecularDesc.width = 1;
        defaultSpecularDesc.height = 1;

        m_defaultSpecularTexture = m_backend.createImage(defaultSpecularDesc);
    }

    {
        ImageDescription defaultNormalDesc;
        defaultNormalDesc.autoCreateMips = true;
        defaultNormalDesc.depth = 1;
        defaultNormalDesc.format = ImageFormat::RG8;
        defaultNormalDesc.initialData = { (char)128, (char)128 };
        defaultNormalDesc.manualMipCount = 1;
        defaultNormalDesc.mipCount = MipCount::FullChain;
        defaultNormalDesc.type = ImageType::Type2D;
        defaultNormalDesc.usageFlags = IMAGE_USAGE_SAMPLED;
        defaultNormalDesc.width = 1;
        defaultNormalDesc.height = 1;

        m_defaultNormalTexture = m_backend.createImage(defaultNormalDesc);
    }

    {
        ImageDescription defaultCubemapDesc;
        defaultCubemapDesc.autoCreateMips = true;
        defaultCubemapDesc.depth = 1;
        defaultCubemapDesc.format = ImageFormat::RGBA8;
        defaultCubemapDesc.initialData = { (char)255, (char)255, (char)255, (char)255 };
        defaultCubemapDesc.manualMipCount = 1;
        defaultCubemapDesc.mipCount = MipCount::FullChain;
        defaultCubemapDesc.type = ImageType::Type2D;
        defaultCubemapDesc.usageFlags = IMAGE_USAGE_SAMPLED;
        defaultCubemapDesc.width = 1;
        defaultCubemapDesc.height = 1;

        m_defaultSkyTexture = m_backend.createImage(defaultCubemapDesc);
    }
}

/*
=========
createDefaultSamplers
=========
*/
void RenderFrontend::createDefaultSamplers() {
    {
        const auto cubeSamplerDesc = SamplerDescription(
            SamplerInterpolation::Linear,
            SamplerWrapping::Clamp,
            false,
            1,
            SamplerBorderColor::Black,
            0);
        m_cubeSampler = m_backend.createSampler(cubeSamplerDesc);
    }

    {
        const auto skySamplerDesc = SamplerDescription(
            SamplerInterpolation::Linear,
            SamplerWrapping::Clamp,
            false,
            0,
            SamplerBorderColor::Black,
            m_skyTextureMipCount);
        m_skySamplerWithMips = m_backend.createSampler(skySamplerDesc);
    }

    {
        const auto texelSamplerDesc = SamplerDescription(
            SamplerInterpolation::Nearest,
            SamplerWrapping::Clamp,
            false,
            0,
            SamplerBorderColor::Black,
            0
        );
        m_defaultTexelSampler = m_backend.createSampler(texelSamplerDesc);
    }
}

/*
=========
updateDepthPyramidShaderDescription
=========
*/
void RenderFrontend::updateDepthPyramidShaderDescription() {

    m_depthPyramidShaderConfig.srcPathRelative = "depthHiZPyramid.comp";

    m_depthPyramidShaderConfig.specialisationConstants.locationIDs.resize(4);
    m_depthPyramidShaderConfig.specialisationConstants.values.resize(4);

    m_depthPyramidShaderConfig.specialisationConstants.locationIDs[0] = m_depthPyramidSpecialisationConstantMipCountIndex;
    m_depthPyramidShaderConfig.specialisationConstants.locationIDs[1] = m_depthPyramidSpecialisationConstantDepthResX;
    m_depthPyramidShaderConfig.specialisationConstants.locationIDs[2] = m_depthPyramidSpecialisationConstantDepthResY;
    m_depthPyramidShaderConfig.specialisationConstants.locationIDs[3] = m_depthPyramidSpecialisationConstantGroupCount;

    const uint32_t depthMipCount = mipCountFromResolution(m_screenWidth / 2, m_screenHeight / 2, 1);
    m_depthPyramidShaderConfig.specialisationConstants.values[0] = depthMipCount;
    m_depthPyramidShaderConfig.specialisationConstants.values[1] = m_screenWidth;
    m_depthPyramidShaderConfig.specialisationConstants.values[2] = m_screenHeight;

    const auto dispatchCount = computeDepthPyramidDispatchCount();
    m_depthPyramidShaderConfig.specialisationConstants.values[3] = dispatchCount.x * dispatchCount.y;
}

/*
=========
computeDepthPyramidDispatchCount
=========
*/
glm::ivec2 RenderFrontend::computeDepthPyramidDispatchCount() {
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
        const uint32_t localThreadGroupExtent = 32 / pow((uint32_t)2, unusedMips);

        //pyramid mip0 is half screen resolution
        count.x = std::ceil((float)m_screenWidth / 2 / localThreadGroupExtent);
        count.y = std::ceil((float)m_screenHeight / 2 / localThreadGroupExtent);

        return count;
    }
}

/*
=========
createSkyCubeMesh
=========
*/
void RenderFrontend::createSkyCubeMesh() {

    MeshDataInternal cubedata;
    cubedata.positions = {
        glm::vec3(-1.f, -1.f, -1.f),
        glm::vec3(1.f, -1.f, -1.f),
        glm::vec3(1.f, 1.f, -1.f),
        glm::vec3(-1.f, 1.f, -1.f),
        glm::vec3(-1.f, -1.f, 1.f),
        glm::vec3(1.f, -1.f, 1.f),
        glm::vec3(1.f, 1.f, 1.f),
        glm::vec3(-1.f, 1.f, 1.f)
    };
    cubedata.indices = {
        0, 1, 3, 3, 1, 2,
        1, 5, 2, 2, 5, 6,
        5, 4, 6, 6, 4, 7,
        4, 0, 7, 7, 0, 3,
        3, 2, 7, 7, 2, 6,
        4, 5, 0, 0, 5, 1
    };
    m_skyCube = m_backend.createMeshes(std::vector<MeshDataInternal> { cubedata }, std::vector<RenderPassHandle>{ m_skyPass })[0];
}

/*
=========
updateSun
=========
*/
void RenderFrontend::updateSun() {

    m_globalShaderInfo.sunDirection = glm::vec4(directionToVector(m_sunDirection), 0.f);

    /*
    set shadow pass matrix
    */
    const glm::mat4 coordinateSystemCorrection = glm::mat4(
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 0.5f, 0.0f,
        0.0f, 0.0f, 0.5f, 1.0f);

    //reference: https://developer.download.nvidia.com/SDK/10.5/opengl/src/cascaded_shadow_maps/doc/cascaded_shadow_maps.pdf
    //z-coordinate is tightly fitted, rendering pass uses depth clamping
    //const glm::mat4 V = glm::lookAt(-glm::vec3(m_globalShaderInfo.sunDirection), glm::vec3(0.f), glm::vec3(0.f, -1.f, 0.f));
    //glm::vec3 maxP = glm::vec3(std::numeric_limits<float>::min());
    //glm::vec3 minP = glm::vec3(std::numeric_limits<float>::max());
    //
    //for (const auto& p : getFrustumPoints(m_cameraFrustum)) {
    //    const glm::vec3 pTransformed = V * glm::vec4(p, 1.f);
    //    minP = glm::min(minP, pTransformed);
    //    maxP = glm::max(maxP, pTransformed);
    //}
    //
    //const glm::vec3 scale = glm::vec3(2.f) / (maxP - minP);
    //const glm::vec3 offset = -0.5f * (maxP + minP) * scale;
    //
    //glm::mat4 P = glm::mat4(1.f);
    //P[0][0] = scale.x;
    //P[1][1] = scale.y;
    //P[2][2] = scale.z;
    //P[3][0] = offset.x;
    //P[3][1] = offset.y;
    //P[3][2] = offset.z;
    //
    //const glm::mat4 L = coordinateSystemCorrection * P * V;
    //m_backend.setViewProjectionMatrix(L, m_shadowPass);
    //
    //m_globalShaderInfo.lightMatrix = L;
}

/*
=========
drawUi
=========
*/
void RenderFrontend::drawUi() {
    ImGui::Begin("Rendering");
    ImGui::Text(("FrameTime: " + std::to_string(m_globalShaderInfo.deltaTime * 1000) + "ms").c_str());
    ImGui::Text(("Main pass drawcalls: " + std::to_string(m_currentFrameMainPassDrawcallCount)).c_str());

    uint32_t allocatedMemorySizeByte;
    uint32_t usedMemorySizeByte;
    m_backend.getMemoryStats(&allocatedMemorySizeByte, &usedMemorySizeByte);

    const float byteToMbDivider = 1048576;
    const float allocatedMemorySizeMegaByte  = allocatedMemorySizeByte   / byteToMbDivider;
    const float usedMemorySizeMegaByte       = usedMemorySizeByte        / byteToMbDivider;

    ImGui::Text(("Allocated memory: " + std::to_string(allocatedMemorySizeMegaByte) + "mb").c_str());
    ImGui::Text(("Used memory: " + std::to_string(usedMemorySizeMegaByte) + "mb").c_str());

    ImGui::DragFloat2("Sun direction", &m_sunDirection.x);
    ImGui::ColorEdit4("Sun color", &m_globalShaderInfo.sunColor.x);
    ImGui::DragFloat("Exposure offset EV", &m_globalShaderInfo.exposureOffset, 0.1f);
    ImGui::DragFloat("Adaption speed EV/s", &m_globalShaderInfo.exposureAdaptionSpeedEvPerSec, 0.1f, 0.f);
    ImGui::InputFloat("Sun Illuminance Lux", &m_globalShaderInfo.sunIlluminanceLux);
    ImGui::InputFloat("Sky Illuminance Lux", &m_globalShaderInfo.skyIlluminanceLux);

    static bool indirectMultiscatterSelection = 
        m_mainPassShaderConfig.fragment.specialisationConstants.values[m_mainPassSpecilisationConstantUseIndirectMultiscatterBRDFIndex] == 1;
    if (ImGui::Checkbox("Indirect Multiscatter BRDF", &indirectMultiscatterSelection)) {
        m_isMainPassShaderDescriptionStale = true;
        m_mainPassShaderConfig.fragment.specialisationConstants.values[m_mainPassSpecilisationConstantUseIndirectMultiscatterBRDFIndex] 
            = indirectMultiscatterSelection ? 1 : 0;
    }

    const char* diffuseBRDFOptions[] = { "Lambert", "Disney", "CoD WWII", "Titanfall 2" };
    const bool diffuseBRDFChanged = ImGui::Combo("Diffuse BRDF", 
        &m_mainPassShaderConfig.fragment.specialisationConstants.values[m_mainPassSpecilisationConstantDiffuseBRDFIndex], 
        diffuseBRDFOptions, 4);
    m_isMainPassShaderDescriptionStale |= diffuseBRDFChanged;
    m_isBRDFLutShaderDescriptionStale = diffuseBRDFChanged;

    const char* directMultiscatterBRDFOptions[] = { "McAuley", "Simplified", "Scaled GGX lobe", "None" };
    m_isMainPassShaderDescriptionStale |= ImGui::Combo("Direct Multiscatter BRDF",
        &m_mainPassShaderConfig.fragment.specialisationConstants.values[m_mainPassSpecilisationConstantDirectMultiscatterBRDFIndex], 
        directMultiscatterBRDFOptions, 4);

    static bool geometricAASelection =
        m_mainPassShaderConfig.fragment.specialisationConstants.values[m_mainPassSpecilisationConstantGeometricAAIndex] == 1;
    if (ImGui::Checkbox("Geometric AA", &geometricAASelection)) {
        m_isMainPassShaderDescriptionStale = true;
        m_mainPassShaderConfig.fragment.specialisationConstants.values[m_mainPassSpecilisationConstantGeometricAAIndex]
            = geometricAASelection ? 1 : 0;
    }

    ImGui::Checkbox("Draw bounding boxes", &m_drawBBs);
    ImGui::Checkbox("Freeze and draw camera frustum", &m_freezeAndDrawCameraFrustum);

    ImGui::End();
}