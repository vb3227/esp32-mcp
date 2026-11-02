// Private implementation included in the runtime namespace after the base API.
std::atomic<int> audioOwner{-1}, canOwner{-1}, counterOwner{-1}, sdOwner{-1};
SemaphoreHandle_t peripheralLock = nullptr;
bool audioTransmit = false;
#if ESP_IDF_VERSION_MAJOR >= 5
i2s_chan_handle_t audioChannel = nullptr;
#if SOC_PCNT_SUPPORTED
pcnt_unit_handle_t counterUnit = nullptr;
pcnt_channel_handle_t counterChannel = nullptr;
#endif
#if MCP_HAS_DAC
dac_oneshot_handle_t dacChannels[2] = {nullptr, nullptr};
#endif
#endif
int dacOwners[2] = {-1, -1};
int expansionOwners[16]; uint8_t expansionIdle[16];
void initHardwareIo() { memset(expansionOwners, -1, sizeof(expansionOwners)); memset(expansionIdle, 0, sizeof(expansionIdle)); }
int expansionPin(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    struct Pin { const char *name; int index; };
    const Pin pins[] = {{"P3",14},{"P4",13},{"P6",11},{"P8",8},{"P9",9},{"P10",10},{"P12",3},{"P13",4},{"P14",5},{"P15",6},{"P16",7}};
    int pin = -1; for (const auto &entry : pins) if (!strcmp(name, entry.name)) pin = entry.index;
    if (pin < 0) luaL_error(L, "Only external K10 logical pins P3/P4/P6/P8/P9/P10/P12..P16 are allowed");
    xSemaphoreTake(lock, portMAX_DELAY); int owner = current(L) - jobs;
    bool busy = expansionOwners[pin] >= 0 && expansionOwners[pin] != owner;
    if (!busy) expansionOwners[pin] = owner; xSemaphoreGive(lock);
    if (busy) luaL_error(L, "Expansion pin belongs to another job"); return pin;
}
int expansionWrite(lua_State *L) {
#ifdef MCP_PROFILE_K10
    check(L); int value = luaL_checkinteger(L, 2), idle = luaL_optinteger(L, 3, 0);
    if ((value != 0 && value != 1) || (idle != 0 && idle != 1)) return luaL_error(L, "Digital level/idle must be 0 or 1");
    int pin = expansionPin(L); xSemaphoreTake(peripheralLock, portMAX_DELAY);
    expansionIdle[pin] = idle; digital_write(static_cast<ePin_t>(pin), value); xSemaphoreGive(peripheralLock); return 0;
#else
    return luaL_error(L, "K10 board-expansion GPIO is unavailable in this profile");
#endif
}
int expansionRead(lua_State *L) {
#ifdef MCP_PROFILE_K10
    check(L); int pin = expansionPin(L); xSemaphoreTake(peripheralLock, portMAX_DELAY);
    int value = digital_read(static_cast<ePin_t>(pin)); xSemaphoreGive(peripheralLock); lua_pushinteger(L, value); return 1;
#else
    return luaL_error(L, "K10 expansion GPIO unavailable");
#endif
}
#if SOC_I2S_NUM > 1
constexpr int EXTERNAL_I2S = 1;
#else
constexpr int EXTERNAL_I2S = 0;
#endif

