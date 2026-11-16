#ifndef MANAKA_CORE_COMMON_H_INCLUDED
#define MANAKA_CORE_COMMON_H_INCLUDED

// Manaka の評価を YaneuraOu に依存せず計算するための共通の道具。
// このディレクトリ(source/eval/manaka/core/)の中身は YaneuraOu の型(Position, Value, Square など)を
// 一切参照しない。YaneuraOu 側とは BoardView(board.h)だけでつなぐ。

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <utility>

namespace YaneuraOu {
namespace Eval {
namespace ManakaCore {

static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "MNW1 はリトルエンディアンで書かれている。");
static_assert(sizeof(float) == 4, "f32 は 4 バイト。");

// 所有するメモリを alignment バイト境界に置く。mmap した領域(所有しない)とは別物。
// コピーも例外も使わない。確保に失敗したら data() が nullptr で size() が 0 のまま。
template<typename T>
class AlignedBuffer {
public:
	AlignedBuffer() = default;
	AlignedBuffer(const AlignedBuffer&) = delete;
	AlignedBuffer& operator=(const AlignedBuffer&) = delete;
	AlignedBuffer(AlignedBuffer&& o) noexcept : ptr_(o.ptr_), size_(o.size_) { o.ptr_ = nullptr; o.size_ = 0; }
	AlignedBuffer& operator=(AlignedBuffer&& o) noexcept {
		if (this != &o) {
			reset();
			ptr_ = o.ptr_; size_ = o.size_;
			o.ptr_ = nullptr; o.size_ = 0;
		}
		return *this;
	}
	~AlignedBuffer() { reset(); }

	// n 要素を確保する。中身は未初期化。alignment は 2 のべき。
	bool allocate(size_t n, size_t alignment = 64) {
		reset();
		if (n == 0)
			return true;
		if (alignment < alignof(T) || (alignment & (alignment - 1)) != 0
		    || n > (SIZE_MAX - (alignment - 1)) / sizeof(T))
			return false;
		size_t bytes = (n * sizeof(T) + alignment - 1) / alignment * alignment;
		void* p = std::aligned_alloc(alignment, bytes);
		if (p == nullptr)
			return false;
		ptr_ = static_cast<T*>(p);
		size_ = n;
		return true;
	}

	void reset() {
		std::free(ptr_);
		ptr_ = nullptr;
		size_ = 0;
	}

	T* data() { return ptr_; }
	const T* data() const { return ptr_; }
	size_t size() const { return size_; }
	T& operator[](size_t i) { return ptr_[i]; }
	const T& operator[](size_t i) const { return ptr_[i]; }

private:
	T* ptr_ = nullptr;
	size_t size_ = 0;
};

// 64 ビットの足し算と掛け算を、桁あふれを検出しながら行う。
inline bool checked_add(uint64_t a, uint64_t b, uint64_t* out) { return !__builtin_add_overflow(a, b, out); }
inline bool checked_mul(uint64_t a, uint64_t b, uint64_t* out) { return !__builtin_mul_overflow(a, b, out); }

} // namespace ManakaCore
} // namespace Eval
} // namespace YaneuraOu

#endif // MANAKA_CORE_COMMON_H_INCLUDED
