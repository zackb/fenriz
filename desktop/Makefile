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
	cmake --install build/release

test: debug
	ctest --test-dir build/debug --output-on-failure

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
