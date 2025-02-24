#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/adc.h"
#include "hardware/pwm.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "pico/critical_section.h"
#include "ws2818b.pio.h"
#include "inc/ssd1306.h"

// ---------------------- Configurações Gerais ----------------------
#define LED_COUNT         25
#define LED_PIN           7

#define LOWER_THRESHOLD   500
#define UPPER_THRESHOLD   (4095 - 500)

#define BUTTON_PIN        6           // Botão principal
#define ERASE_BUTTON_PIN  5           // Botão extra

#define RED_LED_PIN       13          // LED externo (vermelho)
#define GREEN_LED_PIN     11          // LED externo (verde)
#define BUZZER_PIN        21          // Buzzer (via PWM)

#define FACE_COLOR_R      100
#define FACE_COLOR_G      0
#define FACE_COLOR_B      0

#define I2C_SDA           14
#define I2C_SCL           15

#define DECAY_INTERVAL_MS 60000

// ---------------------- Declarações e Variáveis Globais ----------------------
typedef struct pixel {
    uint8_t G, R, B;
} pixel_t;
pixel_t leds[LED_COUNT];

PIO np_pio;
uint sm;
uint8_t led_dma_buffer[LED_COUNT * 3];
int dma_channel;

critical_section_t cs;

int led_index(int row, int col) {
    return (4 - row) * 5 + col;
}

typedef struct {
    int fome;
    int higiene;
    int energia;
    int diversao;
} bob_status_t;
bob_status_t bob = {75, 75, 75, 75};

float decay_multiplier = 1.0f;
uint32_t last_decay_ms = 0;

// ---------------------- Faces do Bob ----------------------
static const uint8_t face_happy[5][5] = {
    {0, 1, 0, 1, 0},
    {0, 1, 0, 1, 0},
    {1, 0, 0, 0, 1},
    {0, 1, 1, 1, 0},
    {0, 0, 0, 0, 0}
};

static const uint8_t face_neutral[5][5] = {
    {0, 1, 0, 1, 0},
    {0, 1, 0, 1, 0},
    {0, 0, 0, 0, 0},
    {1, 1, 1, 1, 1},
    {0, 0, 0, 0, 0}
};

static const uint8_t face_sad[5][5] = {
    {0, 1, 0, 1, 0},
    {0, 1, 0, 1, 0},
    {0, 0, 0, 0, 0},
    {0, 1, 1, 1, 0},
    {1, 0, 0, 0, 1}
};

/**
 * Seleciona a face a ser exibida conforme os atributos do Bob.
 */
const uint8_t (*select_face(void))[5] {
    if (bob.fome < 30 || bob.higiene < 30 || bob.energia < 30 || bob.diversao < 30)
        return face_sad;
    int acima = 0, medio = 0;
    if (bob.fome > 50) acima++; else medio++;
    if (bob.higiene > 50) acima++; else medio++;
    if (bob.energia > 50) acima++; else medio++;
    if (bob.diversao > 50) acima++; else medio++;
    return (acima > medio) ? face_happy : face_neutral;
}

// ---------------------- Funções para a Matriz de LEDs ----------------------
void npInit(uint pin) {
    uint offset = pio_add_program(pio0, &ws2818b_program);
    np_pio = pio0;
    sm = pio_claim_unused_sm(np_pio, false);
    if (sm < 0) {
        np_pio = pio1;
        sm = pio_claim_unused_sm(np_pio, true);
    }
    ws2818b_program_init(np_pio, sm, offset, pin, 800000.f);
    for (uint i = 0; i < LED_COUNT; i++) {
        leds[i].R = 0;
        leds[i].G = 0;
        leds[i].B = 0;
    }
    critical_section_init(&cs);
    dma_channel = dma_claim_unused_channel(true);
}

/**
 * Define a cor de um LED na posição indicada.
 */
void npSetLED(const uint index, const uint8_t r, const uint8_t g, const uint8_t b) {
    leds[index].R = r;
    leds[index].G = g;
    leds[index].B = b;
}

/**
 * Atualiza os LEDs via DMA.
 */
