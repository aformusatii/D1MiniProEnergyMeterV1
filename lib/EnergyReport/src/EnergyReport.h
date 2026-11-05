#ifndef _EnergyReport_H_
#define _EnergyReport_H_

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "EnergyPeriods.h"

/*
 * Family energy report: keeps the last finished days and months from EnergyPeriods, decides when
 * the daily message is due (once per day at a fixed local time, or as soon as possible after it)
 * and writes the texts: the daily message (yesterday, month so far, forecast; on the 1st also the
 * previous month) and the answers to /today, /yesterday and /month.
 *
 * Plain C++ without Arduino dependencies, so it can be unit-tested on the host.
 */

#define REPORT_DAY_HISTORY     8   // yesterday + the 7 days before it for the average
#define REPORT_MONTH_HISTORY  13   // last month + the same month one year earlier

// POD so it can be stored as-is in flash
struct EnergyReportState {
	EnergyRecord days[REPORT_DAY_HISTORY];      // finished days, newest first
	EnergyRecord months[REPORT_MONTH_HISTORY];  // finished months, newest first
	uint8_t dayCount;
	uint8_t monthCount;
	uint32_t reportedDayEnd;    // end of the newest day already sent, 0 = none
	uint32_t reportedMonthEnd;  // end of the newest month already sent, 0 = none
};

class EnergyReport {

	private:
		uint32_t priceCents;  // hundredths of currency per kWh, so costs round exactly
		const char *currency;
		uint8_t hour;
		uint8_t minute;

		static void insert(EnergyRecord *list, uint8_t &count, uint8_t max, const EnergyRecord &record);

		void dayBlock(char *buf, size_t size, size_t &len, time_t now) const;
		void monthSoFarBlock(char *buf, size_t size, size_t &len, uint32_t usedWh, uint32_t at,
				const PeriodState &month, const char *label) const;
		void monthBlock(char *buf, size_t size, size_t &len) const;
		// rounded to cents
		double cost(uint32_t wh) const { return (double) (((uint64_t) wh * priceCents + 500) / 1000) / 100.0; }

	public:
		// pricePerKwh in currency (2 decimals), the daily message goes out at hour:minute local time
		EnergyReport(double pricePerKwh, const char *currency, uint8_t hour, uint8_t minute);

		EnergyReportState state;

		// Day and month records are kept, others ignored. Returns true when the state changed.
		bool add(const EnergyRecord &record);

		// true when the newest finished day was not sent yet and its report time has passed
		bool due(time_t now) const;
		// local time of the report for the day that started at dayStart (DST-safe)
		time_t reportTime(time_t dayStart) const;
		// the daily message was sent: its day (and month, if included) are not due again
		void markReported(void);

		// All texts return their length, 0 when there is nothing to tell yet; they are always
		// NUL-terminated and cut to size. month/day are the tracker's periods in progress.
		size_t daily(char *buf, size_t size, time_t now, const PeriodState &month) const;
		size_t today(char *buf, size_t size, time_t now, uint32_t meterWh, float powerW, const PeriodState &day) const;
		size_t yesterday(char *buf, size_t size, time_t now) const;
		size_t month(char *buf, size_t size, time_t now, uint32_t meterWh, const PeriodState &month) const;
};

#endif /* _EnergyReport_H_ */
