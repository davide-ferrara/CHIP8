#include "chip8.h"
#include <stdio.h>
#include <unistd.h>
#include <stdint.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include "raylib.h"
#include <stdlib.h>

#define W 64
#define H 32
#define RAM_SIZE 4096
#define STACK_SIZE 16
#define FONT_ADDR 0x50
#define CYCLES_PER_FRAME 10
#define SCALE 20

static const uint8_t chip8_font[80] = {
    0xF0, 0x90, 0x90, 0x90, 0xF0, // 0
    0x20, 0x60, 0x20, 0x20, 0x70, // 1
    0xF0, 0x10, 0xF0, 0x80, 0xF0, // 2
    0xF0, 0x10, 0xF0, 0x10, 0xF0, // 3
    0x90, 0x90, 0xF0, 0x10, 0x10, // 4
    0xF0, 0x80, 0xF0, 0x10, 0xF0, // 5
    0xF0, 0x80, 0xF0, 0x90, 0xF0, // 6
    0xF0, 0x10, 0x20, 0x40, 0x40, // 7
    0xF0, 0x90, 0xF0, 0x90, 0xF0, // 8
    0xF0, 0x90, 0xF0, 0x10, 0xF0, // 9
    0xF0, 0x90, 0xF0, 0x90, 0x90, // A
    0xE0, 0x90, 0xE0, 0x90, 0xE0, // B
    0xF0, 0x80, 0x80, 0x80, 0xF0, // C
    0xE0, 0x90, 0x90, 0x90, 0xE0, // D
    0xF0, 0x80, 0xF0, 0x80, 0xF0, // E
    0xF0, 0x80, 0xF0, 0x80, 0x80  // F
};

typedef struct {
    uint8_t display[H][W];
    uint8_t  ram[RAM_SIZE];
    uint16_t stack[STACK_SIZE];
    uint8_t  v[16];
    bool keys[16];
    uint16_t pc;
    uint8_t  sp;
    uint16_t i;
    uint8_t  dt;
    uint8_t  st;
    uint16_t last_write_addr;
    uint8_t  last_write_flash;
} Chip8;

Chip8 chip8 = {0};

/* 70s green phosphor theme */
#define CLR_BG      (Color){ 8, 12, 8, 255 }
#define CLR_TEXT    (Color){ 30, 220, 70, 255 }
#define CLR_PC      (Color){ 255, 60, 60, 255 }
#define CLR_I       (Color){ 80, 180, 255, 255 }
#define CLR_FLASH   (Color){ 255, 230, 60, 255 }
#define CLR_DIM     (Color){ 20, 120, 40, 255 }
#define PANEL_W     520
#define WIN_H       720
#define FONT_SIZE   14
#define ROW_H       18

static void hexdump(size_t start, size_t end) {
    if (end > RAM_SIZE) end = RAM_SIZE;
    if (start >= end) return;
    size_t row = start & ~0xF;
    for (size_t off = row; off < end; off += 16) {
        printf("\033[36m%08zx\033[0m: ", off);
        for (size_t i = 0; i < 16; i++) {
            size_t p = off + i;
            if (p < start || p >= end) printf("  ");
            else printf("%02x", chip8.ram[p]);
            if (i % 2 == 1) printf(" ");
        }
        printf(" ");
        for (size_t i = 0; i < 16; i++) {
            size_t p = off + i;
            if (p < start || p >= end) { printf(" "); continue; }
            uint8_t c = chip8.ram[p];
            printf("%c", c >= 32 && c < 127 ? c : '.');
        }
        printf("\n");
    }
}

void print_bits16(uint16_t n) {
    printf("%d = ", n);
    for (uint8_t i = 0; i < 16; i++) {
        uint8_t bit = ((n >> i) & 1) ? 1 : 0;
        printf("%d", bit);
    }
    printf("\n");
}

void print_chip8_reg(void) {
    for (uint8_t i = 0; i < 16; i++) {
        printf("\033[36mV[%X]\033[0m:%X ", i, chip8.v[i]);
        if (i == 7 || i == 15) printf("\n");
    }
}

void print_chip8_stack(void) {
    for (uint8_t i = 0; i < STACK_SIZE; i++) {
        printf("\033[36mS[%X]\033[0m:%X ", i, chip8.stack[i]);
        if (i == 3 || i == STACK_SIZE - 1) printf("\n");
    }
}

