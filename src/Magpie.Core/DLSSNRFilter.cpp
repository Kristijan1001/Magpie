#include "pch.h"
#include "DLSSNRFilter.h"
#include "DeviceResources.h"
#include "DirectXHelper.h"
#include "Logger.h"
#include "NgxD3D12Core.h"
#include "Win32Helper.h"
#include "FrameGuidanceD3D12Interop.h"
#include "FrameGuidancePerformance.h"

#ifdef MP_ENABLE_DLSSNR
#include <d3d12.h>
#include <nvsdk_ngx.h>
#include <atomic>

namespace Magpie {

namespace {

void LogDlssnrStatus(std::string message, bool error = false) noexcept {
	if (error) {
		Logger::Get().Error(message);
	} else {
		Logger::Get().Info(message);
	}
	message.push_back('\n');
	OutputDebugStringA(message.c_str());
}

constexpr NVSDK_NGX_Feature FEATURE_DLSSNR =
	static_cast<NVSDK_NGX_Feature>(18);
constexpr unsigned long long DLSSNR_SIGNED_SNIPPET_APPLICATION_ID = 0x0876232Cull;
// Keep the unsupported Core Feature 18 route as an explicit diagnostic only.
// It must never run before the signed snippet in a production session.
constexpr bool ENABLE_CORE_FEATURE18_DIAGNOSTIC = false;

constexpr char PARAM_WIDTH[] = "DLSSNR.Width";
constexpr char PARAM_HEIGHT[] = "DLSSNR.Height";
constexpr char PARAM_INPUT_WIDTH[] = "DLSSNR.InputWidth";
constexpr char PARAM_INPUT_HEIGHT[] = "DLSSNR.InputHeight";
constexpr char PARAM_OUTPUT_WIDTH[] = "DLSSNR.OutputWidth";
constexpr char PARAM_OUTPUT_HEIGHT[] = "DLSSNR.OutputHeight";
constexpr char PARAM_OUTPUT_DOT_WIDTH[] = "DLSSNR.Output.Width";
constexpr char PARAM_OUTPUT_DOT_HEIGHT[] = "DLSSNR.Output.Height";
constexpr char PARAM_UPSCALING[] = "DLSSNR.Upscaling";
constexpr char PARAM_SCALE[] = "DLSSNR.Scale";
constexpr char PARAM_SCALING_RATIO[] = "DLSSNR.ScalingRatio";
constexpr char PARAM_SCALING_RATIO_CALLBACK[] = "DLSSNRComputeScalingRatioCallback";
constexpr char PARAM_PRESET[] = "DLSSNR.Hint.Render.Preset";
constexpr char PARAM_COLOR[] = "DLSSNR.Color";
constexpr char PARAM_OUTPUT[] = "DLSSNR.Output";
constexpr char PARAM_MVEC[] = "DLSSNR.MVec";
constexpr char PARAM_DEPTH[] = "DLSSNR.Depth";
constexpr char PARAM_MVEC_SCALE_X[] = "DLSSNR.MVecScaleX";
constexpr char PARAM_MVEC_SCALE_Y[] = "DLSSNR.MVecScaleY";
constexpr char PARAM_DEPTH_INVERTED[] = "DLSSNR.DepthInverted";
constexpr char PARAM_ENABLED[] = "DLSSNR.Enabled";
constexpr char PARAM_RESET[] = "DLSSNR.Reset";
constexpr char PARAM_STYLE[] = "DLSSNR.Style";
constexpr char PARAM_INTENSITY[] = "DLSSNR.Intensity";
constexpr char PARAM_LOCAL_TONE[] = "DLSSNR.LocalToneStrength";
constexpr char PARAM_LOCAL_STRUCTURE[] = "DLSSNR.LocalStructureStrength";
constexpr char PARAM_SKIN_STRUCTURE[] = "DLSSNR.SkinStructureStrength";
constexpr char PARAM_AUTO_MASK[] = "DLSSNR.UseAutoMask";
constexpr char PARAM_UI_CORRECTION[] = "DLSSNR.UICorrection";
constexpr char PARAM_INDICATOR_INVERT_X[] = "DLSS.Indicator.Invert.X.Axis";
constexpr char PARAM_INDICATOR_INVERT_Y[] = "DLSS.Indicator.Invert.Y.Axis";

struct ResourceParameters {
	const char* baseX;
	const char* baseY;
	const char* width;
	const char* height;
};

constexpr ResourceParameters RESOURCE_PARAMETERS[]{
	{ "DLSSNR.ColorSubrectBaseX", "DLSSNR.ColorSubrectBaseY",
		"DLSSNR.ColorSubrectWidth", "DLSSNR.ColorSubrectHeight" },
	{ "DLSSNR.OutputSubrectBaseX", "DLSSNR.OutputSubrectBaseY",
		"DLSSNR.OutputSubrectWidth", "DLSSNR.OutputSubrectHeight" },
	{ "DLSSNR.MVecSubrectBaseX", "DLSSNR.MVecSubrectBaseY",
		"DLSSNR.MVecSubrectWidth", "DLSSNR.MVecSubrectHeight" },
	{ "DLSSNR.DepthSubrectBaseX", "DLSSNR.DepthSubrectBaseY",
		"DLSSNR.DepthSubrectWidth", "DLSSNR.DepthSubrectHeight" }
};

constexpr char COLOR_CONVERT_HLSL[] = R"(
Texture2D<float4> InputColor : register(t0);
RWTexture2D<float4> OutputColor : register(u0);

[numthreads(8, 8, 1)]
void ConvertToRgba(uint3 tid : SV_DispatchThreadID) {
    uint width, height;
    OutputColor.GetDimensions(width, height);
    if (tid.x >= width || tid.y >= height) return;
    OutputColor[tid.xy] = InputColor.Load(int3(tid.xy, 0));
}
)";

bool NGXSucceeded(NVSDK_NGX_Result result) noexcept {
	return NVSDK_NGX_SUCCEED(result);
}

struct TimingSummary {
	double average = 0.0;
	double p95 = 0.0;
	double p99 = 0.0;
	double maximum = 0.0;
	size_t count = 0;
};

struct TimingWindow {
	static constexpr size_t CAPACITY = 120;
	std::array<double, CAPACITY> values{};
	size_t count = 0;
	size_t next = 0;

	void Add(double value) noexcept {
		values[next] = value;
		next = (next + 1) % CAPACITY;
		count = std::min(count + 1, CAPACITY);
	}

	TimingSummary Summarize() const noexcept {
		TimingSummary result{ .count = count };
		if (!count) return result;
		std::array<double, CAPACITY> sorted{};
		std::copy_n(values.begin(), count, sorted.begin());
		std::sort(sorted.begin(), sorted.begin() + count);
		double total = 0.0;
		for (size_t i = 0; i < count; ++i) total += sorted[i];
		result.average = total / static_cast<double>(count);
		result.p95 = sorted[std::min(count - 1, (count * 95 + 99) / 100 - 1)];
		result.p99 = sorted[std::min(count - 1, (count * 99 + 99) / 100 - 1)];
		result.maximum = sorted[count - 1];
		return result;
	}
};

template <typename T>
T GetExport(HMODULE module, const char* name) noexcept {
	return reinterpret_cast<T>(GetProcAddress(module, name));
}

}

struct DLSSNRFilter::Impl {
	static constexpr uint32_t COMMAND_SLOT_COUNT = 4;
	struct CommandSlot {
		winrt::com_ptr<ID3D12CommandAllocator> allocator;
		winrt::com_ptr<ID3D12GraphicsCommandList> commandList;
		uint64_t completionValue = 0;
		uint32_t timestampQuery = 0;
		FrameGuidanceFrameId timestampFrameId = 0;
		bool timestampPending = false;
	};

