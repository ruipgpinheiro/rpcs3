#include <string>
#include "Log.h"

namespace utils
{
	class dynamic_library
	{
		void *m_handle = nullptr;

	public:
		dynamic_library() = default;
		dynamic_library(const std::string &path);

		~dynamic_library();

		bool load(const std::string &path);
		void close();

	private:
		void *get_impl(const std::string &name) const;

	public:
		template<typename Type = void>
		Type *get(const std::string &name) const
		{
			Type *result;
			*(void **)(&result) = get_impl(name);
			return result;
		}

		template<typename Type>
		bool get(Type *&function, const std::string &name) const
		{
			*(void **)(&function) = get_impl(name);

			return !!function;
		}

		bool loaded() const;
		explicit operator bool() const;
	};

	// (assume the lib is always loaded)
	void* get_proc_address(const char* lib, const char* name);

	template <typename F>
	struct dynamic_import
	{
		static_assert(sizeof(F) == 0, "Invalid function type");
	};

	template <typename R, typename... Args>
	struct dynamic_import<R(Args...)>
	{
		R(*ptr)(Args...);

		const char* const lib;
		const char* const name;

	private:
		bool initialized = false;

	public:
		// Constant initialization
		constexpr dynamic_import(const char* lib, const char* name)
			: ptr(nullptr)
			, lib(lib)
			, name(name)
		{
		}

		void init()
		{
			if (!initialized)
			{
				// TODO: atomic
				ptr = reinterpret_cast<R(*)(Args...)>(get_proc_address(lib, name));

				if (!ptr)
				{
					LOG_WARNING(GENERAL, "Could not find dynamic import '%s' in '%s'.", name, lib);
				}

				initialized = true;
			}
		}

		operator bool()
		{
			init();

			return ptr;
		}

		// Caller
		R operator()(Args... args)
		{
			init();

			return ptr(args...);
		}
	};
}

#define DYNAMIC_IMPORT(lib, name, ...) inline utils::dynamic_import<__VA_ARGS__> name(lib, #name);