static int load_rom(char *rom) {
    printf("Trying to load '%s'\n", rom);
    int fd = open(rom, O_RDONLY);
    if (fd < 0) return 1;

    uint16_t i = 0x200;
    while (1) {
        int b = read(fd, &chip8.ram[i++], 1);
        if (b <= 0) break;
    }
    close(fd);
    return 0;
}

uint8_t read8(void) {
    return chip8.ram[chip8.pc];
}

uint8_t fetch8(void) {
    uint8_t val = read8();
    chip8.pc++;
    return val;
}

/* Returns Big-Endian value */
uint16_t read16(void) {
    return (uint16_t)(chip8.ram[chip8.pc] << 8 | chip8.ram[chip8.pc + 1]);
}

/* Returns Big-Endian value
 * TODO: CHECK FOR NOT EVEN JUMPS
 */
uint16_t fetch16(void) {
    uint16_t val = read16();
    chip8.pc += 2;
    return val;
}

/* Big-Endian to Little-Endian rappresentation */
uint16_t btol16(uint16_t big) {
    uint16_t lo = big >> 8;
    uint16_t hi = (big & 0x00FF) << 8;
    return (hi | lo);
}

static void clear_display(void) {
    memset(&chip8.display, 0, sizeof(chip8.display));
    return;
}

/* Push and Pop have no boundary checks for now should not be a problem */
static void push(uint16_t pc) {
    chip8.stack[chip8.sp++] = pc;
}

static uint16_t pop(void) {
    return chip8.stack[--chip8.sp];
}

static uint16_t get_nnn(uint16_t opcode) {
    return opcode & 0xFFF;
}

static uint8_t get_kk(uint16_t opcode) {
    return opcode & 0xFF;
}

static uint8_t get_x(uint16_t opcode) {
    return (opcode >> 8) & 0xF;
}

static uint8_t get_y(uint16_t opcode) {
    return (opcode >> 4) & 0xF;
}

static uint8_t get_cat(uint16_t opcode) {
    return (opcode >> 12) & 0xF;
}

static uint8_t get_n(uint16_t opcode) { return opcode & 0xF; }

static void skip(void) {
    chip8.pc += 2;
}

static void mark_write(uint16_t addr) {
    chip8.last_write_addr = addr;
    chip8.last_write_flash = 30;
}

