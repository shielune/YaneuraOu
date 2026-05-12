LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

CPPFLAGS := -DTARGET_ARCH="$(TARGET_ARCH_ABI)" -DYANEURAOU_FFI

# YANEURAOU_EDITION を ndk-build 呼び出し時に上書き可能にする。
# 例:
#   ndk-build YANEURAOU_EDITION=YANEURAOU_ENGINE_NNUE              (HalfKP256)
#   ndk-build YANEURAOU_EDITION=YANEURAOU_ENGINE_NNUE_KP256        (KP256)
#   ndk-build YANEURAOU_EDITION=YANEURAOU_MATE_ENGINE              (Mate Solver)
YANEURAOU_EDITION ?= YANEURAOU_ENGINE_NNUE
EXTRA_CPPFLAGS =
MATERIAL_LEVEL = 1
CPPFLAGS += $(EXTRA_CPPFLAGS)

# 共通 NNUE ソース (KP256 / HalfKP256 両方で使う)
NNUE_SOURCES := \
  ../source/eval/nnue/evaluate_nnue.cpp                                \
  ../source/eval/nnue/evaluate_nnue_learner.cpp                        \
  ../source/eval/nnue/nnue_test_command.cpp                            \
  ../source/eval/nnue/features/k.cpp                                   \
  ../source/eval/nnue/features/p.cpp                                   \
  ../source/eval/nnue/features/half_kp.cpp                             \
  ../source/eval/nnue/features/half_kp_vm.cpp                          \
  ../source/eval/nnue/features/half_relative_kp.cpp                    \
  ../source/eval/nnue/features/half_kpe9.cpp                           \
  ../source/eval/nnue/features/pe9.cpp                                 \
  ../source/engine/yaneuraou-engine/yaneuraou-search.cpp

ifeq ($(YANEURAOU_EDITION),YANEURAOU_ENGINE_NNUE)
  CPPFLAGS += -DUSE_MAKEFILE -DYANEURAOU_ENGINE_NNUE
  ENGINE_NAME := yaneuraou-halfkp
  ENGINE_SOURCES := $(NNUE_SOURCES)
endif

ifeq ($(YANEURAOU_EDITION),YANEURAOU_ENGINE_NNUE_KP256)
  CPPFLAGS += -DUSE_MAKEFILE -DYANEURAOU_ENGINE_NNUE -DEVAL_NNUE_KP256
  ENGINE_NAME := yaneuraou-kp256
  ENGINE_SOURCES := $(NNUE_SOURCES)
endif

ifeq ($(YANEURAOU_EDITION),YANEURAOU_MATE_ENGINE)
  CPPFLAGS += -DUSE_MAKEFILE -DYANEURAOU_MATE_ENGINE
  ENGINE_NAME := yaneuraou-mate
  ENGINE_SOURCES := ../source/engine/yaneuraou-mate-engine/yaneuraou-mate-search.cpp
endif

ifeq ($(TARGET_ARCH_ABI),arm64-v8a)
  CPPFLAGS += -DIS_64BIT -DUSE_NEON
  LOCAL_ARM_NEON := true
endif

LOCAL_SRC_FILES := \
  ../source/yaneuraou_ffi.cpp                                          \
  ../source/types.cpp                                                  \
  ../source/bitboard.cpp                                               \
  ../source/misc.cpp                                                   \
  ../source/movegen.cpp                                                \
  ../source/position.cpp                                               \
  ../source/usi.cpp                                                    \
  ../source/usi_option.cpp                                             \
  ../source/thread.cpp                                                 \
  ../source/tt.cpp                                                     \
  ../source/movepick.cpp                                               \
  ../source/timeman.cpp                                                \
  ../source/memory.cpp                                                 \
  ../source/book/apery_book.cpp                                        \
  ../source/book/book.cpp                                              \
  ../source/book/policybook.cpp                                        \
  ../source/extra/bitop.cpp                                            \
  ../source/extra/long_effect.cpp                                      \
  ../source/extra/sfen_packer.cpp                                      \
  ../source/extra/super_sort.cpp                                       \
  ../source/mate/mate.cpp                                              \
  ../source/mate/mate1ply_without_effect.cpp                           \
  ../source/mate/mate1ply_with_effect.cpp                              \
  ../source/mate/mate_solver.cpp                                       \
  ../source/eval/evaluate_bona_piece.cpp                               \
  ../source/eval/evaluate.cpp                                          \
  ../source/eval/evaluate_io.cpp                                       \
  ../source/eval/evaluate_mir_inv_tools.cpp                            \
  ../source/eval/material/evaluate_material.cpp                        \
  ../source/testcmd/benchmark.cpp                                      \
  ../source/testcmd/mate_test_cmd.cpp                                  \
  ../source/testcmd/normal_test_cmd.cpp                                \
  ../source/testcmd/unit_test.cpp                                      \
  $(ENGINE_SOURCES)

LOCAL_MODULE    := $(ENGINE_NAME)
LOCAL_CXXFLAGS  := -std=c++17 -fno-exceptions -fno-rtti -Wextra -O3 -MMD -MP -fpermissive -D__STDINT_MACROS -D__STDC_LIMIT_MACROS $(CPPFLAGS)
LOCAL_CXXFLAGS += -DNDEBUG -fPIC -Wno-unused-parameter
LOCAL_LDFLAGS  := -fPIC

include $(BUILD_SHARED_LIBRARY)
