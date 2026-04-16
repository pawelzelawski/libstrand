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
                -D_XOPEN_SOURCE=700				\
                $(HAVE_VALGRIND)

# Platform flags — STRAND_LINUX or STRAND_OPENBSD
CFLAGS_OS != if [ "$(OS)" = "Linux" ]; then echo "-DSTRAND_LINUX"; \
             elif [ "$(OS)" = "OpenBSD" ]; then echo "-DSTRAND_OPENBSD"; \
             else echo ""; fi

# ASan/UBSan — Linux only; OpenBSD clang does not support -fsanitize=address
SANITIZERS != if [ "$(OS)" = "Linux" ]; then echo "-fsanitize=address,undefined"; else echo ""; fi

# Valgrind client request macros — present when valgrind-devel (Fedora/RHEL),
# valgrind-dev (Debian/Ubuntu), or valgrind (OpenBSD ports) is installed.
# The macros are no-ops in non-Valgrind runs; compiled in whenever the header
# is present so Valgrind can track fiber stack boundaries.
# See TECH_STACK.md §7.1.
HAVE_VALGRIND != if $(CC) -include valgrind/valgrind.h -x c /dev/null -c \
                    -o /dev/null 2>/dev/null; \
                 then echo "-DHAVE_VALGRIND"; else echo ""; fi

CFLAGS_DEV     = $(CFLAGS_COMMON) $(CFLAGS_OS)				\
                 -O1 -g -Werror					\
                 $(SANITIZERS)						\
                 -DSTRAND_DEBUG						\
                 -DSTRAND_TEST_CLOCK

CFLAGS_TSAN    = $(CFLAGS_COMMON) $(CFLAGS_OS)				\
                 -O1 -g							\
                 -fsanitize=thread					\
                 -DSTRAND_DEBUG						\
                 -DSTRAND_TEST_CLOCK

CFLAGS_VG      = $(CFLAGS_COMMON) $(CFLAGS_OS) -O1 -g -DSTRAND_DEBUG	\
                 -DSTRAND_TEST_CLOCK

CFLAGS_RELEASE = $(CFLAGS_COMMON) $(CFLAGS_OS) -O2 -DNDEBUG

LDFLAGS = -lpthread

# --- Install paths ----------------------------------------------------------

PREFIX     ?= /usr/local
LIBDIR     ?= $(PREFIX)/lib
INCLUDEDIR ?= $(PREFIX)/include

# --- ASM source — selected by architecture ----------------------------------
#
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
VG_DIR          = $(BUILD_DIR)/vg
LIB_DEV         = $(BUILD_DIR)/libstrand.a
LIB_RELEASE     = $(REL_DIR)/libstrand.a
LIB_VG          = $(VG_DIR)/libstrand.a
TEST_BIN        = $(BUILD_TESTS_DIR)/run_tests
TEST_BIN_VG     = $(BUILD_TESTS_DIR)/run_tests_vg
TSAN_DIR        = $(BUILD_DIR)/tsan
LIB_TSAN        = $(TSAN_DIR)/libstrand.a
TEST_BIN_TSAN   = $(BUILD_TESTS_DIR)/run_tests_tsan

INCLUDES = -I include/

# --- ASM object paths — empty when ASM_SRC is empty ------------------------
#
# Defined after BUILD_DIR so $(BUILD_DIR) is available for expansion.
ASM_OBJ_DEV != if [ -n "$(ASM_SRC)" ]; then echo "$(BUILD_DIR)/strand_context_asm.o"; else echo ""; fi
ASM_OBJ_REL != if [ -n "$(ASM_SRC)" ]; then echo "$(BUILD_DIR)/rel/strand_context_asm.o"; else echo ""; fi
ASM_OBJ_VG   != if [ -n "$(ASM_SRC)" ]; then echo "$(VG_DIR)/strand_context_asm.o"; else echo ""; fi
ASM_OBJ_TSAN != if [ -n "$(ASM_SRC)" ]; then echo "$(TSAN_DIR)/strand_context_asm.o"; else echo ""; fi

# --- Phony targets ----------------------------------------------------------

.PHONY: all dev release test test-tsan valgrind bench lint format clean install

