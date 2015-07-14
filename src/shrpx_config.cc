/*
 * nghttp2 - HTTP/2 C Library
 *
 * Copyright (c) 2012 Tatsuhiro Tsujikawa
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include "shrpx_config.h"

#ifdef HAVE_PWD_H
#include <pwd.h>
#endif // HAVE_PWD_H
#ifdef HAVE_NETDB_H
#include <netdb.h>
#endif // HAVE_NETDB_H
#ifdef HAVE_SYSLOG_H
#include <syslog.h>
#endif // HAVE_SYSLOG_H
#include <sys/types.h>
#include <sys/stat.h>
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif // HAVE_FCNTL_H
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif // HAVE_UNISTD_H

#include <cstring>
#include <cerrno>
#include <limits>
#include <fstream>

#include <nghttp2/nghttp2.h>

#include "http-parser/http_parser.h"

#include "shrpx_log.h"
#include "shrpx_ssl.h"
#include "shrpx_http.h"
#include "http2.h"
#include "util.h"
#include "template.h"
#include "base64.h"

using namespace nghttp2;

namespace shrpx {

namespace {
Config *config = nullptr;
} // namespace

const Config *get_config() { return config; }

Config *mod_config() { return config; }

void create_config() { config = new Config(); }

TicketKeys::~TicketKeys() {
  /* Erase keys from memory */
  for (auto &key : keys) {
    memset(&key, 0, sizeof(key));
  }
}

DownstreamAddr::DownstreamAddr(const DownstreamAddr &other)
    : addr(other.addr), host(other.host ? strcopy(other.host.get()) : nullptr),
      hostport(other.hostport ? strcopy(other.hostport.get()) : nullptr),
      addrlen(other.addrlen), port(other.port), host_unix(other.host_unix) {}

DownstreamAddr &DownstreamAddr::operator=(const DownstreamAddr &other) {
  if (this == &other) {
    return *this;
  }

  addr = other.addr;
  host = (other.host ? strcopy(other.host.get()) : nullptr);
  hostport = (other.hostport ? strcopy(other.hostport.get()) : nullptr);
  addrlen = other.addrlen;
  port = other.port;
  host_unix = other.host_unix;

  return *this;
}

namespace {
int split_host_port(char *host, size_t hostlen, uint16_t *port_ptr,
                    const char *hostport, size_t hostportlen) {
  // host and port in |hostport| is separated by single ','.
  const char *p = strchr(hostport, ',');
  if (!p) {
    LOG(ERROR) << "Invalid host, port: " << hostport;
    return -1;
  }
  size_t len = p - hostport;
  if (hostlen < len + 1) {
    LOG(ERROR) << "Hostname too long: " << hostport;
    return -1;
  }
  memcpy(host, hostport, len);
  host[len] = '\0';

  errno = 0;
  auto portlen = hostportlen - len - 1;
  auto d = util::parse_uint(reinterpret_cast<const uint8_t *>(p + 1), portlen);
  if (1 <= d && d <= std::numeric_limits<uint16_t>::max()) {
    *port_ptr = d;
    return 0;
  } else {
    LOG(ERROR) << "Port is invalid: " << std::string(p + 1, portlen);
    return -1;
  }
}
} // namespace

namespace {
bool is_secure(const char *filename) {
  struct stat buf;
  int rv = stat(filename, &buf);
  if (rv == 0) {
    if ((buf.st_mode & S_IRWXU) && !(buf.st_mode & S_IRWXG) &&
        !(buf.st_mode & S_IRWXO)) {
      return true;
    }
  }

  return false;
}
} // namespace

std::unique_ptr<TicketKeys>
read_tls_ticket_key_file(const std::vector<std::string> &files) {
  auto ticket_keys = make_unique<TicketKeys>();
  auto &keys = ticket_keys->keys;
  keys.resize(files.size());
  size_t i = 0;
  for (auto &file : files) {
    std::ifstream f(file.c_str());
    if (!f) {
      LOG(ERROR) << "tls-ticket-key-file: could not open file " << file;
      return nullptr;
    }
    char buf[48];
    f.read(buf, sizeof(buf));
    if (f.gcount() != sizeof(buf)) {
      LOG(ERROR) << "tls-ticket-key-file: want to read 48 bytes but read "
                 << f.gcount() << " bytes from " << file;
      return nullptr;
    }

    auto &key = keys[i++];
    auto p = buf;
    memcpy(key.name, p, sizeof(key.name));
    p += sizeof(key.name);
    memcpy(key.aes_key, p, sizeof(key.aes_key));
    p += sizeof(key.aes_key);
    memcpy(key.hmac_key, p, sizeof(key.hmac_key));

    if (LOG_ENABLED(INFO)) {
      LOG(INFO) << "session ticket key: " << util::format_hex(key.name,
                                                              sizeof(key.name));
    }
  }
  return ticket_keys;
}

FILE *open_file_for_write(const char *filename) {
#if defined O_CLOEXEC
  auto fd = open(filename, O_WRONLY | O_CLOEXEC | O_CREAT | O_TRUNC,
                 S_IRUSR | S_IWUSR);
#else
  auto fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);

  // We get race condition if execve is called at the same time.
  if (fd != -1) {
    util::make_socket_closeonexec(fd);
  }
#endif
  if (fd == -1) {
    LOG(ERROR) << "Failed to open " << filename
               << " for writing. Cause: " << strerror(errno);
    return nullptr;
  }
  auto f = fdopen(fd, "wb");
  if (f == nullptr) {
    LOG(ERROR) << "Failed to open " << filename
               << " for writing. Cause: " << strerror(errno);
    return nullptr;
  }

  return f;
}

std::string read_passwd_from_file(const char *filename) {
  std::string line;

  if (!is_secure(filename)) {
    LOG(ERROR) << "Private key passwd file " << filename
               << " has insecure mode.";
    return line;
  }

  std::ifstream in(filename, std::ios::binary);
  if (!in) {
    LOG(ERROR) << "Could not open key passwd file " << filename;
    return line;
  }

  std::getline(in, line);
  return line;
}

