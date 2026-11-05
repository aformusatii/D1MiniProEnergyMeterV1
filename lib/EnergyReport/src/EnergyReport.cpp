#include "EnergyReport.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define DAY_S  86400

// the texts stay in flash on the ESP8266 (string literals would take RAM there)
#ifdef ARDUINO
#include <pgmspace.h>
#define TXT(s) PSTR(s)
#define VSNPRINTF vsnprintf_P
#else
#define TXT(s) (s)
#define VSNPRINTF vsnprintf
#endif

static const char *const WEEKDAYS[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char *const MONTHS[] = { "January", "February", "March", "April", "May", "June", "July",
		"August", "September", "October", "November", "December" };

// appends to buf at len, never past size; len stays at the NUL
static void appendf(char *buf, size_t size, size_t &len, const char *fmt, ...) {
	if (len + 1 >= size) {
		return;
	}
	va_list args;
	va_start(args, fmt);
	int n = VSNPRINTF(buf + len, size - len, fmt, args);
	va_end(args);
	if (n > 0) {
		len = (len + n < size) ? len + n : size - 1;
	}
}

static struct tm localTm(time_t t) {
	struct tm tm;
	localtime_r(&t, &tm);
	return tm;
}

// "Wed 4 Nov"
static void dayName(time_t t, char *out, size_t size) {
	struct tm tm = localTm(t);
	snprintf(out, size, "%s %d %.3s", WEEKDAYS[tm.tm_wday], tm.tm_mday, MONTHS[tm.tm_mon]);
}

// "October 2026", or "October" without the year
static void monthName(time_t t, char *out, size_t size, bool year) {
	struct tm tm = localTm(t);
	if (year) {
		snprintf(out, size, "%s %d", MONTHS[tm.tm_mon], tm.tm_year + 1900);
	} else {
		snprintf(out, size, "%s", MONTHS[tm.tm_mon]);
	}
}

// whole local days between two local midnights (23 and 25 hour days count as one)
static int daysBetween(uint32_t from, uint32_t to) {
	return (int) lround((double) (to - from) / DAY_S);
}

static bool singleDay(const EnergyRecord &r) {
	return (uint32_t) EnergyPeriods::periodStart(PERIOD_DAY, (time_t) r.end - 1) == r.start;
}

static double kwh(uint32_t wh) {
	return wh / 1000.0;
}

EnergyReport::EnergyReport(double pricePerKwh, const char *_currency, uint8_t _hour, uint8_t _minute) {
	priceCents = (uint32_t) lround(pricePerKwh * 100.0);
	currency = _currency;
	hour = _hour;
	minute = _minute;
	memset(&state, 0, sizeof(state));
}

void EnergyReport::insert(EnergyRecord *list, uint8_t &count, uint8_t max, const EnergyRecord &record) {
	memmove(&list[1], &list[0], sizeof(EnergyRecord) * (max - 1));
	list[0] = record;
	if (count < max) {
		count++;
	}
}

bool EnergyReport::add(const EnergyRecord &record) {
	if (record.period == PERIOD_DAY) {
		insert(state.days, state.dayCount, REPORT_DAY_HISTORY, record);
		return true;
	}
	if (record.period == PERIOD_MONTH) {
		insert(state.months, state.monthCount, REPORT_MONTH_HISTORY, record);
		return true;
	}
	return false;
}

time_t EnergyReport::reportTime(time_t dayStart) const {
	struct tm tm = localTm(dayStart);
	tm.tm_hour = hour;
	tm.tm_min = minute;
	tm.tm_sec = 0;
	tm.tm_isdst = -1;
	return mktime(&tm);
}

bool EnergyReport::due(time_t now) const {
	if (state.dayCount == 0 || state.days[0].end == state.reportedDayEnd) {
		return false;
	}
	// a late report (device or internet down at report time) goes out as soon as possible
	return now >= reportTime((time_t) state.days[0].end);
}

void EnergyReport::markReported() {
	if (state.dayCount > 0) {
		state.reportedDayEnd = state.days[0].end;
	}
	if (state.monthCount > 0) {
		state.reportedMonthEnd = state.months[0].end;
	}
}

// the newest finished day, compared with the average of the complete days before it
void EnergyReport::dayBlock(char *buf, size_t size, size_t &len, time_t now) const {
	const EnergyRecord &d = state.days[0];
	char from[16];
	dayName((time_t) d.start, from, sizeof(from));

	bool single = singleDay(d);
	if (single) {
		bool isYesterday = d.end == (uint32_t) EnergyPeriods::periodStart(PERIOD_DAY, now);
		appendf(buf, size, len, TXT("\xE2\x9A\xA1 %s%s: %.1f kWh = %.2f %s\n"), isYesterday ? "Yesterday, " : "", from,
				kwh(d.energyWh), cost(d.energyWh), currency);
		if (!d.complete) {
			appendf(buf, size, len, TXT("(incomplete: the meter did not see the whole day)\n"));
		}
	} else {
		char to[16];
		dayName((time_t) d.end - 1, to, sizeof(to));
		appendf(buf, size, len, TXT("\xE2\x9A\xA1 %s - %s (%d days, the meter was off in between): %.1f kWh = %.2f %s\n"),
				from, to, daysBetween(d.start, d.end), kwh(d.energyWh), cost(d.energyWh), currency);
		return;
	}

	uint32_t sum = 0;
	int n = 0;
	for (uint8_t i = 1; i < state.dayCount; i++) {
		if (state.days[i].complete && singleDay(state.days[i])) {
			sum += state.days[i].energyWh;
			n++;
		}
	}
	if (n == 0) {
		return;
	}
	double avg = (double) sum / n;
	appendf(buf, size, len, TXT("Previous %d day%s: %.1f kWh/day on average"), n, n == 1 ? "" : "s", avg / 1000.0);
	if (d.complete && avg > 0) {
		appendf(buf, size, len, TXT(" (%+.0f%%)"), (d.energyWh - avg) * 100.0 / avg);
	}
	appendf(buf, size, len, TXT("\n"));
}

// usedWh consumed from month.start until at (unix time), plus a forecast for the whole month
void EnergyReport::monthSoFarBlock(char *buf, size_t size, size_t &len, uint32_t usedWh, uint32_t at,
		const PeriodState &month, const char *label) const {
	char name[16];
	monthName((time_t) month.start, name, sizeof(name), false);
	appendf(buf, size, len, TXT("%s %s: %.1f kWh = %.2f %s"), name, label, kwh(usedWh), cost(usedWh), currency);
	if (!month.fromStart) {
		appendf(buf, size, len, TXT(" (incomplete: measuring since the device started)\n"));
		return;
	}
	appendf(buf, size, len, TXT("\n"));

	// a forecast from less than a day says nothing
	uint32_t elapsed = at - month.start;
	if (elapsed < DAY_S) {
		return;
	}
	uint32_t end = (uint32_t) EnergyPeriods::periodStart(PERIOD_MONTH, (time_t) month.start + 32 * DAY_S);
	double forecastWh = (double) usedWh * (end - month.start) / elapsed;
	appendf(buf, size, len, TXT("Forecast for %s: ~%.0f kWh = ~%.0f %s\n"), name, forecastWh / 1000.0,
			forecastWh / 1000.0 * priceCents / 100.0, currency);
}

// the newest finished month, compared with the month before and the same month a year earlier
void EnergyReport::monthBlock(char *buf, size_t size, size_t &len) const {
	const EnergyRecord &m = state.months[0];
	char name[24];
	monthName((time_t) m.start, name, sizeof(name), true);
	appendf(buf, size, len, TXT("\xF0\x9F\x93\x85 %s: %.1f kWh = %.2f %s\n"), name, kwh(m.energyWh), cost(m.energyWh), currency);
	if (!m.complete) {
		appendf(buf, size, len, TXT("(incomplete: the meter did not see the whole month)\n"));
	} else {
		appendf(buf, size, len, TXT("%.1f kWh/day on average\n"), kwh(m.energyWh) / daysBetween(m.start, m.end));
	}

	struct tm tm = localTm((time_t) m.start);
	tm.tm_year -= 1;
	tm.tm_isdst = -1;
	uint32_t yearAgo = (uint32_t) mktime(&tm);

	char shortName[16];
	monthName((time_t) m.start, shortName, sizeof(shortName), false);
	for (uint8_t i = 1; i < state.monthCount; i++) {
		const EnergyRecord &o = state.months[i];
		if (o.end != m.start && o.start != yearAgo) {
			continue;
		}
		char other[24];
		monthName((time_t) o.start, other, sizeof(other), true);
		appendf(buf, size, len, TXT("%s: %.1f kWh"), other, kwh(o.energyWh));
		if (m.complete && o.complete && o.energyWh > 0) {
			appendf(buf, size, len, TXT(" (%s %+.0f%%)"), shortName, ((double) m.energyWh - o.energyWh) * 100.0 / o.energyWh);
		}
		appendf(buf, size, len, TXT("\n"));
	}
}

size_t EnergyReport::daily(char *buf, size_t size, time_t now, const PeriodState &month) const {
	size_t len = 0;
	buf[0] = 0;
	if (state.dayCount == 0) {
		return 0;
	}

	const EnergyRecord &d = state.days[0];
	dayBlock(buf, size, len, now);

	// on the 1st the month in progress has just begun, the finished month below says more
	if (month.start != 0 && month.start < d.end && d.meterWh >= month.startWh) {
		monthSoFarBlock(buf, size, len, d.meterWh - month.startWh, d.end, month, "so far");
	}

	if (state.monthCount > 0 && state.months[0].end != state.reportedMonthEnd) {
		appendf(buf, size, len, TXT("\n"));
		monthBlock(buf, size, len);
	}

	return len;
}

size_t EnergyReport::today(char *buf, size_t size, time_t now, uint32_t meterWh, float powerW, const PeriodState &day) const {
	size_t len = 0;
	buf[0] = 0;

	if (day.start == 0 || day.start != (uint32_t) EnergyPeriods::periodStart(PERIOD_DAY, now) || meterWh < day.startWh) {
		appendf(buf, size, len, TXT("No data for today yet, the meter is starting.\n"));
	} else {
		uint32_t used = meterWh - day.startWh;
		appendf(buf, size, len, TXT("\xE2\x9A\xA1 Today so far: %.1f kWh = %.2f %s\n"), kwh(used), cost(used), currency);
		if (!day.fromStart) {
			appendf(buf, size, len, TXT("(incomplete: measuring since the device started)\n"));
		}
	}

	if (!isnan(powerW)) {
		appendf(buf, size, len, TXT("Using now: %.0f W\n"), powerW);
	}
	if (state.dayCount > 0 && singleDay(state.days[0])
			&& state.days[0].end == (uint32_t) EnergyPeriods::periodStart(PERIOD_DAY, now)) {
		appendf(buf, size, len, TXT("Yesterday in total: %.1f kWh\n"), kwh(state.days[0].energyWh));
	}
	return len;
}

size_t EnergyReport::yesterday(char *buf, size_t size, time_t now) const {
	size_t len = 0;
	buf[0] = 0;
	if (state.dayCount == 0) {
		appendf(buf, size, len, TXT("No finished day yet, the first one is ready after midnight.\n"));
		return len;
	}
	dayBlock(buf, size, len, now);
	return len;
}

size_t EnergyReport::month(char *buf, size_t size, time_t now, uint32_t meterWh, const PeriodState &month) const {
	size_t len = 0;
	buf[0] = 0;

	if (month.start == 0 || month.start != (uint32_t) EnergyPeriods::periodStart(PERIOD_MONTH, now) || meterWh < month.startWh) {
		appendf(buf, size, len, TXT("No data for this month yet, the meter is starting.\n"));
	} else {
		appendf(buf, size, len, TXT("\xF0\x9F\x93\x85 "));
		monthSoFarBlock(buf, size, len, meterWh - month.startWh, (uint32_t) now, month, "so far");
	}

	if (state.monthCount > 0 && state.months[0].end == month.start) {
		const EnergyRecord &m = state.months[0];
		char name[24];
		monthName((time_t) m.start, name, sizeof(name), true);
		appendf(buf, size, len, TXT("%s: %.1f kWh = %.2f %s%s\n"), name, kwh(m.energyWh), cost(m.energyWh), currency,
				m.complete ? "" : " (incomplete)");
	}
	return len;
}
