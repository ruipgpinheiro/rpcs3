#include "stdafx.h"
#include "rsx_utils.h"
#include "rsx_methods.h"
#include "RSXThread.h"
#include "Emu/RSX/GCM.h"
#include "Common/BufferUtils.h"
#include "Overlays/overlays.h"
#include "Utilities/sysinfo.h"

extern "C"
{
#include "libswscale/swscale.h"
}

namespace rsx
{
#ifdef TEXTURE_CACHE_DEBUG
	address_range::page_info_array address_range::page_info = {};
#endif

	void convert_scale_image(u8 *dst, AVPixelFormat dst_format, int dst_width, int dst_height, int dst_pitch,
		const u8 *src, AVPixelFormat src_format, int src_width, int src_height, int src_pitch, int src_slice_h, bool bilinear)
	{
		std::unique_ptr<SwsContext, void(*)(SwsContext*)> sws(sws_getContext(src_width, src_height, src_format,
			dst_width, dst_height, dst_format, bilinear ? SWS_FAST_BILINEAR : SWS_POINT, NULL, NULL, NULL), sws_freeContext);

		sws_scale(sws.get(), &src, &src_pitch, 0, src_slice_h, &dst, &dst_pitch);
	}

	void convert_scale_image(std::unique_ptr<u8[]>& dst, AVPixelFormat dst_format, int dst_width, int dst_height, int dst_pitch,
		const u8 *src, AVPixelFormat src_format, int src_width, int src_height, int src_pitch, int src_slice_h, bool bilinear)
	{
		dst.reset(new u8[dst_pitch * dst_height]);
		convert_scale_image(dst.get(), dst_format, dst_width, dst_height, dst_pitch,
			src, src_format, src_width, src_height, src_pitch, src_slice_h, bilinear);
	}

	void clip_image(u8 *dst, const u8 *src, int clip_x, int clip_y, int clip_w, int clip_h, int bpp, int src_pitch, int dst_pitch)
	{
		u8 *pixels_src = (u8*)src + clip_y * src_pitch + clip_x * bpp;
		u8 *pixels_dst = dst;
		const u32 row_length = clip_w * bpp;

		for (int y = 0; y < clip_h; ++y)
		{
			std::memmove(pixels_dst, pixels_src, row_length);
			pixels_src += src_pitch;
			pixels_dst += dst_pitch;
		}
	}

	void clip_image(std::unique_ptr<u8[]>& dst, const u8 *src,
		int clip_x, int clip_y, int clip_w, int clip_h, int bpp, int src_pitch, int dst_pitch)
	{
		dst.reset(new u8[clip_h * dst_pitch]);
		clip_image(dst.get(), src, clip_x, clip_y, clip_w, clip_h, bpp, src_pitch, dst_pitch);
	}

	//Convert decoded integer values for CONSTANT_BLEND_FACTOR into f32 array in 0-1 range
	std::array<float, 4> get_constant_blend_colors()
	{
		//TODO: check another color formats (probably all integer formats with > 8-bits wide channels)
		if (rsx::method_registers.surface_color() == rsx::surface_color_format::w16z16y16x16)
		{
			u16 blend_color_r = rsx::method_registers.blend_color_16b_r();
			u16 blend_color_g = rsx::method_registers.blend_color_16b_g();
			u16 blend_color_b = rsx::method_registers.blend_color_16b_b();
			u16 blend_color_a = rsx::method_registers.blend_color_16b_a();

			return { blend_color_r / 65535.f, blend_color_g / 65535.f, blend_color_b / 65535.f, blend_color_a / 65535.f };
		}
		else
		{
			u8 blend_color_r = rsx::method_registers.blend_color_8b_r();
			u8 blend_color_g = rsx::method_registers.blend_color_8b_g();
			u8 blend_color_b = rsx::method_registers.blend_color_8b_b();
			u8 blend_color_a = rsx::method_registers.blend_color_8b_a();

			return { blend_color_r / 255.f, blend_color_g / 255.f, blend_color_b / 255.f, blend_color_a / 255.f };
		}
	}

	weak_ptr get_super_ptr(address_range &range)
	{
		return get_super_ptr(range.start, range.length());
	}