std::unique_ptr<char[]> strcopy(const char *val) {
  return strcopy(val, strlen(val));
}

std::unique_ptr<char[]> strcopy(const char *val, size_t len) {
  auto res = make_unique<char[]>(len + 1);
  memcpy(res.get(), val, len);
  res[len] = '\0';
  return res;
}

std::unique_ptr<char[]> strcopy(const std::string &val) {
  return strcopy(val.c_str(), val.size());
}

std::vector<char *> parse_config_str_list(const char *s, char delim) {
  size_t len = 1;
  for (const char *first = s, *p = nullptr; (p = strchr(first, delim));
       ++len, first = p + 1)
    ;
  auto list = std::vector<char *>(len);
  auto first = strdup(s);
  len = 0;
  for (;;) {
    auto p = strchr(first, delim);
    if (p == nullptr) {
      break;
    }
    list[len++] = first;
    *p = '\0';
    first = p + 1;
  }
  list[len++] = first;

  return list;
}

void clear_config_str_list(std::vector<char *> &list) {
  if (list.empty()) {
    return;
  }

  free(list[0]);
  list.clear();
}

std::pair<std::string, std::string> parse_header(const char *optarg) {
  // We skip possible ":" at the start of optarg.
  const auto *colon = strchr(optarg + 1, ':');

  // name = ":" is not allowed
  if (colon == nullptr || (optarg[0] == ':' && colon == optarg + 1)) {
    return {"", ""};
  }

  auto value = colon + 1;
  for (; *value == '\t' || *value == ' '; ++value)
    ;

  return {std::string(optarg, colon), std::string(value, strlen(value))};
}

template <typename T>
int parse_uint(T *dest, const char *opt, const char *optarg) {
  char *end = nullptr;

  errno = 0;

  auto val = strtol(optarg, &end, 10);

  if (!optarg[0] || errno != 0 || *end || val < 0) {
    LOG(ERROR) << opt << ": bad value.  Specify an integer >= 0.";
    return -1;
  }

  *dest = val;

  return 0;
}

namespace {
template <typename T>
int parse_uint_with_unit(T *dest, const char *opt, const char *optarg) {
  auto n = util::parse_uint_with_unit(optarg);
  if (n == -1) {
    LOG(ERROR) << opt << ": bad value: '" << optarg << "'";
    return -1;
  }

  *dest = n;

  return 0;
}
} // namespace

template <typename T>
int parse_int(T *dest, const char *opt, const char *optarg) {
  char *end = nullptr;

  errno = 0;

  auto val = strtol(optarg, &end, 10);

  if (!optarg[0] || errno != 0 || *end) {
    LOG(ERROR) << opt << ": bad value.  Specify an integer.";
    return -1;
  }

  *dest = val;

  return 0;
}

namespace {
LogFragment make_log_fragment(LogFragmentType type,
                              std::unique_ptr<char[]> value = nullptr) {
  return LogFragment{type, std::move(value)};
}
} // namespace

namespace {
bool var_token(char c) {
  return util::isAlpha(c) || util::isDigit(c) || c == '_';
}
} // namespace

std::vector<LogFragment> parse_log_format(const char *optarg) {
  auto literal_start = optarg;
  auto p = optarg;
  auto eop = p + strlen(optarg);

  auto res = std::vector<LogFragment>();

  for (; p != eop;) {
    if (*p != '$') {
      ++p;
      continue;
    }

    auto var_start = p;

    ++p;

    const char *var_name;
    size_t var_namelen;
    if (p != eop && *p == '{') {
      var_name = ++p;
      for (; p != eop && var_token(*p); ++p)
        ;

      if (p == eop || *p != '}') {
        LOG(WARN) << "Missing '}' after " << std::string(var_start, p);
        continue;
      }

      var_namelen = p - var_name;
      ++p;
    } else {
      var_name = p;
      for (; p != eop && var_token(*p); ++p)
        ;

      var_namelen = p - var_name;
    }

    auto type = SHRPX_LOGF_NONE;
    const char *value = nullptr;
    size_t valuelen = 0;

    if (util::strieq_l("remote_addr", var_name, var_namelen)) {
      type = SHRPX_LOGF_REMOTE_ADDR;
    } else if (util::strieq_l("time_local", var_name, var_namelen)) {
      type = SHRPX_LOGF_TIME_LOCAL;
    } else if (util::strieq_l("time_iso8601", var_name, var_namelen)) {
      type = SHRPX_LOGF_TIME_ISO8601;
    } else if (util::strieq_l("request", var_name, var_namelen)) {
      type = SHRPX_LOGF_REQUEST;
    } else if (util::strieq_l("status", var_name, var_namelen)) {
      type = SHRPX_LOGF_STATUS;
    } else if (util::strieq_l("body_bytes_sent", var_name, var_namelen)) {
      type = SHRPX_LOGF_BODY_BYTES_SENT;
    } else if (util::istartsWith(var_name, var_namelen, "http_")) {
      type = SHRPX_LOGF_HTTP;
      value = var_name + sizeof("http_") - 1;
      valuelen = var_namelen - (sizeof("http_") - 1);
    } else if (util::strieq_l("remote_port", var_name, var_namelen)) {
      type = SHRPX_LOGF_REMOTE_PORT;
    } else if (util::strieq_l("server_port", var_name, var_namelen)) {
      type = SHRPX_LOGF_SERVER_PORT;
    } else if (util::strieq_l("request_time", var_name, var_namelen)) {
      type = SHRPX_LOGF_REQUEST_TIME;
    } else if (util::strieq_l("pid", var_name, var_namelen)) {
      type = SHRPX_LOGF_PID;
    } else if (util::strieq_l("alpn", var_name, var_namelen)) {
      type = SHRPX_LOGF_ALPN;
    } else if (util::strieq_l("ssl_cipher", var_name, var_namelen)) {
      type = SHRPX_LOGF_SSL_CIPHER;
    } else if (util::strieq_l("ssl_protocol", var_name, var_namelen)) {
      type = SHRPX_LOGF_SSL_PROTOCOL;
    } else if (util::strieq_l("ssl_session_id", var_name, var_namelen)) {
      type = SHRPX_LOGF_SSL_SESSION_ID;
    } else if (util::strieq_l("ssl_session_reused", var_name, var_namelen)) {
      type = SHRPX_LOGF_SSL_SESSION_REUSED;
    } else {
      LOG(WARN) << "Unrecognized log format variable: "
                << std::string(var_name, var_namelen);
      continue;
    }

    if (literal_start < var_start) {
      res.push_back(
          make_log_fragment(SHRPX_LOGF_LITERAL,
                            strcopy(literal_start, var_start - literal_start)));
    }

    literal_start = p;

    if (value == nullptr) {
      res.push_back(make_log_fragment(type));
      continue;
    }

    res.push_back(make_log_fragment(type, strcopy(value, valuelen)));
    auto &v = res.back().value;
    for (size_t i = 0; v[i]; ++i) {
      if (v[i] == '_') {
        v[i] = '-';
      }
    }
  }

  if (literal_start != eop) {
    res.push_back(make_log_fragment(
        SHRPX_LOGF_LITERAL, strcopy(literal_start, eop - literal_start)));
  }

  return res;
}

