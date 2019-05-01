#include <ppu.h>
#include <cpu.h>
#include <fce.h>
#include <memory.h>
#include <klib.h>
#include <stdint.h>

//#define PROFILE
//#define HAS_US_TIMER

static PPU_STATE ppu;
static byte PPU_RAM[0x4000];
static bool ppu_2007_first_read;
static byte ppu_addr_latch;
static bool ppu_sprite_hit_occured = false;
static uint16_t ppu_bg_XHLidx_for_sprite0_check[264][264 / 8];
static uint16_t ppu_bg_XHLidx[264][(264 + W) / 8];

static const word ppu_base_nametable_addresses[4] = { 0x2000, 0x2400, 0x2800, 0x2C00 };

// sprite
byte PPU_SPRRAM[0x100];
typedef struct {
  uint8_t y, tile, atr, x;
} SPR;
static const SPR *spr_array = (void *)PPU_SPRRAM;

// preprocess tables
static byte XHL[256 * 256][8]; // each valus is 0~3
static uint32_t ppu_ram_map[0x4000];
static uint16_t XHL16[256 * 256];
static uint16_t XHLmask16[256 * 256];

// PPUCTRL Functions

static uint32_t base_nametable_address;
static uint32_t vram_address_increment;
static uint32_t sprite_pattern_table_address;
static uint32_t background_pattern_table_address;
static uint32_t sprite_height;
static bool generates_nmi;

static inline void ppu_update_PPUCTRL_internal(byte PPUCTRL) {
  base_nametable_address = ppu_base_nametable_addresses[PPUCTRL & 0x3];
  vram_address_increment = common_bit_set(PPUCTRL, 2) ? 32 : 1;
  sprite_pattern_table_address = common_bit_set(PPUCTRL, 3) ? 0x1000 : 0x0000;
  background_pattern_table_address = common_bit_set(PPUCTRL, 4) ? 0x1000 : 0x0000;
  sprite_height = common_bit_set(PPUCTRL, 5) ? 16 : 8;
  generates_nmi = common_bit_set(PPUCTRL, 7);

  ppu.PPUCTRL = PPUCTRL;
}

inline bool ppu_generates_nmi() { return generates_nmi; }



// PPUMASK Functions

static inline bool ppu_renders_grayscale()                 { return ppu.PPUMASK[0]; }
static inline bool ppu_shows_background_in_leftmost_8px()  { return ppu.PPUMASK[1]; }
static inline bool ppu_shows_sprites_in_leftmost_8px()     { return ppu.PPUMASK[2]; }
static inline bool ppu_shows_background()                  { return ppu.PPUMASK[3]; }
static inline bool ppu_shows_sprites()                     { return ppu.PPUMASK[4]; }
static inline bool ppu_intensifies_reds()                  { return ppu.PPUMASK[5]; }
static inline bool ppu_intensifies_greens()                { return ppu.PPUMASK[6]; }
static inline bool ppu_intensifies_blues()                 { return ppu.PPUMASK[7]; }

static inline void ppu_set_renders_grayscale(bool yesno)                 { ppu.PPUMASK[0] = yesno; }
static inline void ppu_set_shows_background_in_leftmost_8px(bool yesno)  { ppu.PPUMASK[1] = yesno; }
static inline void ppu_set_shows_sprites_in_leftmost_8px(bool yesno)     { ppu.PPUMASK[2] = yesno; }
static inline void ppu_set_shows_background(bool yesno)                  { ppu.PPUMASK[3] = yesno; }
static inline void ppu_set_shows_sprites(bool yesno)                     { ppu.PPUMASK[4] = yesno; }
static inline void ppu_set_intensifies_reds(bool yesno)                  { ppu.PPUMASK[5] = yesno; }
static inline void ppu_set_intensifies_greens(bool yesno)                { ppu.PPUMASK[6] = yesno; }
static inline void ppu_set_intensifies_blues(bool yesno)                 { ppu.PPUMASK[7] = yesno; }



// PPUSTATUS Functions

static inline bool ppu_sprite_overflow()                      { return common_bit_set(ppu.PPUSTATUS, 5); }
static inline bool ppu_sprite_0_hit()                         { return common_bit_set(ppu.PPUSTATUS, 6); }
static inline bool ppu_in_vblank()                            { return common_bit_set(ppu.PPUSTATUS, 7); }

