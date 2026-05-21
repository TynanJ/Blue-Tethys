default_board := "nrf52840dk/nrf52840"

build target:
    west build -p \
        -b {{default_board}} \
        -d {{target}}/build \
        {{target}}

    # Fix clangd warnings
    @sed -i 's/-fno-reorder-functions//g' {{target}}/build/compile_commands.json
    @sed -i 's/-fno-printf-return-value//g' {{target}}/build/compile_commands.json
    @sed -i 's/-mfp16-format=ieee//g' {{target}}/build/compile_commands.json
