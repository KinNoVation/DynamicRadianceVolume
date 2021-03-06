#include "renderer.hpp"
#include "voxelization.hpp"
#include "hdrimage.hpp"

#include "../utilities/utils.hpp"

#include "../scene/model.hpp"
#include "../scene/sceneentity.hpp"
#include "../scene/scene.hpp"
#include "../camera/camera.hpp"

#include "../frameprofiler.hpp"

#include <glhelper/samplerobject.hpp>
#include <glhelper/shaderobject.hpp>
#include <glhelper/texture3d.hpp>
#include <glhelper/screenalignedtriangle.hpp>
#include <glhelper/persistentringbuffer.hpp>
#include <glhelper/framebufferobject.hpp>
#include <glhelper/statemanagement.hpp>
#include <glhelper/utils/flagoperators.hpp>

#include <limits>


Renderer::Renderer(const std::shared_ptr<const Scene>& scene, const ei::UVec2& resolution) :
	m_samplerLinearRepeat(gl::SamplerObject::GetSamplerObject(gl::SamplerObject::Desc(gl::SamplerObject::Filter::LINEAR, gl::SamplerObject::Filter::LINEAR, gl::SamplerObject::Filter::LINEAR,
															gl::SamplerObject::Border::REPEAT))),
	m_samplerLinearClamp(gl::SamplerObject::GetSamplerObject(gl::SamplerObject::Desc(gl::SamplerObject::Filter::LINEAR, gl::SamplerObject::Filter::LINEAR, gl::SamplerObject::Filter::LINEAR,
															gl::SamplerObject::Border::CLAMP))),
	m_samplerNearest(gl::SamplerObject::GetSamplerObject(gl::SamplerObject::Desc(gl::SamplerObject::Filter::NEAREST, gl::SamplerObject::Filter::NEAREST, gl::SamplerObject::Filter::NEAREST, 
															gl::SamplerObject::Border::REPEAT))),
	m_samplerShadow(gl::SamplerObject::GetSamplerObject(gl::SamplerObject::Desc(gl::SamplerObject::Filter::LINEAR, gl::SamplerObject::Filter::LINEAR, gl::SamplerObject::Filter::NEAREST, 
															gl::SamplerObject::Border::BORDER, 1, gl::Vec4(0.0), gl::SamplerObject::CompareMode::GREATER))),

	m_readLightCacheCount(false),
	m_lastNumLightCaches(0),
	m_tonemapExposure(1.0f),
	m_tonemapLMax(1.2f),
	m_mode(Renderer::Mode::DYN_RADIANCE_VOLUME),
	m_indirectDiffuseMode(IndirectDiffuseMode::SH1),

	m_specularEnvmapPerCacheSize(16),
	m_specularEnvmapMaxFillHolesLevel(0),
	m_specularEnvmapDirectWrite(true),
	m_showCAVCascades(false),
	m_CAVCascadeTransitionSize(2.0f),
	m_indirectShadow(true),
	m_indirectSpecular(false),

	m_passedTime(0.0f)
{
	glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &m_UBOAlignment);
	LOG_INFO("Uniform buffer alignment is " << m_UBOAlignment);

	m_screenTriangle = std::make_unique<gl::ScreenAlignedTriangle>();

	LoadAllShaders();
	
	// Init global ubos.
	gl::ShaderObject* uboPrototypeShader = m_shaderLightCachesRSM.get();

	m_uboInfoConstant = uboPrototypeShader->GetUniformBufferInfo()["Constant"];
	m_uboConstant = std::make_unique<gl::Buffer>(m_uboInfoConstant.bufferDataSizeByte, gl::Buffer::MAP_WRITE);
	uboPrototypeShader->BindUBO(*m_uboConstant, "Constant");

	m_uboInfoPerFrame = uboPrototypeShader->GetUniformBufferInfo()["PerFrame"];
	m_uboPerFrame = std::make_unique<gl::Buffer>(m_uboInfoPerFrame.bufferDataSizeByte, gl::Buffer::MAP_WRITE);
	uboPrototypeShader->BindUBO(*m_uboPerFrame, "PerFrame");

	m_uboInfoVolumeInfo = uboPrototypeShader->GetUniformBufferInfo()["VolumeInfo"];
	m_uboVolumeInfo = std::make_unique<gl::Buffer>(m_uboInfoVolumeInfo.bufferDataSizeByte, gl::Buffer::MAP_WRITE);
	uboPrototypeShader->BindUBO(*m_uboVolumeInfo, "VolumeInfo");

	// Expecting about 16 objects.
	const unsigned int maxExpectedObjects = 16;
	m_uboInfoPerObject = uboPrototypeShader->GetUniformBufferInfo()["PerObject"];
	m_uboRing_PerObject = std::make_unique<gl::PersistentRingBuffer>(maxExpectedObjects * RoundSizeToUBOAlignment(m_uboInfoPerObject.bufferDataSizeByte) * 3);

	// Light UBO.
	const unsigned int maxExpectedLights = 16;
	m_uboInfoSpotLight = uboPrototypeShader->GetUniformBufferInfo()["SpotLight"];
	m_uboRing_SpotLight = std::make_unique<gl::PersistentRingBuffer>(maxExpectedLights * RoundSizeToUBOAlignment(m_uboInfoSpotLight.bufferDataSizeByte) * 3);

	// Create voxelization module.
	m_voxelization = std::make_unique<Voxelization>(128);

	// Allocate light cache buffer
	SetMaxCacheCount(16384);
	SetCAVCascades(3, 32);

	// Basic settings.
	SetScene(scene);
	OnScreenResize(resolution);

	// For cache debugging
	m_cacheDebugIndirectDrawBuffer = std::make_unique<gl::Buffer>(sizeof(std::uint32_t) * 5, gl::Buffer::IMMUTABLE);
	m_debugSphereModel = Model::FromFile("../models/sphere.json");

	// General GL settings
	gl::Enable(gl::Cap::DEPTH_TEST);
	gl::Disable(gl::Cap::DITHER);

	//gl::Enable(gl::Cap::CULL_FACE);
	//GL_CALL(glFrontFace, GL_CW);

	// A quick note on depth:
	// http://www.gamedev.net/topic/568014-linear-or-non-linear-shadow-maps/#entry4633140
	// - Outputting depth manually (separate target or gl_FragDepth) can hurt performance in several ways
	// -> need to use real depthbuffer
	// --> precision issues
	// --> better precision with flipped depth test + R32F depthbuffers
	gl::SetDepthFunc(gl::DepthFunc::GREATER);
	GL_CALL(glClearDepth, 0.0f);

	// The OpenGL clip space convention uses depth -1 to 1 which is remapped again. In GL4.5 it is possible to disable this
	GL_CALL(glClipControl, GL_LOWER_LEFT, GL_ZERO_TO_ONE);

	GL_CALL(glBlendFunc, GL_ONE, GL_ONE);
}

Renderer::~Renderer()
{
}

