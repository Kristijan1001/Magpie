#pragma once
#include "BackendDescriptorStore.h"
#include "CursorDrawer.h"
#include "DeviceResources.h"
#include "EffectDrawer.h"
#include "EffectsProfiler.h"
#include "FrameGuidanceService.h"
#include "NgxD3D12Core.h"
#include "OverlayDrawer.h"
#include "PresenterBase.h"
#include "StepTimer.h"

namespace Magpie {

class FrameSourceBase;

class Renderer {
public:
	Renderer() noexcept;
	~Renderer() noexcept;

	Renderer(const Renderer&) = delete;
	Renderer(Renderer&&) = delete;

	ScalingError Initialize(HWND hwndAttach, OverlayOptions& overlayOptions) noexcept;

	bool Render(bool force = false, bool waitForGpu = false) noexcept;
	bool RenderDLSSFGFrame(
		uint32_t sharedTextureSlot,
		uint32_t sharedTextureGeneration
	) noexcept;

	bool OnResize() noexcept;

	void OnEndResize() noexcept;

	void OnMove() noexcept;

	void SwitchToolbarState() noexcept;

	const RECT& SrcRect() const noexcept;

	// 屏幕坐标而不是窗口局部坐标
	const RECT& DestRect() const noexcept {
		return _destRect;
	}

	const FrameSourceBase& FrameSource() const noexcept {
		return *_frameSource;
	}

	void OnCursorVisibilityChanged(bool isVisible, bool onDestory);

	void MessageHandler(UINT msg, WPARAM wParam, LPARAM lParam) noexcept;

	const std::vector<const EffectDesc*>& ActiveEffectDescs() const noexcept {
		return _activeEffectDescs;
	}

	// 实时修改效果参数，可由前台线程调用。同一帧内的多次修改会合并后应用
	void SetEffectParameter(
		uint32_t effectIdx,
		std::string parameterName,
		float value
	) noexcept;

	// 各效果的参数能否实时调整，索引与 ScalingOptions::effects 一致。
	// 和 _activeEffectDescs 一样在后台初始化期间填充，之后只读
	const std::vector<bool>& CanEditEffectParametersLive() const noexcept {
		return _canEditEffectParametersLive;
	}

	void StartProfile() noexcept;

	void StopProfile() noexcept;

	bool IsCursorOnOverlayCaptionArea() const noexcept {
		return _overlayDrawer.IsCursorOnCaptionArea();
	}

	winrt::fire_and_forget TakeScreenshot(
		uint32_t effectIdx,
		uint32_t passIdx = std::numeric_limits<uint32_t>::max(),
		uint32_t outputIdx = std::numeric_limits<uint32_t>::max()
	) noexcept;

private:
	struct FrontendRenderTimings {
		std::chrono::nanoseconds beginFrame{};
		std::chrono::nanoseconds draw{};
		std::chrono::nanoseconds endFrame{};
	};

	bool _FrontendRender(
		bool waitForGpu = false,
		uint32_t sharedTextureSlot = std::numeric_limits<uint32_t>::max(),
		FrontendRenderTimings* timings = nullptr
	) noexcept;
	bool _OpenFrontendSharedTextures() noexcept;
	void _ResetDLSSFGSlotEvents() noexcept;
	void _RecordDLSSFGFrontendTimings(
		bool usesFrameLatencyWaitableObject,
		std::chrono::nanoseconds pacingWait,
		const FrontendRenderTimings& timings
	) noexcept;

	void _BackendThreadProc() noexcept;

	HANDLE _InitBackend() noexcept;

	bool _InitFrameSource() noexcept;

	ID3D11Texture2D* _BuildEffects() noexcept;

	void _UpdateActiveEffectDescs() noexcept;

	bool _ShouldAppendBicubic(ID3D11Texture2D* outTexture) noexcept;

	bool _AppendBicubic(ID3D11Texture2D** inOutTexture) noexcept;

	ID3D11Texture2D* _ResizeEffects() noexcept;

	void _ApplyPendingEffectParameters() noexcept;

	bool _RecreateNativeEffectBackend(uint32_t effectIdx) noexcept;

	bool _RebuildDLSSFrameGenerator() noexcept;

	// 根据当前的效果参数重新计算帧率限制，初始化和实时调整时都会调用
	void _UpdateFrameRateLimits() noexcept;

	void _UpdateSynchronousPresentInterval() noexcept;

	void _UpdateDestRect() noexcept;

	HANDLE _CreateSharedTexture(ID3D11Texture2D* effectsOutput) noexcept;

	void _BackendRender(
		ID3D11Texture2D* effectsOutput,
		bool isNewCaptureFrame
	) noexcept;

	bool _PublishBackendTexture(
		ID3D11Texture2D* texture,
		bool synchronous,
		bool generatedFrame = false
	) noexcept;

	bool _InitializeDLSSFrameGenerator(
		ID3D11Texture2D* input,
		const struct DLSSFrameGenerationSettings& settings
	) noexcept;
	void _HandleDLSSFrameGenerationFailure(ID3D11Texture2D* input) noexcept;
	void _DisableDLSSFrameGenerationForSession() noexcept;
	bool _DrainNgxConsumers() noexcept;
	void _ReleaseNgxConsumers() noexcept;

	bool _UpdateDynamicConstants() const noexcept;

	winrt::IAsyncAction _UpdateNextScreenshotNum(const wchar_t* imgFormat) noexcept;

	winrt::IAsyncOperation<bool> _TakeScreenshotImpl(
		uint32_t effectIdx,
		uint32_t passIdx,
		uint32_t outputIdx
	) noexcept;

	static LRESULT CALLBACK _LowLevelKeyboardHook(int nCode, WPARAM wParam, LPARAM lParam);

