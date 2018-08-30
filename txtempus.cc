// -*- mode: c++; c-basic-offset: 2; indent-tabs-mode: nil; -*-
// Copyright (C) 2018 Henner Zeller <h.zeller@acm.org>
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation version 2.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://gnu.org/licenses/gpl-2.0.txt>
//
// DCF77 simulating transmitter, to be run on the Raspberry Pi.
// Make sure to stay within the regulation limits of HF transmissions!

#include <getopt.h>
#include <limits.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <strings.h>

#include <string>

#include "gpio.h"
#include "time-signal-source.h"

// The GPIO bit that is pulled down for attenuation of the signal.
const uint32_t kAttenuationGPIOBit = (1<<17);

static bool verbose = false;
static bool dryrun = false;

namespace {
volatile sig_atomic_t interrupted = 0;
void InterruptHandler(int signo) { interrupted = signo; }

time_t TruncateTo(time_t t, int v) { return t - t % v; }

void WaitUntil(const struct timespec &ts) {
  if (dryrun) return;
  clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME, &ts, NULL);
}

void StartCarrier(GPIO *gpio, int frequency) {
  if (dryrun) return;
  double f = gpio->StartClock(frequency);
  if (verbose) {
    fprintf(stderr, "Requesting %d Hz, getting %.3f Hz carrier\n",
            frequency, f);
  }
}

void SetPower(GPIO *gpio, bool high) {
  if (dryrun) return;
  if (high) {
    gpio->RequestInput(kAttenuationGPIOBit);   // High-Z
  } else {
    gpio->RequestOutput(kAttenuationGPIOBit);  // Pull down.
    gpio->ClearBits(kAttenuationGPIOBit);
  }
}

time_t ParseTime(const char *time_string) {
  struct tm tm = {};
  const char *final_pos = strptime(time_string, "%Y-%m-%d %H:%M", &tm);
  if (!final_pos || *final_pos)
    return 0;
  tm.tm_isdst = -1;
  return mktime(&tm);
}

void PrintTime(time_t t) {
  char buf[32];
  struct tm tm;
  localtime_r(&t, &tm);
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
  fprintf(stderr, "%s", buf);
}

void PrintModulationChart(const TimeSignalSource::SecondModulation &mod) {
  fprintf(stderr, " [");
  const int kMsPerDash = 50;
  int running_ms = 0;
  int target_ms = 0;
  bool power = false;
  for (auto m : mod) {
    power = (m.power == CarrierPower::HIGH);
    target_ms += m.duration_ms;
    for (/**/; running_ms < target_ms; running_ms+=kMsPerDash)
      fprintf(stderr, "%s", power ? "#":"_");
  }
  for (/**/; running_ms < 1000; running_ms+=kMsPerDash)
    fprintf(stderr, "%s", power ? "#":"_");
  fprintf(stderr, "]\n");
}

int usage(const char *msg, const char *progname) {
  fprintf(stderr, "%susage: %s [options]\n"
          "Options:\n"
          "\t-s <service>          : Service; one of 'DCF77', 'WWVB'.\n"
          "\t-r <minutes>          : Run for limited number of minutes. "
          "(default: no limit)\n"  // in truth: a couple thousand years...
          "\t-t 'YYYY-MM-DD HH:MM' : Transmit the given local time "
          "(default: now)\n"
          "\t-v                    : Verbose.\n"
          "\t-n                    : Dryrun, only showing modulation "
          "envelope.\n"
          "\t-h                    : This help.\n",
          msg, progname);
  return 1;
}

}  // end anonymous namespace

int main(int argc, char *argv[]) {
  const time_t now = TruncateTo(time(NULL), 60);  // Time signals: full minute
  std::string service = "";
  time_t chosen_time = now;
  int ttl = INT_MAX;
  int opt;
  while ((opt = getopt(argc, argv, "t:r:vs:hn")) != -1) {
    switch (opt) {
    case 'v':
      verbose = true;
      break;
    case 't':
      chosen_time = ParseTime(optarg);
      if (chosen_time <= 0) return usage("Invalid time string\n", argv[0]);
      break;
    case 'r':
      ttl = atoi(optarg);
      break;
    case 's':
      service = optarg;
      break;
    case 'n':
      dryrun = true;
      verbose = true;
      ttl = 1;
      break;
    default:
      return usage("", argv[0]);
    }
  }

  const int time_offset = chosen_time - now;

  // Currently only DCF77 and WWVB, but more implementations about to follow
  // JJY, NPL
  TimeSignalSource *time_source = nullptr;
  if (strcasecmp(service.c_str(), "DCF77") == 0)
    time_source = new DCF77TimeSignalSource();
  else if (strcasecmp(service.c_str(), "WWVB") == 0)
    time_source = new WWVBTimeSignalSource();

  if (!time_source)
    return usage("Please choose a service name with -s option\n", argv[0]);

  GPIO gpio;
  if (!dryrun && !gpio.Init()) {
    fprintf(stderr, "Need to be root\n");
    return 1;
  }

  signal(SIGTERM, InterruptHandler);
  signal(SIGINT, InterruptHandler);

  // Make sure the kernel knows that we're serious about accuracy of sleeps.
  struct sched_param sp;
  sp.sched_priority = 99;
  sched_setscheduler(0, SCHED_FIFO, &sp);

  StartCarrier(&gpio, time_source->GetCarrierFrequencyHz());

  struct timespec target_wait;
  for (time_t minute_start = now; !interrupted && ttl--; minute_start += 60) {
    const time_t transmit_time = minute_start + time_offset;
    if (verbose) PrintTime(transmit_time);
    if (dryrun) fprintf(stderr, " -> tx-modulation\n");
    time_source->PrepareMinute(transmit_time);

    for (int second = 0; second < 60 && !interrupted; ++second) {
      const TimeSignalSource::SecondModulation &modulation
        = time_source->GetModulationForSecond(second);

      // First, let's wait until we reach the beginning of that second
      target_wait.tv_sec = minute_start + second;
      target_wait.tv_nsec = 0;
      WaitUntil(target_wait);
      if (interrupted) break;

      // Depending on the time source, there can be multiple amplitude
      // modulation changes per second.
      for (auto m : modulation) {
        SetPower(&gpio, m.power == CarrierPower::HIGH);
        if (m.duration_ms == 0) break; // last one.
        target_wait.tv_nsec += m.duration_ms * 1000000;
        WaitUntil(target_wait);
      }
      if (verbose) fprintf(stderr, "\b\b\b:%02d", second);
      if (dryrun) PrintModulationChart(modulation);
    }
    if (verbose) fprintf(stderr, "\n");
  }

  if (!dryrun) gpio.StopClock();
  delete time_source;
}