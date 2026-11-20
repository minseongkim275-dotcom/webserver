# 빌드: make            → build/webserver, build/demo-backend
# 검사: make test       → 단위 테스트 + 기능 테스트 (게이트가 실행)
#       make asan       → AddressSanitizer + UBSan으로 같은 테스트 (게이트 밖)
# CFLAGS는 밖에서 바꿔도 되고(예: CFLAGS="-O2 -g -Werror"), 경고 옵션은 WARN에 따로 둔다.

BUILD   ?= build
CC      ?= cc
CFLAGS  ?= -O2 -g
SAN     ?=

STD     := -std=c11 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
WARN    := -Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Wstrict-prototypes \
           -Wmissing-prototypes -Wvla -Wundef
ALL_CFLAGS = $(STD) $(WARN) $(CFLAGS) $(SAN) -Isrc -MMD -MP
LDFLAGS += $(SAN)
LDLIBS  += -lpthread

LIB_SRC  := $(filter-out src/main.c,$(wildcard src/*.c))
LIB_OBJ  := $(LIB_SRC:%.c=$(BUILD)/%.o)
TEST_SRC := $(wildcard tests/test_*.c)
TEST_BIN := $(TEST_SRC:%.c=$(BUILD)/%)

.PHONY: all unit functional test asan clean
.SECONDARY:
all: $(BUILD)/libws.a $(BUILD)/webserver $(BUILD)/demo-backend

$(BUILD)/demo-backend: $(BUILD)/backend/backend.o $(BUILD)/libws.a
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/webserver: $(BUILD)/src/main.o $(BUILD)/libws.a
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/libws.a: $(LIB_OBJ)
	$(AR) rcs $@ $^

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(ALL_CFLAGS) -c -o $@ $<

$(BUILD)/tests/%: $(BUILD)/tests/%.o $(BUILD)/libws.a
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

unit: $(TEST_BIN)
	@fail=0; for t in $(TEST_BIN); do ./$$t || fail=1; done; exit $$fail

functional: all
	BUILD=$(BUILD) bash tests/functional.sh

test: all unit functional

asan:
	$(MAKE) BUILD=build-asan CFLAGS="-O1 -g" \
		SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all" test

clean:
	rm -rf build build-asan

-include $(LIB_OBJ:.o=.d) $(TEST_BIN:=.d) $(BUILD)/src/main.d $(BUILD)/backend/backend.d
