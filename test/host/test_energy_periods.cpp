/*
 * Host test for lib/EnergyPeriods (boundary logic, DST, ISO weeks, month ends).
 *
 *   g++ -std=c++17 -Wall -I lib/EnergyPeriods/src test/host/test_energy_periods.cpp \
 *       lib/EnergyPeriods/src/EnergyPeriods.cpp -o .pio/test_energy_periods && .pio/test_energy_periods
 *
 * Needs a libc with POSIX TZ rules (Linux, macOS, Cygwin/MSYS2 g++).
 */
#include "EnergyPeriods.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static time_t local(int y, int mon, int d, int h, int mi, int isdst = -1) {
	struct tm tm = {};
	tm.tm_year = y - 1900; tm.tm_mon = mon - 1; tm.tm_mday = d;
	tm.tm_hour = h; tm.tm_min = mi; tm.tm_isdst = isdst;
	return mktime(&tm);
}

static const char *fmt(time_t t) {
	static char buf[4][40];
	static int i = 0;
	i = (i + 1) % 4;
	struct tm tm;
	localtime_r(&t, &tm);
	strftime(buf[i], sizeof(buf[i]), "%Y-%m-%d %H:%M %z", &tm);
	return buf[i];
}

struct Collected {
	EnergyRecord r[6000];
	int n;
};

// Simulate a meter consuming 1 Wh per minute, sampled every 60 s, draining the queue each step
static void run(EnergyPeriods &tracker, time_t from, time_t to, uint32_t &meterWh, Collected &out) {
	for (time_t t = from; t < to; t += 60) {
		if (tracker.needsReading(t)) {
			tracker.update(t, meterWh);
		}
		EnergyRecord rec;
		while (tracker.peek(rec)) {
			out.r[out.n++] = rec;
			tracker.pop();
		}
		meterWh += 1;
	}
}

static const EnergyRecord *find(const Collected &c, uint8_t period, time_t start) {
	for (int i = 0; i < c.n; i++) {
		if (c.r[i].period == period && c.r[i].start == (uint32_t) start) return &c.r[i];
	}
	return nullptr;
}

