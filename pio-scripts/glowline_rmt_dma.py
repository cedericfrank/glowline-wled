# glowline (REQ-165, firmware 1.19.5): give the ESP32-S3 LED output a DMA-fed RMT channel.
#
# WLED on IDF 5 drives the strip through NeoPixelBus's NeoEsp32RmtXMethod: a 192-symbol RMT block
# refilled by a low-priority interrupt every 96 symbols = 4 LEDs = 120 us. Anything that holds that
# interrupt off longer (a heap walk, a flash read in the web task) makes the channel resend stale
# symbols and the frame runs on past the set length. With DMA the refill comes every half of a
# 2000-symbol buffer: 41 LEDs = 1.25 ms of slack instead of 120 us (ESP-IDF v5.5.4 rmt_tx.c,
# rmt_tx_init_dma_link: ping_pong_symbols = mem_block_symbols / 2).
#
# NeoPixelBus hard-codes the channel config in a header, so this pre-build script patches the
# lumen env's own downloaded copy (.pio/libdeps/<env>/NeoPixelBus@src-*/..., the CORE3 pin from
# platformio.ini [esp32_idf_V5]) before it is compiled. The patch is behind GLOWLINE_RMT_DMA, which
# the lumen envs define as the symbol count; it also checks rmt_new_tx_channel's return, which the
# stock code drops ("ret +="): a refused config falls back to 1024 symbols, then to the stock
# 192-symbol, no-DMA channel, with a serial line for each step and the outcome at
# /json/info -> lumen.rmt (usermods/glowline/glowline.cpp glowlineRmtChannelCreated()).
#
# The script stops the build if it finds anything other than the exact stock function it knows
# (a NeoPixelBus update would otherwise build silently without the fix) or the file is missing.
# Patching an already patched copy is a no-op.
Import("env")
import glob
import os

ENV_NAME = env["PIOENV"]
MARKER = "glowline_rmt_dma.py"
REL_PATH = os.path.join("src", "internal", "methods", "ESP", "ESP32", "NeoEsp32RmtXMethod.h")

STOCK = """    void Initialize()
    {
        esp_err_t ret = ESP_OK;
        rmt_tx_channel_config_t config = {};
        config.clk_src = RMT_CLK_SRC_DEFAULT;
        config.gpio_num = static_cast<gpio_num_t>(_pin);
        config.mem_block_symbols = 192;         // memory block size, 64 * 4 = 256 Bytes
        config.resolution_hz = T_SPEED::RmtTicksPerSecond; // 1 MHz tick resolution, i.e., 1 tick = 1 µs
        config.trans_queue_depth = 4;           // set the number of transactions that can pend in the background
        config.flags.invert_out = T_INVERTED::Inverted;  // do not invert output signal
        config.flags.with_dma = false;          // do not need DMA backend

        ret += rmt_new_tx_channel(&config, &_channel);
"""