void Renderer::LoadAllShaders()
{
	m_shaderDebugGBuffer = new gl::ShaderObject("gbuffer debug");
	m_shaderDebugGBuffer->AddShaderFromFile(gl::ShaderObject::ShaderType::VERTEX, "shader/screenTri.vert");
	m_shaderDebugGBuffer->AddShaderFromFile(gl::ShaderObject::ShaderType::FRAGMENT, "shader/debuggbuffer.frag");
	m_shaderDebugGBuffer->CreateProgram();

	for (int i = 0; i < 2; ++i)
	{
		std::string postfix = (i == (int)ShaderAlphaTest::OFF) ? " - no alphatest" : " - alphatest";
		std::string define = (i == (int)ShaderAlphaTest::OFF) ? "" : "#define ALPHATESTING 0.1";

		m_shaderFillGBuffer[i] = new gl::ShaderObject("fill gbuffer" + postfix);
		m_shaderFillGBuffer[i]->AddShaderFromFile(gl::ShaderObject::ShaderType::VERTEX, "shader/defaultmodel.vert", define);
		m_shaderFillGBuffer[i]->AddShaderFromFile(gl::ShaderObject::ShaderType::FRAGMENT, "shader/fillgbuffer.frag", define);
		m_shaderFillGBuffer[i]->CreateProgram();

		m_shaderFillRSM[i] = new gl::ShaderObject("fill rsm" + postfix);
		m_shaderFillRSM[i]->AddShaderFromFile(gl::ShaderObject::ShaderType::VERTEX, "shader/defaultmodel_rsm.vert", define);
		m_shaderFillRSM[i]->AddShaderFromFile(gl::ShaderObject::ShaderType::FRAGMENT, "shader/fillrsm.frag", define);
		m_shaderFillRSM[i]->CreateProgram();
	}

	m_shaderDeferredDirectLighting_Spot = new gl::ShaderObject("direct lighting - spot");
	m_shaderDeferredDirectLighting_Spot->AddShaderFromFile(gl::ShaderObject::ShaderType::VERTEX, "shader/screenTri.vert");
	m_shaderDeferredDirectLighting_Spot->AddShaderFromFile(gl::ShaderObject::ShaderType::FRAGMENT, "shader/directdeferredlighting.frag");
	m_shaderDeferredDirectLighting_Spot->CreateProgram();

	m_shaderTonemap = new gl::ShaderObject("texture output");
	m_shaderTonemap->AddShaderFromFile(gl::ShaderObject::ShaderType::VERTEX, "shader/screenTri.vert");
	m_shaderTonemap->AddShaderFromFile(gl::ShaderObject::ShaderType::FRAGMENT, "shader/tonemapping.frag");
	m_shaderTonemap->CreateProgram();
	m_shaderTonemap->Activate();
	SetTonemapLMax(m_tonemapLMax);
	SetExposure(m_tonemapExposure);

	/*m_shaderLightCachesDirect = new gl::ShaderObject("cache lighting direct");
	m_shaderLightCachesDirect->AddShaderFromFile(gl::ShaderObject::ShaderType::COMPUTE, "shader/cacheLightingDirect.comp");
	m_shaderLightCachesDirect->CreateProgram();*/

	m_shaderIndirectLightingBruteForceRSM = new gl::ShaderObject("brute force rsm");
	m_shaderIndirectLightingBruteForceRSM->AddShaderFromFile(gl::ShaderObject::ShaderType::VERTEX, "shader/screenTri.vert");
	m_shaderIndirectLightingBruteForceRSM->AddShaderFromFile(gl::ShaderObject::ShaderType::FRAGMENT, "shader/bruteforcersm.frag");
	m_shaderIndirectLightingBruteForceRSM->CreateProgram();

	m_shaderConeTraceAO = new gl::ShaderObject("cone trace ao caches");
	m_shaderConeTraceAO->AddShaderFromFile(gl::ShaderObject::ShaderType::VERTEX, "shader/screenTri.vert");
	m_shaderConeTraceAO->AddShaderFromFile(gl::ShaderObject::ShaderType::FRAGMENT, "shader/ambientocclusion.frag");
	m_shaderConeTraceAO->CreateProgram();
	
	m_shaderLightCachePrepare = new gl::ShaderObject("cache lighting prepare");
	m_shaderLightCachePrepare->AddShaderFromFile(gl::ShaderObject::ShaderType::COMPUTE, "shader/cachePrepareLighting.comp");
	m_shaderLightCachePrepare->CreateProgram();

	m_shaderSpecularEnvmapMipMap = new gl::ShaderObject("specular envmap mipmap");
	m_shaderSpecularEnvmapMipMap->AddShaderFromFile(gl::ShaderObject::ShaderType::VERTEX, "shader/specularenvmap.vert");
	m_shaderSpecularEnvmapMipMap->AddShaderFromFile(gl::ShaderObject::ShaderType::FRAGMENT, "shader/specularenvmap_mipmap.frag");
	m_shaderSpecularEnvmapMipMap->CreateProgram();

	m_shaderSpecularEnvmapFillHoles = new gl::ShaderObject("specular fill holes");
	m_shaderSpecularEnvmapFillHoles->AddShaderFromFile(gl::ShaderObject::ShaderType::VERTEX, "shader/specularenvmap.vert");
	m_shaderSpecularEnvmapFillHoles->AddShaderFromFile(gl::ShaderObject::ShaderType::FRAGMENT, "shader/specularenvmap_fillholes.frag");
	m_shaderSpecularEnvmapFillHoles->CreateProgram();

	m_shaderCacheDebug_Prepare = new gl::ShaderObject("prepare cache debug");
	m_shaderCacheDebug_Prepare->AddShaderFromFile(gl::ShaderObject::ShaderType::COMPUTE, "shader/cachedebug/prepareindirectdrawbuffer.comp");
	m_shaderCacheDebug_Prepare->CreateProgram();

	ReloadLightingSettingDependentCacheShader();
}

void Renderer::ReloadLightingSettingDependentCacheShader()
{
	std::string settings;
	if (m_indirectSpecular)
	{
		settings = "#define INDIRECT_SPECULAR\n"
			"#define SPECULARENVMAP_PERCACHESIZE " + std::to_string(m_specularEnvmapPerCacheSize) + "\n";

		if (m_specularEnvmapDirectWrite)
			settings += "#define DIRECT_SPECULAR_MAP_WRITE\n";
	}
	if (m_indirectShadow)
		settings += "#define INDIRECT_SHADOW\n";
	if (m_showCAVCascades)
		settings += "#define SHOW_ADDRESSVOL_CASCADES\n";
	if (m_CAVCascadeTransitionSize > 0.0f)
		settings += "#define ADDRESSVOL_CASCADE_TRANSITIONS\n";
	
	switch (m_indirectDiffuseMode)
	{
	case IndirectDiffuseMode::SH1:
		settings += "#define INDDIFFUSE_VIA_SH1\n";
		break;
	case IndirectDiffuseMode::SH2:
		settings += "#define INDDIFFUSE_VIA_SH2\n";
		break;
	default:
		LOG_ERROR("Given indirect diffuse mode not implemented yet!");
	}


	m_shaderCacheGather = new gl::ShaderObject("cache gather");
	m_shaderCacheGather->AddShaderFromFile(gl::ShaderObject::ShaderType::COMPUTE, "shader/cacheGather.comp", settings);
	m_shaderCacheGather->CreateProgram();

	m_shaderLightCachesRSM = new gl::ShaderObject("cache lighting rsm");
	m_shaderLightCachesRSM->AddShaderFromFile(gl::ShaderObject::ShaderType::COMPUTE, "shader/cacheLightingRSM.comp", settings);
	m_shaderLightCachesRSM->CreateProgram();

	m_shaderCacheApply = new gl::ShaderObject("apply caches");
	m_shaderCacheApply->AddShaderFromFile(gl::ShaderObject::ShaderType::VERTEX, "shader/screenTri.vert");
	m_shaderCacheApply->AddShaderFromFile(gl::ShaderObject::ShaderType::FRAGMENT, "shader/cacheApply.frag", settings);
	m_shaderCacheApply->CreateProgram();

	m_shaderCacheDebug_Render = new gl::ShaderObject("cache debug render");
	m_shaderCacheDebug_Render->AddShaderFromFile(gl::ShaderObject::ShaderType::VERTEX, "shader/cachedebug/sphere.vert", settings);
	m_shaderCacheDebug_Render->AddShaderFromFile(gl::ShaderObject::ShaderType::FRAGMENT, "shader/cachedebug/sphere.frag", settings);
	m_shaderCacheDebug_Render->CreateProgram();

}

void Renderer::AllocateCacheData()
{
	int maxTextureSize;
	glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTextureSize);

	int demandedSpecularEnvMapSize = static_cast<int>(pow(2, ceil(log2( ceil(sqrt(m_maxNumLightCaches)) * m_specularEnvmapPerCacheSize ))));

	// Specularenvmap to large?
	if (demandedSpecularEnvMapSize > maxTextureSize)
	{
		unsigned int adjustedCacheCount = static_cast<unsigned int>(pow(maxTextureSize / m_specularEnvmapPerCacheSize, 2));
		LOG_WARNING(m_maxNumLightCaches << " caches at a specular envmap size of " << m_specularEnvmapPerCacheSize << " per cache would lead to a texture size of at least " <<
					demandedSpecularEnvMapSize << ". Maximum texture size is " << maxTextureSize << ". Falling back to lower maximum cache count: " << adjustedCacheCount);
		adjustedCacheCount = m_maxNumLightCaches;
		demandedSpecularEnvMapSize = maxTextureSize;
	}

	// Maximum size per cache
	const unsigned int lightCacheSizeInBytes = sizeof(float) * 4 * 8;

	// Allocate cache buffer.
	unsigned int cacheBufferSizeInBytes = m_maxNumLightCaches * lightCacheSizeInBytes;
	if (!m_lightCacheBuffer || m_lightCacheBuffer->GetSize() != cacheBufferSizeInBytes)
	{
		m_lightCacheBuffer = std::make_unique<gl::Buffer>(cacheBufferSizeInBytes, gl::Buffer::IMMUTABLE, nullptr);
		SetReadLightCacheCount(false); // (Re)creates the lightcache buffer

		LOG_INFO("Allocated " << cacheBufferSizeInBytes/1024 << " kb cache buffer.");
	}

	// Allocate specular cache envmap
	if (!m_specularEnvmap || m_specularEnvmapFBOs.empty() || m_specularEnvmap->GetWidth() != demandedSpecularEnvMapSize)
	{
		m_specularEnvmapFBOs.clear();
		m_specularEnvmap = std::make_unique<gl::Texture2D>(demandedSpecularEnvMapSize, demandedSpecularEnvMapSize, gl::TextureFormat::R11F_G11F_B10F, static_cast<int>(log2(m_specularEnvmapPerCacheSize) + 1));
		for (int i = 0; i < m_specularEnvmap->GetNumMipLevels(); ++i)
			m_specularEnvmapFBOs.push_back(std::make_shared<gl::FramebufferObject>(gl::FramebufferObject::Attachment(m_specularEnvmap.get(), i)));

		LOG_INFO("Allocated specular envmap with total size " << demandedSpecularEnvMapSize << "x" << demandedSpecularEnvMapSize << " (" << (demandedSpecularEnvMapSize * demandedSpecularEnvMapSize * 4) / 1024 << " kb)");
	}
}

