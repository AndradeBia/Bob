# Bob - Tamagotchi com Raspberry Pi Pico

Um projeto de Tamagotchi virtual implementado em um Raspberry Pi Pico, usando uma matriz de LEDs WS2812B 5x5, display OLED, joystick analógico e um botão. O Bob é um pet virtual que precisa de cuidados constantes e oferece um minigame de Jogo da Velha para diversão.

## Características

### Hardware Necessário
- Raspberry Pi Pico
- Matriz de LEDs WS2812B 5x5
- Display OLED SSD1306
- Joystick analógico
- 1 botão
- LEDs externos (vermelho e verde)
- Buzzer
- Resistores e componentes básicos para conexão

### Funcionalidades
- **Sistema de Status**: O Bob possui 4 atributos principais
  - Fome
  - Higiene
  - Energia
  - Diversão

- **Ações Disponíveis**:
  1. **Alimentar**
     - Refeição: +20 Fome, +5 Energia
     - Petisco: +10 Fome
     - Energético: +5 Fome, +15 Energia

  2. **Banho**
     - +30 Higiene
     - -5 Diversão

  3. **Dormir**
     - +25 Energia

  4. **Brincar**
     - Minigame: Jogo da Velha contra o Bob
     - Vitória: +20 Diversão
     - Derrota/Empate: +10 Diversão
     - Custo: -10 Energia, -5 Fome

### Expressões do Bob
O Bob exibe diferentes expressões na matriz de LEDs baseadas em seus status:
- **Feliz**: Maioria dos atributos acima de 50%
- **Neutro**: Atributos medianos
- **Triste**: Qualquer atributo abaixo de 30%

### Sistema de Dificuldade
- **Fácil**: Decaimento 0.8x
- **Normal**: Decaimento 1.0x
- **Difícil**: Decaimento 1.5x

## Pinagem

```
GPIO  7: Matriz de LEDs WS2812B (Din)
GPIO 14: I2C SDA (Display OLED)
GPIO 15: I2C SCL (Display OLED)
GPIO 26: ADC0 (Joystick Y)
GPIO 27: ADC1 (Joystick X)
GPIO  6: Botão Principal
GPIO 13: LED Vermelho
GPIO 11: LED Verde
GPIO 21: Buzzer
```

## Como Jogar

1. Ao iniciar, selecione o nível de dificuldade usando o joystick e confirme com o botão
2. Use o joystick para navegar entre as ações disponíveis
3. Pressione o botão principal para executar a ação selecionada
4. No Jogo da Velha:
   - Use o joystick para mover o cursor
   - Pressione o botão para fazer sua jogada
   - Vença o Bob para ganhar mais pontos de diversão

## Mecânicas de Decaimento

- Todos os atributos decaem a cada 60 segundos
- A taxa de decaimento é influenciada pelo nível de dificuldade
- O display OLED mostra um contador para o próximo decaimento

## Feedback Sonoro e Visual

- Sons diferentes para:
  - Navegação no menu
  - Confirmação de ações
  - Sucesso nas ações
  - Falha nas ações

- Display OLED mostra:
  - Status atual do Bob
  - Ação selecionada
  - Mensagens de feedback
  - Tempo até próximo decaimento

## Dependências

- pico-sdk
- Bibliotecas:
  - hardware/adc
  - hardware/pwm
  - hardware/pio
  - hardware/dma
  - hardware/i2c



