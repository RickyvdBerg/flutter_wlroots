#include <stdio.h>
#include <stdlib.h>
#include "flutter_wlroots.h"

int main(int argc, const char *const argv[]) {
    struct fwr_instance_opts opts = {};
    opts.argc = argc;
    opts.argv = &argv[0];
    opts.assets_path = getenv("FWR_ASSETS_PATH");
    opts.icu_data_path = getenv("FWR_ICU_PATH");
    opts.elf_file_path = getenv("FWR_ELF_PATH");

    if (!opts.assets_path) {
        opts.assets_path = "build/example/flutter_assets";
    }
    if (!opts.icu_data_path) {
        opts.icu_data_path = "build/example/icudtl.dat";
    }
    if (!opts.elf_file_path) {
        opts.elf_file_path = "build/example/flutter_assets/app.so";
    }

    struct fwr_instance *instance;
    if (fwr_instance_create(opts, &instance)) {
        fwr_instance_run(instance);
    }

    printf("main exiting\n");
    return 0;
}