	using SnippetInitExtFn = NVSDK_NGX_Result(NVSDK_CONV*)(
		unsigned long long, const wchar_t*, ID3D12Device*, NVSDK_NGX_Version,
		const NVSDK_NGX_Parameter*);
	using CreateFeatureFn = NVSDK_NGX_Result(NVSDK_CONV*)(
		ID3D12GraphicsCommandList*, NVSDK_NGX_Feature, NVSDK_NGX_Parameter*,
		NVSDK_NGX_Handle**);
	using EvaluateFeatureFn = NVSDK_NGX_Result(NVSDK_CONV*)(
		ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*,
		const NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback);
	using ReleaseFeatureFn = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Handle*);
	using ShutdownFn = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12Device*);
	using GetModuleFileNameWFn = DWORD(WINAPI*)(HMODULE, LPWSTR, DWORD);

	~Impl();

	ID3D11Device5* device11 = nullptr;
	ID3D11DeviceContext4* context11 = nullptr;
	NgxD3D12Core* coreOwner = nullptr;
	winrt::com_ptr<ID3D12Device> device12;
	winrt::com_ptr<ID3D12CommandQueue> queue12;
	winrt::com_ptr<ID3D12CommandAllocator> allocator12;
	winrt::com_ptr<ID3D12GraphicsCommandList> commandList12;
	std::array<CommandSlot, COMMAND_SLOT_COUNT> commandSlots;
	uint32_t nextCommandSlot = 0;
	winrt::com_ptr<ID3D11Texture2D> sharedInput11;
	winrt::com_ptr<ID3D11Texture2D> sharedOutput11;
	winrt::com_ptr<ID3D11ShaderResourceView> inputSrv11;
	winrt::com_ptr<ID3D11UnorderedAccessView> sharedInputUav11;
	winrt::com_ptr<ID3D11ComputeShader> colorConvertShader11;
	winrt::com_ptr<ID3D12Resource> sharedInput12;
	winrt::com_ptr<ID3D12Resource> sharedOutput12;
	std::unique_ptr<FrameGuidanceD3D12Interop> guidanceInterop;
	winrt::com_ptr<ID3D11Fence> fence11;
	winrt::com_ptr<ID3D12Fence> fence12;
	wil::unique_event_nothrow fenceEvent;
	winrt::com_ptr<ID3D12QueryHeap> timestampQueryHeap;
	winrt::com_ptr<ID3D12Resource> timestampReadback;
	uint64_t timestampFrequency = 0;
	NVSDK_NGX_Parameter* parameters = nullptr;
	NVSDK_NGX_Handle* feature = nullptr;
	HMODULE snippetModule = nullptr;
	SnippetInitExtFn snippetInitExt = nullptr;
	CreateFeatureFn snippetCreateFeature = nullptr;
	EvaluateFeatureFn snippetEvaluateFeature = nullptr;
	ReleaseFeatureFn snippetReleaseFeature = nullptr;
	ShutdownFn snippetShutdown = nullptr;
	void** snippetGetModuleFileNameIatSlot = nullptr;
	uint64_t fenceValue = 0;
	uint64_t evaluateCount = 0;
	uint64_t evaluateSuccessCount = 0;
	uint64_t evaluateFailureCount = 0;
	TimingWindow slotWaitTimings;
	TimingWindow inputPrepareTimings;
	TimingWindow guidancePrepareTimings;
	TimingWindow evaluateCpuTimings;
	TimingWindow submitTimings;
	TimingWindow evaluateGpuTimings;
	FrameGuidanceFrameId lastGuidanceResetFrameId =
		std::numeric_limits<FrameGuidanceFrameId>::max();
	uint32_t width = 0;
	uint32_t height = 0;
	bool convertInputToRgba = false;
	bool coreRegistered = false;
	bool snippetInitialized = false;
	bool snippetCallerHookInstalled = false;
	bool useSignedSnippet = false;
	bool resetHistory = true;
	bool disabled = false;
};

namespace {

std::atomic<void*> g_snippetCallerHookOwner = nullptr;
std::atomic<HMODULE> g_snippetCallerModule = nullptr;
std::atomic<DLSSNRFilter::Impl::GetModuleFileNameWFn>
	g_snippetOriginalGetModuleFileNameW = nullptr;

LONG CaptureNgxException(DWORD code, DWORD* sehCode) noexcept {
	*sehCode = code;
	return EXCEPTION_EXECUTE_HANDLER;
}

NVSDK_NGX_Result CallCreateFeatureSafely(
	DLSSNRFilter::Impl::CreateFeatureFn function,
	ID3D12GraphicsCommandList* commandList,
	NVSDK_NGX_Feature featureId,
	NVSDK_NGX_Parameter* parameters,
	NVSDK_NGX_Handle** feature,
	DWORD* sehCode
) noexcept {
	*sehCode = 0;
	__try {
		return function(commandList, featureId, parameters, feature);
	} __except (CaptureNgxException(GetExceptionCode(), sehCode)) {
		return NVSDK_NGX_Result_FAIL_PlatformError;
	}
}

NVSDK_NGX_Result CallEvaluateFeatureSafely(
	DLSSNRFilter::Impl::EvaluateFeatureFn function,
	ID3D12GraphicsCommandList* commandList,
	const NVSDK_NGX_Handle* feature,
	const NVSDK_NGX_Parameter* parameters,
	DWORD* sehCode
) noexcept {
	*sehCode = 0;
	__try {
		return function(commandList, feature, parameters, nullptr);
	} __except (CaptureNgxException(GetExceptionCode(), sehCode)) {
		return NVSDK_NGX_Result_FAIL_PlatformError;
	}
}

NVSDK_NGX_Result CallReleaseFeatureSafely(
	DLSSNRFilter::Impl::ReleaseFeatureFn function,
	NVSDK_NGX_Handle* feature,
	DWORD* sehCode
) noexcept {
	*sehCode = 0;
	__try {
		return function(feature);
	} __except (CaptureNgxException(GetExceptionCode(), sehCode)) {
		return NVSDK_NGX_Result_FAIL_PlatformError;
	}
}

NVSDK_NGX_Result CallShutdownSafely(
	DLSSNRFilter::Impl::ShutdownFn function,
	ID3D12Device* device,
	DWORD* sehCode
) noexcept {
	*sehCode = 0;
	__try {
		return function(device);
	} __except (CaptureNgxException(GetExceptionCode(), sehCode)) {
		return NVSDK_NGX_Result_FAIL_PlatformError;
	}
}

NVSDK_NGX_Result CallSnippetInitSafely(
	DLSSNRFilter::Impl::SnippetInitExtFn function,
	const wchar_t* applicationDataPath,
	ID3D12Device* device,
	DWORD* sehCode
) noexcept {
	*sehCode = 0;
	__try {
		return function(
			DLSSNR_SIGNED_SNIPPET_APPLICATION_ID, applicationDataPath,
			device, NVSDK_NGX_Version_API, nullptr);
	} __except (CaptureNgxException(GetExceptionCode(), sehCode)) {
		return NVSDK_NGX_Result_FAIL_PlatformError;
	}
}

DWORD WINAPI SnippetGetModuleFileNameW(
	HMODULE module,
	LPWSTR filename,
	DWORD size
) noexcept {
	if (module == g_snippetCallerModule.load(std::memory_order_acquire)) {
		constexpr wchar_t AUTHORIZED_CALLER[] = L"nvngx.dll";
		constexpr DWORD AUTHORIZED_CALLER_LENGTH = ARRAYSIZE(AUTHORIZED_CALLER) - 1;
		if (!filename || !size) {
			SetLastError(ERROR_INSUFFICIENT_BUFFER);
			return 0;
		}
		if (size <= AUTHORIZED_CALLER_LENGTH) {
			if (size > 1) {
				std::memcpy(filename, AUTHORIZED_CALLER, (size - 1) * sizeof(wchar_t));
			}
			filename[size - 1] = L'\0';
			SetLastError(ERROR_INSUFFICIENT_BUFFER);
			return size;
		}
		std::memcpy(filename, AUTHORIZED_CALLER, sizeof(AUTHORIZED_CALLER));
		return AUTHORIZED_CALLER_LENGTH;
	}

	const auto original =
		g_snippetOriginalGetModuleFileNameW.load(std::memory_order_acquire);
	if (original) return original(module, filename, size);
	SetLastError(ERROR_INVALID_FUNCTION);
	return 0;
}

template <typename T>
void* FunctionAddress(T function) noexcept {
	void* result = nullptr;
	static_assert(sizeof(function) == sizeof(result));
	std::memcpy(&result, &function, sizeof(result));
	return result;
}

void** FindImportedFunctionSlot(HMODULE module, const char* functionName) noexcept {
	if (!module || !functionName) return nullptr;
	auto* base = reinterpret_cast<std::byte*>(module);
	const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
	if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return nullptr;
	const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE ||
		nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
		return nullptr;
	}

