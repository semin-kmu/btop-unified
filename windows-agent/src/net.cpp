#include "collect.hpp"
#include <winsock2.h>
#include <ws2ipdef.h>
#include <iphlpapi.h>
#include <cstdio>
#include <map>

namespace {
struct Prev {
	uint64_t rx, tx;
};
std::map<uint64_t, Prev> prev;  // by LUID
uint64_t prev_t = 0;

// adapters listed by GetAdaptersAddresses (real ones, no filter/miniport duplicates) -> first IPv4
std::map<uint64_t, std::string> adapters() {
	std::map<uint64_t, std::string> r;
	ULONG sz = 16 * 1024;
	std::vector<std::byte> buf;
	ULONG rc;
	do {
		buf.resize(sz);
		rc = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr,
		                          reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &sz);
	} while (rc == ERROR_BUFFER_OVERFLOW);
	if (rc != NO_ERROR) return r;
	for (auto a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()); a; a = a->Next) {
		if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
		char ip[16] = "";
		for (auto u = a->FirstUnicastAddress; u && !ip[0]; u = u->Next)
			if (u->Address.lpSockaddr->sa_family == AF_INET) {
				auto& b = reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr)->sin_addr.S_un.S_un_b;
				snprintf(ip, sizeof ip, "%u.%u.%u.%u", b.s_b1, b.s_b2, b.s_b3, b.s_b4);
			}
		r[a->Luid.Value] = ip;
	}
	return r;
}
}  // namespace

void net_get(std::string& o) {
	auto ad = adapters();
	MIB_IF_TABLE2* t = nullptr;
	if (GetIfTable2(&t) != NO_ERROR) return;
	uint64_t now = now_100ns();
	double dt = prev_t ? static_cast<double>(now - prev_t) / 1e7 : 0;
	std::map<uint64_t, Prev> cur;
	for (ULONG i = 0; i < t->NumEntries; i++) {
		const auto& r = t->Table[i];
		auto it = ad.find(r.InterfaceLuid.Value);
		if (it == ad.end() || r.InterfaceAndOperStatusFlags.FilterInterface || r.Type == IF_TYPE_SOFTWARE_LOOPBACK) continue;
		// virtual adapters (Wi-Fi Direct, Bluetooth PAN, ...) only while up; vEthernet (WSL) is up
		if (!r.InterfaceAndOperStatusFlags.HardwareInterface && r.OperStatus != IfOperStatusUp) continue;
		uint64_t rx = r.InOctets, tx = r.OutOctets, rxs = 0, txs = 0;
		if (auto p = prev.find(r.InterfaceLuid.Value); p != prev.end() && dt > 0) {
			rxs = rx >= p->second.rx ? static_cast<uint64_t>(static_cast<double>(rx - p->second.rx) / dt) : 0;
			txs = tx >= p->second.tx ? static_cast<uint64_t>(static_cast<double>(tx - p->second.tx) / dt) : 0;
		}
		cur[r.InterfaceLuid.Value] = {rx, tx};
		o += "net\t" + clean(utf8(r.Alias)) + "\t" + it->second + "\t" + (r.OperStatus == IfOperStatusUp ? "1" : "0") + "\t"
		     + std::to_string(rxs) + "\t" + std::to_string(txs) + "\t" + std::to_string(rx) + "\t" + std::to_string(tx) + "\n";
	}
	FreeMibTable(t);
	prev = std::move(cur);
	prev_t = now;
}
