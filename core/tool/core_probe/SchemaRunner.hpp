#pragma once

namespace trade_bot::probe {

class SchemaRunner {
public:
    /// Print JSON schema of all trace stages and exit.
    static int run();
};

} // namespace trade_bot::probe