static inline void ppu_set_sprite_overflow(bool yesno)        { common_modify_bitb(&ppu.PPUSTATUS, 5, yesno); }
static inline void ppu_set_sprite_0_hit(bool yesno)           { common_modify_bitb(&ppu.PPUSTATUS, 6, yesno); }
static inline void ppu_set_in_vblank(bool yesno)              { common_modify_bitb(&ppu.PPUSTATUS, 7, yesno); }


// RAM


static inline word ppu_get_real_ram_address(word address)
{
    if (address < 0x2000) {
        return address;
    }
    else if (address < 0x3F00) {
        if (address < 0x3000) {
            return address;
        }
        else {
            return address;// - 0x1000;
        }
    }
    else if (address < 0x4000) {
        address = 0x3F00 | (address & 0x1F);
        if (address == 0x3F10 || address == 0x3F14 || address == 0x3F18 || address == 0x3F1C)
            return address - 0x10;
        else
            return address;
    }
    return 0xFFFF;
}

static inline uint32_t ppu_ram_read(uint32_t address)
{
	log("ppu_ram_read: address:%x, map:%x\n", address, ppu_ram_map[address]);
    return PPU_RAM[ppu_ram_map[address]];
}

static inline uint32_t ppu_ram_read_fast(uint32_t address)
{
    return PPU_RAM[address];
}

static inline void ppu_ram_write(word address, byte data)
{
    //assert(!(ppu_shows_background() && ppu_shows_sprites()));
    PPU_RAM[ppu_ram_map[address]] = data;
}

int ppu_read_idx(void) {
  return ppu_ram_read(0x3F00);
}

// 3F01 = 0F (00001111)
// 3F02 = 2A (00101010)
// 3F03 = 09 (00001001)
// 3F04 = 07 (00000111)
// 3F05 = 0F (00001111)
// 3F06 = 30 (00110000)
// 3F07 = 27 (00100111)
// 3F08 = 15 (00010101)
// 3F09 = 0F (00001111)
// 3F0A = 30 (00110000)
// 3F0B = 02 (00000010)
// 3F0C = 21 (00100001)
// 3F0D = 0F (00001111)
// 3F0E = 30 (00110000)
// 3F0F = 00 (00000000)
// 3F11 = 0F (00001111)
// 3F12 = 16 (00010110)
// 3F13 = 12 (00010010)
// 3F14 = 37 (00110111)
// 3F15 = 0F (00001111)
// 3F16 = 12 (00010010)
// 3F17 = 16 (00010110)
// 3F18 = 37 (00110111)
// 3F19 = 0F (00001111)
// 3F1A = 17 (00010111)
// 3F1B = 11 (00010001)
// 3F1C = 35 (00110101)
// 3F1D = 0F (00001111)
// 3F1E = 17 (00010111)
// 3F1F = 11 (00010001)
// 3F20 = 2B (00101011)

// Rendering
static void table_init() {
  for (int h = 0; h < 256; h ++)
    for (int l = 0; l < 256; l ++) {
      for (int x = 0; x < 8; x ++) {
        int col = (((h >> (7 - x)) & 1) << 1) | ((l >> (7 - x)) & 1);
        XHL[h * 256 + l][x] = col;
        XHL16[h * 256 + l] |= col << (x * 2);
        if (col == 0) {
          XHLmask16[h * 256] |= 0x3 << (x * 2);
        }
      }
  }

  for (int x = 0; x < 0x4000; x ++) {
    ppu_ram_map[x] = ppu_get_real_ram_address(x);
  }
  log("ppu_ram_map[0x3F00]=0x%x\n", ppu_ram_map[0x3F00]);
}

static uint32_t color_cache[4][4];
static uint32_t sprite_color_cache[4][4];

static void make_color_cache(void) {
  extern const uint32_t palette[64];
  int i;
  for (i = 0; i < 4; i ++) {
    uint32_t palette_address = 0x3F00 + (i << 2);
    // still in the range of identify mapping, can bypass ppu_ram_map[]
    // 0 for bbg
    color_cache[i][0] = palette[ppu_ram_read_fast(0x3f00)];
    color_cache[i][1] = palette[ppu_ram_read_fast(palette_address + 1)];
    color_cache[i][2] = palette[ppu_ram_read_fast(palette_address + 2)];
    color_cache[i][3] = palette[ppu_ram_read_fast(palette_address + 3)];

    palette_address = 0x3F10 + (i << 2);
    // still in the range of identify mapping, can bypass ppu_ram_map[]
    sprite_color_cache[i][1] = palette[ppu_ram_read_fast(palette_address + 1)];
    sprite_color_cache[i][2] = palette[ppu_ram_read_fast(palette_address + 2)];
    sprite_color_cache[i][3] = palette[ppu_ram_read_fast(palette_address + 3)];
  }
}

