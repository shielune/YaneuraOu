// SFNN without PSQT architecture

#ifndef CLASSIC_NNUE_SFNN_SFNN_HALFKAHM2_2304_15_64_PROGRESS8EK_H_INCLUDED
#define CLASSIC_NNUE_SFNN_SFNN_HALFKAHM2_2304_15_64_PROGRESS8EK_H_INCLUDED

#include "../features/feature_set.h"

#include "../features/half_ka_hm2.h"

#include "sfnn_network.h"

namespace YaneuraOu {
namespace Eval::NNUE {

// Input features used in evaluation function
// 評価関数で用いる入力特徴量

    using RawFeatures = Features::FeatureSet<
        Features::HalfKA_hm2<Features::Side::kFriend>>;

    // Number of input feature dimensions after conversion
    // 変換後の入力特徴量の次元数
    constexpr IndexType kTransformedFeatureDimensions = 2304;

    // Number of networks stored in the evaluation file
    constexpr int LayerStacks = 9;

    #define NNUE_SFNN_HAND_BUCKETS 1
    #define NNUE_SFNN_KING_BUCKETS 1
    #define NNUE_SFNN_PROGRESS_BUCKETS 9

    // 1なら、進行度バケットの最後の1つを相入玉局面専用にする。
    // 💡 NAGISA_V3 の評価関数 (progress8ek) がこの形。
    #define NNUE_SFNN_PROGRESS_ENTERING_KING 1

    // 1なら、進行度係数を nn.bin ではなく外部ファイルから読む。
    // 💡 progress8ek 系は係数を progress.bin として別配布しているのでこちら。
    //    このとき nn.bin には進行度セクションが無いので、hashにも含めない。
    #define NNUE_SFNN_PROGRESS_EXTERNAL 1

    // Number of groups for the first affine layer of SFNN.
    // common+shard fc_0でのみ2以上になる。
    constexpr IndexType kHidden1GroupCount = 1;

    // common+shard fc_0 settings. kHidden1ShardDimensions is per shard.
    constexpr bool kHidden1UsesCommonShard = false;
    constexpr IndexType kHidden1CommonDimensions = 0;
    constexpr IndexType kHidden1ShardDimensions = 0;

    // 各層の次元数
    constexpr IndexType kInputDims   = kTransformedFeatureDimensions;
    constexpr IndexType kHidden1Dims = 15;
    constexpr IndexType kHidden2Dims = 64;                              

    

    using Fc0Layer = Layers::AffineTransformSparseInputExplicit<kInputDims, kHidden1Dims + 1>;
    using NetworkBase = SfnnNetwork<Fc0Layer, kInputDims, kHidden1Dims, kHidden2Dims>;

    struct Network : NetworkBase {
        static std::string GetStructureString() {
            return "SFNN_HALFKAHM2_2304_15_64_PROGRESS8EK";
        }
    };

}  // namespace Eval::NNUE
}  // namespace YaneuraOu

#endif // CLASSIC_NNUE_SFNN_HALFKAHM2_2304_15_64_PROGRESS8EK_H_INCLUDED
