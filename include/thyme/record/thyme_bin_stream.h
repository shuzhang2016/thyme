#pragma once

#include <cstddef>
#include <cstdint>

namespace thyme {
// TCP helpers for .thyme.bin byte-stream egress (same layout as file).
// Returns connected socket fd (>=0) or -1. connect() is bounded by
// timeout_ms (non-blocking + poll); <=0 means fail immediately if the
// connection cannot be established synchronously.
int ThymeBinTcpConnect(const char* host, uint16_t port, int timeout_ms = 3000);

// Listen, accept one client, return connected fd or -1.
int ThymeBinTcpAcceptOne(uint16_t listen_port, int timeout_sec);

// Copy raw bytes from fd to file until peer closes (PC capture).
bool ThymeBinTcpCaptureToFile(uint16_t listen_port, const char* out_path,
                              int timeout_sec);

// Parse "host:port" (IPv4). Returns false on parse error.
bool ThymeBinParseHostPort(const char* spec, char* host_out, std::size_t host_cap,
                           uint16_t* port_out);

}  // namespace thyme