static uint32_t palette_attr_cache[4][256 >> 4][W >> 5];

static void make_attr_cache(void) {
  uint32_t palette_attr;
  int x, y, i;
  for (i = 0; i < 4; i ++) {
    uint32_t attribute_address = ppu_base_nametable_addresses[i] + 0x3C0;
    for (y = 0; y < (256 >> 4); y += 2) {
      for (x = 0; x < W >> 5; x ++) {
        palette_attr = ppu_ram_read_fast(attribute_address);
        bool left = x < 4;
        if (!left) { palette_attr >>= 2; }

        // !top
        palette_attr_cache[i][y + 1][x] = (palette_attr >> 4) & 3;
        // top
        palette_attr_cache[i][y][x] = palette_attr & 3;

        attribute_address ++;
      }
    }
  }
}

static uint32_t sprite_list[256][64];
static uint8_t sprite_list_cnt[256];

static void make_sprite_list(void) {
  memset(sprite_list_cnt, 0, sizeof(sprite_list_cnt));

  int i;
  for (i = 0; i < 64; i ++) {
    int y = spr_array[i].y;
    if (y < 0xef) {
#define macro(x) sprite_list[y + x][sprite_list_cnt[y + x] ++] = i
      macro(0); macro(1); macro(2); macro(3);
      macro(4); macro(5); macro(6); macro(7);
      if (sprite_height == 16) {
        macro( 8); macro( 9); macro(10); macro(11);
        macro(12); macro(13); macro(14); macro(15);
      }
#undef macro
    }
  }
}

static void ppu_preprocess(void) {
  make_color_cache();
  make_attr_cache();
  make_sprite_list();
}

extern bool do_update;

static inline void ppu_draw_background_scanline(bool mirror) {
    int tile_y = ppu.scanline >> 3;
    int taddr = base_nametable_address | (tile_y << 5);
    int pattern_table_base = background_pattern_table_address | (ppu.scanline & 0x7);

    int tile_x_max = 32;
    int tile_x = ppu_shows_background_in_leftmost_8px() ? 0 : 1;
    int skip_tiles = ppu.PPUSCROLL_X >> 3;
    if (mirror) {
      taddr += 0x400;
      // Skipping off-screen pixels
      tile_x_max = skip_tiles + 1;
    }
    else if (ppu.PPUSCROLL_X > 0) {
      tile_x += skip_tiles;
      taddr += skip_tiles;
    }

    uint16_t *p_bg_sprite0 = &ppu_bg_XHLidx_for_sprite0_check[ppu.scanline][0];
    uint16_t *p_bg = &ppu_bg_XHLidx[ppu.scanline][mirror ? W / 8 : 0];

    for (; tile_x < tile_x_max; tile_x ++) {
        int tile_index = ppu_ram_read_fast(taddr);
        uint32_t tile_address = pattern_table_base | (tile_index << 4);
        uint32_t l = ppu_ram_read_fast(tile_address);
        uint32_t XHLidx = (ppu_ram_read_fast(tile_address + 8) << 8) | l;

        // most of the tiles of bg are transparent, which are unnecessary to process
        p_bg[tile_x] = XHLidx;
        if (XHLidx != 0) {
          p_bg_sprite0[tile_x] = XHLidx;
        }

        taddr ++;
    }
}

static inline void ppu_update_background_scanline(bool mirror) {
    int scroll_base = 256 - ppu.PPUSCROLL_X;
    int tile_x_max = 32;
    int tile_x = ppu_shows_background_in_leftmost_8px() ? 0 : 1;
    int skip_tiles = ppu.PPUSCROLL_X >> 3;
    if (mirror) {
      scroll_base += 256;
      // Skipping off-screen pixels
      tile_x_max = skip_tiles + 1;
    }
    else if (ppu.PPUSCROLL_X > 0) {
      tile_x += skip_tiles;
      scroll_base += ppu.PPUSCROLL_X & ~0x7;
    }

    uint32_t *p_palette_attribute = &palette_attr_cache[(ppu.PPUCTRL & 0x3) + mirror]
      [ppu.scanline >> 4][0];
    uint16_t *p_bg = &ppu_bg_XHLidx[ppu.scanline][mirror ? W / 8 : 0];

    for (; tile_x < tile_x_max; tile_x ++) {
        uint32_t XHLidx = p_bg[tile_x];

        // most of the tiles of bg are transparent, which are unnecessary to process
        if (XHLidx != 0) {
          uint32_t *color_cache_line = color_cache[p_palette_attribute[tile_x >> 2]];
          byte *pXHL = &XHL[XHLidx][0];

#define macro(x) \
          draw_color(scroll_base + x, ppu.scanline, color_cache_line[pXHL[x]]);

          // loop unrolling
          macro(0); macro(1); macro(2); macro(3);
          macro(4); macro(5); macro(6); macro(7);
#undef macro
        }

        scroll_base += 8;
    }
}

