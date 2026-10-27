// HTTPS for the phone (Mobile API 0.5, decision 2026-10-22): a self-signed certificate made on the device the first
// time, and its SHA-256 fingerprint in the pairing QR. The app trusts exactly that certificate (pinning), so no
// certificate authority, domain name or internet access is needed. The private key never leaves the device
// (tls_key_file, 0600 like the pairing secret) and is never in a config file, log or the repository.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace hm::mobile {

// True when this binary was built with OpenSSL (HM_MOBILE_API_TLS). Packages are; a PC build without libssl-dev
// is not and refuses tls_enabled.
bool tls_supported();

struct TlsIdentity {
  std::string sha256;  // lower-case hex SHA-256 of the certificate (DER): the QR's tls_sha256
  bool created = false;
};

// Loads cert_file + key_file, or creates both when either is missing: EC P-256 key, certificate valid 10 years,
// CN = common_name, subjectAltName = host (IP or DNS name, from public_base_url; skipped when empty).
// Throws when the files exist but do not belong together.
TlsIdentity ensure_tls_identity(const std::filesystem::path& cert_file, const std::filesystem::path& key_file,
                                const std::string& common_name, const std::string& host);

// Host part of http(s)://host[:port][/...]; "" when there is none. IPv6 brackets are removed.
std::string url_host(const std::string& url);

}  // namespace hm::mobile