namespace {
int parse_duration(ev_tstamp *dest, const char *opt, const char *optarg) {
  auto t = util::parse_duration_with_unit(optarg);
  if (t == std::numeric_limits<double>::infinity()) {
    LOG(ERROR) << opt << ": bad value: '" << optarg << "'";
    return -1;
  }

  *dest = t;

  return 0;
}
} // namespace

namespace {
// Parses host-path mapping patterns in |src|, and stores mappings in
// config.  We will store each host-path pattern found in |src| with
// |addr|.  |addr| will be copied accordingly.  Also we make a group
// based on the pattern.  The "/" pattern is considered as catch-all.
void parse_mapping(const DownstreamAddr &addr, const char *src) {
  // This returns at least 1 element (it could be empty string).  We
  // will append '/' to all patterns, so it becomes catch-all pattern.
  auto mapping = parse_config_str_list(src, ':');
  assert(!mapping.empty());
  for (auto raw_pattern : mapping) {
    auto done = false;
    std::string pattern;
    auto slash = strchr(raw_pattern, '/');
    if (slash == nullptr) {
      // This effectively makes empty pattern to "/".
      pattern = raw_pattern;
      util::inp_strlower(pattern);
      pattern += "/";
    } else {
      pattern.assign(raw_pattern, slash);
      util::inp_strlower(pattern);
      pattern +=
          http2::normalize_path(slash, raw_pattern + strlen(raw_pattern));
    }
    for (auto &g : mod_config()->downstream_addr_groups) {
      if (g.pattern == pattern) {
        g.addrs.push_back(addr);
        done = true;
        break;
      }
    }
    if (done) {
      continue;
    }
    DownstreamAddrGroup g(pattern);
    g.addrs.push_back(addr);
    mod_config()->downstream_addr_groups.push_back(std::move(g));
  }
  clear_config_str_list(mapping);
}
} // namespace

