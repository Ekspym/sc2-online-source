/* SC2 Online: GGPO's platform, the same for every system (T6's Windows one, made portable) */
#ifndef _GGPO_PLATFORM_H_
#define _GGPO_PLATFORM_H_

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int SOCKET;
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define closesocket close
#define WSAGetLastError() errno
#define WSAEWOULDBLOCK EWOULDBLOCK
#define __cdecl
#endif
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#ifndef FALSE
#define FALSE 0
#define TRUE 1
#endif
#undef sprintf_s
#undef vsprintf_s
#undef strcpy_s
#define sprintf_s snprintf
#define vsprintf_s vsnprintf
#define strcpy_s(to, from) snprintf((to), sizeof(to), "%s", (from))

extern int g_ggpo_network_delay;   /* (tests: each packet sent this late, ms) */

class Platform {
public:
   static int GetProcessID() { return 0; }
   static void AssertFailed(char *msg) { fprintf(stderr, "ggpo: %s\n", msg); fflush(stderr); }
   static unsigned int GetCurrentTimeMS() {
      return (unsigned int)std::chrono::duration_cast<std::chrono::milliseconds>(
         std::chrono::steady_clock::now().time_since_epoch()).count();
   }
   static int GetConfigInt(const char *name) { return strcmp(name, "ggpo.network.delay") == 0 ? g_ggpo_network_delay : 0; }
   static bool GetConfigBool(const char *) { return false; }
};

#endif