	const auto& directory =
		nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
	if (!directory.VirtualAddress || !directory.Size ||
		directory.VirtualAddress >= nt->OptionalHeader.SizeOfImage ||
		directory.Size > nt->OptionalHeader.SizeOfImage ||
		directory.VirtualAddress > nt->OptionalHeader.SizeOfImage - directory.Size) {
		return nullptr;
	}

	auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
		base + directory.VirtualAddress);
	const auto* descriptorEnd = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(
		base + directory.VirtualAddress + directory.Size);
	for (; descriptor < descriptorEnd && descriptor->Name; ++descriptor) {
		if (descriptor->Name >= nt->OptionalHeader.SizeOfImage) continue;
		const char* libraryName = reinterpret_cast<const char*>(base + descriptor->Name);
		if (_stricmp(libraryName, "KERNEL32.dll") != 0 &&
			_stricmp(libraryName, "api-ms-win-core-libraryloader-l1-2-0.dll") != 0 &&
			_stricmp(libraryName, "api-ms-win-core-libraryloader-l1-1-0.dll") != 0) {
			continue;
		}
		if (!descriptor->OriginalFirstThunk || !descriptor->FirstThunk) continue;
		auto* nameThunk = reinterpret_cast<IMAGE_THUNK_DATA64*>(
			base + descriptor->OriginalFirstThunk);
		auto* addressThunk = reinterpret_cast<IMAGE_THUNK_DATA64*>(
			base + descriptor->FirstThunk);
		for (; nameThunk->u1.AddressOfData; ++nameThunk, ++addressThunk) {
			if (IMAGE_SNAP_BY_ORDINAL64(nameThunk->u1.Ordinal)) continue;
			const uint32_t nameRva = static_cast<uint32_t>(nameThunk->u1.AddressOfData);
			if (nameRva >= nt->OptionalHeader.SizeOfImage) return nullptr;
			const auto* import = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + nameRva);
			if (std::strcmp(reinterpret_cast<const char*>(import->Name), functionName) == 0) {
				return reinterpret_cast<void**>(&addressThunk->u1.Function);
			}
		}
	}
	return nullptr;
}

bool InstallSnippetCallerCompatibility(DLSSNRFilter::Impl& impl) noexcept {
	impl.snippetGetModuleFileNameIatSlot =
		FindImportedFunctionSlot(impl.snippetModule, "GetModuleFileNameW");
	if (!impl.snippetGetModuleFileNameIatSlot) {
		Logger::Get().Error(
			"DLSSNR signed snippet has no GetModuleFileNameW import");
		return false;
	}

	void* expectedOwner = nullptr;
	if (!g_snippetCallerHookOwner.compare_exchange_strong(
		expectedOwner, &impl, std::memory_order_acq_rel)) {
		Logger::Get().Error(
			"DLSSNR signed snippet caller compatibility is already owned by another session");
		return false;
	}

	void* hookAddress = FunctionAddress(&SnippetGetModuleFileNameW);
	HMODULE callerModule = nullptr;
	if (!GetModuleHandleExW(
		GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
			GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		reinterpret_cast<LPCWSTR>(hookAddress), &callerModule)) {
		g_snippetCallerHookOwner.store(nullptr, std::memory_order_release);
		Logger::Get().Win32Error(
			"Resolve DLSSNR signed snippet caller module failed");
		return false;
	}

	DWORD oldProtection = 0;
	if (!VirtualProtect(
		impl.snippetGetModuleFileNameIatSlot, sizeof(void*),
		PAGE_READWRITE, &oldProtection)) {
		g_snippetCallerHookOwner.store(nullptr, std::memory_order_release);
		Logger::Get().Win32Error(
			"Make DLSSNR signed snippet IAT writable failed");
		return false;
	}

	g_snippetCallerModule.store(callerModule, std::memory_order_release);
	void* original = InterlockedExchangePointer(
		reinterpret_cast<void* volatile*>(impl.snippetGetModuleFileNameIatSlot),
		hookAddress);
	DLSSNRFilter::Impl::GetModuleFileNameWFn originalFunction = nullptr;
	std::memcpy(&originalFunction, &original, sizeof(original));
	g_snippetOriginalGetModuleFileNameW.store(
		originalFunction, std::memory_order_release);
	impl.snippetCallerHookInstalled = true;

	DWORD ignoredProtection = 0;
	if (!VirtualProtect(
		impl.snippetGetModuleFileNameIatSlot, sizeof(void*),
		oldProtection, &ignoredProtection)) {
		Logger::Get().Win32Error(
			"Restore DLSSNR signed snippet IAT protection failed");
	}
	FlushInstructionCache(
		GetCurrentProcess(), impl.snippetGetModuleFileNameIatSlot, sizeof(void*));
	if (!originalFunction) {
		Logger::Get().Error(
			"DLSSNR signed snippet GetModuleFileNameW import was null");
		return false;
	}
	return true;
}

bool RestoreSnippetCallerCompatibility(DLSSNRFilter::Impl& impl) noexcept {
	if (!impl.snippetCallerHookInstalled) return true;
	const auto original =
		g_snippetOriginalGetModuleFileNameW.load(std::memory_order_acquire);
	DWORD oldProtection = 0;
	if (!impl.snippetGetModuleFileNameIatSlot || !VirtualProtect(
			impl.snippetGetModuleFileNameIatSlot, sizeof(void*),
			PAGE_READWRITE, &oldProtection)) {
		Logger::Get().Win32Error(
			"Restore DLSSNR signed snippet caller IAT failed");
		return false;
	}

	InterlockedExchangePointer(
		reinterpret_cast<void* volatile*>(impl.snippetGetModuleFileNameIatSlot),
		FunctionAddress(original));
	DWORD ignoredProtection = 0;
	VirtualProtect(
		impl.snippetGetModuleFileNameIatSlot, sizeof(void*),
		oldProtection, &ignoredProtection);
	FlushInstructionCache(
		GetCurrentProcess(), impl.snippetGetModuleFileNameIatSlot, sizeof(void*));
	impl.snippetCallerHookInstalled = false;
	impl.snippetGetModuleFileNameIatSlot = nullptr;
	g_snippetOriginalGetModuleFileNameW.store(nullptr, std::memory_order_release);
	g_snippetCallerModule.store(nullptr, std::memory_order_release);
	void* expectedOwner = &impl;
	g_snippetCallerHookOwner.compare_exchange_strong(
		expectedOwner, nullptr, std::memory_order_acq_rel);
	return true;
}

}

static bool WaitForFence(DLSSNRFilter::Impl& impl, uint64_t value) noexcept {
	if (!value || impl.fence12->GetCompletedValue() >= value) {
		return true;
	}
	if (!impl.fenceEvent) {
		Logger::Get().Error("DLSSNR fence event is unavailable");
		return false;
	}
	ResetEvent(impl.fenceEvent.get());
	const HRESULT hr = impl.fence12->SetEventOnCompletion(
		value, impl.fenceEvent.get());
	if (FAILED(hr)) {
		Logger::Get().ComError("Set DLSSNR fence event failed", hr);
		return false;
	}
	impl.fenceEvent.wait();
	return true;
}

