// Input features and network structure used in NNUE evaluation function
// NNUE評価関数で用いる入力特徴量とネットワーク構造

#ifndef CLASSIC_NNUE_ARCHITECTURE_H_INCLUDED
#define CLASSIC_NNUE_ARCHITECTURE_H_INCLUDED

#include "../../config.h"

#if defined(EVAL_NNUE)

// Defines the network structure
// 入力特徴量とネットワーク構造が定義されたヘッダをincludeする

#if defined(NNUE_ARCHITECTURE_HEADER)

// 動的に生成されたファイルがある。
#include NNUE_ARCHITECTURE_HEADER

#elif defined(EVAL_NNUE_HALFKP256)

// 標準NNUE型。NNUE評価関数のデフォルトは、halfKP256

#include "architectures/halfkp_256x2-32-32.h"

#elif defined(EVAL_NNUE_KP256)

// kp型
#include "architectures/kp_256x2-32-32.h"

#elif defined(EVAL_NNUE_HALFKPE9)

// halfkpe9型
#include "architectures/halfkpe9_256x2-32-32.h"

#elif defined(YANEURAOU_ENGINE_NNUE_HALFKP_512X2_16_32)

// halfkp_512x2-16-32型
#include "architectures/halfkp_512x2-16-32.h"

#elif defined(YANEURAOU_ENGINE_NNUE_HALFKP_1024X2_8_32)

// halfkp_1024x2-8-32型
#include "architectures/halfkp_1024x2-8-32.h"

#elif defined(YANEURAOU_ENGINE_NNUE_HALFKP_1024X2_8_64)

// halfkp_1024x2-8-64型
#include "architectures/halfkp_1024x2-8-64.h"

#elif defined(YANEURAOU_ENGINE_SFNN1536)

// SFNN without Psqt 1536型
#include "architectures/sfnn-1536.h"

#elif defined(EVAL_NNUE_HALFKP_VM_256X2_32_32)

// halfkpvm_256x2-32-32型
#include "architectures/halfkpvm_256x2-32-32.h"

#else

// どれも定義されていなかったので標準NNUE型にしておく。
#include "architectures/halfkp_256x2-32-32.h"

#endif

// 📝 進行度バケットの最後を相入玉専用にするか。
//    アーキヘッダ (nnue_arch_gen.py が生成) が定義するが、
//    それ以前に生成された古いヘッダには無いので既定値を置く。
#if !defined(NNUE_SFNN_PROGRESS_ENTERING_KING)
	#define NNUE_SFNN_PROGRESS_ENTERING_KING 0
#endif
#if !defined(NNUE_SFNN_PROGRESS_EXTERNAL)
	#define NNUE_SFNN_PROGRESS_EXTERNAL 0
#endif

namespace YaneuraOu {
namespace Eval::NNUE {

	static_assert(kTransformedFeatureDimensions % kMaxSimdWidth == 0, "");
	static_assert(Network::kOutputDimensions == 1, "");
	static_assert(std::is_same<Network::OutputType, std::int32_t>::value, "");

	// Trigger for full calculation instead of difference calculation
	// 差分計算の代わりに全計算を行うタイミングのリスト
	constexpr auto kRefreshTriggers = RawFeatures::kRefreshTriggers;

} // namespace Eval::NNUE
} // namespace YaneuraOu

#endif  // defined(EVAL_NNUE)

#endif // #ifndef NNUE_ARCHITECTURE_H_INCLUDED