/* Progrma Counter is already incremented by fecthing the opcode */
static void chip8_execute(void) {
    uint16_t opcode = fetch16(); // PC += 2
    switch (opcode >> 12) {
        case 0x0: {
            switch (opcode & 0xFF) {
                case 0xE0: clear_display(); printf("CLS\n"); break; // CLS
                case 0xEE: chip8.pc = pop(); printf("RET -> %X\n", chip8.pc); break; // RET
                default: printf("SYS (DEPRECATED) -> NOP\n"); break; // SYS
            }
            break;
        }
        // Jump to address NNN 
        case 0x1: {
            chip8.pc = get_nnn(opcode);
            printf("GOTO %X\n", chip8.pc);
            break;
        }
        // Execute subroutine starting at address NNN 
        case 0x2: {
            push(chip8.pc);
            chip8.pc = get_nnn(opcode);
            printf("CALL %X\n", chip8.pc);
            break;
        }
        // Skip the following instruction if the value of register VX equals NN 
        case 0x3: {
            uint8_t x = get_x(opcode);
            uint8_t kk = get_kk(opcode);
            if (chip8.v[x] == kk) skip(); // Skip next istruction
            printf("COND V[%X] == %X\n", chip8.v[x], kk);
            break;
        }
        // Skip the following instruction if the value of register VX is not equal to NN 
        case 0x4: {
            uint8_t x = get_x(opcode);
            uint8_t kk = get_kk(opcode);
            if (chip8.v[x] != kk) skip(); // Skip next istruction
            printf("COND V[%X] != %X\n", chip8.v[x], kk);
            break;
        }
        // Skip the following instruction if the value of register VX is equal to the value of register VY
        case 0x5: {
            uint8_t x = get_x(opcode);
            uint8_t y = get_y(opcode);
            if (chip8.v[x] == chip8.v[y]) skip(); // Skip next istruction
            printf("COND V[%X] == V[%X]\n", chip8.v[x], chip8.v[y]);
            break;
        }
        // Store number NN in register VX 
        case 0x6: {
            uint8_t x = get_x(opcode);
            uint8_t kk = get_kk(opcode);
            chip8.v[x] = kk;
            printf("MOV %X -> %X\n", x, kk);
            break;
        }
        // Add the value NN to register VX
        case 0x7: {
            uint8_t x = get_x(opcode);
            uint8_t kk = get_kk(opcode);
            chip8.v[x] += kk;
            printf("ADD %X -> V[%X]\n", kk, x);
            break;
        }
        case 0x8: {
            uint8_t x = get_x(opcode);
            uint8_t y = get_y(opcode);
            switch (get_n(opcode)) {
                // Store the value of register VY in register VX
                case 0x0: {
                    chip8.v[x] = chip8.v[y];
                    printf("STORE V[%X] -> V[%X]\n", x, y);
                    break;
                }
                // Set VX to VX OR VY 
                case 0x1: {
                    chip8.v[x] |= chip8.v[y];
                    printf("OR V[%X] |= V[%X]\n", x, y);
                    break;
                }
                // Set VX to VX AND VY 
                case 0x2: {
                    chip8.v[x] &= chip8.v[y];
                    printf("AND V[%X] &= V[%X]\n", x, y);
                    break;
                }
                // Set VX to VX XOR VY 
                case 0x3: {
                    chip8.v[x] ^= chip8.v[y];
                    printf("XOR V[%X] ^= V[%X]\n", x, y);
                    break;
                }
                // Add the value of register VY to register VX
                // Set VF to 01 if a carry occurs
                // Set VF to 00 if a carry does not occur
                case 0x4: {
                    uint16_t sum = (uint16_t)chip8.v[x] + chip8.v[y];
                    chip8.v[0xF] = sum > 0xFF ? 1 : 0;
                    chip8.v[x] = sum & 0xFF; // Casting to uint8_t
                    printf("ADD V[%X] += V[%X] VF=%X\n", x, y, chip8.v[0xF]);
                    break;
                }
                // Subtract the value of register VY from register VX
                // Set VF to 00 if a borrow occurs
                // Set VF to 01 if a borrow does not occur
                case 0x5: {
                    chip8.v[0xF] = chip8.v[x] >= chip8.v[y] ? 1 : 0;
                    chip8.v[x] -= chip8.v[y];
                    printf("SUB V[%X] -= V[%X] VF=%X\n", x, y, chip8.v[0xF]);
                    break;
                }
                // Store the value of register VY shifted right one bit in register VX¹
                // Set register VF to the least significant bit prior to the shift
                // VY is unchanged
                case 0x6: {
                    chip8.v[x] = chip8.v[y] >> 1;
                    chip8.v[0xF] = chip8.v[y] & 1;
                    printf("SHR V[%X] = V[%X] >> 1 VF=%X\n", x, y, chip8.v[0xF]);
                    break;
                }
                // Set register VX to the value of VY minus VX
                // Set VF to 00 if a borrow occurs
                // Set VF to 01 if a borrow does not occur
                case 0x7: {
                    chip8.v[0xF] = chip8.v[y] >= chip8.v[x] ? 1 : 0;
                    chip8.v[x] = chip8.v[y] - chip8.v[x];
                    printf("SUBN V[%X] = V[%X] - V[%X] VF=%X\n", x, y, x, chip8.v[0xF]);
                    break;
                }
                // Store the value of register VY shifted left one bit in register VX¹
                // Set register VF to the most significant bit prior to the shift
                // VY is unchanged
                case 0xE: {
                    chip8.v[x] = chip8.v[y] << 1;
                    chip8.v[0xF] = (chip8.v[y] >> 7) & 1;
                    printf("SHL V[%X] = V[%X] << 1 VF=%X\n", x, y, chip8.v[0xF]);
                    break;
                }
            }
            break;
        }
        // Skip the following instruction if the value of register VX is not equal to the value of register VY
        case 0x9: {
            uint8_t x = get_x(opcode);
            uint8_t y = get_y(opcode);
            if (chip8.v[x] != chip8.v[y]) skip();
            printf("COND V[%X] != V[%X]\n", chip8.v[x], chip8.v[y]);
            break;
        }
        // Store memory address NNN in register I
        case 0xA: {
            chip8.i = get_nnn(opcode);
            printf("MOV I -> %X\n", chip8.i);
            break;
        }
        // Jump to address NNN + V0
        case 0xB: {
            chip8.pc = get_nnn(opcode) + chip8.v[0];
            printf("GOTO V0 + %X -> %X\n", get_nnn(opcode), chip8.pc);
            break;
        }
        // Set VX to a random number with a mask of NN
        case 0xC: {
            uint8_t x = get_x(opcode);
            uint8_t kk = get_kk(opcode);
            chip8.v[x] = (uint8_t)(rand() & 0xFF) & kk;
            printf("RND V[%X] & %X -> %X\n", x, kk, chip8.v[x]);
            break;
        }
        // Draw a sprite at position VX, VY with N bytes of sprite data starting at the address stored in I
        // Set VF to 01 if any set pixels are changed to unset, and 00 otherwise
        case 0xD: {
            uint8_t x = chip8.v[get_x(opcode)] % W;
            uint8_t y = chip8.v[get_y(opcode)] % H;
            uint8_t n = get_n(opcode);
            chip8.v[0xF] = 0;

            for (uint8_t row = 0; row < n; row++) {
                if (y + row >= H) break;
                uint8_t sprite = chip8.ram[chip8.i + row];

                for (uint8_t col = 0; col < 8; col++) {
                    if (x + col >= W) break;
                    uint8_t bit = sprite & (0x80 >> col);
                    if (bit != 0) { // Pixel On
                        if (chip8.display[y + row][x + col] == 1) chip8.v[0xF] = 1;
                        chip8.display[y + row][x + col] ^= 1; // Flip
                    }
                }
            }
            printf("DRW V[%X], V[%X], %X VF=%X\n", get_x(opcode), get_y(opcode), n, chip8.v[0xF]);
            break;
        }
        case 0xE: {
            switch (get_kk(opcode)) {
                case 0x9E: {
                    uint8_t x = get_x(opcode);
                    if (chip8.keys[chip8.v[x] & 0xF]) skip();
                    printf("SKP V[%X] key=%X pressed=%d\n", x, chip8.v[x] & 0xF, chip8.keys[chip8.v[x] & 0xF]);
                    break;
                }
                case 0xA1: {
                    uint8_t x = get_x(opcode);
                    if (!chip8.keys[chip8.v[x] & 0xF]) skip();
                    printf("SKNP V[%X] key=%X pressed=%d\n", x, chip8.v[x] & 0xF, chip8.keys[chip8.v[x] & 0xF]);
                    break;
                }
                default: printf("TODO Ex%02X\n", get_kk(opcode)); break;
            }
            break;
        }
        case 0xF: {
            switch (get_kk(opcode)) {
                case 0x07: {
                    uint8_t x = get_x(opcode);
                    chip8.v[x] = chip8.dt;
                    printf("MOV V[%X] <- DT=%X\n", x, chip8.v[x]);
                    break;
                }
                case 0x0A: {
                    uint8_t x = get_x(opcode);
                    bool pressed = false;
                    for (int k = 0; k < 16; k++) {
                        if (chip8.keys[k]) {
                            chip8.v[x] = k;
                            pressed = true;
                            break;
                        }
                    }
                    if (!pressed) chip8.pc -= 2;
                    printf("WAIT KEY V[%X] pressed=%d\n", x, pressed);
                    break;
                }
                case 0x15: {
                    uint8_t x = get_x(opcode);
                    chip8.dt = chip8.v[x];
                    printf("MOV DT <- V[%X]=%X\n", x, chip8.v[x]);
                    break;
                }
                case 0x18: {
                    uint8_t x = get_x(opcode);
                    chip8.st = chip8.v[x];
                    printf("MOV ST <- V[%X]=%X\n", x, chip8.v[x]);
                    break;
                }
                case 0x1E: {
                    uint8_t x = get_x(opcode);
                    chip8.i += chip8.v[x];
                    printf("ADD I += V[%X] -> %X\n", x, chip8.i);
                    break;
                }
                // Set I to the memory address of the sprite data corresponding
                // to the hexadecimal digit stored in register VX
                case 0x29: {
                    uint8_t x = get_x(opcode);
                    chip8.i = FONT_ADDR + (chip8.v[x] & 0xF) * 5;
                    printf("MOV I <- font[%X] -> %X\n", chip8.v[x] & 0xF, chip8.i);
                    break;
                }
                // BCD
                case 0x33: {
                    uint8_t val = chip8.v[get_x(opcode)];
                    chip8.ram[chip8.i] = (val / 100);
                    chip8.ram[chip8.i + 1] = (val / 10) % 10;
                    chip8.ram[chip8.i + 2] = (val % 10);
                    mark_write(chip8.i);
                    printf("BCD V=%X -> %X %X %X\n", val, chip8.ram[chip8.i], chip8.ram[chip8.i+1], chip8.ram[chip8.i+2]);
                    break;
                }
                // Store the values of registers V0 to VX inclusive in memory starting at address I
                // I is set to I + X + 1 after operation²
                case 0x55: {
                    uint8_t x = get_x(opcode);
                    for (uint8_t i = 0; i <= x; i++) {
                        chip8.ram[chip8.i + i] = chip8.v[i];
                    }
                    mark_write(chip8.i);
                    chip8.i += x + 1;
                    printf("STORE V0..V%X -> RAM I=%X\n", x, chip8.i);
                    break;
                }
                // Fill registers V0 to VX inclusive with the values stored in memory starting at address I
                // I is set to I + X + 1 after operation²
                case 0x65: {
                    uint8_t x = get_x(opcode);
                    for (uint8_t i = 0; i <= x; i++) {
                        chip8.v[i] = chip8.ram[chip8.i + i] ;
                    }
                    chip8.i += x + 1;
                    printf("LOAD V0..V%X <- RAM I=%X\n", x, chip8.i);
                    break;
                }
                default: printf("TODO Fx%02X\n", opcode & 0xFF); break;
            }
            break;
        }
        default:
            printf("TODO OPCODE: %04X\n", opcode); break;
    }
}