void Renderer::UpdateConstantUBO()
{
	if (!m_HDRBackbufferTexture) return;

	gl::MappedUBOView mappedMemory(m_uboInfoConstant, m_uboConstant->Map(gl::Buffer::MapType::WRITE, gl::Buffer::MapWriteFlag::INVALIDATE_BUFFER));

	mappedMemory["ShCosLobeFactor0"].Set(sqrtf(ei::PI) / 2.0f);
	mappedMemory["ShCosLobeFactor1"].Set(sqrtf(ei::PI / 3.0f));
	mappedMemory["ShCosLobeFactor2n2_p1_n1"].Set(-sqrtf(15.0f * ei::PI) / 8.0f);
	mappedMemory["ShCosLobeFactor20"].Set(sqrtf(5.0f * ei::PI) / 16.0f);
	mappedMemory["ShCosLobeFactor2p2"].Set(sqrtf(15.0f * ei::PI) / 16.0f);

	mappedMemory["ShEvaFactor0"].Set(1.0f / (2.0f * sqrt(ei::PI)));
	mappedMemory["ShEvaFactor1"].Set(sqrtf(3.0f) / (2.0f * sqrt(ei::PI)));
	mappedMemory["ShEvaFactor2n2_p1_n1"].Set(sqrtf(15.0f / (4.0f * ei::PI)));
	mappedMemory["ShEvaFactor20"].Set(sqrtf(5.0f / (16.0f * ei::PI)));
	mappedMemory["ShEvaFactor2p2"].Set(sqrtf(15.0f / (16.0f * ei::PI)));

	mappedMemory["BackbufferResolution"].Set(ei::IVec2(m_HDRBackbufferTexture->GetWidth(), m_HDRBackbufferTexture->GetHeight()));

	mappedMemory["VoxelResolution"].Set(m_voxelization->GetVoxelTexture().GetWidth());
	mappedMemory["AddressVolumeResolution"].Set(static_cast<int>(GetCAVResolution()));
	mappedMemory["NumAddressVolumeCascades"].Set(static_cast<int>(GetCAVCascadeCount()));

	mappedMemory["MaxNumLightCaches"].Set(m_maxNumLightCaches);
	
	mappedMemory["SpecularEnvmapTotalSize"].Set(m_specularEnvmap->GetWidth());
	mappedMemory["SpecularEnvmapPerCacheSize_Texel"].Set(static_cast<int>(m_specularEnvmapPerCacheSize));
	mappedMemory["SpecularEnvmapPerCacheSize_Texcoord"].Set(static_cast<float>(m_specularEnvmapPerCacheSize) / m_specularEnvmap->GetWidth());
	mappedMemory["SpecularEnvmapNumCachesPerDimension"].Set(static_cast<int>(m_specularEnvmap->GetWidth() / m_specularEnvmapPerCacheSize));

	m_uboConstant->Unmap();
}

void Renderer::UpdatePerFrameUBO(const Camera& camera)
{
	auto view = camera.ComputeViewMatrix();
	auto projection = camera.ComputeProjectionMatrix();
	auto viewProjection = projection * view;

	gl::MappedUBOView mappedMemory(m_uboInfoPerFrame, m_uboPerFrame->Map(gl::Buffer::MapType::WRITE, gl::Buffer::MapWriteFlag::INVALIDATE_BUFFER));

	mappedMemory["Projection"].Set(projection);
	//mappedMemory["View"].Set(view);
	mappedMemory["ViewProjection"].Set(viewProjection);
	mappedMemory["InverseView"].Set(ei::invert(view));
	mappedMemory["InverseViewProjection"].Set(ei::invert(viewProjection));
	mappedMemory["CameraPosition"].Set(camera.GetPosition());
	mappedMemory["CameraDirection"].Set(camera.GetDirection());
	mappedMemory["PassedTime"].Set(m_passedTime);//static_cast<float>(ezTime::Now().GetSeconds()));
	


	m_uboPerFrame->Unmap();
}

void Renderer::UpdateVolumeUBO(const Camera& camera)
{
	gl::MappedUBOView mappedMemory(m_uboInfoVolumeInfo, m_uboVolumeInfo->Map(gl::Buffer::MapType::WRITE, gl::Buffer::MapWriteFlag::INVALIDATE_BUFFER));

	// Voxel volume
	ei::Vec3 VolumeWorldMin(m_scene->GetBoundingBox().min - 0.001f);
	ei::Vec3 VolumeWorldMax(m_scene->GetBoundingBox().max + 0.001f);

	ei::Vec3 extent = VolumeWorldMax - VolumeWorldMin;
	float largestExtent = ei::max(extent);
	VolumeWorldMax += ei::Vec3(largestExtent) - extent;
	mappedMemory["VolumeWorldMin"].Set(VolumeWorldMin);
	mappedMemory["VoxelSizeInWorld"].Set((VolumeWorldMax.x - VolumeWorldMin.x) / m_voxelization->GetVoxelTexture().GetWidth());
	mappedMemory["VolumeWorldMax"].Set(VolumeWorldMax);
	
	mappedMemory["CAVTransitionZoneSize"].Set(m_CAVCascadeTransitionSize);
	
	//auto inverseViewProjection = ei::invert(camera.ComputeProjectionMatrix() * camera.ComputeViewMatrix());

	// Cache address volume cascades.
	for (int i = 0; i < m_CAVCascadeWorldSize.size(); ++i)
	{
		float cascadeVoxelSize = m_CAVCascadeWorldSize[i] / GetCAVResolution();

		// Simple version
		
		// Centered around camera
		ei::Vec3 snappedCamera = ei::round(camera.GetPosition() / cascadeVoxelSize) * cascadeVoxelSize;
		ei::Vec3 min = snappedCamera - m_CAVCascadeWorldSize[i] * 0.5f;
		ei::Vec3 max = snappedCamera + m_CAVCascadeWorldSize[i] * 0.5f;

		// Two offsets: 0.5 for assure that there will always be 8 voxels, 1.0 to assure that area is still within the actual min/max above.
		ei::Vec3 decisionMin = camera.GetPosition() - m_CAVCascadeWorldSize[i] * 0.5f + cascadeVoxelSize * 1.5f;
		ei::Vec3 decisionMax = camera.GetPosition() + m_CAVCascadeWorldSize[i] * 0.5f - cascadeVoxelSize * 1.5f;


		// Clever version
		
		// Box around the entire frustum
	/*	ei::Vec3 frustumPoints[4];
		frustumPoints[0] = ei::transform(ei::Vec3(1.0f, 1.0f, 0.0f), inverseViewProjection);
		frustumPoints[1] = ei::transform(ei::Vec3(-1.0f, 1.0f, 0.0f), inverseViewProjection);
		frustumPoints[2] = ei::transform(ei::Vec3(1.0f, -1.0f, 0.0f), inverseViewProjection);
		frustumPoints[3] = ei::transform(ei::Vec3(-1.0f, -1.0f, 0.0f), inverseViewProjection);
		ei::Box frustumBox(camera.GetPosition(), camera.GetPosition());
		for (int j = 0; j < 4; ++j)
		{
			frustumBox.min = ei::min(frustumBox.min, frustumPoints[j]);
			frustumBox.max = ei::max(frustumBox.max, frustumPoints[j]);
		}

		// Resize largest extend to axis
		float largestFrustumBoxExtend = ei::max(frustumBox.max - frustumBox.min);
		float frustumBoxScale = m_CAVCascadeWorldSize[i] / largestFrustumBoxExtend;
		frustumBox.min = (frustumBox.min - camera.GetPosition()) * frustumBoxScale + camera.GetPosition();
		frustumBox.max = (frustumBox.max - camera.GetPosition()) * frustumBoxScale + camera.GetPosition();

		// Extend to cube along allowed axis
		ei::Vec3 extensionDirs = ei::sign(camera.GetDirection());
		for (int j = 0; j < 3; ++j)
		{
			if (extensionDirs[j] < 0.0)
				frustumBox.min[j] = frustumBox.max[j] - m_CAVCascadeWorldSize[i];
			else
				frustumBox.max[j] = m_CAVCascadeWorldSize[i] - frustumBox.min[j];
		}

		// Generate snapped version.
		ei::Vec3 snap = ei::round(camera.GetPosition() / cascadeVoxelSize) * cascadeVoxelSize - camera.GetPosition();
		ei::Vec3 min = frustumBox.min + snap;
		ei::Vec3 max = frustumBox.max + snap;

		// Generate decision box.
		ei::Vec3 decisionMin = frustumBox.min + cascadeVoxelSize * 1.5f;
		ei::Vec3 decisionMax = frustumBox.max * 1.5f; */

		std::string num = std::to_string(i);
		mappedMemory["AddressVolumeCascades[" + num + "].Min"].Set(min);
		mappedMemory["AddressVolumeCascades[" + num + "].WorldVoxelSize"].Set(cascadeVoxelSize);
		mappedMemory["AddressVolumeCascades[" + num + "].Max"].Set(max);
		mappedMemory["AddressVolumeCascades[" + num + "].DecisionMin"].Set(decisionMin);
		mappedMemory["AddressVolumeCascades[" + num + "].DecisionMax"].Set(decisionMax);
	}

	m_uboVolumeInfo->Unmap();
}

