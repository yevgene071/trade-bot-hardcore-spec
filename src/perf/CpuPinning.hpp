#pragma once

namespace trade_bot {

/**
 * Pin current thread to a specific CPU core.
 * @param cpu_id The ID of the CPU core (0-based).
 * @return true if successful, false otherwise.
 */
bool pin_thread_to_cpu(int cpu_id);

} // namespace trade_bot