	weak_ptr get_super_ptr(u32 addr, u32 len)
	{
		verify(HERE), g_current_renderer;

		if (!g_current_renderer->local_super_memory_block.first)
		{
			auto block = vm::get(vm::any, 0xC0000000);
			if (block)
			{
				g_current_renderer->local_super_memory_block.first = block->used();
				g_current_renderer->local_super_memory_block.second = vm::get_super_ptr<u8>(0xC0000000, g_current_renderer->local_super_memory_block.first - 1);

				if (!g_current_renderer->local_super_memory_block.second)
				{
					//Disjoint allocation?
					LOG_ERROR(RSX, "Could not initialize contiguous RSX super-memory");
				}
			}
			else
			{
				fmt::throw_exception("RSX memory not mapped!");
			}
		}

		if (g_current_renderer->local_super_memory_block.second)
		{
			if (addr >= 0xC0000000 && (addr + len) <= (0xC0000000 + g_current_renderer->local_super_memory_block.first))
			{
				//RSX local
				return { g_current_renderer->local_super_memory_block.second.get() + (addr - 0xC0000000) };
			}
		}

		const auto cached = g_current_renderer->main_super_memory_block.find(addr);
		if (cached != g_current_renderer->main_super_memory_block.end())
		{
			const auto& _ptr = cached->second;
			if (_ptr.size() >= len)
			{
				return _ptr;
			}
		}

		if (auto result = vm::get_super_ptr<u8>(addr, len - 1))
		{
			weak_ptr _ptr = { result, len };
			auto &ret = g_current_renderer->main_super_memory_block[addr] = std::move(_ptr);
			return _ptr;
		}

		//Probably allocated as split blocks. Try to grab separate chunks
		std::vector<weak_ptr::memory_block_t> blocks;
		const u32 limit = addr + len;
		u32 next = addr;
		u32 remaining = len;

		while (true)
		{
			auto region = vm::get(vm::any, next)->get(next, 1);
			if (!region.second)
			{
				break;
			}

			const u32 block_offset = next - region.first;
			const u32 block_length = std::min(remaining, region.second->size() - block_offset);
			std::shared_ptr<u8> _ptr = { region.second, region.second->get(block_offset, block_length) };
			blocks.push_back({_ptr, block_length});

			remaining -= block_length;
			next = region.first + region.second->size();
			if (next >= limit)
			{
				weak_ptr _ptr = { blocks };
				auto &ret = g_current_renderer->main_super_memory_block[addr] = std::move(_ptr);
				return ret;
			}
		}

		LOG_ERROR(RSX, "Could not get super_ptr for memory block 0x%x+0x%x", addr, len);
		return {};
	}

	/* Fast image scaling routines
	* Only uses fast nearest scaling and integral scaling factors
	* T - Dst type
	* U - Src type
	* N - Sample count
	*/
	template <typename T, typename U>
	void scale_image_fallback_impl(T* dst, const U* src, u16 src_width, u16 src_height, u16 dst_pitch, u16 src_pitch, u8 element_size, u8 samples_u, u8 samples_v)
	{
		u32 dst_offset = 0;
		u32 src_offset = 0;

		u32 padding = (dst_pitch - (src_pitch * samples_u)) / sizeof(T);

		for (u16 h = 0; h < src_height; ++h)
		{
			const auto row_start = dst_offset;
			for (u16 w = 0; w < src_width; ++w)
			{
				for (u8 n = 0; n < samples_u; ++n)
				{
					dst[dst_offset++] = src[src_offset];
				}

				src_offset++;
			}

			dst_offset += padding;

			for (int n = 1; n < samples_v; ++n)
			{
				memcpy(&dst[dst_offset], &dst[row_start], dst_pitch);
				dst_offset += dst_pitch;
			}
		}
	}