static bool WaitForQueue(DLSSNRFilter::Impl& impl) noexcept {
	const uint64_t value = ++impl.fenceValue;
	HRESULT hr = impl.queue12->Signal(impl.fence12.get(), value);
	if (FAILED(hr)) {
		Logger::Get().ComError("Signal DLSSNR D3D12 fence failed", hr);
		return false;
	}
	return WaitForFence(impl, value);
}

static void CollectGpuTiming(
	DLSSNRFilter::Impl& impl,
	DLSSNRFilter::Impl::CommandSlot& slot
) noexcept {
	if (!slot.timestampPending || !impl.timestampReadback ||
		!impl.timestampFrequency) return;
	const size_t offset = size_t(slot.timestampQuery) * sizeof(uint64_t);
	const D3D12_RANGE readRange{ offset, offset + sizeof(uint64_t) * 2 };
	void* mapped = nullptr;
	const HRESULT hr = impl.timestampReadback->Map(0, &readRange, &mapped);
	if (FAILED(hr) || !mapped) {
		Logger::Get().ComError("Read DLSSNR GPU timestamp failed", hr);
		slot.timestampPending = false;
		return;
	}
	const auto* timestamps = reinterpret_cast<const uint64_t*>(
		static_cast<const uint8_t*>(mapped) + offset);
	if (timestamps[1] >= timestamps[0]) {
		const double gpuMs = double(timestamps[1] - timestamps[0]) * 1000.0 /
			double(impl.timestampFrequency);
		impl.evaluateGpuTimings.Add(gpuMs);
		FrameGuidancePerformance::PublishDlssnrGpuTiming(gpuMs);
	}
	const D3D12_RANGE writtenRange{ 0, 0 };
	impl.timestampReadback->Unmap(0, &writtenRange);
	slot.timestampPending = false;
}

static std::string FormatTimingSummary(
	std::string_view name,
	const TimingSummary& value
) {
	return fmt::format(
		"{}[n={} avg={:.3f} p95={:.3f} p99={:.3f} max={:.3f}]",
		name, value.count, value.average, value.p95, value.p99, value.maximum);
}

DLSSNRFilter::Impl::~Impl() {
	if (queue12 && fence12) {
		WaitForQueue(*this);
	}
	if (feature) {
		DWORD sehCode = 0;
		const auto function = useSignedSnippet ? snippetReleaseFeature :
			static_cast<ReleaseFeatureFn>(&NVSDK_NGX_D3D12_ReleaseFeature);
		const NVSDK_NGX_Result result = function ?
			CallReleaseFeatureSafely(function, feature, &sehCode) :
			NVSDK_NGX_Result_FAIL_NotInitialized;
		if (sehCode) {
			Logger::Get().Warn(fmt::format(
				"DLSSNR ReleaseFeature raised SEH {:#x}", sehCode));
		} else if (!NGXSucceeded(result)) {
			Logger::Get().Warn(fmt::format(
				"DLSSNR ReleaseFeature failed ({:#x})", (uint32_t)result));
		}
		feature = nullptr;
	}
	if (parameters) {
		if (!coreOwner || !coreOwner->DestroyParameters(parameters, "DLSSNR")) {
			Logger::Get().Warn("DLSSNR shared Core parameter destruction failed");
		}
		parameters = nullptr;
	}
	if (snippetInitialized && snippetShutdown && device12) {
		DWORD sehCode = 0;
		const NVSDK_NGX_Result result =
			CallShutdownSafely(snippetShutdown, device12.get(), &sehCode);
		if (sehCode) {
			Logger::Get().Warn(fmt::format(
				"DLSSNR signed snippet Shutdown1 raised SEH {:#x}", sehCode));
		} else if (!NGXSucceeded(result)) {
			Logger::Get().Warn(fmt::format(
				"DLSSNR signed snippet Shutdown1 failed ({:#x})", (uint32_t)result));
		}
		snippetInitialized = false;
	}
	const bool callerCompatibilityRestored =
		RestoreSnippetCallerCompatibility(*this);
	if (snippetModule) {
		if (callerCompatibilityRestored) {
			if (!FreeLibrary(snippetModule)) {
				Logger::Get().Win32Error(
					"Release DLSSNR signed snippet DLL failed");
			}
		} else {
			Logger::Get().Warn(
				"DLSSNR signed snippet DLL retained because caller IAT restoration failed");
		}
		snippetModule = nullptr;
	}
	if (coreRegistered && coreOwner) {
		coreOwner->Release("DLSSNR");
		coreRegistered = false;
	}
}

static bool CreateSharedTexture(
	DLSSNRFilter::Impl& impl,
	const D3D11_TEXTURE2D_DESC& sourceDesc,
	bool allowUav,
	winrt::com_ptr<ID3D11Texture2D>& texture11,
	winrt::com_ptr<ID3D12Resource>& texture12
) noexcept {
	D3D11_TEXTURE2D_DESC desc = sourceDesc;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.CPUAccessFlags = 0;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE |
		(allowUav ? D3D11_BIND_UNORDERED_ACCESS : 0);
	desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED |
		D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

	HRESULT hr = impl.device11->CreateTexture2D(&desc, nullptr, texture11.put());
	if (FAILED(hr)) {
		Logger::Get().ComError("Create DLSSNR shared D3D11 texture failed", hr);
		return false;
	}

	winrt::com_ptr<IDXGIResource1> dxgiResource;
	hr = texture11->QueryInterface(IID_PPV_ARGS(dxgiResource.put()));
	if (FAILED(hr)) {
		Logger::Get().ComError("Query DLSSNR shared IDXGIResource1 failed", hr);
		return false;
	}
	HANDLE rawHandle = nullptr;
	hr = dxgiResource->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &rawHandle);
	if (FAILED(hr)) {
		Logger::Get().ComError("Create DLSSNR texture shared handle failed", hr);
		return false;
	}
	wil::unique_handle handle(rawHandle);
	hr = impl.device12->OpenSharedHandle(handle.get(), IID_PPV_ARGS(texture12.put()));
	if (FAILED(hr)) {
		Logger::Get().ComError("Open DLSSNR texture in D3D12 failed", hr);
		return false;
	}
	return true;
}

static bool UpdateGuidanceResources(
	DLSSNRFilter::Impl& impl,
	const FrameGuidanceView& view,
	FrameGuidanceFrameId frameId
) noexcept {
	return impl.guidanceInterop && impl.guidanceInterop->Update(
		view, frameId, { impl.width, impl.height });
}

static void SetSubrect(
	NVSDK_NGX_Parameter* parameters,
	const ResourceParameters& resource,
	FrameGuidanceRegion region
) {
	parameters->Set(resource.baseX, region.x);
	parameters->Set(resource.baseY, region.y);
	parameters->Set(resource.width, region.width);
	parameters->Set(resource.height, region.height);
}

static NVSDK_NGX_Result NVSDK_CONV SetScalingRatioCallback(
	NVSDK_NGX_Parameter* parameters
) noexcept {
	__try {
		if (!parameters) return NVSDK_NGX_Result_FAIL_InvalidParameter;
		parameters->Set(PARAM_SCALING_RATIO, 1.0f);
		return NVSDK_NGX_Result_Success;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return NVSDK_NGX_Result_FAIL_PlatformError;
	}
}

static void SetCreateParametersUnsafe(
	DLSSNRFilter::Impl& impl,
	const DLSSNRSettings& settings
) {
	impl.parameters->Set(PARAM_WIDTH, impl.width);
	impl.parameters->Set(PARAM_HEIGHT, impl.height);
	impl.parameters->Set(PARAM_INPUT_WIDTH, impl.width);
	impl.parameters->Set(PARAM_INPUT_HEIGHT, impl.height);
	impl.parameters->Set(PARAM_OUTPUT_WIDTH, impl.width);
	impl.parameters->Set(PARAM_OUTPUT_HEIGHT, impl.height);
	impl.parameters->Set(PARAM_OUTPUT_DOT_WIDTH, impl.width);
	impl.parameters->Set(PARAM_OUTPUT_DOT_HEIGHT, impl.height);
	impl.parameters->Set(PARAM_UPSCALING, 0u);
	impl.parameters->Set(PARAM_SCALE, 1.0f);
	impl.parameters->Set(PARAM_SCALING_RATIO, 1.0f);
	impl.parameters->Set(
		PARAM_SCALING_RATIO_CALLBACK,
		FunctionAddress(&SetScalingRatioCallback));
	impl.parameters->Set(PARAM_PRESET, settings.preset);
	impl.parameters->Set(NVSDK_NGX_Parameter_Width, impl.width);
	impl.parameters->Set(NVSDK_NGX_Parameter_Height, impl.height);
	impl.parameters->Set(
		NVSDK_NGX_Parameter_PerfQualityValue,
		static_cast<int>(NVSDK_NGX_PerfQuality_Value_Balanced));
	impl.parameters->Set(NVSDK_NGX_Parameter_CreationNodeMask, 1u);
	impl.parameters->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
}

