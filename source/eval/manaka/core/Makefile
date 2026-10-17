# YaneuraOu の他のファイルに依存しない、Manaka の評価の core の単体ビルド。
#   make            : build/ に eval_dump、eval_bench、selftest を作る
#   make test       : selftest を走らせる
#   make asan       : build-asan/ に ASan と UBSan 付きで作る(make test-asan で selftest を走らせる)
#
# 浮動小数の結果を Rust と一致させるため、-ffast-math を使わず、-ffp-contract=off にする(FMA への融合を禁止)。
# AVX2 だけを使う(AVX-512 は使わない)。

CXX      ?= g++
BUILD    ?= build
OPT      ?= -O3
SAN      ?=
CXXFLAGS ?= -std=c++17 $(OPT) -g -mavx2 -ffp-contract=off -fno-fast-math -Wall -Wextra -Wshadow $(SAN)
LDFLAGS  ?= $(SAN)

LIB_SRC := mnw.cpp tokens.cpp eval_f32.cpp
LIB_OBJ := $(LIB_SRC:%.cpp=$(BUILD)/%.o)
TOOLS   := $(BUILD)/eval_dump $(BUILD)/eval_bench $(BUILD)/selftest

all: $(TOOLS)

$(BUILD)/%.o: %.cpp
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/tools_%.o: tools/%.cpp
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/eval_dump: $(BUILD)/tools_eval_dump.o $(LIB_OBJ)
	$(CXX) $^ -o $@ $(LDFLAGS)

$(BUILD)/eval_bench: $(BUILD)/tools_eval_bench.o $(LIB_OBJ)
	$(CXX) $^ -o $@ $(LDFLAGS)

$(BUILD)/selftest: $(BUILD)/tools_selftest.o $(LIB_OBJ)
	$(CXX) $^ -o $@ $(LDFLAGS)

test: $(BUILD)/selftest
	$(BUILD)/selftest

asan:
	$(MAKE) BUILD=build-asan OPT="-O1" SAN="-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer" all

test-asan: asan
	build-asan/selftest

clean:
	rm -rf build build-asan

.PHONY: all test asan test-asan clean

-include $(wildcard $(BUILD)/*.d)