void Renderer::SetVoxelVolumeResultion(unsigned int resolution)
{
	m_voxelization->SetResolution(resolution);
	UpdateConstantUBO();
}

unsigned int Renderer::GetVoxelVolumeResultion() const
{
	return m_voxelization->GetResolution();
}

void Renderer::SetVoxelVolumeAdaptionRate(float adaptionRate)
{
	m_voxelization->SetAdaptionRate(adaptionRate);
}

float Renderer::GetVoxelVolumeAdaptionRate() const
{
	return m_voxelization->GetAdaptionRate();
}

void Renderer::SetPerCacheSpecularEnvMapSize(unsigned int specularEnvmapPerCacheSize)
{
	Assert(IsPowerOfTwo(specularEnvmapPerCacheSize), "Per cache specular envmap size needs to be a power of two!");

	m_specularEnvmapPerCacheSize = specularEnvmapPerCacheSize;
	m_specularEnvmapMaxFillHolesLevel = std::min(m_specularEnvmapMaxFillHolesLevel, static_cast<unsigned int>(log2(m_specularEnvmapPerCacheSize)));

	AllocateCacheData();
	ReloadLightingSettingDependentCacheShader();
	UpdateConstantUBO();
}

void Renderer::OnScreenResize(const ei::UVec2& newResolution)
{
	m_GBuffer_diffuse = std::make_unique<gl::Texture2D>(newResolution.x, newResolution.y, gl::TextureFormat::SRGB8, 1, 0);
	m_GBuffer_roughnessMetallic = std::make_unique<gl::Texture2D>(newResolution.x, newResolution.y, gl::TextureFormat::RG8, 1, 0);
	m_GBuffer_normal = std::make_unique<gl::Texture2D>(newResolution.x, newResolution.y, gl::TextureFormat::RG16I, 1, 0);
	m_GBuffer_depth = std::make_unique<gl::Texture2D>(newResolution.x, newResolution.y, gl::TextureFormat::DEPTH_COMPONENT32F, 1, 0);

	// Render to snorm integer makes problems.
	// Others seem to have this problem too http://www.gamedev.net/topic/657167-opengl-44-render-to-snorm/

	m_GBuffer.reset(new gl::FramebufferObject({ gl::FramebufferObject::Attachment(m_GBuffer_diffuse.get()), gl::FramebufferObject::Attachment(m_GBuffer_normal.get()), gl::FramebufferObject::Attachment(m_GBuffer_roughnessMetallic.get()) },
									gl::FramebufferObject::Attachment(m_GBuffer_depth.get())));


	m_HDRBackbufferTexture = std::make_unique<gl::Texture2D>(newResolution.x, newResolution.y, gl::TextureFormat::RGBA16F, 1, 0);
	m_HDRBackbuffer.reset(new gl::FramebufferObject(gl::FramebufferObject::Attachment(m_HDRBackbufferTexture.get())));

	m_HDRBackbufferWithGBufferDepth.reset(new gl::FramebufferObject(gl::FramebufferObject::Attachment(m_HDRBackbufferTexture.get()), gl::FramebufferObject::Attachment(m_GBuffer_depth.get())));

	GL_CALL(glViewport, 0, 0, newResolution.x, newResolution.y);


	UpdateConstantUBO();
}

void Renderer::SetScene(const std::shared_ptr<const Scene>& scene)
{
	Assert(scene.get(), "Scene pointer is null!");

	m_scene = scene;

	if(m_HDRBackbufferTexture)
		UpdateConstantUBO();
}

void Renderer::Draw(const Camera& camera, bool detachViewFromCameraUpdate, float timeSinceLastFrame)
{
	m_passedTime += timeSinceLastFrame;

	// All SRGB frame buffer textures should be do a conversion on writing to them.
	// This also applies to the backbuffer.
	gl::Enable(gl::Cap::FRAMEBUFFER_SRGB);

	// Update data.
	//PROFILE_GPU_START(PrepareUBOs)
	UpdatePerFrameUBO(camera);
	if (!detachViewFromCameraUpdate)
		UpdateVolumeUBO(camera);
	UpdatePerObjectUBORingBuffer();
	PrepareLights();
	//PROFILE_GPU_END()

	// Scene dependent renderings.
	DrawSceneToGBuffer();
	DrawShadowMaps();

	switch (m_mode)
	{
	case Mode::RSM_BRUTEFORCE:
		m_uboRing_PerObject->CompleteFrame();

		m_HDRBackbuffer->Bind(true);
		GL_CALL(glClear, GL_COLOR_BUFFER_BIT);
		ApplyDirectLighting();

		ApplyRSMsBruteForce();

		m_uboRing_SpotLight->CompleteFrame();

		OutputHDRTextureToBackbuffer();
		break;

	//case Mode::DIRECTONLY_CACHE:
	case Mode::DYN_RADIANCE_VOLUME_DEBUG:
	case Mode::DYN_RADIANCE_VOLUME:
		//m_lightCacheBuffer->ClearToZero(); // As of 19.09.2015 the Nvidia driver can apparently glClearNamedBufferData AND glClearNamedBufferSubData do only if the buffer is not too large.

		if (m_indirectShadow)
			m_voxelization->VoxelizeScene(*this);

		m_uboRing_PerObject->CompleteFrame();

		if (!detachViewFromCameraUpdate)
		{
			AllocateCaches();

			/*	if (m_mode == Mode::DIRECTONLY_CACHE)
					LightCachesDirect();
				else */
			{
				LightCachesRSM();
				if (m_indirectSpecular)
					PrepareSpecularEnvmaps();
			}
		}

		m_HDRBackbuffer->Bind(true);
		GL_CALL(glClear, GL_COLOR_BUFFER_BIT);

		//if (m_mode != Mode::DIRECTONLY_CACHE)
			ApplyDirectLighting();

		m_uboRing_SpotLight->CompleteFrame();

		ApplyCaches();

		if (m_mode == Mode::DYN_RADIANCE_VOLUME_DEBUG && m_debugSphereModel)
		{
			m_lightCacheBuffer->BindShaderStorageBuffer(0);
			m_lightCacheCounter->BindShaderStorageBuffer(1);
			m_cacheDebugIndirectDrawBuffer->BindShaderStorageBuffer(4);

			m_shaderCacheDebug_Prepare->Activate();
			GL_CALL(glDispatchCompute, 1, 1, 1);

			gl::Enable(gl::Cap::CULL_FACE);
			gl::Enable(gl::Cap::DEPTH_TEST);
			gl::SetDepthWrite(true);
			m_HDRBackbufferWithGBufferDepth->Bind(false);
			m_debugSphereModel->BindVAO();
			m_debugSphereModel->BindBuffers();
			GL_CALL(glMemoryBarrier, GL_COMMAND_BARRIER_BIT);
			m_cacheDebugIndirectDrawBuffer->BindIndirectDrawBuffer();
			m_shaderCacheDebug_Render->Activate();
			glDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_INT, nullptr);
		}

		OutputHDRTextureToBackbuffer();
		break;


	case Mode::DIRECTONLY:
		m_uboRing_PerObject->CompleteFrame();

		m_HDRBackbuffer->Bind(true);
		GL_CALL(glClear, GL_COLOR_BUFFER_BIT);
		ApplyDirectLighting();

		m_uboRing_SpotLight->CompleteFrame();

		OutputHDRTextureToBackbuffer();
		break;


	case Mode::GBUFFER_DEBUG:
		m_uboRing_PerObject->CompleteFrame();
		m_uboRing_SpotLight->CompleteFrame();

		DrawGBufferDebug();
		break;


	case Mode::VOXELVIS:
		m_uboRing_SpotLight->CompleteFrame();

		m_voxelization->VoxelizeScene(*this);
		
		m_uboRing_PerObject->CompleteFrame();

		GL_CALL(glViewport, 0, 0, m_HDRBackbufferTexture->GetWidth(), m_HDRBackbufferTexture->GetHeight());
		m_voxelization->DrawVoxelRepresentation();
		break;

	case Mode::AMBIENTOCCLUSION:
		m_uboRing_SpotLight->CompleteFrame();

		m_voxelization->VoxelizeScene(*this);

		m_uboRing_PerObject->CompleteFrame();

		m_HDRBackbuffer->Bind(true);
		GL_CALL(glClear, GL_COLOR_BUFFER_BIT);
		ConeTraceAO();
		OutputHDRTextureToBackbuffer();
		break;
	}

	// Turn SRGB conversions off, since ui will look odd otherwise.
	gl::Disable(gl::Cap::FRAMEBUFFER_SRGB);
}

