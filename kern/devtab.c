#include "u.h"
#include "lib.h"
#include "dat.h"
#include "fns.h"
#include "error.h"

extern Dev consdevtab;
extern Dev rootdevtab;
extern Dev pipedevtab;
extern Dev ssldevtab;
extern Dev tlsdevtab;
extern Dev mousedevtab;
extern Dev drawdevtab;
extern Dev ipdevtab;
extern Dev fsdevtab;
extern Dev mntdevtab;
extern Dev lfddevtab;
extern Dev audiodevtab;
extern Dev kbddevtab;

Dev *devtab[] = {
	&rootdevtab,
	&consdevtab,
	&pipedevtab,
	&ssldevtab,
	&tlsdevtab,
	&mousedevtab,
	&drawdevtab,
	&ipdevtab,
	&fsdevtab,
	&mntdevtab,
	&lfddevtab,
	&audiodevtab,
	&kbddevtab,
	0
};