static inline void check_sprite0_hit(int XHLidx, int y, int hflip) {
  int x;
  if (!ppu_sprite_hit_occured && ppu_shows_background()) {
    for (x = 0; x < 8; x ++) {
      int color = XHL[XHLidx][ (hflip ? 7 - x : x) ];
      if (color != 0) {
        uint32_t bg16 = XHL16[ppu_bg_XHLidx_for_sprite0_check[y][(spr_array[0].x + x) >> 3]];
        uint32_t bg = (bg16 >> (((spr_array[0].x + x) & 0x7) * 2)) & 0x3;
        if (bg == color) {
          ppu_set_sprite_0_hit(true);
          ppu_sprite_hit_occured = true;
        }
      }
    }
  }
}

void ppu_draw_sprite_scanline() {
    int n, i;
    int nr_sprite = sprite_list_cnt[ppu.scanline];

    // PPU can't render > 8 sprites
    if (nr_sprite > 8) {
      ppu_set_sprite_overflow(true);
      nr_sprite = 8;
    }

    for (i = 0; i < nr_sprite; i ++) {
        n = sprite_list[ppu.scanline][i];

        bool vflip = spr_array[n].atr & 0x80;
        bool hflip = spr_array[n].atr & 0x40;

        int y_in_tile = ppu.scanline & 0x7;
        uint32_t y = spr_array[n].y + y_in_tile;
        uint32_t tile_address = sprite_pattern_table_address + 16 * spr_array[n].tile + (vflip ? (7 - y_in_tile) : y_in_tile);
        uint32_t l = ppu_ram_read_fast(tile_address);
        uint32_t XHLidx = (ppu_ram_read_fast(tile_address + 8) << 8) | l;

        if (n == 0) check_sprite0_hit(XHLidx, y, hflip);

        if (do_update) {
          uint32_t palette_attribute = spr_array[n].atr & 0x3;
          uint32_t *color_cache_line = sprite_color_cache[palette_attribute];
          uint32_t sprite_x = spr_array[n].x + 256;

          byte *pXHL = &XHL[XHLidx][0];
          if (hflip) {
#define macro(x) \
            if (pXHL[x] != 0) { \
              draw_color(sprite_x + 7 - x, y, color_cache_line[pXHL[x]]); \
            }

            macro(0); macro(1); macro(2); macro(3);
            macro(4); macro(5); macro(6); macro(7);
#undef macro
          }
          else {
#define macro(x) \
            if (pXHL[x] != 0) { \
              draw_color(sprite_x + x, y, color_cache_line[pXHL[x]]); \
            }

            macro(0); macro(1); macro(2); macro(3);
            macro(4); macro(5); macro(6); macro(7);
#undef macro
          }
        }
    }
}


static uint32_t background_time, sprite_time, cpu_time;
#ifdef PROFILE
#ifdef HAS_US_TIMER
# define time_read(x) read_us(&x)
# define time_diff(t1, t0) us_timediff(&t1, &t0)
# define TIME_TYPE amtime
#else
# define time_read(x) x = uptime()
# define time_diff(t1, t0) (t1 - t0)
# define TIME_TYPE uint32_t
#endif
#else
# define time_read(x)
# define time_diff(t1, t0) 0
#endif

// PPU Lifecycle

