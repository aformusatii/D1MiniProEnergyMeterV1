#ifndef _EnergyPeriods_H_
#define _EnergyPeriods_H_

#include <stdint.h>
#include <time.h>

/*
 * Turns the cumulative PZEM energy counter into consumption per local calendar period
 * (hour, day, ISO week starting Monday, month).
 *
 * At every period boundary a record {start, end, energyWh, meterWh} goes into a small
 * RAM queue that the MQTT side drains. Local time comes from the TZ environment
 * (configTime), so DST is handled by localtime/mktime.
 *
 * Plain C++ without Arduino dependencies, so it can be unit-tested on the host.
 */

enum EnergyPeriod : uint8_t {
	PERIOD_HOUR = 0,
	PERIOD_DAY,
	PERIOD_WEEK,
	PERIOD_MONTH,
	PERIOD_COUNT
};

#define ENERGY_QUEUE_SIZE  36   // 1.5 days of hourly records while the broker is unreachable

struct EnergyRecord {
	uint32_t start;      // period start, unix time
	uint32_t end;        // period end (= start of the next period), unix time
	uint32_t energyWh;   // consumed in [start, end)
	uint32_t meterWh;    // PZEM counter at end
	uint8_t period;      // EnergyPeriod
	bool complete;       // false: tracking began mid-period, spans several periods, or meter was reset
};

struct PeriodState {
	uint32_t start;      // start of the period in progress
	uint32_t startWh;    // PZEM counter at start
	uint8_t fromStart;   // 1 if tracking began exactly at start
};

// POD so it can be stored as-is in RTC memory (survives reset / OTA reboot)
struct EnergyTrackerState {
	PeriodState periods[PERIOD_COUNT];
	uint8_t valid;
};

class EnergyPeriods {

	private:
		EnergyRecord queue[ENERGY_QUEUE_SIZE];
		uint8_t queueHead;
		uint8_t queueCount;
		uint16_t droppedCount;

		void push(const EnergyRecord &record);

	public:
		EnergyPeriods(void);

		EnergyTrackerState state;

		static time_t periodStart(uint8_t period, time_t t);
		static const char *periodName(uint8_t period);

		// true when update() wants a fresh meter reading (not initialized yet or a boundary was crossed)
		bool needsReading(time_t now) const;

		// Returns true when the state changed (caller should persist it)
		bool update(time_t now, uint32_t meterWh);

		bool peek(EnergyRecord &record) const;
		void pop(void);
		uint8_t queued(void) const { return queueCount; }
		uint16_t dropped(void) const { return droppedCount; }
};

#endif /* _EnergyPeriods_H_ */
