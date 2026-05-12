#if defined(YANEURAOU_FFI)

#include "yaneuraou_ffi.h"

#include "bitboard.h"
#include "position.h"
#include "search.h"
#include "thread.h"
#include "tt.h"
#include "usi.h"
#include "evaluate.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <unistd.h>
#include <fcntl.h>

extern "C" int usi_command(const char* c_cmd);

namespace {

std::atomic<yaneuraou_recv_cb> g_cb{nullptr};
std::atomic<void*>             g_user{nullptr};
std::atomic<bool>              g_reader_run{false};
std::thread                    g_reader;
int                            g_saved_stdout = -1;
int                            g_pipe_read    = -1;
int                            g_pipe_write   = -1;
FILE*                          g_pipe_file    = nullptr;
std::atomic<bool>              g_inited{false};

void reader_loop() {
	std::string buf;
	char chunk[4096];
	while (g_reader_run.load(std::memory_order_acquire)) {
		ssize_t n = ::read(g_pipe_read, chunk, sizeof(chunk));
		if (n <= 0) {
			if (n < 0 && (errno == EINTR)) continue;
			break;
		}
		buf.append(chunk, chunk + n);
		size_t pos;
		while ((pos = buf.find('\n')) != std::string::npos) {
			std::string line = buf.substr(0, pos);
			buf.erase(0, pos + 1);
			auto cb   = g_cb.load(std::memory_order_acquire);
			auto user = g_user.load(std::memory_order_acquire);
			if (cb) cb(line.c_str(), user);
		}
	}
}

} // namespace

extern "C" int yaneuraou_init(int argc, char** argv) {
	if (g_inited.exchange(true)) return 0;

	int pipefd[2];
	if (::pipe(pipefd) != 0) return -1;
	g_pipe_read  = pipefd[0];
	g_pipe_write = pipefd[1];

	g_saved_stdout = ::dup(STDOUT_FILENO);
	if (::dup2(g_pipe_write, STDOUT_FILENO) < 0) return -2;
	::setvbuf(stdout, nullptr, _IOLBF, 0);

	g_reader_run.store(true, std::memory_order_release);
	g_reader = std::thread(reader_loop);

	Bitboards::init();
	Position::init();

	static int    s_argc = argc;
	static char** s_argv = argv;
	USIEngine engine(s_argc, s_argv);
	(void)engine;

	USI::init(Options);
	Search::init();

	size_t thread_num = Options.count("Threads") ? (size_t)Options["Threads"] : 1;
	Threads.set(thread_num);

	Eval::init();
	return 0;
}

extern "C" void yaneuraou_set_recv(yaneuraou_recv_cb cb, void* user) {
	g_cb.store(cb, std::memory_order_release);
	g_user.store(user, std::memory_order_release);
}

extern "C" int yaneuraou_send(const char* cmd) {
	if (!g_inited.load()) return -1;
	return usi_command(cmd);
}

extern "C" void yaneuraou_shutdown(void) {
	if (!g_inited.exchange(false)) return;

	usi_command("stop");
	Threads.set(0);

	::fflush(stdout);
	if (g_saved_stdout >= 0) {
		::dup2(g_saved_stdout, STDOUT_FILENO);
		::close(g_saved_stdout);
		g_saved_stdout = -1;
	}
	if (g_pipe_write >= 0) { ::close(g_pipe_write); g_pipe_write = -1; }

	g_reader_run.store(false, std::memory_order_release);
	if (g_reader.joinable()) g_reader.join();

	if (g_pipe_read >= 0) { ::close(g_pipe_read); g_pipe_read = -1; }
}

#endif
