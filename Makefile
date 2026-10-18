# chef-cyclo: one entry point for every component build (docs/building.md).
#
#   make device          every phone binary, each component into its own
#                        build/device/ (flags in tools/device.mk)
#   make install-device DESTDIR=DIR   them all into DIR/usr/bin
#                        (scripts/mkinitramfs.sh runs both)
#   make test            every host test suite in parallel; each suite's
#                        output in build/test-logs/<suite>.log, one pass or
#                        FAIL line per suite with its wall time, and a
#                        non-zero exit if any failed
#   make clean           every component's build/ and build/ here
#
# The suites run as make prerequisites, never as shell `&` jobs: those
# start with SIGINT and SIGQUIT ignored, which the chef-reboot tests
# depend on not being.

COMPONENTS := tools tools/rmtfs tools/servreg-locator tools/tftp tools/sensord tools/blesensd tools/chefui
SUITES := $(COMPONENTS) scripts/tests
LOGS := build/test-logs
# Nothing is written into the source tree, not even Python's bytecode cache.
export PYTHONDONTWRITEBYTECODE := 1

.PHONY: all device install-device test clean \
	$(COMPONENTS:%=%.device) $(COMPONENTS:%=%.install) $(COMPONENTS:%=%.clean) $(SUITES:%=%.test)

all: device

device: $(COMPONENTS:%=%.device)

$(COMPONENTS:%=%.device): %.device:
	$(MAKE) -C $* device

install-device: device
	@test -n "$(DESTDIR)" || { echo "install-device needs DESTDIR=<staged root>" >&2; exit 1; }
	@for c in $(COMPONENTS); do $(MAKE) -C $$c install-device DESTDIR="$(abspath $(DESTDIR))" || exit; done

# Each suite records its result instead of failing, so every suite runs
# and the summary below sees them all.
test:
	@rm -rf $(LOGS) && mkdir -p $(LOGS)
	@$(MAKE) -j$(words $(SUITES)) $(SUITES:%=%.test)
	@cat $(foreach s,$(SUITES),$(LOGS)/$(subst /,_,$(s)).result)
	@! grep -q FAIL $(LOGS)/*.result

$(SUITES:%=%.test): %.test:
	@mkdir -p $(LOGS); start=$$(date +%s.%N); \
	if $(if $(filter scripts/tests,$*),python3 -m unittest discover scripts/tests,$(MAKE) -C $* test) \
		> $(LOGS)/$(subst /,_,$*).log 2>&1; then r=pass; else r=FAIL; fi; \
	awk -v r=$$r -v s=$* -v t0=$$start -v t1=$$(date +%s.%N) -v out=$(LOGS)/$(subst /,_,$*).log \
		'BEGIN { printf "%-4s %-22s %7.1f s  %s\n", r, s, t1 - t0, out }' > $(LOGS)/$(subst /,_,$*).result

$(COMPONENTS:%=%.clean): %.clean:
	$(MAKE) -C $* clean

clean: $(COMPONENTS:%=%.clean)
	rm -rf build
