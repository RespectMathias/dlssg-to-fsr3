#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "../e2e/dx12/dx12_ngx_backend.h"
#include "../support/wic_image_writer.h"
#include "../e2e/vulkan/vulkan_ngx_e2e_backend.h"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace
{
	using Clock = std::chrono::steady_clock;

	enum class BackendKind
	{
		Dx12,
		Vulkan,
	};

	enum class FgMode
	{
		Off,
		On,
		Compare,
	};

	struct Options
	{
		BackendKind backend = BackendKind::Vulkan;
		FgMode fg = FgMode::On;
		bool visual = true;
		std::uint32_t frames = 300;
		std::uint32_t width = 640;
		std::uint32_t height = 360;
		std::uint32_t renderFps = 30;
		vulkan_ngx_e2e::SubmitApi submit = vulkan_ngx_e2e::SubmitApi::Legacy;
		vulkan_ngx_e2e::CopyPath copy = vulkan_ngx_e2e::CopyPath::Shader;
		std::filesystem::path dll;
		std::filesystem::path report;
		bool help = false;
	};

	std::string_view BackendName(BackendKind backend)
	{
		return backend == BackendKind::Dx12 ? "dx12" : "vulkan";
	}

	std::string_view FgName(FgMode mode)
	{
		switch (mode)
		{
		case FgMode::Off:
			return "off";
		case FgMode::On:
			return "on";
		case FgMode::Compare:
			return "compare";
		}
		return "unknown";
	}

	std::string_view SubmitName(vulkan_ngx_e2e::SubmitApi submit)
	{
		return submit == vulkan_ngx_e2e::SubmitApi::Legacy ? "legacy" : "submit2";
	}

	std::string_view CopyName(vulkan_ngx_e2e::CopyPath copy)
	{
		return copy == vulkan_ngx_e2e::CopyPath::Shader ? "shader" : "transfer";
	}

	std::uint32_t ParseNumber(std::string_view value, std::string_view option)
	{
		std::uint32_t result = 0;
		const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
		if (error != std::errc {} || end != value.data() + value.size())
			throw std::invalid_argument(std::string(option) + " requires an unsigned integer");
		return result;
	}

	Options ParseOptions(int argc, char** argv)
	{
		Options options;
		for (int index = 1; index < argc; ++index)
		{
			const std::string_view argument = argv[index];
			if (argument == "--help" || argument == "-h")
			{
				options.help = true;
			}
			else if (argument == "--visual")
			{
				options.visual = true;
			}
			else if (argument == "--headless")
			{
				options.visual = false;
			}
			else if (argument.starts_with("--backend="))
			{
				const auto value = argument.substr(10);
				if (value == "dx12")
					options.backend = BackendKind::Dx12;
				else if (value == "vulkan")
					options.backend = BackendKind::Vulkan;
				else
					throw std::invalid_argument("--backend must be dx12 or vulkan");
			}
			else if (argument.starts_with("--fg="))
			{
				const auto value = argument.substr(5);
				if (value == "off")
					options.fg = FgMode::Off;
				else if (value == "on")
					options.fg = FgMode::On;
				else if (value == "compare")
					options.fg = FgMode::Compare;
				else
					throw std::invalid_argument("--fg must be on, off, or compare");
			}
			else if (argument.starts_with("--frames="))
			{
				options.frames = ParseNumber(argument.substr(9), "--frames");
			}
			else if (argument.starts_with("--width="))
			{
				options.width = ParseNumber(argument.substr(8), "--width");
			}
			else if (argument.starts_with("--height="))
			{
				options.height = ParseNumber(argument.substr(9), "--height");
			}
			else if (argument.starts_with("--render-fps="))
			{
				options.renderFps = ParseNumber(argument.substr(13), "--render-fps");
			}
			else if (argument.starts_with("--submit="))
			{
				const auto value = argument.substr(9);
				if (value == "legacy")
					options.submit = vulkan_ngx_e2e::SubmitApi::Legacy;
				else if (value == "submit2")
					options.submit = vulkan_ngx_e2e::SubmitApi::Submit2;
				else
					throw std::invalid_argument("--submit must be legacy or submit2");
			}
			else if (argument.starts_with("--copy="))
			{
				const auto value = argument.substr(7);
				if (value == "shader")
					options.copy = vulkan_ngx_e2e::CopyPath::Shader;
				else if (value == "transfer")
					options.copy = vulkan_ngx_e2e::CopyPath::Transfer;
				else
					throw std::invalid_argument("--copy must be shader or transfer");
			}
			else if (argument.starts_with("--dll="))
			{
				options.dll = std::filesystem::path(argument.substr(6));
			}
			else if (argument.starts_with("--report="))
			{
				options.report = std::filesystem::path(argument.substr(9));
			}
			else
			{
				throw std::invalid_argument("unknown option: " + std::string(argument));
			}
		}

		if (options.help)
			return options;
		if (options.frames == 0)
			throw std::invalid_argument("--frames must be positive");
		if (options.width <= 32 || options.height <= 32 || options.width > 16384 || options.height > 16384)
			throw std::invalid_argument("--width and --height must be in range 33..16384");
		if (!options.visual && options.fg == FgMode::Compare && options.report.empty())
			options.report = "testbed-report.json";
		return options;
	}

	void PrintUsage()
	{
		std::cout
			<< "Usage: dlssg-testbed [options]\n"
			<< "  --backend=dx12|vulkan\n"
			<< "  --fg=on|off|compare\n"
			<< "  --visual | --headless\n"
			<< "  --frames=N --width=N --height=N\n"
			<< "  --render-fps=N (visual mode, 0 disables pacing)\n"
			<< "  --submit=legacy|submit2 --copy=shader|transfer\n"
			<< "  --dll=path --report=path\n";
	}

	struct FrameImages
	{
		std::vector<std::uint8_t> real;
		std::vector<std::uint8_t> interpolated;
		bool generated = false;
	};

	bool MostlyChangedRgb(const std::vector<std::uint8_t>& output, const std::vector<std::uint8_t>& baseline)
	{
		if (output.size() != baseline.size() || output.size() % 4 != 0 || output.empty())
			return false;
		std::size_t changedChannels = 0;
		for (std::size_t offset = 0; offset < output.size(); offset += 4)
		{
			for (std::size_t channel = 0; channel < 3; ++channel)
				changedChannels += output[offset + channel] != baseline[offset + channel] ? 1U : 0U;
		}
		return changedChannels * 10 >= (output.size() / 4) * 27;
	}

	bool MostlyChangedRgb(const std::vector<std::uint8_t>& output, std::uint8_t baseline)
	{
		if (output.size() % 4 != 0 || output.empty())
			return false;
		std::size_t changedChannels = 0;
		for (std::size_t offset = 0; offset < output.size(); offset += 4)
		{
			for (std::size_t channel = 0; channel < 3; ++channel)
				changedChannels += output[offset + channel] != baseline ? 1U : 0U;
		}
		return changedChannels * 10 >= (output.size() / 4) * 27;
	}

	class Backend
	{
	public:
		virtual ~Backend() = default;
		virtual FrameImages Run(std::uint32_t frameIndex, bool fg, bool reset) = 0;
		virtual std::string_view Name() const = 0;
		virtual const std::vector<std::string>& ValidationErrors() const = 0;
		virtual void Shutdown() = 0;
	};

	class Dx12Backend final : public Backend
	{
	public:
		explicit Dx12Backend(const Options& options)
		{
			auto config = dx12_ngx_e2e::BackendConfig::FromEnvironment();
			config.width = options.width;
			config.height = options.height;
			if (!options.dll.empty())
				config.dll = options.dll.wstring();
			backend_ = dx12_ngx_e2e::Dx12NgxBackend::Create(config);
		}

		FrameImages Run(std::uint32_t frameIndex, bool fg, bool reset) override
		{
			auto output = backend_->RunFrame(frameIndex, fg, reset);
			const bool generated = MostlyChangedRgb(output.outputInterpolated, output.interpolationBaseline);
			return { std::move(output.outputReal), std::move(output.outputInterpolated), generated };
		}

		std::string_view Name() const override
		{
			return "DX12";
		}

		const std::vector<std::string>& ValidationErrors() const override
		{
			static const std::vector<std::string> none;
			return none;
		}

		void Shutdown() override
		{
			backend_.reset();
		}

	private:
		std::unique_ptr<dx12_ngx_e2e::Dx12NgxBackend> backend_;
	};

	vulkan_ngx_e2e::FrameInput ProceduralVulkanFrame(
		std::uint32_t width,
		std::uint32_t height,
		std::uint32_t frameIndex,
		bool fg,
		bool reset)
	{
		const std::size_t pixelCount = static_cast<std::size_t>(width) * height;
		vulkan_ngx_e2e::FrameInput input;
		input.colorRgba8.resize(pixelCount * 4);
		input.depth.resize(pixelCount);
		input.motion.resize(pixelCount);
		input.enableInterpolation = fg;
		input.reset = reset;
		const std::uint32_t shift = static_cast<std::uint32_t>((static_cast<std::uint64_t>(frameIndex) * 3) % width);
		const float motionX = -3.0F / static_cast<float>(width);
		const std::uint32_t objectShift = static_cast<std::uint32_t>((static_cast<std::uint64_t>(frameIndex) * 3) % width);
		const float centerX = static_cast<float>((width / 4 + width - objectShift) % width);
		const float centerY = static_cast<float>(height) * 0.52F;
		const float radius = static_cast<float>(std::min(width, height)) * 0.16F;
		const float radiusSquared = radius * radius;

		for (std::uint32_t y = 0; y < height; ++y)
		{
			for (std::uint32_t x = 0; x < width; ++x)
			{
				const std::size_t pixel = static_cast<std::size_t>(y) * width + x;
				const std::size_t color = pixel * 4;
				const std::uint32_t movedX = (x + shift) % width;
				const bool checker = ((movedX / 16) + (y / 16)) % 2 != 0;
				input.colorRgba8[color] = static_cast<std::uint8_t>((movedX * 211U / width + (checker ? 35U : 5U)) & 0xffU);
				input.colorRgba8[color + 1] = static_cast<std::uint8_t>((y * 223U / height + (checker ? 19U : 67U)) & 0xffU);
				input.colorRgba8[color + 2] = checker ? 210U : 42U;
				input.colorRgba8[color + 3] = 255U;
				input.depth[pixel] = 0.35F + 0.55F * static_cast<float>(y) / static_cast<float>(height - 1);
				input.motion[pixel] = { motionX, 0.0F };

				float dx = std::abs(static_cast<float>(x) - centerX);
				dx = std::min(dx, static_cast<float>(width) - dx);
				const float dy = static_cast<float>(y) - centerY;
				if (dx * dx + dy * dy <= radiusSquared)
				{
					input.colorRgba8[color] = 238U;
					input.colorRgba8[color + 1] = 72U;
					input.colorRgba8[color + 2] = 31U;
					input.depth[pixel] = 0.18F;
				}
			}
		}
		return input;
	}

	class VulkanBackend final : public Backend
	{
	public:
		explicit VulkanBackend(const Options& options) : width_(options.width), height_(options.height)
		{
			vulkan_ngx_e2e::Config config;
			config.width = options.width;
			config.height = options.height;
			config.submitApi = options.submit;
			config.copyPath = options.copy;
			if (!options.dll.empty())
				config.dll = options.dll.wstring();
			if (!backend_.Initialize(config))
				throw std::runtime_error(backend_.LastError());
		}

		FrameImages Run(std::uint32_t frameIndex, bool fg, bool reset) override
		{
			auto input = ProceduralVulkanFrame(width_, height_, frameIndex, fg, reset);
			vulkan_ngx_e2e::FrameOutput output;
			if (!backend_.RunFrame(input, &output))
				throw std::runtime_error(backend_.LastError());
			constexpr std::uint32_t success = 0x1;
			constexpr std::uint32_t invalidParameter = 0xBAD00005;
			if (output.ngxResult != success && !(reset && output.ngxResult == invalidParameter))
			{
				std::ostringstream message;
				message << "NVSDK_NGX_VULKAN_EvaluateFeature returned 0x" << std::hex << output.ngxResult;
				throw std::runtime_error(message.str());
			}
			const bool generated = MostlyChangedRgb(output.outputInterpolatedRgba8, 0x5a);
			return { std::move(output.outputRealRgba8), std::move(output.outputInterpolatedRgba8), generated };
		}

		std::string_view Name() const override
		{
			return "Vulkan";
		}

		const std::vector<std::string>& ValidationErrors() const override
		{
			return backend_.ValidationErrors();
		}

		void Shutdown() override
		{
			backend_.Shutdown();
		}

	private:
		std::uint32_t width_;
		std::uint32_t height_;
		vulkan_ngx_e2e::Backend backend_;
	};

	std::unique_ptr<Backend> CreateBackend(const Options& options)
	{
		if (options.backend == BackendKind::Dx12)
			return std::make_unique<Dx12Backend>(options);
		return std::make_unique<VulkanBackend>(options);
	}

	double Percentile(std::vector<double> values, double percentile)
	{
		if (values.empty())
			return 0.0;
		std::ranges::sort(values);
		const double position = static_cast<double>(values.size() - 1) * percentile;
		const auto lower = static_cast<std::size_t>(position);
		const auto upper = std::min(lower + 1, values.size() - 1);
		const double fraction = position - static_cast<double>(lower);
		return values[lower] + (values[upper] - values[lower]) * fraction;
	}

	struct Metrics
	{
		std::uint64_t realFrames = 0;
		std::uint64_t generatedFrames = 0;
		std::uint64_t presents = 0;
		std::uint64_t warmupFrames = 0;
		std::uint64_t droppedFrames = 0;
		std::size_t validationErrors = 0;
		double elapsedSeconds = 0.0;
		double realFps = 0.0;
		double generatedFps = 0.0;
		double presentFps = 0.0;
		double multiplier = 0.0;
		double p50Ms = 0.0;
		double p95Ms = 0.0;
	};

	class PhaseAccumulator
	{
	public:
		PhaseAccumulator(bool fg, std::size_t validationBase)
			: fg_(fg), validationBase_(validationBase), started_(Clock::now())
		{
		}

		void AddFrame(double milliseconds, bool warmup, bool generated)
		{
			++realFrames_;
			warmupFrames_ += warmup ? 1U : 0U;
			generatedFrames_ += generated ? 1U : 0U;
			if (fg_ && !warmup && !generated)
				++droppedFrames_;
			frameTimesMs_.push_back(milliseconds);
		}

		void AddPresents(std::uint64_t count)
		{
			presents_ += count;
		}

		Metrics Snapshot(std::size_t validationCount) const
		{
			Metrics result;
			result.realFrames = realFrames_;
			result.generatedFrames = generatedFrames_;
			result.presents = presents_;
			result.warmupFrames = warmupFrames_;
			result.droppedFrames = droppedFrames_;
			result.validationErrors = validationCount >= validationBase_ ? validationCount - validationBase_ : validationCount;
			result.elapsedSeconds = std::chrono::duration<double>(Clock::now() - started_).count();
			if (result.elapsedSeconds > 0.0)
			{
				result.realFps = static_cast<double>(realFrames_) / result.elapsedSeconds;
				result.generatedFps = static_cast<double>(generatedFrames_) / result.elapsedSeconds;
				result.presentFps = static_cast<double>(presents_) / result.elapsedSeconds;
			}
			result.multiplier = realFrames_ == 0 ? 0.0 : static_cast<double>(presents_) / static_cast<double>(realFrames_);
			result.p50Ms = Percentile(frameTimesMs_, 0.50);
			result.p95Ms = Percentile(frameTimesMs_, 0.95);
			return result;
		}

	private:
		bool fg_;
		std::size_t validationBase_;
		Clock::time_point started_;
		std::uint64_t realFrames_ = 0;
		std::uint64_t generatedFrames_ = 0;
		std::uint64_t presents_ = 0;
		std::uint64_t warmupFrames_ = 0;
		std::uint64_t droppedFrames_ = 0;
		std::vector<double> frameTimesMs_;
	};

	struct PhaseResult
	{
		std::string name;
		bool fg = false;
		Metrics metrics;
		FrameImages lastFrame;
	};

	struct Dashboard
	{
		std::string_view backend;
		bool fg = false;
		bool warmup = false;
		Metrics metrics;
		std::size_t validationErrors = 0;
	};

	class VisualPresenter
	{
	public:
		VisualPresenter(std::uint32_t imageWidth, std::uint32_t imageHeight)
			: imageWidth_(imageWidth), imageHeight_(imageHeight)
		{
			glfwSetErrorCallback([](int, const char* description)
			{
				std::cerr << "GLFW: " << description << '\n';
			});
			if (!glfwInit())
				throw std::runtime_error("glfwInit failed");
			glfwInitialized_ = true;
			glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
			glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
			glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
			window_ = glfwCreateWindow(
				static_cast<int>(std::max<std::uint32_t>(imageWidth, 800)),
				static_cast<int>(std::max<std::uint32_t>(imageHeight, 450)),
				"DLSSG to FSR3 testbed",
				nullptr,
				nullptr);
			if (!window_)
				throw std::runtime_error("glfwCreateWindow failed");
			glfwMakeContextCurrent(window_);
			glfwSwapInterval(1);
			IMGUI_CHECKVERSION();
			ImGui::CreateContext();
			imguiContext_ = true;
			ImGui::GetIO().IniFilename = nullptr;
			ImGui::StyleColorsDark();
			if (!ImGui_ImplGlfw_InitForOpenGL(window_, true) || !ImGui_ImplOpenGL3_Init("#version 330"))
				throw std::runtime_error("Dear ImGui OpenGL3 initialization failed");
			imguiBackends_ = true;
			ImGui::GetIO().IniFilename = nullptr;
			glGenTextures(1, &texture_);
			glBindTexture(GL_TEXTURE_2D, texture_);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			constexpr GLint clampToEdge = 0x812F;
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, clampToEdge);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, clampToEdge);
		}

		~VisualPresenter()
		{
			if (window_)
				glfwMakeContextCurrent(window_);
			if (texture_ != 0)
				glDeleteTextures(1, &texture_);
			if (imguiBackends_)
			{
				ImGui_ImplOpenGL3_Shutdown();
				ImGui_ImplGlfw_Shutdown();
			}
			if (imguiContext_)
				ImGui::DestroyContext();
			if (window_)
				glfwDestroyWindow(window_);
			if (glfwInitialized_)
				glfwTerminate();
		}

		VisualPresenter(const VisualPresenter&) = delete;
		VisualPresenter& operator=(const VisualPresenter&) = delete;

		void PollEvents()
		{
			glfwPollEvents();
		}

		bool ShouldClose() const
		{
			return glfwWindowShouldClose(window_) != 0;
		}

		bool ToggleKeyPressed()
		{
			const bool down = glfwGetKey(window_, GLFW_KEY_F) == GLFW_PRESS;
			const bool pressed = down && !fKeyDown_;
			fKeyDown_ = down;
			return pressed;
		}

		std::optional<bool> Present(
			const std::vector<std::uint8_t>& rgba,
			std::string_view imageName,
			const Dashboard& dashboard,
			bool interactive)
		{
			const std::size_t expected = static_cast<std::size_t>(imageWidth_) * imageHeight_ * 4;
			if (rgba.size() != expected)
				throw std::runtime_error("backend returned image with unexpected size");
			glBindTexture(GL_TEXTURE_2D, texture_);
			glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
			glTexImage2D(
				GL_TEXTURE_2D,
				0,
				GL_RGB,
				static_cast<GLsizei>(imageWidth_),
				static_cast<GLsizei>(imageHeight_),
				0,
				GL_RGBA,
				GL_UNSIGNED_BYTE,
				rgba.data());

			int framebufferWidth = 0;
			int framebufferHeight = 0;
			glfwGetFramebufferSize(window_, &framebufferWidth, &framebufferHeight);
			glViewport(0, 0, framebufferWidth, framebufferHeight);
			glClearColor(0.015F, 0.018F, 0.024F, 1.0F);
			glClear(GL_COLOR_BUFFER_BIT);
			ImGui_ImplOpenGL3_NewFrame();
			ImGui_ImplGlfw_NewFrame();
			ImGui::NewFrame();

			const ImGuiViewport* viewport = ImGui::GetMainViewport();
			ImGui::SetNextWindowPos(viewport->Pos);
			ImGui::SetNextWindowSize(viewport->Size);
			constexpr ImGuiWindowFlags imageFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
				ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav |
				ImGuiWindowFlags_NoBackground;
			ImGui::Begin("Frame", nullptr, imageFlags);
			const ImVec2 available = ImGui::GetContentRegionAvail();
			const float scale = std::min(available.x / static_cast<float>(imageWidth_), available.y / static_cast<float>(imageHeight_));
			const ImVec2 size(static_cast<float>(imageWidth_) * scale, static_cast<float>(imageHeight_) * scale);
			ImGui::SetCursorPos(ImVec2((available.x - size.x) * 0.5F, (available.y - size.y) * 0.5F));
			ImGui::Image(reinterpret_cast<ImTextureID>(static_cast<std::intptr_t>(texture_)), size, ImVec2(0, 1), ImVec2(1, 0));
			ImGui::End();

			ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + 14.0F, viewport->Pos.y + 14.0F), ImGuiCond_Always);
			ImGui::SetNextWindowBgAlpha(0.88F);
			ImGui::Begin("DLSSG testbed", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings);
			ImGui::Text("Image: %.*s", static_cast<int>(imageName.size()), imageName.data());
			ImGui::Text("Backend: %.*s", static_cast<int>(dashboard.backend.size()), dashboard.backend.data());
			bool nextFg = dashboard.fg;
			std::optional<bool> changed;
			if (ImGui::Checkbox("Frame generation (F)", &nextFg) && interactive)
				changed = nextFg;
			ImGui::SameLine();
			ImGui::TextDisabled("[%s]", dashboard.fg ? (dashboard.warmup ? "warmup" : "ready") : "off");
			ImGui::Separator();
			ImGui::Text("Real FPS:      %8.2f", dashboard.metrics.realFps);
			ImGui::Text("Generated FPS: %8.2f", dashboard.metrics.generatedFps);
			ImGui::Text("Present FPS:   %8.2f", dashboard.metrics.presentFps);
			ImGui::Text("Multiplier:    %8.2fx", dashboard.metrics.multiplier);
			ImGui::Text("Frame p50/p95: %.3f / %.3f ms", dashboard.metrics.p50Ms, dashboard.metrics.p95Ms);
			ImGui::Text("Dropped frames: %llu", static_cast<unsigned long long>(dashboard.metrics.droppedFrames));
			ImGui::Text("Validation errors: %zu", dashboard.validationErrors);
			ImGui::End();

			ImGui::Render();
			ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
			glfwSwapBuffers(window_);
			return changed;
		}

	private:
		std::uint32_t imageWidth_;
		std::uint32_t imageHeight_;
		GLFWwindow* window_ = nullptr;
		GLuint texture_ = 0;
		bool glfwInitialized_ = false;
		bool imguiContext_ = false;
		bool imguiBackends_ = false;
		bool fKeyDown_ = false;
	};

	void PrintMetrics(const PhaseResult& phase)
	{
		const auto& metrics = phase.metrics;
		std::cout << std::fixed << std::setprecision(2)
			<< phase.name
			<< ": real=" << metrics.realFps << " fps"
			<< " generated=" << metrics.generatedFps << " fps"
			<< " present=" << metrics.presentFps << " fps"
			<< " multiplier=" << metrics.multiplier << 'x'
			<< " p50=" << metrics.p50Ms << " ms"
			<< " p95=" << metrics.p95Ms << " ms"
			<< " dropped=" << metrics.droppedFrames
			<< " validation_errors=" << metrics.validationErrors << '\n';
	}

	PhaseResult RunHeadlessPhase(
		Backend& backend,
		std::string name,
		bool fg,
		std::uint32_t frames,
		std::uint32_t& frameIndex)
	{
		PhaseAccumulator accumulator(fg, backend.ValidationErrors().size());
		bool reset = true;
		FrameImages lastFrame;
		for (std::uint32_t index = 0; index < frames; ++index, ++frameIndex)
		{
			const auto started = Clock::now();
			FrameImages images = backend.Run(frameIndex, fg, reset);
			const double milliseconds = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
			const bool generated = fg && !reset && images.generated;
			accumulator.AddFrame(milliseconds, fg && reset, generated);
			accumulator.AddPresents(generated ? 2 : 1);
			lastFrame = std::move(images);
			reset = false;
		}
		PhaseResult result { std::move(name), fg, accumulator.Snapshot(backend.ValidationErrors().size()), std::move(lastFrame) };
		PrintMetrics(result);
		return result;
	}

	std::vector<PhaseResult> RunHeadless(Backend& backend, const Options& options)
	{
		std::vector<PhaseResult> results;
		std::uint32_t frameIndex = 0;
		if (options.fg == FgMode::Off || options.fg == FgMode::Compare)
			results.push_back(RunHeadlessPhase(backend, "off", false, options.frames, frameIndex));
		if (options.fg == FgMode::On || options.fg == FgMode::Compare)
			results.push_back(RunHeadlessPhase(backend, "on", true, options.frames, frameIndex));
		return results;
	}

	std::vector<PhaseResult> RunVisual(Backend& backend, const Options& options)
	{
		VisualPresenter presenter(options.width, options.height);
		std::vector<PhaseResult> results;
		bool fg = options.fg == FgMode::On;
		bool reset = true;
		std::size_t phaseNumber = 0;
		auto phaseName = [&]
		{
			return std::string("visual-") + (fg ? "on-" : "off-") + std::to_string(phaseNumber++);
		};
		std::string currentName = phaseName();
		PhaseAccumulator accumulator(fg, backend.ValidationErrors().size());
		const std::uint64_t frameLimit = options.fg == FgMode::Compare
			? static_cast<std::uint64_t>(options.frames) * 2
			: options.frames;

		auto switchFg = [&](bool enabled)
		{
			if (enabled == fg)
				return;
			const Metrics finished = accumulator.Snapshot(backend.ValidationErrors().size());
			if (finished.realFrames != 0)
				results.push_back({ currentName, fg, finished, {} });
			fg = enabled;
			reset = enabled;
			currentName = phaseName();
			accumulator = PhaseAccumulator(fg, backend.ValidationErrors().size());
		};

		for (std::uint64_t index = 0; index < frameLimit && !presenter.ShouldClose(); ++index)
		{
			const auto cycleStarted = Clock::now();
			presenter.PollEvents();
			if (presenter.ToggleKeyPressed())
				switchFg(!fg);
			if (options.fg == FgMode::Compare && index == options.frames)
				switchFg(true);

			const bool warmup = fg && reset;
			const auto started = Clock::now();
			const FrameImages images = backend.Run(static_cast<std::uint32_t>(index), fg, reset);
			const double milliseconds = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
			const bool generated = fg && !reset && images.generated;
			accumulator.AddFrame(milliseconds, warmup, generated);
			std::optional<bool> checkboxChange;

			if (fg && generated)
			{
				const Dashboard dashboard {
					backend.Name(), fg, warmup, accumulator.Snapshot(backend.ValidationErrors().size()), backend.ValidationErrors().size()
				};
				const auto generatedChange = presenter.Present(images.interpolated, "scene", dashboard, true);
				const auto realChange = presenter.Present(images.real, "scene", dashboard, true);
				checkboxChange = realChange ? realChange : generatedChange;
				accumulator.AddPresents(2);
			}
			else
			{
				const Dashboard dashboard { backend.Name(), fg, false, accumulator.Snapshot(backend.ValidationErrors().size()), backend.ValidationErrors().size() };
				checkboxChange = presenter.Present(images.real, "real", dashboard, true);
				accumulator.AddPresents(1);
			}

			reset = false;
			if (checkboxChange)
				switchFg(*checkboxChange);
			if (options.renderFps != 0)
			{
				const auto frameDuration = std::chrono::duration<double>(1.0 / static_cast<double>(options.renderFps));
				std::this_thread::sleep_until(cycleStarted + std::chrono::duration_cast<Clock::duration>(frameDuration));
			}
		}

		const Metrics finished = accumulator.Snapshot(backend.ValidationErrors().size());
		if (finished.realFrames != 0)
			results.push_back({ currentName, fg, finished, {} });
		for (const auto& result : results)
			PrintMetrics(result);
		return results;
	}

	std::string JsonString(std::string_view value)
	{
		std::ostringstream output;
		output << '"';
		for (const unsigned char character : value)
		{
			switch (character)
			{
			case '"':
				output << "\\\"";
				break;
			case '\\':
				output << "\\\\";
				break;
			case '\b':
				output << "\\b";
				break;
			case '\f':
				output << "\\f";
				break;
			case '\n':
				output << "\\n";
				break;
			case '\r':
				output << "\\r";
				break;
			case '\t':
				output << "\\t";
				break;
			default:
				if (character < 0x20)
					output << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<unsigned int>(character) << std::dec;
				else
					output << character;
			}
		}
		output << '"';
		return output.str();
	}

	void WriteReport(
		const std::filesystem::path& path,
		const Options& options,
		const std::vector<PhaseResult>& phases,
		const std::vector<std::string>& validationErrors)
	{
		if (path.empty())
			return;
		std::ofstream output(path, std::ios::binary | std::ios::trunc);
		if (!output)
			throw std::runtime_error("could not open report: " + path.string());
		std::vector<std::pair<std::filesystem::path, std::filesystem::path>> artifacts;
		artifacts.reserve(phases.size());
		for (const auto& phase : phases)
		{
			const auto base = path.parent_path() / (path.stem().string() + "-" + phase.name);
			const auto realPath = std::filesystem::path(base.string() + "-real.png");
			const auto interpolatedPath = std::filesystem::path(base.string() + "-interpolated.png");
			if (!phase.lastFrame.real.empty())
				test_support::WriteImageArtifact(realPath, options.width, options.height, phase.lastFrame.real);
			if (phase.fg && phase.lastFrame.generated && !phase.lastFrame.interpolated.empty())
				test_support::WriteImageArtifact(interpolatedPath, options.width, options.height, phase.lastFrame.interpolated);
			artifacts.emplace_back(realPath, interpolatedPath);
		}
		output << std::fixed << std::setprecision(6)
			<< "{\n"
			<< "  \"backend\": " << JsonString(BackendName(options.backend)) << ",\n"
			<< "  \"fg\": " << JsonString(FgName(options.fg)) << ",\n"
			<< "  \"mode\": " << JsonString(options.visual ? "visual" : "headless") << ",\n"
			<< "  \"frames_per_phase\": " << options.frames << ",\n"
			<< "  \"width\": " << options.width << ",\n"
			<< "  \"height\": " << options.height << ",\n"
			<< "  \"submit\": " << JsonString(SubmitName(options.submit)) << ",\n"
			<< "  \"copy\": " << JsonString(CopyName(options.copy)) << ",\n"
			<< "  \"phases\": [\n";
		for (std::size_t index = 0; index < phases.size(); ++index)
		{
			const auto& phase = phases[index];
			const auto& metrics = phase.metrics;
			output
				<< "    {\n"
				<< "      \"name\": " << JsonString(phase.name) << ",\n"
				<< "      \"fg_enabled\": " << (phase.fg ? "true" : "false") << ",\n"
				<< "      \"real_frames\": " << metrics.realFrames << ",\n"
				<< "      \"generated_frames\": " << metrics.generatedFrames << ",\n"
				<< "      \"logical_presents\": " << metrics.presents << ",\n"
				<< "      \"warmup_frames\": " << metrics.warmupFrames << ",\n"
				<< "      \"dropped_frames\": " << metrics.droppedFrames << ",\n"
				<< "      \"validation_errors\": " << metrics.validationErrors << ",\n"
				<< "      \"elapsed_seconds\": " << metrics.elapsedSeconds << ",\n"
				<< "      \"real_fps\": " << metrics.realFps << ",\n"
				<< "      \"generated_fps\": " << metrics.generatedFps << ",\n"
				<< "      \"present_fps\": " << metrics.presentFps << ",\n"
				<< "      \"multiplier\": " << metrics.multiplier << ",\n"
				<< "      \"frame_p50_ms\": " << metrics.p50Ms << ",\n"
				<< "      \"frame_p95_ms\": " << metrics.p95Ms << ",\n"
				<< "      \"real_artifact\": "
				<< (phase.lastFrame.real.empty() ? "null" : JsonString(artifacts[index].first.string())) << ",\n"
				<< "      \"interpolated_artifact\": "
				<< (phase.fg && phase.lastFrame.generated ? JsonString(artifacts[index].second.string()) : "null") << "\n"
				<< "    }" << (index + 1 == phases.size() ? "\n" : ",\n");
		}
		output << "  ],\n  \"validation_messages\": [\n";
		for (std::size_t index = 0; index < validationErrors.size(); ++index)
			output << "    " << JsonString(validationErrors[index]) << (index + 1 == validationErrors.size() ? "\n" : ",\n");
		output << "  ]\n}\n";
		if (!output)
			throw std::runtime_error("failed while writing report: " + path.string());
		std::cout << "report=" << path.string() << '\n';
	}

	bool HeadlessPassed(const std::vector<PhaseResult>& phases, const std::vector<std::string>& validationErrors)
	{
		if (!validationErrors.empty())
			return false;
		return std::ranges::all_of(phases, [](const PhaseResult& phase)
		{
			const auto& metrics = phase.metrics;
			if (metrics.droppedFrames != 0 || metrics.validationErrors != 0)
				return false;
			if (!phase.fg)
				return metrics.generatedFrames == 0 && metrics.presents == metrics.realFrames;
			const auto expectedGenerated = metrics.realFrames - std::min(metrics.realFrames, metrics.warmupFrames);
			return metrics.generatedFrames == expectedGenerated && metrics.presents == metrics.realFrames + metrics.generatedFrames;
		});
	}
}

int main(int argc, char** argv)
{
	try
	{
		const Options options = ParseOptions(argc, argv);
		if (options.help)
		{
			PrintUsage();
			return 0;
		}
		auto backend = CreateBackend(options);
		const auto phases = options.visual ? RunVisual(*backend, options) : RunHeadless(*backend, options);
		backend->Shutdown();
		WriteReport(options.report, options, phases, backend->ValidationErrors());
		for (const auto& error : backend->ValidationErrors())
			std::cerr << "validation: " << error << '\n';
		return (!options.visual && !HeadlessPassed(phases, backend->ValidationErrors())) ? 2 :
			(backend->ValidationErrors().empty() ? 0 : 2);
	}
	catch (const std::exception& error)
	{
		std::cerr << "error: " << error.what() << '\n';
		return 1;
	}
}
