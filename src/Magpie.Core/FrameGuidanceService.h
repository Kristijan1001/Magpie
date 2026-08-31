#pragma once
#include "ZeroFrameGuidanceProvider.h"

namespace Magpie {

class DeviceResources;

struct FrameGuidanceConsumerViews {
	FrameGuidanceView produced{};
	FrameGuidanceView zero{};
	bool adapted = false;
	bool usedFallback = false;
};

class FrameGuidanceService {
public:
	FrameGuidanceService() noexcept;
	~FrameGuidanceService();
	FrameGuidanceService(const FrameGuidanceService&) = delete;
	FrameGuidanceService& operator=(const FrameGuidanceService&) = delete;
	bool SetDepthProvider(std::unique_ptr<IDepthProvider> provider) noexcept;
	bool SetMotionVectorProvider(
		std::unique_ptr<IMotionVectorProvider> provider
	) noexcept;

	bool Initialize(
		DeviceResources& resources,
		ID3D11Texture2D* sourceFrame,
		const FrameGuidanceRequirements& requirements
	) noexcept;
	const FrameGuidanceView& BeginFrame(
		FrameGuidanceFrameId frameId,
		ID3D11Texture2D* sourceFrame,
		const FrameGuidanceRequirements& requirements
	) noexcept;
	bool Resize(
		FrameGuidanceExtent sourceExtent,
		FrameGuidanceFrameId currentFrameId,
		const FrameGuidanceRequirements& requirements
	) noexcept;
	void ResetHistory(FrameGuidanceResetReason reason) noexcept;
	FrameGuidanceConsumerViews GetConsumerViews(
		FrameGuidanceFrameId frameId,
		FrameGuidanceExtent targetExtent
	) noexcept;

	const FrameGuidanceView& View() const noexcept { return _view; }
	const FrameGuidanceView& ZeroView() const noexcept { return _zeroView; }
	FrameGuidanceExtent SourceExtent() const noexcept { return _sourceExtent; }
	bool IsInitialized() const noexcept { return _resources != nullptr; }
	// 提供器只能在 Initialize 之前设置，实时调整参数时据此判断新的需求能否满足
	bool HasDepthProvider() const noexcept { return _depthProvider != nullptr; }
	bool HasMotionVectorProvider() const noexcept {
		return _motionProvider != nullptr;
	}

private:
	struct AdapterCache;

	const FrameGuidanceView& _Produce(
		const FrameGuidanceFrame& frame,
		const FrameGuidanceRequirements& requirements
	) noexcept;

	DeviceResources* _resources = nullptr;
	ZeroFrameGuidanceResources _zeroResources;
	ZeroDepthProvider _zeroDepthProvider;
	ZeroMotionVectorProvider _zeroMotionProvider;
	std::unique_ptr<IDepthProvider> _depthProvider;
	std::unique_ptr<IMotionVectorProvider> _motionProvider;
	FrameGuidanceExtent _sourceExtent{};
	FrameGuidanceView _view{};
	FrameGuidanceView _zeroView{};
	FrameGuidanceRequirements _cachedRequirements{};
	FrameGuidanceRequirements _lastLoggedRequirements{};
	FrameGuidanceFrameId _cachedFrameId = 0;
	bool _hasCachedFrame = false;
	bool _hasLoggedRequirements = false;
	bool _depthProviderReady = false;
	bool _motionProviderReady = false;
	std::unique_ptr<AdapterCache> _adapterCache;
};

}
