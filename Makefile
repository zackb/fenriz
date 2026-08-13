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
	./build/debug/fenriz

install: release
	cmake --install build/release

reinstall: release
	sudo cmake --install build/release
	$(MAKE) -C desktop release
	sudo $(MAKE) -C desktop install

package: release
	cd build/release && cpack

test: debug
	ctest --test-dir build/debug --output-on-failure -LE integration

test-wl: debug
	ctest --test-dir build/debug --output-on-failure -L integration

# fenriz-desktop is a standalone subproject with its own presets; these just forward.
.PHONY: desktop install-desktop run-desktop package-desktop
desktop:
	$(MAKE) -C desktop

install-desktop:
	$(MAKE) -C desktop install

package-desktop:
	$(MAKE) -C desktop package

run-desktop:
	$(MAKE) -C desktop run

.PHONY: fmt
fmt:
	@echo "Formatting code with clang-format..."
	@find ./src ./tests \( -name "*.cpp" -o -name "*.hpp" -o -name "*.c" -o -name "*.h" \) -print0 | xargs -0 -n 1 clang-format -i
	@echo "Done."

clean:
	rm -rf build
	rm -f compile_commands.json
