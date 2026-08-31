#include "pch.h"
#include "NativeEffectBackendFactory.h"
#include "NgxD3D12Core.h"
#include "DLSSNRFilter.h"
#include "DLSSSRUpscaler.h"
#include "FSR2ZeroMVUpscaler.h"
#include "FSR3ZeroMVUpscaler.h"
#include "RTXVideoDenoiser.h"
#include "XeSSZeroMVUpscaler.h"
#include "FrameGuidanceDiagnostics.h"
#include "Logger.h"
#include "ScalingOptions.h"

namespace Magpie {

template <typename T, typename... Args>
static NativeEffectBackendResult CreateBackend(
	std::string_view displayName,
	DeviceResources& resources,
	ID3D11Texture2D* input,
	ID3D11Texture2D* output,
	Args&&... args
) noexcept {
	auto backend = std::make_unique<T>();
	if (!backend->Initialize(
		resources, input, output, std::forward<Args>(args)...)) {
		Logger::Get().Error(fmt::format("Initialize native effect {} failed", displayName));
		return { true, nullptr };
	}
	return { true, std::move(backend) };
}

DLSSNRSettings ParseDLSSNRSettings(const EffectOption& option) noexcept {
	auto getParameter = [&](std::string_view name, float defaultValue) {
		auto it = option.parameters.find(std::string(name));
		return it == option.parameters.end() ? defaultValue : it->second;
	};

	return DLSSNRSettings{
		.preset = std::clamp(
			static_cast<int>(std::lround(
				getParameter("nrPreset", 0.0f))), 0, 3),
		.style = std::clamp(
			static_cast<int>(std::lround(getParameter("style", 0.0f))), 0, 2),
		.intensity = std::clamp(getParameter("intensity", 1.0f), 0.0f, 2.0f),
		.localToneStrength = std::clamp(
			getParameter("localToneStrength", 1.0f), 0.0f, 2.0f),
		.localStructureStrength = std::clamp(
			getParameter("localStructureStrength", 1.0f), 0.0f, 2.0f),
		.skinStructureStrength = std::clamp(
			getParameter("skinStructureStrength", -1.0f), -1.0f, 2.0f),
		.useAutoMask = getParameter("useAutoMask", 0.0f) >= 0.5f,
		.uiCorrection = getParameter("uiCorrection", 0.0f) >= 0.5f,
		.guidanceMode = std::clamp(
			static_cast<int>(std::lround(
				getParameter("guidanceMode", 0.0f))), 0, 3),
		.depthInferenceInterval = static_cast<uint32_t>(std::clamp(
			static_cast<int>(std::lround(
				getParameter("depthInferenceInterval", 4.0f))), 1, 8))
	};
}

NativeEffectBackendResult CreateNativeEffectBackend(
	std::string_view effectName,
	const EffectOption& option,
	DeviceResources& resources,
	NgxD3D12Core& ngxCore,
	ID3D11Texture2D* input,
	ID3D11Texture2D* output
) noexcept {
	if (effectName.starts_with("Diagnostics\\FrameGuidance_")) {
		auto getParameter = [&](std::string_view name, float defaultValue) {
			auto it = option.parameters.find(std::string(name));
			return it == option.parameters.end() ? defaultValue : it->second;
		};
		FrameGuidanceDiagnosticKind kind =
			FrameGuidanceDiagnosticKind::Motion;
		if (effectName.ends_with("Confidence")) {
			kind = FrameGuidanceDiagnosticKind::Confidence;
		} else if (effectName.ends_with("DepthResidual")) {
			kind = FrameGuidanceDiagnosticKind::DepthResidual;
		} else if (effectName.ends_with("Depth")) {
			kind = FrameGuidanceDiagnosticKind::Depth;
		}
		return CreateBackend<FrameGuidanceDiagnostics>(
			effectName, resources, input, output,
			FrameGuidanceDiagnosticSettings{
				.kind = kind,
				.gain = std::max(0.001f, getParameter("gain", 1.0f)),
				.invert = getParameter("invert", 0.0f) >= 0.5f,
				.showRawDepth = getParameter("percentileClip", 1.0f) < 0.5f
			});
	}

	if (effectName == "DLSSNR\\DLSSNR_AI_Filter") {
		const DLSSNRSettings settings = ParseDLSSNRSettings(option);
		auto backend = std::make_unique<DLSSNRFilter>();
		if (!backend->Initialize(resources, ngxCore, input, output, settings)) {
			const char status[] =
				"DLSSNR STATUS: Feature=18 created=false path=unavailable "
				"fallback=pass-through\n";
			Logger::Get().Warn(status);
			OutputDebugStringA(status);
			return {};
		}
		return { true, std::move(backend) };
	}

	if (effectName == "DLSS\\DLSS_SR" ||
		effectName == "DLSS\\DLSS_ZeroMV" ||
		effectName == "DLSS\\DLSS_ZeroMV_Jitter" ||
		effectName == "DLSS\\DLSS_OpticalFlow") {
		auto getParameter = [&](std::string_view name, float defaultValue) {
			auto it = option.parameters.find(std::string(name));
			return it == option.parameters.end() ? defaultValue : it->second;
		};
		const bool isJitter = effectName == "DLSS\\DLSS_ZeroMV_Jitter";
		const bool isLegacyOpticalFlow =
			effectName == "DLSS\\DLSS_OpticalFlow";
		return CreateBackend<DLSSSRUpscaler>(
			effectName, resources, input, output,
			DLSSSRSettings{
				.enableJitter = isJitter,
				.useMotionVectors = !isJitter && (isLegacyOpticalFlow ||
					getParameter("useMotionVectors", 1.0f) >= 0.5f),
				.useEstimatedDepth = !isJitter && !isLegacyOpticalFlow &&
					getParameter("useEstimatedDepth", 0.0f) >= 0.5f
			});
	}

	if (effectName == "FSR2\\FSR2_ZeroMV" ||
		effectName == "FSR2\\FSR2_ZeroMV_Jitter" ||
		effectName == "FSR2\\FSR2_OpticalFlow") {
		return CreateBackend<FSR2ZeroMVUpscaler>(effectName, resources, input, output,
			effectName == "FSR2\\FSR2_OpticalFlow",
			effectName == "FSR2\\FSR2_ZeroMV_Jitter");
	}

	const bool isFsr3 = effectName == "FSR3\\FSR3_ZeroMV" ||
		effectName == "FSR3\\FSR3_ZeroMV_Jitter" ||
		effectName == "FSR3\\FSR3_OpticalFlow";
	const bool isFsr4 = effectName == "FSR4\\FSR4_ZeroMV" ||
		effectName == "FSR4\\FSR4_ZeroMV_Jitter" ||
		effectName == "FSR4\\FSR4_OpticalFlow";
	if (isFsr3 || isFsr4) {
		return CreateBackend<FSR3ZeroMVUpscaler>(effectName, resources, input, output,
			effectName.ends_with("OpticalFlow"),
			effectName.ends_with("ZeroMV_Jitter"), isFsr4);
	}

	if (effectName == "XeSS\\XeSS_ZeroMV" ||
		effectName == "XeSS\\XeSS_ZeroMV_Jitter" ||
		effectName == "XeSS\\XeSS_OpticalFlow") {
		return CreateBackend<XeSSZeroMVUpscaler>(effectName, resources, input, output,
			effectName == "XeSS\\XeSS_OpticalFlow",
			effectName == "XeSS\\XeSS_ZeroMV_Jitter");
	}

	const bool isRtxVideo = effectName.starts_with("RTXVideo\\RTXVideo_Denoise_") ||
		effectName.starts_with("RTXVideo\\RTXVideo_VSR_");
	if (isRtxVideo) {
		const bool isVsr = effectName.find("_VSR_") != std::string_view::npos;
		uint32_t qualityLevel = 8;
		if (isVsr) {
			qualityLevel = effectName.ends_with("_Low") ? 1 :
				effectName.ends_with("_Medium") ? 2 :
				effectName.ends_with("_High") ? 3 : 4;
		} else if (effectName.ends_with("_Medium")) {
			qualityLevel = 9;
		} else if (effectName.ends_with("_High")) {
			qualityLevel = 10;
		} else if (effectName.ends_with("_Ultra")) {
			qualityLevel = 11;
		}
		return CreateBackend<RTXVideoDenoiser>(
			effectName, resources, input, output, qualityLevel);
	}

	return {};
}

}
