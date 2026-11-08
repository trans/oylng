CC      ?= cc

# Optimization flags, overridable so packagers can supply their own
CFLAGS  ?= -O2

WARNINGS := -Wall -Wextra -Wpedantic

# Flags always applied
OYLNG_CFLAGS  := -std=c11 $(WARNINGS) $(CFLAGS) -Iinclude -fvisibility=hidden
OYLNG_LDFLAGS := $(LDFLAGS)

SRCDIR := src
OBJDIR := build

SRCS := $(wildcard $(SRCDIR)/*.c)
OBJS := $(SRCS:$(SRCDIR)/%.c=$(OBJDIR)/%.o)
HDRS := include/oylng/oylng.h $(SRCDIR)/oylng_internal.h

LIB  := $(OBJDIR)/liboylng.a
TOOL := $(OBJDIR)/oylng

# The YAML 2.0 suite: trans/yaml2's suite/, beside this checkout by default
SUITE      ?= ../yaml2/suite
SUITE_DATA := $(OBJDIR)/suite-data

# The draft and its checker, for test-diff: trans/yaml2, its ai branch
YAML2      ?= ../yaml2
DRAFTCHECK ?= $(YAML2)/.ai/memory/draftcheck.py
DIFF_N     ?= 20000

SAN := -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer

.PHONY: all test test-api test-suite test-diff test-asan bench fuzz clean

all: $(LIB) $(TOOL)

$(OBJDIR):
	mkdir -p $@

$(OBJDIR)/%.o: $(SRCDIR)/%.c $(HDRS) | $(OBJDIR)
	$(CC) $(OYLNG_CFLAGS) -c $< -o $@

$(LIB): $(OBJS)
	$(AR) rcs $@ $^

$(TOOL): tools/oylng.c tools/events.h $(LIB)
	$(CC) $(OYLNG_CFLAGS) -Itools $< $(LIB) $(OYLNG_LDFLAGS) -o $@

$(OBJDIR)/test_suite: test/test_suite.c tools/events.h $(LIB)
	$(CC) $(OYLNG_CFLAGS) -Itools $< $(LIB) $(OYLNG_LDFLAGS) -o $@

$(OBJDIR)/test_api: test/test_api.c $(LIB)
	$(CC) $(OYLNG_CFLAGS) $< $(LIB) $(OYLNG_LDFLAGS) -o $@

$(OBJDIR)/test_batch: test/test_batch.c tools/events.h $(LIB)
	$(CC) $(OYLNG_CFLAGS) -Itools $< $(LIB) $(OYLNG_LDFLAGS) -o $@

# libyaml, if pkg-config finds it, for the benchmark to compare with
LIBYAML := $(shell pkg-config --exists yaml-0.1 2>/dev/null && echo 1)
BENCH_LIBYAML := $(if $(LIBYAML),-DHAVE_LIBYAML $(shell pkg-config --cflags yaml-0.1))
BENCH_LIBS    := $(if $(LIBYAML),$(shell pkg-config --libs yaml-0.1))

$(OBJDIR)/bench_parser: bench/bench_parser.c bench/bench_gen.h $(LIB)
	$(CC) $(OYLNG_CFLAGS) $(BENCH_LIBYAML) -Ibench $< $(LIB) $(OYLNG_LDFLAGS) $(BENCH_LIBS) -o $@

# The library built into each test with AddressSanitizer and UBSan
$(OBJDIR)/%_asan: test/%.c $(SRCS) $(HDRS) tools/events.h | $(OBJDIR)
	$(CC) -std=c11 $(WARNINGS) $(SAN) -Iinclude -Itools $(SRCS) $< -o $@

$(OBJDIR)/fuzz_parser: fuzz/fuzz_parser.c $(SRCS) $(HDRS) | $(OBJDIR)
	clang -std=c11 -g -O1 -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined \
	    -Iinclude $(SRCS) $< -o $@

# Written again whenever the suite's tests change (needs PyYAML)
$(SUITE_DATA): $(wildcard $(SUITE)/src/*.yaml) | $(OBJDIR)
	@test -d $(SUITE)/src || { echo "no suite at $(SUITE): set SUITE=path/to/yaml2/suite"; exit 1; }
	rm -rf $@
	python3 -I $(SUITE)/bin/suite-to-data $(SUITE)/src $@

test-suite: $(OBJDIR)/test_suite $(SUITE_DATA)
	$(OBJDIR)/test_suite $(SUITE_DATA)

test-api: $(OBJDIR)/test_api
	$(OBJDIR)/test_api

test: test-api test-suite

# Both tests again with the sanitizers
test-asan: $(OBJDIR)/test_api_asan $(OBJDIR)/test_suite_asan $(SUITE_DATA)
	$(OBJDIR)/test_api_asan
	$(OBJDIR)/test_suite_asan $(SUITE_DATA)

# Mutated and generated streams through this parser and the draft's checker
test-diff: $(OBJDIR)/test_batch
	python3 -I test/diffcheck.py $(DRAFTCHECK) $(YAML2)/spec/yaml-2.0.md $(SUITE) $(OBJDIR)/test_batch -n $(DIFF_N)

# Pinned to one core if BENCH_CPU is set: make bench BENCH_CPU=5
bench: $(OBJDIR)/bench_parser
	$(if $(BENCH_CPU),taskset -c $(BENCH_CPU)) $(OBJDIR)/bench_parser

# One worker, five minutes, seeded with the suite's streams
FUZZ_TIME ?= 300
fuzz: $(OBJDIR)/fuzz_parser $(SUITE_DATA)
	mkdir -p $(OBJDIR)/fuzz-corpus $(OBJDIR)/fuzz-crashes
	sh fuzz/seed.sh $(SUITE_DATA) $(OBJDIR)/fuzz-corpus
	cd $(OBJDIR)/fuzz-crashes && ../fuzz_parser -dict=../../fuzz/yaml2.dict -max_len=4096 \
	    -timeout=10 -max_total_time=$(FUZZ_TIME) ../fuzz-corpus

clean:
	rm -rf $(OBJDIR)
