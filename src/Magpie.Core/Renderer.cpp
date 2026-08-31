#include "pch.h"
#include "CommonSharedConstants.h"
#include "DesktopDuplicationFrameSource.h"
#include "DeviceResources.h"
#include "DirectXHelper.h"
#include "DwmSharedSurfaceFrameSource.h"
#include "EffectCompiler.h"
#include "EffectDrawer.h"
#include "EffectsProfiler.h"
#include "GDIFrameSource.h"
#include "GraphicsCaptureFrameSource.h"
#include "Logger.h"
#include "OverlayDrawer.h"
#include "Renderer.h"
#include "ScalingOptions.h"
#include "ScalingWindow.h"
#include "ScreenshotHelper.h"
#include "StrHelper.h"
#include "TextureHelper.h"
#include "Win32Helper.h"
#include "DLSSFrameGenerator.h"
#include "NativeEffectBackend.h"
#include "NativeEffectBackendFactory.h"
#include "NvidiaOpticalFlowProvider.h"
#include "DepthAnythingV2Provider.h"
#include "XeSSFGPresenter.h"
#ifdef MP_USE_COMPSWAPCHAIN
#include "CompSwapchainPresenter.h"
#else
#include "AdaptivePresenter.h"
#endif
#include <dispatcherqueue.h>
#include <d3dkmthk.h>

namespace Magpie {

static FrameGuidanceRequirements CollectFrameGuidanceRequirements(
	const std::vector<std::unique_ptr<NativeEffectBackend>>& backends,
	const DLSSFrameGenerator* frameGenerator
) noexcept {
	FrameGuidanceRequirements result;
	for (const auto& backend : backends) {
		if (backend) result.Merge(backend->GetFrameGuidanceRequirements());
	}
	if (frameGenerator) {
		result.Merge(frameGenerator->GetFrameGuidanceRequirements());
	}
	return result;
}

// 大多数时候会在最后添加 Bicubic 来降采样或升采样，因此缓存在内存中
static EffectDesc bicubicDesc;

static bool IsDLSSFrameGenerationEffect(std::string_view name) noexcept {
	return name == "DLSSFG\\DLSS_FrameGeneration";
}

static bool IsXeSSFrameGenerationEffect(std::string_view name) noexcept {
	return name == "XeSSFG\\XeSS_FrameGeneration_x2_ZeroMV" ||
		name == "XeSSFG\\XeSS_MultiFrameGeneration_ZeroMV";
}

static bool IsFrameGenerationEffect(std::string_view name) noexcept {
	return IsDLSSFrameGenerationEffect(name) || IsXeSSFrameGenerationEffect(name);
}

static bool IsFrameRateFilterEffect(std::string_view name) noexcept {
	return name == "FrameRate_Filter";
}

// 创建和实时更新共用同一份解析逻辑
static DLSSFrameGenerationSettings ParseDLSSFrameGenerationSettings(
	const EffectOption& option
) noexcept {
	auto getParameter = [&](std::string_view name, float defaultValue) {
		auto it = option.parameters.find(std::string(name));
		return it == option.parameters.end() ? defaultValue : it->second;
	};

	return DLSSFrameGenerationSettings{
		.multiplier = std::clamp(
			(uint32_t)std::lround(getParameter("multiplier", 2.0f)), 2u, 4u),
		.useMotionVectors = getParameter("useMotionVectors", 1.0f) >= 0.5f,
		.useEstimatedDepth = getParameter("useEstimatedDepth", 0.0f) >= 0.5f
	};
}

static float ParseFrameRateFilterTarget(const EffectOption& option) noexcept {
	auto it = option.parameters.find("targetFrameRate");
	return std::clamp(
		it == option.parameters.end() ? 60.0f : it->second, 1.0f, 240.0f);
}

static double GetDisplayRefreshRate(HWND window) noexcept {
	MONITORINFOEXW monitorInfo{};
	monitorInfo.cbSize = sizeof(monitorInfo);
	const HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
	if (!monitor || !GetMonitorInfoW(monitor, &monitorInfo)) return 0.0;
	DEVMODEW mode{};
	mode.dmSize = sizeof(mode);
	if (!EnumDisplaySettingsW(
		monitorInfo.szDevice, ENUM_CURRENT_SETTINGS, &mode) ||
		mode.dmDisplayFrequency <= 1) return 0.0;
	return static_cast<double>(mode.dmDisplayFrequency);
}

static void WaitUntilHighResolution(
	std::chrono::steady_clock::time_point deadline
) noexcept {
	const auto now = std::chrono::steady_clock::now();
	if (deadline <= now) {
		return;
	}

	// CREATE_WAITABLE_TIMER_HIGH_RESOLUTION is 0x2. Use the literal so this
	// remains buildable with older Windows SDK headers, and fall back to a
	// regular waitable timer when the running OS does not support the flag.
	static thread_local wil::unique_handle timer = []() noexcept {
		wil::unique_handle result;
		HANDLE handle = CreateWaitableTimerExW(
			nullptr, nullptr, 0x2, TIMER_MODIFY_STATE | SYNCHRONIZE);
		if (!handle) {
			handle = CreateWaitableTimerExW(
				nullptr, nullptr, 0, TIMER_MODIFY_STATE | SYNCHRONIZE);
		}
		result.reset(handle);
		return result;
	}();

	if (!timer) {
		std::this_thread::sleep_until(deadline);
		return;
	}

	const int64_t remainingNanoseconds =
		std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now).count();
	LARGE_INTEGER dueTime{};
	dueTime.QuadPart = -std::max<int64_t>(
		1, (remainingNanoseconds + 99) / 100);
	if (!SetWaitableTimer(timer.get(), &dueTime, 0, nullptr, nullptr, FALSE)) {
		std::this_thread::sleep_until(deadline);
		return;
	}
	WaitForSingleObject(timer.get(), INFINITE);
}

Renderer::Renderer() noexcept {}

Renderer::~Renderer() noexcept {
	// The backend can be waiting for a synchronous DLSSFG presentation while
	// the frontend thread is destroying this Renderer. Stop issuing new
	// synchronous sends before waiting for the backend thread to exit.
	_synchronousFramePresentationEnabled.store(false, std::memory_order_release);

	_hKeyboardHook.reset();

	if (_backendThread.joinable()) {
		const HANDLE hThread = _backendThread.native_handle();

		if (!wil::handle_wait(hThread, 0)) {
			const DWORD threadId = GetThreadId(_backendThread.native_handle());

			// 持续尝试直到 _backendThread 创建了消息队列
			while (!PostThreadMessage(threadId, WM_QUIT, 0, 0)) {
				if (wil::handle_wait(hThread, 1)) {
					break;
				}
			}
		}
		
		_backendThread.join();
	}
	_ReleaseNgxConsumers();
}

static void LogAdapter(IDXGIAdapter4* adapter) noexcept {
	DXGI_ADAPTER_DESC1 desc;
	adapter->GetDesc1(&desc);

	Logger::Get().Info(fmt::format("当前图形适配器: \n\tVendorId: {:#x}\n\tDeviceId: {:#x}\n\tDescription: {}",
		desc.VendorId, desc.DeviceId, StrHelper::UTF16ToUTF8(desc.Description)));
}

static void SetGpuPriority() noexcept {
	// 来自 https://github.com/obsproject/obs-studio/blob/16cb051a57bb357fe866252c1360ce2c38e2deec/libobs-d3d11/d3d11-subsystem.cpp#L429
	// 不使用 REALTIME 优先级，它会造成系统不稳定，而且可能会导致源窗口卡顿。
	// OBS 还调用了 SetGPUThreadPriority，但这个接口似乎无用。
	NTSTATUS status = D3DKMTSetProcessSchedulingPriorityClass(
		GetCurrentProcess(), D3DKMT_SCHEDULINGPRIORITYCLASS_HIGH);
	if (status != STATUS_SUCCESS) {
		Logger::Get().NTError("D3DKMTSetProcessSchedulingPriorityClass 失败", status);
	}
}

