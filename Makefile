# Makefile — libstrand
#
# Targets:
#   make / make dev  — debug build with ASan/UBSan (Linux only)
#   make release     — optimised build
#   make test        — build and run test suite (wired in Task 1.2)
#   make test-tsan   — TSan build, Clang only, Linux only
#   make valgrind    — run tests under Valgrind, Linux only
#   make bench       — build and run benchmarks (Phase 7)
#   make lint        — clang-tidy + cppcheck
#   make format      — clang-format -i on all sources
#   make clean       — remove build artefacts
#   make install     — install libstrand.a and include/strand.h
#
# Compatible with GNU make (Linux) and BSD make (OpenBSD).
# Uses != for shell assignment — supported by GNU make >= 3.82 and BSD make.
# Explicit per-file compile rules — no pattern rules (BSD make portable).
# See TECH_STACK.md §5 for full build system specification.

# --- Platform and architecture detection ------------------------------------

OS   != uname -s
ARCH != uname -m

# --- Compiler ---------------------------------------------------------------

# Clang primary (required), GCC secondary.
# Override on the command line: make CC=gcc
CC = clang

# --- Compiler flags ---------------------------------------------------------

CFLAGS_COMMON = -std=c11 -Wall -Wextra -Wpedantic			\
                -Wno-unused-parameter					\
                -fno-omit-frame-pointer					\
                -D_POSIX_C_SOURCE=200809L				\
                -D_XOPEN_SOURCE=700

# Platform flags — STRAND_LINUX or STRAND_OPENBSD
CFLAGS_OS != if [ "$(OS)" = "Linux" ]; then echo "-DSTRAND_LINUX"; \
             elif [ "$(OS)" = "OpenBSD" ]; then echo "-DSTRAND_OPENBSD"; \
             else echo ""; fi

# ASan/UBSan — Linux only; OpenBSD clang does not support -fsanitize=address
SANITIZERS != if [ "$(OS)" = "Linux" ]; then echo "-fsanitize=address,undefined"; else echo ""; fi

CFLAGS_DEV     = $(CFLAGS_COMMON) $(CFLAGS_OS)				\
                 -O1 -g -Werror					\
                 $(SANITIZERS)						\
                 -DSTRAND_DEBUG

CFLAGS_TSAN    = $(CFLAGS_COMMON) $(CFLAGS_OS)				\
                 -O1 -g							\
                 -fsanitize=thread					\
                 -DSTRAND_DEBUG

CFLAGS_VG      = $(CFLAGS_COMMON) $(CFLAGS_OS) -O1 -g -DSTRAND_DEBUG

CFLAGS_RELEASE = $(CFLAGS_COMMON) $(CFLAGS_OS) -O2 -DNDEBUG

LDFLAGS = -lpthread

# --- Install paths ----------------------------------------------------------

PREFIX     ?= /usr/local
LIBDIR     ?= $(PREFIX)/lib
INCLUDEDIR ?= $(PREFIX)/include

# --- ASM source — selected by architecture ----------------------------------
#
# Not compiled in Phase 1 (files do not exist yet).
# Phase 2 (Task 2.2/2.3) creates these files and adds ASM_SRC to the build.
# OpenBSD reports x86_64 as "amd64" from uname -m.
ASM_SRC != if [ "$(ARCH)" = "x86_64" ] || [ "$(ARCH)" = "amd64" ]; then \
               echo "src/arch/x86_64/strand_context.S"; \
           elif [ "$(ARCH)" = "aarch64" ] || [ "$(ARCH)" = "arm64" ]; then \
               echo "src/arch/arm64/strand_context.S"; \
           else echo ""; fi

# --- Library source files ---------------------------------------------------

LIB_SRCS = src/strand_context.c	\
           src/strand_fiber.c		\
           src/strand_sched.c		\
           src/strand_poller.c		\
           src/strand_inject.c		\
           src/strand_runtime.c		\
           src/strand_offload.c		\
           src/strand_scope.c

# --- Build paths ------------------------------------------------------------

BUILD_DIR       = build
REL_DIR         = $(BUILD_DIR)/rel
BUILD_TESTS_DIR = $(BUILD_DIR)/tests
LIB_DEV         = $(BUILD_DIR)/libstrand.a
LIB_RELEASE     = $(REL_DIR)/libstrand.a
TEST_BIN        = $(BUILD_TESTS_DIR)/run_tests
TEST_BIN_VG     = $(BUILD_TESTS_DIR)/run_tests_vg

INCLUDES = -I include/

# --- Phony targets ----------------------------------------------------------

.PHONY: all dev release test test-tsan valgrind bench lint format clean install

all: dev

# Development build with ASan/UBSan (Linux) — also builds test binary
dev: $(LIB_DEV) $(TEST_BIN)

# Release — optimised static library
release: $(LIB_RELEASE)

# Test suite — ASan/UBSan build
test: $(TEST_BIN)
	$(TEST_BIN)

# TSan — Clang only, Linux only
test-tsan:
	@command -v clang >/dev/null 2>&1 || \
	    { echo "TSan target requires Clang"; exit 1; }
	@echo "No test binary yet — implement Task 1.2 first"

# Valgrind — Linux only, no sanitizers (ASan + Valgrind conflict)
valgrind: $(TEST_BIN_VG)
	valgrind --leak-check=full			\
	         --show-leak-kinds=all			\
	         --track-origins=yes			\
	         --error-exitcode=1			\
	         $(TEST_BIN_VG)

# Benchmarks — Phase 7
bench:
	@echo "No benchmarks yet — implement Phase 7"