static void chip8_loop(void) {
    while (1) {
        if (chip8.pc >= RAM_SIZE - 1) break;
        chip8_execute();
        usleep(100 * 1000);
    }
}

static int chip8_init(char *rom) {
    printf("CHIP8 Emulator!\n");

    if (load_rom(rom)) { printf("Could not load ROM, is provided path correct?\n"); return 1; }
    printf("ROM has been loaded!\n");

    chip8.pc = 0x200; // 512
    printf("PC set to 0x%04X\n", chip8.pc);

    clear_display();
    printf("Display cleared...\n");

    memcpy(&chip8.ram[FONT_ADDR], chip8_font, sizeof(chip8_font));
    printf("Font loaded...\n");

    return 0;
}

void handle_input(void) {
    memset(chip8.keys, 0, sizeof(chip8.keys));

    // Glove80: keypad shiftato di 1 a destra (WASD → ESDF)
    chip8.keys[0x1] = IsKeyDown(KEY_TWO);
    chip8.keys[0x2] = IsKeyDown(KEY_THREE);
    chip8.keys[0x3] = IsKeyDown(KEY_FOUR);
    chip8.keys[0xC] = IsKeyDown(KEY_FIVE);
    chip8.keys[0x4] = IsKeyDown(KEY_W);
    chip8.keys[0x5] = IsKeyDown(KEY_E);
    chip8.keys[0x6] = IsKeyDown(KEY_R);
    chip8.keys[0xD] = IsKeyDown(KEY_T);
    chip8.keys[0x7] = IsKeyDown(KEY_S);
    chip8.keys[0x8] = IsKeyDown(KEY_D);
    chip8.keys[0x9] = IsKeyDown(KEY_F);
    chip8.keys[0xE] = IsKeyDown(KEY_G);
    chip8.keys[0xA] = IsKeyDown(KEY_X);
    chip8.keys[0x0] = IsKeyDown(KEY_C);
    chip8.keys[0xB] = IsKeyDown(KEY_V);
    chip8.keys[0xF] = IsKeyDown(KEY_B);
}

