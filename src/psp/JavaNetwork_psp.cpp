// JavaNetwork_psp.cpp — the PSP build has no network (NO_NETWORK); everything fails cleanly.
#ifdef PSP_PLATFORM
#include "java/JavaNetwork.h"
#include <istream>
#include <ostream>
#include <sstream>

namespace JavaNetwork
{
namespace
{
class NullSocket final : public Socket
{
public:
	bool connect(const std::string &, int) override { return false; }
	int read(char *, int) override { return -1; }
	bool write(const char *, int) override { return false; }
	bool flush() override { return false; }
	void close() override {}
	std::string getRemoteSocketAddress() const override { return std::string(); }
};
} // namespace

std::unique_ptr<Socket> createSocket() { return std::make_unique<NullSocket>(); }
std::unique_ptr<std::istream> createInputStream(Socket &) { return std::make_unique<std::istringstream>(); }
std::unique_ptr<std::ostream> createOutputStream(Socket &) { return std::make_unique<std::ostringstream>(); }
bool readUrl(const std::string &, std::vector<unsigned char> &) { return false; }
int getResponseCode(const std::string &) { return -1; }
bool postUrl(const std::string &, const std::string &, const std::string &, std::vector<unsigned char> &) { return false; }
}
#endif