all: dev

# Development build with ASan/UBSan (Linux) — also builds test binary
dev: $(LIB_DEV) $(TEST_BIN)

# Release — optimised static library
release: $(LIB_RELEASE)

# Test suite — ASan/UBSan build
# LSAN (LeakSanitizer) is disabled here: its tracer process uses ptrace to
# scan all mapped pages and crashes (SIGSEGV in the tracer) when it
# encounters PROT_NONE fiber guard pages.  Leak checking is covered by the
# 'valgrind' target which handles guard pages correctly.
test: $(TEST_BIN)
	ASAN_OPTIONS=detect_leaks=0 $(TEST_BIN)

# TSan — Clang only, Linux only
test-tsan:
	@command -v clang >/dev/null 2>&1 || \
	    { echo "TSan target requires Clang"; exit 1; }
	@mkdir -p $(TSAN_DIR) $(BUILD_TESTS_DIR)
	clang $(CFLAGS_TSAN) $(INCLUDES) -c src/strand_context.c  -o $(TSAN_DIR)/strand_context.o
	clang $(CFLAGS_TSAN) $(INCLUDES) -c src/strand_fiber.c    -o $(TSAN_DIR)/strand_fiber.o
	clang $(CFLAGS_TSAN) $(INCLUDES) -c src/strand_sched.c    -o $(TSAN_DIR)/strand_sched.o
	clang $(CFLAGS_TSAN) $(INCLUDES) -c src/strand_poller.c   -o $(TSAN_DIR)/strand_poller.o
	clang $(CFLAGS_TSAN) $(INCLUDES) -c src/strand_inject.c   -o $(TSAN_DIR)/strand_inject.o
	clang $(CFLAGS_TSAN) $(INCLUDES) -c src/strand_runtime.c  -o $(TSAN_DIR)/strand_runtime.o
	clang $(CFLAGS_TSAN) $(INCLUDES) -c src/strand_offload.c  -o $(TSAN_DIR)/strand_offload.o
	clang $(CFLAGS_TSAN) $(INCLUDES) -c src/strand_scope.c    -o $(TSAN_DIR)/strand_scope.o
	test -z "$(ASM_SRC)" || clang $(CFLAGS_TSAN) -c $(ASM_SRC) -o $(ASM_OBJ_TSAN)
	ar rcs $(LIB_TSAN)                                              \
	    $(TSAN_DIR)/strand_context.o                                \
	    $(TSAN_DIR)/strand_fiber.o                                  \
	    $(TSAN_DIR)/strand_sched.o                                  \
	    $(TSAN_DIR)/strand_poller.o                                 \
	    $(TSAN_DIR)/strand_inject.o                                 \
	    $(TSAN_DIR)/strand_runtime.o                                \
	    $(TSAN_DIR)/strand_offload.o                                \
	    $(TSAN_DIR)/strand_scope.o
	test -z "$(ASM_OBJ_TSAN)" || ar qs $(LIB_TSAN) $(ASM_OBJ_TSAN)
	clang $(CFLAGS_TSAN) $(INCLUDES) -I tests/ -I src/              \
	    tests/run_tests.c tests/test_layer1.c tests/test_layer2.c tests/test_layer3.c $(LIB_TSAN) $(LDFLAGS) \
	    -o $(TEST_BIN_TSAN)
	TSAN_OPTIONS=die_after_fork=0 $(TEST_BIN_TSAN)

# Valgrind — Linux only, no sanitizers (ASan + Valgrind conflict)
valgrind: $(TEST_BIN_VG)
	# --child-silent-after-fork=yes: intentional — Valgrind re-instruments any
	# forked child process and then crashes (SIGSEGV in the tracer) when it
	# walks the PROT_NONE fiber guard pages.  Silencing child output avoids
	# spurious "Invalid read" / leak reports from those short-lived children
	# without hiding any real errors in the parent under test.
	valgrind --leak-check=full			\
	         --show-leak-kinds=all			\
	         --track-origins=yes			\
	         --child-silent-after-fork=yes		\
	         --error-exitcode=1			\
	         $(TEST_BIN_VG)
	rm -f vgcore.*