static int mem_scroll = 0x200;

static void draw_registers(int x, int y) {
    char buf[160];

    sprintf(buf, "PC:%04X   I:%04X   SP:%02X", chip8.pc, chip8.i, chip8.sp);
    DrawText(buf, x, y, FONT_SIZE, CLR_TEXT);

    sprintf(buf, "DT:%02X     ST:%02X    WRT:%04X (%d)", chip8.dt, chip8.st,
            chip8.last_write_addr, chip8.last_write_flash);
    DrawText(buf, x, y + 20, FONT_SIZE, CLR_TEXT);

    sprintf(buf, "STACK:");
    DrawText(buf, x, y + 40, FONT_SIZE, CLR_DIM);
    char sb[200] = {0};
    for (int k = 0; k < chip8.sp && k < STACK_SIZE; k++) {
        char tmp[12];
        sprintf(tmp, " %03X", chip8.stack[k]);
        strcat(sb, tmp);
    }
    if (sb[0]) DrawText(sb, x + 70, y + 40, FONT_SIZE, CLR_TEXT);

    for (int row = 0; row < 2; row++) {
        for (int col = 0; col < 8; col++) {
            int i = row * 8 + col;
            Color c = (i == 0xF) ? CLR_I : CLR_TEXT;
            sprintf(buf, "V%X:%02X", i, chip8.v[i]);
            DrawText(buf, x + col * 75, y + 65 + row * 20, FONT_SIZE, c);
        }
    }

    sprintf(buf, "KEYS PRESSED:");
    DrawText(buf, x, y + 110, FONT_SIZE, CLR_DIM);
}

