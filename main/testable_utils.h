#ifndef TESTABLE_UTILS_H
#define TESTABLE_UTILS_H

// Calculate next clock-aligned wake-up time considering sleep schedule
// Returns seconds until next wake-up
// Takes into account:
// - Clock alignment (aligns to rotation interval boundaries)
// - Sleep schedule (skips wake-ups that fall within sleep schedule)
// - Overnight schedules (handles schedules that cross midnight)
int calculate_next_aligned_wakeup(int rotate_interval);

#endif