# Benchmarks — Phase 7
bench:
	@echo "No benchmarks yet — implement Phase 7"

# clang-tidy + cppcheck
lint: $(LIB_DEV)
	clang-tidy $(LIB_SRCS) -- $(CFLAGS_DEV) $(INCLUDES)
	@if command -v cppcheck >/dev/null 2>&1; then \
		cppcheck --enable=all --error-exitcode=1 \
		         --suppress=missingIncludeSystem \	         --suppress=unusedFunction \	         --suppress=constParameterPointer \	         --suppress=staticFunction \	         --suppress=normalCheckLevelMaxBranches \		         src/; \
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

$(LIB_DEV): $(LIB_SRCS) $(ASM_SRC)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS_DEV) $(INCLUDES) -c src/strand_context.c  -o $(BUILD_DIR)/strand_context.o
	$(CC) $(CFLAGS_DEV) $(INCLUDES) -c src/strand_fiber.c    -o $(BUILD_DIR)/strand_fiber.o
	$(CC) $(CFLAGS_DEV) $(INCLUDES) -c src/strand_sched.c    -o $(BUILD_DIR)/strand_sched.o
	$(CC) $(CFLAGS_DEV) $(INCLUDES) -c src/strand_poller.c   -o $(BUILD_DIR)/strand_poller.o
	$(CC) $(CFLAGS_DEV) $(INCLUDES) -c src/strand_inject.c   -o $(BUILD_DIR)/strand_inject.o
	$(CC) $(CFLAGS_DEV) $(INCLUDES) -c src/strand_runtime.c  -o $(BUILD_DIR)/strand_runtime.o
	$(CC) $(CFLAGS_DEV) $(INCLUDES) -c src/strand_offload.c  -o $(BUILD_DIR)/strand_offload.o
	$(CC) $(CFLAGS_DEV) $(INCLUDES) -c src/strand_scope.c    -o $(BUILD_DIR)/strand_scope.o
	test -z "$(ASM_SRC)" || $(CC) $(CFLAGS_DEV) -c $(ASM_SRC) -o $(ASM_OBJ_DEV)
	ar rcs $(LIB_DEV)					\
	    $(BUILD_DIR)/strand_context.o			\
	    $(BUILD_DIR)/strand_fiber.o				\
	    $(BUILD_DIR)/strand_sched.o				\
	    $(BUILD_DIR)/strand_poller.o			\
	    $(BUILD_DIR)/strand_inject.o			\
	    $(BUILD_DIR)/strand_runtime.o			\
	    $(BUILD_DIR)/strand_offload.o			\
	    $(BUILD_DIR)/strand_scope.o
	test -z "$(ASM_OBJ_DEV)" || ar qs $(LIB_DEV) $(ASM_OBJ_DEV)

# --- Release library (explicit per-file compile + ar) -----------------------

$(LIB_RELEASE): $(LIB_SRCS) $(ASM_SRC)
	@mkdir -p $(REL_DIR)
	$(CC) $(CFLAGS_RELEASE) $(INCLUDES) -c src/strand_context.c  -o $(REL_DIR)/strand_context.o
	$(CC) $(CFLAGS_RELEASE) $(INCLUDES) -c src/strand_fiber.c    -o $(REL_DIR)/strand_fiber.o
	$(CC) $(CFLAGS_RELEASE) $(INCLUDES) -c src/strand_sched.c    -o $(REL_DIR)/strand_sched.o
	$(CC) $(CFLAGS_RELEASE) $(INCLUDES) -c src/strand_poller.c   -o $(REL_DIR)/strand_poller.o
	$(CC) $(CFLAGS_RELEASE) $(INCLUDES) -c src/strand_inject.c   -o $(REL_DIR)/strand_inject.o
	$(CC) $(CFLAGS_RELEASE) $(INCLUDES) -c src/strand_runtime.c  -o $(REL_DIR)/strand_runtime.o
	$(CC) $(CFLAGS_RELEASE) $(INCLUDES) -c src/strand_offload.c  -o $(REL_DIR)/strand_offload.o
	$(CC) $(CFLAGS_RELEASE) $(INCLUDES) -c src/strand_scope.c    -o $(REL_DIR)/strand_scope.o
	test -z "$(ASM_SRC)" || $(CC) $(CFLAGS_RELEASE) -c $(ASM_SRC) -o $(ASM_OBJ_REL)
	ar rcs $(LIB_RELEASE)					\
	    $(REL_DIR)/strand_context.o				\
	    $(REL_DIR)/strand_fiber.o				\
	    $(REL_DIR)/strand_sched.o				\
	    $(REL_DIR)/strand_poller.o				\
	    $(REL_DIR)/strand_inject.o				\
	    $(REL_DIR)/strand_runtime.o				\
	    $(REL_DIR)/strand_offload.o				\
	    $(REL_DIR)/strand_scope.o
	test -z "$(ASM_OBJ_REL)" || ar qs $(LIB_RELEASE) $(ASM_OBJ_REL)

