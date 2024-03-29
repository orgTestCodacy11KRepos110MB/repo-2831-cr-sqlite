COMMIT=$(shell git rev-parse HEAD)
VERSION=$(shell cat VERSION)
DATE=$(shell date +'%FT%TZ%z')
CC:=gcc

LOADABLE_CFLAGS=-std=c99 -fPIC -shared -Wall

ifeq ($(shell uname -s),Darwin)
CONFIG_DARWIN=y
else ifeq ($(OS),Windows_NT)
CONFIG_WINDOWS=y
else
CONFIG_LINUX=y
endif

ifdef CONFIG_DARWIN
LOADABLE_EXTENSION=dylib
endif

ifdef CONFIG_LINUX
LOADABLE_EXTENSION=so
endif

ifdef CONFIG_WINDOWS
LOADABLE_EXTENSION=dll
endif

DEFINE_SQLITE_PATH_DATE=-DSQLITE_PATH_DATE="\"$(DATE)\""
DEFINE_SQLITE_PATH_VERSION=-DSQLITE_PATH_VERSION="\"$(VERSION)\""
DEFINE_SQLITE_PATH_SOURCE=-DSQLITE_PATH_SOURCE="\"$(COMMIT)\""
DEFINE_SQLITE_PATH=$(DEFINE_SQLITE_PATH_DATE) $(DEFINE_SQLITE_PATH_VERSION) $(DEFINE_SQLITE_PATH_SOURCE)

prefix=./dist

TARGET_LOADABLE=$(prefix)/crsqlite.$(LOADABLE_EXTENSION)
TARGET_SQLITE3_EXTRA_C=$(prefix)/sqlite3-extra.c
TARGET_SQLITE3=$(prefix)/sqlite3
TARGET_SQLITE3_VANILLA=$(prefix)/vanilla-sqlite3
TARGET_TEST=$(prefix)/test
TARGET_FUZZ=$(prefix)/fuzz
TARGET_TEST_ASAN=$(prefix)/test-asan


# js/browser/wa-sqlite/Makefile, deps/sqlite/GNUMakefile, core/binding.gyp, core/Makefile
ext_files=src/crsqlite.c \
	src/util.c \
	src/tableinfo.c \
	src/triggers.c \
	src/changes-vtab.c \
	src/changes-vtab-read.c \
	src/changes-vtab-common.c \
	src/changes-vtab-write.c \
	src/ext-data.c \
	src/get-table.c \
	src/seen-peers.c
ext_headers=src/crsqlite.h \
	src/util.h \
	src/tableinfo.h \
	src/triggers.h \
	src/changes-vtab.h \
	src/changes-vtab-read.h \
	src/changes-vtab-common.h \
	src/changes-vtab-write.h \
	src/ext-data.h \
	src/seen-peers.h

$(prefix):
	mkdir -p $(prefix)

clean:
	rm -rf $(prefix)


FORMAT_FILES=$(ext_files) $(ext_headers) ./src/core_init.c
format: $(FORMAT_FILES)
	clang-format -i $(FORMAT_FILES)

loadable: $(TARGET_LOADABLE)
sqlite3: $(TARGET_SQLITE3)
vanilla: $(TARGET_SQLITE3_VANILLA)
test: $(TARGET_TEST)
	$(prefix)/test
# ASAN_OPTIONS=detect_leaks=1
asan: CC=clang
asan: $(TARGET_TEST_ASAN)
	$(TARGET_TEST_ASAN)
correctness: $(TARGET_LOADABLE) FORCE
	cd ../py/correctness && pytest
valgrind: $(TARGET_TEST)
	valgrind $(prefix)/test
analyzer:
	scan-build $(MAKE) clean loadable
ubsan: CC=clang
ubsan: LDLIBS += -lubsan
ubsan: clean $(TARGET_TEST)
	$(prefix)/test
fuzz: $(TARGET_FUZZ)
	$(prefix)/fuzz

rs_lib_dbg_static = ./rs/bundle/target/debug/libcrsql_bundle.a
rs_lib_static_loadable = ./rs/bundle/target/release/libcrsql_bundle.a

$(rs_lib_dbg_static): FORCE
	cd ./rs/bundle && $(rustflags_static) cargo build --features omit_load_extension $(rs_build_flags)

$(rs_lib_static_loadable): FORCE
	cd ./rs/bundle && $(rustflags_static) cargo build --release $(rs_build_flags)