void npWrite() {
    for (int i = 0; i < LED_COUNT; i++) {
        int base = i * 3;
        led_dma_buffer[base + 0] = leds[i].G;
        led_dma_buffer[base + 1] = leds[i].R;
        led_dma_buffer[base + 2] = leds[i].B;
    }
    
    dma_channel_config cfg = dma_channel_get_default_config(dma_channel);
    channel_config_set_transfer_data_size(&cfg, DMA_SIZE_8);
    channel_config_set_read_increment(&cfg, true);
    channel_config_set_write_increment(&cfg, false);
    channel_config_set_dreq(&cfg, pio_get_dreq(np_pio, sm, true));
    
    volatile void *fifo_addr = &np_pio->txf[sm];
    
    dma_channel_configure(
        dma_channel,
        &cfg,
        (void *)fifo_addr,
        led_dma_buffer,
        LED_COUNT * 3,
        true
    );
    
    dma_channel_wait_for_finish_blocking(dma_channel);
}

/**
 * Desenha um padrão (por exemplo, a face do Bob) na matriz de LEDs.
 */
void draw_pattern(const uint8_t pattern[5][5]) {
    for (int row = 0; row < 5; row++) {
        for (int col = 0; col < 5; col++) {
            int index = led_index(row, col);
            if (pattern[row][col])
                npSetLED(index, FACE_COLOR_R, FACE_COLOR_G, FACE_COLOR_B);
            else
                npSetLED(index, 0, 0, 0);
        }
    }
    npWrite();
}

// ---------------------- Funções do Buzzer ----------------------
void pwm_init_buzzer(uint pin) {
    gpio_set_function(pin, GPIO_FUNC_PWM);
    uint slice_num = pwm_gpio_to_slice_num(pin);
    pwm_config config = pwm_get_default_config();
    pwm_config_set_clkdiv(&config, 4.0f);
    pwm_init(slice_num, &config, true);
    pwm_set_gpio_level(pin, 0);
}

void play_tone(uint pin, uint frequency, uint duration_ms) {
    uint slice_num = pwm_gpio_to_slice_num(pin);
    uint32_t clock_freq = clock_get_hz(clk_sys);
    uint32_t top = clock_freq / frequency - 1;
    pwm_set_wrap(slice_num, top);
    pwm_set_gpio_level(pin, top / 2);
    sleep_ms(duration_ms);
    pwm_set_gpio_level(pin, 0);
    sleep_ms(50);
}

void beep_success(void) {
    play_tone(BUZZER_PIN, 800, 150);
    sleep_ms(100);
    play_tone(BUZZER_PIN, 800, 150);
    sleep_ms(100);
    play_tone(BUZZER_PIN, 800, 150);
}

void beep_failure(void) {
    play_tone(BUZZER_PIN, 400, 800);
    sleep_ms(100);
    play_tone(BUZZER_PIN, 400, 800);
    sleep_ms(100);
    play_tone(BUZZER_PIN, 400, 1600);
}

void sound_menu_change(void) {
    play_tone(BUZZER_PIN, 650, 80);
}

void sound_menu_confirm(void) {
    play_tone(BUZZER_PIN, 750, 150);
}

// ---------------------- Funções Auxiliares para OLED ----------------------
/**
 * Função auxiliar para dividir uma mensagem em duas linhas (até 16 caracteres cada).
 */
void split_message(const char *msg, char *line1, char *line2) {
    size_t len = strlen(msg);
    memset(line1, 0, 17);
    memset(line2, 0, 17);
    if (len <= 16) {
        strncpy(line1, msg, 16);
    } else {
        char *newline = strchr(msg, '\n');
        if (newline != NULL) {
            size_t len1 = newline - msg;
            if (len1 > 16)
                len1 = 16;
            strncpy(line1, msg, len1);
            strncpy(line2, newline + 1, 16);
        } else {
            strncpy(line1, msg, 16);
            strncpy(line2, msg + 16, 16);
        }
    }
}

