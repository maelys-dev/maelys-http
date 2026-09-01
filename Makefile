SHELL := /bin/sh

VERSION := 0.1.0-dev
ABI_VERSION := 1
CC ?= cc
CXX ?= c++
AR ?= ar
RANLIB ?= ranlib
PREFIX ?= /usr/local
DESTDIR ?=
REQUIRE_MBEDTLS ?= 0
SYSTEM_DIR ?= ../maelys-system
SYSTEM_PIN := c1fa1d4ebf1a33f084239d55af565150d5e51e13
SYSTEM_REQUIRED_VERSION := 0.5.0
SYSTEM_LIB := $(SYSTEM_DIR)/build/release/lib/libmaelys_sys.a

CPPFLAGS += -Iinclude -Isrc -I$(SYSTEM_DIR)/include
CFLAGS ?= -O2
CFLAGS += -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wpedantic -Werror \
	-Wconversion -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wformat=2
CXXFLAGS ?= -O2
CXXFLAGS += -std=c++17 -Wall -Wextra -Wpedantic -Werror

BUILD := build
CORE_OBJECTS := $(BUILD)/common.o $(BUILD)/parser.o $(BUILD)/message.o $(BUILD)/tls.o
CLIENT_OBJECTS := $(BUILD)/client.o $(BUILD)/resolver.o \
	$(BUILD)/resolver_posix.o $(BUILD)/transport_posix.o
TEST_BINS := $(BUILD)/test_parser $(BUILD)/test_conformance $(BUILD)/test_message \
	$(BUILD)/test_client \
	$(BUILD)/test_transport_posix $(BUILD)/test_resolver_internal \
	$(BUILD)/test_tls_provider $(BUILD)/header_cpp

.PHONY: all clean check test sanitizers tsan fuzzers fuzz-libfuzzer install uninstall \
	check-system-pin install-check package check-mbedtls tls-integration \
	install-mbedtls

all: $(BUILD)/libmaelys_http.a $(BUILD)/libmaelys_http_client.a

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: src/%.c src/internal.h include/maelys/http.h \
	include/maelys/http_client.h include/maelys/http_tls.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/transport_posix.o: providers/transport_posix.c src/internal.h \
	src/resolver_internal.h \
	include/maelys/http_transports.h include/maelys/http_client.h \
	include/maelys/http_tls.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/resolver_posix.o: providers/resolver_posix.c src/internal.h \
	src/resolver_internal.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/libmaelys_http.a: $(CORE_OBJECTS)
	ZERO_AR_DATE=1 $(AR) rcs $@ $^
	$(RANLIB) $@

$(BUILD)/libmaelys_http_client.a: $(CLIENT_OBJECTS)
	ZERO_AR_DATE=1 $(AR) rcs $@ $^
	$(RANLIB) $@

$(BUILD)/test_parser: tests/test_parser.c $(BUILD)/libmaelys_http.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(BUILD)/libmaelys_http.a -o $@

$(BUILD)/test_conformance: tests/test_conformance.c $(BUILD)/libmaelys_http.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(BUILD)/libmaelys_http.a -o $@

$(BUILD)/test_message: tests/test_message.c $(BUILD)/libmaelys_http.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(BUILD)/libmaelys_http.a -o $@

$(BUILD)/test_client: tests/test_client.c $(BUILD)/libmaelys_http_client.a \
	$(BUILD)/libmaelys_http.a $(SYSTEM_LIB)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(BUILD)/libmaelys_http_client.a \
		$(BUILD)/libmaelys_http.a $(SYSTEM_LIB) -pthread -o $@

$(BUILD)/test_transport_posix: tests/test_transport_posix.c \
	$(BUILD)/libmaelys_http_client.a $(BUILD)/libmaelys_http.a $(SYSTEM_LIB)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(BUILD)/libmaelys_http_client.a \
		$(BUILD)/libmaelys_http.a $(SYSTEM_LIB) -pthread -o $@

$(BUILD)/test_resolver_internal: tests/test_resolver_internal.c \
	$(BUILD)/libmaelys_http_client.a $(BUILD)/libmaelys_http.a $(SYSTEM_LIB)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(BUILD)/libmaelys_http_client.a \
		$(BUILD)/libmaelys_http.a $(SYSTEM_LIB) -pthread -o $@

