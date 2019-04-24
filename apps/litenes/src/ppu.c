#include <ppu.h>
#include <cpu.h>
#include <fce.h>
#include <memory.h>
#include <klib.h>
#include <stdint.h>

static const word ppu_base_nametable_addresses[4] = { 0x2000, 0x2400, 0x2800, 0x2C00 };

byte PPU_SPRRAM[0x100];
byte PPU_RAM[0x4000];
bool ppu_2007_first_read;
byte ppu_addr_latch;
PPU_STATE ppu;
byte ppu_latch;
bool ppu_sprite_hit_occured = false;
byte ppu_screen_background[264][264];


// preprocess tables
static byte XHL[256][256][8]; // each valus is 0~3
static uint64_t XHL64[256][256];
static uint64_t XHLmask[256][256];
static uint32_t ppu_ram_map[0x4000];

static inline void draw(int col, int row, int idx) {
  canvas[row][col + 0xff] = idx;
  if(idx != 0) {
	  log("row:%d, col:%d, idx:%d\n", row, col, idx);
  }
}

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

static inline uint64_t int_shl(int n, int s) {
  // compute (uint64_t)n << s without generating shld instruction in x86
  int hi, lo;
  if (s < 32) {
    lo = n << s;
    hi = (s == 0 ? 0 : n >> (32 - s));
  }
  else {
    lo = 0;
    hi = n << (s - 32);
  }
  return ((uint64_t)hi << 32) | lo;
}

// Rendering
static void table_init() {
  for (int h = 0; h < 256; h ++)
    for (int l = 0; l < 256; l ++) {
      for (int x = 0; x < 8; x ++) {
        int col = (((h >> (7 - x)) & 1) << 1) | ((l >> (7 - x)) & 1);
        XHL[h][l][x] = col;
        XHL64[h][l] |= int_shl(col, x * 8);
        if (col == 0) {
          XHLmask[h][l] |= int_shl(0xff,x * 8);
        }
      }
  }

  for (int x = 0; x < 0x4000; x ++) {
    ppu_ram_map[x] = ppu_get_real_ram_address(x);
  }
  log("ppu_ram_map[0x3F00]=0x%x\n", ppu_ram_map[0x3F00]);
}

static int palette_cache[4][4];

void palette_cache_read() {
  int i;
  for (i = 0; i < 4; i ++) {
    word palette_address = 0x3F00 + (i << 2);
    // still in the range of identify mapping, can bypass ppu_ram_map[]
    palette_cache[i][1] = ppu_ram_read_fast(palette_address + 1);
    palette_cache[i][2] = ppu_ram_read_fast(palette_address + 2);
    palette_cache[i][3] = ppu_ram_read_fast(palette_address + 3);
  }
}

void ppu_draw_background_scanline(bool mirror) {
    int tile_x, tile_y = ppu.scanline >> 3;
    int taddr = base_nametable_address + (tile_y << 5) + (mirror ? 0x400 : 0);
    int y_in_tile = ppu.scanline & 0x7;
    int scroll_base = - ppu.PPUSCROLL_X + (mirror ? 256 : 0);

    int do_update = frame_cnt % 3 == 0;
    bool top = (ppu.scanline & 31) < 16;

    uint32_t attribute_address = (base_nametable_address + (mirror ? 0x400 : 0) + 0x3C0 +  -1 + ((ppu.scanline >> 5) << 3));
    uint32_t palette_attribute = ppu_ram_read_fast(attribute_address);
    if (!top) {
      palette_attribute >>= 4;
    }
    palette_attribute &= 3;
    int *palette_cache_line = palette_cache[palette_attribute];

    int off_screen_idx = 256 + ppu.PPUSCROLL_X - (mirror ? 256 : 0);

    for (tile_x = ppu_shows_background_in_leftmost_8px() ? 0 : 1; tile_x < 32; tile_x++) {
        // Skipping off-screen pixels
        if ((tile_x << 3)  > off_screen_idx)
            continue;

        int tile_index = ppu_ram_read_fast(taddr);
        uint32_t tile_address = background_pattern_table_address + (tile_index << 4);

        uint32_t l = ppu_ram_read_fast(tile_address + y_in_tile);
        uint32_t h = ppu_ram_read_fast(tile_address + y_in_tile + 8);

        if (do_update) {
            if ((tile_x & 3) == 0) {
              attribute_address ++;
              palette_attribute = ppu_ram_read_fast(attribute_address);
              bool left = (tile_x < 16);
              if (!top) {
                palette_attribute >>= 4;
              }
              if (!left) {
                palette_attribute >>= 2;
              }
              palette_attribute &= 3;
              palette_cache_line = palette_cache[palette_attribute];
            }

            union {
              uint64_t u64;
              byte color[8];
            } buf;
            buf.u64 = XHL64[h][l];

#define macro(x) \
            if (buf.color[x] != 0) { \
              draw(scroll_base + x, ppu.scanline + 1, palette_cache_line[buf.color[x]]); \
            }

            // loop unrolling
            macro(0); macro(1); macro(2); macro(3);
            macro(4); macro(5); macro(6); macro(7);
        }
        uint64_t *ptr = (uint64_t*)&ppu_screen_background[ppu.scanline][(tile_x << 3)];
        *ptr = (XHL64[h][l]) | (XHLmask[h][l] & (*ptr)) ;

        taddr ++;
        scroll_base += 8;
    }
}

