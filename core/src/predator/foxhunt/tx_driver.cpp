#include <predator/foxhunt/tx_driver.h>

// Single process-wide registry anchor. Compiled into sdrpp_core so every
// driver module (soapy_source, plutosdr_source, ...) and the UI resolve the
// same instance across shared-object boundaries.
namespace predator::foxhunt {
    TxDriverRegistry& TxDriverRegistry::instance() {
        static TxDriverRegistry reg;
        return reg;
    }
}