ScalingError Renderer::Initialize(HWND hwndAttach, OverlayOptions& overlayOptions) noexcept {
	_backendThread = std::thread(&Renderer::_BackendThreadProc, this);

	if (!_frontendResources.Initialize(true)) {
		Logger::Get().Error("初始化前端资源失败");
		return ScalingError::ScalingFailedGeneral;
	}

	LogAdapter(_frontendResources.GetGraphicsAdapter());

	// 每次创建 D3D 设备后尝试提高 GPU 优先级，OBS 也是这么做的
	SetGpuPriority();

	uint32_t xessFrameGenerationEffectCount = 0;
	uint32_t xessFrameGenerationMultiplier = 2;
	bool useDLSSFrameGeneration = false;
	for (const EffectOption& effect : ScalingWindow::Get().Options().effects) {
		if (IsXeSSFrameGenerationEffect(effect.name)) {
			++xessFrameGenerationEffectCount;
			if (effect.name == "XeSSFG\\XeSS_MultiFrameGeneration_ZeroMV") {
				auto it = effect.parameters.find("multiplier");
				const float value = it == effect.parameters.end() ? 2.0f : it->second;
				xessFrameGenerationMultiplier = std::clamp(
					static_cast<uint32_t>(std::lround(value)), 2u, 4u);
			}
		}
		useDLSSFrameGeneration |= IsDLSSFrameGenerationEffect(effect.name);
	}
	if (xessFrameGenerationEffectCount > 1) {
		Logger::Get().Error("Only one XeSS Frame Generation effect is allowed");
		return ScalingError::ScalingFailedGeneral;
	}
	const bool useXeSSFrameGeneration = xessFrameGenerationEffectCount == 1;
	_isXeSSFrameGenerationActive = useXeSSFrameGeneration;
	if (useXeSSFrameGeneration && useDLSSFrameGeneration) {
		Logger::Get().Error("XeSSFG and DLSSFG cannot be enabled in the same effect chain");
		return ScalingError::ScalingFailedGeneral;
	}

	if (useXeSSFrameGeneration) {
		_presenter = std::make_unique<XeSSFGPresenter>(xessFrameGenerationMultiplier);
		if (!_presenter->Initialize(hwndAttach, _frontendResources)) {
			Logger::Get().Error("初始化 XeSSFGPresenter 失败");
			return ScalingError::ScalingFailedGeneral;
		}
	} else {
#ifdef MP_USE_COMPSWAPCHAIN
	_presenter = std::make_unique<CompSwapchainPresenter>();
	if (!_presenter->Initialize(hwndAttach, _frontendResources)) {
		Logger::Get().Error("初始化 CompSwapchainPresenter 失败");
#else
	_presenter = std::make_unique<AdaptivePresenter>();
	if (!_presenter->Initialize(hwndAttach, _frontendResources)) {
		Logger::Get().Error("初始化 AdaptivePresenter 失败");
#endif
		return ScalingError::ScalingFailedGeneral;
	}
	}

	// 等待后端初始化完成
	_sharedTextureHandle.wait(NULL, std::memory_order_relaxed);
	const HANDLE sharedTextureHandle = _sharedTextureHandle.load(std::memory_order_acquire);
	if (sharedTextureHandle == INVALID_HANDLE_VALUE) {
		Logger::Get().Error("后端初始化失败");
		// 一般的错误不会设置 _backendInitError
		return _backendInitError == ScalingError::NoError ? ScalingError::ScalingFailedGeneral : _backendInitError;
	}

	if (!_OpenFrontendSharedTextures()) {
		return ScalingError::ScalingFailedGeneral;
	}

	_UpdateDestRect();

	Logger::Get().Info(fmt::format("目标矩形: {},{},{},{} ({}x{})",
		_destRect.left, _destRect.top, _destRect.right, _destRect.bottom,
		_destRect.right - _destRect.left, _destRect.bottom - _destRect.top));

	if (!_cursorDrawer.Initialize(_frontendResources)) {
		Logger::Get().Error("Initialize CursorDrawer failed");
		return ScalingError::ScalingFailedGeneral;
	}

	if (!_overlayDrawer.Initialize(_frontendResources, overlayOptions)) {
		Logger::Get().Error("初始化 OverlayDrawer 失败");
		return ScalingError::ScalingFailedGeneral;
	}

	_ResetDLSSFGSlotEvents();
	_lastSynchronousPresentTime = {};
	_synchronousFramePresentationEnabled.store(true, std::memory_order_release);

	const ScalingOptions& options = ScalingWindow::Get().Options();
	if (!options.Is3DGameMode()) {
		_overlayDrawer.ToolbarState(options.IsWindowedMode() ?
			options.windowedInitialToolbarState : options.fullscreenInitialToolbarState);
	}

	_hKeyboardHook.reset(SetWindowsHookEx(WH_KEYBOARD_LL, _LowLevelKeyboardHook, NULL, 0));
	if (!_hKeyboardHook) {
		Logger::Get().Win32Warn("SetWindowsHookEx 失败");
	}

	return ScalingError::NoError;
}

void Renderer::OnCursorVisibilityChanged(bool isVisible, bool onDestory) {
	_backendThreadDispatcher.TryEnqueue([this, isVisible, onDestory]() {
		if (_frameSource) {
			_frameSource->OnCursorVisibilityChanged(isVisible, onDestory);
		}
	});
}

void Renderer::MessageHandler(UINT msg, WPARAM wParam, LPARAM lParam) noexcept {
	if (!_overlayDrawer.AnyVisibleWindow()) {
		return;
	}

	_overlayDrawer.MessageHandler(msg, wParam, lParam);

	// 有些鼠标操作需要渲染 ImGui 多次，见 https://github.com/ocornut/imgui/issues/2268
	if (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MOUSEWHEEL ||
		msg == WM_MOUSEHWHEEL || msg == WM_LBUTTONUP || msg == WM_RBUTTONUP) {
		Render();
	}
}

void Renderer::StartProfile() noexcept {
	_backendThreadDispatcher.TryEnqueue([this] {
		uint32_t passCount = 0;
		for (const EffectDesc* desc : _activeEffectDescs) {
			passCount += (uint32_t)desc->passes.size();
		}
		_effectsProfiler.Start(_backendResources.GetD3DDevice(), passCount);
	});
}

void Renderer::StopProfile() noexcept {
	_backendThreadDispatcher.TryEnqueue([this] {
		_effectsProfiler.Stop();
	});
}

winrt::fire_and_forget Renderer::TakeScreenshot(
	uint32_t effectIdx,
	uint32_t passIdx,
	uint32_t outputIdx
) noexcept {
	assert(effectIdx < _activeEffectDescs.size());

	if (!co_await _TakeScreenshotImpl(effectIdx, passIdx, outputIdx)) {
		Logger::Get().Error("_TakeScreenshotImpl 失败");
		ScalingWindow::Get().ShowToast(
			ScalingWindow::Get().GetLocalizedString(L"Message_ScreenshotFailed"));
	}
}

bool Renderer::_OpenFrontendSharedTextures() noexcept {
	for (uint32_t i = 0; i < MAX_SHARED_TEXTURE_SLOTS; ++i) {
		_frontendSharedTextureMutexes[i] = nullptr;
		_frontendSharedTextures[i] = nullptr;
		_lastAccessMutexKeys[i] = 0;
	}
	if (!_sharedTextureHandles[0]) {
		Logger::Get().Error("DLSSFG shared presentation slot has no handle");
		return false;
	}
	// 打开所有已分配的槽位，帧生成倍数可以在缩放期间改变
	for (uint32_t i = 0; i < MAX_SHARED_TEXTURE_SLOTS; ++i) {
		if (!_sharedTextureHandles[i]) {
			continue;
		}
		const HRESULT hr = _frontendResources.GetD3DDevice()->OpenSharedResource(
			_sharedTextureHandles[i],
			IID_PPV_ARGS(_frontendSharedTextures[i].put()));
		if (FAILED(hr)) {
			Logger::Get().ComError("Open shared presentation texture failed", hr);
			return false;
		}
		_frontendSharedTextureMutexes[i] =
			_frontendSharedTextures[i].try_as<IDXGIKeyedMutex>();
		if (!_frontendSharedTextureMutexes[i]) {
			Logger::Get().Error("Get shared presentation texture keyed mutex failed");
			return false;
		}
	}
	return true;
}

void Renderer::_ResetDLSSFGSlotEvents() noexcept {
	for (uint32_t i = 0; i < _sharedTextureSlotCount; ++i) {
		if (_sharedTextureAvailableEvents[i]) {
			SetEvent(_sharedTextureAvailableEvents[i].get());
		}
	}
}

bool Renderer::_FrontendRender(
	bool waitForGpu,
	uint32_t sharedTextureSlot,
	FrontendRenderTimings* timings
) noexcept {
	const auto beginFrameStart = std::chrono::steady_clock::now();
	if (sharedTextureSlot >= _sharedTextureSlotCount) {
		sharedTextureSlot = _latestSharedTextureSlot.load(std::memory_order_acquire);
	}
	ID3D11Texture2D* frontendSharedTexture =
		_frontendSharedTextures[sharedTextureSlot].get();
	IDXGIKeyedMutex* frontendSharedTextureMutex =
		_frontendSharedTextureMutexes[sharedTextureSlot].get();
	if (!frontendSharedTexture || !frontendSharedTextureMutex) {
		return false;
	}
	winrt::com_ptr<ID3D11Texture2D> frameTex;
	winrt::com_ptr<ID3D11RenderTargetView> frameRtv;
	POINT drawOffset;
	if (!_presenter->BeginFrame(frameTex, frameRtv, drawOffset)) {
		if (timings) {
			timings->beginFrame = std::chrono::steady_clock::now() - beginFrameStart;
		}
		return false;
	}
	const auto drawStart = std::chrono::steady_clock::now();
	if (timings) {
		timings->beginFrame = drawStart - beginFrameStart;
	}

	ID3D11DeviceContext4* d3dDC = _frontendResources.GetD3DDC();
	d3dDC->ClearState();

	// 所有渲染都使用三角形带拓扑
	d3dDC->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

	const RECT& rendererRect = ScalingWindow::Get().RendererRect();
	if (_destRect != rendererRect) {
		// 存在黑边时应以黑色填充背景。使用交换链呈现时需要这个操作，因为我们指定了 
		// DXGI_SWAP_EFFECT_FLIP_DISCARD，同时也是为了和 RTSS 兼容。使用 DirectComposition
		// 呈现时也需要这个操作，因为必须渲染所有像素。
		// 
		// 当渲染到 IDCompositionSurface 上时这个调用并不符合标准，文档说不应该在更新矩形外绘
		// 制。不过 Chromium 也是这么做的，而且声称这个操作只会影响更新矩形中的像素。见
		// https://github.com/chromium/chromium/blob/3653c48c3dc9ca9004f241a79238a1b3e0d0c633/gpu/command_buffer/service/shared_image/dcomp_surface_image_backing.cc#L63
		static constexpr FLOAT BLACK[4] = { 0.0f,0.0f,0.0f,1.0f };
		d3dDC->ClearRenderTargetView(frameRtv.get(), BLACK);
	}

	uint64_t& lastAccessMutexKey = _lastAccessMutexKeys[sharedTextureSlot];
	lastAccessMutexKey = ++_sharedTextureMutexKeys[sharedTextureSlot];
	HRESULT hr = frontendSharedTextureMutex->AcquireSync(
		lastAccessMutexKey - 1, INFINITE);
	if (FAILED(hr)) {
		Logger::Get().ComError("AcquireSync 失败", hr);
		return false;
	}

	{
		D3D11_TEXTURE2D_DESC desc;
		frameTex->GetDesc(&desc);
		if ((LONG)desc.Width == _destRect.right - _destRect.left
			&& (LONG)desc.Height == _destRect.bottom - _destRect.top) {
			d3dDC->CopyResource(frameTex.get(), frontendSharedTexture);
		} else {
			d3dDC->CopySubresourceRegion(
				frameTex.get(),
				0,
				drawOffset.x + _destRect.left - rendererRect.left,
				drawOffset.y + _destRect.top - rendererRect.top,
				0,
				frontendSharedTexture,
				0,
				nullptr
			);
		}
	}

	frontendSharedTextureMutex->ReleaseSync(lastAccessMutexKey);

	// 叠加层和光标都绘制到 back buffer
	{
		ID3D11RenderTargetView* t = frameRtv.get();
		d3dDC->OMSetRenderTargets(1, &t, nullptr);
	}

	// 绘制叠加层。ImGui 至少渲染两遍，否则经常有布局错误
	_overlayDrawer.Draw(2, _stepTimer.FPS(), _effectsProfiler.GetTimings(), drawOffset);

	// 绘制光标
	_cursorDrawer.Draw(frameTex.get(), drawOffset);

	const auto endFrameStart = std::chrono::steady_clock::now();
	if (timings) {
		timings->draw = endFrameStart - drawStart;
	}
	const bool submitted = _presenter->EndFrame(waitForGpu);
	if (timings) {
		timings->endFrame = std::chrono::steady_clock::now() - endFrameStart;
	}
	return submitted;
}

bool Renderer::Render(bool force, bool waitForGpu) noexcept {
	// In synchronous DLSSFG mode, the dedicated FIFO is the only presentation
	// ring consumer. Letting this regular path take the latest slot can consume
	// the real frame before older generated frames and break ordered playback.
	if (_dlssFrameGenerator &&
		_synchronousFramePresentationEnabled.load(std::memory_order_acquire)) {
		return false;
	}

	const uint32_t sharedTextureSlot = std::min(
		_latestSharedTextureSlot.load(std::memory_order_acquire),
		_sharedTextureSlotCount - 1);
	const bool hasNewBackendFrame =
		_lastAccessMutexKeys[sharedTextureSlot] !=
		_sharedTextureMutexKeys[sharedTextureSlot].load(std::memory_order_relaxed);
	if (!force && !hasNewBackendFrame) {
		if (_lastAccessMutexKeys[sharedTextureSlot] == 0) {
			// 第一帧尚未完成
			return false;
		}
		// XeSSFG owns the presenting swap chain. Presenting only because Magpie's
		// software cursor or overlay changed would make the SDK treat the same
		// captured colour frame as a new game frame and inflate its input rate.
		// Wait for the next genuinely new backend texture instead.
		if (_isXeSSFrameGenerationActive) {
			if (!_xessFgFrontendSuppressionLogged) {
				_xessFgFrontendSuppressionLogged = true;
				Logger::Get().Info(
					"XeSSFG: suppressing cursor-only frontend presents until a new captured frame");
			}
			return false;
		}

		if (!_cursorDrawer.NeedRedraw() && !_overlayDrawer.NeedRedraw(_stepTimer.FPS())) {
			return false;
		}
	}

	return _FrontendRender(waitForGpu, sharedTextureSlot);
}

void Renderer::_RecordDLSSFGFrontendTimings(
	bool usesFrameLatencyWaitableObject,
	std::chrono::nanoseconds pacingWait,
	const FrontendRenderTimings& timings
) noexcept {
	if (!_dlssFgFrontendTimingModeInitialized ||
		_dlssFgFrontendTimingUsesWaitableObject != usesFrameLatencyWaitableObject) {
		_dlssFgFrontendTimingModeInitialized = true;
		_dlssFgFrontendTimingUsesWaitableObject = usesFrameLatencyWaitableObject;
		_dlssFgFrontendTimingFrames = 0;
		_dlssFgFrontendPacingWait = {};
		_dlssFgFrontendBeginFrame = {};
		_dlssFgFrontendDraw = {};
		_dlssFgFrontendEndFrame = {};
		_dlssFgRingWaitNanoseconds.exchange(0, std::memory_order_relaxed);
		_dlssFgRingWaitSamples.exchange(0, std::memory_order_relaxed);
	}

	++_dlssFgFrontendTimingFrames;
	_dlssFgFrontendPacingWait += pacingWait;
	_dlssFgFrontendBeginFrame += timings.beginFrame;
	_dlssFgFrontendDraw += timings.draw;
	_dlssFgFrontendEndFrame += timings.endFrame;
	if (_dlssFgFrontendTimingFrames < 120) {
		return;
	}

	const uint64_t ringWaitNanoseconds =
		_dlssFgRingWaitNanoseconds.exchange(0, std::memory_order_relaxed);
	const uint64_t ringWaitSamples =
		_dlssFgRingWaitSamples.exchange(0, std::memory_order_relaxed);
	const auto averageMilliseconds = [frames = _dlssFgFrontendTimingFrames](
		std::chrono::nanoseconds total) noexcept {
		return std::chrono::duration<double, std::milli>(total).count() / frames;
	};
	const double averageRingWaitMilliseconds = ringWaitSamples ?
		double(ringWaitNanoseconds) / 1'000'000.0 / ringWaitSamples : 0.0;
	Logger::Get().Info(fmt::format(
		"DLSSFG frontend timing: mode={} frames={} paceWait={:.3f} ms "
		"beginFrame={:.3f} ms draw={:.3f} ms endFrame={:.3f} ms "
		"ringWait={:.3f} ms",
		usesFrameLatencyWaitableObject ? "deadline+DXGI" : "DWM",
		_dlssFgFrontendTimingFrames,
		averageMilliseconds(_dlssFgFrontendPacingWait),
		averageMilliseconds(_dlssFgFrontendBeginFrame),
		averageMilliseconds(_dlssFgFrontendDraw),
		averageMilliseconds(_dlssFgFrontendEndFrame),
		averageRingWaitMilliseconds));

	_dlssFgFrontendTimingFrames = 0;
	_dlssFgFrontendPacingWait = {};
	_dlssFgFrontendBeginFrame = {};
	_dlssFgFrontendDraw = {};
	_dlssFgFrontendEndFrame = {};
}

bool Renderer::RenderDLSSFGFrame(
	uint32_t sharedTextureSlot,
	uint32_t sharedTextureGeneration
) noexcept {
	if (sharedTextureGeneration !=
		_sharedTextureGeneration.load(std::memory_order_acquire) ||
		sharedTextureSlot >= _sharedTextureSlotCount) {
		return false;
	}
	if (!_synchronousFramePresentationEnabled.load(std::memory_order_acquire)) {
		if (_sharedTextureAvailableEvents[sharedTextureSlot]) {
			SetEvent(_sharedTextureAvailableEvents[sharedTextureSlot].get());
		}
		return false;
	}

	const bool usesFrameLatencyWaitableObject =
		_presenter->UsesFrameLatencyWaitableObject();
	const auto pacingStart = std::chrono::steady_clock::now();
	const auto targetTime = _lastSynchronousPresentTime.time_since_epoch().count() ?
		_lastSynchronousPresentTime + _synchronousPresentInterval : pacingStart;
	if (usesFrameLatencyWaitableObject) {
		// BeginFrame owns swap-chain capacity pacing. This deadline controls only
		// the requested output cadence; adding a DWM wait here halves the usable
		// presentation rate on some high-refresh composition paths.
		if (_synchronousPresentInterval.count() > 0) {
			WaitUntilHighResolution(targetTime);
		}
	} else {
		// DirectComposition-style presenters do not expose a frame-latency
		// waitable object, so the compositor clock remains their pacing source.
		do {
			Win32Helper::WaitForDwmComposition();
		} while (_synchronousFramePresentationEnabled.load(std::memory_order_acquire) &&
			_synchronousPresentInterval.count() > 0 &&
			std::chrono::steady_clock::now() < targetTime);
	}
	const auto pacingEnd = std::chrono::steady_clock::now();

	FrontendRenderTimings timings;
	const bool presented = _FrontendRender(false, sharedTextureSlot, &timings);
	const auto presentEnd = std::chrono::steady_clock::now();
	if (_synchronousPresentInterval.count() > 0) {
		// Keep a stable deadline when on time, but do not issue burst catch-up
		// presents after a stall longer than one output interval.
		_lastSynchronousPresentTime =
			presentEnd > targetTime + _synchronousPresentInterval ? presentEnd : targetTime;
	} else {
		_lastSynchronousPresentTime = presentEnd;
	}
	if (_sharedTextureAvailableEvents[sharedTextureSlot]) {
		SetEvent(_sharedTextureAvailableEvents[sharedTextureSlot].get());
	}
	if (presented) {
		_RecordDLSSFGFrontendTimings(
			usesFrameLatencyWaitableObject, pacingEnd - pacingStart, timings);
	}
	return presented;
}

bool Renderer::OnResize() noexcept {
	_synchronousFramePresentationEnabled.store(false, std::memory_order_release);

	if (!_presenter->OnResize()) {
		Logger::Get().Error("更改呈现器尺寸失败");
		return false;
	}

	_sharedTextureHandle.store(NULL, std::memory_order_relaxed);

	_backendThreadDispatcher.TryEnqueue([this]() {
		ID3D11Texture2D* outputTexture = _ResizeEffects();
		if (!outputTexture) {
			Logger::Get().Win32Error("_ResizeEffects 失败");
			_sharedTextureHandle.store(INVALID_HANDLE_VALUE, std::memory_order_relaxed);
			_sharedTextureHandle.notify_one();
			return;
		}

		HANDLE sharedHandle = _CreateSharedTexture(outputTexture);
		if (!sharedHandle) {
			Logger::Get().Win32Error("_CreateSharedTexture 失败");
			_sharedTextureHandle.store(INVALID_HANDLE_VALUE, std::memory_order_relaxed);
			_sharedTextureHandle.notify_one();
			return;
		}

		// 渲染完成再通知前端防止黑屏。前端会自动执行渲染，因此无需发送 WM_FRONTEND_RENDER
		_BackendRender(outputTexture, false);

		_sharedTextureHandle.store(sharedHandle, std::memory_order_release);
		_sharedTextureHandle.notify_one();
	});

	// 等待后端更改分辨率和渲染
	_sharedTextureHandle.wait(NULL, std::memory_order_relaxed);
	// 将三个成员同步到前端线程
	const HANDLE sharedTextureHandle = _sharedTextureHandle.load(std::memory_order_acquire);
	if (sharedTextureHandle == INVALID_HANDLE_VALUE) {
		return false;
	}

	if (!_OpenFrontendSharedTextures()) {
		return false;
	}
	_ResetDLSSFGSlotEvents();
	_lastSynchronousPresentTime = {};
	_synchronousFramePresentationEnabled.store(true, std::memory_order_release);

	_UpdateDestRect();
	return true;
}

void Renderer::OnEndResize() noexcept {
	bool shouldRedraw = false;
	_presenter->OnEndResize(shouldRedraw);

	if (shouldRedraw) {
		_FrontendRender();
	}
}

void Renderer::OnMove() noexcept {
	_UpdateDestRect();
}

void Renderer::SwitchToolbarState() noexcept {
	const ScalingWindow& scalingWindow = ScalingWindow::Get();

	if (scalingWindow.Options().Is3DGameMode()) {
		scalingWindow.ShowToast(scalingWindow.GetLocalizedString(L"Message_ToolbarIn3DGameMode"));
		return;
	}

	const ToolbarState newState = ToolbarState(
		((uint32_t)_overlayDrawer.ToolbarState() + 1) % (uint32_t)ToolbarState::COUNT);
	_overlayDrawer.ToolbarState(newState);

	// 显示状态切换消息
	const wchar_t* stateResName = nullptr;
	if (newState == ToolbarState::Off) {
		stateResName = L"Home_Toolbar_InitialState_Off/Content";
	} else if (newState == ToolbarState::AlwaysShow) {
		stateResName = L"Home_Toolbar_InitialState_AlwaysShow/Content";
	} else {
		stateResName = L"Home_Toolbar_InitialState_AutoHide/Content";
	}

	winrt::hstring newStateMsg = scalingWindow.GetLocalizedString(L"Message_ToolbarNewState");
	scalingWindow.ShowToast(fmt::format(
		fmt::runtime(std::wstring_view(newStateMsg)),
		std::wstring_view(scalingWindow.GetLocalizedString(stateResName))
	));

	// 立即渲染一帧
	_FrontendRender();
}

const RECT& Renderer::SrcRect() const noexcept {
	return ScalingWindow::Get().SrcTracker().SrcRect();
}

bool Renderer::_InitFrameSource() noexcept {
	switch (ScalingWindow::Get().Options().captureMethod) {
	case CaptureMethod::GraphicsCapture:
		_frameSource = std::make_unique<GraphicsCaptureFrameSource>();
		break;
	case CaptureMethod::DesktopDuplication:
		_frameSource = std::make_unique<DesktopDuplicationFrameSource>();
		break;
	case CaptureMethod::GDI:
		_frameSource = std::make_unique<GDIFrameSource>();
		break;
	case CaptureMethod::DwmSharedSurface:
		_frameSource = std::make_unique<DwmSharedSurfaceFrameSource>();
		break;
	default:
		Logger::Get().Error("未知的捕获模式");
		return false;
	}

	Logger::Get().Info(StrHelper::Concat("当前捕获模式: ", _frameSource->Name()));

	const bool forceDuplicateFrameDetection = std::ranges::any_of(
		ScalingWindow::Get().Options().effects,
		[](const EffectOption& effect) { return IsFrameGenerationEffect(effect.name); });
	_frameSource->ForceDuplicateFrameDetection(forceDuplicateFrameDetection);
	if (forceDuplicateFrameDetection) {
		Logger::Get().Info(
			"Frame Generation: exact duplicate-frame filtering forced for captured input");
	}

	if (!_frameSource->Initialize(_backendResources, _backendDescriptorStore)) {
		Logger::Get().Error("初始化 FrameSource 失败");
		_backendInitError = ScalingError::CaptureFailed;
		return false;
	}

	// 由于 DPI 缩放，捕获尺寸和边界矩形尺寸不一定相同
	D3D11_TEXTURE2D_DESC desc;
	_frameSource->GetOutput()->GetDesc(&desc);
	Logger::Get().Info(fmt::format("捕获尺寸: {}x{}", desc.Width, desc.Height));

	return true;
}

static std::optional<EffectDesc> CompileEffect(
	const EffectOption& effectOption,
	bool noFP16,
	bool forceInlineParams = false
) noexcept {
	// 指定效果名
	EffectDesc result{ .name = effectOption.name };

	uint32_t compileFlag = 0;
	const ScalingOptions& scalingOptions = ScalingWindow::Get().Options();
	if (scalingOptions.IsEffectCacheDisabled()) {
		compileFlag |= EffectCompilerFlags::NoCache;
	}
	if (scalingOptions.IsSaveEffectSources()) {
		compileFlag |= EffectCompilerFlags::SaveSources;
	}
	if (scalingOptions.IsWarningsAreErrors()) {
		compileFlag |= EffectCompilerFlags::WarningsAreErrors;
	}
	if (scalingOptions.IsInlineParams() || forceInlineParams) {
		compileFlag |= EffectCompilerFlags::InlineParams;
	}
	if (noFP16) {
		compileFlag |= EffectCompilerFlags::NoFP16;
	}

	bool success = true;
	uint32_t duration = Measure([&]() {
		success = !EffectCompiler::Compile(result, compileFlag, &effectOption.parameters);
	});

	if (success) {
		Logger::Get().Info(fmt::format("编译 {}.hlsl 用时 {} 毫秒",
			effectOption.name, duration / 1000.0f));
		return result;
	} else {
		Logger::Get().Error(StrHelper::Concat("编译 ",
			effectOption.name, ".hlsl 失败"));
		return std::nullopt;
	}
}

ID3D11Texture2D* Renderer::_BuildEffects() noexcept {
	const ScalingOptions& options = ScalingWindow::Get().Options();
	const bool noFP16 = !_backendResources.IsFP16Supported() || options.IsFP16Disabled();

	// _effectOptions 是 options.effects 的副本，叠加层可以实时修改其中的参数
	const std::vector<EffectOption>& effects = _effectOptions;
	assert(!effects.empty());
	const uint32_t effectCount = (uint32_t)effects.size();

	// 并行编译所有效果
	_effectDescs.resize(effects.size());
	bool anyFailure = false;
	wil::srwlock writeLock;
	
	int duration = Measure([&]() {
		Win32Helper::RunParallel([&](uint32_t id) {
			std::optional<EffectDesc> desc = CompileEffect(effects[id], noFP16);

			auto lk = writeLock.lock_exclusive();
			if (desc) {
				_effectDescs[id] = std::move(*desc);
			} else {
				anyFailure = true;
			}
		}, effectCount);
	});

	if (anyFailure) {
		return nullptr;
	}

	if (effectCount > 1) {
		Logger::Get().Info(fmt::format("编译着色器总计用时 {} 毫秒", duration / 1000.0f));
	}

	_ReleaseNgxConsumers();
	_effectDrawers.resize(effectCount);
	_nativeEffectBackends.resize(effectCount);
	_dlssFgConsecutiveFailures = 0;
	_dlssFgRecoveryAttempts = 0;
	std::optional<DLSSFrameGenerationSettings> dlssFrameGenerationSettings;

	ID3D11Texture2D* inOutTexture = _frameSource->GetOutput();
	for (uint32_t i = 0; i < effectCount; ++i) {
		if (!_effectDrawers[i].Initialize(
			_effectDescs[i],
			effects[i],
			_backendResources,
			_backendDescriptorStore,
			&inOutTexture
		)) {
			Logger::Get().Error(fmt::format("初始化效果#{} ({}) 失败", i, effects[i].name));
			return nullptr;
		}

		NativeEffectBackendResult nativeBackend = CreateNativeEffectBackend(
			effects[i].name,
			effects[i],
			_backendResources,
			_ngxD3D12Core,
			_effectDrawers[i].GetTexture(0),
			_effectDrawers[i].GetOutputTexture());
		if (nativeBackend.recognized && !nativeBackend.backend) {
			return nullptr;
		}
		_nativeEffectBackends[i] = std::move(nativeBackend.backend);

		if (IsDLSSFrameGenerationEffect(effects[i].name)) {
			if (dlssFrameGenerationSettings) {
				Logger::Get().Error("Only one DLSS Frame Generation effect is allowed");
				return nullptr;
			}
			dlssFrameGenerationSettings =
				ParseDLSSFrameGenerationSettings(effects[i]);
		}

		// 释放 CSO 内存，不再需要它们
		for (EffectPassDesc& passDesc : _effectDescs[i].passes) {
			passDesc.cso = nullptr;
		}
	}
	
	_canEditEffectParametersLive.assign(effectCount, false);
	for (uint32_t i = 0; i < effectCount; ++i) {
		// 帧生成的参数决定共享纹理数量和呈现方式，只能在初始化时确定；内联参数
		// 被编译进着色器，但原生后端直接读取参数，不受影响
		// XeSS 帧生成通过独立的呈现器实现，其参数无法实时切换
		_canEditEffectParametersLive[i] = !IsXeSSFrameGenerationEffect(effects[i].name) &&
			(_nativeEffectBackends[i] || IsDLSSFrameGenerationEffect(effects[i].name) ||
				IsFrameRateFilterEffect(effects[i].name) ||
				!(_effectDescs[i].flags & EffectFlags::InlineParams));
	}

	if (_ShouldAppendBicubic(inOutTexture)) {
		if (!_AppendBicubic(&inOutTexture)) {
			Logger::Get().Error("_AppendBicubic 失败");
			return nullptr;
		}
	}

	if (dlssFrameGenerationSettings &&
		!_InitializeDLSSFrameGenerator(
			inOutTexture, *dlssFrameGenerationSettings)) {
		return nullptr;
	}

	_UpdateActiveEffectDescs();

	// 初始化所有效果共用的动态常量缓冲区
	for (const EffectDesc& effectDesc : _effectDescs) {
		if (effectDesc.flags & EffectFlags::UseDynamic) {
			D3D11_BUFFER_DESC bd{
				.ByteWidth = 16,	// 只用 4 个字节
				.Usage = D3D11_USAGE_DYNAMIC,
				.BindFlags = D3D11_BIND_CONSTANT_BUFFER,
				.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE
			};

			HRESULT hr = _backendResources.GetD3DDevice()->CreateBuffer(&bd, nullptr, _dynamicCB.put());
			if (FAILED(hr)) {
				Logger::Get().ComError("CreateBuffer 失败", hr);
				return nullptr;
			}

			break;
		}
	}

	return inOutTexture;
}

void Renderer::SetEffectParameter(
	uint32_t effectIdx,
	std::string parameterName,
	float value
) noexcept {
	if (!_backendThreadDispatcher) {
		return;
	}

	// 参数只能在后台线程更新，拖动滑块产生的多次修改会合并到下一帧一并应用
	_backendThreadDispatcher.TryEnqueue([this, effectIdx,
		parameterName(std::move(parameterName)), value]() {
		if (effectIdx >= _effectOptions.size()) {
			return;
		}

		_effectOptions[effectIdx].parameters[parameterName] = value;

		if (std::ranges::find(_dirtyEffectParameters, effectIdx) ==
			_dirtyEffectParameters.end()) {
			_dirtyEffectParameters.push_back(effectIdx);
		}
	});
}

void Renderer::_ApplyPendingEffectParameters() noexcept {
	for (const uint32_t effectIdx : _dirtyEffectParameters) {
		if (effectIdx >= _effectOptions.size()) {
			continue;
		}

		const EffectOption& option = _effectOptions[effectIdx];

		if (IsFrameRateFilterEffect(option.name)) {
			// 目标帧率同时影响 StepTimer 和帧生成的呈现节奏
			_UpdateFrameRateLimits();
			continue;
		}

		if (IsDLSSFrameGenerationEffect(option.name)) {
			_RebuildDLSSFrameGenerator();
			continue;
		}

		if (IsXeSSFrameGenerationEffect(option.name)) {
			// 叠加层会禁用这些参数，正常情况下不会执行到这里
			continue;
		}

		if (_nativeEffectBackends[effectIdx]) {
			if (_nativeEffectBackends[effectIdx]->UpdateParameters(option) ==
				NativeEffectParameterUpdate::NeedsRecreate) {
				_RecreateNativeEffectBackend(effectIdx);
			}
		} else if (_effectDescs[effectIdx].flags & EffectFlags::InlineParams) {
			// 参数已被编译进着色器，需重新编译整个效果链才能生效。叠加层会禁用
			// 这些参数，正常情况下不会执行到这里
			Logger::Get().Warn(fmt::format(
				"效果#{} ({}) 内联了参数，无法实时调整", effectIdx, option.name));
			continue;
		} else if (!_effectDrawers[effectIdx].UpdateParameters(
			_effectDescs[effectIdx], option, _backendResources)) {
			Logger::Get().Error(fmt::format(
				"更新效果#{} ({}) 的参数失败", effectIdx, option.name));
		}
	}

	_dirtyEffectParameters.clear();

	// 帧引导的提供器只能在初始化时创建，之后无法补充
	if (!_loggedFrameGuidanceUnavailable) {
		const FrameGuidanceRequirements requirements =
			CollectFrameGuidanceRequirements(
				_nativeEffectBackends, _dlssFrameGenerator.get());
		if ((requirements.motion &&
				!_frameGuidanceService.HasMotionVectorProvider()) ||
			(requirements.depth && !_frameGuidanceService.HasDepthProvider())) {
			_loggedFrameGuidanceUnavailable = true;
			Logger::Get().Warn("Frame Guidance: provider unavailable for the new "
				"parameters, restart scaling to apply");
		}
	}

	// 源窗口静止时也应立刻看到调整结果
	_forceNextRender = true;
}

bool Renderer::_RebuildDLSSFrameGenerator() noexcept {
	if (!_dlssFrameGenerator) {
		// 本次缩放已禁用帧生成，重建没有意义
		return false;
	}

	const auto it = std::ranges::find_if(_effectOptions,
		[](const EffectOption& effect) {
			return IsDLSSFrameGenerationEffect(effect.name);
		});
	if (it == _effectOptions.end()) {
		return false;
	}

	// 呈现环按最大槽位数分配，因此倍数改变无需重建共享纹理
	if (!_InitializeDLSSFrameGenerator(
		_effectDrawers.back().GetOutputTexture(),
		ParseDLSSFrameGenerationSettings(*it))) {
		Logger::Get().Error("Rebuild DLSSFG with the new parameters failed");
		_DisableDLSSFrameGenerationForSession();
		return false;
	}

	_sharedTextureSlotCount = std::clamp(
		_dlssFrameGenerator->Multiplier(), 2u, MAX_SHARED_TEXTURE_SLOTS);
	_dlssFgConsecutiveFailures = 0;
	_dlssFgRecoveryAttempts = 0;
	_ResetDLSSFGSlotEvents();
	_lastSynchronousPresentTime = {};

	Logger::Get().Info(fmt::format(
		"Rebuilt DLSSFG with the new parameters: multiplier={}x slots={}",
		_dlssFrameGenerator->Multiplier(), _sharedTextureSlotCount));
	return true;
}

void Renderer::_UpdateSynchronousPresentInterval() noexcept {
	if (_dlssFrameGenerator && _frameRateFilterTarget > 0.0f) {
		const double outputFrameRate =
			double(_frameRateFilterTarget) * _dlssFrameGenerator->Multiplier();
		_synchronousPresentInterval = std::chrono::nanoseconds(
			(int64_t)std::llround(1'000'000'000.0 / outputFrameRate));
	} else {
		_synchronousPresentInterval = {};
	}
}

void Renderer::_UpdateFrameRateLimits() noexcept {
	const ScalingOptions& options = ScalingWindow::Get().Options();

	std::optional<float> maxFrameRate = _captureMaxFrameRate;

	_frameRateFilterTarget = 0.0f;
	for (const EffectOption& effect : _effectOptions) {
		if (!IsFrameRateFilterEffect(effect.name)) {
			continue;
		}

		const float targetFrameRate = ParseFrameRateFilterTarget(effect);
		if (!maxFrameRate || targetFrameRate < *maxFrameRate) {
			maxFrameRate = targetFrameRate;
		}
		_frameRateFilterTarget = _frameRateFilterTarget == 0.0f
			? targetFrameRate
			: std::min(_frameRateFilterTarget, targetFrameRate);
		Logger::Get().Info(fmt::format(
			"Frame Rate Filter enabled: {} FPS", targetFrameRate));
	}

	if (options.maxFrameRate) {
		if (!maxFrameRate || *options.maxFrameRate < *maxFrameRate) {
			maxFrameRate = options.maxFrameRate;
		}
	}

	// 测试着色器性能时最小帧率应设为无限大，但由于 /fp:fast 下无限大不可靠，因此改为使用 max()，
	// 和无限大效果相同。
	const bool useFrameGeneration = std::ranges::any_of(
		_effectOptions,
		[](const EffectOption& effect) { return IsFrameGenerationEffect(effect.name); });
	const float minFrameRate = useFrameGeneration
		? 0.0f
		: (options.IsBenchmarkMode()
			? std::numeric_limits<float>::max() : options.minFrameRate);
	if (useFrameGeneration &&
		(options.minFrameRate > 0 || options.IsBenchmarkMode())) {
		Logger::Get().Info(
			"Frame Generation: minimum-FPS duplicate frame synthesis disabled");
	}

	_stepTimer.Initialize(minFrameRate, maxFrameRate);
	_UpdateSynchronousPresentInterval();
}

bool Renderer::_RecreateNativeEffectBackend(uint32_t effectIdx) noexcept {
	const EffectOption& option = _effectOptions[effectIdx];

	if (!_DrainNgxConsumers()) {
		Logger::Get().Error("Drain NGX consumers before recreating native effect failed");
		return false;
	}

	// 必须先释放旧后端，SDK 特征独占资源
	_nativeEffectBackends[effectIdx].reset();

	NativeEffectBackendResult result = CreateNativeEffectBackend(
		option.name,
		option,
		_backendResources,
		_ngxD3D12Core,
		_effectDrawers[effectIdx].GetTexture(0),
		_effectDrawers[effectIdx].GetOutputTexture()
	);
	if (!result.backend) {
		// 重建失败时该效果退化为直通，避免中断缩放
		Logger::Get().Error(fmt::format(
			"Recreate native effect {} failed", option.name));
		return false;
	}

	_nativeEffectBackends[effectIdx] = std::move(result.backend);
	Logger::Get().Info(fmt::format(
		"Recreated native effect {} with new parameters", option.name));
	return true;
}

void Renderer::_UpdateActiveEffectDescs() noexcept {
	const uint32_t effectCount = (uint32_t)_effectDescs.size();
	const uint32_t drawerCount = (uint32_t)_effectDrawers.size();

	_activeEffectDescs.resize(drawerCount);

	for (uint32_t i = 0; i < effectCount; ++i) {
		_activeEffectDescs[i] = &_effectDescs[i];
	}

	if (drawerCount > effectCount) {
		// 已追加 Bicubic
		assert(drawerCount == effectCount + 1);
		_activeEffectDescs[effectCount] = &bicubicDesc;
	}
}

bool Renderer::_ShouldAppendBicubic(ID3D11Texture2D* outTexture) noexcept {
	const ScalingOptions& options = ScalingWindow::Get().Options();

	D3D11_TEXTURE2D_DESC texDesc;
	outTexture->GetDesc(&texDesc);
	const SIZE lastOutputSize = { (LONG)texDesc.Width, (LONG)texDesc.Height };
	const SIZE rendererSize = Win32Helper::GetSizeOfRect(ScalingWindow::Get().RendererRect());

	if (options.IsWindowedMode()) {
		// 窗口模式缩放时使用 Bicubic 放大。Bicubic (B=0, C=0.5) 的锐利度和 Lanczos 相差无几
		return lastOutputSize != rendererSize;
	} else {
		// 输出尺寸大于交换链尺寸则需要降采样
		return lastOutputSize.cx > rendererSize.cx || lastOutputSize.cy > rendererSize.cy;
	}
}

bool Renderer::_AppendBicubic(ID3D11Texture2D** inOutTexture) noexcept {
	const ScalingOptions& options = ScalingWindow::Get().Options();

	const EffectOption bicubicOption{
		.name = "Bicubic",
		.parameters{
			{"paramB", 0.0f},
			{"paramC", 0.5f}
		},
		.scalingType = options.IsWindowedMode() ? ScalingType::Fill : ScalingType::Fit
	};

	if (bicubicDesc.name.empty()) {
		// 参数不会改变，因此可以内联
		std::optional<EffectDesc> desc = CompileEffect(bicubicOption, true, true);
		if (!desc) {
			Logger::Get().Error("编译降采样效果失败");
			return false;
		}

		bicubicDesc = std::move(*desc);
	}

	EffectDrawer& bicubicDrawer = _effectDrawers.emplace_back();
	if (!bicubicDrawer.Initialize(
		bicubicDesc,
		bicubicOption,
		_backendResources,
		_backendDescriptorStore,
		inOutTexture
	)) {
		Logger::Get().Error("初始化降采样效果失败");
		return false;
	}

	return true;
}

ID3D11Texture2D* Renderer::_ResizeEffects() noexcept {
	const ScalingOptions& options = ScalingWindow::Get().Options();
	// 使用副本而不是 options.effects，否则实时调整的参数会被还原
	const std::vector<EffectOption>& effects = _effectOptions;
	assert(!effects.empty());
	const uint32_t effectCount = (uint32_t)effects.size();
	if (!_DrainNgxConsumers()) {
		Logger::Get().Error("Drain NGX consumers before resize failed");
		return nullptr;
	}

	ID3D11Texture2D* inOutTexture = _frameSource->GetOutput();
	D3D11_TEXTURE2D_DESC sourceDesc{};
	inOutTexture->GetDesc(&sourceDesc);
	const FrameGuidanceRequirements guidanceRequirements =
		CollectFrameGuidanceRequirements(
			_nativeEffectBackends, _dlssFrameGenerator.get());
	if (_frameGuidanceService.IsInitialized() && !_frameGuidanceService.Resize(
		{ sourceDesc.Width, sourceDesc.Height }, _capturedFrameId,
		guidanceRequirements)) {
		Logger::Get().Error("Resize Frame Guidance service failed");
		return nullptr;
	}
	for (uint32_t i = 0; i < effectCount; ++i) {
		if (!_effectDrawers[i].ResizeTextures(
			_effectDescs[i],
			effects[i],
			_backendResources,
			&inOutTexture
		)) {
			Logger::Get().Error(fmt::format("更改效果#{} ({}) 尺寸失败", i, effects[i].name));
			return nullptr;
		}

		if (_nativeEffectBackends[i] && !_nativeEffectBackends[i]->Resize(
			_backendResources, _effectDrawers[i].GetTexture(0), _effectDrawers[i].GetOutputTexture())) {
			if (effects[i].name == "DLSSNR\\DLSSNR_AI_Filter") {
				const char status[] =
					"DLSSNR STATUS: Feature=18 created=false stage=resize "
					"fallback=pass-through";
				Logger::Get().Warn(status);
				OutputDebugStringA(status);
				_nativeEffectBackends[i].reset();
				continue;
			}
			Logger::Get().Error(fmt::format("Resize native effect {} failed", effects[i].name));
			return nullptr;
		}
	}

	// 处理追加的 Bicubic
	bool changed = false;
	if (_ShouldAppendBicubic(inOutTexture)) {
		if (_effectDrawers.size() > effectCount) {
			const EffectOption bicubicOption{
				.name = "Bicubic",
				.parameters{
					{"paramB", 0.0f},
					{"paramC", 0.5f}
				},
				.scalingType = options.IsWindowedMode() ? ScalingType::Fill : ScalingType::Fit
			};

			if (!_effectDrawers.back().ResizeTextures(
				bicubicDesc,
				bicubicOption,
				_backendResources,
				&inOutTexture
			)) {
				Logger::Get().Error("更改效果 Bicubic 尺寸失败");
				return nullptr;
			}
		} else {
			_AppendBicubic(&inOutTexture);
			changed = true;
		}
	} else {
		if (_effectDrawers.size() > effectCount) {
			_effectDrawers.resize(effectCount);
			changed = true;
		}
	}

	if (changed) {
		_UpdateActiveEffectDescs();
		_overlayDrawer.UpdateAfterActiveEffectsChanged();

		if (_effectsProfiler.IsProfiling()) {
			uint32_t passCount = 0;
			for (const EffectDesc* desc : _activeEffectDescs) {
				passCount += (uint32_t)desc->passes.size();
			}
			_effectsProfiler.SetPassCount(_backendResources.GetD3DDevice(), passCount);
		}
	}

	if (_dlssFrameGenerator) {
		const DLSSFrameGenerationSettings settings =
			_dlssFrameGenerator->Settings();
		if (!_InitializeDLSSFrameGenerator(inOutTexture, settings)) {
			return nullptr;
		}
	}

	return inOutTexture;
}

bool Renderer::_InitializeDLSSFrameGenerator(
	ID3D11Texture2D* input,
	const DLSSFrameGenerationSettings& settings
) noexcept {
	if (_dlssFrameGenerator) {
		if (!_dlssFrameGenerator->Drain()) {
			Logger::Get().Warn("Drain old DLSSFG feature before rebuild failed");
			return false;
		}
		_dlssFrameGenerator.reset();
	}
	D3D11_TEXTURE2D_DESC sourceDesc{};
	_frameSource->GetOutput()->GetDesc(&sourceDesc);
	auto frameGenerator = std::make_unique<DLSSFrameGenerator>();
	if (!frameGenerator->Initialize(
		_backendResources, _ngxD3D12Core, input,
		{ sourceDesc.Width, sourceDesc.Height }, settings)) {
		return false;
	}
	const double refreshRate = GetDisplayRefreshRate(ScalingWindow::Get().Handle());
	const double theoreticalOutput = _frameRateFilterTarget > 0.0f ?
		double(_frameRateFilterTarget) * frameGenerator->Multiplier() : 0.0;
	Logger::Get().Info(fmt::format(
		"DLSSFG capacity: displayRefresh={:.3f} Hz baseTarget={} requested={}x "
		"sdkMax={}x active={}x theoreticalOutput={}",
		refreshRate,
		_frameRateFilterTarget > 0.0f ?
			fmt::format("{:.3f} FPS", _frameRateFilterTarget) : "unlimited",
		settings.multiplier, frameGenerator->MaxSupportedMultiplier(),
		frameGenerator->Multiplier(),
		theoreticalOutput > 0.0 ?
			fmt::format("{:.3f} FPS", theoreticalOutput) : "capture-driven"));
	if (refreshRate > 0.0 && theoreticalOutput > refreshRate * 1.05) {
		Logger::Get().Warn(fmt::format(
			"DLSSFG requested output {:.1f} FPS exceeds the {:.1f} Hz display; "
			"higher multipliers may reduce real-frame rate without increasing "
			"visible presentation rate",
			theoreticalOutput, refreshRate));
	}
	_dlssFgDiagnosticsStart = {};
	_dlssFgCapturedFrameCount = 0;
	_dlssFgPresentedFrameCount = 0;
	_dlssFgGeneratedPublishSuccess = 0;
	_dlssFgGeneratedPublishFailure = 0;
	_dlssFgRealPublishSuccess = 0;
	_dlssFgRealPublishFailure = 0;
	_dlssFrameGenerator = std::move(frameGenerator);
	_UpdateSynchronousPresentInterval();
	return true;
}

void Renderer::_HandleDLSSFrameGenerationFailure(ID3D11Texture2D* input) noexcept {
	if (!_dlssFrameGenerator) {
		return;
	}

	++_dlssFgConsecutiveFailures;
	if (_dlssFgConsecutiveFailures == 1) {
		Logger::Get().Warn(
			"DLSS Frame Generation failed; resetting history and presenting real frames");
		_dlssFrameGenerator->RequestHistoryReset();
		return;
	}

	if (_dlssFgRecoveryAttempts == 0) {
		++_dlssFgRecoveryAttempts;
		const DLSSFrameGenerationSettings settings =
			_dlssFrameGenerator->Settings();
		Logger::Get().Warn("DLSS Frame Generation failed again; recreating the feature once");
		if (_InitializeDLSSFrameGenerator(input, settings)) {
			_dlssFgConsecutiveFailures = 0;
			return;
		}
	}

	_DisableDLSSFrameGenerationForSession();
}

void Renderer::_DisableDLSSFrameGenerationForSession() noexcept {
	if (_dlssFrameGenerator && !_dlssFrameGenerator->Drain()) {
		Logger::Get().Warn("Drain DLSSFG queue before disabling failed");
	}
	_dlssFrameGenerator.reset();
	_synchronousPresentInterval = {};
	Logger::Get().Error(
		"DLSS Frame Generation was disabled for this scaling session after repeated failures");
}

bool Renderer::_DrainNgxConsumers() noexcept {
	bool succeeded = true;
	if (_dlssFrameGenerator && !_dlssFrameGenerator->Drain()) {
		Logger::Get().Warn("Drain DLSSFG queue failed");
		succeeded = false;
	}
	for (const auto& backend : _nativeEffectBackends) {
		if (backend && !backend->Drain()) {
			Logger::Get().Warn("Drain native NGX backend queue failed");
			succeeded = false;
		}
	}
	return succeeded;
}

void Renderer::_ReleaseNgxConsumers() noexcept {
	_DrainNgxConsumers();
	_dlssFrameGenerator.reset();
	_nativeEffectBackends.clear();
}

void Renderer::_UpdateDestRect() noexcept {
	const RECT& rendererRect = ScalingWindow::Get().RendererRect();
	DestAlignment alignment = ScalingWindow::Get().Options().destAlignment;

	LONG destWidth;
	LONG destHeight;
	{
		D3D11_TEXTURE2D_DESC desc;
		_frontendSharedTextures[0]->GetDesc(&desc);
		destWidth = (LONG)desc.Width;
		destHeight = (LONG)desc.Height;
	}

	using enum DestAlignment;

	if (alignment == LeftTop || alignment == Left || alignment == LeftBottom) {
		_destRect.left = 0;
		_destRect.right = destWidth;
	} else if (alignment == Top || alignment == Center || alignment == Bottom) {
		_destRect.left = (rendererRect.left + rendererRect.right - destWidth) / 2;
		_destRect.right = _destRect.left + destWidth;
	} else {
		_destRect.left = rendererRect.right - destWidth;
		_destRect.right = rendererRect.right;
	}

	if (alignment == LeftTop || alignment == Top || alignment == RightTop) {
		_destRect.top = 0;
		_destRect.bottom = destHeight;
	} else if (alignment == Left || alignment == Center || alignment == Right) {
		_destRect.top = (rendererRect.top + rendererRect.bottom - destHeight) / 2;
		_destRect.bottom = _destRect.top + destHeight;
	} else {
		_destRect.top = rendererRect.bottom - destHeight;
		_destRect.bottom = rendererRect.bottom;
	}

	assert(_destRect.left + destWidth == _destRect.right);
	assert(_destRect.top + destHeight == _destRect.bottom);
}

HANDLE Renderer::_CreateSharedTexture(ID3D11Texture2D* effectsOutput) noexcept {
	D3D11_TEXTURE2D_DESC desc;
	effectsOutput->GetDesc(&desc);
	SIZE textureSize = { (LONG)desc.Width, (LONG)desc.Height };
	// 按最大槽位数分配，这样实时改变帧生成倍数时无需重建共享纹理
	const uint32_t slotCapacity =
		_dlssFrameGenerator ? MAX_SHARED_TEXTURE_SLOTS : 1u;
	_sharedTextureSlotCount = _dlssFrameGenerator ?
		std::clamp(_dlssFrameGenerator->Multiplier(), 2u, MAX_SHARED_TEXTURE_SLOTS) : 1u;
	_sharedTextureGeneration.fetch_add(1, std::memory_order_release);
	_nextBackendSharedTextureSlot = 0;
	_latestSharedTextureSlot.store(0, std::memory_order_relaxed);
	for (uint32_t i = 0; i < MAX_SHARED_TEXTURE_SLOTS; ++i) {
		_backendSharedTextureMutexes[i] = nullptr;
		_backendSharedTextures[i] = nullptr;
		_sharedTextureHandles[i] = nullptr;
		_sharedTextureAvailableEvents[i].reset();
		_sharedTextureMutexKeys[i].store(0, std::memory_order_relaxed);
	}

	for (uint32_t i = 0; i < slotCapacity; ++i) {
		_backendSharedTextures[i] = DirectXHelper::CreateTexture2D(
			_backendResources.GetD3DDevice(),
			DXGI_FORMAT_R8G8B8A8_UNORM,
			textureSize.cx,
			textureSize.cy,
			D3D11_BIND_SHADER_RESOURCE,
			D3D11_USAGE_DEFAULT,
			D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX);
		if (!_backendSharedTextures[i]) {
			Logger::Get().Error("Create shared presentation texture failed");
			return NULL;
		}
		_backendSharedTextureMutexes[i] =
			_backendSharedTextures[i].try_as<IDXGIKeyedMutex>();
		if (!_backendSharedTextureMutexes[i]) {
			Logger::Get().Error("Get shared presentation keyed mutex failed");
			return NULL;
		}

		winrt::com_ptr<IDXGIResource> sharedDxgiRes =
			_backendSharedTextures[i].try_as<IDXGIResource>();
		const HRESULT hr = sharedDxgiRes->GetSharedHandle(
			&_sharedTextureHandles[i]);
		if (FAILED(hr) || !_sharedTextureHandles[i]) {
			Logger::Get().ComError("Get shared presentation handle failed", hr);
			return NULL;
		}
		_sharedTextureAvailableEvents[i].reset(
			CreateEventW(nullptr, FALSE, TRUE, nullptr));
		if (!_sharedTextureAvailableEvents[i]) {
			Logger::Get().Win32Error(
				"Create shared presentation slot event failed");
			return NULL;
		}
	}
	if (_sharedTextureSlotCount > 1) {
		Logger::Get().Info(fmt::format(
			"DLSSFG bounded presentation ring initialized: slots={}",
			_sharedTextureSlotCount));
	}
	return _sharedTextureHandles[0];
}

void Renderer::_BackendThreadProc() noexcept {
#ifdef _DEBUG
	SetThreadDescription(GetCurrentThread(), L"Magpie-缩放后端线程");
#endif

	winrt::init_apartment(winrt::apartment_type::single_threaded);

	if (const HANDLE sharedHandle = _InitBackend()) {
		_sharedTextureHandle.store(sharedHandle, std::memory_order_release);
		_sharedTextureHandle.notify_one();
	} else {
		_frameSource.reset();
		// 通知前端初始化失败
		_sharedTextureHandle.store(INVALID_HANDLE_VALUE, std::memory_order_release);
		_sharedTextureHandle.notify_one();

		// 即使失败也要创建消息循环，否则前端线程将一直等待
		MSG msg;
		while (GetMessage(&msg, NULL, 0, 0)) {
			DispatchMessage(&msg);
		}
		return;
	}

	StepTimerStatus stepTimerStatus = StepTimerStatus::WaitForNewFrame;
	const bool waitMsgForNewFrame =
		_frameSource->WaitType() == FrameSourceWaitType::WaitForMessage;

	MSG msg;
	while (true) {
		bool fpsUpdated = false;
		stepTimerStatus = _stepTimer.WaitForNextFrame(
			waitMsgForNewFrame && stepTimerStatus != StepTimerStatus::WaitForFPSLimiter,
			fpsUpdated
		);

		while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
			if (msg.message == WM_QUIT) {
				// 不能在前端线程释放
				_frameSource.reset();
				return;
			}

			DispatchMessage(&msg);
		}

		if (!_dirtyEffectParameters.empty()) {
			_ApplyPendingEffectParameters();
		}

		if (stepTimerStatus == StepTimerStatus::WaitForFPSLimiter) {
			// 新帧消息可能已被处理，之后的 WaitForNextFrame 不要等待消息，直到状态变化
			continue;
		}

		const FrameSourceState frameSourceState = _frameSource->Update();
		switch (frameSourceState) {
		case FrameSourceState::Waiting:
			// 参数刚刚改变时即使没有新的捕获帧也要渲染一次
			if (stepTimerStatus != StepTimerStatus::ForceNewFrame && !_forceNextRender) {
				if (fpsUpdated) {
					// FPS 变化则要求前端重新渲染以更新叠加层，调整大小时这个操作十分必要
					PostMessage(ScalingWindow::Get().Handle(),
						CommonSharedConstants::WM_FRONTEND_RENDER, 0, 0);
				}
				break;
			}

			// 强制帧
			[[fallthrough]];
		case FrameSourceState::NewFrame:
			_forceNextRender = false;
			_BackendRender(
				_effectDrawers.back().GetOutputTexture(),
				frameSourceState == FrameSourceState::NewFrame);
			// DLSSFG uses synchronous, individually paced presentation so generated
			// frames cannot be coalesced into the following real frame.
			if (!_dlssFrameGenerator ||
				!_synchronousFramePresentationEnabled.load(std::memory_order_acquire)) {
				PostMessage(ScalingWindow::Get().Handle(),
					CommonSharedConstants::WM_FRONTEND_RENDER, 0, 0);
			}
			break;
		case FrameSourceState::Error:
			// 捕获出错，退出缩放
			ScalingWindow::Dispatcher().TryEnqueue([]() {
				ScalingWindow& scalingWindow = ScalingWindow::Get();
				scalingWindow.ShowError(ScalingError::CaptureFailed);
				scalingWindow.Stop();
			});

			while (GetMessage(&msg, NULL, 0, 0)) {
				DispatchMessage(&msg);
			}

			_frameSource.reset();
			return;
		}
	}
}

HANDLE Renderer::_InitBackend() noexcept {
	// 创建 DispatcherQueue
	{
		winrt::Windows::System::DispatcherQueueController dqc{ nullptr };
		HRESULT hr = CreateDispatcherQueueController(
			DispatcherQueueOptions{
				.dwSize = sizeof(DispatcherQueueOptions),
				.threadType = DQTYPE_THREAD_CURRENT
			},
			(PDISPATCHERQUEUECONTROLLER*)winrt::put_abi(dqc)
		);
		if (FAILED(hr)) {
			Logger::Get().ComError("CreateDispatcherQueueController 失败", hr);
			return NULL;
		}

		_backendThreadDispatcher = dqc.DispatcherQueue();
	}

	// 保存一份副本，叠加层可以实时修改其中的参数
	_effectOptions = ScalingWindow::Get().Options().effects;

	if (!_backendResources.Initialize(false)) {
		return NULL;
	}
	
	ID3D11Device5* d3dDevice = _backendResources.GetD3DDevice();
	_backendDescriptorStore.Initialize(d3dDevice);

	if (!_InitFrameSource()) {
		return NULL;
	}
	{
		if (_frameSource->WaitType() == FrameSourceWaitType::NoWait) {
			// 某些捕获方式不会限制捕获帧率，因此将捕获帧率限制为屏幕刷新率
			const HWND hwndSrc = ScalingWindow::Get().SrcTracker().Handle();
			if (HMONITOR hMon = MonitorFromWindow(hwndSrc, MONITOR_DEFAULTTONEAREST)) {
				MONITORINFOEX mi{ { sizeof(MONITORINFOEX) } };
				GetMonitorInfo(hMon, &mi);

				DEVMODE dm{ .dmSize = sizeof(DEVMODE) };
				EnumDisplaySettings(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm);

				if (dm.dmDisplayFrequency > 0) {
					Logger::Get().Info(fmt::format("屏幕刷新率: {}", dm.dmDisplayFrequency));
					_captureMaxFrameRate = float(dm.dmDisplayFrequency);
				}
			}
		}

		_UpdateFrameRateLimits();
	}

	ID3D11Texture2D* outputTexture = _BuildEffects();
	if (!outputTexture) {
		return NULL;
	}

	const FrameGuidanceRequirements guidanceRequirements =
		CollectFrameGuidanceRequirements(
			_nativeEffectBackends, _dlssFrameGenerator.get());
	if (guidanceRequirements.Any()) {
#ifdef MP_ENABLE_NVIDIA_OPTICAL_FLOW
		if (guidanceRequirements.motion) {
			_frameGuidanceService.SetMotionVectorProvider(
				std::make_unique<NvidiaOpticalFlowProvider>());
		}
#endif
#ifdef MP_ENABLE_DEPTH_ANYTHING_V2
		if (guidanceRequirements.depth) {
			_frameGuidanceService.SetDepthProvider(
				std::make_unique<DepthAnythingV2Provider>(
					guidanceRequirements.depthInferenceInterval));
		}
#endif
		if (!_frameGuidanceService.Initialize(
			_backendResources, _frameSource->GetOutput(), guidanceRequirements)) {
			return NULL;
		}
	}

	HRESULT hr = d3dDevice->CreateFence(
		_fenceValue, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&_d3dFence));
	if (FAILED(hr)) {
		// GH#979
		// 这个错误会在某些很旧的显卡上出现，似乎是驱动的 bug。文档中提到 ID3D11Device5::CreateFence 
		// 和 ID3D12Device::CreateFence 等价，但支持 DX12 的显卡也有失败的可能，如 GH#1013
		Logger::Get().ComError("CreateFence 失败", hr);
		_backendInitError = ScalingError::CreateFenceFailed;
		return NULL;
	}

	if (!_fenceEvent.try_create(wil::EventOptions::None, nullptr)) {
		Logger::Get().Win32Error("CreateEvent 失败");
		return NULL;
	}

	HANDLE sharedHandle = _CreateSharedTexture(outputTexture);
	if (!sharedHandle) {
		Logger::Get().Error("_CreateSharedTexture 失败");
		return NULL;
	}

	// 最后启动捕获以尽可能推迟显示黄色边框 (Win10) 或禁用圆角 (Win11)
	if (!_frameSource->Start()) {
		Logger::Get().Error("启动捕获失败");
		return NULL;
	}

	return sharedHandle;
}

void Renderer::_BackendRender(
	ID3D11Texture2D* effectsOutput,
	bool isNewCaptureFrame
) noexcept {
	_stepTimer.PrepareForRender();
	if (isNewCaptureFrame) {
		++_capturedFrameId;
		const FrameGuidanceRequirements guidanceRequirements =
			CollectFrameGuidanceRequirements(
				_nativeEffectBackends, _dlssFrameGenerator.get());
		if (_frameGuidanceService.IsInitialized()) {
			_frameGuidanceService.BeginFrame(
				_capturedFrameId, _frameSource->GetOutput(), guidanceRequirements);
		}
	}
	if (_dlssFrameGenerator) {
		++_dlssFgCapturedFrameCount;
	}

	ID3D11DeviceContext4* d3dDC = _backendResources.GetD3DDC();
	d3dDC->ClearState();

	if (ID3D11Buffer* t = _dynamicCB.get()) {
		_UpdateDynamicConstants();
		d3dDC->CSSetConstantBuffers(1, 1, &t);
	}

	_effectsProfiler.OnBeginEffects(d3dDC);

	for (uint32_t i = 0; i < _effectDrawers.size(); ++i) {
		const EffectDrawer& effectDrawer = _effectDrawers[i];
		if (i < _nativeEffectBackends.size() && _nativeEffectBackends[i]) {
			D3D11_TEXTURE2D_DESC inputDesc{};
			effectDrawer.GetTexture(0)->GetDesc(&inputDesc);
			const FrameGuidanceConsumerViews guidance =
				_frameGuidanceService.GetConsumerViews(
					_capturedFrameId, { inputDesc.Width, inputDesc.Height });
			const NativeEffectDrawContext drawContext{
				.input = effectDrawer.GetTexture(0),
				.output = effectDrawer.GetOutputTexture(),
				.frameId = _capturedFrameId,
				.frameGuidance = guidance.produced,
				.zeroFrameGuidance = guidance.zero
			};
			if (!_nativeEffectBackends[i]->Draw(drawContext)) {
				Logger::Get().Error("Draw native effect failed");
			}
			_effectsProfiler.OnEndPass(d3dDC);
		} else {
			effectDrawer.Draw(_effectsProfiler);
		}
	}

	_effectsProfiler.OnEndEffects(d3dDC);

	if (_dlssFrameGenerator) {
		D3D11_TEXTURE2D_DESC sourceDesc{};
		_frameSource->GetOutput()->GetDesc(&sourceDesc);
		const FrameGuidanceConsumerViews guidance =
			_frameGuidanceService.GetConsumerViews(
				_capturedFrameId, { sourceDesc.Width, sourceDesc.Height });
		_dlssFgPresentationStopping = false;
		const bool generated = _dlssFrameGenerator->Draw(
			effectsOutput,
			_capturedFrameId,
			guidance.produced,
			guidance.zero,
			[this](ID3D11Texture2D* generatedFrame) {
				return _PublishBackendTexture(generatedFrame, true, true);
			}
		);
		if (!generated && !_dlssFgPresentationStopping) {
			_HandleDLSSFrameGenerationFailure(effectsOutput);
		} else if (!generated) {
			Logger::Get().Info(
				"DLSSFG presentation interrupted by normal window shutdown");
		} else {
			_dlssFgConsecutiveFailures = 0;
		}
	}

	const bool synchronous = _dlssFrameGenerator &&
		_synchronousFramePresentationEnabled.load(std::memory_order_acquire);
	if (!_PublishBackendTexture(effectsOutput, synchronous, false)) {
		return;
	}

	// 查询效果的渲染时间
	_effectsProfiler.QueryTimings(d3dDC);
}

bool Renderer::_PublishBackendTexture(
	ID3D11Texture2D* texture,
	bool synchronous,
	bool generatedFrame
) noexcept {
	ID3D11DeviceContext4* d3dDC = _backendResources.GetD3DDC();
	const bool queuedPresentation = synchronous &&
		_synchronousFramePresentationEnabled.load(std::memory_order_acquire);
	const uint32_t sharedTextureSlot =
		_nextBackendSharedTextureSlot++ % _sharedTextureSlotCount;
	bool slotReserved = false;
	if (queuedPresentation) {
		const auto ringWaitStart = std::chrono::steady_clock::now();
		while (true) {
			const DWORD waitResult = WaitForSingleObject(
				_sharedTextureAvailableEvents[sharedTextureSlot].get(), 50);
			if (waitResult == WAIT_OBJECT_0) {
				slotReserved = true;
				break;
			}
			if (waitResult != WAIT_TIMEOUT ||
				!_synchronousFramePresentationEnabled.load(std::memory_order_acquire)) {
				_dlssFgPresentationStopping =
					!_synchronousFramePresentationEnabled.load(std::memory_order_acquire);
				return false;
			}
		}
		_dlssFgRingWaitNanoseconds.fetch_add(
			(uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now() - ringWaitStart).count(),
			std::memory_order_relaxed);
		_dlssFgRingWaitSamples.fetch_add(1, std::memory_order_relaxed);
	}
	auto releaseReservedSlot = [&]() noexcept {
		if (slotReserved && _sharedTextureAvailableEvents[sharedTextureSlot]) {
			SetEvent(_sharedTextureAvailableEvents[sharedTextureSlot].get());
			slotReserved = false;
		}
	};

	const uint64_t key = ++_sharedTextureMutexKeys[sharedTextureSlot];
	HRESULT hr = _backendSharedTextureMutexes[sharedTextureSlot]->AcquireSync(
		key - 1, 250);
	if (FAILED(hr)) {
		releaseReservedSlot();
		Logger::Get().ComError("Acquire shared presentation slot failed", hr);
		return false;
	}
	d3dDC->CopyResource(_backendSharedTextures[sharedTextureSlot].get(), texture);
	hr = _backendSharedTextureMutexes[sharedTextureSlot]->ReleaseSync(key);
	if (FAILED(hr)) {
		releaseReservedSlot();
		Logger::Get().ComError("Release shared presentation slot failed", hr);
		return false;
	}

	// Signal after the copy so the D3D12 generated texture cannot be reused
	// until its D3D11 copy into the bounded presentation ring has completed.
	hr = d3dDC->Signal(_d3dFence.get(), ++_fenceValue);
	if (SUCCEEDED(hr)) {
		hr = _d3dFence->SetEventOnCompletion(_fenceValue, _fenceEvent.get());
	}
	if (FAILED(hr)) {
		releaseReservedSlot();
		Logger::Get().ComError("Signal shared presentation copy failed", hr);
		return false;
	}
	d3dDC->Flush();
	_fenceEvent.wait();
	_latestSharedTextureSlot.store(sharedTextureSlot, std::memory_order_release);

	if (queuedPresentation) {
		if (!PostMessage(
			ScalingWindow::Get().Handle(),
			CommonSharedConstants::WM_FRONTEND_RENDER_DLSSFG,
			sharedTextureSlot,
			_sharedTextureGeneration.load(std::memory_order_acquire)
		)) {
			releaseReservedSlot();
			const bool stopping =
				!_synchronousFramePresentationEnabled.load(std::memory_order_acquire) ||
				!IsWindow(ScalingWindow::Get().Handle());
			_dlssFgPresentationStopping = stopping;
			if (generatedFrame) {
				++_dlssFgGeneratedPublishFailure;
			} else {
				++_dlssFgRealPublishFailure;
			}
			if (!stopping) {
				Logger::Get().Warn("DLSSFG synchronous frontend presentation failed");
			}
			return false;
		}
		slotReserved = false;
		++_dlssFgPresentedFrameCount;
		if (generatedFrame) {
			++_dlssFgGeneratedPublishSuccess;
		} else {
			++_dlssFgRealPublishSuccess;
		}
		const auto now = std::chrono::steady_clock::now();
		if (_dlssFgDiagnosticsStart.time_since_epoch().count() == 0) {
			_dlssFgDiagnosticsStart = now;
		} else {
			const double elapsed = std::chrono::duration<double>(
				now - _dlssFgDiagnosticsStart).count();
			if (elapsed >= 1.0) {
				Logger::Get().Info(fmt::format(
					"DLSSFG presentation ring: captured={:.1f} FPS, queued={:.1f} FPS "
					"generatedPublish={}/{} realPublish={}/{}",
					_dlssFgCapturedFrameCount / elapsed,
					_dlssFgPresentedFrameCount / elapsed,
					_dlssFgGeneratedPublishSuccess,
					_dlssFgGeneratedPublishFailure,
					_dlssFgRealPublishSuccess,
					_dlssFgRealPublishFailure));
				_dlssFgDiagnosticsStart = now;
				_dlssFgCapturedFrameCount = 0;
				_dlssFgPresentedFrameCount = 0;
				_dlssFgGeneratedPublishSuccess = 0;
				_dlssFgGeneratedPublishFailure = 0;
				_dlssFgRealPublishSuccess = 0;
				_dlssFgRealPublishFailure = 0;
			}
		}
	}

	return true;
}

bool Renderer::_UpdateDynamicConstants() const noexcept {
	// cbuffer __CB2 : register(b1) { uint __frameCount; };

	ID3D11DeviceContext4* d3dDC = _backendResources.GetD3DDC();

	D3D11_MAPPED_SUBRESOURCE ms;
	HRESULT hr = d3dDC->Map(_dynamicCB.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms);
	if (SUCCEEDED(hr)) {
		// 避免使用 *(uint32_t*)ms.pData，见
		// https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-map
		const uint32_t frameCount = _stepTimer.FrameCount();
		std::memcpy(ms.pData, &frameCount, 4);
		d3dDC->Unmap(_dynamicCB.get(), 0);
	} else {
		Logger::Get().ComError("Map 失败", hr);
		return false;
	}

	return true;
}

winrt::IAsyncAction Renderer::_UpdateNextScreenshotNum(const wchar_t* imgFormat) noexcept {
	// 由于中途会转到后台，应防止并发计算 _screenshotNum
	static wil::srwlock screenshotNumLock;

	wil::rwlock_release_exclusive_scope_exit lk;
	while (true) {
		lk = screenshotNumLock.try_lock_exclusive();
		if (lk) {
			break;
		} else {
			// 前一次截图正在执行 FindUnusedScreenshotNum，给它继续执行的机会直到释放锁
			co_await _backendThreadDispatcher;
		}
	}

	const std::filesystem::path& screenshotsDir = ScalingWindow::Get().Options().screenshotsDir;

	if (_screenshotNum != 0) {
		if (_screenshotNum == std::numeric_limits<uint32_t>::max()) {
			// 如果达到 UINT_MAX 应重新寻找可用序号，除了特意构造的数据不可能出现这种情况
			_screenshotNum = 0;
		} else {
			++_screenshotNum;

			if (Win32Helper::DirExists(screenshotsDir.c_str())) {
				const std::wstring fileName =
					fmt::format(L"{}\\Magpie_{:03}.{}", screenshotsDir.native(), _screenshotNum, imgFormat);
				if (Win32Helper::FileExists(fileName.c_str())) {
					// 下一个序号不可用则需要重新寻找可用序号
					_screenshotNum = 0;
				}
			}
		}
	}

	if (_screenshotNum == 0) {
		co_await winrt::resume_background();
		// 如果已有截图很多可能较耗时，转到后台防止阻塞后端线程
		const uint32_t screenshotNum = ScreenshotHelper::FindUnusedScreenshotNum(screenshotsDir);
		co_await _backendThreadDispatcher;

		// FindUnusedScreenshotNum 失败则始终使用 001
		_screenshotNum = screenshotNum == 0 ? 1 : screenshotNum;
	}
}

winrt::IAsyncOperation<bool> Renderer::_TakeScreenshotImpl(
	uint32_t effectIdx,
	uint32_t passIdx,
	uint32_t outputIdx
) noexcept {
	co_await _backendThreadDispatcher;

	// 最后一个通道的输出即 OUTPUT 不会被覆盖，可以直接使用。
	// 倒数第二个通道的输出也不会被覆盖，因为最后一个通道只会写入 OUTPUT。
	// 从倒数第三个通道开始需要检查输出是否被后面的通道覆盖。
	bool isOverwritten = false;
	ID3D11Texture2D* sourceTex;
	EffectIntermediateTextureFormat format;
	// 效果输出保存为 png，中间结果保存为 dds
	const wchar_t* imgFormat;

	if (passIdx == std::numeric_limits<uint32_t>::max()) {
		sourceTex = _effectDrawers[effectIdx].GetOutputTexture();
		format = _activeEffectDescs[effectIdx]->textures[1].format;
		imgFormat = L"png";
	} else {
		const std::vector<EffectPassDesc>& passes = _activeEffectDescs[effectIdx]->passes;
		const uint32_t passCount = (uint32_t)passes.size();

		const SmallVector<uint32_t>& outputs = passes[passIdx].outputs;
		// 只有一个输出时才允许不提供 outputIdx
		assert(outputIdx != std::numeric_limits<uint32_t>::max() || outputs.size() == 1);
		const uint32_t targetOutput =
			outputIdx == std::numeric_limits<uint32_t>::max() ? outputs[0] : outputs[outputIdx];

		sourceTex = _effectDrawers[effectIdx].GetTexture(targetOutput);
		format = _activeEffectDescs[effectIdx]->textures[targetOutput].format;
		imgFormat = targetOutput == 1 ? L"png" : L"dds";

		if (passIdx + 3 <= passCount) {
			// 检查 targetOutput 是否被后面的通道修改 
			for (uint32_t i = passIdx + 1, end = passCount - 1; i < end; ++i) {
				const SmallVector<uint32_t>& curOutputs = passes[i].outputs;
				if (std::find(curOutputs.begin(), curOutputs.end(), targetOutput) != curOutputs.end()) {
					isOverwritten = true;
					break;
				}
			}
		}
	}

	co_await _UpdateNextScreenshotNum(imgFormat);
	// 读取纹理数据时 _screenshotNum 有被并发修改的可能，把当前值保存到本地
	const uint32_t screenshotNum = _screenshotNum;

	ID3D11Device5* d3dDevice = _backendResources.GetD3DDevice();
	ID3D11DeviceContext4* d3dDC = _backendResources.GetD3DDC();

	if (isOverwritten) {
		// 重新渲染
		d3dDC->ClearState();

		if (ID3D11Buffer* t = _dynamicCB.get()) {
			d3dDC->CSSetConstantBuffers(1, 1, &t);
		}

		_effectDrawers[effectIdx].DrawForExport(*_activeEffectDescs[effectIdx], passIdx);
	}

	// 创建 staging 纹理
	D3D11_TEXTURE2D_DESC desc;
	sourceTex->GetDesc(&desc);
	desc.Usage = D3D11_USAGE_STAGING;
	desc.BindFlags = 0;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	desc.MiscFlags = 0;

	winrt::com_ptr<ID3D11Texture2D> stagingTex;
	HRESULT hr = d3dDevice->CreateTexture2D(
		&desc, nullptr, stagingTex.put());
	if (FAILED(hr)) {
		Logger::Get().ComError("CreateTexture2D 失败", hr);
		co_return false;
	}

	d3dDC->CopyResource(stagingTex.get(), sourceTex);
	
	// 如果要导出的纹理不会被覆盖则转到后台等待 GPU 以防止卡顿
	if (!isOverwritten) {
		// 为避免混乱，使用独立的栅栏
		winrt::com_ptr<ID3D11Fence> localFence;
		wil::unique_event_nothrow localFenceEvent;

		hr = d3dDevice->CreateFence(
			0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&localFence));
		if (FAILED(hr)) {
			Logger::Get().ComError("CreateFence 失败", hr);
			co_return false;
		}

		if (!localFenceEvent.try_create(wil::EventOptions::None, nullptr)) {
			Logger::Get().Win32Error("CreateEvent 失败");
			co_return false;
		}

		hr = d3dDC->Signal(localFence.get(), 1);
		if (FAILED(hr)) {
			Logger::Get().ComError("Signal 失败", hr);
			co_return false;
		}

		hr = localFence->SetEventOnCompletion(1, localFenceEvent.get());
		if (FAILED(hr)) {
			Logger::Get().ComError("SetEventOnCompletion 失败", hr);
			co_return false;
		}

		d3dDC->Flush();

		winrt::DispatcherQueue dispatcher = _backendThreadDispatcher;
		co_await winrt::resume_background();
		localFenceEvent.wait();
		co_await dispatcher;
	}

	// 读取纹理数据到内存。isOverwritten 为真时这个调用将阻塞 CPU
	D3D11_MAPPED_SUBRESOURCE mapped;
	hr = d3dDC->Map(stagingTex.get(), 0, D3D11_MAP_READ, 0, &mapped);
	if (FAILED(hr)) {
		Logger::Get().ComError("Map 失败", hr);
		co_return false;
	}

	std::vector<uint8_t> pixelData(size_t(mapped.RowPitch) * desc.Height);
	std::memcpy(pixelData.data(), mapped.pData, pixelData.size());

	d3dDC->Unmap(stagingTex.get(), 0);

	co_await winrt::resume_background();

	// 确保截图保存目录存在
	const std::filesystem::path& screenshotsDir = ScalingWindow::Get().Options().screenshotsDir;
	if (!Win32Helper::CreateDir(screenshotsDir.c_str(), true)) {
		Logger::Get().Error("CreateDir 失败");
		co_return false;
	}

	std::wstring fileName = fmt::format(L"Magpie_{:03}.{}", screenshotNum, imgFormat);
	const std::filesystem::path& fullPath = screenshotsDir / fileName;

	if (!TextureHelper::SaveTexture(
		fullPath.c_str(), desc.Width, desc.Height, format, pixelData, mapped.RowPitch)) {
		Logger::Get().Error("SaveImage 失败");
		co_return false;
	}

	winrt::hstring successMsg =
		ScalingWindow::Get().GetLocalizedString(L"Message_ScreenshotSaved");
	ScalingWindow::Get().ShowToast(
		fmt::format(fmt::runtime(std::wstring_view(successMsg)), fileName));
	co_return true;
}

