/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements. See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership. The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License. You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied. See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include <errno.h>
#include <string>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <boost/lexical_cast.hpp>
#include <boost/shared_array.hpp>
#include <openssl/err.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include "concurrency/Mutex.h"
#include "TSSLSocket.h"

#define OPENSSL_VERSION_NO_THREAD_ID 0x10000000L

using namespace std;
using namespace boost;
using namespace apache::thrift::concurrency;

struct CRYPTO_dynlock_value {
  Mutex mutex;
};

namespace apache { namespace thrift { namespace transport {


static void buildErrors(string& message, int error = 0);
static bool matchName(const char* host, const char* pattern, int size);
static char uppercase(char c);

// SSLContext implementation
SSLContext::SSLContext() {
  ctx_ = SSL_CTX_new(TLSv1_method());
  if (ctx_ == NULL) {
    string errors;
    buildErrors(errors);
    throw TSSLException("SSL_CTX_new: " + errors);
  }
  SSL_CTX_set_mode(ctx_, SSL_MODE_AUTO_RETRY);
}

SSLContext::~SSLContext() {
  if (ctx_ != NULL) {
    SSL_CTX_free(ctx_);
    ctx_ = NULL;
  }
}

SSL* SSLContext::createSSL() {
  SSL* ssl = SSL_new(ctx_);
  if (ssl == NULL) {
    string errors;
    buildErrors(errors);
    throw TSSLException("SSL_new: " + errors);
  }
  return ssl;
}

// TSSLSocket implementation
TSSLSocket::TSSLSocket(shared_ptr<SSLContext> ctx):
  TSocket(), server_(false), ssl_(NULL), ctx_(ctx) {
}

TSSLSocket::TSSLSocket(shared_ptr<SSLContext> ctx, int socket):
  TSocket(socket), server_(false), ssl_(NULL), ctx_(ctx) {
}

TSSLSocket::TSSLSocket(shared_ptr<SSLContext> ctx, string host, int port):
  TSocket(host, port), server_(false), ssl_(NULL), ctx_(ctx) {
}

TSSLSocket::~TSSLSocket() {
  close();
}

bool TSSLSocket::isOpen() {
  if (ssl_ == NULL || !TSocket::isOpen()) {
    return false;
  }
  int shutdown = SSL_get_shutdown(ssl_);
  bool shutdownReceived = (shutdown & SSL_RECEIVED_SHUTDOWN);
  bool shutdownSent     = (shutdown & SSL_SENT_SHUTDOWN);
  if (shutdownReceived && shutdownSent) {
    return false;
  }
  return true;
}

bool TSSLSocket::peek() {
  if (!isOpen()) {
    return false;
  }
  checkHandshake();
  int rc;
  uint8_t byte;
  rc = SSL_peek(ssl_, &byte, 1);
  if (rc < 0) {
    int errno_copy = errno;
    string errors;
    buildErrors(errors, errno_copy);
    throw TSSLException("SSL_peek: " + errors);
  }
  if (rc == 0) {
    ERR_clear_error();
  }
  return (rc > 0);
}

void TSSLSocket::open() {
  if (isOpen() || server()) {
    throw TTransportException(TTransportException::BAD_ARGS);
  }
  TSocket::open();
}

void TSSLSocket::close() {
  if (ssl_ != NULL) {
    int rc = SSL_shutdown(ssl_);
    if (rc == 0) {
      rc = SSL_shutdown(ssl_);
    }
    if (rc < 0) {
      int errno_copy = errno;
      string errors;
      buildErrors(errors, errno_copy);
      GlobalOutput(("SSL_shutdown: " + errors).c_str());
    }
    SSL_free(ssl_);
    ssl_ = NULL;
    ERR_remove_state(0);
  }
  TSocket::close();
}

uint32_t TSSLSocket::read(uint8_t* buf, uint32_t len) {
  checkHandshake();
  int32_t bytes = 0;
  for (int32_t retries = 0; retries < maxRecvRetries_; retries++){
    bytes = SSL_read(ssl_, buf, len);
    if (bytes >= 0)
      break;
    int errno_copy = errno;
    if (SSL_get_error(ssl_, bytes) == SSL_ERROR_SYSCALL) {
      if (ERR_get_error() == 0 && errno_copy == EINTR) {
        continue;
      }
    }
    string errors;
    buildErrors(errors, errno_copy);
    throw TSSLException("SSL_read: " + errors);
  }
  return bytes;
}

void TSSLSocket::write(const uint8_t* buf, uint32_t len) {
  checkHandshake();
  // loop in case SSL_MODE_ENABLE_PARTIAL_WRITE is set in SSL_CTX.
  uint32_t written = 0;
  while (written < len) {
    int32_t bytes = SSL_write(ssl_, &buf[written], len - written);
    if (bytes <= 0) {
      int errno_copy = errno;
      string errors;
      buildErrors(errors, errno_copy);
      throw TSSLException("SSL_write: " + errors);
    }
    written += bytes;
  }
}

void TSSLSocket::flush() {
  // Don't throw exception if not open. Thrift servers close socket twice.
  if (ssl_ == NULL) {
    return;
  }
  checkHandshake();
  BIO* bio = SSL_get_wbio(ssl_);
  if (bio == NULL) {
    throw TSSLException("SSL_get_wbio returns NULL");
  }
  if (BIO_flush(bio) != 1) {
    int errno_copy = errno;
    string errors;
    buildErrors(errors, errno_copy);
    throw TSSLException("BIO_flush: " + errors);
  }
}

void TSSLSocket::checkHandshake() {
  if (!TSocket::isOpen()) {
    throw TTransportException(TTransportException::NOT_OPEN);
  }
  if (ssl_ != NULL) {
    return;
  }
  ssl_ = ctx_->createSSL();
  SSL_set_fd(ssl_, socket_);
  int rc;
  if (server()) {
    rc = SSL_accept(ssl_);
  } else {
    rc = SSL_connect(ssl_);
  }
  if (rc <= 0) {
    int errno_copy = errno;
    string fname(server() ? "SSL_accept" : "SSL_connect");
    string errors;
    buildErrors(errors, errno_copy);
    throw TSSLException(fname + ": " + errors);
  }
  authorize();
}

void TSSLSocket::authorize() {
  int rc = SSL_get_verify_result(ssl_);
  if (rc != X509_V_OK) {  // verify authentication result
    throw TSSLException(string("SSL_get_verify_result(), ") +
                        X509_verify_cert_error_string(rc));
  }

  X509* cert = SSL_get_peer_certificate(ssl_);
  if (cert == NULL) {
    // certificate is not present
    if (SSL_get_verify_mode(ssl_) & SSL_VERIFY_FAIL_IF_NO_PEER_CERT) {
      throw TSSLException("authorize: required certificate not present");
    }
    // certificate was optional: didn't intend to authorize remote
    if (server() && access_ != NULL) {
      throw TSSLException("authorize: certificate required for authorization");
    }
    return;
  }
  // certificate is present
  if (access_ == NULL) {
    X509_free(cert);
    return;
  }
  // both certificate and access manager are present

  string host;
  sockaddr_storage sa = {};
  socklen_t saLength = sizeof(sa);

  if (getpeername(socket_, (sockaddr*)&sa, &saLength) != 0) {
    sa.ss_family = AF_UNSPEC;
  }

  AccessManager::Decision decision = access_->verify(sa);

  if (decision != AccessManager::SKIP) {
    X509_free(cert);
    if (decision != AccessManager::ALLOW) {
      throw TSSLException("authorize: access denied based on remote IP");
    }
    return;
  }

  // extract subjectAlternativeName
  STACK_OF(GENERAL_NAME)* alternatives = (STACK_OF(GENERAL_NAME)*)
                       X509_get_ext_d2i(cert, NID_subject_alt_name, NULL, NULL);
  if (alternatives != NULL) {
    const int count = sk_GENERAL_NAME_num(alternatives);
    for (int i = 0; decision == AccessManager::SKIP && i < count; i++) {
      const GENERAL_NAME* name = sk_GENERAL_NAME_value(alternatives, i);
      if (name == NULL) {
        continue;
      }
      char* data = (char*)ASN1_STRING_data(name->d.ia5);
      int length = ASN1_STRING_length(name->d.ia5);
      switch (name->type) {
        case GEN_DNS:
          if (host.empty()) {
            host = (server() ? getPeerHost() : getHost());
          }
          decision = access_->verify(host, data, length);
          break;
        case GEN_IPADD:
          decision = access_->verify(sa, data, length);
          break;
      }
    }
    sk_GENERAL_NAME_pop_free(alternatives, GENERAL_NAME_free);
  }

  if (decision != AccessManager::SKIP) {
    X509_free(cert);
    if (decision != AccessManager::ALLOW) {
      throw TSSLException("authorize: access denied");
    }
    return;
  }

  // extract commonName
  X509_NAME* name = X509_get_subject_name(cert);
  if (name != NULL) {
    X509_NAME_ENTRY* entry;
    unsigned char* utf8;
    int last = -1;
    while (decision == AccessManager::SKIP) {
      last = X509_NAME_get_index_by_NID(name, NID_commonName, last);
      if (last == -1)
        break;
      entry = X509_NAME_get_entry(name, last);
      if (entry == NULL)
        continue;
      ASN1_STRING* common = X509_NAME_ENTRY_get_data(entry);
      int size = ASN1_STRING_to_UTF8(&utf8, common);
      if (host.empty()) {
        host = (server() ? getHost() : getHost());
      }
      decision = access_->verify(host, (char*)utf8, size);
      OPENSSL_free(utf8);
    }
  }
  X509_free(cert);
  if (decision != AccessManager::ALLOW) {
    throw TSSLException("authorize: cannot authorize peer");
  }
}

// TSSLSocketFactory implementation
bool     TSSLSocketFactory::initialized = false;
uint64_t TSSLSocketFactory::count_ = 0;
Mutex    TSSLSocketFactory::mutex_;

TSSLSocketFactory::TSSLSocketFactory(): server_(false) {
  Guard guard(mutex_);
  if (count_ == 0) {
    initializeOpenSSL();
    randomize();
  }
  count_++;
  ctx_ = shared_ptr<SSLContext>(new SSLContext);
}

TSSLSocketFactory::~TSSLSocketFactory() {
  Guard guard(mutex_);
  count_--;
  if (count_ == 0) {
    cleanupOpenSSL();
  }
}

shared_ptr<TSSLSocket> TSSLSocketFactory::createSocket() {
  shared_ptr<TSSLSocket> ssl(new TSSLSocket(ctx_));
  setup(ssl);
  return ssl;
}

shared_ptr<TSSLSocket> TSSLSocketFactory::createSocket(int socket) {
  shared_ptr<TSSLSocket> ssl(new TSSLSocket(ctx_, socket));
  setup(ssl);
  return ssl;
}

shared_ptr<TSSLSocket> TSSLSocketFactory::createSocket(const string& host,
                                                       int port) {
  shared_ptr<TSSLSocket> ssl(new TSSLSocket(ctx_, host, port));
  setup(ssl);
  return ssl;
}

void TSSLSocketFactory::setup(shared_ptr<TSSLSocket> ssl) {
  ssl->server(server());
  if (access_ == NULL && !server()) {
    access_ = shared_ptr<AccessManager>(new DefaultClientAccessManager);
  }
  if (access_ != NULL) {
    ssl->access(access_);
  }
}

void TSSLSocketFactory::ciphers(const string& enable) {
  int rc = SSL_CTX_set_cipher_list(ctx_->get(), enable.c_str());
  if (ERR_peek_error() != 0) {
    string errors;
    buildErrors(errors);
    throw TSSLException("SSL_CTX_set_cipher_list: " + errors);
  }
  if (rc == 0) {
    throw TSSLException("None of specified ciphers are supported");
  }
}

void TSSLSocketFactory::authenticate(bool required) {
  int mode;
  if (required) {
    mode  = SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT | SSL_VERIFY_CLIENT_ONCE;
  } else {
    mode = SSL_VERIFY_NONE;
  }
  SSL_CTX_set_verify(ctx_->get(), mode, NULL);
}

void TSSLSocketFactory::loadCertificate(const char* path, const char* format) {
  if (path == NULL || format == NULL) {
    throw TTransportException(TTransportException::BAD_ARGS,
         "loadCertificateChain: either <path> or <format> is NULL");
  }
  if (strcmp(format, "PEM") == 0) {
    if (SSL_CTX_use_certificate_chain_file(ctx_->get(), path) == 0) {
      int errno_copy = errno;
      string errors;
      buildErrors(errors, errno_copy);
      throw TSSLException("SSL_CTX_use_certificate_chain_file: " + errors);
    }
  } else {
    throw TSSLException("Unsupported certificate format: " + string(format));
  }
}

void TSSLSocketFactory::loadPrivateKey(const char* path, const char* format) {
  if (path == NULL || format == NULL) {
    throw TTransportException(TTransportException::BAD_ARGS,
         "loadPrivateKey: either <path> or <format> is NULL");
  }
  if (strcmp(format, "PEM") == 0) {
    if (SSL_CTX_use_PrivateKey_file(ctx_->get(), path, SSL_FILETYPE_PEM) == 0) {
      int errno_copy = errno;
      string errors;
      buildErrors(errors, errno_copy);
      throw TSSLException("SSL_CTX_use_PrivateKey_file: " + errors);
    }
  }
}

void TSSLSocketFactory::loadTrustedCertificates(const char* path) {
  if (path == NULL) {
    throw TTransportException(TTransportException::BAD_ARGS,
         "loadTrustedCertificates: <path> is NULL");
  }
  if (SSL_CTX_load_verify_locations(ctx_->get(), path, NULL) == 0) {
    int errno_copy = errno;
    string errors;
    buildErrors(errors, errno_copy);
    throw TSSLException("SSL_CTX_load_verify_locations: " + errors);
  }
}

void TSSLSocketFactory::randomize() {
  RAND_poll();
}

void TSSLSocketFactory::overrideDefaultPasswordCallback() {
  SSL_CTX_set_default_passwd_cb(ctx_->get(), passwordCallback);
  SSL_CTX_set_default_passwd_cb_userdata(ctx_->get(), this);
}

int TSSLSocketFactory::passwordCallback(char* password,
                                        int size,
                                        int,
                                        void* data) {
  TSSLSocketFactory* factory = (TSSLSocketFactory*)data;
  string userPassword;
  factory->getPassword(userPassword, size);
  int length = userPassword.size();
  if (length > size) {
    length = size;
  }
  strncpy(password, userPassword.c_str(), length);
  return length;
}

static shared_array<Mutex> mutexes;

static void callbackLocking(int mode, int n, const char*, int) {
  if (mode & CRYPTO_LOCK) {
    mutexes[n].lock();
  } else {
    mutexes[n].unlock();
  }
}

#if (OPENSSL_VERSION_NUMBER < OPENSSL_VERSION_NO_THREAD_ID)
static unsigned long callbackThreadID() {
  return (unsigned long) pthread_self();
}
#endif

static CRYPTO_dynlock_value* dyn_create(const char*, int) {
  return new CRYPTO_dynlock_value;
}

static void dyn_lock(int mode,
                     struct CRYPTO_dynlock_value* lock,
                     const char*, int) {
  if (lock != NULL) {
    if (mode & CRYPTO_LOCK) {
      lock->mutex.lock();
    } else {
      lock->mutex.unlock();
    }
  }
}

static void dyn_destroy(struct CRYPTO_dynlock_value* lock, const char*, int) {
  delete lock;
}

void TSSLSocketFactory::initializeOpenSSL() {
  if (initialized) {
    return;
  }
  initialized = true;
  SSL_library_init();
  SSL_load_error_strings();
  // static locking
  mutexes = shared_array<Mutex>(new Mutex[::CRYPTO_num_locks()]);
  if (mutexes == NULL) {
    throw TTransportException(TTransportException::INTERNAL_ERROR,
          "initializeOpenSSL() failed, "
          "out of memory while creating mutex array");
  }
#if (OPENSSL_VERSION_NUMBER < OPENSSL_VERSION_NO_THREAD_ID)
  CRYPTO_set_id_callback(callbackThreadID);
#endif
  CRYPTO_set_locking_callback(callbackLocking);
  // dynamic locking
  CRYPTO_set_dynlock_create_callback(dyn_create);
  CRYPTO_set_dynlock_lock_callback(dyn_lock);
  CRYPTO_set_dynlock_destroy_callback(dyn_destroy);
}

void TSSLSocketFactory::cleanupOpenSSL() {
  if (!initialized) {
    return;
  }
  initialized = false;
#if (OPENSSL_VERSION_NUMBER < OPENSSL_VERSION_NO_THREAD_ID)
  CRYPTO_set_id_callback(NULL);
#endif
  CRYPTO_set_locking_callback(NULL);
  CRYPTO_set_dynlock_create_callback(NULL);
  CRYPTO_set_dynlock_lock_callback(NULL);
  CRYPTO_set_dynlock_destroy_callback(NULL);
  CRYPTO_cleanup_all_ex_data();
  ERR_free_strings();
  EVP_cleanup();
  ERR_remove_state(0);
  mutexes.reset();
}

// extract error messages from error queue
void buildErrors(string& errors, int errno_copy) {
  unsigned long  errorCode;
  char   message[256];

  errors.reserve(512);
  while ((errorCode = ERR_get_error()) != 0) {
    if (!errors.empty()) {
      errors += "; ";
    }
    const char* reason = ERR_reason_error_string(errorCode);
    if (reason == NULL) {
      snprintf(message, sizeof(message) - 1, "SSL error # %lu", errorCode);
      reason = message;
    }
    errors += reason;
  }
  if (errors.empty()) {
    if (errno_copy != 0) {
      errors += TOutput::strerror_s(errno_copy);
    }
  }
  if (errors.empty()) {
    errors = "error code: " + lexical_cast<string>(errno_copy);
  }
}

/**
 * Default implementation of AccessManager
 */
Decision DefaultClientAccessManager::verify(const sockaddr_storage& sa)
  throw() { return SKIP; }

Decision DefaultClientAccessManager::verify(const string& host,
                                            const char* name,
                                            int size) throw() {
  if (host.empty() || name == NULL || size <= 0) {
    return SKIP;
  }
  return (matchName(host.c_str(), name, size) ? ALLOW : SKIP);
}

Decision DefaultClientAccessManager::verify(const sockaddr_storage& sa,
                                            const char* data,
                                            int size) throw() {
  bool match = false;
  if (sa.ss_family == AF_INET && size == sizeof(in_addr)) {
    match = (memcmp(&((sockaddr_in*)&sa)->sin_addr, data, size) == 0);
  } else if (sa.ss_family == AF_INET6 && size == sizeof(in6_addr)) {
    match = (memcmp(&((sockaddr_in6*)&sa)->sin6_addr, data, size) == 0);
  }
  return (match ? ALLOW : SKIP);
}

/**
 * Match a name with a pattern. The pattern may include wildcard. A single
 * wildcard "*" can match up to one component in the domain name.
 *
 * @param  host    Host name, typically the name of the remote host
 * @param  pattern Name retrieved from certificate
 * @param  size    Size of "pattern"
 * @return True, if "host" matches "pattern". False otherwise.
 */
bool matchName(const char* host, const char* pattern, int size) {
  bool match = false;
  int i = 0, j = 0;
  while (i < size && host[j] != '\0') {
    if (uppercase(pattern[i]) == uppercase(host[j])) {
      i++;
      j++;
      continue;
    }
    if (pattern[i] == '*') {
      while (host[j] != '.' && host[j] != '\0') {
        j++;
      }
      i++;
      continue;
    }
    break;
  }
  if (i == size && host[j] == '\0') {
    match = true;
  }
  return match;

}

// This is to work around the Turkish locale issue, i.e.,
// toupper('i') != toupper('I') if locale is "tr_TR"
char uppercase (char c) {
  if ('a' <= c && c <= 'z') {
    return c + ('A' - 'a');
  }
  return c;
}

}}}
