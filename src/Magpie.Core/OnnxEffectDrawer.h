#pragma once

namespace Magpie {

class DeviceResources;
class EffectsProfiler;
class InferenceBackendBase;
class BackendDescriptorStore;

class OnnxEffectDrawer {
public:
	OnnxEffectDrawer();
	OnnxEffectDrawer(const OnnxEffectDrawer&) = delete;
	OnnxEffectDrawer(OnnxEffectDrawer&&) = default;

	~OnnxEffectDrawer();

	bool Initialize(
		DeviceResources& deviceResources,
		BackendDescriptorStore& descriptorStore,
		ID3D11Texture2D** inOutTexture
	) noexcept;

	void Draw(EffectsProfiler& profiler) const noexcept;

	// 由应用层安装，用于把状态显示给用户（托盘气球）
	// Installed by the app layer to surface status to the user (tray balloon).
	// Called from the scaling thread - the implementation must marshal.
	static inline std::function<void(std::wstring, std::wstring)> StatusCallback;

	static void ReportStatus(std::wstring title, std::wstring text) noexcept {
		if (StatusCallback) {
			StatusCallback(std::move(title), std::move(text));
		}
	}

private:
	std::unique_ptr<InferenceBackendBase> _inferenceBackend;

	// 可选的预降采样 / optional pre-downscale before inference
	winrt::com_ptr<ID3D11Texture2D> _downscaledTex;
	winrt::com_ptr<ID3D11ComputeShader> _downscaleShader;
	ID3D11UnorderedAccessView* _downscaledUav = nullptr;
	ID3D11ShaderResourceView* _srcSrv = nullptr;
	ID3D11SamplerState* _sampler = nullptr;
	ID3D11DeviceContext4* _d3dDC = nullptr;
	std::pair<uint32_t, uint32_t> _downscaleDispatch{};
};

}
