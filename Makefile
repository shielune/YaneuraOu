# Default emscripten image used by `make build`.
# Pinned to 5.0.0 because it is the most recent emsdk that produces
# both web and node variants we trust end-to-end (5.0.5 ships a
# minifier bug — see docs/wasm_eval_results.md).
EMSDK_VERSION ?= 5.0.0

.PHONY: build
build:
	docker run --rm -v ./:/src emscripten/emsdk:$(EMSDK_VERSION) node script/wasm_build.js k-p

# Convenience: build under a single explicit version (still routed through
# script/wasm_build.js, which produces the web/node 2-variant layout).
.PHONY: build-emsdk
build-emsdk:
	@if [ -z "$(VERSION)" ]; then echo "usage: make build-emsdk VERSION=<emsdk-tag>" && exit 1; fi
	docker run --rm -v ./:/src emscripten/emsdk:$(VERSION) node script/wasm_build.js k-p