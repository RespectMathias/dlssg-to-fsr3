#include "ngx_parameters.h"

namespace test_support
{
	void NgxParameterBag::SetVoidPointer(const char *name, void *value)
	{
		SetPointer(name, value);
	}

	void NgxParameterBag::Set2(const char *name, float value)
	{
		if (name != nullptr)
		{
			values_.insert_or_assign(name, value);
		}
	}

	void NgxParameterBag::Set3(const char *name, void *value)
	{
		SetPointer(name, value);
	}

	void NgxParameterBag::Set4(const char *name, std::uint32_t value)
	{
		if (name != nullptr)
		{
			values_.insert_or_assign(name, value);
		}
	}

	void NgxParameterBag::Set5(const char *name, std::uint32_t value)
	{
		Set4(name, value);
	}

	void NgxParameterBag::Set6(const char *name, void *value)
	{
		SetPointer(name, value);
	}

	void NgxParameterBag::Set7(const char *name, ID3D12Resource *value)
	{
		SetPointer(name, value);
	}

	void NgxParameterBag::Set8(const char *name, void *value)
	{
		SetPointer(name, value);
	}

	NgxResult NgxParameterBag::GetVoidPointer(const char *name, void **value)
	{
		return GetPointer(name, value);
	}

	NgxResult NgxParameterBag::Get2(const char *name, float *value)
	{
		return GetFloat(name, value);
	}

	NgxResult NgxParameterBag::Get3(const char *name, void *value)
	{
		return GetPointer(name, value);
	}

	NgxResult NgxParameterBag::Get4(const char *name, std::uint32_t *value)
	{
		return GetUInt(name, value);
	}

	NgxResult NgxParameterBag::Get5(const char *name, std::uint32_t *value)
	{
		return GetUInt(name, value);
	}

	NgxResult NgxParameterBag::Get6(const char *name, void *value)
	{
		return GetPointer(name, value);
	}

	NgxResult NgxParameterBag::Get7(const char *name, float *value)
	{
		return GetFloat(name, value);
	}

	NgxResult NgxParameterBag::Get8(const char *name, void *value)
	{
		return GetPointer(name, value);
	}

	void NgxParameterBag::Unknown()
	{
		Clear();
	}

	void NgxParameterBag::Clear() noexcept
	{
		values_.clear();
	}

	bool NgxParameterBag::Contains(const std::string& name) const
	{
		return values_.contains(name);
	}

	std::size_t NgxParameterBag::Size() const noexcept
	{
		return values_.size();
	}

	void NgxParameterBag::SetPointer(const char *name, void *value)
	{
		if (name != nullptr)
		{
			values_.insert_or_assign(name, value);
		}
	}

	NgxResult NgxParameterBag::GetPointer(const char *name, void *value) const
	{
		if (name == nullptr || value == nullptr)
		{
			return ngx_invalid_parameter;
		}
		const auto found = values_.find(name);
		if (found == values_.end() || !std::holds_alternative<void *>(found->second))
		{
			return ngx_invalid_parameter;
		}
		*static_cast<void **>(value) = std::get<void *>(found->second);
		return ngx_success;
	}

	NgxResult NgxParameterBag::GetFloat(const char *name, float *value) const
	{
		if (name == nullptr || value == nullptr)
		{
			return ngx_invalid_parameter;
		}
		const auto found = values_.find(name);
		if (found == values_.end() || !std::holds_alternative<float>(found->second))
		{
			return ngx_invalid_parameter;
		}
		*value = std::get<float>(found->second);
		return ngx_success;
	}

	NgxResult NgxParameterBag::GetUInt(const char *name, std::uint32_t *value) const
	{
		if (name == nullptr || value == nullptr)
		{
			return ngx_invalid_parameter;
		}
		const auto found = values_.find(name);
		if (found == values_.end() || !std::holds_alternative<std::uint32_t>(found->second))
		{
			return ngx_invalid_parameter;
		}
		*value = std::get<std::uint32_t>(found->second);
		return ngx_success;
	}
}
