#include "EnergyPeriods.h"
#include <string.h>

EnergyPeriods::EnergyPeriods() {
	memset(&state, 0, sizeof(state));
	queueHead = 0;
	queueCount = 0;
	droppedCount = 0;
}

time_t EnergyPeriods::periodStart(uint8_t period, time_t t) {
	struct tm tm;
	localtime_r(&t, &tm);

	tm.tm_sec = 0;
	tm.tm_min = 0;

	if (period == PERIOD_HOUR) {
		// keep tm_isdst so the repeated hour at the end of DST resolves to the right instant
		return mktime(&tm);
	}

	tm.tm_hour = 0;
	tm.tm_isdst = -1;

	if (period == PERIOD_WEEK) {
		// ISO week: Monday 00:00 (tm_wday: 0 = Sunday)
		tm.tm_mday -= (tm.tm_wday + 6) % 7;
	} else if (period == PERIOD_MONTH) {
		tm.tm_mday = 1;
	}

	return mktime(&tm);
}

const char *EnergyPeriods::periodName(uint8_t period) {
	switch (period) {
		case PERIOD_HOUR:  return "hour";
		case PERIOD_DAY:   return "day";
		case PERIOD_WEEK:  return "week";
		case PERIOD_MONTH: return "month";
		default:           return "unknown";
	}
}

bool EnergyPeriods::needsReading(time_t now) const {
	if (!state.valid) {
		return true;
	}

	for (uint8_t p = 0; p < PERIOD_COUNT; p++) {
		if ((uint32_t) periodStart(p, now) != state.periods[p].start) {
			return true;
		}
	}

	return false;
}

bool EnergyPeriods::update(time_t now, uint32_t meterWh) {
	if (!state.valid) {
		// First reading after a cold boot: the periods in progress are only partially covered
		for (uint8_t p = 0; p < PERIOD_COUNT; p++) {
			PeriodState &ps = state.periods[p];
			ps.start = (uint32_t) periodStart(p, now);
			ps.startWh = meterWh;
			ps.fromStart = (ps.start == (uint32_t) now) ? 1 : 0;
		}
		state.valid = 1;
		return true;
	}

	bool changed = false;

	for (uint8_t p = 0; p < PERIOD_COUNT; p++) {
		PeriodState &ps = state.periods[p];
		uint32_t newStart = (uint32_t) periodStart(p, now);

		if (newStart == ps.start) {
			continue;
		}

		if (newStart > ps.start) {
			bool meterReset = meterWh < ps.startWh;

			EnergyRecord record;
			record.period = p;
			record.start = ps.start;
			record.end = newStart;
			record.meterWh = meterWh;
			record.energyWh = meterReset ? meterWh : meterWh - ps.startWh;
			// exactly one period: the last second before end still belongs to the period at start
			record.complete = ps.fromStart && !meterReset
					&& (uint32_t) periodStart(p, (time_t) newStart - 1) == ps.start;
			push(record);
		}
		// else: clock went backwards, drop the period in progress without a record

		ps.start = newStart;
		ps.startWh = meterWh;
		ps.fromStart = 1;
		changed = true;
	}

	return changed;
}

void EnergyPeriods::push(const EnergyRecord &record) {
	if (queueCount == ENERGY_QUEUE_SIZE) {
		// full: drop the oldest record (meterWh in later records still allows reconstruction)
		queueHead = (queueHead + 1) % ENERGY_QUEUE_SIZE;
		queueCount--;
		droppedCount++;
	}

	queue[(queueHead + queueCount) % ENERGY_QUEUE_SIZE] = record;
	queueCount++;
}

bool EnergyPeriods::peek(EnergyRecord &record) const {
	if (queueCount == 0) {
		return false;
	}

	record = queue[queueHead];
	return true;
}

void EnergyPeriods::pop() {
	if (queueCount == 0) {
		return;
	}

	queueHead = (queueHead + 1) % ENERGY_QUEUE_SIZE;
	queueCount--;
}
