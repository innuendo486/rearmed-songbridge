# Vendored libraries

Other people's code, unchanged, built into the mod by `build.sh`.

| Library | Version | From | What is here | License |
|---|---|---|---|---|
| libyaml | 0.2.5 | https://github.com/yaml/libyaml (tag `0.2.5`) | the reading half: `api.c`, `reader.c`, `scanner.c`, `parser.c`, `loader.c`, `yaml.h`, `yaml_private.h` | MIT (`libyaml/License`) |
| yyjson | 0.13.0 | https://github.com/ibireme/yyjson (tag `0.13.0`) | `yyjson.c`, `yyjson.h` | MIT (`yyjson/LICENSE`) |

SHA-256 of the release archives the files were taken from:

    fa240dbf262be053f3898006d502d514936c818e422afdcf33921c63bed9bf2e  libyaml 0.2.5 (archive/refs/tags/0.2.5.tar.gz)
    34e0f62a2bc11ab20d601e8ca1cc2b2079503aa45119a19133d89d19b94a0fae  yyjson 0.13.0 (archive/refs/tags/0.13.0.tar.gz)

`libyaml/config.h` is not libyaml's: it holds the version its own build would generate (`-DHAVE_CONFIG_H`). yyjson is built without its
writer and its utilities (`YYJSON_DISABLE_WRITER`, `YYJSON_DISABLE_UTILS`): the mod only reads JSON.