	// 只能由前台线程访问
	DeviceResources _frontendResources;
	std::unique_ptr<PresenterBase> _presenter;
	
	CursorDrawer _cursorDrawer;
	OverlayDrawer _overlayDrawer;

	static constexpr uint32_t MAX_SHARED_TEXTURE_SLOTS = 4;
	std::array<winrt::com_ptr<ID3D11Texture2D>, MAX_SHARED_TEXTURE_SLOTS>
		_frontendSharedTextures;
	std::array<winrt::com_ptr<IDXGIKeyedMutex>, MAX_SHARED_TEXTURE_SLOTS>
		_frontendSharedTextureMutexes;
	std::array<uint64_t, MAX_SHARED_TEXTURE_SLOTS> _lastAccessMutexKeys{};
	RECT _destRect{};
	
	std::thread _backendThread;

	wil::unique_hhook _hKeyboardHook;
	
	// 只能由后台线程访问
	DeviceResources _backendResources;
	Magpie::BackendDescriptorStore _backendDescriptorStore;
	std::unique_ptr<FrameSourceBase> _frameSource;
	FrameGuidanceService _frameGuidanceService;
	FrameGuidanceFrameId _capturedFrameId = 0;
	NgxD3D12Core _ngxD3D12Core;
	std::vector<EffectDrawer> _effectDrawers;
	// options.effects 的副本，叠加层实时修改的参数保存在这里
	std::vector<EffectOption> _effectOptions;
	// 参数已改变、等待应用的效果
	SmallVector<uint32_t> _dirtyEffectParameters;
	// 参数改变后即使没有新的捕获帧也应渲染一次
	bool _forceNextRender = false;
	bool _loggedFrameGuidanceUnavailable = false;
	std::vector<std::unique_ptr<class NativeEffectBackend>> _nativeEffectBackends;
	std::unique_ptr<class DLSSFrameGenerator> _dlssFrameGenerator;
	uint32_t _dlssFgConsecutiveFailures = 0;
	uint32_t _dlssFgRecoveryAttempts = 0;

	StepTimer _stepTimer;
	EffectsProfiler _effectsProfiler;

	winrt::com_ptr<ID3D11Fence> _d3dFence;
	uint64_t _fenceValue = 0;
	wil::unique_event_nothrow _fenceEvent;

	std::array<winrt::com_ptr<ID3D11Texture2D>, MAX_SHARED_TEXTURE_SLOTS>
		_backendSharedTextures;
	std::array<winrt::com_ptr<IDXGIKeyedMutex>, MAX_SHARED_TEXTURE_SLOTS>
		_backendSharedTextureMutexes;
	std::array<HANDLE, MAX_SHARED_TEXTURE_SLOTS> _sharedTextureHandles{};
	std::array<wil::unique_handle, MAX_SHARED_TEXTURE_SLOTS>
		_sharedTextureAvailableEvents;
	uint32_t _sharedTextureSlotCount = 1;
	uint32_t _nextBackendSharedTextureSlot = 0;

	winrt::com_ptr<ID3D11Buffer> _dynamicCB;

	uint32_t _screenshotNum = 0;

	// 可由所有线程访问
	std::array<std::atomic<uint64_t>, MAX_SHARED_TEXTURE_SLOTS>
		_sharedTextureMutexKeys{};
	std::atomic<uint32_t> _latestSharedTextureSlot = 0;
	std::atomic<uint32_t> _sharedTextureGeneration = 0;
	std::atomic<bool> _synchronousFramePresentationEnabled = false;
	float _frameRateFilterTarget = 0.0f;
	// 捕获方式和屏幕刷新率决定的帧率上限，不随效果参数改变
	std::optional<float> _captureMaxFrameRate;
	std::chrono::nanoseconds _synchronousPresentInterval{};
	std::chrono::steady_clock::time_point _lastSynchronousPresentTime{};
	uint32_t _dlssFgFrontendTimingFrames = 0;
	bool _dlssFgFrontendTimingModeInitialized = false;
	bool _dlssFgFrontendTimingUsesWaitableObject = false;
	std::chrono::nanoseconds _dlssFgFrontendPacingWait{};
	std::chrono::nanoseconds _dlssFgFrontendBeginFrame{};
	std::chrono::nanoseconds _dlssFgFrontendDraw{};
	std::chrono::nanoseconds _dlssFgFrontendEndFrame{};
	std::atomic<uint64_t> _dlssFgRingWaitNanoseconds = 0;
	std::atomic<uint64_t> _dlssFgRingWaitSamples = 0;
	std::chrono::steady_clock::time_point _dlssFgDiagnosticsStart{};
	uint32_t _dlssFgCapturedFrameCount = 0;
	uint32_t _dlssFgPresentedFrameCount = 0;
	uint32_t _dlssFgGeneratedPublishSuccess = 0;
	uint32_t _dlssFgGeneratedPublishFailure = 0;
	uint32_t _dlssFgRealPublishSuccess = 0;
	uint32_t _dlssFgRealPublishFailure = 0;
	bool _dlssFgPresentationStopping = false;
	bool _isXeSSFrameGenerationActive = false;
	bool _xessFgFrontendSuppressionLogged = false;

	// INVALID_HANDLE_VALUE 表示后端初始化失败
	std::atomic<HANDLE> _sharedTextureHandle{ NULL };
	// 下面四个成员由 _sharedTextureHandle 同步
	winrt::Windows::System::DispatcherQueue _backendThreadDispatcher{ nullptr };
	ScalingError _backendInitError = ScalingError::NoError;
	std::vector<EffectDesc> _effectDescs;
	// 包含追加的 Bicubic
	std::vector<const EffectDesc*> _activeEffectDescs;
	std::vector<bool> _canEditEffectParametersLive;
};

}