static bool SetCreateParametersSafely(
	DLSSNRFilter::Impl& impl,
	const DLSSNRSettings& settings,
	DWORD* sehCode
) noexcept {
	*sehCode = 0;
	__try {
		SetCreateParametersUnsafe(impl, settings);
		return true;
	} __except (CaptureNgxException(GetExceptionCode(), sehCode)) {
		return false;
	}
}

static bool InitializeSignedSnippet(
	DLSSNRFilter::Impl& impl,
	const std::filesystem::path& applicationDirectory
) noexcept {
	const std::filesystem::path dllPath =
		applicationDirectory / L"nvngx_dlssnr.dll";
	impl.snippetModule = LoadLibraryExW(
		dllPath.c_str(), nullptr,
		LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
	if (!impl.snippetModule) {
		Logger::Get().Win32Error("Load signed nvngx_dlssnr.dll failed");
		return false;
	}

	impl.snippetInitExt = GetExport<DLSSNRFilter::Impl::SnippetInitExtFn>(
		impl.snippetModule, "NVSDK_NGX_D3D12_Init_Ext");
	impl.snippetCreateFeature = GetExport<DLSSNRFilter::Impl::CreateFeatureFn>(
		impl.snippetModule, "NVSDK_NGX_D3D12_CreateFeature");
	impl.snippetEvaluateFeature = GetExport<DLSSNRFilter::Impl::EvaluateFeatureFn>(
		impl.snippetModule, "NVSDK_NGX_D3D12_EvaluateFeature");
	impl.snippetReleaseFeature = GetExport<DLSSNRFilter::Impl::ReleaseFeatureFn>(
		impl.snippetModule, "NVSDK_NGX_D3D12_ReleaseFeature");
	impl.snippetShutdown = GetExport<DLSSNRFilter::Impl::ShutdownFn>(
		impl.snippetModule, "NVSDK_NGX_D3D12_Shutdown1");
	if (!impl.snippetInitExt || !impl.snippetCreateFeature ||
		!impl.snippetEvaluateFeature || !impl.snippetReleaseFeature ||
		!impl.snippetShutdown) {
		Logger::Get().Error("DLSSNR signed snippet exports are incomplete");
		return false;
	}
	if (!InstallSnippetCallerCompatibility(impl)) return false;

	DWORD sehCode = 0;
	const NVSDK_NGX_Result result = CallSnippetInitSafely(
		impl.snippetInitExt, applicationDirectory.c_str(),
		impl.device12.get(), &sehCode);
	if (sehCode) {
		Logger::Get().Error(fmt::format(
			"DLSSNR signed snippet Init_Ext raised SEH {:#x}", sehCode));
		return false;
	}
	if (!NGXSucceeded(result)) {
		Logger::Get().Error(fmt::format(
			"DLSSNR signed snippet Init_Ext failed ({:#x})", (uint32_t)result));
		return false;
	}
	impl.snippetInitialized = true;
	impl.useSignedSnippet = true;
	return true;
}

static void SetEvaluateParametersUnsafe(
	DLSSNRFilter::Impl& impl,
	const DLSSNRSettings& settings,
	const FrameGuidanceView& guidance,
	bool guidanceReset
) {
	impl.parameters->Set(PARAM_COLOR, impl.sharedInput12.get());
	impl.parameters->Set(PARAM_OUTPUT, impl.sharedOutput12.get());
	impl.parameters->Set(PARAM_MVEC, impl.guidanceInterop->Motion());
	impl.parameters->Set(PARAM_DEPTH, impl.guidanceInterop->Depth());
	const FrameGuidanceRegion full = FrameGuidanceRegion::Full(
		{ impl.width, impl.height });
	SetSubrect(impl.parameters, RESOURCE_PARAMETERS[0], full);
	SetSubrect(impl.parameters, RESOURCE_PARAMETERS[1], full);
	SetSubrect(impl.parameters, RESOURCE_PARAMETERS[2],
		guidance.motion.metadata.validRegion);
	SetSubrect(impl.parameters, RESOURCE_PARAMETERS[3],
		guidance.depth.metadata.validRegion);
	impl.parameters->Set(PARAM_MVEC_SCALE_X, 1.0f);
	impl.parameters->Set(PARAM_MVEC_SCALE_Y, 1.0f);
	impl.parameters->Set(PARAM_DEPTH_INVERTED, 1);
	impl.parameters->Set(PARAM_INDICATOR_INVERT_X, 0);
	impl.parameters->Set(PARAM_INDICATOR_INVERT_Y, 0);
	impl.parameters->Set(PARAM_ENABLED, 1);
	impl.parameters->Set(
		PARAM_RESET, impl.resetHistory || guidanceReset ? 1 : 0);
	impl.parameters->Set(PARAM_STYLE, settings.style);
	impl.parameters->Set(PARAM_INTENSITY, settings.intensity);
	impl.parameters->Set(PARAM_LOCAL_TONE, settings.localToneStrength);
	impl.parameters->Set(PARAM_LOCAL_STRUCTURE, settings.localStructureStrength);
	impl.parameters->Set(PARAM_SKIN_STRUCTURE, settings.skinStructureStrength);
	impl.parameters->Set(PARAM_AUTO_MASK, settings.useAutoMask ? 1 : 0);
	impl.parameters->Set(PARAM_UI_CORRECTION, settings.uiCorrection ? 1 : 0);
}

static bool SetEvaluateParametersSafely(
	DLSSNRFilter::Impl& impl,
	const DLSSNRSettings& settings,
	const FrameGuidanceView& guidance,
	bool guidanceReset,
	DWORD* sehCode
) noexcept {
	*sehCode = 0;
	__try {
		SetEvaluateParametersUnsafe(impl, settings, guidance, guidanceReset);
		return true;
	} __except (CaptureNgxException(GetExceptionCode(), sehCode)) {
		return false;
	}
}

static bool PrepareInput(
	DLSSNRFilter::Impl& impl,
	ID3D11Texture2D* input
) noexcept {
	if (!impl.convertInputToRgba) {
		impl.context11->CopyResource(impl.sharedInput11.get(), input);
		return true;
	}
	ID3D11ShaderResourceView* srv = impl.inputSrv11.get();
	ID3D11UnorderedAccessView* uav = impl.sharedInputUav11.get();
	impl.context11->CSSetShader(impl.colorConvertShader11.get(), nullptr, 0);
	impl.context11->CSSetShaderResources(0, 1, &srv);
	impl.context11->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
	impl.context11->Dispatch((impl.width + 7) / 8, (impl.height + 7) / 8, 1);
	ID3D11ShaderResourceView* nullSrv = nullptr;
	ID3D11UnorderedAccessView* nullUav = nullptr;
	impl.context11->CSSetShaderResources(0, 1, &nullSrv);
	impl.context11->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
	impl.context11->CSSetShader(nullptr, nullptr, 0);
	return true;
}

DLSSNRFilter::DLSSNRFilter() = default;
DLSSNRFilter::~DLSSNRFilter() = default;

FrameGuidanceRequirements
DLSSNRFilter::GetFrameGuidanceRequirements() const noexcept {
	if (!_impl || _impl->disabled) return {};
	FrameGuidanceRequirements result{ .zero = true };
	switch (_settings.guidanceMode) {
	case 0:
		result.motion = true;
		result.depth = true;
		break;
	case 1:
		break;
	case 2:
		result.motion = true;
		break;
	case 3:
		result.depth = true;
		break;
	default:
		break;
	}
	result.depthInferenceInterval = _settings.depthInferenceInterval;
	return result;
}

NativeEffectParameterUpdate DLSSNRFilter::UpdateParameters(
	const EffectOption& option
) noexcept {
	const DLSSNRSettings settings = ParseDLSSNRSettings(option);

	// NR Preset 在创建特征时写入，只能通过重建生效。其余参数每帧提交给 NGX，
	// 直接替换即可实时生效
	if (settings.preset != _settings.preset) {
		return NativeEffectParameterUpdate::NeedsRecreate;
	}

	_settings = settings;
	return NativeEffectParameterUpdate::Applied;
}

bool DLSSNRFilter::Initialize(
	DeviceResources& resources,
	NgxD3D12Core& ngxCore,
	ID3D11Texture2D* input,
	ID3D11Texture2D* output,
	const DLSSNRSettings& settings
) noexcept {
	_settings = settings;
	_ngxCore = &ngxCore;
	_impl.reset();
	FrameGuidancePerformance::ResetDlssnrGpuTiming();
	auto impl = std::make_unique<Impl>();
	impl->device11 = resources.GetD3DDevice();
	impl->context11 = resources.GetD3DDC();
	impl->coreOwner = &ngxCore;

	D3D11_TEXTURE2D_DESC inputDesc{};
	D3D11_TEXTURE2D_DESC outputDesc{};
	input->GetDesc(&inputDesc);
	output->GetDesc(&outputDesc);
	if (inputDesc.Width != outputDesc.Width || inputDesc.Height != outputDesc.Height) {
		Logger::Get().Error(fmt::format(
			"DLSSNR requires same-resolution input/output: {}x{} -> {}x{}",
			inputDesc.Width, inputDesc.Height, outputDesc.Width, outputDesc.Height));
		return false;
	}
	const bool supportedInput = inputDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
		inputDesc.Format == DXGI_FORMAT_B8G8R8A8_UNORM;
	if (!supportedInput || outputDesc.Format != DXGI_FORMAT_R8G8B8A8_UNORM) {
		Logger::Get().Error(fmt::format(
			"DLSSNR SDR path unsupported formats: input={}, output={}",
			(uint32_t)inputDesc.Format, (uint32_t)outputDesc.Format));
		return false;
	}
	impl->width = inputDesc.Width;
	impl->height = inputDesc.Height;
	impl->convertInputToRgba = inputDesc.Format == DXGI_FORMAT_B8G8R8A8_UNORM;

	if (!ngxCore.Acquire(resources, "DLSSNR")) {
		return false;
	}
	impl->coreRegistered = true;
	impl->device12.copy_from(ngxCore.Device());
	HRESULT hr = S_OK;
	D3D12_COMMAND_QUEUE_DESC queueDesc{};
	queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	hr = impl->device12->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(impl->queue12.put()));
	if (SUCCEEDED(hr)) {
		hr = impl->device12->CreateCommandAllocator(
			D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(impl->allocator12.put()));
	}
	if (SUCCEEDED(hr)) {
		hr = impl->device12->CreateCommandList(
			0, D3D12_COMMAND_LIST_TYPE_DIRECT, impl->allocator12.get(), nullptr,
			IID_PPV_ARGS(impl->commandList12.put()));
	}
	if (FAILED(hr)) {
		Logger::Get().ComError("Create DLSSNR D3D12 command objects failed", hr);
		return false;
	}

	D3D11_TEXTURE2D_DESC sharedDesc = outputDesc;
	sharedDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	if (!CreateSharedTexture(*impl, sharedDesc, true,
		impl->sharedInput11, impl->sharedInput12) ||
		!CreateSharedTexture(*impl, sharedDesc, true,
			impl->sharedOutput11, impl->sharedOutput12)) {
		return false;
	}
	if (impl->convertInputToRgba) {
		hr = impl->device11->CreateShaderResourceView(
			input, nullptr, impl->inputSrv11.put());
		if (SUCCEEDED(hr)) {
			hr = impl->device11->CreateUnorderedAccessView(
				impl->sharedInput11.get(), nullptr, impl->sharedInputUav11.put());
		}
		winrt::com_ptr<ID3DBlob> shaderBlob;
		if (SUCCEEDED(hr) && !DirectXHelper::CompileComputeShader(
			COLOR_CONVERT_HLSL, "ConvertToRgba", shaderBlob.put(),
			"DLSSNRColorConvert")) {
			hr = E_FAIL;
		}
		if (SUCCEEDED(hr)) {
			hr = impl->device11->CreateComputeShader(
				shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize(), nullptr,
				impl->colorConvertShader11.put());
		}
		if (FAILED(hr)) {
			Logger::Get().ComError("Create DLSSNR BGRA conversion resources failed", hr);
			return false;
		}
	}

	hr = impl->device11->CreateFence(
		0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(impl->fence11.put()));
	if (FAILED(hr)) {
		Logger::Get().ComError("Create DLSSNR shared fence failed", hr);
		return false;
	}
	HANDLE rawFence = nullptr;
	hr = impl->fence11->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &rawFence);
	if (FAILED(hr)) {
		Logger::Get().ComError("Create DLSSNR fence shared handle failed", hr);
		return false;
	}
	wil::unique_handle fenceHandle(rawFence);
	hr = impl->device12->OpenSharedHandle(
		fenceHandle.get(), IID_PPV_ARGS(impl->fence12.put()));
	if (FAILED(hr)) {
		Logger::Get().ComError("Open DLSSNR shared fence in D3D12 failed", hr);
		return false;
	}
	hr = impl->fenceEvent.create();
	if (FAILED(hr)) {
		Logger::Get().ComError("Create reusable DLSSNR fence event failed", hr);
		return false;
	}

	D3D12_QUERY_HEAP_DESC queryDesc{};
	queryDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
	queryDesc.Count = Impl::COMMAND_SLOT_COUNT * 2;
	hr = impl->device12->CreateQueryHeap(
		&queryDesc, IID_PPV_ARGS(impl->timestampQueryHeap.put()));
	D3D12_HEAP_PROPERTIES readbackHeap{};
	readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;
	readbackHeap.CreationNodeMask = 1;
	readbackHeap.VisibleNodeMask = 1;
	D3D12_RESOURCE_DESC readbackDesc{};
	readbackDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	readbackDesc.Width = uint64_t(queryDesc.Count) * sizeof(uint64_t);
	readbackDesc.Height = 1;
	readbackDesc.DepthOrArraySize = 1;
	readbackDesc.MipLevels = 1;
	readbackDesc.SampleDesc.Count = 1;
	readbackDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	if (SUCCEEDED(hr)) {
		hr = impl->device12->CreateCommittedResource(
			&readbackHeap, D3D12_HEAP_FLAG_NONE, &readbackDesc,
			D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
			IID_PPV_ARGS(impl->timestampReadback.put()));
	}
	if (SUCCEEDED(hr)) {
		hr = impl->queue12->GetTimestampFrequency(&impl->timestampFrequency);
	}
	if (FAILED(hr) || !impl->timestampFrequency) {
		Logger::Get().Warn(fmt::format(
			"DLSSNR GPU timestamp telemetry unavailable ({:#x})",
			static_cast<uint32_t>(hr)));
		impl->timestampQueryHeap = nullptr;
		impl->timestampReadback = nullptr;
		impl->timestampFrequency = 0;
	}

	const std::filesystem::path applicationDirectory =
		Win32Helper::GetExePath().parent_path();
	DWORD sehCode = 0;
	NVSDK_NGX_Result result = NVSDK_NGX_Result_Success;

	if constexpr (!ENABLE_CORE_FEATURE18_DIAGNOSTIC) {
		if (!InitializeSignedSnippet(*impl, applicationDirectory)) return false;
	}

	if (!ngxCore.AllocateParameters(&impl->parameters, "DLSSNR")) {
		return false;
	}
	sehCode = 0;
	if (!SetCreateParametersSafely(*impl, _settings, &sehCode)) {
		Logger::Get().Error(fmt::format(
			"DLSSNR creation parameter setup raised SEH {:#x}", sehCode));
		return false;
	}

	const Impl::CreateFeatureFn createFeature =
		ENABLE_CORE_FEATURE18_DIAGNOSTIC ?
		static_cast<Impl::CreateFeatureFn>(&NVSDK_NGX_D3D12_CreateFeature) :
		impl->snippetCreateFeature;
	sehCode = 0;
	result = CallCreateFeatureSafely(
		createFeature, impl->commandList12.get(), FEATURE_DLSSNR,
		impl->parameters, &impl->feature, &sehCode);
	if (sehCode) {
		Logger::Get().Error(fmt::format(
			"DLSSNR {} CreateFeature raised SEH {:#x}",
			ENABLE_CORE_FEATURE18_DIAGNOSTIC ? "Core diagnostic" : "signed snippet",
			sehCode));
		return false;
	}
	if (!NGXSucceeded(result) || !impl->feature) {
		Logger::Get().Error(fmt::format(
			"DLSSNR {} Feature 18 creation failed ({:#x})",
			ENABLE_CORE_FEATURE18_DIAGNOSTIC ? "Core diagnostic" : "signed snippet",
			(uint32_t)result));
		return false;
	}

	hr = impl->commandList12->Close();
	if (FAILED(hr)) {
		Logger::Get().ComError("Close DLSSNR initialization command list failed", hr);
		return false;
	}
	ID3D12CommandList* lists[]{ impl->commandList12.get() };
	impl->queue12->ExecuteCommandLists(1, lists);
	if (!WaitForQueue(*impl)) {
		return false;
	}
	for (uint32_t i = 0; i < impl->commandSlots.size(); ++i) {
		Impl::CommandSlot& slot = impl->commandSlots[i];
		slot.timestampQuery = i * 2;
		hr = impl->device12->CreateCommandAllocator(
			D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(slot.allocator.put()));
		if (SUCCEEDED(hr)) {
			hr = impl->device12->CreateCommandList(
				0, D3D12_COMMAND_LIST_TYPE_DIRECT, slot.allocator.get(), nullptr,
				IID_PPV_ARGS(slot.commandList.put()));
		}
		if (SUCCEEDED(hr)) hr = slot.commandList->Close();
		if (FAILED(hr)) {
			Logger::Get().ComError("Create DLSSNR command ring failed", hr);
			return false;
		}
	}
	impl->guidanceInterop = std::make_unique<FrameGuidanceD3D12Interop>();
	if (!impl->guidanceInterop->Initialize(
		impl->device12.get(), impl->fence12.get())) {
		return false;
	}

	LogDlssnrStatus(fmt::format(
		"DLSSNR STATUS: Feature=18 created=true path={} size={}x{} preset={} "
		"style={} intensity={} localTone={} localStructure={} skinStructure={} "
		"guidanceMode={} autoMask={} uiCorrection={} depthInterval={} disabled=false",
		ENABLE_CORE_FEATURE18_DIAGNOSTIC ? "core-diagnostic" : "signed-snippet",
		impl->width, impl->height, _settings.preset, _settings.style,
		_settings.intensity, _settings.localToneStrength,
		_settings.localStructureStrength, _settings.skinStructureStrength,
		_settings.guidanceMode, _settings.useAutoMask, _settings.uiCorrection,
		_settings.depthInferenceInterval));
	_impl = std::move(impl);
	return true;
}