int main() {
	setenv("TZ", "EET-2EEST,M3.5.0,M10.5.0/3", 1);  // Europe/Chisinau
	tzset();

	// --- periodStart basics ---
	time_t t = local(2026, 10, 18, 13, 27);   // Sunday
	CHECK(EnergyPeriods::periodStart(PERIOD_HOUR, t) == local(2026, 10, 18, 13, 0), "hour start");
	CHECK(EnergyPeriods::periodStart(PERIOD_DAY, t) == local(2026, 10, 18, 0, 0), "day start");
	CHECK(EnergyPeriods::periodStart(PERIOD_WEEK, t) == local(2026, 10, 12, 0, 0), "week start (Sunday -> previous Monday) %s", fmt(EnergyPeriods::periodStart(PERIOD_WEEK, t)));
	CHECK(EnergyPeriods::periodStart(PERIOD_WEEK, local(2026, 10, 19, 0, 0)) == local(2026, 10, 19, 0, 0), "week start on Monday");
	CHECK(EnergyPeriods::periodStart(PERIOD_MONTH, t) == local(2026, 10, 1, 0, 0), "month start");

	// --- long simulation: cold boot Sun 2026-10-18 13:27, through DST end (Oct 25), to 2027-04-02 ---
	static Collected c;
	EnergyPeriods tracker;
	uint32_t meter = 100000;
	run(tracker, local(2026, 10, 18, 13, 27), local(2027, 4, 2, 0, 0), meter, c);

	const EnergyRecord *r;

	r = find(c, PERIOD_HOUR, local(2026, 10, 18, 13, 0));
	CHECK(r && !r->complete && r->energyWh == 33, "first hour partial, 33 Wh");
	r = find(c, PERIOD_HOUR, local(2026, 10, 18, 14, 0));
	CHECK(r && r->complete && r->energyWh == 60 && r->end == (uint32_t) local(2026, 10, 18, 15, 0), "full hour");

	// DST end: 2026-10-25 04:00 EEST -> 03:00 EET, the 03:00 hour happens twice
	time_t h3a = local(2026, 10, 25, 3, 0, 1), h3b = local(2026, 10, 25, 3, 0, 0);
	CHECK(h3b - h3a == 3600, "repeated hour distinct");
	r = find(c, PERIOD_HOUR, h3a);
	CHECK(r && r->complete && r->energyWh == 60 && r->end == (uint32_t) h3b, "first 03:00 hour (EEST)");
	r = find(c, PERIOD_HOUR, h3b);
	CHECK(r && r->complete && r->energyWh == 60, "second 03:00 hour (EET)");

	r = find(c, PERIOD_DAY, local(2026, 10, 18, 0, 0));
	CHECK(r && !r->complete, "boot day partial");
	r = find(c, PERIOD_DAY, local(2026, 10, 24, 0, 0));
	CHECK(r && r->complete && r->energyWh == 24 * 60, "normal day 1440 Wh, got %u", r ? r->energyWh : 0);
	r = find(c, PERIOD_DAY, local(2026, 10, 25, 0, 0));
	CHECK(r && r->complete && r->energyWh == 25 * 60, "DST-end day has 25 h, got %u", r ? r->energyWh : 0);
	r = find(c, PERIOD_DAY, local(2027, 3, 28, 0, 0));
	CHECK(r && r->complete && r->energyWh == 23 * 60, "DST-start day has 23 h, got %u", r ? r->energyWh : 0);

	r = find(c, PERIOD_WEEK, local(2026, 10, 12, 0, 0));
	CHECK(r && !r->complete && r->end == (uint32_t) local(2026, 10, 19, 0, 0), "boot week partial, ends Monday");
	r = find(c, PERIOD_WEEK, local(2026, 10, 19, 0, 0));
	CHECK(r && r->complete && r->energyWh == (7 * 24 + 1) * 60, "week with DST end has 169 h, got %u", r ? r->energyWh : 0);
	r = find(c, PERIOD_WEEK, local(2026, 12, 28, 0, 0));
	CHECK(r && r->complete && r->end == (uint32_t) local(2027, 1, 4, 0, 0), "week across new year");

	r = find(c, PERIOD_MONTH, local(2026, 10, 1, 0, 0));
	CHECK(r && !r->complete && r->end == (uint32_t) local(2026, 11, 1, 0, 0), "boot month partial");
	r = find(c, PERIOD_MONTH, local(2026, 11, 1, 0, 0));
	CHECK(r && r->complete && r->energyWh == 30 * 24 * 60, "November 43200 Wh");
	r = find(c, PERIOD_MONTH, local(2027, 2, 1, 0, 0));
	CHECK(r && r->complete && r->energyWh == 28 * 24 * 60 && r->end == (uint32_t) local(2027, 3, 1, 0, 0), "February");

	// counts: hours between first full hour and the end must be contiguous
	int hours = 0, days = 0;
	for (int i = 0; i < c.n; i++) {
		if (c.r[i].period == PERIOD_HOUR) hours++;
		if (c.r[i].period == PERIOD_DAY) days++;
		if (i > 0 && c.r[i].period == PERIOD_HOUR) {
			for (int j = i - 1; j >= 0; j--) {
				if (c.r[j].period == PERIOD_HOUR) { CHECK(c.r[j].end == c.r[i].start, "hour gap at %s", fmt(c.r[i].start)); break; }
			}
		}
	}
	int expectedHours = (int) ((local(2027, 4, 2, 0, 0) - local(2026, 10, 18, 13, 0)) / 3600) - 1;  // the last hour is still in progress at the end
	CHECK(hours == expectedHours, "hour records %d, expected %d", hours, expectedHours);

	// --- reboot: state restored from RTC continues the period as complete ---
	{
		EnergyPeriods a;
		static Collected ca;
		uint32_t m = 5000;
		run(a, local(2026, 11, 3, 9, 0), local(2026, 11, 3, 10, 30), m, ca);
		EnergyPeriods b;
		b.state = a.state;  // reboot at 10:30
		run(b, local(2026, 11, 3, 10, 30), local(2026, 11, 3, 11, 1), m, ca);
		r = find(ca, PERIOD_HOUR, local(2026, 11, 3, 10, 0));
		CHECK(r && r->complete && r->energyWh == 60, "hour across reboot complete, %u Wh", r ? r->energyWh : 0);
	}

	// --- device off for 3 hours: one record spanning the gap, marked incomplete ---
	{
		EnergyPeriods a;
		static Collected ca;
		uint32_t m = 0;
		run(a, local(2026, 11, 3, 9, 0), local(2026, 11, 3, 9, 30), m, ca);
		run(a, local(2026, 11, 3, 12, 15), local(2026, 11, 3, 12, 16), m, ca);
		r = find(ca, PERIOD_HOUR, local(2026, 11, 3, 9, 0));
		CHECK(r && !r->complete && r->end == (uint32_t) local(2026, 11, 3, 12, 0), "gap record spans to 12:00, incomplete");
	}

	// --- meter reset ---
	{
		EnergyPeriods a;
		a.update(local(2026, 11, 3, 9, 0), 5000);
		a.update(local(2026, 11, 3, 10, 0), 20);
		EnergyRecord rec;
		CHECK(a.peek(rec) && !rec.complete && rec.energyWh == 20, "meter reset counted from 0, incomplete");
	}

	// --- queue overflow drops oldest ---
	{
		EnergyPeriods a;
		a.update(local(2026, 11, 3, 0, 0), 0);
		for (int h = 1; h <= 40; h++) a.update(local(2026, 11, 3, 0, 0) + h * 3600, h);
		EnergyRecord rec;
		CHECK(a.queued() == ENERGY_QUEUE_SIZE && a.dropped() > 0, "queue full, dropped %u", a.dropped());
		CHECK(a.peek(rec) && rec.start > (uint32_t) local(2026, 11, 3, 0, 0), "oldest dropped");
	}

	printf("%d records, %s\n", c.n, failures ? "FAILED" : "all checks passed");
	return failures ? 1 : 0;
}
