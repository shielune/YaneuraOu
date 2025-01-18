.PHONY: build
build:
	docker run --rm -v ./:/src emscripten/emsdk:3.1.43 node script/wasm_build.js k-p