PATCHED = """    void Initialize()
    {
        esp_err_t ret = ESP_OK;
        rmt_tx_channel_config_t config = {};
        config.clk_src = RMT_CLK_SRC_DEFAULT;
        config.gpio_num = static_cast<gpio_num_t>(_pin);
        config.mem_block_symbols = 192;         // memory block size, 64 * 4 = 256 Bytes
        config.resolution_hz = T_SPEED::RmtTicksPerSecond; // 1 MHz tick resolution, i.e., 1 tick = 1 µs
        config.trans_queue_depth = 4;           // set the number of transactions that can pend in the background
        config.flags.invert_out = T_INVERTED::Inverted;  // do not invert output signal
        config.flags.with_dma = false;          // do not need DMA backend

#ifdef GLOWLINE_RMT_DMA
        // glowline (REQ-165): patched in at build time by pio-scripts/glowline_rmt_dma.py (see it
        // for why). A DMA-fed channel with GLOWLINE_RMT_DMA symbols is refilled every half buffer
        // (1000 symbols = 41 LEDs = 1.25 ms at 2000) instead of every 96 symbols (120 us), so a
        // short interrupt hold-off no longer resends stale symbols past the set length. The stock
        // code drops rmt_new_tx_channel's result; here a refused config falls back to 1024
        // symbols, then to the stock channel, so a bad number can never leave the strip dead.
        {
            const size_t symbols[] = { static_cast<size_t>(GLOWLINE_RMT_DMA), 1024 };
            esp_err_t err = ESP_FAIL;
            for (size_t i = 0; i < 2 && err != ESP_OK; i++)
            {
                if (i > 0 && symbols[i] == symbols[0]) break;
                config.mem_block_symbols = symbols[i];
                config.flags.with_dma = true;
                config.intr_priority = 3;
                _channel = nullptr;
                err = rmt_new_tx_channel(&config, &_channel);
                if (err != ESP_OK)
                {
                    Serial.printf("glowline rmt: DMA channel with %u symbols refused: %s\\n",
                                  static_cast<unsigned>(symbols[i]), esp_err_to_name(err));
                }
            }
            if (err != ESP_OK)
            {
                config.mem_block_symbols = 192;
                config.flags.with_dma = false;
                config.intr_priority = 0;
                _channel = nullptr;
                err = rmt_new_tx_channel(&config, &_channel);
                Serial.printf("glowline rmt: stock channel (192 symbols, no DMA): %s\\n", esp_err_to_name(err));
            }
            glowlineRmtChannelCreated(config.flags.with_dma, static_cast<unsigned>(config.mem_block_symbols), err);
            ret += err;
        }
#else
        ret += rmt_new_tx_channel(&config, &_channel);
#endif
"""

DECL_ANCHOR = "#define NEOPIXELBUS_RMT_INT_FLAGS (ESP_INTR_FLAG_LOWMED)\n"
DECL = DECL_ANCHOR + """
#ifdef GLOWLINE_RMT_DMA
// glowline (REQ-165): defined in usermods/glowline/glowline.cpp; reports the channel config
// Initialize() ended up with (/json/info -> lumen.rmt). Patched in by pio-scripts/glowline_rmt_dma.py.
void glowlineRmtChannelCreated(bool dma, unsigned symbols, esp_err_t err);
#endif
"""


def fail(msg):
    print("glowline_rmt_dma.py: " + msg)
    env.Exit(1)


libdeps = env.subst("$PROJECT_LIBDEPS_DIR")
pattern = os.path.join(libdeps, ENV_NAME, "NeoPixelBus*", REL_PATH)
files = sorted(glob.glob(pattern))
if len(files) != 1:
    fail("expected exactly one NeoPixelBus RMT header for env %s, found %d: %s (pattern %s)"
         % (ENV_NAME, len(files), files, pattern))
path = files[0]

flags = " ".join(str(f) for f in env.get("BUILD_FLAGS", []))
if "GLOWLINE_RMT_DMA" not in flags:
    fail("env %s runs this script but does not define GLOWLINE_RMT_DMA in its build_flags" % ENV_NAME)

with open(path, "r", encoding="utf-8") as fh:
    text = fh.read()

if MARKER in text:
    if PATCHED not in text or DECL not in text:
        fail("%s carries an earlier patch that does not match this script; delete .pio/libdeps/%s and rebuild"
             % (path, ENV_NAME))
    print("glowline_rmt_dma.py: %s already patched (GLOWLINE_RMT_DMA)" % path)
else:
    if text.count(STOCK) != 1 or text.count(DECL_ANCHOR) != 1:
        fail("%s is not the stock NeoEsp32RmtXMethod.h this script knows (NeoPixelBus CORE3 76afe832); "
             "refusing to build without the RMT DMA change" % path)
    text = text.replace(STOCK, PATCHED).replace(DECL_ANCHOR, DECL)
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(text)
    print("glowline_rmt_dma.py: patched %s (RMT channel with DMA, GLOWLINE_RMT_DMA symbols)" % path)