int parse_config(const char *opt, const char *optarg,
                 std::set<std::string> &included_set) {
  char host[NI_MAXHOST];
  uint16_t port;

  if (util::strieq(opt, SHRPX_OPT_BACKEND)) {
    auto optarglen = strlen(optarg);
    const char *pat_delim = strchr(optarg, ';');
    if (!pat_delim) {
      pat_delim = optarg + optarglen;
    }
    DownstreamAddr addr;
    if (util::istartsWith(optarg, SHRPX_UNIX_PATH_PREFIX)) {
      auto path = optarg + str_size(SHRPX_UNIX_PATH_PREFIX);
      addr.host = strcopy(path, pat_delim - path);
      addr.host_unix = true;
    } else {
      if (split_host_port(host, sizeof(host), &port, optarg,
                          pat_delim - optarg) == -1) {
        return -1;
      }

      addr.host = strcopy(host);
      addr.port = port;
    }

    auto mapping = pat_delim < optarg + optarglen ? pat_delim + 1 : pat_delim;
    // We may introduce new parameter after additional ';', so don't
    // allow extra ';' in pattern for now.
    if (strchr(mapping, ';') != nullptr) {
      LOG(ERROR) << opt << ": ';' must not be used in pattern";
      return -1;
    }
    parse_mapping(addr, mapping);

    return 0;
  }

  if (util::strieq(opt, SHRPX_OPT_FRONTEND)) {
    if (util::istartsWith(optarg, SHRPX_UNIX_PATH_PREFIX)) {
      auto path = optarg + str_size(SHRPX_UNIX_PATH_PREFIX);
      mod_config()->host = strcopy(path);
      mod_config()->port = 0;
      mod_config()->host_unix = true;

      return 0;
    }

    if (split_host_port(host, sizeof(host), &port, optarg, strlen(optarg)) ==
        -1) {
      return -1;
    }

    mod_config()->host = strcopy(host);
    mod_config()->port = port;
    mod_config()->host_unix = false;

    return 0;
  }

  if (util::strieq(opt, SHRPX_OPT_WORKERS)) {
    return parse_uint(&mod_config()->num_worker, opt, optarg);
  }

  if (util::strieq(opt, SHRPX_OPT_HTTP2_MAX_CONCURRENT_STREAMS)) {
    return parse_uint(&mod_config()->http2_max_concurrent_streams, opt, optarg);
  }

  if (util::strieq(opt, SHRPX_OPT_LOG_LEVEL)) {
    if (Log::set_severity_level_by_name(optarg) == -1) {
      LOG(ERROR) << opt << ": Invalid severity level: " << optarg;
      return -1;
    }

    return 0;
  }

  if (util::strieq(opt, SHRPX_OPT_DAEMON)) {
    mod_config()->daemon = util::strieq(optarg, "yes");

    return 0;
  }

  if (util::strieq(opt, SHRPX_OPT_HTTP2_PROXY)) {
    mod_config()->http2_proxy = util::strieq(optarg, "yes");

    return 0;
  }

  if (util::strieq(opt, SHRPX_OPT_HTTP2_BRIDGE)) {
    mod_config()->http2_bridge = util::strieq(optarg, "yes");

    return 0;
  }

  if (util::strieq(opt, SHRPX_OPT_CLIENT_PROXY)) {
    mod_config()->client_proxy = util::strieq(optarg, "yes");

    return 0;
  }

  if (util::strieq(opt, SHRPX_OPT_ADD_X_FORWARDED_FOR)) {
    mod_config()->add_x_forwarded_for = util::strieq(optarg, "yes");

    return 0;
  }

  if (util::strieq(opt, SHRPX_OPT_STRIP_INCOMING_X_FORWARDED_FOR)) {
    mod_config()->strip_incoming_x_forwarded_for = util::strieq(optarg, "yes");

    return 0;
  }

  if (util::strieq(opt, SHRPX_OPT_NO_VIA)) {
    mod_config()->no_via = util::strieq(optarg, "yes");

    return 0;
  }

  if (util::strieq(opt, SHRPX_OPT_FRONTEND_HTTP2_READ_TIMEOUT)) {
    return parse_duration(&mod_config()->http2_upstream_read_timeout, opt,
                          optarg);
  }

  if (util::strieq(opt, SHRPX_OPT_FRONTEND_READ_TIMEOUT)) {
    return parse_duration(&mod_config()->upstream_read_timeout, opt, optarg);
  }

  if (util::strieq(opt, SHRPX_OPT_FRONTEND_WRITE_TIMEOUT)) {
    return parse_duration(&mod_config()->upstream_write_timeout, opt, optarg);
  }

  if (util::strieq(opt, SHRPX_OPT_BACKEND_READ_TIMEOUT)) {
    return parse_duration(&mod_config()->downstream_read_timeout, opt, optarg);
  }

  if (util::strieq(opt, SHRPX_OPT_BACKEND_WRITE_TIMEOUT)) {
    return parse_duration(&mod_config()->downstream_write_timeout, opt, optarg);
  }

  if (util::strieq(opt, SHRPX_OPT_STREAM_READ_TIMEOUT)) {
    return parse_duration(&mod_config()->stream_read_timeout, opt, optarg);
  }

  if (util::strieq(opt, SHRPX_OPT_STREAM_WRITE_TIMEOUT)) {
    return parse_duration(&mod_config()->stream_write_timeout, opt, optarg);
  }

  if (util::strieq(opt, SHRPX_OPT_ACCESSLOG_FILE)) {
    mod_config()->accesslog_file = strcopy(optarg);

    return 0;
  }

  if (util::strieq(opt, SHRPX_OPT_ACCESSLOG_SYSLOG)) {
    mod_config()->accesslog_syslog = util::strieq(optarg, "yes");

    return 0;
  }

  if (util::strieq(opt, SHRPX_OPT_ACCESSLOG_FORMAT)) {
    mod_config()->accesslog_format = parse_log_format(optarg);

    return 0;
  }

  if (util::strieq(opt, SHRPX_OPT_ERRORLOG_FILE)) {
    mod_config()->errorlog_file = strcopy(optarg);

    return 0;
  }

  if (util::strieq(opt, SHRPX_OPT_ERRORLOG_SYSLOG)) {
    mod_config()->errorlog_syslog = util::strieq(optarg, "yes");

    return 0;
  }

  if (util::strieq(opt, SHRPX_OPT_BACKEND_KEEP_ALIVE_TIMEOUT)) {
    return parse_duration(&mod_config()->downstream_idle_read_timeout, opt,
                          optarg);
  }

  if (util::strieq(opt, SHRPX_OPT_FRONTEND_HTTP2_WINDOW_BITS) ||
      util::strieq(opt, SHRPX_OPT_BACKEND_HTTP2_WINDOW_BITS)) {

    size_t *resp;

    if (util::strieq(opt, SHRPX_OPT_FRONTEND_HTTP2_WINDOW_BITS)) {
      resp = &mod_config()->http2_upstream_window_bits;
    } else {
      resp = &mod_config()->http2_downstream_window_bits;
    }

    errno = 0;

    int n;

    if (parse_uint(&n, opt, optarg) != 0) {
      return -1;
    }

    if (n >= 31) {
      LOG(ERROR) << opt
                 << ": specify the integer in the range [0, 30], inclusive";
      return -1;
    }

    *resp = n;

    return 0;
  }

  if (util::strieq(opt, SHRPX_OPT_FRONTEND_HTTP2_CONNECTION_WINDOW_BITS) ||
      util::strieq(opt, SHRPX_OPT_BACKEND_HTTP2_CONNECTION_WINDOW_BITS)) {

    size_t *resp;

    if (util::strieq(opt, SHRPX_OPT_FRONTEND_HTTP2_CONNECTION_WINDOW_BITS)) {
      resp = &mod_config()->http2_upstream_connection_window_bits;
    } else {
      resp = &mod_config()->http2_downstream_connection_window_bits;
    }

    errno = 0;

    int n;

    if (parse_uint(&n, opt, optarg) != 0) {
      return -1;
    }

    if (n < 16 || n >= 31) {
      LOG(ERROR) << opt
                 << ": specify the integer in the range [16, 30], inclusive";
      return -1;
    }

    *resp = n;

    return 0;
  }

  if (util::strieq(opt, SHRPX_OPT_FRONTEND_NO_TLS)) {
    mod_config()->upstream_no_tls = util::strieq(optarg, "yes");

    return 0;
  }

  if (util::strieq(opt, SHRPX_OPT_BACKEND_NO_TLS)) {
    mod_config()->downstream_no_tls = util::strieq(optarg, "yes");

    return 0;
  }

  if (util::strieq(opt, SHRPX_OPT_BACKEND_TLS_SNI_FIELD)) {
    mod_config()->backend_tls_sni_name = strcopy(optarg);

    return 0;
  }

  if (util::strieq(opt, SHRPX_OPT_PID_FILE)) {
    mod_config()->pid_file = strcopy(optarg);

    return 0;
  }

  if (util::strieq(opt, SHRPX_OPT_USER)) {
    auto pwd = getpwnam(optarg);
    if (!pwd) {
      LOG(ERROR) << opt << ": failed to get uid from " << optarg << ": "
                 << strerror(errno);
      return -1;
    }
    mod_config()->user = strcopy(pwd->pw_name);
    mod_config()->uid = pwd->pw_uid;
    mod_config()->gid = pwd->pw_gid;

    return 0;
  }

  if (util::strieq(opt, SHRPX_OPT_PRIVATE_KEY_FILE)) {
    mod_config()->private_key_file = strcopy(optarg);

    return 0;
  }

  if (util::strieq(opt, SHRPX_OPT_PRIVATE_KEY_PASSWD_FILE)) {
    auto passwd = read_passwd_from_file(optarg);
    if (passwd.empty()) {
      LOG(ERROR) << opt << ": Couldn't read key file's passwd from " << optarg;
      return -1;
    }
    mod_config()->private_key_passwd = strcopy(passwd);

    return 0;
  }

  if (util::strieq(opt, SHRPX_OPT_CERTIFICATE_FILE)) {
    mod_config()->cert_file = strcopy(optarg);

    return 0;
  }

  if (util::strieq(opt, SHRPX_OPT_DH_PARAM_FILE)) {
    mod_config()->dh_param_file = strcopy(optarg);

    return 0;
  }

  if (util::strieq(opt, SHRPX_OPT_SUBCERT)) {
    // Private Key file and certificate file separated by ':'.
    const char *sp = strchr(optarg, ':');
    if (sp) {
      std::string keyfile(optarg, sp);
      // TODO Do we need private key for subcert?
      mod_config()->subcerts.emplace_back(keyfile, sp + 1);
    }

    return 0;
  }

  if (util::strieq(opt, SHRPX_OPT_SYSLOG_FACILITY)) {
    int facility = int_syslog_facility(optarg);
    if (facility == -1) {
      LOG(ERROR) << opt << ": Unknown syslog facility: " << optarg;
      return -1;
    }
    mod_config()->syslog_facility = facility;

    return 0;
  }

  if (util::strieq(opt, SHRPX_OPT_BACKLOG)) {
    int n;
    if (parse_int(&n, opt, optarg) != 0) {
      return -1;
    }

    if (n < -1) {
      LOG(ERROR) << opt << ": " << optarg << " is not allowed";

      return -1;
    }

    mod_config()->backlog = n;

    return 0;
  }

  if (util::strieq(opt, SHRPX_OPT_CIPHERS)) {
    mod_config()->ciphers = strcopy(optarg);

    return 0;
  }

  if (util::strieq(opt, SHRPX_OPT_CLIENT)) {
    mod_config()->client = util::strieq(optarg, "yes");

    return 0;
  }

  if (util::strieq(opt, SHRPX_OPT_INSECURE)) {
    mod_config()->insecure = util::strieq(optarg, "yes");

    return 0;
  }

  if (util::strieq(opt, SHRPX_OPT_CACERT)) {
    mod_config()->cacert = strcopy(optarg);

    return 0;
  }

  if (util::strieq(opt, SHRPX_OPT_BACKEND_IPV4)) {
    mod_config()->backend_ipv4 = util::strieq(optarg, "yes");

    return 0;
  }

  if (util::strieq(opt, SHRPX_OPT_BACKEND_IPV6)) {
    mod_config()->backend_ipv6 = util::strieq(optarg, "yes");

    return 0;
  }

  if (util::strieq(opt, SHRPX_OPT_BACKEND_HTTP_PROXY_URI)) {
    // parse URI and get hostname, port and optionally userinfo.
    http_parser_url u;
    memset(&u, 0, sizeof(u));
    int rv = http_parser_parse_url(optarg, strlen(optarg), 0, &u);
    if (rv == 0) {
      std::string val;
      if (u.field_set & UF_USERINFO) {
        http2::copy_url_component(val, &u, UF_USERINFO, optarg);
        // Surprisingly, u.field_set & UF_USERINFO is nonzero even if
        // userinfo component is empty string.
        if (!val.empty()) {
          val = util::percentDecode(val.begin(), val.end());
          mod_config()->downstream_http_proxy_userinfo = strcopy(val);
        }
      }
      if (u.field_set & UF_HOST) {
        http2::copy_url_component(val, &u, UF_HOST, optarg);
        mod_config()->downstream_http_proxy_host = strcopy(val);
      } else {
        LOG(ERROR) << opt << ": no hostname specified";
        return -1;
      }
      if (u.field_set & UF_PORT) {
        mod_config()->downstream_http_proxy_port = u.port;
      } else {
        LOG(ERROR) << opt << ": no port specified";
        return -1;
      }
    } else {
      LOG(ERROR) << opt << ": parse error";
      return -1;
    }

    return 0;
  }

  if (util::strieq(opt, SHRPX_OPT_READ_RATE)) {
    return parse_uint_with_unit(&mod_config()->read_rate, opt, optarg);
  }

  if (util::strieq(opt, SHRPX_OPT_READ_BURST)) {
    return parse_uint_with_unit(&mod_config()->read_burst, opt, optarg);
  }

  if (util::strieq(opt, SHRPX_OPT_WRITE_RATE)) {
    return parse_uint_with_unit(&mod_config()->write_rate, opt, optarg);
  }

  if (util::strieq(opt, SHRPX_OPT_WRITE_BURST)) {
    return parse_uint_with_unit(&mod_config()->write_burst, opt, optarg);
  }

  if (util::strieq(opt, SHRPX_OPT_WORKER_READ_RATE)) {
    LOG(WARN) << opt << ": not implemented yet";
    return parse_uint_with_unit(&mod_config()->worker_read_rate, opt, optarg);
  }

  if (util::strieq(opt, SHRPX_OPT_WORKER_READ_BURST)) {
    LOG(WARN) << opt << ": not implemented yet";
    return parse_uint_with_unit(&mod_config()->worker_read_burst, opt, optarg);
  }

  if (util::strieq(opt, SHRPX_OPT_WORKER_WRITE_RATE)) {
    LOG(WARN) << opt << ": not implemented yet";
    return parse_uint_with_unit(&mod_config()->worker_write_rate, opt, optarg);
  }

  if (util::strieq(opt, SHRPX_OPT_WORKER_WRITE_BURST)) {
    LOG(WARN) << opt << ": not implemented yet";
    return parse_uint_with_unit(&mod_config()->worker_write_burst, opt, optarg);
  }

  if (util::strieq(opt, SHRPX_OPT_NPN_LIST)) {
    clear_config_str_list(mod_config()->npn_list);

    mod_config()->npn_list = parse_config_str_list(optarg);

    return 0;
  }

  if (util::strieq(opt, SHRPX_OPT_TLS_PROTO_LIST)) {
    clear_config_str_list(mod_config()->tls_proto_list);

    mod_config()->tls_proto_list = parse_config_str_list(optarg);

    return 0;
  }

  if (util::strieq(opt, SHRPX_OPT_VERIFY_CLIENT)) {
    mod_config()->verify_client = util::strieq(optarg, "yes");

    return 0;
  }

  if (util::strieq(opt, SHRPX_OPT_VERIFY_CLIENT_CACERT)) {
    mod_config()->verify_client_cacert = strcopy(optarg);

    return 0;
  }

  if (util::strieq(opt, SHRPX_OPT_CLIENT_PRIVATE_KEY_FILE)) {
    mod_config()->client_private_key_file = strcopy(optarg);

    return 0;
  }

  if (util::strieq(opt, SHRPX_OPT_CLIENT_CERT_FILE)) {
    mod_config()->client_cert_file = strcopy(optarg);

    return 0;
  }

  if (util::strieq(opt, SHRPX_OPT_FRONTEND_HTTP2_DUMP_REQUEST_HEADER)) {
    mod_config()->http2_upstream_dump_request_header_file = strcopy(optarg);

    return 0;
  }

  if (util::strieq(opt, SHRPX_OPT_FRONTEND_HTTP2_DUMP_RESPONSE_HEADER)) {
    mod_config()->http2_upstream_dump_response_header_file = strcopy(optarg);

    return 0;
  }

  if (util::strieq(opt, SHRPX_OPT_HTTP2_NO_COOKIE_CRUMBLING)) {
    mod_config()->http2_no_cookie_crumbling = util::strieq(optarg, "yes");

    return 0;
  }

  if (util::strieq(opt, SHRPX_OPT_FRONTEND_FRAME_DEBUG)) {
    mod_config()->upstream_frame_debug = util::strieq(optarg, "yes");

    return 0;
  }

  if (util::strieq(opt, SHRPX_OPT_PADDING)) {
    return parse_uint(&mod_config()->padding, opt, optarg);
  }

  if (util::strieq(opt, SHRPX_OPT_ALTSVC)) {
    auto tokens = parse_config_str_list(optarg);

    if (tokens.size() < 2) {
      // Requires at least protocol_id and port
      LOG(ERROR) << opt << ": too few parameters: " << optarg;
      return -1;
    }

    if (tokens.size() > 4) {
      // We only need protocol_id, port, host and origin
      LOG(ERROR) << opt << ": too many parameters: " << optarg;
      return -1;
    }

    int port;

    if (parse_uint(&port, opt, tokens[1]) != 0) {
      return -1;
    }

    if (port < 1 ||
        port > static_cast<int>(std::numeric_limits<uint16_t>::max())) {
      LOG(ERROR) << opt << ": port is invalid: " << tokens[1];
      return -1;
    }

    AltSvc altsvc;

    altsvc.port = port;

    altsvc.protocol_id = tokens[0];
    altsvc.protocol_id_len = strlen(altsvc.protocol_id);

    if (tokens.size() > 2) {
      altsvc.host = tokens[2];
      altsvc.host_len = strlen(altsvc.host);

      if (tokens.size() > 3) {
        altsvc.origin = tokens[3];
        altsvc.origin_len = strlen(altsvc.origin);
      }
    }

    mod_config()->altsvcs.push_back(std::move(altsvc));

    return 0;
  }

  if (util::strieq(opt, SHRPX_OPT_ADD_REQUEST_HEADER) ||
      util::strieq(opt, SHRPX_OPT_ADD_RESPONSE_HEADER)) {
    auto p = parse_header(optarg);
    if (p.first.empty()) {
      LOG(ERROR) << opt << ": header field name is empty: " << optarg;
      return -1;
    }
    if (util::strieq(opt, SHRPX_OPT_ADD_REQUEST_HEADER)) {
      mod_config()->add_request_headers.push_back(std::move(p));
    } else {
      mod_config()->add_response_headers.push_back(std::move(p));
    }
    return 0;
  }

  if (util::strieq(opt, SHRPX_OPT_WORKER_FRONTEND_CONNECTIONS)) {
    return parse_uint(&mod_config()->worker_frontend_connections, opt, optarg);
  }

  if (util::strieq(opt, SHRPX_OPT_NO_LOCATION_REWRITE)) {
    mod_config()->no_location_rewrite = util::strieq(optarg, "yes");

    return 0;
  }

  if (util::strieq(opt, SHRPX_OPT_NO_HOST_REWRITE)) {
    mod_config()->no_host_rewrite = util::strieq(optarg, "yes");

    return 0;
  }

  if (util::strieq(opt, SHRPX_OPT_BACKEND_HTTP1_CONNECTIONS_PER_HOST)) {
    int n;

    if (parse_uint(&n, opt, optarg) != 0) {
      return -1;
    }

    if (n == 0) {
      LOG(ERROR) << opt << ": specify an integer strictly more than 0";

      return -1;
    }

    mod_config()->downstream_connections_per_host = n;

    return 0;
  }

  if (util::strieq(opt, SHRPX_OPT_BACKEND_HTTP1_CONNECTIONS_PER_FRONTEND)) {
    return parse_uint(&mod_config()->downstream_connections_per_frontend, opt,
                      optarg);
  }

  if (util::strieq(opt, SHRPX_OPT_LISTENER_DISABLE_TIMEOUT)) {
    return parse_duration(&mod_config()->listener_disable_timeout, opt, optarg);
  }

  if (util::strieq(opt, SHRPX_OPT_TLS_TICKET_KEY_FILE)) {
    mod_config()->tls_ticket_key_files.push_back(optarg);
    return 0;
  }

  if (util::strieq(opt, SHRPX_OPT_RLIMIT_NOFILE)) {
    int n;

    if (parse_uint(&n, opt, optarg) != 0) {
      return -1;
    }

    if (n < 0) {
      LOG(ERROR) << opt << ": specify the integer more than or equal to 0";

      return -1;
    }

    mod_config()->rlimit_nofile = n;

    return 0;
  }

  if (util::strieq(opt, SHRPX_OPT_BACKEND_REQUEST_BUFFER) ||
      util::strieq(opt, SHRPX_OPT_BACKEND_RESPONSE_BUFFER)) {
    size_t n;
    if (parse_uint_with_unit(&n, opt, optarg) != 0) {
      return -1;
    }

    if (n == 0) {
      LOG(ERROR) << opt << ": specify an integer strictly more than 0";

      return -1;
    }

    if (util::strieq(opt, SHRPX_OPT_BACKEND_REQUEST_BUFFER)) {
      mod_config()->downstream_request_buffer_size = n;
    } else {
      mod_config()->downstream_response_buffer_size = n;
    }

    return 0;
  }

  if (util::strieq(opt, SHRPX_OPT_NO_SERVER_PUSH)) {
    mod_config()->no_server_push = util::strieq(optarg, "yes");

    return 0;
  }

  if (util::strieq(opt, SHRPX_OPT_BACKEND_HTTP2_CONNECTIONS_PER_WORKER)) {
    return parse_uint(&mod_config()->http2_downstream_connections_per_worker,
                      opt, optarg);
  }

  if (util::strieq(opt, SHRPX_OPT_FETCH_OCSP_RESPONSE_FILE)) {
    mod_config()->fetch_ocsp_response_file = strcopy(optarg);

    return 0;
  }

  if (util::strieq(opt, SHRPX_OPT_OCSP_UPDATE_INTERVAL)) {
    return parse_duration(&mod_config()->ocsp_update_interval, opt, optarg);
  }

  if (util::strieq(opt, SHRPX_OPT_NO_OCSP)) {
    mod_config()->no_ocsp = util::strieq(optarg, "yes");

    return 0;
  }

  if (util::strieq(opt, SHRPX_OPT_HEADER_FIELD_BUFFER)) {
    return parse_uint_with_unit(&mod_config()->header_field_buffer, opt,
                                optarg);
  }

  if (util::strieq(opt, SHRPX_OPT_MAX_HEADER_FIELDS)) {
    return parse_uint(&mod_config()->max_header_fields, opt, optarg);
  }

  if (util::strieq(opt, SHRPX_OPT_INCLUDE)) {
    if (included_set.count(optarg)) {
      LOG(ERROR) << opt << ": " << optarg << " has already been included";
      return -1;
    }

    included_set.emplace(optarg);
    auto rv = load_config(optarg, included_set);
    included_set.erase(optarg);

    if (rv != 0) {
      return -1;
    }

    return 0;
  }

  if (util::strieq(opt, "conf")) {
    LOG(WARN) << "conf: ignored";

    return 0;
  }

  LOG(ERROR) << "Unknown option: " << opt;

  return -1;
}

