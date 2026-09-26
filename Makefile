.DEFAULT_GOAL := all
CC ?= cc
BUILD ?= build
MODE ?= release
CLANG_FORMAT ?= $(if $(wildcard tmp/clang-format/bin/clang-format),tmp/clang-format/bin/clang-format,clang-format)
CPPFLAGS += -Isrc
WARN := -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wstrict-prototypes
BASE := -std=c17 -pthread $(WARN)
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
  BASE += -mmacosx-version-min=11.0
  LTO ?= -flto
endif
ifeq ($(MODE),debug)
  OPT := -O0 -g3
else ifeq ($(MODE),sanitize)
  OPT := -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined
  LDFLAGS += -fsanitize=address,undefined
else
  OPT := -O3 -DNDEBUG $(LTO)
  LDFLAGS += $(LTO)
endif
CFLAGS ?= $(BASE) $(OPT)
LDFLAGS += -pthread
SOURCES := $(wildcard src/*.c)
OBJECTS := $(patsubst src/%.c,$(BUILD)/%.o,$(SOURCES))
VANTAGE_SOURCES := $(wildcard cli/*.c)
VANTAGE_OBJECTS := $(patsubst cli/%.c,$(BUILD)/vantage-%.o,$(VANTAGE_SOURCES))
PREFIX ?= /usr/local

.PHONY: all clean test test-mac check format format-check sanitize debug test-mock install-vantage app install-app FORCE
all: $(BUILD)/vantage
$(BUILD):
	mkdir -p $@
FORCE:
$(BUILD)/settings: FORCE | $(BUILD)
	@printf '%s\n' '$(CC)' '$(CPPFLAGS)' '$(CFLAGS)' '$(LDFLAGS)' > $(BUILD)/settings.tmp
	@cmp -s $(BUILD)/settings.tmp $@ || cp $(BUILD)/settings.tmp $@
	@rm -f $(BUILD)/settings.tmp
$(BUILD)/%.o: src/%.c src/scan.h src/attrs.h Makefile $(BUILD)/settings | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@
$(BUILD)/vantage-%.o: cli/%.c cli/vantage.h src/scan.h Makefile $(BUILD)/settings | $(BUILD)
	$(CC) $(CPPFLAGS) -Icli $(CFLAGS) -MMD -MP -c $< -o $@
$(BUILD)/vantage: $(VANTAGE_OBJECTS) $(OBJECTS)
	$(CC) $(VANTAGE_OBJECTS) $(OBJECTS) $(LDFLAGS) -o $@
app:
	mac/build-app.sh "$(BUILD)"
install-app: app
	-osascript -e 'quit app id "dev.cyr1en.Vantage"' 2>/dev/null
	rm -rf /Applications/Vantage.app
	ditto "$(BUILD)/Vantage.app" /Applications/Vantage.app
install-vantage: $(BUILD)/vantage
	install -d "$(DESTDIR)$(PREFIX)/bin"
	install -m 755 $(BUILD)/vantage "$(DESTDIR)$(PREFIX)/bin/vantage"
$(BUILD)/unit: tests/unit.c tests/wire.h tests/fixtures/attrs/captured.h $(OBJECTS)
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/unit.c $(OBJECTS) $(LDFLAGS) -o $@
$(BUILD)/vantage-unit: tests/vantage_unit.c cli/vantage.h $(BUILD)/vantage-view.o $(OBJECTS)
	$(CC) $(CPPFLAGS) -Icli $(CFLAGS) tests/vantage_unit.c $(BUILD)/vantage-view.o $(OBJECTS) $(LDFLAGS) -o $@
$(BUILD)/bulk-mock: tests/bulk_mock.c src/walk.c src/scheduler.c src/attrs.c src/collect.c src/util.c src/json.c tests/wire.h tests/darwin_shim.h src/scan.h src/attrs.h src/darwin_api.h Makefile $(BUILD)/settings
	mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) -Itests $(CFLAGS) -DVS_TEST_DARWIN $(filter %.c,$^) $(LDFLAGS) -o $@
test: $(BUILD)/vantage $(BUILD)/unit $(BUILD)/vantage-unit $(BUILD)/bulk-mock
	$(BUILD)/unit
	$(BUILD)/vantage-unit
	$(BUILD)/bulk-mock
	python3 tests/tooling.py
	python3 tests/installer.py
	python3 tests/vantage.py $(BUILD)/vantage
check: test
format:
	python3 tools/format_c.py --clang-format "$(CLANG_FORMAT)"
format-check:
	python3 tools/format_c.py --clang-format "$(CLANG_FORMAT)" --check
test-mac: $(BUILD)/vantage
	. mac/toolchain.sh && VANTAGE_HELPER="$(abspath $(BUILD)/vantage)" swift test --package-path mac -c release
test-mock: $(BUILD)/bulk-mock
	$(BUILD)/bulk-mock
debug:
	$(MAKE) MODE=debug BUILD=build-debug all
sanitize:
	$(MAKE) MODE=sanitize BUILD=build-sanitize test
clean:
	rm -rf build build-debug build-sanitize
-include $(OBJECTS:.o=.d) $(VANTAGE_OBJECTS:.o=.d)