$(BUILD)/test_tls_provider: tests/test_tls_provider.c $(BUILD)/libmaelys_http.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(BUILD)/libmaelys_http.a -o $@

$(BUILD)/header_cpp: tests/header_cpp.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< -c -o $(BUILD)/header_cpp.o
	$(CXX) $(BUILD)/header_cpp.o -o $@

$(SYSTEM_LIB):
	$(MAKE) -C $(SYSTEM_DIR) all

test: $(TEST_BINS)
	$(BUILD)/test_parser
	$(BUILD)/test_conformance conformance/response-wire-cases.txt
	$(BUILD)/test_message
	$(BUILD)/test_client
	$(BUILD)/test_transport_posix
	$(BUILD)/test_resolver_internal
	$(BUILD)/test_tls_provider
	$(BUILD)/header_cpp

check: test fuzzers
	./scripts/audit-boundaries.sh
	./scripts/audit-symbols.sh
	./scripts/audit-whitespace.sh
	./scripts/check-fragmentation.sh

fuzzers: $(BUILD)/fuzz_request $(BUILD)/fuzz_response \
	$(BUILD)/fuzz_chunked $(BUILD)/fuzz_smuggling

fuzz-libfuzzer:
	mkdir -p $(BUILD)/libfuzzer
	@for target in request response chunked smuggling; do \
		$(CC) $(CPPFLAGS) -std=c11 -D_POSIX_C_SOURCE=200809L -O1 -g \
			-fno-omit-frame-pointer -fsanitize=fuzzer,address,undefined \
			src/common.c src/parser.c src/message.c src/tls.c \
			fuzz/fuzz_$$target.c -o $(BUILD)/libfuzzer/fuzz_$$target || exit 1; \
		$(BUILD)/libfuzzer/fuzz_$$target -runs=1000 fuzz/corpus/$$target \
			-artifact_prefix=$(BUILD)/libfuzzer/ || exit 1; \
	done

$(BUILD)/fuzz_%: fuzz/fuzz_%.c fuzz/fuzz_driver.c $(BUILD)/libmaelys_http.a
	$(CC) $(CPPFLAGS) $(CFLAGS) fuzz/fuzz_driver.c $< \
		$(BUILD)/libmaelys_http.a -o $@
	$@ fuzz/corpus/$*

sanitizers: $(SYSTEM_LIB)
	rm -rf $(BUILD)/san
	mkdir -p $(BUILD)/san
	$(CC) $(CPPFLAGS) -std=c11 -D_POSIX_C_SOURCE=200809L -O1 -g \
		-fno-omit-frame-pointer -fsanitize=address,undefined \
		src/common.c src/parser.c src/message.c tests/test_parser.c \
		-o $(BUILD)/san/test_parser
	@if [ "$$(uname -s)" = Darwin ]; then leaks=0; else leaks=1; fi; \
		ASAN_OPTIONS=detect_leaks=$$leaks $(BUILD)/san/test_parser
	$(CC) $(CPPFLAGS) -std=c11 -D_POSIX_C_SOURCE=200809L -O1 -g \
		-fno-omit-frame-pointer -fsanitize=address,undefined \
		src/common.c src/parser.c src/message.c tests/test_message.c \
		-o $(BUILD)/san/test_message
	@if [ "$$(uname -s)" = Darwin ]; then leaks=0; else leaks=1; fi; \
		ASAN_OPTIONS=detect_leaks=$$leaks $(BUILD)/san/test_message
	$(CC) $(CPPFLAGS) -std=c11 -D_POSIX_C_SOURCE=200809L -O1 -g \
		-fno-omit-frame-pointer -fsanitize=address,undefined \
		src/common.c src/parser.c src/message.c src/tls.c src/client.c \
		src/resolver.c providers/resolver_posix.c providers/transport_posix.c \
		tests/test_client.c $(SYSTEM_LIB) -pthread \
		-o $(BUILD)/san/test_client
	@if [ "$$(uname -s)" = Darwin ]; then leaks=0; else leaks=1; fi; \
		ASAN_OPTIONS=detect_leaks=$$leaks $(BUILD)/san/test_client
	$(CC) $(CPPFLAGS) -std=c11 -D_POSIX_C_SOURCE=200809L -O1 -g \
		-fno-omit-frame-pointer -fsanitize=address,undefined \
		src/common.c src/parser.c src/message.c src/tls.c src/client.c \
		src/resolver.c providers/resolver_posix.c providers/transport_posix.c \
		tests/test_transport_posix.c \
		$(SYSTEM_LIB) -pthread -o $(BUILD)/san/test_transport_posix
	@if [ "$$(uname -s)" = Darwin ]; then leaks=0; else leaks=1; fi; \
		ASAN_OPTIONS=detect_leaks=$$leaks $(BUILD)/san/test_transport_posix
	$(CC) $(CPPFLAGS) -std=c11 -D_POSIX_C_SOURCE=200809L -O1 -g \
		-fno-omit-frame-pointer -fsanitize=address,undefined \
		src/common.c src/parser.c src/message.c src/tls.c src/client.c \
		src/resolver.c providers/resolver_posix.c providers/transport_posix.c \
		tests/test_resolver_internal.c \
		$(SYSTEM_LIB) -pthread -o $(BUILD)/san/test_resolver_internal
	@if [ "$$(uname -s)" = Darwin ]; then leaks=0; else leaks=1; fi; \
		ASAN_OPTIONS=detect_leaks=$$leaks $(BUILD)/san/test_resolver_internal