	void scale_image_fallback(void* dst, const void* src, u16 src_width, u16 src_height, u16 dst_pitch, u16 src_pitch, u8 element_size, u8 samples_u, u8 samples_v)
	{
		switch (element_size)
		{
		case 1:
			scale_image_fallback_impl<u8, u8>((u8*)dst, (const u8*)src, src_width, src_height, dst_pitch, src_pitch, element_size, samples_u, samples_v);
			break;
		case 2:
			scale_image_fallback_impl<u16, u16>((u16*)dst, (const u16*)src, src_width, src_height, dst_pitch, src_pitch, element_size, samples_u, samples_v);
			break;
		case 4:
			scale_image_fallback_impl<u32, u32>((u32*)dst, (const u32*)src, src_width, src_height, dst_pitch, src_pitch, element_size, samples_u, samples_v);
			break;
		default:
			fmt::throw_exception("unsupported element size %d" HERE, element_size);
		}
	}

	void scale_image_fallback_with_byte_swap(void* dst, const void* src, u16 src_width, u16 src_height, u16 dst_pitch, u16 src_pitch, u8 element_size, u8 samples_u, u8 samples_v)
	{
		switch (element_size)
		{
		case 1:
			scale_image_fallback_impl<u8, u8>((u8*)dst, (const u8*)src, src_width, src_height, dst_pitch, src_pitch, element_size, samples_u, samples_v);
			break;
		case 2:
			scale_image_fallback_impl<u16, be_t<u16>>((u16*)dst, (const be_t<u16>*)src, src_width, src_height, dst_pitch, src_pitch, element_size, samples_u, samples_v);
			break;
		case 4:
			scale_image_fallback_impl<u32, be_t<u32>>((u32*)dst, (const be_t<u32>*)src, src_width, src_height, dst_pitch, src_pitch, element_size, samples_u, samples_v);
			break;
		default:
			fmt::throw_exception("unsupported element size %d" HERE, element_size);
		}
	}

	template <typename T, typename U, int N>
	void scale_image_impl(T* dst, const U* src, u16 src_width, u16 src_height, u16 padding)
	{
		u32 dst_offset = 0;
		u32 src_offset = 0;

		for (u16 h = 0; h < src_height; ++h)
		{
			for (u16 w = 0; w < src_width; ++w)
			{
				for (u8 n = 0; n < N; ++n)
				{
					dst[dst_offset++] = src[src_offset];
				}

				//Fetch next pixel
				src_offset++;
			}

			//Pad this row
			dst_offset += padding;
		}
	}

	template <int N>
	void scale_image_fast(void *dst, const void *src, u8 element_size, u16 src_width, u16 src_height, u16 padding)
	{
		switch (element_size)
		{
		case 1:
			scale_image_impl<u8, u8, N>((u8*)dst, (const u8*)src, src_width, src_height, padding);
			break;
		case 2:
			scale_image_impl<u16, u16, N>((u16*)dst, (const u16*)src, src_width, src_height, padding);
			break;
		case 4:
			scale_image_impl<u32, u32, N>((u32*)dst, (const u32*)src, src_width, src_height, padding);
			break;
		case 8:
			scale_image_impl<u64, u64, N>((u64*)dst, (const u64*)src, src_width, src_height, padding);
			break;
		default:
			fmt::throw_exception("unsupported pixel size %d" HERE, element_size);
		}
	}

	template <int N>
	void scale_image_fast_with_byte_swap(void *dst, const void *src, u8 element_size, u16 src_width, u16 src_height, u16 padding)
	{
		switch (element_size)
		{
		case 1:
			scale_image_impl<u8, u8, N>((u8*)dst, (const u8*)src, src_width, src_height, padding);
			break;
		case 2:
			scale_image_impl<u16, be_t<u16>, N>((u16*)dst, (const be_t<u16>*)src, src_width, src_height, padding);
			break;
		case 4:
			scale_image_impl<u32, be_t<u32>, N>((u32*)dst, (const be_t<u32>*)src, src_width, src_height, padding);
			break;
		case 8:
			scale_image_impl<u64, be_t<u64>, N>((u64*)dst, (const be_t<u64>*)src, src_width, src_height, padding);
			break;
		default:
			fmt::throw_exception("unsupported pixel size %d" HERE, element_size);
		}
	}