void update_oled_status(int selected_action, const char *action_names[],
                        struct render_area *area, uint8_t *buffer) {
    char line[64];
    memset(buffer, 0, ssd1306_buffer_length);
    
    snprintf(line, sizeof(line), "Acao: %s", action_names[selected_action]);
    ssd1306_draw_string(buffer, 0, 0, line);
    
    ssd1306_draw_string(buffer, 0, 10, "");
    
    snprintf(line, sizeof(line), "Fome:%d Hig:%d", bob.fome, bob.higiene);
    ssd1306_draw_string(buffer, 0, 20, line);
    snprintf(line, sizeof(line), "Ener:%d Div:%d", bob.energia, bob.diversao);
    ssd1306_draw_string(buffer, 0, 30, line);
    
    ssd1306_draw_string(buffer, 0, 40, "----------------");
    
    uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    uint32_t elapsed = now_ms - last_decay_ms;
    uint32_t remaining_ms = (elapsed < DECAY_INTERVAL_MS) ? (DECAY_INTERVAL_MS - elapsed) : 0;
    uint32_t seconds_remaining = remaining_ms / 1000;
    snprintf(line, sizeof(line), "Prox: %lu s", seconds_remaining);
    ssd1306_draw_string(buffer, 0, 50, line);
    
    render_on_display(buffer, area);
}

void display_message(const char *msg, struct render_area *area, uint8_t *buffer) {
    char line1[17], line2[17];
    memset(buffer, 0, ssd1306_buffer_length);
    split_message(msg, line1, line2);
    ssd1306_draw_string(buffer, 0, 0, line1);
    if (strlen(line2) > 0)
        ssd1306_draw_string(buffer, 0, 10, line2);
    render_on_display(buffer, area);
    sleep_ms(3000);
}

void update_oled_no_delay(const char *msg, struct render_area *area, uint8_t *buffer) {
    char line1[17], line2[17];
    memset(buffer, 0, ssd1306_buffer_length);
    split_message(msg, line1, line2);
    ssd1306_draw_string(buffer, 0, 0, line1);
    if (strlen(line2) > 0)
        ssd1306_draw_string(buffer, 0, 10, line2);
    render_on_display(buffer, area);
}

// ---------------------- Funções do Jogo da Velha (Tic Tac Toe) ----------------------
#define GRID_COLOR_R      255
#define GRID_COLOR_G      255
#define GRID_COLOR_B      255

#define COLOR_OFF_R       0
#define COLOR_OFF_G       0
#define COLOR_OFF_B       0

#define COLOR_PLAYER1_R   50    
#define COLOR_PLAYER1_G   0
#define COLOR_PLAYER1_B   0

#define COLOR_PLAYER2_R   0    
#define COLOR_PLAYER2_G   0
#define COLOR_PLAYER2_B   50

#define COLOR_CURSOR_R    50    
#define COLOR_CURSOR_G    50
#define COLOR_CURSOR_B    0

int board[3][3] = { {0, 0, 0},
                    {0, 0, 0},
                    {0, 0, 0} };
int current_player = 1;
int cursor_row = 0;
int cursor_col = 0;

int led_index_game(int row, int col) {
    return row * 5 + col;
}

void draw_board() {
    for (int row = 0; row < 5; row++) {
        for (int col = 0; col < 5; col++) {
            int index = led_index_game(row, col);
            if ((row % 2 == 0) && (col % 2 == 0)) {
                int cell_row = row / 2;
                int cell_col = col / 2;
                if (cell_row == cursor_row && cell_col == cursor_col && current_player == 1)
                    npSetLED(index, COLOR_CURSOR_R, COLOR_CURSOR_G, COLOR_CURSOR_B);
                else if (board[cell_row][cell_col] == 1)
                    npSetLED(index, COLOR_PLAYER1_R, COLOR_PLAYER1_G, COLOR_PLAYER1_B);
                else if (board[cell_row][cell_col] == 2)
                    npSetLED(index, COLOR_PLAYER2_R, COLOR_PLAYER2_G, COLOR_PLAYER2_B);
                else
                    npSetLED(index, COLOR_OFF_R, COLOR_OFF_G, COLOR_OFF_B);
            } else {
                npSetLED(index, GRID_COLOR_R, GRID_COLOR_G, GRID_COLOR_B);
            }
        }
    }
    npWrite();
}