void requireOutput(lua_State *L, int pin) {
    if (!GPIO_IS_VALID_OUTPUT_GPIO(pin)) luaL_error(L, "Expected an output-capable GPIO");
    claim(L, pin);
}
int oneWireReset(lua_State *L) {
    int pin = luaL_checkinteger(L, 1); requireOutput(L, pin);
    gpio_set_direction(static_cast<gpio_num_t>(pin), GPIO_MODE_INPUT_OUTPUT_OD);
    gpio_set_level(static_cast<gpio_num_t>(pin), 0); delayMicroseconds(480);
    gpio_set_level(static_cast<gpio_num_t>(pin), 1); delayMicroseconds(70);
    bool present = !gpio_get_level(static_cast<gpio_num_t>(pin)); delayMicroseconds(410);
    lua_pushboolean(L, present); return 1;
}
int oneWireWrite(lua_State *L) {
    int pin = luaL_checkinteger(L, 1); size_t size; const char *bytes = luaL_checklstring(L, 2, &size);
    if (size > 16) return luaL_error(L, "1-Wire write limit is 16 bytes");
    requireOutput(L, pin); gpio_set_direction(static_cast<gpio_num_t>(pin), GPIO_MODE_INPUT_OUTPUT_OD);
    for (size_t i = 0; i < size; ++i) for (int bit = 0; bit < 8; ++bit) {
        bool high = (static_cast<uint8_t>(bytes[i]) >> bit) & 1;
        gpio_set_level(static_cast<gpio_num_t>(pin), 0); delayMicroseconds(high ? 6 : 60);
        gpio_set_level(static_cast<gpio_num_t>(pin), 1); delayMicroseconds(high ? 64 : 10);
    }
    check(L); return 0;
}
int oneWireRead(lua_State *L) {
    int pin = luaL_checkinteger(L, 1), size = luaL_optinteger(L, 2, 9);
    if (size < 1 || size > 16) return luaL_error(L, "1-Wire read limit is 1..16 bytes");
    requireOutput(L, pin); gpio_set_direction(static_cast<gpio_num_t>(pin), GPIO_MODE_INPUT_OUTPUT_OD);
    uint8_t bytes[16] = {};
    for (int i = 0; i < size; ++i) for (int bit = 0; bit < 8; ++bit) {
        gpio_set_level(static_cast<gpio_num_t>(pin), 0); delayMicroseconds(6);
        gpio_set_level(static_cast<gpio_num_t>(pin), 1); delayMicroseconds(9);
        bytes[i] |= gpio_get_level(static_cast<gpio_num_t>(pin)) << bit; delayMicroseconds(55);
    }
    check(L); lua_pushlstring(L, reinterpret_cast<const char *>(bytes), size); return 1;
}
int rs485Open(lua_State *L) {
    int tx = luaL_checkinteger(L, 1), rx = luaL_checkinteger(L, 2), de = luaL_checkinteger(L, 3), baud = luaL_optinteger(L, 4, 9600);
    if (de == tx || de == rx) return luaL_error(L, "RS485 DE must be distinct from TX/RX");
    requireOutput(L, de); lua_settop(L, 0); lua_pushinteger(L, tx); lua_pushinteger(L, rx); lua_pushinteger(L, baud);
    uartOpen(L); externalUart.setPins(rx, tx, -1, de);
    if (uart_set_mode(UART_NUM_1, UART_MODE_RS485_HALF_DUPLEX) != ESP_OK) { uartClose(L); return luaL_error(L, "RS485 configuration failed"); }
    current(L)->outputs |= 1ULL << de; return 0;
}
void closeAudio() {
#if ESP_IDF_VERSION_MAJOR >= 5
    if (audioChannel) { i2s_channel_disable(audioChannel); i2s_del_channel(audioChannel); audioChannel = nullptr; }
#else
    if (audioOwner >= 0) i2s_driver_uninstall(static_cast<i2s_port_t>(EXTERNAL_I2S));
#endif
    audioOwner = -1;
}
int i2sOpen(lua_State *L) {
    int bclk = luaL_checkinteger(L, 1), ws = luaL_checkinteger(L, 2), data = luaL_checkinteger(L, 3);
    int rate = luaL_optinteger(L, 4, 16000); bool transmit = lua_toboolean(L, 5);
    if (bclk == ws || bclk == data || ws == data || rate < 8000 || rate > 48000) return luaL_error(L, "I2S requires distinct pins and 8000..48000 Hz");
    requireOutput(L, bclk); requireOutput(L, ws); if (transmit) requireOutput(L, data); else claim(L, data);
    xSemaphoreTake(peripheralLock, portMAX_DELAY);
    if (audioOwner >= 0) { xSemaphoreGive(peripheralLock); return luaL_error(L, "External I2S already leased"); }
    bool ok = false;
#if ESP_IDF_VERSION_MAJOR >= 5
    i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(static_cast<i2s_port_t>(EXTERNAL_I2S), I2S_ROLE_MASTER);
    channel.dma_desc_num = 4; channel.dma_frame_num = 128;
    esp_err_t result = i2s_new_channel(&channel, transmit ? &audioChannel : nullptr, transmit ? nullptr : &audioChannel);
    if (result == ESP_OK) {
        i2s_std_config_t config = {};
        config.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(rate);
        config.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
        config.gpio_cfg.mclk = I2S_GPIO_UNUSED; config.gpio_cfg.bclk = static_cast<gpio_num_t>(bclk); config.gpio_cfg.ws = static_cast<gpio_num_t>(ws);
        config.gpio_cfg.dout = transmit ? static_cast<gpio_num_t>(data) : I2S_GPIO_UNUSED;
        config.gpio_cfg.din = transmit ? I2S_GPIO_UNUSED : static_cast<gpio_num_t>(data);
        ok = i2s_channel_init_std_mode(audioChannel, &config) == ESP_OK && i2s_channel_enable(audioChannel) == ESP_OK;
        if (!ok) { i2s_del_channel(audioChannel); audioChannel = nullptr; }
    }
#else
    i2s_config_t config = {};
    config.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | (transmit ? I2S_MODE_TX : I2S_MODE_RX));
    config.sample_rate = rate; config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT; config.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
    config.communication_format = I2S_COMM_FORMAT_STAND_I2S; config.intr_alloc_flags = 0; config.dma_buf_count = 4; config.dma_buf_len = 128;
    ok = i2s_driver_install(static_cast<i2s_port_t>(EXTERNAL_I2S), &config, 0, nullptr) == ESP_OK;
    if (ok) {
        i2s_pin_config_t pins = {}; pins.mck_io_num = I2S_PIN_NO_CHANGE; pins.bck_io_num = bclk; pins.ws_io_num = ws;
        pins.data_out_num = transmit ? data : I2S_PIN_NO_CHANGE; pins.data_in_num = transmit ? I2S_PIN_NO_CHANGE : data;
        ok = i2s_set_pin(static_cast<i2s_port_t>(EXTERNAL_I2S), &pins) == ESP_OK;
        if (!ok) i2s_driver_uninstall(static_cast<i2s_port_t>(EXTERNAL_I2S));
    }