tsan: $(SYSTEM_LIB)
	rm -rf $(BUILD)/tsan
	mkdir -p $(BUILD)/tsan
	$(CC) $(CPPFLAGS) -std=c11 -D_POSIX_C_SOURCE=200809L -O1 -g \
		-fno-omit-frame-pointer -fsanitize=thread \
		src/common.c src/parser.c src/message.c src/tls.c src/client.c \
		src/resolver.c providers/resolver_posix.c providers/transport_posix.c \
		tests/test_client.c $(SYSTEM_LIB) -pthread \
		-o $(BUILD)/tsan/test_client
	TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1 \
		$(BUILD)/tsan/test_client
	$(CC) $(CPPFLAGS) -std=c11 -D_POSIX_C_SOURCE=200809L -O1 -g \
		-fno-omit-frame-pointer -fsanitize=thread \
		src/common.c src/parser.c src/message.c src/tls.c src/client.c \
		src/resolver.c providers/resolver_posix.c providers/transport_posix.c \
		tests/test_transport_posix.c \
		$(SYSTEM_LIB) -pthread -o $(BUILD)/tsan/test_transport_posix
	TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1 \
		$(BUILD)/tsan/test_transport_posix

check-mbedtls: all
	@set -e; \
	rm -f $(BUILD)/tls_mbedtls.o $(BUILD)/libmaelys_http_tls_mbedtls.a \
		$(BUILD)/test_tls_mbedtls; \
	cflags="$$(pkg-config --cflags mbedtls mbedx509 mbedcrypto 2>/dev/null || true)"; \
	libs="$$(pkg-config --libs mbedtls mbedx509 mbedcrypto 2>/dev/null || true)"; \
	if [ -z "$$libs" ]; then \
		if [ "$(REQUIRE_MBEDTLS)" = 1 ]; then echo 'ERROR: mbedTLS is required'; exit 1; fi; \
		echo 'SKIP: mbedTLS pkg-config modules unavailable'; exit 0; \
	fi; \
	$(CC) $(CPPFLAGS) $(CFLAGS) $$cflags -c providers/tls_mbedtls.c \
		-o $(BUILD)/tls_mbedtls.o; \
	ZERO_AR_DATE=1 $(AR) rcs $(BUILD)/libmaelys_http_tls_mbedtls.a $(BUILD)/tls_mbedtls.o; \
	$(RANLIB) $(BUILD)/libmaelys_http_tls_mbedtls.a; \
	$(CC) $(CPPFLAGS) $(CFLAGS) $$cflags tests/test_tls_mbedtls.c \
		$(BUILD)/libmaelys_http_tls_mbedtls.a $(BUILD)/libmaelys_http.a \
		$$libs -pthread -o $(BUILD)/test_tls_mbedtls; \
	$(BUILD)/test_tls_mbedtls

