#pragma once

#include "http_client.h"

// One function per command (design.md §3.3). Each returns the process exit code (§3.6: fixed
// 0/1, never a raw HTTP status).
int commandPlay(HttpClient& client, int argc, char** argv);
int commandPause(HttpClient& client);
int commandResume(HttpClient& client);
int commandStop(HttpClient& client);
int commandNext(HttpClient& client);
int commandPrevious(HttpClient& client);
int commandList(HttpClient& client, int argc, char** argv);

void printUsage();
