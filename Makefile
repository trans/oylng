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
HDRS := include/oylng/oylng.h include/oylng/schema.h $(SRCDIR)/oylng_internal.h $(SRCDIR)/oylng_simd.h $(SRCDIR)/oylng_yes.h

LIB  := $(OBJDIR)/liboylng.a
TOOL := $(OBJDIR)/oylng

# The YAML 2.0 suite: trans/yaml2's suite/, beside this checkout by default
SUITE      ?= ../yaml2/suite
SUITE_DATA := $(OBJDIR)/suite-data
YES_DATA   := $(OBJDIR)/yes-data

# The draft and its checker, for test-diff: trans/yaml2, its ai branch
YAML2      ?= ../yaml2
DRAFTCHECK ?= $(YAML2)/.ai/memory/draftcheck.py
DIFF_N     ?= 20000
SCHEMACHECK ?= $(YAML2)/.ai/memory/schemacheck.py

SAN := -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer

.PHONY: all test test-api test-emit test-schema test-suite test-yes test-plain test-diff test-schema-diff test-pattern-diff test-asan bench bench-files fuzz clean

all: $(LIB) $(TOOL)

$(OBJDIR):
	mkdir -p $@

$(OBJDIR)/%.o: $(SRCDIR)/%.c $(HDRS) | $(OBJDIR)
	$(CC) $(OYLNG_CFLAGS) -c $< -o $@

$(LIB): $(OBJS)
	$(AR) rcs $@ $^

$(TOOL): tools/oylng.c tools/events.h include/oylng/schema.h $(LIB)
	$(CC) $(OYLNG_CFLAGS) -Itools $< $(LIB) $(OYLNG_LDFLAGS) -o $@

$(OBJDIR)/test_suite: test/test_suite.c tools/events.h $(LIB)
	$(CC) $(OYLNG_CFLAGS) -Itools $< $(LIB) $(OYLNG_LDFLAGS) -o $@

$(OBJDIR)/test_yes: test/test_yes.c tools/events.h $(LIB)
	$(CC) $(OYLNG_CFLAGS) -Itools $< $(LIB) $(OYLNG_LDFLAGS) -o $@

$(OBJDIR)/test_api: test/test_api.c $(LIB)
	$(CC) $(OYLNG_CFLAGS) $< $(LIB) $(OYLNG_LDFLAGS) -o $@

$(OBJDIR)/test_schema: test/test_schema.c $(LIB)
	$(CC) $(OYLNG_CFLAGS) $< $(LIB) -lm -o $@

$(OBJDIR)/test_emit: test/test_emit.c $(LIB)
	$(CC) $(OYLNG_CFLAGS) $< $(LIB) $(OYLNG_LDFLAGS) -o $@

# Part 2's patterns, for test-pattern-diff: it uses the library's own
$(OBJDIR)/rx_batch: test/rx_batch.c $(LIB)
	$(CC) $(OYLNG_CFLAGS) -I$(SRCDIR) $< $(LIB) $(OYLNG_LDFLAGS) -o $@

$(OBJDIR)/test_batch: test/test_batch.c tools/events.h $(LIB)
	$(CC) $(OYLNG_CFLAGS) -Itools $< $(LIB) $(OYLNG_LDFLAGS) -o $@

# libyaml, if pkg-config finds it, for the benchmark to compare with
LIBYAML := $(shell pkg-config --exists yaml-0.1 2>/dev/null && echo 1)
BENCH_LIBYAML := $(if $(LIBYAML),-DHAVE_LIBYAML $(shell pkg-config --cflags yaml-0.1))
BENCH_LIBS    := $(if $(LIBYAML),$(shell pkg-config --libs yaml-0.1))

$(OBJDIR)/bench_parser: bench/bench_parser.c bench/bench_gen.h $(LIB)
	$(CC) $(OYLNG_CFLAGS) $(BENCH_LIBYAML) -Ibench $< $(LIB) $(OYLNG_LDFLAGS) $(BENCH_LIBS) -o $@

$(OBJDIR)/bench_files: bench/bench_files.c $(LIB)
	$(CC) $(OYLNG_CFLAGS) $(BENCH_LIBYAML) $< $(LIB) $(OYLNG_LDFLAGS) $(BENCH_LIBS) -o $@

# The library built into each test with AddressSanitizer and UBSan
$(OBJDIR)/%_asan: test/%.c $(SRCS) $(HDRS) tools/events.h | $(OBJDIR)
	$(CC) -std=c11 $(WARNINGS) $(SAN) -Iinclude -Itools $(SRCS) $< -o $@

# The library built into each test with plain loops for the SIMD scans
# and without the fast paths, so that what they stand in for stays tested
$(OBJDIR)/%_plain: test/%.c $(SRCS) $(HDRS) tools/events.h | $(OBJDIR)
	$(CC) $(OYLNG_CFLAGS) -DOYLNG_NO_SIMD -DOYLNG_NO_FAST -Itools $(SRCS) $< -o $@

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

