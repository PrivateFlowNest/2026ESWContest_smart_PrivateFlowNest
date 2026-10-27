#include "tls.hpp"

#include <stdexcept>

#include "auth.hpp"
#include "hm/common.hpp"

#ifdef HM_MOBILE_API_TLS
#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <memory>
#endif

namespace fs = std::filesystem;

namespace hm::mobile {

std::string url_host(const std::string& url) {
  const auto scheme = url.find("://");
  if (scheme == std::string::npos) return "";
  std::string rest = url.substr(scheme + 3);
  rest = rest.substr(0, rest.find_first_of("/?#"));
  if (const auto at = rest.rfind('@'); at != std::string::npos) rest = rest.substr(at + 1);
  if (!rest.empty() && rest.front() == '[') {
    const auto close = rest.find(']');
    return close == std::string::npos ? "" : rest.substr(1, close - 1);
  }
  return rest.substr(0, rest.find(':'));
}

#ifndef HM_MOBILE_API_TLS

bool tls_supported() { return false; }

TlsIdentity ensure_tls_identity(const fs::path&, const fs::path&, const std::string&, const std::string&) {
  throw std::runtime_error("this build has no TLS support (rebuild with OpenSSL: libssl-dev)");
}

#else

namespace {

template <typename T, void (*Free)(T*)>
struct Deleter {
  void operator()(T* p) const { Free(p); }
};
using Bio = std::unique_ptr<BIO, Deleter<BIO, BIO_free_all>>;
using Key = std::unique_ptr<EVP_PKEY, Deleter<EVP_PKEY, EVP_PKEY_free>>;
using Cert = std::unique_ptr<X509, Deleter<X509, X509_free>>;

void check(bool ok, const char* what) {
  if (!ok) throw std::runtime_error(std::string("TLS: ") + what + " failed");
}

std::string bio_text(BIO* bio) {
  char* data = nullptr;
  const long size = BIO_get_mem_data(bio, &data);
  return std::string(data, static_cast<std::size_t>(size));
}

std::string fingerprint(X509* cert) {
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int size = 0;
  check(X509_digest(cert, EVP_sha256(), digest, &size) == 1, "certificate digest");
  static const char* hex = "0123456789abcdef";
  std::string out;
  for (unsigned int i = 0; i < size; ++i) {
    out.push_back(hex[digest[i] >> 4]);
    out.push_back(hex[digest[i] & 15]);
  }
  return out;
}

void add_extension(X509* cert, int nid, const std::string& value) {
  X509V3_CTX ctx;
  X509V3_set_ctx_nodb(&ctx);
  X509V3_set_ctx(&ctx, cert, cert, nullptr, nullptr, 0);
  X509_EXTENSION* extension = X509V3_EXT_conf_nid(nullptr, &ctx, nid, value.c_str());
  check(extension != nullptr, "certificate extension");
  const int added = X509_add_ext(cert, extension, -1);
  X509_EXTENSION_free(extension);
  check(added == 1, "certificate extension");
}

bool is_ip(const std::string& host) {
  if (host.find(':') != std::string::npos) return true;  // IPv6
  if (host.empty()) return false;
  for (char c : host) {
    if (!(c == '.' || (c >= '0' && c <= '9'))) return false;
  }
  return true;
}

}  // namespace

bool tls_supported() { return true; }

TlsIdentity ensure_tls_identity(const fs::path& cert_file, const fs::path& key_file, const std::string& common_name,
                                const std::string& host) {
  TlsIdentity identity;
  if (fs::exists(cert_file) && fs::exists(key_file)) {
    Bio cert_bio(BIO_new_file(cert_file.c_str(), "r"));
    Bio key_bio(BIO_new_file(key_file.c_str(), "r"));
    check(cert_bio && key_bio, "opening the certificate or key file");
    Cert cert(PEM_read_bio_X509(cert_bio.get(), nullptr, nullptr, nullptr));
    Key key(PEM_read_bio_PrivateKey(key_bio.get(), nullptr, nullptr, nullptr));
    if (!cert || !key) throw std::runtime_error("TLS: " + cert_file.string() + " or its key is not PEM");
    if (X509_check_private_key(cert.get(), key.get()) != 1) {
      throw std::runtime_error("TLS: " + key_file.string() + " is not the key of " + cert_file.string());
    }
    identity.sha256 = fingerprint(cert.get());
    return identity;
  }

  Key key(EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "P-256"));
  check(key != nullptr, "key generation");
  Cert cert(X509_new());
  check(cert != nullptr, "certificate allocation");
  check(X509_set_version(cert.get(), 2) == 1, "certificate version");  // v3
  unsigned char serial[16];
  check(RAND_bytes(serial, sizeof serial) == 1, "serial number");
  serial[0] &= 0x7f;  // positive
  BIGNUM* number = BN_bin2bn(serial, sizeof serial, nullptr);
  check(number != nullptr && BN_to_ASN1_INTEGER(number, X509_get_serialNumber(cert.get())) != nullptr,
        "serial number");
  BN_free(number);
  check(X509_gmtime_adj(X509_getm_notBefore(cert.get()), -3600) != nullptr, "validity");
  check(X509_gmtime_adj(X509_getm_notAfter(cert.get()), 10L * 365 * 24 * 3600) != nullptr, "validity");
  check(X509_set_pubkey(cert.get(), key.get()) == 1, "public key");
  X509_NAME* name = X509_get_subject_name(cert.get());
  check(X509_NAME_add_entry_by_txt(name, "O", MBSTRING_UTF8,
                                   reinterpret_cast<const unsigned char*>("HomeMind"), -1, -1, 0) == 1 &&
            X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_UTF8,
                                       reinterpret_cast<const unsigned char*>(common_name.c_str()), -1, -1, 0) == 1,
        "subject");
  check(X509_set_issuer_name(cert.get(), name) == 1, "issuer");
  add_extension(cert.get(), NID_basic_constraints, "critical,CA:FALSE");
  add_extension(cert.get(), NID_key_usage, "critical,digitalSignature");
  add_extension(cert.get(), NID_ext_key_usage, "serverAuth");
  if (!host.empty()) add_extension(cert.get(), NID_subject_alt_name, (is_ip(host) ? "IP:" : "DNS:") + host);
  check(X509_sign(cert.get(), key.get(), EVP_sha256()) > 0, "signing");

  Bio key_pem(BIO_new(BIO_s_mem()));
  Bio cert_pem(BIO_new(BIO_s_mem()));
  check(key_pem && cert_pem, "memory");
  check(PEM_write_bio_PrivateKey(key_pem.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr) == 1, "key PEM");
  check(PEM_write_bio_X509(cert_pem.get(), cert.get()) == 1, "certificate PEM");
  ensure_private_dir(key_file.parent_path());
  ensure_private_dir(cert_file.parent_path());
  write_private_file(key_file, bio_text(key_pem.get()));
  write_private_file(cert_file, bio_text(cert_pem.get()));
  identity.sha256 = fingerprint(cert.get());
  identity.created = true;
  log(LogLevel::info, "tls", "created a self-signed certificate (sha256 " + identity.sha256.substr(0, 16) + "…) for " +
                                 (host.empty() ? common_name : host));
  return identity;
}

#endif

}  // namespace hm::mobile