tls-integration: check-mbedtls $(SYSTEM_LIB)
	@if ! pkg-config --exists mbedtls mbedx509 mbedcrypto; then \
		if [ "$(REQUIRE_MBEDTLS)" = 1 ]; then \
			echo 'ERROR: Mbed TLS integration dependencies unavailable'; exit 1; \
		fi; \
		echo 'SKIP: Mbed TLS integration dependencies unavailable'; \
	else \
		cflags="$$(pkg-config --cflags mbedtls mbedx509 mbedcrypto)"; \
		libs="$$(pkg-config --libs mbedtls mbedx509 mbedcrypto)"; \
		$(CC) $(CPPFLAGS) $(CFLAGS) $$cflags tests/tls_integration_client.c \
			$(BUILD)/libmaelys_http_tls_mbedtls.a \
			$(BUILD)/libmaelys_http_client.a $(BUILD)/libmaelys_http.a \
			$(SYSTEM_LIB) $$libs -pthread -o $(BUILD)/tls_integration_client && \
		./scripts/test-tls-integration.sh; \
	fi

check-system-pin:
	@test "$$(cat deps/MAELYS_SYSTEM_PIN)" = "$(SYSTEM_PIN)" || \
		{ echo 'recorded maelys-system pin mismatch'; exit 1; }
	@test "$$(sed -n 's/^#define MAELYS_SYS_VERSION "\([^"]*\)"/\1/p' \
		$(SYSTEM_DIR)/include/maelys/sys/version.h)" = "$(SYSTEM_REQUIRED_VERSION)" || \
		{ echo "maelys-system $(SYSTEM_REQUIRED_VERSION) is required"; exit 1; }
	@test "$$(git -C $(SYSTEM_DIR) rev-parse HEAD)" = "$(SYSTEM_PIN)" || \
		{ echo "maelys-system checkout is not the pinned commit $(SYSTEM_PIN)"; exit 1; }

install: all
	install -d $(DESTDIR)$(PREFIX)/include/maelys
	install -m 0644 include/maelys/http.h include/maelys/http_client.h \
		include/maelys/http_tls.h include/maelys/http_tls_modules.h \
		include/maelys/http_transports.h $(DESTDIR)$(PREFIX)/include/maelys/
	install -d $(DESTDIR)$(PREFIX)/lib/pkgconfig
	install -m 0644 $(BUILD)/libmaelys_http.a $(BUILD)/libmaelys_http_client.a \
		$(DESTDIR)$(PREFIX)/lib/
	sed -e 's|@PREFIX@|$(PREFIX)|g' -e 's|@VERSION@|$(VERSION)|g' \
		pkgconfig/maelys-http.pc.in > $(DESTDIR)$(PREFIX)/lib/pkgconfig/maelys-http.pc
	sed -e 's|@PREFIX@|$(PREFIX)|g' -e 's|@VERSION@|$(VERSION)|g' \
		pkgconfig/maelys-http-client.pc.in > \
		$(DESTDIR)$(PREFIX)/lib/pkgconfig/maelys-http-client.pc

install-mbedtls: check-mbedtls install
	install -m 0644 $(BUILD)/libmaelys_http_tls_mbedtls.a \
		$(DESTDIR)$(PREFIX)/lib/
	sed -e 's|@PREFIX@|$(PREFIX)|g' -e 's|@VERSION@|$(VERSION)|g' \
		pkgconfig/maelys-http-tls-mbedtls.pc.in > \
		$(DESTDIR)$(PREFIX)/lib/pkgconfig/maelys-http-tls-mbedtls.pc

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/include/maelys/http.h \
		$(DESTDIR)$(PREFIX)/include/maelys/http_client.h \
		$(DESTDIR)$(PREFIX)/include/maelys/http_tls.h \
		$(DESTDIR)$(PREFIX)/include/maelys/http_tls_modules.h \
		$(DESTDIR)$(PREFIX)/include/maelys/http_transports.h \
		$(DESTDIR)$(PREFIX)/lib/libmaelys_http.a \
		$(DESTDIR)$(PREFIX)/lib/libmaelys_http_client.a \
		$(DESTDIR)$(PREFIX)/lib/libmaelys_http_tls_mbedtls.a \
		$(DESTDIR)$(PREFIX)/lib/pkgconfig/maelys-http.pc \
		$(DESTDIR)$(PREFIX)/lib/pkgconfig/maelys-http-client.pc \
		$(DESTDIR)$(PREFIX)/lib/pkgconfig/maelys-http-tls-mbedtls.pc

install-check: all
	./scripts/install-check.sh

package: check
	./scripts/package-release.sh $(VERSION)

clean:
	rm -rf $(BUILD) dist
