default: debug

debug:
	cmake --preset debug
	cmake --build --preset debug
	ln -sf build/debug/compile_commands.json

release:
	cmake --preset release
	cmake --build --preset release
	ln -sf build/release/compile_commands.json

run: debug
	./build/debug/fenriz-desktop

install: release
	sudo cmake --install build/release

package: release
	cd build/release && cpack

test: debug
	ctest --test-dir build/debug --output-on-failure

# Render a styled surface to a PNG without restarting the session.
#   make preview CLASS=fenriz-launcher OUT=/tmp/card.png
#   make preview CLASS=osd-pill OUT=/tmp/pill.png ARGS="--content pill --size 340x120"
CLASS ?= fenriz-launcher
OUT   ?= /tmp/fenriz-preview.png
ARGS  ?=
preview: debug
	@if [ -r ../scripts/nested.sh ] && [ -x ../build/debug/fenriz ]; then \
	    cd .. && . ./scripts/nested.sh && \
	    fenriz_boot /tmp/fenriz-preview.log headless "$$PWD/build/debug/fenriz" >/dev/null 2>&1 && \
	    WAYLAND_DISPLAY=$$FENRIZ_DISPLAY ./desktop/build/debug/desktop_preview $(CLASS) $(OUT) $(ARGS); \
	    rc=$$?; fenriz_kill; exit $$rc; \
	else \
	    echo "no ../build/debug/fenriz — rendering against \$$WAYLAND_DISPLAY instead (a window will flash)"; \
	    ./build/debug/desktop_preview $(CLASS) $(OUT) $(ARGS); \
	fi

test-polkit:
	@echo "Triggering a polkit prompt..."
	pkexec --user root id

.PHONY: fmt
fmt:
	@echo "Formatting code with clang-format..."
	@find ./src \( -name "*.cpp" -o -name "*.hpp" \) -print0 | xargs -0 -n 1 clang-format -i
	@echo "Done."

clean:
	rm -rf build
	rm -f compile_commands.json