int check_winner() {
    // Verifica linhas e colunas
    for (int i = 0; i < 3; i++) {
        if (board[i][0] && board[i][0] == board[i][1] && board[i][1] == board[i][2])
            return board[i][0];
    }
    for (int i = 0; i < 3; i++) {
        if (board[0][i] && board[0][i] == board[1][i] && board[1][i] == board[2][i])
            return board[0][i];
    }
    // Verifica diagonais
    if (board[0][0] && board[0][0] == board[1][1] && board[1][1] == board[2][2])
        return board[0][0];
    if (board[0][2] && board[0][2] == board[1][1] && board[1][1] == board[2][0])
        return board[0][2];
    
    int filled = 1;
    for (int i = 0; i < 3; i++) 
        for (int j = 0; j < 3; j++)
            if (board[i][j] == 0)
                filled = 0;
    if (filled)
        return -1;
    
    return 0;
}

void flash_win(int player) {
    for (int i = 0; i < 6; i++) {
        for (int row = 0; row < 5; row++) {
            for (int col = 0; col < 5; col++) {
                int index = led_index_game(row, col);
                if ((row % 2 == 0) && (col % 2 == 0)) {
                    if (player == 1)
                        npSetLED(index, COLOR_PLAYER1_R, COLOR_PLAYER1_G, COLOR_PLAYER1_B);
                    else if (player == 2)
                        npSetLED(index, COLOR_PLAYER2_R, COLOR_PLAYER2_G, COLOR_PLAYER2_B);
                } else {
                    npSetLED(index, GRID_COLOR_R, GRID_COLOR_G, GRID_COLOR_B);
                }
            }
        }
        npWrite();
        sleep_ms(300);
        for (int row = 0; row < 5; row++) {
            for (int col = 0; col < 5; col++) {
                int index = led_index_game(row, col);
                if ((row % 2 == 0) && (col % 2 == 0))
                    npSetLED(index, COLOR_OFF_R, COLOR_OFF_G, COLOR_OFF_B);
                else
                    npSetLED(index, GRID_COLOR_R, GRID_COLOR_G, GRID_COLOR_B);
            }
        }
        npWrite();
        sleep_ms(300);
    }
}

void flash_draw() {
    for (int i = 0; i < 6; i++) {
        for (int row = 0; row < 5; row++) {
            for (int col = 0; col < 5; col++) {
                int index = led_index_game(row, col);
                if ((row % 2 == 0) && (col % 2 == 0))
                    npSetLED(index, 200, 200, 200);
                else
                    npSetLED(index, GRID_COLOR_R, GRID_COLOR_G, GRID_COLOR_B);
            }
        }
        npWrite();
        sleep_ms(300);
        for (int row = 0; row < 5; row++) {
            for (int col = 0; col < 5; col++) {
                int index = led_index_game(row, col);
                if ((row % 2 == 0) && (col % 2 == 0))
                    npSetLED(index, COLOR_OFF_R, COLOR_OFF_G, COLOR_OFF_B);
                else
                    npSetLED(index, GRID_COLOR_R, GRID_COLOR_G, GRID_COLOR_B);
            }
        }
        npWrite();
        sleep_ms(300);
    }
}

void reset_game() {
    memset(board, 0, sizeof(board));
    cursor_row = 0;
    cursor_col = 0;
    current_player = 1;
}