$(rs_lib_dbg_dynamic): FORCE
	cd ./rs/bundle && RUSTFLAGS="-C link-args=-lc" cargo rustc --crate-type cdylib

$(rs_lib_dynamic): FORCE
	cd ./rs/bundle && RUSTFLAGS="-C link-args=-lc" cargo rustc --crate-type cdylib --release

# Build the loadable extension.
$(TARGET_LOADABLE): $(prefix) $(ext_files) $(rs_lib_static_loadable)
	$(CC) -O2 -I./src/ -I./src/sqlite \
	$(LOADABLE_CFLAGS) \
	$(DEFINE_SQLITE_PATH) \
	$(ext_files) $(rs_lib_static_loadable) -o $@

# Build a SQLite CLI that pre-loads cr-sqlite.
# Useful for debugging.
$(TARGET_SQLITE3): $(prefix) $(TARGET_SQLITE3_EXTRA_C) src/sqlite/shell.c $(ext_files)
	$(CC) -g \
	$(DEFINE_SQLITE_PATH) \
	-DSQLITE_THREADSAFE=0 \
	-DSQLITE_OMIT_LOAD_EXTENSION=1 \
	-DSQLITE_EXTRA_INIT=core_init \
	-I./src/ -I./src/sqlite \
	$(TARGET_SQLITE3_EXTRA_C) src/sqlite/shell.c $(ext_files) $(rs_lib_dbg_static) \
	$(LDLIBS) -o $@

# Build a normal SQLite CLI that does not include cr-sqlite.
# cr-sqlite can be laoded in via the `.load` pragma.
# Useful for debugging.
$(TARGET_SQLITE3_VANILLA): $(prefix) src/sqlite/shell.c
	$(CC) -g \
	$(DEFINE_SQLITE_PATH) \
	-DSQLITE_THREADSAFE=0 \
	-I./src/ -I./src/sqlite \
	src/sqlite/sqlite3.c src/sqlite/shell.c \
	-o $@

$(TARGET_SQLITE3_EXTRA_C): src/sqlite/sqlite3.c src/core_init.c
	cat src/sqlite/sqlite3.c src/core_init.c > $@

# run tests
$(TARGET_TEST): $(prefix) $(TARGET_SQLITE3_EXTRA_C) src/tests.c src/*.test.c $(ext_files) $(rs_lib_dbg_static)
	$(CC) -g -Wall \
	$(DEFINE_SQLITE_PATH) \
	-DSQLITE_THREADSAFE=0 \
	-DSQLITE_OMIT_LOAD_EXTENSION=1 \
	-DSQLITE_EXTRA_INIT=core_init \
	-DUNIT_TEST=1 \
	-I./src/ -I./src/sqlite \
	$(TARGET_SQLITE3_EXTRA_C) src/tests.c src/*.test.c $(ext_files) $(rs_lib_dbg_static) \
	$(LDLIBS) -o $@

$(TARGET_TEST_ASAN): $(prefix) $(TARGET_SQLITE3_EXTRA_C) src/tests.c src/*.test.c $(ext_files)
	$(CC) -fsanitize=address -g -fno-omit-frame-pointer -Wall \
	$(DEFINE_SQLITE_PATH) \
	-DSQLITE_THREADSAFE=0 \
	-DSQLITE_OMIT_LOAD_EXTENSION=1 \
	-DSQLITE_EXTRA_INIT=core_init \
	-DUNIT_TEST=1 \
	-I./src/ -I./src/sqlite \
	$(TARGET_SQLITE3_EXTRA_C) src/tests.c src/*.test.c $(ext_files) $(rs_lib_dbg_static) \
	$(LDLIBS) -o $@

$(TARGET_FUZZ): $(prefix) $(TARGET_SQLITE3_EXTRA_C) src/fuzzer.cc $(ext_files)
	clang -fsanitize=fuzzer \
	$(DEFINE_SQLITE_PATH) \
	-DSQLITE_THREADSAFE=0 \
	-DSQLITE_OMIT_LOAD_EXTENSION=1 \
	-DSQLITE_EXTRA_INIT=core_init \
	-I./src/ -I./src/sqlite \
	$(TARGET_SQLITE3_EXTRA_C) src/fuzzer.cc $(ext_files) $(rs_lib_dbg_static) \
	$(LDLIBS) -o $@

.PHONY: all clean format \
	test \
	loadable \
	sqlite3 \
	correctness \
	valgrind \
	ubsan analyzer fuzz asan

FORCE: ;