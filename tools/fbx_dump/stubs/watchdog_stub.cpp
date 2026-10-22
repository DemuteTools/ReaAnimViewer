// SPDX-License-Identifier: MIT
//
// Link-time stand-in for the plugin's freeze watchdog (src/watchdog.cpp): the load path leaves
// breadcrumbs and scopes (RAV_SCOPE), which here do nothing. The command-line dump has no main
// thread to watch.
#include "watchdog.h"

namespace rav {
namespace wd {

void Crumb(const char*, const char*) {}
void CrumbNum(const char*, long long) {}
Scope::Scope(const char*, const char*) {}
Scope::Scope(const char*, long long) {}
Scope::~Scope() {}
void Scope::Enter(const char*, const char*, bool) {}
void Scope::EnterNum(const char*, long long, bool) {}
void AudioEnter(const char*) {}
void AudioExit() {}
void AudioArg(const char*) {}
void VideoEnter(const char*, const char*) {}
void VideoExit() {}
int MainDepth() { return 0; }
void MainBeat() {}
int WorkerSlot(const char*, int) { return -1; }
void WorkerBusy(int, const char*, const char*) {}
void WorkerIdle(int) {}

}  // namespace wd
}  // namespace rav
