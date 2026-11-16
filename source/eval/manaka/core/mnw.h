#ifndef MANAKA_CORE_MNW_H_INCLUDED
#define MANAKA_CORE_MNW_H_INCLUDED

// MNW1 (.mnw) の読み込み。f32 のままの開発用の入口で、本番の nn.bin ではない。ファイルのメモリ上限は 8 GiB。
//
// 書式の正本は Rust 側の crates/manaka-core/src/params.rs(Params::write / Params::read)。
// 36 バイトのヘッダーの後ろに f32 の配列が隙間なく並ぶ。リトルエンディアン。
//
//   ヘッダー: u32 magic("MNW1") / u16 version(5) / u16 x 9 (dim order hidden pair_dim hidden2
//             moment_dim field_dim pair_lse flags) / u32 tokens / u32 policy_size / u32 pair_rows
//   配列    : embed gamma pair pair_gamma scale_gamma w1 b1 w2 b2 value_w value_b policy policy_b
//             policy_from policy_from_gamma moment_embed moment_gamma field field_gamma policy_field
//
// 読み込みは「ヘッダーから全配列の大きさを checked な 64 ビット演算で出し、確保の前にファイルの大きさと
// 照合する」。ずれていれば(切り詰め、末尾の余り)何も確保せずに拒否する。
// 全配列の NaN と Inf は拒否する(policy 系も調べる)。
// 評価の中間値が f32 の範囲を超えうる形も、最大トークン数による保守的上界で拒否する。

#include <cstdint>
#include <string>

#include "common.h"

namespace YaneuraOu {
namespace Eval {
namespace ManakaCore {

constexpr uint32_t kMnwMagic       = 0x31574E4Du; // "MNW1"
constexpr uint16_t kMnwVersion     = 5;
constexpr uint64_t kMnwHeaderBytes = 36;

// Features::Tokens 空間の大きさ(token.rs)。
constexpr uint32_t kNumTokens     = 14 * 2 * 81 + 2 * (18 + 4 + 4 + 4 + 4 + 2 + 2); // 2344
constexpr uint32_t kNumPairs      = kNumTokens * (kNumTokens - 1) / 2;              // 2,745,996
constexpr uint32_t kPolicySize    = 27 * 81;                                          // 2187
constexpr uint32_t kMoveChannels  = 27;
constexpr uint32_t kMaxOrder      = 4;
constexpr uint32_t kMnwFieldSquares = 81; // field 配列の升の数

// ヘッダー flags のビット。
constexpr uint16_t kFlagKeepScale = 1u << 0;
constexpr uint16_t kFlagNnue      = 1u << 3;
constexpr uint16_t kFlagSplit     = 1u << 4;
constexpr uint16_t kFlagKnownMask = 0x1Fu; // keep_scale、特徴空間(bit 1-2)、nnue、split。これ以外のビットは拒否する。

// 特徴空間(flags の bit 1-2)。
constexpr int kFeaturesTokens = 0;
constexpr int kFeaturesKp     = 1;
constexpr int kFeaturesHalfKp = 2;

// ファイル中の配列。並びがそのままファイルの並び。
enum MnwSectionId {
	kSecEmbed,
	kSecGamma,
	kSecPair,
	kSecPairGamma,
	kSecScaleGamma,
	kSecW1,
	kSecB1,
	kSecW2,
	kSecB2,
	kSecValueW,
	kSecValueB,
	kSecPolicy,
	kSecPolicyB,
	kSecPolicyFrom,
	kSecPolicyFromGamma,
	kSecMomentEmbed,
	kSecMomentGamma,
	kSecField,
	kSecFieldGamma,
	kSecPolicyField,
	kNumMnwSections
};

struct MnwSection {
	uint64_t offset = 0; // ファイル先頭からのバイト位置
	uint64_t count  = 0; // f32 の個数
};

struct MnwHeader {
	// ファイルにある値
	uint16_t version = 0;
	uint16_t dim = 0, order = 0, hidden = 0, pair_dim = 0, hidden2 = 0;
	uint16_t moment_dim = 0, field_dim = 0, pair_lse = 0, flags = 0;
	uint32_t tokens = 0, policy_size = 0, pair_rows = 0;

	// flags を分けたもの
	bool keep_scale = false;
	int  features = kFeaturesTokens;
	bool nnue = false;
	bool split_sides = false;

	// 形から決まる値
	uint32_t head_width = 0;  // value 行の幅。hidden2 があればそれ、なければ hidden
	uint64_t moment_len = 0;  // moment_dim * (moment_dim + 1) / 2
	uint64_t scale_len = 0;   // keep_scale のときの正規化ブロックの数
	uint64_t block_len = 0;   // 1 枠の特徴の長さ
	uint64_t feature_len = 0; // W1 の入力幅 = 2 * block_len