# Part 2's suite, suite/yes
$(YES_DATA): $(wildcard $(SUITE)/yes/src/*.yaml) | $(OBJDIR)
	@test -d $(SUITE)/yes/src || { echo "no part 2 suite at $(SUITE)/yes: set SUITE=path/to/yaml2/suite"; exit 1; }
	rm -rf $@
	python3 -I $(SUITE)/bin/suite-to-data $(SUITE)/yes/src $@

test-yes: $(OBJDIR)/test_yes $(YES_DATA)
	$(OBJDIR)/test_yes $(YES_DATA)

test-api: $(OBJDIR)/test_api
	$(OBJDIR)/test_api

test-emit: $(OBJDIR)/test_emit
	$(OBJDIR)/test_emit

# Part 2: the draft's examples, beside the suite
SCHEMA_SPEC ?= $(SUITE)/../spec/yaml-easy-schema.md

test-schema: $(OBJDIR)/test_schema
	$(OBJDIR)/test_schema $(SCHEMA_SPEC)

test: test-api test-emit test-schema test-suite test-yes test-plain

# Both tests again, built for test-plain
test-plain: $(OBJDIR)/test_api_plain $(OBJDIR)/test_emit_plain $(OBJDIR)/test_schema_plain $(OBJDIR)/test_suite_plain \
            $(OBJDIR)/test_yes_plain $(SUITE_DATA) $(YES_DATA)
	$(OBJDIR)/test_api_plain
	$(OBJDIR)/test_emit_plain
	$(OBJDIR)/test_schema_plain $(SCHEMA_SPEC)
	$(OBJDIR)/test_suite_plain $(SUITE_DATA)
	$(OBJDIR)/test_yes_plain $(YES_DATA)

# Both tests again with the sanitizers
test-asan: $(OBJDIR)/test_api_asan $(OBJDIR)/test_emit_asan $(OBJDIR)/test_schema_asan $(OBJDIR)/test_suite_asan \
           $(OBJDIR)/test_yes_asan $(SUITE_DATA) $(YES_DATA)
	$(OBJDIR)/test_api_asan
	$(OBJDIR)/test_emit_asan
	$(OBJDIR)/test_schema_asan $(SCHEMA_SPEC)
	$(OBJDIR)/test_suite_asan $(SUITE_DATA)
	$(OBJDIR)/test_yes_asan $(YES_DATA)

# Mutated and generated streams through this parser and the draft's checker
test-diff: $(OBJDIR)/test_batch
	python3 -I test/diffcheck.py $(DRAFTCHECK) $(YAML2)/spec/yaml-2.0.md $(SUITE) $(OBJDIR)/test_batch -n $(DIFF_N)

# Part 2: the schema checker's examples and edge cases through this parser
test-schema-diff: $(TOOL)
	python3 -I test/schemadiff.py $(SCHEMACHECK) $(SCHEMA_SPEC) $(TOOL)

# Part 2's patterns: generated ones through this parser and the checker's
PATTERN_N ?= 20000
test-pattern-diff: $(OBJDIR)/rx_batch
	python3 -I test/rxdiff.py $(SCHEMACHECK) $(OBJDIR)/rx_batch -n $(PATTERN_N)

# Pinned to one core if BENCH_CPU is set: make bench BENCH_CPU=5
bench: $(OBJDIR)/bench_parser
	$(if $(BENCH_CPU),taskset -c $(BENCH_CPU)) $(OBJDIR)/bench_parser

# Real files, each directory of FILES a group: make bench-files FILES='dir1 dir2'
bench-files: $(OBJDIR)/bench_files
	$(if $(FILES),,$(error set FILES to the directories to read))
	$(if $(BENCH_CPU),taskset -c $(BENCH_CPU)) $(OBJDIR)/bench_files $(FILES)

# One worker, five minutes, seeded with the suite's streams and the schema
# checker's
FUZZ_TIME ?= 300
fuzz: $(OBJDIR)/fuzz_parser $(SUITE_DATA) $(TOOL)
	mkdir -p $(OBJDIR)/fuzz-corpus $(OBJDIR)/fuzz-crashes
	sh fuzz/seed.sh $(SUITE_DATA) $(OBJDIR)/fuzz-corpus
	-test -f $(SCHEMACHECK) && python3 -I test/schemadiff.py $(SCHEMACHECK) $(SCHEMA_SPEC) $(TOOL) --seeds $(OBJDIR)/fuzz-corpus > /dev/null
	cd $(OBJDIR)/fuzz-crashes && ../fuzz_parser -dict=../../fuzz/yaml2.dict -max_len=4096 \
	    -timeout=10 -max_total_time=$(FUZZ_TIME) ../fuzz-corpus

clean:
	rm -rf $(OBJDIR)
