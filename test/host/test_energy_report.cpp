/*
 * Host test for lib/EnergyReport (daily message timing, month end, texts) fed by lib/EnergyPeriods.
 *
 *   g++ -std=gnu++17 -Wall -I lib/EnergyPeriods/src -I lib/EnergyReport/src test/host/test_energy_report.cpp \
 *       lib/EnergyPeriods/src/EnergyPeriods.cpp lib/EnergyReport/src/EnergyReport.cpp \
 *       -o .pio/test_energy_report && .pio/test_energy_report
 *
 * Needs a libc with POSIX TZ rules (Linux, macOS, Cygwin/MSYS2 g++).
 */
#include "EnergyReport.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define CONTAINS(text, part) CHECK(strstr(text, part) != nullptr, "missing \"%s\" in:\n%s", part, text)
#define LACKS(text, part) CHECK(strstr(text, part) == nullptr, "unexpected \"%s\" in:\n%s", part, text)

static time_t local(int y, int mon, int d, int h, int mi) {
	struct tm tm = {};
	tm.tm_year = y - 1900; tm.tm_mon = mon - 1; tm.tm_mday = d;
	tm.tm_hour = h; tm.tm_min = mi; tm.tm_isdst = -1;
	return mktime(&tm);
}

static EnergyReport *current = nullptr;
static void onRecord(const EnergyRecord &r) { current->add(r); }

// meter consuming whPerMin each minute, sampled every 60 s like the firmware (queue drained, as MQTT does)
static void run(EnergyPeriods &tracker, time_t from, time_t to, uint32_t &meterWh, uint32_t whPerMin) {
	for (time_t t = from; t < to; t += 60) {
		if (tracker.needsReading(t)) {
			tracker.update(t, meterWh);
		}
		EnergyRecord rec;
		while (tracker.peek(rec)) tracker.pop();
		meterWh += whPerMin;
	}
}

static EnergyRecord day(int y, int mon, int d, uint32_t wh, bool complete = true) {
	EnergyRecord r = {};
	r.period = PERIOD_DAY;
	r.start = (uint32_t) local(y, mon, d, 0, 0);
	r.end = (uint32_t) local(y, mon, d + 1, 0, 0);
	r.energyWh = wh;
	r.complete = complete;
	return r;
}

static EnergyRecord month(int y, int mon, uint32_t wh, bool complete = true) {
	EnergyRecord r = {};
	r.period = PERIOD_MONTH;
	r.start = (uint32_t) local(y, mon, 1, 0, 0);
	r.end = (uint32_t) local(y, mon + 1, 1, 0, 0);
	r.energyWh = wh;
	r.complete = complete;
	return r;
}

