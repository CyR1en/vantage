.DEFAULT_GOAL := all
CC ?= cc
BUILD ?= build
MODE ?= release
CPPFLAGS += -Isrc
WARN := -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wstrict-prototypes
BASE := -std=c17 -pthread $(WARN)
SOURCE_ID := $(shell shasum -a 256 src/*.c src/*.h 2>/dev/null | shasum -a 256 | cut -c1-16)
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
  BASE += -mmacosx-version-min=11.0
  SDK_VERSION := $(shell xcrun --sdk macosx --show-sdk-version 2>/dev/null)
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
LIB_SOURCES := $(filter-out src/main.c,$(SOURCES))
LIB_OBJECTS := $(filter-out $(BUILD)/main.o,$(OBJECTS))
VANTAGE_SOURCES := $(wildcard cli/*.c)
VANTAGE_OBJECTS := $(patsubst cli/%.c,$(BUILD)/vantage-%.o,$(VANTAGE_SOURCES))
PREFIX ?= /usr/local

.PHONY: all clean test test-mac check sanitize debug test-mock install-vantage app install-app FORCE
all: $(BUILD)/scanbench $(BUILD)/vantage
$(BUILD):
	mkdir -p $@
FORCE:
$(BUILD)/settings: FORCE | $(BUILD)
	@printf '%s\n' '$(CC)' '$(CPPFLAGS)' '$(CFLAGS)' '$(LDFLAGS)' '$(SOURCE_ID)' '$(SDK_VERSION)' > $(BUILD)/settings.tmp
	@cmp -s $(BUILD)/settings.tmp $@ || cp $(BUILD)/settings.tmp $@
	@rm -f $(BUILD)/settings.tmp
$(BUILD)/%.o: src/%.c src/scanbench.h src/attrs.h Makefile $(BUILD)/settings | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DSB_BUILD_FLAGS='"$(CFLAGS)"' -DSB_REVISION='"$(SOURCE_ID)"' -DSB_SDK='"$(SDK_VERSION)"' -MMD -MP -c $< -o $@
$(BUILD)/scanbench: $(OBJECTS)
	$(CC) $(OBJECTS) $(LDFLAGS) -o $@
$(BUILD)/vantage-%.o: cli/%.c cli/vantage.h src/scanbench.h Makefile $(BUILD)/settings | $(BUILD)
	$(CC) $(CPPFLAGS) -Icli $(CFLAGS) -MMD -MP -c $< -o $@
$(BUILD)/vantage: $(VANTAGE_OBJECTS) $(LIB_OBJECTS)
	$(CC) $(VANTAGE_OBJECTS) $(LIB_OBJECTS) $(LDFLAGS) -o $@
app:
	mac/build-app.sh "$(BUILD)"
install-app: app
	-osascript -e 'quit app id "dev.scanbench.Vantage"' 2>/dev/null
	rm -rf /Applications/Vantage.app
	ditto "$(BUILD)/Vantage.app" /Applications/Vantage.app
install-vantage: $(BUILD)/vantage
	install -d "$(DESTDIR)$(PREFIX)/bin"
	install -m 755 $(BUILD)/vantage "$(DESTDIR)$(PREFIX)/bin/vantage"
$(BUILD)/unit: tests/unit.c tests/wire.h tests/fixtures/attrs/captured.h $(LIB_OBJECTS)
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/unit.c $(LIB_OBJECTS) $(LDFLAGS) -o $@
$(BUILD)/vantage-unit: tests/vantage_unit.c cli/vantage.h $(BUILD)/vantage-view.o $(LIB_OBJECTS)
	$(CC) $(CPPFLAGS) -Icli $(CFLAGS) tests/vantage_unit.c $(BUILD)/vantage-view.o $(LIB_OBJECTS) $(LDFLAGS) -o $@
$(BUILD)/comparison-mock: tests/comparison_mock.c src/main.c $(LIB_OBJECTS)
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/comparison_mock.c $(LIB_OBJECTS) $(LDFLAGS) -o $@
$(BUILD)/catalog-mock: tests/catalog_mock.c src/catalog.c src/attrs.c src/collect.c src/manifest.c src/util.c src/json.c tests/wire.h tests/darwin_shim.h src/scanbench.h src/attrs.h src/darwin_api.h Makefile $(BUILD)/settings
	mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) -Itests $(CFLAGS) -DSB_TEST_DARWIN $(filter %.c,$^) $(LDFLAGS) -o $@
$(BUILD)/bulk-mock: tests/bulk_mock.c src/walk.c src/scheduler.c src/attrs.c src/collect.c src/manifest.c src/util.c src/json.c tests/wire.h tests/darwin_shim.h src/scanbench.h src/attrs.h src/darwin_api.h Makefile $(BUILD)/settings
	mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) -Itests $(CFLAGS) -DSB_TEST_DARWIN $(filter %.c,$^) $(LDFLAGS) -o $@
test: $(BUILD)/scanbench $(BUILD)/vantage $(BUILD)/unit $(BUILD)/vantage-unit $(BUILD)/catalog-mock $(BUILD)/bulk-mock $(BUILD)/comparison-mock
	$(BUILD)/unit
	$(BUILD)/vantage-unit
	$(BUILD)/catalog-mock
	$(BUILD)/bulk-mock
	$(BUILD)/comparison-mock
	python3 tests/tooling.py
	python3 tests/installer.py
	python3 tests/integration.py $(BUILD)/scanbench
	python3 tests/vantage.py $(BUILD)/vantage
check: test
test-mac: $(BUILD)/vantage
	. mac/toolchain.sh && VANTAGE_HELPER="$(abspath $(BUILD)/vantage)" swift test --package-path mac -c release
test-mock: $(BUILD)/catalog-mock
	$(BUILD)/catalog-mock
debug:
	$(MAKE) MODE=debug BUILD=build-debug all
sanitize:
	$(MAKE) MODE=sanitize BUILD=build-sanitize test
clean:
	rm -rf build build-debug build-sanitize
-include $(OBJECTS:.o=.d) $(VANTAGE_OBJECTS:.o=.d)
