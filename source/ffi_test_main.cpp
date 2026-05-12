#include "yaneuraou_ffi.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {
std::mutex                g_mu;
std::vector<std::string>  g_lines;
std::atomic<bool>         g_saw_bestmove{false};
std::atomic<bool>         g_saw_readyok{false};
std::atomic<bool>         g_saw_usiok{false};

void on_line(const char* line, void*) {
	std::lock_guard<std::mutex> lk(g_mu);
	std::string s(line);
	g_lines.push_back(s);
	std::fprintf(stderr, "[engine] %s\n", s.c_str());
	if (s.rfind("bestmove", 0) == 0) g_saw_bestmove.store(true);
	if (s == "readyok")              g_saw_readyok.store(true);
	if (s == "usiok")                g_saw_usiok.store(true);
}

bool wait_for(std::atomic<bool>& flag, int timeout_ms) {
	auto start = std::chrono::steady_clock::now();
	while (!flag.load()) {
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
		auto el = std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - start).count();
		if (el > timeout_ms) return false;
	}
	return true;
}
}

int main(int argc, char** argv) {
	if (yaneuraou_init(argc, argv) != 0) {
		std::fprintf(stderr, "yaneuraou_init failed\n");
		return 1;
	}
	yaneuraou_set_recv(on_line, nullptr);

	yaneuraou_send("usi");
	if (!wait_for(g_saw_usiok, 5000)) {
		std::fprintf(stderr, "timeout waiting usiok\n");
		return 2;
	}

	yaneuraou_send("setoption name USI_Hash value 16");
	yaneuraou_send("setoption name Threads value 1");
	yaneuraou_send("setoption name EvalDir value ../build/eval");
	yaneuraou_send("isready");
	if (!wait_for(g_saw_readyok, 30000)) {
		std::fprintf(stderr, "timeout waiting readyok\n");
		return 3;
	}

	yaneuraou_send("usinewgame");
	yaneuraou_send("position startpos");
	yaneuraou_send("go depth 10");
	if (!wait_for(g_saw_bestmove, 60000)) {
		std::fprintf(stderr, "timeout waiting bestmove\n");
		return 4;
	}

	yaneuraou_send("quit");
	std::this_thread::sleep_for(std::chrono::milliseconds(200));
	yaneuraou_shutdown();

	std::fprintf(stderr, "\n=== FFI test OK ===\n");
	return 0;
}