bool DLSSNRFilter::Resize(
	DeviceResources& resources,
	ID3D11Texture2D* input,
	ID3D11Texture2D* output
) noexcept {
	return _ngxCore && Initialize(resources, *_ngxCore, input, output, _settings);
}

bool DLSSNRFilter::Drain() noexcept {
	return !_impl || !_impl->queue12 || !_impl->fence12 || WaitForQueue(*_impl);
}

static FrameGuidanceView SelectGuidance(
	const NativeEffectDrawContext& context,
	const DLSSNRSettings& settings,
	FrameGuidanceExtent extent
) noexcept {
	FrameGuidanceView selected = context.frameGuidance;
	const FrameGuidanceView& zero = context.zeroFrameGuidance;
	if (!selected.IsValidFor(context.frameId, extent)) selected = zero;
	switch (settings.guidanceMode) {
	case 1:
		selected = zero;
		break;
	case 2:
		selected.depth = zero.depth;
		selected.rawDepth = {};
		selected.depthResidual = {};
		break;
	case 3:
		selected.motion = zero.motion;
		selected.confidence = zero.confidence;
		break;
	default:
		break;
	}
	selected.requiresHistoryReset =
		selected.depth.metadata.requiresHistoryReset ||
		selected.motion.metadata.requiresHistoryReset ||
		selected.confidence.metadata.requiresHistoryReset;
	return selected.IsValidFor(context.frameId, extent) ? selected : zero;
}

