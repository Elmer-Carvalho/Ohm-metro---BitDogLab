#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "pico/stdlib.h"
#include "hardware/adc.h"
#include "hardware/i2c.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "./lib/ssd1306.h"
#include "./lib/font.h"
#include "pio_matrix.pio.h"

// Comunicação Serial I2C
#define I2C_PORT i2c1
#define I2C_SDA_PIN 14
#define I2C_SCL_PIN 15

// Definições do Display
#define SSD_ADDR 0x3C
#define SSD_WIDTH 128
#define SSD_HEIGHT 64

// Matriz de LEDs
#define MATRIZ_LEDS_PIN 7
#define NUM_LEDS 25

// ADC
#define ADC_PIN 28
#define ADC_SELECTED 2
#define ADC_RESOLUTION 4095

// Variáveis globais
ssd1306_t ssd;
PIO pio = pio0;
uint sm;
const uint ohm_known = 10000; // Resistor conhecido de 10kΩ
const float input_volt = 3.3f;
float output_volt = 0.0f;

const char *color_names[] = {
    "Preto", "Marrom", "Vermelho", "Laranja", "Amarelo",
    "Verde", "Azul", "Violeta", "Cinza", "Branco"
};

// Valores da série E24 (510Ω a 100kΩ)
const float e24_values[] = {
    510, 560, 620, 680, 750, 820, 910,
    1000, 1100, 1200, 1300, 1500, 1600, 1800, 2000, 2200, 2400, 2700, 3000, 3300, 3600, 3900, 4300, 4700,
    5100, 5600, 6200, 6800, 7500, 8200, 9100,
    10000, 11000, 12000, 13000, 15000, 16000, 18000, 20000, 22000, 24000, 27000, 30000, 33000, 36000, 39000, 43000, 47000,
    51000, 56000, 62000, 68000, 75000, 82000, 91000,
    100000
};
const int e24_count = sizeof(e24_values) / sizeof(e24_values[0]);

// Protótipos
void setup();
void init_i2c_display();
void init_matrix_leds();
void clear_matrix();
void init_adc();
float calculate_resistance(float adc_value);
float find_closest_e24(float resistance);
void get_color_bands(float resistance, int *first, int *second, int *multiplier);
void display_resistance_and_colors(float resistance, int first, int second, int multiplier);

// Função principal
int main() {
    stdio_init_all();
    setup();

    while (true) {
        // Média de 100 leituras do ADC para maior precisão
        float volt = 0.0;
        for (uint i = 0; i < 100; i++) {
            volt += adc_read();
            sleep_ms(1);
        }
        volt = volt / 100.0f;
        volt = (volt * input_volt) / ADC_RESOLUTION;

        // Calcular resistência
        float resistance = calculate_resistance(volt);
        if (resistance < 510 || resistance > 100000) {
            // Resistência fora do intervalo; limpar display e mostrar erro
            ssd1306_fill(&ssd, false);
            ssd1306_draw_string(&ssd, "Fora de faixa", 0, 0);
            ssd1306_send_data(&ssd);
            printf("Resistência fora de faixa: %.0f Ω\n", resistance);
            sleep_ms(1000);
            continue;
        }

        // Encontrar valor E24 mais próximo
        float e24_resistance = find_closest_e24(resistance);

        // Determinar faixas de cores
        int first_band, second_band, multiplier;
        get_color_bands(e24_resistance, &first_band, &second_band, &multiplier);

        // Exibir no display
        display_resistance_and_colors(e24_resistance, first_band, second_band, multiplier);

        // Exibir na serial para debug
        printf("Resistência: %.0f Ω, Cores: %s, %s, %s\n",
               e24_resistance, color_names[first_band], color_names[second_band], color_names[multiplier]);

        sleep_ms(1000);
    }
}

// Configuração inicial
void setup() {
    init_matrix_leds();
    init_i2c_display();
    init_adc();
}

// Inicializar matriz de LEDs
void init_matrix_leds() {
    sm = pio_claim_unused_sm(pio, true);
    uint offset = pio_add_program(pio, &pio_matrix_program);
    pio_matrix_program_init(pio, sm, offset, MATRIZ_LEDS_PIN);
    pio_sm_set_enabled(pio, sm, true);
    clear_matrix(); // Limpar matriz no início
}

// Limpar matriz de LEDs
void clear_matrix() {
    for (uint i = 0; i < NUM_LEDS; i++) {
        pio_sm_put_blocking(pio, sm, 0);
    }
}

// Inicializar display I2C
void init_i2c_display() {
    i2c_init(I2C_PORT, 400000);
    gpio_set_function(I2C_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA_PIN);
    gpio_pull_up(I2C_SCL_PIN);
    ssd1306_init(&ssd, SSD_WIDTH, SSD_HEIGHT, false, SSD_ADDR, I2C_PORT);
    ssd1306_config(&ssd);
    ssd1306_fill(&ssd, false);
    ssd1306_send_data(&ssd);
}

// Inicializar ADC
void init_adc() {
    adc_init();
    adc_gpio_init(ADC_PIN);
    adc_select_input(ADC_SELECTED);
}

// Calcular resistência
float calculate_resistance(float adc_volt) {
    if (adc_volt >= input_volt || adc_volt <= 0) return 0; // Evitar divisão por zero
    return (ohm_known * adc_volt) / (input_volt - adc_volt);
}

// Encontrar valor E24 mais próximo
float find_closest_e24(float resistance) {
    float closest = e24_values[0];
    float min_diff = fabs(resistance - closest);

    for (int i = 1; i < e24_count; i++) {
        float diff = fabs(resistance - e24_values[i]);
        if (diff < min_diff) {
            min_diff = diff;
            closest = e24_values[i];
        }
    }
    return closest;
}

// Determinar faixas de cores
void get_color_bands(float resistance, int *first, int *second, int *multiplier) {
    int value = (int)resistance;
    int multiplier_count = 0;

    // Calcular multiplicador
    while (value >= 100) {
        value /= 10;
        multiplier_count++;
    }

    *first = value / 10;
    *second = value % 10;
    *multiplier = multiplier_count;

    // Ajustar para resistências < 1000Ω
    if (resistance < 1000) {
        value = (int)(resistance * 10);
        *first = value / 100;
        *second = (value / 10) % 10;
        *multiplier = multiplier_count - 1;
    }
}

// Exibir resistência e cores no display
void display_resistance_and_colors(float resistance, int first, int second, int multiplier) {
    uint count = 1;
    float aux = resistance;
    while (aux >= 10) {
        aux /= 10;
        count++;
    }

    char resistance_str[count];
    sprintf(resistance_str, "%1.0f", resistance); // Converte o inteiro em string

    ssd1306_fill(&ssd, false); // Limpar display
    ssd1306_rect(&ssd, 3, 3, 122, 60, true, false); // Desenha um retângulo
    ssd1306_draw_string(&ssd, "1 ", 8, 6); // Desenha uma string
    ssd1306_draw_string(&ssd, color_names[first], 60, 6);
    ssd1306_draw_string(&ssd, "2 ", 8, 16);
    ssd1306_draw_string(&ssd, color_names[second], 60, 16);
    ssd1306_draw_string(&ssd, "Multi.", 8, 26);
    ssd1306_draw_string(&ssd, color_names[multiplier], 60, 26);
    ssd1306_line(&ssd, 3, 40, SSD_WIDTH - 3, 40, true); // Desenha uma linha
    ssd1306_draw_string(&ssd, "RESIS.", 15, 47);
    ssd1306_draw_string(&ssd, resistance_str, 65, 47);

    ssd1306_send_data(&ssd);
}