# --- Test binary (ASan/UBSan build) -----------------------------------------

$(TEST_BIN): $(LIB_DEV) tests/run_tests.c tests/test_layer1.c tests/test_layer2.c tests/test_layer3.c tests/test_harness.h
	@mkdir -p $(BUILD_TESTS_DIR)
	$(CC) $(CFLAGS_DEV) $(INCLUDES) -I tests/ -I src/		\
	    tests/run_tests.c tests/test_layer1.c tests/test_layer2.c tests/test_layer3.c $(LIB_DEV) $(LDFLAGS)	\
	    -o $(TEST_BIN)

# --- Valgrind test binary (no sanitizers) -----------------------------------

$(TEST_BIN_VG): $(LIB_VG) tests/run_tests.c tests/test_layer1.c tests/test_layer2.c tests/test_layer3.c tests/test_harness.h
	@mkdir -p $(BUILD_TESTS_DIR)
	$(CC) $(CFLAGS_VG) $(INCLUDES) -I tests/ -I src/		\
	    tests/run_tests.c tests/test_layer1.c tests/test_layer2.c tests/test_layer3.c $(LIB_VG) $(LDFLAGS)	\
	    -o $(TEST_BIN_VG)

# --- Valgrind library (no sanitizers) ---------------------------------------

$(LIB_VG): $(LIB_SRCS) $(ASM_SRC)
	@mkdir -p $(VG_DIR)
	$(CC) $(CFLAGS_VG) $(INCLUDES) -c src/strand_context.c  -o $(VG_DIR)/strand_context.o
	$(CC) $(CFLAGS_VG) $(INCLUDES) -c src/strand_fiber.c    -o $(VG_DIR)/strand_fiber.o
	$(CC) $(CFLAGS_VG) $(INCLUDES) -c src/strand_sched.c    -o $(VG_DIR)/strand_sched.o
	$(CC) $(CFLAGS_VG) $(INCLUDES) -c src/strand_poller.c   -o $(VG_DIR)/strand_poller.o
	$(CC) $(CFLAGS_VG) $(INCLUDES) -c src/strand_inject.c   -o $(VG_DIR)/strand_inject.o
	$(CC) $(CFLAGS_VG) $(INCLUDES) -c src/strand_runtime.c  -o $(VG_DIR)/strand_runtime.o
	$(CC) $(CFLAGS_VG) $(INCLUDES) -c src/strand_offload.c  -o $(VG_DIR)/strand_offload.o
	$(CC) $(CFLAGS_VG) $(INCLUDES) -c src/strand_scope.c    -o $(VG_DIR)/strand_scope.o
	test -z "$(ASM_SRC)" || $(CC) $(CFLAGS_VG) -c $(ASM_SRC) -o $(ASM_OBJ_VG)
	ar rcs $(LIB_VG)						\
	    $(VG_DIR)/strand_context.o				\
	    $(VG_DIR)/strand_fiber.o				\
	    $(VG_DIR)/strand_sched.o				\
	    $(VG_DIR)/strand_poller.o				\
	    $(VG_DIR)/strand_inject.o				\
	    $(VG_DIR)/strand_runtime.o				\
	    $(VG_DIR)/strand_offload.o				\
	    $(VG_DIR)/strand_scope.o
	test -z "$(ASM_OBJ_VG)" || ar qs $(LIB_VG) $(ASM_OBJ_VG)