bool DLSSNRFilter::Draw(const NativeEffectDrawContext& context) noexcept {
	if (!_impl || !_impl->feature || !_impl->parameters) {
		return false;
	}
	Impl& impl = *_impl;
	ID3D11Texture2D* input = context.input;
	ID3D11Texture2D* output = context.output;
	auto fail = [&](std::string_view stage) noexcept {
		impl.disabled = true;
		LogDlssnrStatus(fmt::format(
			"DLSSNR STATUS: Feature=18 frameId={} stage={} result=internal-failure "
			"disabled=true fallback=pass-through-next-frame",
			context.frameId, stage), true);
		return false;
	};
	Impl::CommandSlot& commandSlot =
		impl.commandSlots[impl.nextCommandSlot++ % Impl::COMMAND_SLOT_COUNT];
	const auto slotWaitStart = std::chrono::steady_clock::now();
	if (commandSlot.completionValue &&
		!WaitForFence(impl, commandSlot.completionValue)) {
		return fail("command-slot-wait");
	}
	const double slotWaitMs = std::chrono::duration<double, std::milli>(
		std::chrono::steady_clock::now() - slotWaitStart).count();
	CollectGpuTiming(impl, commandSlot);
	const auto inputPrepareStart = std::chrono::steady_clock::now();
	if (!PrepareInput(impl, input)) return fail("prepare-input");
	const double inputPrepareMs = std::chrono::duration<double, std::milli>(
		std::chrono::steady_clock::now() - inputPrepareStart).count();
	if (impl.disabled) {
		impl.context11->CopyResource(output, impl.sharedInput11.get());
		return true;
	}
	const auto guidancePrepareStart = std::chrono::steady_clock::now();
	const FrameGuidanceView guidance = SelectGuidance(
		context, _settings, { impl.width, impl.height });
	if (!UpdateGuidanceResources(impl, guidance, context.frameId) ||
		!impl.guidanceInterop->WaitForProducer(impl.context11, guidance)) {
		return fail("guidance-interop");
	}
	const double guidancePrepareMs = std::chrono::duration<double, std::milli>(
		std::chrono::steady_clock::now() - guidancePrepareStart).count();
	const uint64_t inputReady = ++impl.fenceValue;
	HRESULT hr = impl.context11->Signal(impl.fence11.get(), inputReady);
	if (FAILED(hr)) {
		return fail("d3d11-input-signal");
	}
	impl.context11->Flush();
	hr = impl.queue12->Wait(impl.fence12.get(), inputReady);
	if (FAILED(hr)) {
		return fail("d3d12-input-wait");
	}
	hr = commandSlot.allocator->Reset();
	if (SUCCEEDED(hr)) {
		hr = commandSlot.commandList->Reset(commandSlot.allocator.get(), nullptr);
	}
	if (FAILED(hr)) {
		return fail("command-list-reset");
	}
	ID3D12GraphicsCommandList* commandList = commandSlot.commandList.get();

	D3D12_RESOURCE_BARRIER barriers[2]{};
	barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barriers[0].Transition = {
		impl.sharedInput12.get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
		D3D12_RESOURCE_STATE_COMMON,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
	};
	barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barriers[1].Transition = {
		impl.sharedOutput12.get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
		D3D12_RESOURCE_STATE_COMMON,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS
	};
	commandList->ResourceBarrier(ARRAYSIZE(barriers), barriers);
	impl.guidanceInterop->Transition(
		commandList, D3D12_RESOURCE_STATE_COMMON,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	const bool guidanceReset = guidance.requiresHistoryReset &&
		impl.lastGuidanceResetFrameId != context.frameId;
	DWORD sehCode = 0;
	if (!SetEvaluateParametersSafely(
		impl, _settings, guidance, guidanceReset, &sehCode)) {
		commandList->Close();
		Logger::Get().Error(fmt::format(
			"DLSSNR evaluation parameter setup raised SEH {:#x}", sehCode));
		return fail("ngx-parameters-seh");
	}
	if (impl.timestampQueryHeap) {
		commandList->EndQuery(
			impl.timestampQueryHeap.get(), D3D12_QUERY_TYPE_TIMESTAMP,
			commandSlot.timestampQuery);
	}
	const auto evaluateStart = std::chrono::steady_clock::now();
	const Impl::EvaluateFeatureFn evaluateFeature = impl.useSignedSnippet ?
		impl.snippetEvaluateFeature :
		static_cast<Impl::EvaluateFeatureFn>(&NVSDK_NGX_D3D12_EvaluateFeature);
	const NVSDK_NGX_Result result = CallEvaluateFeatureSafely(
		evaluateFeature, commandList, impl.feature,
		impl.parameters, &sehCode);
	const double evaluateCpuMs = std::chrono::duration<double, std::milli>(
		std::chrono::steady_clock::now() - evaluateStart).count();
	if (impl.timestampQueryHeap) {
		commandList->EndQuery(
			impl.timestampQueryHeap.get(), D3D12_QUERY_TYPE_TIMESTAMP,
			commandSlot.timestampQuery + 1);
		commandList->ResolveQueryData(
			impl.timestampQueryHeap.get(), D3D12_QUERY_TYPE_TIMESTAMP,
			commandSlot.timestampQuery, 2, impl.timestampReadback.get(),
			uint64_t(commandSlot.timestampQuery) * sizeof(uint64_t));
	}
	++impl.evaluateCount;
	const bool evaluateSucceeded = !sehCode && NGXSucceeded(result);
	if (evaluateSucceeded) {
		++impl.evaluateSuccessCount;
	} else {
		++impl.evaluateFailureCount;
		impl.disabled = true;
		if (sehCode) {
			Logger::Get().Error(fmt::format(
				"DLSSNR EvaluateFeature raised SEH {:#x}", sehCode));
		}
	}
	if (!evaluateSucceeded || impl.evaluateCount <= 8 ||
		impl.evaluateCount % 120 == 0) {
		LogDlssnrStatus(fmt::format(
			"DLSSNR STATUS: Feature=18 frameId={} evaluateCount={} result={:#x} "
			"success={} failures={} guidanceMode={} path={} disabled={}",
			context.frameId, impl.evaluateCount, static_cast<uint32_t>(result),
			impl.evaluateSuccessCount, impl.evaluateFailureCount,
			_settings.guidanceMode,
			impl.useSignedSnippet ? "signed-snippet" : "core-diagnostic",
			impl.disabled), !evaluateSucceeded);
	}
	const auto submitStart = std::chrono::steady_clock::now();
	for (D3D12_RESOURCE_BARRIER& barrier : barriers) {
		std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
	}
	impl.guidanceInterop->Transition(
		commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
		D3D12_RESOURCE_STATE_COMMON);
	commandList->ResourceBarrier(ARRAYSIZE(barriers), barriers);
	hr = commandList->Close();
	if (FAILED(hr)) {
		return fail("command-list-close");
	}
	ID3D12CommandList* lists[]{ commandList };
	impl.queue12->ExecuteCommandLists(1, lists);
	commandSlot.timestampPending = impl.timestampQueryHeap != nullptr;
	commandSlot.timestampFrameId = context.frameId;
	const uint64_t outputReady = ++impl.fenceValue;
	hr = impl.queue12->Signal(impl.fence12.get(), outputReady);
	commandSlot.completionValue = outputReady;
	impl.guidanceInterop->MarkSubmitted(outputReady);
	if (SUCCEEDED(hr)) {
		hr = impl.context11->Wait(impl.fence11.get(), outputReady);
	}
	if (FAILED(hr)) {
		return fail("output-signal-wait");
	}
	impl.context11->CopyResource(
		output, impl.disabled ? impl.sharedInput11.get() : impl.sharedOutput11.get());
	const double submitMs = std::chrono::duration<double, std::milli>(
		std::chrono::steady_clock::now() - submitStart).count();
	impl.slotWaitTimings.Add(slotWaitMs);
	impl.inputPrepareTimings.Add(inputPrepareMs);
	impl.guidancePrepareTimings.Add(guidancePrepareMs);
	impl.evaluateCpuTimings.Add(evaluateCpuMs);
	impl.submitTimings.Add(submitMs);
	if (guidanceReset) {
		impl.lastGuidanceResetFrameId = context.frameId;
	}
	impl.resetHistory = false;
	if (impl.evaluateCount <= 8 || impl.evaluateCount % 120 == 0) {
		if (impl.evaluateCount <= 8) {
			Logger::Get().Info(fmt::format(
				"DLSSNR timing: frameId={} evaluateCount={} slotWait={:.3f} ms "
				"inputPrepare={:.3f} ms guidancePrepare={:.3f} ms "
				"evaluateCPU={:.3f} ms submit={:.3f} ms",
				context.frameId, impl.evaluateCount, slotWaitMs, inputPrepareMs,
				guidancePrepareMs, evaluateCpuMs, submitMs));
		} else {
			Logger::Get().Info(fmt::format(
				"DLSSNR timing 120-frame window: frameId={} evaluateCount={} {} {} {} {} {} {}",
				context.frameId, impl.evaluateCount,
				FormatTimingSummary("slotWait", impl.slotWaitTimings.Summarize()),
				FormatTimingSummary("inputPrepare", impl.inputPrepareTimings.Summarize()),
				FormatTimingSummary("guidancePrepare", impl.guidancePrepareTimings.Summarize()),
				FormatTimingSummary("evaluateCPU", impl.evaluateCpuTimings.Summarize()),
				FormatTimingSummary("submit", impl.submitTimings.Summarize()),
				FormatTimingSummary("evaluateGPU", impl.evaluateGpuTimings.Summarize())));
		}
	}
	return true;
}

}

#else

namespace Magpie {

struct DLSSNRFilter::Impl {};
DLSSNRFilter::DLSSNRFilter() = default;
DLSSNRFilter::~DLSSNRFilter() = default;
FrameGuidanceRequirements
DLSSNRFilter::GetFrameGuidanceRequirements() const noexcept { return {}; }
bool DLSSNRFilter::Initialize(
	DeviceResources&, NgxD3D12Core&, ID3D11Texture2D*, ID3D11Texture2D*,
	const DLSSNRSettings&) noexcept {
	Logger::Get().Error("DLSSNR support is disabled at build time");
	return false;
}
bool DLSSNRFilter::Resize(
	DeviceResources&, ID3D11Texture2D*, ID3D11Texture2D*) noexcept {
	return false;
}
bool DLSSNRFilter::Drain() noexcept { return true; }
NativeEffectParameterUpdate DLSSNRFilter::UpdateParameters(
	const EffectOption&) noexcept {
	return NativeEffectParameterUpdate::NeedsRecreate;
}
bool DLSSNRFilter::Draw(const NativeEffectDrawContext&) noexcept {
	return false;
}

}

#endif
