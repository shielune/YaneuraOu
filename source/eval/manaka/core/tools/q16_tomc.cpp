#include "q16_mkc.h"
#include <iostream>
int main(int argc, char** argv) {
	if (argc != 3) { std::cerr << "usage: q16_tomc legacy_SYMq16.nn.bin output_SYMq16c.nn.bin\n"; return 2; }
	std::string error;
	if (!q16mkc::make_checksummed(argv[1], argv[2], &error)) { std::cerr << error << "\n"; return 1; }
	return 0;
}