#endif
    if (ok) { audioOwner = current(L) - jobs; audioTransmit = transmit; }
    xSemaphoreGive(peripheralLock);
    if (!ok) return luaL_error(L, "External I2S driver initialization failed");
    return 0;
}
int i2sRead(lua_State *L) {
    int size = luaL_optinteger(L, 1, 512);
    if (size < 2 || size > 1024 || size % 2 || audioOwner != current(L) - jobs || audioTransmit) return luaL_error(L, "Open RX I2S; read 2..1024 even bytes");
    uint8_t bytes[1024]; size_t count = 0; check(L);
#if ESP_IDF_VERSION_MAJOR >= 5
    i2s_channel_read(audioChannel, bytes, size, &count, 20);
#else
    i2s_read(static_cast<i2s_port_t>(EXTERNAL_I2S), bytes, size, &count, pdMS_TO_TICKS(20));
#endif
    check(L); lua_pushlstring(L, reinterpret_cast<const char *>(bytes), count); return 1;
}
int i2sWrite(lua_State *L) {
    size_t size; const char *data = luaL_checklstring(L, 1, &size);
    if (size > 1024 || size % 2 || audioOwner != current(L) - jobs || !audioTransmit) return luaL_error(L, "Open TX I2S; write at most 1024 even bytes");
    size_t count = 0; check(L);
#if ESP_IDF_VERSION_MAJOR >= 5
    i2s_channel_write(audioChannel, data, size, &count, 20);
#else
    i2s_write(static_cast<i2s_port_t>(EXTERNAL_I2S), data, size, &count, pdMS_TO_TICKS(20));
#endif
    check(L); lua_pushinteger(L, count); return 1;
}
int i2sClose(lua_State *L) {
    xSemaphoreTake(peripheralLock, portMAX_DELAY); if (audioOwner == current(L) - jobs) closeAudio(); xSemaphoreGive(peripheralLock); return 0;
}
int canOpen(lua_State *L) {
#if SOC_TWAI_SUPPORTED
    int tx = luaL_checkinteger(L, 1), rx = luaL_checkinteger(L, 2), baud = luaL_optinteger(L, 3, 500000);
    if (tx == rx) return luaL_error(L, "CAN pins must be distinct");
    requireOutput(L, tx); claim(L, rx);
    twai_timing_config_t timing = TWAI_TIMING_CONFIG_500KBITS();
    switch (baud) {
        case 125000: { twai_timing_config_t v = TWAI_TIMING_CONFIG_125KBITS(); timing = v; break; }
        case 250000: { twai_timing_config_t v = TWAI_TIMING_CONFIG_250KBITS(); timing = v; break; }
        case 500000: break;
        case 1000000: { twai_timing_config_t v = TWAI_TIMING_CONFIG_1MBITS(); timing = v; break; }
        default: return luaL_error(L, "CAN baud: 125000, 250000, 500000 or 1000000");
    }
    twai_general_config_t config = TWAI_GENERAL_CONFIG_DEFAULT(static_cast<gpio_num_t>(tx), static_cast<gpio_num_t>(rx), TWAI_MODE_NORMAL);
    config.tx_queue_len = 4; config.rx_queue_len = 8; twai_filter_config_t filter = TWAI_FILTER_CONFIG_ACCEPT_ALL();
    xSemaphoreTake(peripheralLock, portMAX_DELAY); bool installed = false, ok = false;
    if (canOwner < 0) { installed = twai_driver_install(&config, &timing, &filter) == ESP_OK; ok = installed && twai_start() == ESP_OK; }
    if (ok) canOwner = current(L) - jobs; else if (installed) twai_driver_uninstall();
    xSemaphoreGive(peripheralLock);
    if (!ok) return luaL_error(L, "CAN leased or initialization failed; an external transceiver is required");
    return 0;
#else
    return luaL_error(L, "CAN is unsupported on this chip");
#endif
}
int canSend(lua_State *L) {
#if SOC_TWAI_SUPPORTED
    uint32_t id = luaL_checkinteger(L, 1); size_t size; const char *bytes = luaL_checklstring(L, 2, &size); bool extended = lua_toboolean(L, 3);
    if (size > 8 || id > (extended ? 0x1fffffffU : 0x7ffU) || canOwner != current(L) - jobs) return luaL_error(L, "Open CAN first; valid identifier and 0..8 bytes required");
    twai_message_t message = {}; message.identifier = id; message.extd = extended; message.data_length_code = size; memcpy(message.data, bytes, size);
    check(L); lua_pushboolean(L, twai_transmit(&message, pdMS_TO_TICKS(20)) == ESP_OK); return 1;
#else
    return luaL_error(L, "CAN unsupported");
#endif
}
int canReceive(lua_State *L) {
#if SOC_TWAI_SUPPORTED
    if (canOwner != current(L) - jobs) return luaL_error(L, "Open CAN first");
    twai_message_t message = {}; check(L);
    if (twai_receive(&message, pdMS_TO_TICKS(20)) != ESP_OK) { lua_pushnil(L); return 1; }
    lua_newtable(L); lua_pushinteger(L, message.identifier); lua_setfield(L, -2, "id");
    lua_pushboolean(L, message.extd); lua_setfield(L, -2, "extended");
    lua_pushlstring(L, reinterpret_cast<const char *>(message.data), message.data_length_code); lua_setfield(L, -2, "data"); return 1;
#else
    return luaL_error(L, "CAN unsupported");
#endif
}
int canClose(lua_State *L) {
#if SOC_TWAI_SUPPORTED
    xSemaphoreTake(peripheralLock, portMAX_DELAY); if (canOwner == current(L) - jobs) { twai_stop(); twai_driver_uninstall(); canOwner = -1; } xSemaphoreGive(peripheralLock);
#endif
    return 0;
}
int pulseCounterOpen(lua_State *L) {
#if SOC_PCNT_SUPPORTED
    int pin = luaL_checkinteger(L, 1); claim(L, pin);
    xSemaphoreTake(peripheralLock, portMAX_DELAY); bool ok = false;
    if (counterOwner < 0) {
#if ESP_IDF_VERSION_MAJOR >= 5
        pcnt_unit_config_t config = {}; config.low_limit = -32768; config.high_limit = 32767;
        if (pcnt_new_unit(&config, &counterUnit) == ESP_OK) {
            pcnt_chan_config_t channel = {}; channel.edge_gpio_num = pin; channel.level_gpio_num = -1;
            ok = pcnt_new_channel(counterUnit, &channel, &counterChannel) == ESP_OK;
            if (ok) ok = pcnt_channel_set_edge_action(counterChannel, PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_HOLD) == ESP_OK && pcnt_unit_enable(counterUnit) == ESP_OK && pcnt_unit_start(counterUnit) == ESP_OK;
            if (!ok) { if (counterChannel) pcnt_del_channel(counterChannel); pcnt_del_unit(counterUnit); counterChannel = nullptr; counterUnit = nullptr; }
        }
#else
        pcnt_config_t config = {}; config.pulse_gpio_num = pin; config.ctrl_gpio_num = PCNT_PIN_NOT_USED; config.unit = PCNT_UNIT_0; config.channel = PCNT_CHANNEL_0;
        config.pos_mode = PCNT_COUNT_INC; config.neg_mode = PCNT_COUNT_DIS; config.lctrl_mode = config.hctrl_mode = PCNT_MODE_KEEP; config.counter_h_lim = 32767; config.counter_l_lim = -32768;
        ok = pcnt_unit_config(&config) == ESP_OK;
        if (ok) { pcnt_counter_clear(PCNT_UNIT_0); pcnt_counter_resume(PCNT_UNIT_0); }
#endif
        if (ok) counterOwner = current(L) - jobs;
    }
    xSemaphoreGive(peripheralLock); if (!ok) return luaL_error(L, "Pulse counter unavailable or leased"); return 0;
#else
    return luaL_error(L, "Hardware pulse counter unsupported");
#endif
}
int pulseCounterRead(lua_State *L) {
#if SOC_PCNT_SUPPORTED
    if (counterOwner != current(L) - jobs) return luaL_error(L, "Open pulse counter first");
#if ESP_IDF_VERSION_MAJOR >= 5
    int count = 0; pcnt_unit_get_count(counterUnit, &count); if (lua_toboolean(L, 1)) pcnt_unit_clear_count(counterUnit);
#else
    int16_t count = 0; pcnt_get_counter_value(PCNT_UNIT_0, &count); if (lua_toboolean(L, 1)) pcnt_counter_clear(PCNT_UNIT_0);
#endif
    lua_pushinteger(L, count); return 1;
#else
    return luaL_error(L, "Pulse counter unsupported");
#endif
}
void closeCounter() {
#if SOC_PCNT_SUPPORTED
#if ESP_IDF_VERSION_MAJOR >= 5
    if (counterUnit) { pcnt_unit_stop(counterUnit); pcnt_unit_disable(counterUnit); if (counterChannel) pcnt_del_channel(counterChannel); pcnt_del_unit(counterUnit); counterChannel = nullptr; counterUnit = nullptr; }
#else
    pcnt_counter_pause(PCNT_UNIT_0);
#endif
#endif
    counterOwner = -1;
}
int pulseCounterClose(lua_State *L) { xSemaphoreTake(peripheralLock, portMAX_DELAY); if (counterOwner == current(L) - jobs) closeCounter(); xSemaphoreGive(peripheralLock); return 0; }
int dacOutput(lua_State *L) {
#if MCP_HAS_DAC
    int pin = luaL_checkinteger(L, 1), value = luaL_checkinteger(L, 2);
#if defined(CONFIG_IDF_TARGET_ESP32S2)
    int index = pin == 17 ? 0 : pin == 18 ? 1 : -1;
#else
    int index = pin == 25 ? 0 : pin == 26 ? 1 : -1;
#endif
    if (index < 0 || value < 0 || value > 255) return luaL_error(L, "Use a chip DAC pin and value 0..255");
    requireOutput(L, pin); bool ok = true; xSemaphoreTake(peripheralLock, portMAX_DELAY);
#if ESP_IDF_VERSION_MAJOR >= 5
    if (!dacChannels[index]) { dac_oneshot_config_t config = {}; config.chan_id = static_cast<dac_channel_t>(index); ok = dac_oneshot_new_channel(&config, &dacChannels[index]) == ESP_OK; }
    if (ok) ok = dac_oneshot_output_voltage(dacChannels[index], value) == ESP_OK;
#else
    dac_channel_t channel = static_cast<dac_channel_t>(index + 1); ok = dac_output_enable(channel) == ESP_OK && dac_output_voltage(channel, value) == ESP_OK;
#endif
    if (ok) dacOwners[index] = current(L) - jobs; xSemaphoreGive(peripheralLock);
    if (!ok) return luaL_error(L, "DAC initialization failed"); return 0;
#else
    return luaL_error(L, "This chip has no internal DAC; use an external I2C/SPI DAC");
#endif
}
int touchInput(lua_State *L) {
#if MCP_HAS_TOUCH
    int pin = luaL_checkinteger(L, 1); claim(L, pin); lua_pushinteger(L, touchRead(pin)); return 1;
#else
    return luaL_error(L, "Capacitive touch unsupported");
#endif
}
int rmtTransmit(lua_State *L) {
    int pin = luaL_checkinteger(L, 1), carrier = luaL_optinteger(L, 3, 0);
    luaL_checktype(L, 2, LUA_TTABLE); size_t pulses = lua_rawlen(L, 2);
    if (!pulses || pulses > 160 || pulses % 2 || carrier < 0 || carrier > 100000 || (carrier && carrier < 1000)) return luaL_error(L, "RMT requires 2..160 even pulses; carrier 0 or 1000..100000 Hz");
    requireOutput(L, pin); uint32_t total = 0;
#if ESP_IDF_VERSION_MAJOR >= 5
    rmt_symbol_word_t symbols[80] = {};
#else
    rmt_item32_t symbols[80] = {};
#endif
    for (size_t i = 0; i < pulses; ++i) {
        lua_rawgeti(L, 2, i + 1); int duration = luaL_checkinteger(L, -1); lua_pop(L, 1);
        if (duration < 1 || duration > 32767 || (total += duration) > 100000) return luaL_error(L, "Pulse 1..32767 us; burst <=100000 us");
        if (i % 2) { symbols[i / 2].level1 = 0; symbols[i / 2].duration1 = duration; }
        else { symbols[i / 2].level0 = 1; symbols[i / 2].duration0 = duration; }
    }
    bool ok = false; xSemaphoreTake(peripheralLock, portMAX_DELAY);
#if ESP_IDF_VERSION_MAJOR >= 5
    rmt_channel_handle_t channel = nullptr; rmt_encoder_handle_t encoder = nullptr;
    rmt_tx_channel_config_t config = {}; config.gpio_num = static_cast<gpio_num_t>(pin); config.clk_src = RMT_CLK_SRC_DEFAULT; config.resolution_hz = 1000000; config.mem_block_symbols = SOC_RMT_MEM_WORDS_PER_CHANNEL; config.trans_queue_depth = 1;
    rmt_copy_encoder_config_t copy = {};
    ok = rmt_new_tx_channel(&config, &channel) == ESP_OK;
    if (ok) ok = rmt_new_copy_encoder(&copy, &encoder) == ESP_OK;
    if (ok && carrier) { rmt_carrier_config_t modulation = {}; modulation.frequency_hz = carrier; modulation.duty_cycle = .33f; ok = rmt_apply_carrier(channel, &modulation) == ESP_OK; }
    if (ok) ok = rmt_enable(channel) == ESP_OK;
    if (ok) { rmt_transmit_config_t transmit = {}; ok = rmt_transmit(channel, encoder, symbols, pulses / 2 * sizeof(symbols[0]), &transmit) == ESP_OK && rmt_tx_wait_all_done(channel, 150) == ESP_OK; }
    if (channel) { rmt_disable(channel); rmt_del_channel(channel); } if (encoder) rmt_del_encoder(encoder);
#else
    constexpr rmt_channel_t channel = RMT_CHANNEL_3;
    rmt_config_t config = RMT_DEFAULT_CONFIG_TX(static_cast<gpio_num_t>(pin), channel); config.clk_div = 80;
    config.tx_config.carrier_en = carrier != 0; config.tx_config.carrier_freq_hz = carrier; config.tx_config.carrier_duty_percent = 33;
    // Never reconfigure a channel already owned by the board's RGB/other driver.
    rmt_channel_status_result_t channelStatus = {};
    bool installed = rmt_get_channel_status(&channelStatus) == ESP_OK
        && channelStatus.status[channel] == RMT_CHANNEL_UNINIT
        && rmt_config(&config) == ESP_OK && rmt_driver_install(channel, 0, 0) == ESP_OK;
    ok = installed && rmt_write_items(channel, symbols, pulses / 2, false) == ESP_OK && rmt_wait_tx_done(channel, pdMS_TO_TICKS(150)) == ESP_OK;
    if (installed) rmt_driver_uninstall(channel);
#endif
    xSemaphoreGive(peripheralLock); check(L);
    if (!ok) return luaL_error(L, "RMT transmitter unavailable or hardware timeout"); return 0;
}
bool validCardPath(const char *path) { if (!path || path[0] != '/' || strlen(path) > 96 || strstr(path, "..") || strchr(path, '\\')) return false; return true; }
int sdOpen(lua_State *L) {
    int sck = luaL_checkinteger(L, 1), miso = luaL_checkinteger(L, 2), mosi = luaL_checkinteger(L, 3), cs = luaL_checkinteger(L, 4);
    if (sck == miso || sck == mosi || sck == cs || miso == mosi || miso == cs || mosi == cs) return luaL_error(L, "SD-SPI requires four distinct pins");
    requireOutput(L, sck); claim(L, miso); requireOutput(L, mosi); requireOutput(L, cs);
    xSemaphoreTake(busLock, portMAX_DELAY); bool ok = false;
    if (sdOwner < 0) { externalSpi.begin(sck, miso, mosi, cs); ok = SD.begin(cs, externalSpi, 4000000); if (ok) sdOwner = current(L) - jobs; else externalSpi.end(); }
    xSemaphoreGive(busLock); if (!ok) return luaL_error(L, "SD card unavailable or SPI bus leased"); return 0;
}
int sdRead(lua_State *L) {
    const char *path = luaL_checkstring(L, 1); int offset = luaL_optinteger(L, 2, 0), size = luaL_optinteger(L, 3, 256);
    if (!validCardPath(path) || offset < 0 || size < 1 || size > 512 || sdOwner != current(L) - jobs) return luaL_error(L, "Open SD; valid path, offset and 1..512 bytes required");
    uint8_t bytes[512]; int count = -1; xSemaphoreTake(busLock, portMAX_DELAY);
    File file = SD.open(path, FILE_READ);
    if (file && file.seek(offset)) count = file.read(bytes, size); file.close(); file = File(); xSemaphoreGive(busLock);
    if (count < 0) return luaL_error(L, "SD file read failed"); lua_pushlstring(L, reinterpret_cast<const char *>(bytes), count); return 1;
}
int sdWrite(lua_State *L) {
    const char *path = luaL_checkstring(L, 1); size_t size; const char *bytes = luaL_checklstring(L, 2, &size); bool append = lua_toboolean(L, 3);
    if (!validCardPath(path) || size > 512 || sdOwner != current(L) - jobs) return luaL_error(L, "Open SD; path and <=512 bytes required");
    size_t count = 0; xSemaphoreTake(busLock, portMAX_DELAY); File file = SD.open(path, append ? FILE_APPEND : FILE_WRITE);
    if (file && file.size() + size <= 65536) count = file.write(reinterpret_cast<const uint8_t *>(bytes), size); file.close(); file = File(); xSemaphoreGive(busLock);
    lua_pushinteger(L, count); return 1;
}
int sdClose(lua_State *L) { xSemaphoreTake(busLock, portMAX_DELAY); if (sdOwner == current(L) - jobs) { SD.end(); externalSpi.end(); sdOwner = -1; } xSemaphoreGive(busLock); return 0; }
void releaseHardwareIo(Job *job) {
    int owner = job - jobs; xSemaphoreTake(peripheralLock, portMAX_DELAY);
    if (audioOwner == owner) closeAudio();
#if SOC_TWAI_SUPPORTED
    if (canOwner == owner) { twai_stop(); twai_driver_uninstall(); canOwner = -1; }
#endif
    if (counterOwner == owner) closeCounter();
#ifdef MCP_PROFILE_K10
    for (int i = 0; i < 16; ++i) if (expansionOwners[i] == owner) {
        digital_write(static_cast<ePin_t>(i), expansionIdle[i]);
        xSemaphoreTake(lock, portMAX_DELAY); expansionOwners[i] = -1; xSemaphoreGive(lock);
    }
#endif
#if MCP_HAS_DAC
    for (int i = 0; i < 2; ++i) if (dacOwners[i] == owner) {
#if ESP_IDF_VERSION_MAJOR >= 5
        dac_oneshot_output_voltage(dacChannels[i], 0); dac_oneshot_del_channel(dacChannels[i]); dacChannels[i] = nullptr;
#else
        dac_output_disable(static_cast<dac_channel_t>(i + 1));
#endif
        dacOwners[i] = -1;
    }
#endif
    xSemaphoreGive(peripheralLock);
    xSemaphoreTake(busLock, portMAX_DELAY); if (sdOwner == owner) { SD.end(); externalSpi.end(); sdOwner = -1; } xSemaphoreGive(busLock);
}
const luaL_Reg hardwareIoApi[] = {
    {"onewire_reset", oneWireReset}, {"onewire_write", oneWireWrite}, {"onewire_read", oneWireRead}, {"rs485_open", rs485Open},
    {"i2s_open", i2sOpen}, {"i2s_read", i2sRead}, {"i2s_write", i2sWrite}, {"i2s_close", i2sClose},
    {"can_open", canOpen}, {"can_send", canSend}, {"can_receive", canReceive}, {"can_close", canClose},
    {"pcnt_open", pulseCounterOpen}, {"pcnt_read", pulseCounterRead}, {"pcnt_close", pulseCounterClose},
    {"dac_write", dacOutput}, {"touch_read", touchInput}, {"rmt_send", rmtTransmit},
    {"k10_gpio_write", expansionWrite}, {"k10_gpio_read", expansionRead},
    {"sd_open", sdOpen}, {"sd_read", sdRead}, {"sd_write", sdWrite}, {"sd_close", sdClose}, {nullptr, nullptr}
};