void Renderer::UpdatePerObjectUBORingBuffer()
{
	for (unsigned int entityIndex = 0; entityIndex < m_scene->GetEntities().size(); ++entityIndex)
	{
		const SceneEntity& entity = m_scene->GetEntities()[entityIndex];
		
		void* blockMemory = nullptr;
		size_t blockIndex = 0;	
		m_uboRing_PerObject->AddBlock(blockMemory, blockIndex, sizeof(ei::Mat4x4), m_UBOAlignment);
		Assert(blockIndex == entityIndex, "Entity index and memory block index are different.");

		auto worldMatrix = entity.ComputeWorldMatrix();
		memcpy(blockMemory, &worldMatrix, sizeof(ei::Mat4x4));
	}
	m_uboRing_PerObject->FlushAllBlocks();
}

void Renderer::PrepareLights()
{
	m_shadowMaps.resize(m_scene->GetLights().size());

	for (unsigned int lightIndex = 0; lightIndex < m_scene->GetLights().size(); ++lightIndex)
	{
		const Light& light = m_scene->GetLights()[lightIndex];
		Assert(light.type == Light::Type::SPOT, "Only spot lights are supported so far!");

		void* blockMemory = nullptr;
		size_t blockIndex = 0;
		m_uboRing_SpotLight->AddBlock(blockMemory, blockIndex, m_uboInfoSpotLight.bufferDataSizeByte, m_UBOAlignment);
		Assert(blockIndex == lightIndex, "Light index and memory block index are different.");

		gl::MappedUBOView uboView(m_uboInfoSpotLight, blockMemory);
		uboView["LightIntensity"].Set(light.intensity);
		uboView["ShadowNormalOffset"].Set(light.normalOffsetShadowBias);
		uboView["ShadowBias"].Set(light.shadowBias);
		
		uboView["LightPosition"].Set(light.position);
		uboView["LightDirection"].Set(ei::normalize(light.direction));
		uboView["LightCosHalfAngle"].Set(cosf(light.halfAngle));

		ei::Mat4x4 view = ei::camera(light.position, light.position + light.direction);
		ei::Mat4x4 projection = ei::perspectiveDX(light.halfAngle * 2.0f, 1.0f, light.farPlane, light.nearPlane); // far and near intentionally swapped!
		ei::Mat4x4 viewProjection = projection * view;
		ei::Mat4x4 inverseViewProjection = ei::invert(viewProjection);
		uboView["LightViewProjection"].Set(viewProjection);
		uboView["InverseLightViewProjection"].Set(inverseViewProjection);

		int shadowMapResolution_nextPow2 = 1 << static_cast<int>(ceil(log2(light.rsmResolution)));
		if (shadowMapResolution_nextPow2 != light.rsmResolution)
			LOG_WARNING("RSM resolution needs to be a power of 2! Using " << shadowMapResolution_nextPow2);
		
		uboView["RSMRenderResolution"].Set(shadowMapResolution_nextPow2);
		
		int rsmReadResolution = static_cast<int>(shadowMapResolution_nextPow2 / pow(2, light.rsmReadLod));
		uboView["RSMReadResolution"].Set(rsmReadResolution);

		float clipPlaneWidth = sinf(light.halfAngle) * light.nearPlane * 2.0f;
		float valAreaFactor = clipPlaneWidth * clipPlaneWidth / (light.nearPlane * light.nearPlane * rsmReadResolution * rsmReadResolution);
		//float valAreaFactor = powf(sinf(light.halfAngle) * 2.0f / light.shadowMapResolution, 2.0);
		uboView["ValAreaFactor"].Set(valAreaFactor);

		// Indirect shadowing
		uboView["IndirectShadowComputationLod"].Set(static_cast<float>(light.indirectShadowComputationLod));
		float indirectShadowComputationBlockSize = static_cast<float>(1 << light.indirectShadowComputationLod);
		uboView["IndirectShadowComputationBlockSize"].Set(indirectShadowComputationBlockSize);
		std::int32_t indirectShadowComputationSampleInterval = static_cast<int>(indirectShadowComputationBlockSize * indirectShadowComputationBlockSize);
		Assert(indirectShadowComputationBlockSize <= rsmReadResolution, "Shadow sample interval can not be larger than the RSM.");
		
		uboView["IndirectShadowComputationSampleInterval"].Set(indirectShadowComputationSampleInterval);

		
		uboView["IndirectShadowComputationSuperValWidth"].Set(sqrtf(valAreaFactor) * indirectShadowComputationBlockSize);
		uboView["IndirectShadowSamplingOffset"].Set(0.5f + sqrtf(2.0f) * indirectShadowComputationBlockSize / 2.0f);


		// (Re)Init shadow map if necessary (function does not do anything if already intialized with same settings.
		m_shadowMaps[lightIndex].Init(light.rsmResolution);
	}
}

void Renderer::BindGBuffer()
{
	m_GBuffer_diffuse->Bind(0);
	m_GBuffer_roughnessMetallic->Bind(1);
	m_GBuffer_normal->Bind(2);
	m_GBuffer_depth->Bind(3);

	m_samplerNearest.BindSampler(0);
	m_samplerNearest.BindSampler(1);
	m_samplerNearest.BindSampler(2);
	m_samplerNearest.BindSampler(3);
}

void Renderer::BindObjectUBO(unsigned int _objectIndex)
{
	m_uboRing_PerObject->BindBlockAsUBO(m_uboInfoPerObject.bufferBinding, _objectIndex);
}

void Renderer::OutputHDRTextureToBackbuffer()
{
	gl::Disable(gl::Cap::CULL_FACE);
	gl::Disable(gl::Cap::DEPTH_TEST);
	gl::SetDepthWrite(false);

	gl::FramebufferObject::BindBackBuffer();
	m_shaderTonemap->Activate();

	m_samplerNearest.BindSampler(0);
	m_HDRBackbufferTexture->Bind(0);

	m_screenTriangle->Draw();
}