	void scale_image_nearest(void* dst, const void* src, u16 src_width, u16 src_height, u16 dst_pitch, u16 src_pitch, u8 element_size, u8 samples_u, u8 samples_v, bool swap_bytes)
	{
		//Scale this image by repeating pixel data n times
		//n = expected_pitch / real_pitch
		//Use of fixed argument templates for performance reasons

		const u16 dst_width = dst_pitch / element_size;
		const u16 padding = dst_width - (src_width * samples_u);

		if (!swap_bytes)
		{
			if (samples_v == 1)
			{
				switch (samples_u)
				{
				case 1:
					scale_image_fast<1>(dst, src, element_size, src_width, src_height, padding);
					break;
				case 2:
					scale_image_fast<2>(dst, src, element_size, src_width, src_height, padding);
					break;
				case 3:
					scale_image_fast<3>(dst, src, element_size, src_width, src_height, padding);
					break;
				case 4:
					scale_image_fast<4>(dst, src, element_size, src_width, src_height, padding);
					break;
				case 8:
					scale_image_fast<8>(dst, src, element_size, src_width, src_height, padding);
					break;
				case 16:
					scale_image_fast<16>(dst, src, element_size, src_width, src_height, padding);
					break;
				default:
					scale_image_fallback(dst, src, src_width, src_height, dst_pitch, src_pitch, element_size, samples_u, 1);
				}
			}
			else
			{
				scale_image_fallback(dst, src, src_width, src_height, dst_pitch, src_pitch, element_size, samples_u, samples_v);
			}
		}
		else
		{
			if (samples_v == 1)
			{
				switch (samples_u)
				{
				case 1:
					scale_image_fast_with_byte_swap<1>(dst, src, element_size, src_width, src_height, padding);
					break;
				case 2:
					scale_image_fast_with_byte_swap<2>(dst, src, element_size, src_width, src_height, padding);
					break;
				case 3:
					scale_image_fast_with_byte_swap<3>(dst, src, element_size, src_width, src_height, padding);
					break;
				case 4:
					scale_image_fast_with_byte_swap<4>(dst, src, element_size, src_width, src_height, padding);
					break;
				case 8:
					scale_image_fast_with_byte_swap<8>(dst, src, element_size, src_width, src_height, padding);
					break;
				case 16:
					scale_image_fast_with_byte_swap<16>(dst, src, element_size, src_width, src_height, padding);
					break;
				default:
					scale_image_fallback_with_byte_swap(dst, src, src_width, src_height, dst_pitch, src_pitch, element_size, samples_u, 1);
				}
			}
			else
			{
				scale_image_fallback_with_byte_swap(dst, src, src_width, src_height, dst_pitch, src_pitch, element_size, samples_u, samples_v);
			}
		}
	}

	void convert_le_f32_to_be_d24(void *dst, void *src, u32 row_length_in_texels, u32 num_rows)
	{
		const u32 num_pixels = row_length_in_texels * num_rows;
		verify(HERE), (num_pixels & 3) == 0;

		const auto num_iterations = (num_pixels >> 2);

		__m128i* dst_ptr = (__m128i*)dst;
		__m128i* src_ptr = (__m128i*)src;

		const __m128 scale_vector = _mm_set1_ps(16777214.f);

#if defined (_MSC_VER) || defined (__SSSE3__)
		if (LIKELY(utils::has_ssse3()))
		{
			const __m128i swap_mask = _mm_set_epi8
			(
				0xF, 0xC, 0xD, 0xE,
				0xB, 0x8, 0x9, 0xA,
				0x7, 0x4, 0x5, 0x6,
				0x3, 0x0, 0x1, 0x2
			);

			for (u32 n = 0; n < num_iterations; ++n)
			{
				const __m128i src_vector = _mm_loadu_si128(src_ptr);
				const __m128i result = _mm_cvtps_epi32(_mm_mul_ps((__m128&)src_vector, scale_vector));
				const __m128i shuffled_vector = _mm_shuffle_epi8(result, swap_mask);
				_mm_stream_si128(dst_ptr, shuffled_vector);
				++dst_ptr;
				++src_ptr;
			}

			return;
		}
#endif

		const __m128i mask1 = _mm_set1_epi32(0xFF00FF00);
		const __m128i mask2 = _mm_set1_epi32(0x00FF0000);
		const __m128i mask3 = _mm_set1_epi32(0x000000FF);

		for (u32 n = 0; n < num_iterations; ++n)
		{
			const __m128i src_vector = _mm_loadu_si128(src_ptr);
			const __m128i result = _mm_cvtps_epi32(_mm_mul_ps((__m128&)src_vector, scale_vector));

			const __m128i v1 = _mm_and_si128(result, mask1);
			const __m128i v2 = _mm_and_si128(_mm_slli_epi32(result, 16), mask2);
			const __m128i v3 = _mm_and_si128(_mm_srli_epi32(result, 16), mask3);
			const __m128i shuffled_vector = _mm_or_si128(_mm_or_si128(v1, v2), v3);

			_mm_stream_si128(dst_ptr, shuffled_vector);
			++dst_ptr;
			++src_ptr;
		}
	}

