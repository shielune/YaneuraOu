// SFNN without PSQT architecture

#ifndef CLASSIC_NNUE_SFNN_SFNN_KA2_2048_8_64_PROGRESS8X_H_INCLUDED
#define CLASSIC_NNUE_SFNN_SFNN_KA2_2048_8_64_PROGRESS8X_H_INCLUDED

#include "../features/feature_set.h"

#include "../features/k.h"
#include "../features/a2.h"

#include "sfnn_network.h"

namespace YaneuraOu {
namespace Eval::NNUE {

// Input features used in evaluation function
// 評価関数で用いる入力特徴量

    using RawFeatures = Features::FeatureSet<Features::K, Features::A2>;

    // Number of input feature dimensions after conversion
    // 変換後の入力特徴量の次元数
    constexpr IndexType kTransformedFeatureDimensions = 2048;

    // 小幅SFNN専用。従来幅のSFNNでは定義せず、既存の高速経路をそのまま使う。
    

    // Number of networks stored in the evaluation file
    constexpr int LayerStacks = 8;

    #define NNUE_SFNN_HAND_BUCKETS 1
    #define NNUE_SFNN_HAND_BUCKET_TYPE 0
    #define NNUE_SFNN_KING_BUCKETS 1
    #define NNUE_SFNN_KING_BUCKET_TYPE 0
    #define NNUE_SFNN_PROGRESS_BUCKETS 8

    // 1なら、進行度バケットの最後の1つを相入玉局面専用にする。(フォーク独自)
    // 💡 NAGISA_V3 の評価関数 (progress8ek) がこの形。
    #define NNUE_SFNN_PROGRESS_ENTERING_KING 0

    // Number of groups for the first affine layer of SFNN.
    // common+shard fc_0でのみ2以上になる。
    constexpr IndexType kHidden1GroupCount = 1;

    // common+shard fc_0 settings. kHidden1ShardDimensions is per shard.
    constexpr bool kHidden1UsesCommonShard = false;
    constexpr IndexType kHidden1CommonDimensions = 0;
    constexpr IndexType kHidden1ShardDimensions = 0;

    // 各層の次元数
    constexpr IndexType kInputDims   = kTransformedFeatureDimensions;
    constexpr IndexType kHidden1Dims = 8;
    constexpr bool kUseShortcut = false;
    constexpr IndexType kHidden1OutputDims = kHidden1Dims + (kUseShortcut ? 1 : 0);
    constexpr IndexType kHidden2Dims = 64;                              

    #if !defined(ENABLE_SFNN_16BIT_WEIGHT)
    
    #endif

    using Fc0Layer = Layers::AffineTransformSparseInputExplicit<kInputDims, kHidden1OutputDims>;
    #if defined(ENABLE_SFNN_16BIT_WEIGHT)
    using NetworkBase = SfnnNetwork16<kInputDims, kHidden1Dims, kHidden2Dims, kUseShortcut>;
    #else
    using NetworkBase = SfnnNetwork<Fc0Layer, kInputDims, kHidden1Dims, kHidden2Dims, kUseShortcut>;
    #endif

    struct Network : NetworkBase {
        static std::string GetStructureString() {
            return "SFNN_KA2_2048_8_64_PROGRESS8X";
        }
    };

}  // namespace Eval::NNUE
}  // namespace YaneuraOu

#endif // CLASSIC_NNUE_SFNN_KA2_2048_8_64_PROGRESS8X_H_INCLUDED