void ppu_cycle() {
#ifdef PROFILE
  TIME_TYPE t0, t1, t2, t3, t4, t5;
#endif

    if (!ppu.ready && cpu_clock() > 29658)
        ppu.ready = true;

    time_read(t0);
    cpu_run(256);
    time_read(t1);

    ppu.scanline++;

    if (ppu.scanline < H && ppu_shows_background()) {
        ppu_draw_background_scanline(false);
        ppu_draw_background_scanline(true);
        if (do_update) {
          ppu_update_background_scanline(false);
          ppu_update_background_scanline(true);
        }
    }

    time_read(t2);
    cpu_run(85 - 16);
    time_read(t3);

    if (ppu.scanline < H && ppu_shows_sprites()) {
      ppu_draw_sprite_scanline();
    }

    time_read(t4);
    cpu_run(16);
    time_read(t5);

    cpu_time += time_diff(t1, t0) + time_diff(t3, t2) + time_diff(t5, t4);
    background_time += time_diff(t2, t1);
    sprite_time += time_diff(t4, t3);

    if (ppu.scanline == 241) {
        ppu_set_in_vblank(true);
        ppu_set_sprite_0_hit(false);
        cpu_interrupt();
    }
    else if (ppu.scanline == 262) {
        ppu.scanline = -1;
        ppu_sprite_hit_occured = false;
        ppu_set_in_vblank(false);

        time_read(t0);
        fce_update_screen();
        time_read(t1);

#ifdef PROFILE
        uint32_t total = cpu_time + background_time + sprite_time + time_diff(t1, t0);
        printf("Time(us): cpu + bg + spr + scr = (%d + %d + %d + %d)\t= %d\n",
            cpu_time, background_time, sprite_time, time_diff(t1, t0), total);
#endif
        cpu_time = 0;
        background_time = 0;
        sprite_time = 0;
    }
}

void ppu_run(int cycles) {
    while (cycles-- > 0) {
        ppu_cycle();
    }
}


inline void ppu_copy(word address, byte *source, int length)
{
    memcpy(&PPU_RAM[address], source, length);
}

inline byte ppu_io_read(word address)
{
    switch (address & 7) {
        case 2:
        {
            byte value = ppu.PPUSTATUS;
            ppu_set_in_vblank(false);
            ppu_set_sprite_0_hit(false);
            ppu.scroll_received_x = 0;
            ppu.PPUSCROLL = 0;
            ppu.addr_received_high_byte = 0;
            ppu_addr_latch = 0;
            ppu_2007_first_read = true;
            return value;
        }
        case 4: return PPU_SPRRAM[ppu.OAMADDR];
        case 7:
        {
            byte data;
            
            if (ppu.PPUADDR < 0x3F00) {
                data = ppu_ram_read_fast(ppu.PPUADDR);
            }
            else {
                data = ppu_ram_read(ppu.PPUADDR);
            }
            
            if (ppu_2007_first_read) {
                ppu_2007_first_read = false;
            }
            else {
                ppu.PPUADDR += vram_address_increment;
                ppu.PPUADDR &= 0x3FFF;
            }
            return data;
        }
        default:
            return 0xFF;
    }
}

inline void ppu_io_write(word address, byte data)
{
    switch (address & 7) {
        case 0: if (ppu.ready) ppu_update_PPUCTRL_internal(data); break;
        case 1: if (ppu.ready) byte_unpack(ppu.PPUMASK, data);
                  if (ppu_shows_background() && ppu_shows_sprites()) {
                    ppu_preprocess();
                  }
                  break;
        case 3: ppu.OAMADDR = data; break;
        case 4: PPU_SPRRAM[ppu.OAMADDR++] = data; break;
        case 5:
        {
            if (ppu.scroll_received_x)
                ppu.PPUSCROLL_Y = data;
            else
                ppu.PPUSCROLL_X = data;

            ppu.scroll_received_x ^= 1;
            break;
        }
        case 6:
        {
            if (!ppu.ready)
                return;

            if (ppu.addr_received_high_byte) {
              ppu.PPUADDR = (ppu_addr_latch << 8) + data;
              ppu.PPUADDR &= 0x3FFF;
            }
            else
                ppu_addr_latch = data;

            ppu.addr_received_high_byte ^= 1;
            ppu_2007_first_read = true;
            break;
        }
        case 7:
        {
            if (ppu.PPUADDR > 0x1FFF && ppu.PPUADDR < 0x4000) {
                ppu_ram_write(ppu.PPUADDR ^ ppu.mirroring_xor, data);
            }
            ppu_ram_write(ppu.PPUADDR, data);

            ppu.PPUADDR += vram_address_increment;
            ppu.PPUADDR &= 0x3FFF;
        }
    }
}

void ppu_init()
{
    ppu.PPUSTATUS = ppu.OAMADDR = ppu.PPUSCROLL_X = ppu.PPUSCROLL_Y = ppu.PPUADDR = 0;
    ppu_update_PPUCTRL_internal(0);
    byte_unpack(ppu.PPUMASK, 0);
    ppu.PPUSTATUS |= 0xA0;
    ppu.PPUDATA = 0;
    ppu_2007_first_read = true;
    table_init();
}

void ppu_set_background_color(byte color)
{
}

void ppu_set_mirroring(byte mirroring)
{
    ppu.mirroring = mirroring;
    ppu.mirroring_xor = 0x400 << mirroring;
}
