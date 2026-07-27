.PHONY: default

default: compile;

clean:
	rm -rf build

# Formatting targets
clang-format:
	find pstsdk | xargs clang-format -i

clang-format-check:
	find pstsdk | xargs clang-format --dry-run --Werror

compile:
	mkdir -p build;
	cd build && cmake -G Ninja .. && ninja;

.PHONY: test
test: compile
	CTEST_OUTPUT_ON_FAILURE=1 ninja -C build test