int main() {
	setenv("TZ", "EET-2EEST,M3.5.0,M10.5.0/3", 1);  // Europe/Chisinau
	tzset();

	char buf[600];
	PeriodState none = {};

	// --- report time, DST-safe ---
	{
		EnergyReport r(3.59, "lei", 7, 0);
		CHECK(r.reportTime(local(2026, 11, 5, 0, 0)) == local(2026, 11, 5, 7, 0), "07:00 local");
		CHECK(r.reportTime(local(2026, 10, 25, 0, 0)) == local(2026, 10, 25, 7, 0), "07:00 on DST end day");
		CHECK(r.reportTime(local(2027, 3, 28, 0, 0)) == local(2027, 3, 28, 7, 0), "07:00 on DST start day");
		CHECK(r.daily(buf, sizeof(buf), local(2026, 11, 5, 7, 0), none) == 0 && buf[0] == 0, "nothing without a day");
		CHECK(!r.due(local(2026, 11, 5, 7, 0)), "not due without a day");
	}

	// --- full simulation: tracker -> report, cold boot Mon 2026-10-26 10:00, 0.5 kWh per hour ---
	{
		EnergyReport r(3.59, "lei", 7, 0);
		current = &r;
		EnergyPeriods tracker;
		tracker.setListener(onRecord);
		uint32_t meter = 1000000;

		run(tracker, local(2026, 10, 26, 10, 0), local(2026, 10, 27, 6, 59), meter, 0);
		CHECK(r.state.dayCount == 1 && !r.state.days[0].complete, "boot day recorded as incomplete");
		CHECK(!r.due(local(2026, 10, 27, 6, 59)), "not due before 07:00");
		CHECK(r.due(local(2026, 10, 27, 7, 0)), "due at 07:00");
		r.markReported();
		CHECK(!r.due(local(2026, 10, 27, 7, 1)), "sent once");

		// 27 Oct .. 2 Nov at 12 kWh/day (500 Wh each full hour), Tue 3 Nov at 18 kWh
		for (int d = 27; d <= 34; d++) {
			uint32_t whPerHour = (d == 34) ? 750 : 500;
			for (time_t t = local(2026, 10, d, 0, 0); t < local(2026, 10, d + 1, 0, 0); t += 60) {
				if (tracker.needsReading(t)) tracker.update(t, meter);
				EnergyRecord rec;
				while (tracker.peek(rec)) tracker.pop();
				if ((t / 60) % 60 == 59) meter += whPerHour;
			}
		}
		run(tracker, local(2026, 11, 4, 0, 0), local(2026, 11, 4, 0, 1), meter, 0);
		// days 27 Oct .. 3 Nov are finished now
		CHECK(r.state.dayCount == 8, "day history full, %u", r.state.dayCount);
		CHECK(r.state.monthCount == 1 && !r.state.months[0].complete, "October partial");
		CHECK(r.state.days[0].start == (uint32_t) local(2026, 11, 3, 0, 0), "newest is 3 Nov");
		CHECK(r.state.days[0].energyWh == 18000, "3 Nov 18 kWh, got %u", r.state.days[0].energyWh);

		// the report was not sent for days in between: only the newest day is due, October too
		CHECK(r.due(local(2026, 11, 4, 7, 0)), "due on 4 Nov");
		r.daily(buf, sizeof(buf), local(2026, 11, 4, 7, 0), tracker.state.periods[PERIOD_MONTH]);
		CONTAINS(buf, "Yesterday, Tue 3 Nov: 18.0 kWh = 64.62 lei");
		CONTAINS(buf, "Previous 7 days: 12.0 kWh/day on average (+50%)");
		CONTAINS(buf, "November so far: 42.0 kWh = 150.78 lei");
		CONTAINS(buf, "Forecast for November: ~420 kWh = ~1508 lei");
		CONTAINS(buf, "October 2026: ");
		CONTAINS(buf, "(incomplete: the meter did not see the whole month)");
		r.markReported();
		CHECK(!r.due(local(2026, 11, 4, 12, 0)), "not due after sending");

		// next day: October is not repeated
		run(tracker, local(2026, 11, 4, 0, 1), local(2026, 11, 5, 0, 1), meter, 0);
		CHECK(r.due(local(2026, 11, 5, 8, 30)), "late report still due");
		r.daily(buf, sizeof(buf), local(2026, 11, 5, 8, 30), tracker.state.periods[PERIOD_MONTH]);
		LACKS(buf, "October");
		CONTAINS(buf, "Forecast for November: ~315 kWh");  // 0 kWh on 4 Nov lowers it

		// /today and /month
		r.today(buf, sizeof(buf), local(2026, 11, 5, 12, 0), tracker.state.periods[PERIOD_DAY].startWh + 2500, 431.4f,
				tracker.state.periods[PERIOD_DAY]);
		CONTAINS(buf, "Today so far: 2.5 kWh = 8.98 lei");
		CONTAINS(buf, "Using now: 431 W");
		CONTAINS(buf, "Yesterday in total: 0.0 kWh");
		LACKS(buf, "incomplete");
		r.today(buf, sizeof(buf), local(2026, 11, 5, 12, 0), 0, NAN, none);
		CONTAINS(buf, "No data for today yet");
		LACKS(buf, "Using now");

		r.month(buf, sizeof(buf), local(2026, 11, 5, 12, 0), tracker.state.periods[PERIOD_MONTH].startWh + 50000,
				tracker.state.periods[PERIOD_MONTH]);
		CONTAINS(buf, "November so far: 50.0 kWh = 179.50 lei");
		CONTAINS(buf, "Forecast for November: ~333 kWh");  // 50 kWh in 4.5 days of 30
		CONTAINS(buf, "October 2026: ");
		CONTAINS(buf, " (incomplete)");

		r.yesterday(buf, sizeof(buf), local(2026, 11, 5, 12, 0));
		CONTAINS(buf, "Yesterday, Wed 4 Nov: 0.0 kWh = 0.00 lei");
		r.yesterday(buf, sizeof(buf), local(2026, 11, 7, 12, 0));
		CONTAINS(buf, "\xE2\x9A\xA1 Wed 4 Nov: ");
		LACKS(buf, "Yesterday");
	}

	// --- month end: 1 Dec report carries November, compared with October and November a year ago ---
	{
		EnergyReport r(3.59, "lei", 7, 0);
		r.add(month(2025, 11, 300000));
		r.add(month(2026, 10, 330000));
		for (int d = 23; d <= 30; d++) r.add(day(2026, 11, d, 11000));
		r.add(month(2026, 11, 352100));

		PeriodState dec = {};
		dec.start = (uint32_t) local(2026, 12, 1, 0, 0);
		dec.fromStart = 1;
		CHECK(r.due(local(2026, 12, 1, 7, 0)), "due on the 1st");
		size_t n = r.daily(buf, sizeof(buf), local(2026, 12, 1, 7, 0), dec);
		CONTAINS(buf, "Yesterday, Mon 30 Nov: 11.0 kWh = 39.49 lei");
		CONTAINS(buf, "Previous 7 days: 11.0 kWh/day on average (+0%)");
		LACKS(buf, "December");  // nothing measured in December yet
		CONTAINS(buf, "\xF0\x9F\x93\x85 November 2026: 352.1 kWh = 1264.04 lei");
		CONTAINS(buf, "11.7 kWh/day on average");
		CONTAINS(buf, "October 2026: 330.0 kWh (November +7%)");
		CONTAINS(buf, "November 2025: 300.0 kWh (November +17%)");
		CHECK(n == strlen(buf) && n < 400, "length %zu", n);
		printf("--- 1 Dec message (%zu bytes) ---\n%s---\n", n, buf);

		r.markReported();
		CHECK(r.state.reportedMonthEnd == (uint32_t) local(2026, 12, 1, 0, 0), "month marked");

		// truncation keeps the text terminated
		char small[40];
		r.state.reportedMonthEnd = 0;
		size_t m = r.daily(small, sizeof(small), local(2026, 12, 1, 7, 0), dec);
		CHECK(m == strlen(small) && m < sizeof(small), "cut to buffer, %zu", m);
	}

	// --- device off for several days: one record spanning them ---
	{
		EnergyReport r(3.59, "lei", 7, 0);
		EnergyRecord span = day(2026, 11, 2, 30000, false);
		span.end = (uint32_t) local(2026, 11, 5, 0, 0);
		r.add(span);
		r.daily(buf, sizeof(buf), local(2026, 11, 5, 9, 0), none);
		CONTAINS(buf, "Mon 2 Nov - Wed 4 Nov (3 days, the meter was off in between): 30.0 kWh");
	}

	// --- power cut: day/month resumed from flash keep the totals exact ---
	{
		EnergyReport r(3.59, "lei", 7, 0);
		current = &r;
		EnergyPeriods a;
		a.setListener(onRecord);
		uint32_t meter = 5000;
		run(a, local(2026, 11, 1, 0, 0), local(2026, 11, 3, 14, 0), meter, 10);

		// power cut 14:00 - 16:00: RTC lost, the flash copy has day/week/month as of 00:00
		EnergyPeriods b;
		b.setListener(onRecord);
		b.state.periods[PERIOD_DAY] = a.state.periods[PERIOD_DAY];
		b.state.periods[PERIOD_WEEK] = a.state.periods[PERIOD_WEEK];
		b.state.periods[PERIOD_MONTH] = a.state.periods[PERIOD_MONTH];
		b.state.valid = 1;
		CHECK(b.needsReading(local(2026, 11, 3, 16, 0)), "hour not tracked after resume");
		run(b, local(2026, 11, 3, 16, 0), local(2026, 11, 4, 0, 1), meter, 10);
		CHECK(r.state.days[0].start == (uint32_t) local(2026, 11, 3, 0, 0) && r.state.days[0].complete
				&& r.state.days[0].energyWh == 22 * 60 * 10, "day across power cut complete, %u Wh", r.state.days[0].energyWh);
		CHECK(b.state.periods[PERIOD_MONTH].fromStart == 1 && b.state.periods[PERIOD_HOUR].start != 0, "month continues");
	}

	printf("%s\n", failures ? "FAILED" : "all checks passed");
	return failures ? 1 : 0;
}
