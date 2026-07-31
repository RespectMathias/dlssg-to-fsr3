#pragma once

#include "ngx_abi.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <variant>

namespace test_support
{
	class NgxParameterBag final : public NgxInstanceParameters
	{
	public:
		void SetVoidPointer(const char *name, void *value) override;
		void Set2(const char *name, float value) override;
		void Set3(const char *name, void *value) override;
		void Set4(const char *name, std::uint32_t value) override;
		void Set5(const char *name, std::uint32_t value) override;
		void Set6(const char *name, void *value) override;
		void Set7(const char *name, ID3D12Resource *value) override;
		void Set8(const char *name, void *value) override;
		NgxResult GetVoidPointer(const char *name, void **value) override;
		NgxResult Get2(const char *name, float *value) override;
		NgxResult Get3(const char *name, void *value) override;
		NgxResult Get4(const char *name, std::uint32_t *value) override;
		NgxResult Get5(const char *name, std::uint32_t *value) override;
		NgxResult Get6(const char *name, void *value) override;
		NgxResult Get7(const char *name, float *value) override;
		NgxResult Get8(const char *name, void *value) override;
		void Unknown() override;

		void Clear() noexcept;
		[[nodiscard]] bool Contains(const std::string& name) const;
		[[nodiscard]] std::size_t Size() const noexcept;

	private:
		using Value = std::variant<void *, float, std::uint32_t>;
		std::unordered_map<std::string, Value> values_;

		void SetPointer(const char *name, void *value);
		NgxResult GetPointer(const char *name, void *value) const;
		NgxResult GetFloat(const char *name, float *value) const;
		NgxResult GetUInt(const char *name, std::uint32_t *value) const;
	};
}