void ppu_draw_sprite_scanline() {
    int do_update = frame_cnt % 3 == 0;
    int scanline_sprite_count = 0;
    int i, n;

    int sprite_palette_cache[4][4];
    for (i = 0; i < 4; i ++) {
      uint32_t palette_address = 0x3F10 + (i << 2);
      // still in the range of identify mapping, can bypass ppu_ram_map[]
      sprite_palette_cache[i][1] = ppu_ram_read_fast(palette_address + 1);
      sprite_palette_cache[i][2] = ppu_ram_read_fast(palette_address + 2);
      sprite_palette_cache[i][3] = ppu_ram_read_fast(palette_address + 3);
    }

    for (n = 0; n < 0x100; n += 4) {
        uint32_t sprite_x = PPU_SPRRAM[n + 3];
        uint32_t sprite_y = PPU_SPRRAM[n];

        // Skip if sprite not on scanline
        if (sprite_y > ppu.scanline || sprite_y + sprite_height < ppu.scanline)
           continue;

        scanline_sprite_count++;

        // PPU can't render > 8 sprites
        if (scanline_sprite_count > 8) {
            ppu_set_sprite_overflow(true);
            return;
            // break;
        }

        bool vflip = PPU_SPRRAM[n + 2] & 0x80;
        bool hflip = PPU_SPRRAM[n + 2] & 0x40;

        uint32_t tile_address = sprite_pattern_table_address + 16 * PPU_SPRRAM[n + 1];
        int y_in_tile = ppu.scanline & 0x7;
        uint32_t l = ppu_ram_read_fast(tile_address + (vflip ? (7 - y_in_tile) : y_in_tile));
        uint32_t h = ppu_ram_read_fast(tile_address + (vflip ? (7 - y_in_tile) : y_in_tile) + 8);

        uint32_t palette_attribute = PPU_SPRRAM[n + 2] & 0x3;
        int *palette_cache_line = sprite_palette_cache[palette_attribute];
        int x;
        for (x = 0; x < 8; x++) {
            int color = XHL[h][l][ (hflip ? 7 - x : x) ];

            // Color 0 is transparent
            if (color != 0) {
                int screen_x = sprite_x + x;

                if (do_update) {
                    draw(screen_x, sprite_y + y_in_tile + 1, palette_cache_line[color]);
                }

                // Checking sprite 0 hit
                if (n == 0 && !ppu_sprite_hit_occured && ppu_shows_background() && ppu_screen_background[sprite_y + y_in_tile][screen_x] == color) {
                    ppu_set_sprite_0_hit(true);
                    ppu_sprite_hit_occured = true;
                }
            }
        }
    }
}



// PPU Lifecycle

