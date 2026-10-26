CXX ?= clang++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic -pthread -Iinclude
BUILD_DIR := build
OPENSSL_PREFIX ?= /opt/homebrew/opt/openssl@3
OPENSSL_CFLAGS ?= -I$(OPENSSL_PREFIX)/include
OPENSSL_LIBS ?= -L$(OPENSSL_PREFIX)/lib -lssl -lcrypto
BOOST_PREFIX ?= /opt/homebrew/opt/boost
BOOST_CFLAGS ?= -I$(BOOST_PREFIX)/include
BOOST_LIBS ?= -L$(BOOST_PREFIX)/lib -lboost_json
LIBSSH2_PREFIX ?= /opt/homebrew/opt/libssh2
SFTP_CFLAGS ?= -I$(LIBSSH2_PREFIX)/include
SFTP_LIBS ?= -L$(LIBSSH2_PREFIX)/lib -lssh2

LIB_SOURCES := src/protocol.cpp src/server.cpp
APP_SOURCES := src/main.cpp $(LIB_SOURCES)
TEST_SOURCES := tests/protocol_tests.cpp src/protocol.cpp

.PHONY: all test test-server smoke ux-analytics sftp-example clean valgrind valgrind-server

all: $(BUILD_DIR)/tcp_server

valgrind:
	bash scripts/valgrind.sh tests

valgrind-server:
	bash scripts/valgrind.sh server

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/tcp_server: $(APP_SOURCES) $(wildcard include/tcp_interview/*.hpp) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(OPENSSL_CFLAGS) $(APP_SOURCES) $(OPENSSL_LIBS) -o $@

$(BUILD_DIR)/protocol_tests: $(TEST_SOURCES) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(TEST_SOURCES) -o $@

test: $(BUILD_DIR)/protocol_tests
	./$(BUILD_DIR)/protocol_tests

test-server: $(BUILD_DIR)/tcp_server
	python3 tests/server_pool_tests.py

.PHONY: test-tls
test-tls: $(BUILD_DIR)/tcp_server
	OPENSSL=$(OPENSSL_PREFIX)/bin/openssl python3 tests/tls_tests.py

.PHONY: test-pkcs11
test-pkcs11: $(BUILD_DIR)/tcp_server
	python3 tests/pkcs11_tests.py

smoke: $(BUILD_DIR)/tcp_server
	./scripts/smoke_test_tcp.sh

ux-analytics: examples/ux_analytics_pipeline.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(BOOST_CFLAGS) examples/ux_analytics_pipeline.cpp $(BOOST_LIBS) -o $(BUILD_DIR)/ux_analytics_pipeline

sftp-example: examples/sftp_upload_libssh2.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(SFTP_CFLAGS) examples/sftp_upload_libssh2.cpp $(SFTP_LIBS) -o $(BUILD_DIR)/sftp_upload_libssh2

clean:
	rm -rf $(BUILD_DIR)