int load_config(const char *filename, std::set<std::string> &include_set) {
  std::ifstream in(filename, std::ios::binary);
  if (!in) {
    LOG(ERROR) << "Could not open config file " << filename;
    return -1;
  }
  std::string line;
  int linenum = 0;
  while (std::getline(in, line)) {
    ++linenum;
    if (line.empty() || line[0] == '#') {
      continue;
    }
    size_t i;
    size_t size = line.size();
    for (i = 0; i < size && line[i] != '='; ++i)
      ;
    if (i == size) {
      LOG(ERROR) << "Bad configuration format in " << filename << " at line "
                 << linenum;
      return -1;
    }
    line[i] = '\0';
    auto s = line.c_str();
    if (parse_config(s, s + i + 1, include_set) == -1) {
      return -1;
    }
  }
  return 0;
}

const char *str_syslog_facility(int facility) {
  switch (facility) {
  case (LOG_AUTH):
    return "auth";
#ifdef LOG_AUTHPRIV
  case (LOG_AUTHPRIV):
    return "authpriv";
#endif // LOG_AUTHPRIV
  case (LOG_CRON):
    return "cron";
  case (LOG_DAEMON):
    return "daemon";
#ifdef LOG_FTP
  case (LOG_FTP):
    return "ftp";
#endif // LOG_FTP
  case (LOG_KERN):
    return "kern";
  case (LOG_LOCAL0):
    return "local0";
  case (LOG_LOCAL1):
    return "local1";
  case (LOG_LOCAL2):
    return "local2";
  case (LOG_LOCAL3):
    return "local3";
  case (LOG_LOCAL4):
    return "local4";
  case (LOG_LOCAL5):
    return "local5";
  case (LOG_LOCAL6):
    return "local6";
  case (LOG_LOCAL7):
    return "local7";
  case (LOG_LPR):
    return "lpr";
  case (LOG_MAIL):
    return "mail";
  case (LOG_SYSLOG):
    return "syslog";
  case (LOG_USER):
    return "user";
  case (LOG_UUCP):
    return "uucp";
  default:
    return "(unknown)";
  }
}