void Renderer::DrawSceneToGBuffer()
{
	PROFILE_GPU_SCOPED(DrawSceneToGBuffer);

	gl::Enable(gl::Cap::DEPTH_TEST);
	gl::SetDepthWrite(true);

	m_samplerLinearRepeat.BindSampler(0);

	m_GBuffer->Bind(false);
	GL_CALL(glClear, GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	m_shaderFillGBuffer[(int)ShaderAlphaTest::OFF]->Activate();
	DrawScene(true, SceneDrawSubset::FULLOPAQUE_ONLY);
	m_shaderFillGBuffer[(int)ShaderAlphaTest::ON]->Activate();
	DrawScene(true, SceneDrawSubset::ALPHATESTED_ONLY);
}

void Renderer::DrawShadowMaps()
{
	PROFILE_GPU_SCOPED(DrawShadowMaps);

	gl::Enable(gl::Cap::DEPTH_TEST);
	gl::SetDepthWrite(true);

	m_samplerLinearClamp.BindSampler(0);

	
	for (unsigned int lightIndex = 0; lightIndex < m_scene->GetLights().size(); ++lightIndex)
	{
		m_uboRing_SpotLight->BindBlockAsUBO(m_uboInfoSpotLight.bufferBinding, lightIndex);

		m_shadowMaps[lightIndex].BindFBO_RSM();
		GL_CALL(glClear, GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

		m_shaderFillRSM[(int)ShaderAlphaTest::OFF]->Activate();
		DrawScene(true, SceneDrawSubset::FULLOPAQUE_ONLY);
		m_shaderFillRSM[(int)ShaderAlphaTest::ON]->Activate();
		DrawScene(true, SceneDrawSubset::ALPHATESTED_ONLY);
	}

	for (unsigned int lightIndex = 0; lightIndex < m_scene->GetLights().size(); ++lightIndex)
	{
		// Resolve and generate RSM mipmaps
		m_shadowMaps[lightIndex].PrepareRSM(*m_screenTriangle);

		// Set base levels for reading.
		GL_CALL(glTextureParameteri, m_shadowMaps[lightIndex].GetFlux().GetInternHandle(), GL_TEXTURE_BASE_LEVEL, m_scene->GetLights()[lightIndex].rsmReadLod);
		GL_CALL(glTextureParameteri, m_shadowMaps[lightIndex].GetNormal().GetInternHandle(), GL_TEXTURE_BASE_LEVEL, m_scene->GetLights()[lightIndex].rsmReadLod);
		GL_CALL(glTextureParameteri, m_shadowMaps[lightIndex].GetDepthLinSq().GetInternHandle(), GL_TEXTURE_BASE_LEVEL, m_scene->GetLights()[lightIndex].rsmReadLod);
	}
}

void Renderer::DrawGBufferDebug()
{
	gl::Disable(gl::Cap::DEPTH_TEST);
	gl::Disable(gl::Cap::CULL_FACE);
	GL_CALL(glViewport, 0, 0, m_HDRBackbufferTexture->GetWidth(), m_HDRBackbufferTexture->GetHeight());

	m_shaderDebugGBuffer->Activate();
	gl::FramebufferObject::BindBackBuffer();

	BindGBuffer();

	m_screenTriangle->Draw();
}

void Renderer::ApplyDirectLighting()
{
	PROFILE_GPU_SCOPED(ApplyDirectLighting);

	gl::Disable(gl::Cap::CULL_FACE);
	gl::Disable(gl::Cap::DEPTH_TEST);
	gl::Enable(gl::Cap::BLEND);

	m_shaderDeferredDirectLighting_Spot->Activate();

	BindGBuffer();
	m_samplerShadow.BindSampler(4);

	for (unsigned int lightIndex = 0; lightIndex < m_scene->GetLights().size(); ++lightIndex)
	{
		m_shadowMaps[lightIndex].GetHighResDepth().Bind(4);
		
		m_uboRing_SpotLight->BindBlockAsUBO(m_uboInfoSpotLight.bufferBinding, lightIndex);
		m_screenTriangle->Draw();
	}

	gl::Disable(gl::Cap::BLEND);
}

void Renderer::ApplyRSMsBruteForce()
{
	gl::Disable(gl::Cap::DEPTH_TEST);
	gl::Enable(gl::Cap::BLEND);

	m_shaderIndirectLightingBruteForceRSM->Activate();

	BindGBuffer();
	m_samplerNearest.BindSampler(4);
	m_samplerNearest.BindSampler(5);
	m_samplerNearest.BindSampler(6);

	for (unsigned int lightIndex = 0; lightIndex < m_scene->GetLights().size(); ++lightIndex)
	{
		m_shadowMaps[lightIndex].GetFlux().Bind(4);
		m_shadowMaps[lightIndex].GetDepthLinSq().Bind(5);
		m_shadowMaps[lightIndex].GetNormal().Bind(6);

		m_uboRing_SpotLight->BindBlockAsUBO(m_uboInfoSpotLight.bufferBinding, lightIndex);
		m_screenTriangle->Draw();
	}

	gl::Disable(gl::Cap::BLEND);
}

/*void Renderer::LightCachesDirect()
{
	PROFILE_GPU_SCOPED(LightCaches);

	m_lightCacheCounter->BindIndirectDispatchBuffer();
	m_shaderLightCachesDirect->BindSSBO(*m_lightCacheBuffer, "LightCacheBuffer");
	m_shaderLightCachesDirect->BindSSBO(*m_lightCacheCounter, "LightCacheCounter");

	m_samplerShadow.BindSampler(0);

	m_shaderLightCachesDirect->Activate();

	GL_CALL(glMemoryBarrier, GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT);

	for (unsigned int lightIndex = 0; lightIndex < m_scene->GetLights().size(); ++lightIndex)
	{
		m_shadowMaps[lightIndex].depthBuffer->Bind(0);

		m_uboRing_SpotLight->BindBlockAsUBO(m_uboInfoSpotLight.bufferBinding, lightIndex);
		GL_CALL(glDispatchComputeIndirect, 0);
	}
}*/

void Renderer::LightCachesRSM()
{
	PROFILE_GPU_SCOPED(LightCaches);

	m_specularEnvmap->ClearToZero();
	m_specularEnvmap->BindImage(0, gl::Texture::ImageAccess::WRITE);

	m_lightCacheCounter->BindIndirectDispatchBuffer();
	m_shaderLightCachesRSM->BindSSBO(*m_lightCacheBuffer, "LightCacheBuffer");
	m_shaderLightCachesRSM->BindSSBO(*m_lightCacheCounter, "LightCacheCounter");

	m_samplerNearest.BindSampler(0);
	m_samplerLinearClamp.BindSampler(1); // filtering allowed for depthLinSq
	m_samplerNearest.BindSampler(2);

	if (m_indirectShadow)
	{
		m_samplerLinearClamp.BindSampler(4);
		m_voxelization->GetVoxelTexture().Bind(4);
	}
	
	m_shaderLightCachesRSM->Activate();

	GL_CALL(glMemoryBarrier, GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT);

	for (unsigned int lightIndex = 0; lightIndex < m_scene->GetLights().size(); ++lightIndex)
	{
		m_shadowMaps[lightIndex].GetFlux().Bind(0);
		m_shadowMaps[lightIndex].GetDepthLinSq().Bind(1);
		m_shadowMaps[lightIndex].GetNormal().Bind(2);

		m_uboRing_SpotLight->BindBlockAsUBO(m_uboInfoSpotLight.bufferBinding, lightIndex);
		GL_CALL(glDispatchComputeIndirect, 0);
	}
}


void Renderer::ConeTraceAO()
{
	gl::Disable(gl::Cap::CULL_FACE);
	gl::Disable(gl::Cap::DEPTH_TEST);
	gl::SetDepthWrite(false);


	BindGBuffer();
	m_samplerLinearClamp.BindSampler(4);
	m_voxelization->GetVoxelTexture().Bind(4);

	m_shaderConeTraceAO->Activate();
	m_screenTriangle->Draw();
}

void Renderer::AllocateCaches()
{
	PROFILE_GPU_SCOPED(AllocateCaches);

	gl::Disable(gl::Cap::CULL_FACE);
	gl::Disable(gl::Cap::DEPTH_TEST);
	gl::SetDepthWrite(false);

	// Optionally read old light cache count
	if (m_readLightCacheCount)
	{
		const void* counterData = m_lightCacheCounter->Map(gl::Buffer::MapType::READ, gl::Buffer::MapWriteFlag::NONE);
		m_lastNumLightCaches = reinterpret_cast<const int*>(counterData)[3];
		m_lightCacheCounter->Unmap();
		FrameProfiler::GetInstance().ReportValue("CacheCount", static_cast<float>(m_lastNumLightCaches));
	}

	// Clear cache counter and atlas. No need to clear the cache buffer itself!
	m_lightCacheCounter->ClearToZero();
	m_CAVAtlas->ClearToZero(); // TODO: Consider making it int and clearing with -1, simplifying the shaders

	BindGBuffer();

	m_shaderCacheGather->BindSSBO(*m_lightCacheCounter, "LightCacheCounter");
	m_shaderCacheGather->BindSSBO(*m_lightCacheBuffer, "LightCacheBuffer");
	//m_shaderCacheGather->BindSSBO(*m_lightCacheHashMap);
	m_CAVAtlas->BindImage(0, gl::Texture::ImageAccess::READ_WRITE, gl::TextureFormat::R32UI, 0);

	m_shaderCacheGather->Activate();

	const unsigned int threadsPerGroupX = 16;
	const unsigned int threadsPerGroupY = 16;
	unsigned numThreadGroupsX = (m_HDRBackbufferTexture->GetWidth() + threadsPerGroupX - 1) / threadsPerGroupX;
	unsigned numThreadGroupsY = (m_HDRBackbufferTexture->GetHeight() + threadsPerGroupY - 1) / threadsPerGroupY;
	GL_CALL(glDispatchCompute, numThreadGroupsX, numThreadGroupsY, 1);


	// Write command buffer.
	GL_CALL(glMemoryBarrier, GL_SHADER_STORAGE_BARRIER_BIT);
	m_shaderLightCachePrepare->Activate();
	GL_CALL(glDispatchCompute, 1, 1, 1);
}

void Renderer::PrepareSpecularEnvmaps()
{
	PROFILE_GPU_SCOPED(ProcessSpecularEnvmap);

	gl::Disable(gl::Cap::CULL_FACE);
	gl::Disable(gl::Cap::DEPTH_TEST);
	gl::SetDepthWrite(false);

	// Need to access previous results via texture fetch.
	GL_CALL(glMemoryBarrier, GL_TEXTURE_FETCH_BARRIER_BIT);

	// Sufficient for all shaders in this function.
	m_shaderSpecularEnvmapMipMap->BindSSBO(*m_lightCacheCounter, "LightCacheCounter");


	// MipMap remaining levels.
	m_specularEnvmap->Bind(0);
	m_samplerLinearClamp.BindSampler(0);
	m_shaderSpecularEnvmapMipMap->Activate();
	for (int i = 1; i < m_specularEnvmapFBOs.size(); ++i)
	{
		GL_CALL(glTextureParameteri, m_specularEnvmap->GetInternHandle(), GL_TEXTURE_BASE_LEVEL, i - 1);
		m_specularEnvmapFBOs[i]->Bind(true);
		m_screenTriangle->Draw();
	}

	GL_CALL(glTextureParameteri, m_specularEnvmap->GetInternHandle(), GL_TEXTURE_BASE_LEVEL, 0);


	// Push down for each pulled layer.
	// This pass uses the vertex/fragment shader only for simple thread spawning. A compute shader might work as well!

	// Need to bind a target that is large enough, otherwise Nvidia driver clamps the viewport down!
	// On the other hand it apparently does not mind writing to the same texture as currently bound as target.
	m_specularEnvmapFBOs[0]->Bind(false); 
	GL_CALL(glColorMask, GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);

	m_shaderSpecularEnvmapFillHoles->Activate();
	for (int i = m_specularEnvmapMaxFillHolesLevel; i > 0; --i)
	{
		GL_CALL(glMemoryBarrier, GL_SHADER_IMAGE_ACCESS_BARRIER_BIT); // Need to access previous results via imageLoad.

		m_specularEnvmap->BindImage(0, gl::Texture::ImageAccess::READ, i);
		m_specularEnvmap->BindImage(1, gl::Texture::ImageAccess::READ_WRITE, i - 1);
		
		unsigned int readTextureSize = static_cast<unsigned int>(m_specularEnvmap->GetWidth() * pow(2, -i));
		GL_CALL(glViewport, 0, 0, readTextureSize, readTextureSize);
		m_screenTriangle->Draw();
	}

	GL_CALL(glColorMask, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
}

void Renderer::ApplyCaches()
{
	PROFILE_GPU_SCOPED(ApplyCaches);

	gl::Disable(gl::Cap::CULL_FACE);
	gl::Disable(gl::Cap::DEPTH_TEST);
	gl::Enable(gl::Cap::BLEND);

	BindGBuffer();


	//m_shaderCacheApply->BindSSBO(*m_lightCacheHashMap);
	m_shaderCacheApply->BindSSBO(*m_lightCacheBuffer, "LightCacheBuffer");

	m_CAVAtlas->Bind(4);
	m_samplerNearest.BindSampler(4);

	/*if (m_indirectShadow)
	{
		m_voxelization->GetVoxelTexture().Bind(5);
		m_samplerLinearClamp.BindSampler(5);
	}*/

	m_samplerLinearClamp.BindSampler(6);
	m_specularEnvmap->Bind(6);

	m_shaderCacheApply->Activate();

	GL_CALL(glMemoryBarrier, GL_SHADER_STORAGE_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
	m_screenTriangle->Draw();

	gl::Disable(gl::Cap::BLEND);
}

void Renderer::DrawScene(bool setTextures, SceneDrawSubset drawSubset)
{
	Model::BindVAO();

	/*if (drawSubset == SceneDrawSubset::FULLOPAQUE_ONLY)
		gl::Enable(gl::Cap::CULL_FACE);
	else if (drawSubset == SceneDrawSubset::ALPHATESTED_ONLY)
		gl::Disable(gl::Cap::CULL_FACE);*/

	if (setTextures)
	{
		m_samplerLinearRepeat.BindSampler(0);
		m_samplerLinearRepeat.BindSampler(1);
		m_samplerLinearRepeat.BindSampler(2);
	}
	else if (drawSubset == SceneDrawSubset::ALPHATESTED_ONLY)
		m_samplerLinearRepeat.BindSampler(0);

	for (unsigned int entityIndex =0; entityIndex < m_scene->GetEntities().size(); ++entityIndex)
	{
		const SceneEntity& entity = m_scene->GetEntities()[entityIndex];
		if (!entity.GetModel())
			continue;

		BindObjectUBO(entityIndex);
		entity.GetModel()->BindBuffers();
		for (const Model::Mesh& mesh : entity.GetModel()->GetMeshes())
		{
			Assert(mesh.diffuse, "Mesh has no diffuse texture. This is not supported by the renderer.");
			Assert(mesh.normalmap, "Mesh has no normal map. This is not supported by the renderer.");
			Assert(mesh.roughnessMetallic, "Mesh has no roughnessMetallic map. This is not supported by the renderer.");

			if ((drawSubset == SceneDrawSubset::FULLOPAQUE_ONLY && mesh.alphaTesting) ||
				(drawSubset == SceneDrawSubset::ALPHATESTED_ONLY && !mesh.alphaTesting))
			{
				continue;
			}
			//if (drawSubset == SceneDrawSubset::ALL)
			//	mesh.alphaTesting ? gl::Enable(gl::Cap::CULL_FACE) : gl::Disable(gl::Cap::CULL_FACE);

			if (mesh.doubleSided)
				gl::Disable(gl::Cap::CULL_FACE);
			else
				gl::Enable(gl::Cap::CULL_FACE);


			if (setTextures)
			{
				if (mesh.diffuse)
					mesh.diffuse->Bind(0);
				if (mesh.normalmap)
					mesh.normalmap->Bind(1);
				if (mesh.roughnessMetallic)
					mesh.roughnessMetallic->Bind(2);
			}
			else if (drawSubset == SceneDrawSubset::ALPHATESTED_ONLY)
				mesh.diffuse->Bind(0);


			GL_CALL(glDrawElements, GL_TRIANGLES, mesh.numIndices, GL_UNSIGNED_INT, reinterpret_cast<const void*>(sizeof(std::uint32_t) * mesh.startIndex));
		}
	}
}

void Renderer::SetReadLightCacheCount(bool trackLightCacheHashCollisionCount)
{
	m_readLightCacheCount = trackLightCacheHashCollisionCount;
	gl::Buffer::UsageFlag usageFlag = gl::Buffer::IMMUTABLE;
	if (trackLightCacheHashCollisionCount)
		usageFlag = gl::Buffer::MAP_READ;
	m_lightCacheCounter = std::make_unique<gl::Buffer>(sizeof(unsigned int) * 4, usageFlag, nullptr);
	m_lastNumLightCaches = 0;
}

bool Renderer::GetReadLightCacheCount() const
{
	return m_readLightCacheCount;
}

unsigned int Renderer::GetLightCacheActiveCount() const
{
	return m_lastNumLightCaches;
}


unsigned int Renderer::GetCAVResolution() const
{
	if (m_CAVAtlas)
		return m_CAVAtlas->GetHeight();
	else
		return 0;
}

void Renderer::SetCAVCascades(unsigned int numCascades, unsigned int resolutionPerCascade)
{
	Assert(numCascades > 0 && resolutionPerCascade > 0, "Invalid address volume cascade settings!");
	Assert(numCascades <= s_maxNumCAVCascades, "Maximum number of cascades exceeded!");

	m_CAVAtlas = std::make_unique<gl::Texture3D>(numCascades * resolutionPerCascade, resolutionPerCascade, resolutionPerCascade, gl::TextureFormat::R32UI);
	
	// Fill in new voxel sizes if necessary.
	size_t previousSize = m_CAVCascadeWorldSize.size();
	m_CAVCascadeWorldSize.resize(numCascades);
	if (previousSize == 0)
		m_CAVCascadeWorldSize[0] = 4.0f;
	for (size_t i = std::max<size_t>(1, previousSize); i < m_CAVCascadeWorldSize.size(); ++i)
		m_CAVCascadeWorldSize[i] = m_CAVCascadeWorldSize[i - 1] * 2.0f;

	LOG_INFO("Address volume atlas texture resolution " << m_CAVAtlas->GetWidth() << "x" << m_CAVAtlas->GetHeight() << "x" << m_CAVAtlas->GetDepth() <<
		" using " << (m_CAVAtlas->GetWidth()* m_CAVAtlas->GetHeight() * m_CAVAtlas->GetDepth() * 4 / 1024) << "kb memory.");

	UpdateConstantUBO();
}

void Renderer::SetCAVCascadeWorldSize(unsigned int cascade, float voxelWorldSize)
{
	Assert(cascade < m_CAVCascadeWorldSize.size(), "Given address volume cascade does not exist!");
	Assert(voxelWorldSize > 0.0f, "Voxel world size can not be negative!");

	/*if (cascade > 0 && m_lightCacheAddressCascadeWorldVoxelSize[cascade - 1] > m_lightCacheAddressCascadeWorldVoxelSize[cascade])
	{
		LOG_WARNING("Cascade voxel size of higher order cascades should be higher than lower ones");
	}*/

	m_CAVCascadeWorldSize[cascade] = voxelWorldSize;
}

void Renderer::SetCAVCascadeTransitionSize(float transitionZoneSize)
{
	bool updateShader = (m_CAVCascadeTransitionSize > 0 && transitionZoneSize <= 0.0) || 
						(m_CAVCascadeTransitionSize <= 0 && transitionZoneSize > 0.0);

	m_CAVCascadeTransitionSize = transitionZoneSize;
	if (updateShader)
		ReloadLightingSettingDependentCacheShader();
}

void Renderer::SetExposure(float exposure)
{
	m_tonemapExposure = exposure;
	GL_CALL(glProgramUniform1f, m_shaderTonemap->GetProgram(), 0, m_tonemapExposure);
}
void Renderer::SetTonemapLMax(float tonemapLMax)
{
	m_tonemapLMax = tonemapLMax;
	GL_CALL(glProgramUniform1f, m_shaderTonemap->GetProgram(), 1, log2f(m_tonemapLMax + 1.0f));
}

void Renderer::SaveToPFM(const std::string& filename) const
{
	std::unique_ptr<ei::Vec4[]> imageData(new ei::Vec4[m_HDRBackbufferTexture->GetWidth() * m_HDRBackbufferTexture->GetHeight()]);
	m_HDRBackbufferTexture->ReadImage(0, gl::TextureReadFormat::RGBA, gl::TextureReadType::FLOAT, m_HDRBackbufferTexture->GetWidth() * m_HDRBackbufferTexture->GetHeight() * sizeof(ei::Vec4), imageData.get());
	if (WritePfm(imageData.get(), ei::IVec2(m_HDRBackbufferTexture->GetWidth(), m_HDRBackbufferTexture->GetHeight()), filename))
		LOG_INFO("Wrote screenshot \"" + filename + "\"");
}





AutoReloadShaderPtr Renderer::ShadowMap::m_shaderRSMDownSample;

Renderer::ShadowMap::ShadowMap(ShadowMap& old) :
	rsmFBOs(old.rsmFBOs),

	flux(old.flux),
	normal(old.normal),
	depthLinSq(old.depthLinSq),
	depthBuffer(old.depthBuffer)
{
	old.rsmFBOs.clear();

	old.flux = nullptr;
	old.normal = nullptr;
	old.depthLinSq = nullptr;
	old.depthBuffer = nullptr;
}
Renderer::ShadowMap::ShadowMap() :
	flux(nullptr),
	normal(nullptr),
	depthLinSq(nullptr),
	depthBuffer(nullptr)
{}

void Renderer::ShadowMap::DeInit()
{
	for (gl::FramebufferObject* fbo : rsmFBOs)
	{
		SAFE_DELETE(fbo);
	}
	rsmFBOs.clear();

	SAFE_DELETE(flux);
	SAFE_DELETE(normal);
	SAFE_DELETE(depthLinSq);
	SAFE_DELETE(depthBuffer);
}

void Renderer::ShadowMap::Init(unsigned int rsmResolution)
{
	if (flux != nullptr && rsmResolution == flux->GetWidth())
	{
		return;
	}

	DeInit();

	flux = new gl::Texture2D(rsmResolution, rsmResolution, gl::TextureFormat::RGB16F, 0, 0); // R11G11B10 was not sufficient for downsampling ops
	normal = new gl::Texture2D(rsmResolution, rsmResolution, gl::TextureFormat::RG16I, 0, 0);
	depthLinSq = new gl::Texture2D(rsmResolution, rsmResolution, gl::TextureFormat::RG16F, 0, 0);
	depthBuffer = new gl::Texture2D(rsmResolution, rsmResolution, gl::TextureFormat::DEPTH_COMPONENT32F, 1, 0);

	rsmFBOs.push_back(new gl::FramebufferObject({ gl::FramebufferObject::Attachment(flux), gl::FramebufferObject::Attachment(normal), gl::FramebufferObject::Attachment(depthLinSq) }, gl::FramebufferObject::Attachment(depthBuffer)));
	for (int i = 1; i < log2(rsmResolution); ++i)
	{
		rsmFBOs.push_back(new gl::FramebufferObject({ gl::FramebufferObject::Attachment(flux, i), gl::FramebufferObject::Attachment(normal, i), gl::FramebufferObject::Attachment(depthLinSq, i) }));
	}	
}

void Renderer::ShadowMap::PrepareRSM(const gl::ScreenAlignedTriangle& screenTri)
{	
	if (m_shaderRSMDownSample.get() == nullptr)
	{
		m_shaderRSMDownSample = new gl::ShaderObject("RSM downsample");
		m_shaderRSMDownSample->AddShaderFromFile(gl::ShaderObject::ShaderType::VERTEX, "shader/screenTri.vert");
		m_shaderRSMDownSample->AddShaderFromFile(gl::ShaderObject::ShaderType::FRAGMENT, "shader/downsamplersm.frag");
		m_shaderRSMDownSample->CreateProgram();
	}

	gl::Disable(gl::Cap::DEPTH_TEST);
	gl::Disable(gl::Cap::CULL_FACE);
	gl::SetDepthWrite(false);
	m_shaderRSMDownSample->Activate();

	const auto& samplerLinearClamp = gl::SamplerObject::GetSamplerObject(gl::SamplerObject::Desc(gl::SamplerObject::Filter::LINEAR, gl::SamplerObject::Filter::LINEAR, gl::SamplerObject::Filter::LINEAR,
		gl::SamplerObject::Border::CLAMP));
	const auto& samplerNearestClamp = gl::SamplerObject::GetSamplerObject(gl::SamplerObject::Desc(gl::SamplerObject::Filter::NEAREST, gl::SamplerObject::Filter::NEAREST, gl::SamplerObject::Filter::NEAREST,
		gl::SamplerObject::Border::CLAMP));


	GetFlux().Bind(0);
	samplerNearestClamp.BindSampler(0);
	GetNormal().Bind(1);
	samplerNearestClamp.BindSampler(1);
	GetDepthLinSq().Bind(2);
	samplerLinearClamp.BindSampler(2);

	for (int i = 1; i < rsmFBOs.size(); ++i)
	{
		GL_CALL(glTextureParameteri, GetFlux().GetInternHandle(), GL_TEXTURE_BASE_LEVEL, i - 1);
		GL_CALL(glTextureParameteri, GetNormal().GetInternHandle(), GL_TEXTURE_BASE_LEVEL, i - 1);
		GL_CALL(glTextureParameteri, GetDepthLinSq().GetInternHandle(), GL_TEXTURE_BASE_LEVEL, i - 1);

		rsmFBOs[i]->Bind(true);
		GL_CALL(glClear, GL_COLOR_BUFFER_BIT);

		screenTri.Draw();
	}
}

void Renderer::ShadowMap::BindFBO_RSM()
{
	rsmFBOs[0]->Bind(true);
}