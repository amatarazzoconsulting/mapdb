CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic -I.
LDFLAGS  ?=
LDLIBS   ?= -pthread

BIN := bin
TESTS := $(BIN)/test_mapdb
DEMO  := $(BIN)/crm_demo
DEMO2 := $(BIN)/advanced_demo

.PHONY: all test test-sanitize demo docs-check clean

all: test docs-check demo

demo: $(DEMO) $(DEMO2)

$(BIN):
	@mkdir -p $(BIN)

$(TESTS): tests/test_mapdb.cpp mapdb.hpp | $(BIN)
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS) $(LDLIBS)

$(DEMO): examples/crm_demo.cpp mapdb.hpp | $(BIN)
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS) $(LDLIBS)

$(DEMO2): examples/advanced_demo.cpp mapdb.hpp | $(BIN)
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS) $(LDLIBS)

test: $(TESTS)
	@cd $(BIN) && ./test_mapdb

test-sanitize: | $(BIN)
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -I. \
		-fsanitize=address,undefined -fno-omit-frame-pointer \
		tests/test_mapdb.cpp -o $(BIN)/test_mapdb_san $(LDLIBS)
	@cd $(BIN) && ./test_mapdb_san

run-demo: demo
	@cd $(BIN) && ./crm_demo

docs-check: | $(BIN)
	@tools/check_readme.sh

clean:
	rm -rf $(BIN)