static void draw_memory(int x, int y, int w, int h) {
    int rows = h / ROW_H;
    int start = mem_scroll & ~0xF;

    DrawLine(x, y - 4, x + w, y - 4, CLR_DIM);

    for (int r = 0; r < rows; r++) {
        uint16_t addr = start + r * 16;
        if (addr >= RAM_SIZE) break;

        char buf[16];
        Color rowc = (addr >= 0x200) ? CLR_TEXT : CLR_DIM;
        sprintf(buf, "%04X:", addr);
        DrawText(buf, x, y + r * ROW_H, FONT_SIZE, rowc);

        for (int b = 0; b < 16; b++) {
            uint16_t a = addr + b;
            Color c = rowc;

            if (a == chip8.pc || a == (uint16_t)(chip8.pc + 1)) c = CLR_PC;
            else if (a == chip8.i) c = CLR_I;
            else if (a == chip8.last_write_addr && chip8.last_write_flash > 0) c = CLR_FLASH;

            sprintf(buf, "%02X", chip8.ram[a]);
            DrawText(buf, x + 50 + b * 24, y + r * ROW_H, FONT_SIZE, c);
        }
    }
}

void display(void) {
    const int screenWidth = W * SCALE + PANEL_W;
    const int screenHeight = WIN_H;

    InitWindow(screenWidth, screenHeight, "CHIP-8 Emulator");
    InitAudioDevice();      // Initialize audio device
    Sound beep = LoadSound("beep.wav");         // Load WAV audio file
    if (!IsAudioDeviceReady()) { printf("ERROR: Audio device not ready\n"); exit(1); };
    if (!IsSoundValid(beep)) { printf("ERROR: Sound not valid\n"); exit(1); }
    SetTargetFPS(60);

    while (!WindowShouldClose())
    {
        handle_input();
        for (uint8_t i = 0; i < CYCLES_PER_FRAME; i++) {
            chip8_execute();
        }
        if (chip8.dt > 0) chip8.dt--;
        if (chip8.st > 0) { PlaySound(beep); chip8.st--; }
        if (chip8.last_write_flash > 0) chip8.last_write_flash--;

        Vector2 mouse = GetMousePosition();
        if (mouse.x >= W * SCALE) {
            float wheel = GetMouseWheelMove();
            if (wheel != 0) {
                int max_scroll = RAM_SIZE - (PANEL_W / 24) * 16;
                mem_scroll -= (int)(wheel * 32);
                if (mem_scroll < 0) mem_scroll = 0;
                if (mem_scroll > max_scroll) mem_scroll = max_scroll;
            }
        }

        BeginDrawing();
        ClearBackground(CLR_BG);

        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
                if (chip8.display[y][x])
                    DrawRectangle(x * SCALE, y * SCALE, SCALE, SCALE, CLR_TEXT);

        int px = W * SCALE + 10;
        draw_registers(px, 10);
        draw_memory(px, 140, PANEL_W - 20, WIN_H - 170);

        DrawText("PC=red  I=blue  WRITE=yellow  (wheel=scroll)", px, WIN_H - 22, FONT_SIZE, CLR_DIM);

        EndDrawing();
    }

    UnloadSound(beep);     // Unload sound data
    CloseAudioDevice();     // Close audio device
    CloseWindow();
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("USAGE: chip8 <ROM>\n");
        return 0;
    }
    if (chip8_init(argv[1])) return -1;
    display();
    // hexdump(0, 800);
    // print_chip8_reg();
    // print_chip8_stack();
    return 0;
}
