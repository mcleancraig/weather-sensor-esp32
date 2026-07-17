#pragma once

// Minimal fire-and-forget UDP syslog (RFC 3164) sender, used from the
// logger's on_message trigger in garden-weather-sensor-waveshare.yaml to
// mirror ESPHome's own log stream to an external syslog/Loki receiver.
//
// The socket is opened once and reused; sendto() on a UDP socket doesn't
// block waiting for a peer, so a receiver being briefly unreachable (e.g.
// during a WiFi reconnect) just drops packets rather than stalling the
// device.

#include <cstdio>
#include <cstring>
#include "lwip/sockets.h"
#include "lwip/inet.h"

inline void waveshare_send_syslog(const char *host, uint16_t port, int severity, const char *tag,
                                   const char *message) {
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
