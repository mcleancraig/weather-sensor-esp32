#pragma once

// Fire-and-forget UDP syslog (RFC 3164) mirroring of ESPHome's log stream.
//
// IMPORTANT: logger's on_message trigger can fire from contexts where doing
// actual network I/O is unsafe (very early boot, or from deep inside
// FreeRTOS/lwIP internals - a crash was observed here with the socket call
// made directly in on_message: "Illegal instruction" / assert failure with
// esp_vApplicationTickHook on the stack). So on_message only does a cheap,
// non-blocking copy into a small fixed-size ring buffer (waveshare_syslog_enqueue);
// the actual socket send (waveshare_syslog_drain) only ever runs from a
// regular interval: component in the normal main-loop task, where it's safe.

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "esphome/core/log.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

namespace waveshare_syslog {

constexpr int QUEUE_SIZE = 32;
constexpr int TAG_MAX = 24;
constexpr int MESSAGE_MAX = 160;

struct Entry {
  bool valid;
  int severity;
  char tag[TAG_MAX];
  char message[MESSAGE_MAX];
};

// static (not C++17 "inline variables", to avoid depending on a C++
// standard newer than what ESP-IDF's toolchain may be configured for) -
// safe here since this header is only ever included once, into the single
// generated main.cpp.
static Entry queue[QUEUE_SIZE];
static volatile uint32_t head = 0;
static volatile uint32_t tail = 0;

// ESPHome's logger colors terminal output using ANSI CSI escape sequences
// (ESC '[' ... 'm'). Those raw control bytes end up in the `message` string
// passed to on_message, and a strict syslog/Loki parser silently drops any
// message containing them (confirmed via tcpdump: packets reach the syslog
// receiver's NIC but never appear in Loki - the receiver rejects them at
// the parsing stage). So strip them here rather than forwarding them.
inline void copy_stripping_ansi(char *dst, size_t dst_size, const char *src) {
  size_t si = 0, di = 0;
  while (src[si] != '\0' && di + 1 < dst_size) {
    unsigned char c = static_cast<unsigned char>(src[si]);
    if (c == 0x1B && src[si + 1] == '[') {
      si += 2;  // skip ESC '['
      // skip parameter/intermediate bytes up to and including the final
      // byte (0x40-0x7E) that ends a CSI sequence, e.g. the 'm' in \x1b[0m
      while (src[si] != '\0' && !(static_cast<unsigned char>(src[si]) >= 0x40 && static_cast<unsigned char>(src[si]) <= 0x7E)) {
        si++;
      }
      if (src[si] != '\0')
        si++;
      continue;
    }
    if (c < 0x20 || c == 0x7F) {
      // Any other stray control byte, including a lone ESC not part of a
      // full CSI sequence (e.g. truncated right at that boundary) - drop
      // it rather than forward a raw control character into the message.
      si++;
      continue;
    }
    dst[di++] = static_cast<char>(c);
    si++;
  }
  dst[di] = '\0';
}

// Safe to call from any context, including logger callbacks that may fire
// from early boot or FreeRTOS/lwIP internals: no heap allocation, no
// blocking, no networking - just copies into the next ring slot.
inline void enqueue(int severity, const char *tag, const char *message) {
  uint32_t idx = head % QUEUE_SIZE;
  Entry &e = queue[idx];
  e.severity = severity;
  strncpy(e.tag, tag, sizeof(e.tag) - 1);
  e.tag[sizeof(e.tag) - 1] = '\0';
  copy_stripping_ansi(e.message, sizeof(e.message), message);
  e.valid = true;
  head++;
}

// send_one (and therefore this diagnostic logging) is only ever reached via
// drain(), which only ever runs from the interval: component's main-loop
// context - a normal, safe place to call ESP_LOGW, unlike on_message.
inline void report_send_result(int rc) {
  static uint32_t consecutive_failures = 0;
  static bool ever_failed = false;

  if (rc < 0) {
    consecutive_failures++;
    ever_failed = true;
    // Only the 1st failure and then every ~30s (150 * 200ms interval) after
    // that, so a persistently unreachable receiver doesn't itself flood the
    // serial/API log we actually rely on.
    if (consecutive_failures == 1 || consecutive_failures % 150 == 0) {
      ESP_LOGW("waveshare_syslog", "sendto() failed (errno=%d, %s), %u consecutive", errno, strerror(errno),
               consecutive_failures);
    }
  } else if (ever_failed && consecutive_failures > 0) {
    ESP_LOGI("waveshare_syslog", "sendto() recovered after %u consecutive failures", consecutive_failures);
    consecutive_failures = 0;
  }
}

// Format: <PRI>TIMESTAMP HOSTNAME APP[PROCID]: MSG - mirrors the sibling
// moisture-sensor-esp32 device's proven-working syslogSend() exactly
// (same receiver/parser), rather than the earlier no-timestamp format that
// never showed up in Loki despite reaching the receiver's NIC (confirmed
// via tcpdump). `timestamp` is precomputed by the caller (from a real
// NTP-synced clock, "%b %e %T" format, e.g. "Jul 17 20:11:28") since this
// header has no ESPHome time-component dependency of its own.
inline void send_one(const char *host, uint16_t port, const char *timestamp, int severity, const char *tag,
                      const char *message) {
  static int sock = -1;
  if (sock < 0) {
    sock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
      report_send_result(-1);
      return;
    }
  }

  struct sockaddr_in dest {};
  dest.sin_family = AF_INET;
  dest.sin_port = htons(port);
  dest.sin_addr.s_addr = inet_addr(host);

  const int facility = 16;  // local0 - arbitrary but consistent for this device
  int pri = facility * 8 + severity;

  char buf[256];
  snprintf(buf, sizeof(buf), "<%d>%s garden-weather-sensor-ws garden-weather-sensor-ws[%s]: %s", pri, timestamp, tag,
           message);
  int rc = ::sendto(sock, buf, strlen(buf), 0, (struct sockaddr *) &dest, sizeof(dest));
  report_send_result(rc);
}

// Only ever call this from the main loop task (e.g. an interval: component) -
// this is where the actual socket I/O happens.
inline void drain(const char *host, uint16_t port, const char *timestamp) {
  while (tail != head) {
    uint32_t idx = tail % QUEUE_SIZE;
    tail++;
    Entry &e = queue[idx];
    if (!e.valid)
      continue;
    e.valid = false;
    send_one(host, port, timestamp, e.severity, e.tag, e.message);
  }
}

}  // namespace waveshare_syslog