	uint64_t file_bytes = 0;
	MnwSection section[kNumMnwSections];
};

// 36 バイトのヘッダーとファイル全体の大きさから、形と全配列の位置を決める。ファイルは読まない。
// 検査するのは、magic、版、flags の未知のビット、特徴空間と tokens と pair_rows と policy_size の整合、
// 次元の範囲、全配列の大きさの合計(桁あふれなし)とファイルの大きさの一致。
bool parse_mnw_header(const uint8_t* bytes, uint64_t file_bytes, MnwHeader* out, std::string* err);

// ファイルを開いてヘッダーだけを読み、parse_mnw_header と同じ検査をする。配列は読まない。
bool read_mnw_header(const std::string& path, MnwHeader* out, std::string* err);

// この core(v1)が評価できない形なら、理由を返す。評価できれば nullptr。
//   対象: Tokens 空間の pair / h2 / order 1 から 4 / 任意の dim と hidden。
//   対象外: lse、ks(keep_scale)、m(moment)、f(field)、split、nnue 算術、KP と HalfKP。
const char* mnw_unsupported_reason(const MnwHeader& h);

// "token_pair16_d128_o3_h512_h2256" の形の名前(shape.rs の format と同じ並び)。
std::string mnw_shape_name(const MnwHeader& h);

struct MnwLoadOptions {
	// true: pair 表(base 形で 176 MB)を、ファイルの mmap のまま参照する。コピーしないので起動が速く常駐も増えないが、
	//       行がファイル中の 4 バイト境界にあるため、1 行(64 バイト)がキャッシュ行をまたぎうる。
	// false: 64 バイト境界(大きければ 2 MiB 境界 + MADV_HUGEPAGE)の領域へコピーする。速度の測定向き。
	bool mmap_pair = true;
	// mmap に MAP_POPULATE を付けて、先にページを引き込む。
	bool populate = false;
};

// 評価に使う配列を持つ。embed、gamma、pair_gamma、w1、b1、w2、b2、value_w は 64 バイト境界の自前の領域にコピーし
// (合わせて数 MB)、pair だけを MnwLoadOptions で選ぶ。
class MnwWeights {
public:
	MnwWeights() = default;
	MnwWeights(const MnwWeights&) = delete;
	MnwWeights& operator=(const MnwWeights&) = delete;
	MnwWeights(MnwWeights&& o) noexcept;
	MnwWeights& operator=(MnwWeights&& o) noexcept;
	~MnwWeights();

	// 対象外の形(mnw_unsupported_reason)は、ここで明示的に拒否する。
	static bool load(const std::string& path, const MnwLoadOptions& opt, MnwWeights* out, std::string* err);

	const MnwHeader& header() const { return header_; }

	const float* embed() const { return embed_.data(); }           // [tokens][dim]
	const float* gamma() const { return gamma_.data(); }           // [order][dim]
	const float* pair() const { return pair_; }                    // [pair_rows][pair_dim]。pair_dim が 0 なら nullptr
	const float* pair_gamma() const { return pair_gamma_.data(); } // [pair_dim]
	const float* w1() const { return w1_.data(); }                 // [hidden][feature_len]
	const float* b1() const { return b1_.data(); }                 // [hidden]
	const float* w2() const { return w2_.data(); }                 // [hidden2][hidden]
	const float* b2() const { return b2_.data(); }                 // [hidden2]
	const float* value_w() const { return value_w_.data(); }       // [head_width]
	float value_b() const { return value_b_; }

	// 重みの置換と move で変わる。評価器は一致しない版を使用しない。
	uint64_t revision() const { return revision_; }

	bool pair_is_mapped() const { return map_base_ != nullptr && pair_ != nullptr && pair_owned_.data() == nullptr; }

	// コピーして自前で持っているバイト数(pair を mmap のままなら pair は含まない)。
	uint64_t owned_bytes() const;

private:
	void release();

	uint64_t revision_ = 0;
	MnwHeader header_;
	AlignedBuffer<float> embed_, gamma_, pair_gamma_, w1_, b1_, w2_, b2_, value_w_;
	AlignedBuffer<float> pair_owned_;
	const float* pair_ = nullptr;
	float value_b_ = 0.0f;
	void* map_base_ = nullptr;
	uint64_t map_bytes_ = 0;
};

} // namespace ManakaCore
} // namespace Eval
} // namespace YaneuraOu

#endif // MANAKA_CORE_MNW_H_INCLUDED