static inline void ppu_cycle()
{
    if (!ppu.ready && cpu_clock() > 29658)
        ppu.ready = true;

    ppu.scanline++;
	log("flag:%x, %x\n", ppu_shows_background(), ppu_shows_sprites());
    if (ppu_shows_background()) {
		log("shows_background\n");
        // preprocessing
        palette_cache_read();

        ppu_draw_background_scanline(false);
        ppu_draw_background_scanline(true);
    }
    
    if (ppu_shows_sprites()) {
		log("ppu_draw_sprite_scanline\n");
		ppu_draw_sprite_scanline();
	}

    if (ppu.scanline == 241) {
        ppu_set_in_vblank(true);
        ppu_set_sprite_0_hit(false);
        cpu_interrupt();
    }
    else if (ppu.scanline == 262) {
        ppu.scanline = -1;
        ppu_sprite_hit_occured = false;
        ppu_set_in_vblank(false);
		log("fce_update_screen\n");
        fce_update_screen();
    }
}

void ppu_run(int cycles)
{
	log("cycles:%d\n", cycles);
    while (cycles-- > 0) {
        ppu_cycle();
    }
	log("cycles:%d\n", cycles);
}


inline void ppu_copy(word address, byte *source, int length)
{
    memcpy(&PPU_RAM[address], source, length);
}

inline byte ppu_io_read(word address)
{
    ppu.PPUADDR &= 0x3FFF;
    switch (address & 7) {
        case 2:
        {
            byte value = ppu.PPUSTATUS;
            ppu_set_in_vblank(false);
            ppu_set_sprite_0_hit(false);
            ppu.scroll_received_x = 0;
            ppu.PPUSCROLL = 0;
            ppu.addr_received_high_byte = 0;
            ppu_latch = value;
            ppu_addr_latch = 0;
            ppu_2007_first_read = true;
            return value;
        }
        case 4: return ppu_latch = PPU_SPRRAM[ppu.OAMADDR];
        case 7:
        {
            byte data;
            
            if (ppu.PPUADDR < 0x3F00) {
                data = ppu_latch = ppu_ram_read(ppu.PPUADDR);
            }
            else {
                data = ppu_ram_read(ppu.PPUADDR);
                ppu_latch = 0;
            }
            
            if (ppu_2007_first_read) {
                ppu_2007_first_read = false;
            }
            else {
                ppu.PPUADDR += vram_address_increment;
            }
            return data;
        }
        default:
            return 0xFF;
    }
}

inline void ppu_io_write(word address, byte data)
{
    address &= 7;
    ppu_latch = data;
    ppu.PPUADDR &= 0x3FFF;
    switch(address) {
        case 0: if (ppu.ready) ppu_update_PPUCTRL_internal(data); break;
        case 1: if (ppu.ready) byte_unpack(ppu.PPUMASK, data); break;
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

            if (ppu.addr_received_high_byte)
                ppu.PPUADDR = (ppu_addr_latch << 8) + data;
            else
                ppu_addr_latch = data;

            ppu.addr_received_high_byte ^= 1;
            ppu_2007_first_read = true;
            break;
        }
        case 7:
        {
            if (ppu.PPUADDR > 0x1FFF || ppu.PPUADDR < 0x4000) {
				log("ppu_write.1(%x,%d)\n", ppu.PPUADDR ^ ppu.mirroring_xor, data);
                ppu_ram_write(ppu.PPUADDR ^ ppu.mirroring_xor, data);
				log("ppu_write.2(%x,%d)\n", ppu.PPUADDR, data);
                ppu_ram_write(ppu.PPUADDR, data);
            }
            else {
				log("ppu_write.3(%x,%d)\n", ppu.PPUADDR, data);
                ppu_ram_write(ppu.PPUADDR, data);
            }

            ppu.PPUADDR += vram_address_increment;
        }
    }
    ppu_latch = data;
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

void ppu_sprram_write(byte data)
{
    PPU_SPRRAM[ppu.OAMADDR++] = data;
}

void ppu_set_background_color(byte color)
{
}

void ppu_set_mirroring(byte mirroring)
{
    ppu.mirroring = mirroring;
    ppu.mirroring_xor = 0x400 << mirroring;
}