bool ticTacToe_game(struct render_area *area, uint8_t *buffer) {
    reset_game();
    bool move_registered = false;
    char direction[20];
    int winner = 0;
    
    display_message("Jogo da Velha!\nSua vez", area, buffer);
    
    while (winner == 0) {
        draw_board();
        if (current_player == 1) {
            adc_select_input(0);
            uint16_t adc_y = adc_read();
            adc_select_input(1);
            uint16_t adc_x = adc_read();
            
            if (adc_x < LOWER_THRESHOLD)
                strcpy(direction, "Direita");
            else if (adc_x > UPPER_THRESHOLD)
                strcpy(direction, "Esquerda");
            else if (adc_y < LOWER_THRESHOLD)
                strcpy(direction, "Cima");
            else if (adc_y > UPPER_THRESHOLD)
                strcpy(direction, "Baixo");
            else
                strcpy(direction, "Centro");
            
            if (strcmp(direction, "Centro") != 0 && !move_registered) {
                if (strcmp(direction, "Esquerda") == 0)
                    cursor_col = (cursor_col == 0) ? 2 : cursor_col - 1;
                else if (strcmp(direction, "Direita") == 0)
                    cursor_col = (cursor_col == 2) ? 0 : cursor_col + 1;
                else if (strcmp(direction, "Cima") == 0)
                    cursor_row = (cursor_row == 0) ? 2 : cursor_row - 1;
                else if (strcmp(direction, "Baixo") == 0)
                    cursor_row = (cursor_row == 2) ? 0 : cursor_row + 1;
                sound_menu_change();
                move_registered = true;
                sleep_ms(300);
            } else if (strcmp(direction, "Centro") == 0)
                move_registered = false;
            
            bool button_pressed = !gpio_get(BUTTON_PIN);
            if (button_pressed) {
                if (board[cursor_row][cursor_col] == 0) {
                    board[cursor_row][cursor_col] = 1;
                    winner = check_winner();
                    if (winner != 0)
                        break;
                    current_player = 2;
                    sleep_ms(300);
                }
            }
        } else {
            int empty_cells[9][2], count = 0;
            for (int i = 0; i < 3; i++)
                for (int j = 0; j < 3; j++)
                    if (board[i][j] == 0) {
                        empty_cells[count][0] = i;
                        empty_cells[count][1] = j;
                        count++;
                    }
            if (count > 0) {
                int r = rand() % count;
                board[empty_cells[r][0]][empty_cells[r][1]] = 2;
            }
            winner = check_winner();
            current_player = 1;
            sleep_ms(500);
        }
        sleep_ms(50);
    }
    draw_board();
    if (winner == 1) {
        flash_win(1);
        display_message("Voce venceu!", area, buffer);
        beep_success();
        return true;
    } else if (winner == 2) {
        flash_win(2);
        display_message("Bob venceu!", area, buffer);
        beep_failure();
        return false;
    } else {
        flash_draw();
        display_message("Empate!", area, buffer);
        return false;
    }
}

// ---------------------- Seletores de Menu ----------------------
/**
 * Menu para selecionar o tipo de alimento.
 */
int select_food_type(struct render_area *area, uint8_t *buffer) {
    const char *food_names[3] = {"Refeicao", "Petisco", "Energetico"};
    int selected = 0;
    bool move_registered = false;
    bool confirmed = false;
    
    while (!confirmed) {
        char msg[32];
        snprintf(msg, sizeof(msg), "Alimentar:\n%s", food_names[selected]);
        update_oled_no_delay(msg, area, buffer);
        
        adc_select_input(1);
        uint16_t adc_x = adc_read();
        if (adc_x < LOWER_THRESHOLD && !move_registered) {
            selected = (selected - 1 + 3) % 3;
            sound_menu_change();
            move_registered = true;
        } else if (adc_x > UPPER_THRESHOLD && !move_registered) {
            selected = (selected + 1) % 3;
            sound_menu_change();
            move_registered = true;
        } else if (adc_x >= LOWER_THRESHOLD && adc_x <= UPPER_THRESHOLD)
            move_registered = false;
        
        bool btn = !gpio_get(BUTTON_PIN);
        if (btn) {
            sound_menu_confirm();
            sleep_ms(200);
            confirmed = true;
        }
        sleep_ms(100);
    }
    return selected;
}

/**
 * Menu para selecionar a dificuldade.
 */