	void convert_le_d24x8_to_be_d24x8(void *dst, void *src, u32 row_length_in_texels, u32 num_rows)
	{
		const u32 num_pixels = row_length_in_texels * num_rows;
		verify(HERE), (num_pixels & 3) == 0;

		const auto num_iterations = (num_pixels >> 2);

		__m128i* dst_ptr = (__m128i*)dst;
		__m128i* src_ptr = (__m128i*)src;

#if defined (_MSC_VER) || defined (__SSSE3__)
		if (LIKELY(utils::has_ssse3()))
		{
			const __m128i swap_mask = _mm_set_epi8
			(
				0xF, 0xC, 0xD, 0xE,
				0xB, 0x8, 0x9, 0xA,
				0x7, 0x4, 0x5, 0x6,
				0x3, 0x0, 0x1, 0x2
			);

			for (u32 n = 0; n < num_iterations; ++n)
			{
				const __m128i src_vector = _mm_loadu_si128(src_ptr);
				const __m128i shuffled_vector = _mm_shuffle_epi8(src_vector, swap_mask);
				_mm_stream_si128(dst_ptr, shuffled_vector);
				++dst_ptr;
				++src_ptr;
			}

			return;
		}
#endif

		const __m128i mask1 = _mm_set1_epi32(0xFF00FF00);
		const __m128i mask2 = _mm_set1_epi32(0x00FF0000);
		const __m128i mask3 = _mm_set1_epi32(0x000000FF);

		for (u32 n = 0; n < num_iterations; ++n)
		{
			const __m128i src_vector = _mm_loadu_si128(src_ptr);
			const __m128i v1 = _mm_and_si128(src_vector, mask1);
			const __m128i v2 = _mm_and_si128(_mm_slli_epi32(src_vector, 16), mask2);
			const __m128i v3 = _mm_and_si128(_mm_srli_epi32(src_vector, 16), mask3);
			const __m128i shuffled_vector = _mm_or_si128(_mm_or_si128(v1, v2), v3);

			_mm_stream_si128(dst_ptr, shuffled_vector);
			++dst_ptr;
			++src_ptr;
		}
	}

	void convert_le_d24x8_to_le_f32(void *dst, void *src, u32 row_length_in_texels, u32 num_rows)
	{
		const u32 num_pixels = row_length_in_texels * num_rows;
		verify(HERE), (num_pixels & 3) == 0;

		const auto num_iterations = (num_pixels >> 2);

		__m128i* dst_ptr = (__m128i*)dst;
		__m128i* src_ptr = (__m128i*)src;

		const __m128 scale_vector = _mm_set1_ps(1.f / 16777214.f);
		const __m128i mask = _mm_set1_epi32(0x00FFFFFF);
		for (u32 n = 0; n < num_iterations; ++n)
		{
			const __m128 src_vector = _mm_cvtepi32_ps(_mm_and_si128(mask, _mm_loadu_si128(src_ptr)));
			const __m128 normalized_vector = _mm_mul_ps(src_vector, scale_vector);
			_mm_stream_si128(dst_ptr, (__m128i&)normalized_vector);
			++dst_ptr;
			++src_ptr;
		}
	}
}