int int_syslog_facility(const char *strfacility) {
  if (util::strieq(strfacility, "auth")) {
    return LOG_AUTH;
  }

#ifdef LOG_AUTHPRIV
  if (util::strieq(strfacility, "authpriv")) {
    return LOG_AUTHPRIV;
  }
#endif // LOG_AUTHPRIV

  if (util::strieq(strfacility, "cron")) {
    return LOG_CRON;
  }

  if (util::strieq(strfacility, "daemon")) {
    return LOG_DAEMON;
  }

#ifdef LOG_FTP
  if (util::strieq(strfacility, "ftp")) {
    return LOG_FTP;
  }
#endif // LOG_FTP

  if (util::strieq(strfacility, "kern")) {
    return LOG_KERN;
  }

  if (util::strieq(strfacility, "local0")) {
    return LOG_LOCAL0;
  }

  if (util::strieq(strfacility, "local1")) {
    return LOG_LOCAL1;
  }

  if (util::strieq(strfacility, "local2")) {
    return LOG_LOCAL2;
  }

  if (util::strieq(strfacility, "local3")) {
    return LOG_LOCAL3;
  }

  if (util::strieq(strfacility, "local4")) {
    return LOG_LOCAL4;
  }

  if (util::strieq(strfacility, "local5")) {
    return LOG_LOCAL5;
  }

  if (util::strieq(strfacility, "local6")) {
    return LOG_LOCAL6;
  }

  if (util::strieq(strfacility, "local7")) {
    return LOG_LOCAL7;
  }

  if (util::strieq(strfacility, "lpr")) {
    return LOG_LPR;
  }

  if (util::strieq(strfacility, "mail")) {
    return LOG_MAIL;
  }

  if (util::strieq(strfacility, "news")) {
    return LOG_NEWS;
  }

  if (util::strieq(strfacility, "syslog")) {
    return LOG_SYSLOG;
  }

  if (util::strieq(strfacility, "user")) {
    return LOG_USER;
  }

  if (util::strieq(strfacility, "uucp")) {
    return LOG_UUCP;
  }

  return -1;
}

