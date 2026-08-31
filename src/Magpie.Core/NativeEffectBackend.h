#pragma once
#include "FrameGuidanceTypes.h"

namespace Magpie {

class DeviceResources;
struct EffectOption;

// 实时修改效果参数时后端的响应方式
enum class NativeEffectParameterUpdate {
	// 新参数已生效，无需重建
	Applied,
	// 参数在创建时写入，必须重建后端
	NeedsRecreate
};

struct NativeEffectDrawContext {
	ID3D11Texture2D* input = nullptr;
	ID3D11Texture2D* output = nullptr;
	FrameGuidanceFrameId frameId = 0;
	const FrameGuidanceView& frameGuidance;
	const FrameGuidanceView& zeroFrameGuidance;
};

// Common lifetime and rendering contract for native SDK-backed effects.
// Creation parameters remain in NativeEffectBackendFactory so Renderer does
// not need one parallel container and one name-dispatch branch per SDK.
class NativeEffectBackend {
public:
	virtual ~NativeEffectBackend() = default;

	virtual FrameGuidanceRequirements GetFrameGuidanceRequirements() const noexcept {
		return {};
	}
	virtual bool Drain() noexcept { return true; }

	// 叠加层实时修改参数后调用，运行于后台线程。默认认为参数只能在创建时写入，
	// Renderer 会重建后端，因此后端无需实现此方法也能正确响应参数变化。
	virtual NativeEffectParameterUpdate UpdateParameters(
		const EffectOption& /*option*/
	) noexcept {
		return NativeEffectParameterUpdate::NeedsRecreate;
	}

	virtual bool Resize(
		DeviceResources& resources,
		ID3D11Texture2D* input,
		ID3D11Texture2D* output
	) noexcept = 0;

	virtual bool Draw(const NativeEffectDrawContext& context) noexcept = 0;
};

}
