/**
 * @file Types.cpp
 * @brief Global definitions and storage for common HFT engine variables.
 */

#include "hft/common/Types.hpp"

namespace hft::common
{
    using namespace std;

    AlignedAtomicFlag g_running{true};
    AlignedAtomicFlag g_producer_done{false};
    AlignedAtomicFlag g_consumer_done{false};

    double g_cycles_per_ns = 1.0;

} // namespace hft::common
