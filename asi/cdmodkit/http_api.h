#pragma once
#include <string>

// Loopback-only HTTP API (HTTP_API.md). Off by default; the Settings tab checkbox (settings.txt http_api=1) starts and stops it at runtime.
namespace httpapi {
    bool Start(int port);        // binds 127.0.0.1:port and serves on a background thread; restarts on a new port; false = see LastError
    void Stop();                 // closes the listener at once; a request in progress still gets its answer
    int ActivePort();            // 0 = not running
    std::string LastError();     // why the last Start failed ("" after a successful one)
}
