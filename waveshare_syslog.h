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

#include <cstdint>
#include <cstdio>
#include <cstring>
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

// Safe to call from any context, including logger callbacks that may fire
// from early boot or FreeRTOS/lwIP internals: no heap allocation, no
// blocking, no networking - just copies into the next ring slot.
inline void enqueue(int severity, const char *tag, const char *message) {
  uint32_t idx = head % QUEUE_SIZE;
  Entry &e = queue[idx];
  e.severity = severity;
  strncpy(e.tag, tag, sizeof(e.tag) - 1);
  e.tag[sizeof(e.tag) - 1] = '\0';
  strncpy(e.message, message, sizeof(e.message) - 1);
  e.message[sizeof(e.message) - 1] = '\0';
  e.valid = true;
  head++;
}

inline void send_one(const char *host, uint16_t port, int severity, const char *tag, const char *message) {
  static int sock = -1;
  if (sock < 0) {
    sock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0)
      return;
  }

  struct sockaddr_in dest {};
  dest.sin_family = AF_INET;
  dest.sin_port = htons(port);
  dest.sin_addr.s_addr = inet_addr(host);

  const int facility = 16;  // local0 - arbitrary but consistent for this device
  int pri = facility * 8 + severity;

  char buf[256];
  snprintf(buf, sizeof(buf), "<%d>garden-weather-sensor-ws %s: %s", pri, tag, message);
  ::sendto(sock, buf, strlen(buf), 0, (struct sockaddr *) &dest, sizeof(dest));
}

// Only ever call this from the main loop task (e.g. an interval: component) -
// this is where the actual socket I/O happens.
inline void drain(const char *host, uint16_t port) {
  while (tail != head) {
    uint32_t idx = tail % QUEUE_SIZE;
    tail++;
    Entry &e = queue[idx];
    if (!e.valid)
      continue;
    e.valid = false;
    send_one(host, port, e.severity, e.tag, e.message);
  }
}

}  // namespace waveshare_syslog
