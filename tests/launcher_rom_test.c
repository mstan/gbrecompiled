#include "launcher.h"
#include "gb_sha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%d: %s\n", __LINE__, #x); exit(1); } } while (0)

static int environment(const char *name, const char *value) {
#ifdef _WIN32
    return _putenv_s(name, value);
#else
    return setenv(name, value, 1);
#endif
}

static void fixture(const char *path, const char *bytes, char hash[65]) {
    FILE *file = fopen(path, "wb");
    CHECK(file);
    size_t size = strlen(bytes);
    CHECK(fwrite(bytes, 1, size, file) == size);
    CHECK(fclose(file) == 0);
    gb_sha256_hex((const uint8_t*)bytes, size, hash);
}

int main(int argc, char **argv) {
    CHECK(argc == 2);
    CHECK(environment("GBRECOMP_STATE_DIR", argv[1]) == 0);
    CHECK(environment("GBRECOMP_HEADLESS", "1") == 0);
    CHECK(environment("GBRECOMP_ROM_PATH", "") == 0);
    char one[1024], two[1024], first_hash[65], second_hash[65];
    snprintf(one, sizeof(one), "%s/one.gb", argv[1]);
    snprintf(two, sizeof(two), "%s/two.gb", argv[1]);
    fixture(one, "first synthetic cartridge", first_hash);
    fixture(two, "second synthetic cartridge", second_hash);
    launcher_init();
    launcher_set_rom_hint(one);
    launcher_set_expected_sha256(first_hash);
    CHECK(!strcmp(launcher_get_rom_path(), one));
    /* Selecting the second game must discard the first resolved path. */
    launcher_set_rom_hint(two);
    launcher_set_expected_sha256(second_hash);
    CHECK(!strcmp(launcher_get_rom_path(), two));
    launcher_set_rom_hint(two);
    CHECK(environment("GBRECOMP_ROM_PATH", one) == 0);
    CHECK(!strcmp(launcher_get_rom_hint(), one));
    CHECK(launcher_get_rom_path() == NULL); /* Explicit wrong cart: no dialog. */
    launcher_set_expected_sha256(first_hash);
    CHECK(!strcmp(launcher_get_rom_path(), one));
    CHECK(environment("GBRECOMP_ROM_PATH", "") == 0);
    launcher_set_rom_hint("missing-selected-cartridge.gb");
    CHECK(launcher_get_rom_path() == NULL);
    launcher_set_rom_hint(NULL);
    launcher_set_expected_sha256("0000000000000000000000000000000000000000000000000000000000000000");
    CHECK(launcher_get_rom_path() == NULL); /* No unattended native picker. */
    remove(one);
    remove(two);
    return 0;
}
