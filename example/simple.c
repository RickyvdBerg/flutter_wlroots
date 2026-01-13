#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "flutter_wlroots.h"

#define MAX_ENGINE_ARGS 32

int main(int argc, const char *const argv[]) {
    const char *engine_argv[MAX_ENGINE_ARGS];
    int engine_argc = 0;

    engine_argv[engine_argc++] = argv[0];

    for (int i = 1; i < argc && engine_argc < MAX_ENGINE_ARGS; i++) {
        engine_argv[engine_argc++] = argv[i];
    }

    const char *switch_count_str = getenv("FLUTTER_ENGINE_SWITCHES");
    if (switch_count_str) {
        int switch_count = atoi(switch_count_str);
        for (int i = 1; i <= switch_count && engine_argc < MAX_ENGINE_ARGS; i++) {
            char env_name[64];
            snprintf(env_name, sizeof(env_name), "FLUTTER_ENGINE_SWITCH_%d", i);
            const char *switch_value = getenv(env_name);
            if (switch_value) {
                char *arg = malloc(strlen(switch_value) + 3);
                sprintf(arg, "--%s", switch_value);
                engine_argv[engine_argc++] = arg;
                printf("Adding engine switch: %s\n", arg);
            }
        }
    }

    struct fwr_instance_opts opts = {};
    opts.argc = engine_argc;
    opts.argv = engine_argv;
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