float select_difficulty(struct render_area *area, uint8_t *buffer) {
    const char *difficulty_names[3] = {"Facil", "Normal", "Dificil"};
    float multipliers[3] = {0.8f, 1.0f, 1.5f};
    int selected = 1; // Normal por default
    bool move_registered = false;
    bool confirmed = false;
    
    while (!confirmed) {
        char msg[32];
        snprintf(msg, sizeof(msg), "Dificuldade:\n%s", difficulty_names[selected]);
        update_oled_no_delay(msg, area, buffer);
        
        adc_select_input(1);
        uint16_t adc_x = adc_read();
        if (adc_x < LOWER_THRESHOLD && !move_registered) {
            selected = (selected - 1 + 3) % 3;
            sound_menu_change();
            move_registered = true;
        } else if (adc_x > UPPER_THRESHOLD && !move_registered) {
            selected = (selected + 1) % 3;
            sound_menu_change();
            move_registered = true;
        } else if (adc_x >= LOWER_THRESHOLD && adc_x <= UPPER_THRESHOLD)
            move_registered = false;
        
        bool btn = !gpio_get(BUTTON_PIN);
        if (btn) {
            sound_menu_confirm();
            sleep_ms(200);
            confirmed = true;
        }
        sleep_ms(100);
    }
    return multipliers[selected];
}

// ---------------------- Função de Decaimento ----------------------
struct repeating_timer decay_timer;
bool decay_timer_callback(struct repeating_timer *t) {
    critical_section_enter_blocking(&cs);
    int decay_val = (int)(5 * decay_multiplier);
    bob.fome    = (bob.fome    >= decay_val) ? bob.fome - decay_val : 0;
    bob.higiene = (bob.higiene >= decay_val) ? bob.higiene - decay_val : 0;
    bob.energia = (bob.energia >= decay_val) ? bob.energia - decay_val : 0;
    bob.diversao = (bob.diversao >= decay_val) ? bob.diversao - decay_val : 0;
    critical_section_exit(&cs);
    
    last_decay_ms = to_ms_since_boot(get_absolute_time());
    return true;
}

