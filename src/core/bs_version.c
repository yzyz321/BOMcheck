#include "bs_version.h"
#include "bs_version_gen.h"

const char *bs_version(void) { return BS_VERSION_STR; }

const char *bs_build_stamp(void) { return __DATE__ " " __TIME__; }
