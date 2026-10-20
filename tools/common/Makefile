# tools/common/: code shared by the chef-cyclo daemons and their clients,
# compiled from source by each consumer (as ../qrtr/qmi.c is); no
# archive and no phone binary of its own. chef_log.{h,c} is the one log
# call (syslog, kmsg when syslogd cannot take a line), linesrv.{h,c}
# the server side of a local socket (listener, client table, line framing,
# bounded output queues), lineclient.{h,c} the client side (a connection
# with framing and backoff, driven by the caller's loop and clock) and
# json.{h,c} json_escape() plus the strict JSON line reader for clients.
# Each file header documents its interface.
#
#   make test            host tests (build/host/tests/), each also under
#                        ASan/UBSan
#
# The root Makefile runs this suite and cleans build/, but builds nothing
# here for the phone.

CC ?= cc
CFLAGS ?= -std=gnu11 -Wall -Wextra -Werror -O2 -g
LDFLAGS ?=
H := build/host
# No test reaches the host's syslog: chef_log() finds no
# socket at this path and writes the kmsg path each test sets instead.
export CHEF_LOG_SOCKET := /nonexistent/chef-log-socket
T := $(H)/tests
SAN := -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all

TESTS := test-json test-linesrv test-lineclient test-chef-log
TEST_OUT := $(addprefix $(T)/,$(TESTS) $(TESTS:%=%-san))

.PHONY: all test clean

all: $(TEST_OUT)

$(T)/test-json $(T)/test-json-san: tests/test_json.c json.c json.h
$(T)/test-linesrv $(T)/test-linesrv-san: tests/test_linesrv.c linesrv.c linesrv.h chef_log.c chef_log.h
$(T)/test-lineclient $(T)/test-lineclient-san: tests/test_lineclient.c lineclient.c lineclient.h
$(T)/test-chef-log $(T)/test-chef-log-san: tests/test_chef_log.c chef_log.c chef_log.h

$(T)/test-json $(T)/test-json-san: SRC := tests/test_json.c json.c
$(T)/test-linesrv $(T)/test-linesrv-san: SRC := tests/test_linesrv.c linesrv.c chef_log.c
$(T)/test-lineclient $(T)/test-lineclient-san: SRC := tests/test_lineclient.c lineclient.c
$(T)/test-chef-log $(T)/test-chef-log-san: SRC := tests/test_chef_log.c chef_log.c

$(addprefix $(T)/,$(TESTS)):
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -I. -o $@ $(SRC) $(LDFLAGS)

$(addprefix $(T)/,$(TESTS:%=%-san)):
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(SAN) -I. -o $@ $(SRC) $(LDFLAGS)

test: $(TEST_OUT)
	@set -e; for t in $(TEST_OUT); do echo $$t; $$t; done

clean:
	rm -rf build