// ---------------------- Função Principal ----------------------
int main() {
    stdio_init_all();
    srand((unsigned) to_ms_since_boot(get_absolute_time()));
  
    last_decay_ms = to_ms_since_boot(get_absolute_time());
    npInit(LED_PIN);
  
    // Configuração dos LEDs externos
    gpio_init(RED_LED_PIN);
    gpio_set_dir(RED_LED_PIN, GPIO_OUT);
    gpio_put(RED_LED_PIN, 0);
    gpio_init(GREEN_LED_PIN);
    gpio_set_dir(GREEN_LED_PIN, GPIO_OUT);
    gpio_put(GREEN_LED_PIN, 0);
  
    // Inicializa ADC (joystick)
    adc_init();
    adc_gpio_init(26);
    adc_gpio_init(27);
  
    // Configura os botões
    gpio_init(BUTTON_PIN);
    gpio_set_dir(BUTTON_PIN, GPIO_IN);
    gpio_pull_up(BUTTON_PIN);
    gpio_init(ERASE_BUTTON_PIN);
    gpio_set_dir(ERASE_BUTTON_PIN, GPIO_IN);
    gpio_pull_up(ERASE_BUTTON_PIN);
  
    // Inicializa o buzzer via PWM
    pwm_init_buzzer(BUZZER_PIN);
  
    // Inicializa o display OLED via I2C (i2c1)
    i2c_init(i2c1, ssd1306_i2c_clock * 1000);
    gpio_set_function(I2C_SDA, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA);
    gpio_pull_up(I2C_SCL);
    ssd1306_init();
  
    // Configuração da área de renderização para o OLED
    struct render_area frame_area = {
        .start_column = 0,
        .end_column   = ssd1306_width - 1,
        .start_page   = 0,
        .end_page     = ssd1306_n_pages - 1
    };
    calculate_render_area_buffer_length(&frame_area);
    uint8_t oled_buffer[ssd1306_buffer_length];
    memset(oled_buffer, 0, ssd1306_buffer_length);
    render_on_display(oled_buffer, &frame_area);
  
    // Seletor de dificuldade
    decay_multiplier = select_difficulty(&frame_area, oled_buffer);
    {
        char diffMsg[32];
        if (decay_multiplier == 0.8f)
            snprintf(diffMsg, sizeof(diffMsg), "Dificuldade: Facil");
        else if (decay_multiplier == 1.0f)
            snprintf(diffMsg, sizeof(diffMsg), "Dificuldade: Normal");
        else if (decay_multiplier == 1.5f)
            snprintf(diffMsg, sizeof(diffMsg), "Dificuldade: Dificil");
        display_message(diffMsg, &frame_area, oled_buffer);
    }
  
    add_repeating_timer_ms(DECAY_INTERVAL_MS, decay_timer_callback, NULL, &decay_timer);
  
    const char *action_names[4] = {"Alimentar", "Banho", "Dormir", "Brincar"};
    const char *action_msgs[4]  = {"Bob alimentado!", "Bob tomou banho!",
                                   "Bob Dormiu bastante", "Bob brincou!"};
  
    int selected_action = 0;
    bool move_registered = false;
    bool button_registered = false;
  
    while (true) {
        adc_select_input(1);
        uint16_t adc_x = adc_read();
        if (adc_x < LOWER_THRESHOLD && !move_registered) {
            selected_action = (selected_action - 1 + 4) % 4;
            sound_menu_change();
            move_registered = true;
        } else if (adc_x > UPPER_THRESHOLD && !move_registered) {
            selected_action = (selected_action + 1) % 4;
            sound_menu_change();
            move_registered = true;
        } else if (adc_x >= LOWER_THRESHOLD && adc_x <= UPPER_THRESHOLD)
            move_registered = false;
      
        update_oled_status(selected_action, action_names, &frame_area, oled_buffer);
      
        // Atualiza a face do Bob conforme seus status
        const uint8_t (*face)[5] = select_face();
        draw_pattern(face);
      
        bool button_pressed = !gpio_get(BUTTON_PIN);
        if (button_pressed && !button_registered) {
            sound_menu_confirm();
            switch (selected_action) {
                case 0: {
                    int foodType = select_food_type(&frame_area, oled_buffer);
                    if (foodType == 0) {
                        bob.fome += 20;
                        if (bob.fome > 100) bob.fome = 100;
                        bob.energia += 5;
                        if (bob.energia > 100) bob.energia = 100;
                    } else if (foodType == 1) {
                        bob.fome += 10;
                        if (bob.fome > 100) bob.fome = 100;
                    } else if (foodType == 2) {
                        bob.fome += 5;
                        if (bob.fome > 100) bob.fome = 100;
                        bob.energia += 15;
                        if (bob.energia > 100) bob.energia = 100;
                    }
                    display_message(action_msgs[0], &frame_area, oled_buffer);
                    beep_success();
                    break;
                }
                case 1:
                    bob.higiene += 30;
                    if (bob.higiene > 100) bob.higiene = 100;
                    bob.diversao = (bob.diversao >= 5) ? bob.diversao - 5 : 0;
                    display_message(action_msgs[1], &frame_area, oled_buffer);
                    beep_success();
                    break;
                case 2:
                    bob.energia += 25;
                    if (bob.energia > 100) bob.energia = 100;
                    display_message(action_msgs[2], &frame_area, oled_buffer);
                    beep_success();
                    break;
                case 3: {
                    bool win = ticTacToe_game(&frame_area, oled_buffer);
                    if (win)
                        bob.diversao += 20;
                    else
                        bob.diversao += 10;
                    if (bob.diversao > 100) bob.diversao = 100;
                    bob.energia = (bob.energia >= 10) ? bob.energia - 10 : 0;
                    bob.fome = (bob.fome >= 5) ? bob.fome - 5 : 0;
                    display_message("Bob brincou!", &frame_area, oled_buffer);
                    beep_success();
                    break;
                }
                default:
                    break;
            }
            sleep_ms(300);
            button_registered = true;
        } else if (!button_pressed) {
            button_registered = false;
        }
      
        sleep_ms(50);
    }
  
    return 0;
}