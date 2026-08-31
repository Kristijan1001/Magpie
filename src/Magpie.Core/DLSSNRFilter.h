#pragma once
#include "NativeEffectBackend.h"

namespace Magpie {

class DeviceResources;
class NgxD3D12Core;
struct EffectOption;

struct DLSSNRSettings {
	int preset = 0;
	int style = 0;
	float intensity = 1.0f;
	float localToneStrength = 1.0f;
	float localStructureStrength = 1.0f;
	float skinStructureStrength = -1.0f;
	bool useAutoMask = false;
	bool uiCorrection = false;
	// 0 available/both, 1 force Zero, 2 motion only, 3 depth only.
	int guidanceMode = 0;
	uint32_t depthInferenceInterval = 4;
};

// 定义于 NativeEffectBackendFactory.cpp，创建和实时更新共用同一份解析逻辑
DLSSNRSettings ParseDLSSNRSettings(const EffectOption& option) noexcept;

// Experimental same-resolution DLSS neural filter. Magpie only owns the
// composited colour frame, so valid zero-filled motion/depth textures are used
// as explicit temporal guides.
class DLSSNRFilter final : public NativeEffectBackend {
public:
	struct Impl;

	DLSSNRFilter();
	DLSSNRFilter(const DLSSNRFilter&) = delete;
	DLSSNRFilter& operator=(const DLSSNRFilter&) = delete;
	~DLSSNRFilter() override;

	FrameGuidanceRequirements GetFrameGuidanceRequirements() const noexcept override;
	bool Drain() noexcept override;

	NativeEffectParameterUpdate UpdateParameters(
		const EffectOption& option
	) noexcept override;

	bool Initialize(
		DeviceResources& resources,
		NgxD3D12Core& ngxCore,
		ID3D11Texture2D* input,
		ID3D11Texture2D* output,
		const DLSSNRSettings& settings
	) noexcept;

	bool Resize(
		DeviceResources& resources,
		ID3D11Texture2D* input,
		ID3D11Texture2D* output
	) noexcept override;

	bool Draw(const NativeEffectDrawContext& context) noexcept override;

private:
	std::unique_ptr<Impl> _impl;
	DLSSNRSettings _settings;
	NgxD3D12Core* _ngxCore = nullptr;
};

}