# clang-tidy + cppcheck
lint: $(LIB_DEV)
	clang-tidy $(LIB_SRCS) -- $(CFLAGS_DEV) $(INCLUDES)
	@if command -v cppcheck >/dev/null 2>&1; then \
		cppcheck --enable=all --error-exitcode=1 \
		         --suppress=missingIncludeSystem \
		         src/; \
	else \
		echo "cppcheck not found; skipping cppcheck step"; \
	fi

format:
	clang-format -i $(LIB_SRCS) src/*.h include/strand.h

install: $(LIB_RELEASE)
	install -d $(LIBDIR) $(INCLUDEDIR)
	install -m 644 $(LIB_RELEASE) $(LIBDIR)/libstrand.a
	install -m 644 include/strand.h $(INCLUDEDIR)/strand.h

clean:
	rm -rf $(BUILD_DIR)

# --- Development library (explicit per-file compile + ar) -------------------

$(LIB_DEV): $(LIB_SRCS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS_DEV) $(INCLUDES) -c src/strand_context.c  -o $(BUILD_DIR)/strand_context.o
	$(CC) $(CFLAGS_DEV) $(INCLUDES) -c src/strand_fiber.c    -o $(BUILD_DIR)/strand_fiber.o
	$(CC) $(CFLAGS_DEV) $(INCLUDES) -c src/strand_sched.c    -o $(BUILD_DIR)/strand_sched.o
	$(CC) $(CFLAGS_DEV) $(INCLUDES) -c src/strand_poller.c   -o $(BUILD_DIR)/strand_poller.o
	$(CC) $(CFLAGS_DEV) $(INCLUDES) -c src/strand_inject.c   -o $(BUILD_DIR)/strand_inject.o
	$(CC) $(CFLAGS_DEV) $(INCLUDES) -c src/strand_runtime.c  -o $(BUILD_DIR)/strand_runtime.o
	$(CC) $(CFLAGS_DEV) $(INCLUDES) -c src/strand_offload.c  -o $(BUILD_DIR)/strand_offload.o
	$(CC) $(CFLAGS_DEV) $(INCLUDES) -c src/strand_scope.c    -o $(BUILD_DIR)/strand_scope.o
	ar rcs $(LIB_DEV)					\
	    $(BUILD_DIR)/strand_context.o			\
	    $(BUILD_DIR)/strand_fiber.o				\
	    $(BUILD_DIR)/strand_sched.o				\
	    $(BUILD_DIR)/strand_poller.o			\
	    $(BUILD_DIR)/strand_inject.o			\
	    $(BUILD_DIR)/strand_runtime.o			\
	    $(BUILD_DIR)/strand_offload.o			\
	    $(BUILD_DIR)/strand_scope.o

# --- Release library (explicit per-file compile + ar) -----------------------

$(LIB_RELEASE): $(LIB_SRCS)
	@mkdir -p $(REL_DIR)
	$(CC) $(CFLAGS_RELEASE) $(INCLUDES) -c src/strand_context.c  -o $(REL_DIR)/strand_context.o
	$(CC) $(CFLAGS_RELEASE) $(INCLUDES) -c src/strand_fiber.c    -o $(REL_DIR)/strand_fiber.o
	$(CC) $(CFLAGS_RELEASE) $(INCLUDES) -c src/strand_sched.c    -o $(REL_DIR)/strand_sched.o
	$(CC) $(CFLAGS_RELEASE) $(INCLUDES) -c src/strand_poller.c   -o $(REL_DIR)/strand_poller.o
	$(CC) $(CFLAGS_RELEASE) $(INCLUDES) -c src/strand_inject.c   -o $(REL_DIR)/strand_inject.o
	$(CC) $(CFLAGS_RELEASE) $(INCLUDES) -c src/strand_runtime.c  -o $(REL_DIR)/strand_runtime.o
	$(CC) $(CFLAGS_RELEASE) $(INCLUDES) -c src/strand_offload.c  -o $(REL_DIR)/strand_offload.o
	$(CC) $(CFLAGS_RELEASE) $(INCLUDES) -c src/strand_scope.c    -o $(REL_DIR)/strand_scope.o
	ar rcs $(LIB_RELEASE)					\
	    $(REL_DIR)/strand_context.o				\
	    $(REL_DIR)/strand_fiber.o				\
	    $(REL_DIR)/strand_sched.o				\
	    $(REL_DIR)/strand_poller.o				\
	    $(REL_DIR)/strand_inject.o				\
	    $(REL_DIR)/strand_runtime.o				\
	    $(REL_DIR)/strand_offload.o				\
	    $(REL_DIR)/strand_scope.o

# --- Test binary (ASan/UBSan build) -----------------------------------------

$(TEST_BIN): $(LIB_DEV) tests/run_tests.c tests/test_harness.h
	@mkdir -p $(BUILD_TESTS_DIR)
	$(CC) $(CFLAGS_DEV) $(INCLUDES) -I tests/			\
	    tests/run_tests.c $(LIB_DEV) $(LDFLAGS)			\
	    -o $(TEST_BIN)

# --- Valgrind test binary (no sanitizers) -----------------------------------

$(TEST_BIN_VG): $(LIB_DEV) tests/run_tests.c tests/test_harness.h
	@mkdir -p $(BUILD_TESTS_DIR)
	$(CC) $(CFLAGS_VG) $(INCLUDES) -I tests/			\
	    tests/run_tests.c $(LIB_DEV) $(LDFLAGS)			\
	    -o $(TEST_BIN_VG)