namespace {
template <typename InputIt>
bool path_match(const std::string &pattern, const std::string &host,
                InputIt path_first, InputIt path_last) {
  if (pattern.back() != '/') {
    return pattern.size() == host.size() + (path_last - path_first) &&
           std::equal(std::begin(host), std::end(host), std::begin(pattern)) &&
           std::equal(path_first, path_last, std::begin(pattern) + host.size());
  }

  if (pattern.size() >= host.size() &&
      std::equal(std::begin(host), std::end(host), std::begin(pattern)) &&
      util::startsWith(path_first, path_last, std::begin(pattern) + host.size(),
                       std::end(pattern))) {
    return true;
  }

  // If pattern ends with '/', and pattern and path matches without
  // that slash, we consider they match to deal with request to the
  // directory without trailing slash.  That is if pattern is "/foo/"
  // and path is "/foo", we consider they match.

  assert(!pattern.empty());
  return pattern.size() - 1 == host.size() + (path_last - path_first) &&
         std::equal(std::begin(host), std::end(host), std::begin(pattern)) &&
         std::equal(path_first, path_last, std::begin(pattern) + host.size());
}
} // namespace

namespace {
template <typename InputIt>
ssize_t match(const std::string &host, InputIt path_first, InputIt path_last,
              const std::vector<DownstreamAddrGroup> &groups) {
  ssize_t res = -1;
  size_t best = 0;
  for (size_t i = 0; i < groups.size(); ++i) {
    auto &g = groups[i];
    auto &pattern = g.pattern;
    if (!path_match(pattern, host, path_first, path_last)) {
      continue;
    }
    if (res == -1 || best < pattern.size()) {
      best = pattern.size();
      res = i;
    }
  }
  return res;
}
} // namespace

