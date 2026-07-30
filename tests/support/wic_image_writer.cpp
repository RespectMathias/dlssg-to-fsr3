#include "wic_image_writer.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <objbase.h>
#include <wincodec.h>

#include <algorithm>
#include <cstddef>
#include <cwctype>
#include <limits>
#include <stdexcept>
#include <string>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "windowscodecs.lib")

namespace test_support
{
	namespace
	{
		template<typename T>
		class ComPtr
		{
		public:
			~ComPtr()
			{
				if (value_ != nullptr)
				{
					value_->Release();
				}
			}

			ComPtr() = default;
			ComPtr(const ComPtr&) = delete;
			ComPtr& operator=(const ComPtr&) = delete;
			[[nodiscard]] T *Get() const noexcept
			{
				return value_;
			}
			[[nodiscard]] T **Put() noexcept
			{
				return &value_;
			}

		private:
			T *value_ = nullptr;
		};

		class ComApartment
		{
		public:
			ComApartment()
			{
				const HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
				if (SUCCEEDED(result))
				{
					uninitialize_ = true;
				}
				else if (result != RPC_E_CHANGED_MODE)
				{
					throw std::runtime_error("CoInitializeEx failed, HRESULT " + std::to_string(static_cast<unsigned long>(result)));
				}
			}

			~ComApartment()
			{
				if (uninitialize_)
				{
					CoUninitialize();
				}
			}

			ComApartment(const ComApartment&) = delete;
			ComApartment& operator=(const ComApartment&) = delete;

		private:
			bool uninitialize_ = false;
		};

		void Check(HRESULT result, const char *operation)
		{
			if (FAILED(result))
			{
				throw std::runtime_error(std::string(operation) + " failed, HRESULT " +
										 std::to_string(static_cast<unsigned long>(result)));
			}
		}

		ImageArtifactFormat ResolveFormat(const std::filesystem::path& path, ImageArtifactFormat format)
		{
			if (format != ImageArtifactFormat::auto_detect)
			{
				return format;
			}
			std::wstring extension = path.extension().wstring();
			std::transform(extension.begin(), extension.end(), extension.begin(), [](wchar_t character)
			{
				return static_cast<wchar_t>(std::towlower(character));
			});
			if (extension == L".png")
			{
				return ImageArtifactFormat::png;
			}
			if (extension == L".bmp")
			{
				return ImageArtifactFormat::bmp;
			}
			throw std::invalid_argument("Image artifact extension must be .png or .bmp");
		}
	}

	void WriteImageArtifact(const std::filesystem::path& path,
							std::uint32_t width,
							std::uint32_t height,
							std::span<const std::uint8_t> rgba8,
							ImageArtifactFormat format)
	{
		const std::size_t byte_count = static_cast<std::size_t>(width) * height * 4U;
		if (width == 0U || height == 0U || rgba8.size() != byte_count)
		{
			throw std::invalid_argument("WIC writer requires a non-empty, tightly packed RGBA8 image");
		}
		if (static_cast<std::uint64_t>(width) * 4U > std::numeric_limits<UINT>::max() || byte_count > std::numeric_limits<UINT>::max())
		{
			throw std::overflow_error("RGBA8 image exceeds WIC buffer limits");
		}

		format = ResolveFormat(path, format);
		const GUID& container = format == ImageArtifactFormat::png ? GUID_ContainerFormatPng : GUID_ContainerFormatBmp;
		const UINT stride = width * 4U;
		const UINT buffer_size = static_cast<UINT>(byte_count);
		ComApartment apartment;
		ComPtr<IWICImagingFactory> factory;
		Check(CoCreateInstance(CLSID_WICImagingFactory,
							 nullptr,
							 CLSCTX_INPROC_SERVER,
							 IID_PPV_ARGS(factory.Put())),
			  "Create WIC factory");

		ComPtr<IWICStream> stream;
		Check(factory.Get()->CreateStream(stream.Put()), "Create WIC stream");
		Check(stream.Get()->InitializeFromFilename(path.c_str(), GENERIC_WRITE), "Open image artifact");
		ComPtr<IWICBitmapEncoder> encoder;
		Check(factory.Get()->CreateEncoder(container, nullptr, encoder.Put()), "Create WIC encoder");
		Check(encoder.Get()->Initialize(stream.Get(), WICBitmapEncoderNoCache), "Initialize WIC encoder");

		ComPtr<IWICBitmapFrameEncode> frame;
		Check(encoder.Get()->CreateNewFrame(frame.Put(), nullptr), "Create WIC frame");
		Check(frame.Get()->Initialize(nullptr), "Initialize WIC frame");
		Check(frame.Get()->SetSize(width, height), "Set WIC frame size");
		WICPixelFormatGUID pixel_format = GUID_WICPixelFormat32bppRGBA;
		Check(frame.Get()->SetPixelFormat(&pixel_format), "Set WIC pixel format");

		if (IsEqualGUID(pixel_format, GUID_WICPixelFormat32bppRGBA))
		{
			Check(frame.Get()->WritePixels(height, stride, buffer_size, const_cast<BYTE *>(rgba8.data())), "Write WIC pixels");
		}
		else
		{
			ComPtr<IWICBitmap> bitmap;
			Check(factory.Get()->CreateBitmapFromMemory(width,
											  height,
											  GUID_WICPixelFormat32bppRGBA,
											  stride,
											  buffer_size,
											  const_cast<BYTE *>(rgba8.data()),
											  bitmap.Put()),
				  "Create WIC bitmap");
			ComPtr<IWICFormatConverter> converter;
			Check(factory.Get()->CreateFormatConverter(converter.Put()), "Create WIC converter");
			Check(converter.Get()->Initialize(bitmap.Get(), pixel_format, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom),
				  "Convert WIC pixels");
			Check(frame.Get()->WriteSource(converter.Get(), nullptr), "Write converted WIC pixels");
		}

		Check(frame.Get()->Commit(), "Commit WIC frame");
		Check(encoder.Get()->Commit(), "Commit WIC encoder");
	}
}
