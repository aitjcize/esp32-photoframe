// Fake storage for host tests: persistence is a test-controllable global.
#include "storage.h"

bool test_storage_persistent = true;

bool storage_has_persistent_storage(void)
{
    return test_storage_persistent;
}