// 监听 PrintScreen 实现截屏时隐藏光标
LRESULT CALLBACK Renderer::_LowLevelKeyboardHook(int nCode, WPARAM wParam, LPARAM lParam) {
	if (nCode != HC_ACTION || wParam != WM_KEYDOWN) {
		return CallNextHookEx(NULL, nCode, wParam, lParam);
	}

	KBDLLHOOKSTRUCT* info = (KBDLLHOOKSTRUCT*)lParam;
	if (info->vkCode == VK_SNAPSHOT) {
		// 为了缩短钩子处理时间，异步执行所有逻辑
		ScalingWindow::Dispatcher().TryEnqueue([]() -> winrt::fire_and_forget {
			// 暂时隐藏光标
			Renderer& renderer = ScalingWindow::Get().Renderer();
			renderer._cursorDrawer.IsCursorVisible(false);
			renderer.Render();

			const uint32_t runId = ScalingWindow::RunId();

			winrt::DispatcherQueue dispatcher = ScalingWindow::Dispatcher();
			co_await 200ms;
			co_await dispatcher;

			if (ScalingWindow::RunId() == runId &&
				!renderer._cursorDrawer.IsCursorVisible()
			) {
				renderer._cursorDrawer.IsCursorVisible(true);
				renderer.Render();
			}
		});
	}

	return CallNextHookEx(NULL, nCode, wParam, lParam);
}

}