namespace {
template <typename InputIt>
size_t match_downstream_addr_group_host(
    const std::string &host, InputIt path_first, InputIt path_last,
    const std::vector<DownstreamAddrGroup> &groups, size_t catch_all) {
  if (path_first == path_last || *path_first != '/') {
    constexpr const char P[] = "/";
    auto group = match(host, P, P + 1, groups);
    if (group != -1) {
      if (LOG_ENABLED(INFO)) {
        LOG(INFO) << "Found pattern with query " << host
                  << ", matched pattern=" << groups[group].pattern;
      }
      return group;
    }
    return catch_all;
  }

  if (LOG_ENABLED(INFO)) {
    LOG(INFO) << "Perform mapping selection, using host=" << host
              << ", path=" << std::string(path_first, path_last);
  }

  auto group = match(host, path_first, path_last, groups);
  if (group != -1) {
    if (LOG_ENABLED(INFO)) {
      LOG(INFO) << "Found pattern with query " << host
                << std::string(path_first, path_last)
                << ", matched pattern=" << groups[group].pattern;
    }
    return group;
  }

  group = match("", path_first, path_last, groups);
  if (group != -1) {
    if (LOG_ENABLED(INFO)) {
      LOG(INFO) << "Found pattern with query "
                << std::string(path_first, path_last)
                << ", matched pattern=" << groups[group].pattern;
    }
    return group;
  }

  if (LOG_ENABLED(INFO)) {
    LOG(INFO) << "None match.  Use catch-all pattern";
  }
  return catch_all;
}
} // namespace

size_t match_downstream_addr_group(
    const std::string &hostport, const std::string &raw_path,
    const std::vector<DownstreamAddrGroup> &groups, size_t catch_all) {
  if (std::find(std::begin(hostport), std::end(hostport), '/') !=
      std::end(hostport)) {
    // We use '/' specially, and if '/' is included in host, it breaks
    // our code.  Select catch-all case.
    return catch_all;
  }

  auto fragment = std::find(std::begin(raw_path), std::end(raw_path), '#');
  auto query = std::find(std::begin(raw_path), fragment, '?');
  auto path_first = std::begin(raw_path);
  auto path_last = query;

  if (hostport.empty()) {
    return match_downstream_addr_group_host(hostport, path_first, path_last,
                                            groups, catch_all);
  }

  std::string host;
  if (hostport[0] == '[') {
    // assume this is IPv6 numeric address
    auto p = std::find(std::begin(hostport), std::end(hostport), ']');
    if (p == std::end(hostport)) {
      return catch_all;
    }
    if (p + 1 < std::end(hostport) && *(p + 1) != ':') {
      return catch_all;
    }
    host.assign(std::begin(hostport), p + 1);
  } else {
    auto p = std::find(std::begin(hostport), std::end(hostport), ':');
    if (p == std::begin(hostport)) {
      return catch_all;
    }
    host.assign(std::begin(hostport), p);
  }

  util::inp_strlower(host);
  return match_downstream_addr_group_host(host, path_first, path_last, groups,
                                          catch_all);
}

} // namespace shrpx
