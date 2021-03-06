// Arduino Due waveform generator using AD660
// Copyright 2013 University of California, San Francisco
// Author: Mark Tsuchida

//
// Introduction
//

// Most of the code below is written directly against Atmel and ARM's API, as
// opposed to the Arduino library. This is because the Arduino library, in
// wrapping and simplifying the vendor APIs, makes it impossible to allow the
// microcontroller to handle multiple tasks at the same time (in particular,
// most of the Arduino API I/O calls block, and the interrupt handling is very
// limited).
//
// This unfortunately means that this code is tied to a specific
// microcontroller (the SAM3X8E that comes on the Due), although care has been
// taken to keep it easy to adapt to related microcontrollers (especially other
// models in the SAM3X series).
//
// To read or modify this code, you will need to know (at least) about
// - C and Arduino programming,
// - The basics of microcontroller programming (registers, memory addressing,
//   I/O, etc.),
// - How interrupts work and how they are used for time-critical control,
// - How SPI works,
// - The AD660's SPI-compatible digital interface,
// - The details of the SAM3X8E as described on Atmel's SAM3X/A datasheet:
//   especially, but not limited to,
//   * the interrupt controller (Section 12.20),
//   * SPI (Chapter 33), and
//   * Timer Counter (TC; Chapter 37).
// - The ARM CMSIS API (a common interface to ARM Cortex microcontroller
//   peripherals), and
// - The Atmel SAM libraries shipped with the Arduino IDE.
//
// CMSIS documentation can be found in the
// hardware/arduino/sam/system/CMSIS/CMSIS/Documentation directory in the
// Arduino IDE distribution (inside Arduino.app/Contents/Resources/Java on
// OS X); details are best discovered from the headers in
// hardware/arduino/sam/system/CMSIS/Device/ATMEL/sam3xa/include.
//
// The Atmel SAM libraries, which provide utility macros and functions for the
// SAM peripherals, is in hardware/arduino/sam/system/libsam. The best
// documentation is the code itself (and the comments). The API of this "SAM
// Software Package" shipped with the Arduino IDE resembles but differs from
// that of the Atmel Software Framework (ASF).


//
// Design
//

// The goal here is to send sampled (i.e. precomputed and stored) waveforms to
// 2 (or more) SPI daisy-chained AD660s. The waveforms will be uploaded from
// the computer. We also want to be able to respond to commands from the
// computer to start, stop, or switch waveforms.
//
// In order to accurately generate the waveforms, we perform the SPI
// transmission to the AD660s in interrupt service routines. We use a periodic
// timer interrupt to kick off the transmission of the channel 0 sample, and we
// use an interrupt indicating the completion of SPI transmission to kick off
// the transmission of the channel 1 sample (and so on, if there are more than
// 2 channels, i.e. more than two DACs). The samples for all channels are sent
// in a single SPI transmission (chip select assertion), so that all DACs will
// load their samples at the same moment. This way, the ISRs (interrupt service
// routines) are restricted to doing minimal work, and the MCU is free to
// communicate with the computer over the serial port (and such communication
// code can be written in a blocking fashion using the Arduino library).
//
// (If we had many DAC channels, it would have made sense to use the DMA
// controller for SPI output.)
//
// To enable switching between waveforms, we introduce the concept of a
// waveform bank (see struct WaveformBank), which specifies the waveform for
// each channel (a per-channel starting offset is also included, so that the
// same waveform table can be shared among channels whose output is to differ
// only in phase). The current bank can be switched at any time, and the output
// will switch to the new bank starting at the next timer interrupt.
//
//
// Coding assumptions:
// - Timer (TC) interrupts and SPI TDRE interrupts do not coincide (this is
//   true as long as the sampling frequency is not too high).
//
//
// Limitations:
// - There is no protection against the heap and stack running into each other.
//   Currently, this can result in the waveforms becoming corrupted.
// - The Due only has 96 KB of RAM, which can quickly become limiting if more
//   than a handful of waveforms are required. If that becomes an issue, it
//   might make sense to read waveforms from an SD card via SPI. The SPI
//   interface of an SD (MMC) card can be clocked at 25 MHz, so it is probably
//   fast enough for may applications.


//
// Serial communication protocol
//

// Commands are terminated with LF
//
// ID - identify the device
// ID\n
// -> OK18569\n
//
// DM - set waveform length, number of banks, and number of tables
// DM512,8,4\n (error if generating waveform)
//
// FQ - set sampling frequency
// FQ1000\n (1 kHz)
//
// LW - load waveform into table
// LW0\n
// -> EX1024\n
// <- 1024 bytes (512 little-endian samples)
// -> OK\n
//
// AW - assign waveform
// AW2,0,1\n (use waveform in table 2 for bank 0, channel 1)
//
// PH - set phase offset
// PH0,127\n (channel 0 starts at sample 127)
//
// SB - switch bank
// SB2\n (switch to bank 2)
//
// RN - start generating waveform
// RN\n
//
// HL - halt waveform generation
// HL\n
//
// MV - set absolute DAC value on channel
// MV0,32767\n (error if generating waveform)
//
// WH - query DAC value on channel
// WH0\n (error if generating waveform)
//
//
// Replies are also termianted with LF
// Host should not send next command before receiving complete reply
//
// OK\n
// OK128\n (response to a query)
// EX512\n (expecting 512 bytes of binary data)
// ER12\n (error code 12)


//
// Error codes
//
// TODO Put these in a header that can be included from host code, too.
// TODO Rename with different prefix.
enum {
  ERR_OK,
  ERR_BAD_ALLOC,
  ERR_BAD_CMD,
  ERR_BAD_PARAMS,
  ERR_TOO_FEW_PARAMS,
  ERR_CMD_TOO_LONG,
  ERR_EXPECTED_UINT,
  ERR_FREQ_OUT_OF_RANGE,
  ERR_INVALID_BANK,
  ERR_INVALID_CHANNEL,
  ERR_INVALID_PHASE_OFFSET,
  ERR_INVALID_SAMPLE_VALUE,
  ERR_INVALID_TABLE,
  ERR_OVERFLOW,
  ERR_TIMEOUT,
  ERR_NO_WAVEFORM,
  ERR_BUSY,
};


//
// Hardware configuration
//

// Which TC to use.
// Note that what Atmel calls TC1, channel 0 is referred to as TC3 in CMSIS
#define ID_TC_SAMPLER ID_TC3
Tc *const TC_SAMPLER = TC1;
#define TC_SAMPLER_CHANNEL 0
const IRQn_Type TC_SAMPLER_IRQn = TC3_IRQn;
#define TC_Sampler_Handler TC3_Handler

// TC clock frequency
#define TC_SAMPLER_TIMER_CLOCK TC_CMR_TCCLKS_TIMER_CLOCK3
#define TC_SAMPLER_MCK_DIVIDER 32  // for TIMER_CLOCK3
#define TC_SAMPLER_TICK_FREQ (VARIANT_MCK / TC_SAMPLER_MCK_DIVIDER)
#define MIN_SAMPLE_FREQ (1 + TC_SAMPLER_TICK_FREQ / UINT16_MAX)
#define MAX_SAMPLE_FREQ TC_SAMPLER_TICK_FREQ

// Which SPI peripheral to use
#define ID_SPI_DAC ID_SPI0
Spi *const SPI_DAC = SPI0;
const IRQn_Type SPI_DAC_IRQn = SPI0_IRQn;
#define SPI_DAC_Handler SPI0_Handler

// SPI peripheral chip select (slave select) line
const uint32_t SPI_DAC_NPCS = 0; // This is PA28, which is Arduino board pin 10

// Number of daisy-chained DACs
const int NUM_CHANNELS = 2;


//
// Other constants
//

#define SERIAL_MAGIC_ID 18569
#define MAX_CMD_LEN 32


//
// Waveform data and state
//

// Static descriptions of the waveforms for each channel
struct WaveformBank {
  // Pointer to the start of the waveform table, for each channel (channels may
  // share the same table)
  uint16_t *channel_waveform[NUM_CHANNELS];

  // Starting offset (phase) into the table, for each channel
  // (The actual array is stored outside of this struct, since in many cases it
  // will be shared between many (if not all) banks.)
  size_t *channel_offset;
};

// We use a fixed length for waveform tables; the length may only be changed
// when waveform generation is paused. This makes it easy to switch between
// waveform banks at any time during the scanning, without large disruptions in
// the output (if the waveforms from the two banks are phase-matched).
size_t g_waveform_length = 0;

uint8_t g_n_banks = 0;
uint8_t g_n_tables = 0;

struct WaveformBank *g_banks = NULL;
uint16_t *g_tables = NULL;

inline uint16_t *waveform_table(uint8_t index) {
  return g_tables + (index * g_waveform_length);
}

// For now, we have one global setting for per-channel starting sample indices.
// We could conceivably extend this to allow multiple offset lists.
size_t g_channel_offset[NUM_CHANNELS];

// The current waveform bank. Flip this pointer to switch between banks. IRQ
// for the TC must be disabled while writing to this pointer (so use
// set_bank()). The TC ISR reads but does not modify this pointer. Other ISRs
// do not access this pointer.
struct WaveformBank *volatile g_current_bank;
inline void set_bank(struct WaveformBank *new_bank) {
  NVIC_DisableIRQ(TC_SAMPLER_IRQn);
  g_current_bank = new_bank;
  NVIC_EnableIRQ(TC_SAMPLER_IRQn);
}

// The bank used for the current iteration through the samples. This gets
// updated from g_current_bank before sending channel 0 (so that we do not mix
// channel samples from different banks). Accessed only from ISRs.
struct WaveformBank *g_current_sample_bank = NULL;

// The index of the current or next sample (0 through g_waveform_length - 1)
size_t g_sample_index = 0;

// The next channel whose sample should be sent over SPI. The value is in the
// range 0 to NUM_CHANNELS (inclusive); the value is NUM_CHANNELS while sending
// the sample value for the last channel and 0 while waiting for the next timer
// interrupt.
volatile uint8_t g_next_channel = 0;

bool g_tc_sampler_active = false;

// State for single-value setting (g_next_channel is shared with waveform
// generation).
volatile uint8_t g_sending_adhoc_sample = false;
uint16_t g_adhoc_sample[NUM_CHANNELS];

// The last set values for each DAC channel
volatile uint16_t g_last_sample[NUM_CHANNELS];
inline uint16_t get_last_sample(size_t channel) {
  __disable_irq();
  uint16_t s = g_last_sample[channel];
  __enable_irq();
  return s;
}


//
// Memory management
//

// To avoid heap fragmentation, we allocate in one step the memory for waveform
// tables and banks.
bool reset_waveform_data(size_t waveform_length, uint8_t n_banks, uint8_t n_tables) {
  if (g_tables) {
    free(g_tables);
  }
  if (g_banks) {
    free(g_banks);
  }
  g_current_bank = 0;
  g_banks = NULL;
  g_tables = NULL;
  g_waveform_length = 0;
  g_n_banks = 0;
  g_n_tables = 0;

  if (!waveform_length) {
    return true;
  }

  // Put the banks in lower memory, where there is lower risk of corruption due
  // to clashing with the stack
  if (n_banks) {
    g_banks = (struct WaveformBank *)calloc(n_banks, sizeof(struct WaveformBank));
    if (!g_banks) {
      return false;
    }
  }
  if (n_tables) {
    g_tables = (uint16_t *)calloc(n_tables * g_waveform_length, sizeof(uint16_t));
    if (!g_tables) {
      free(g_banks);
      return false;
    }
  }

  g_current_bank = g_banks;

  g_waveform_length = waveform_length;
  g_n_banks = n_banks;
  g_n_tables = n_tables;

  // Set default values for banks.
  for (size_t i = 0; i < n_banks; i++) {
    if (n_tables > 0) {
      for (size_t ch = 0; ch < NUM_CHANNELS; ch++) {
        g_banks[i].channel_waveform[ch] = waveform_table(0);
      }
    }
    g_banks[i].channel_offset = g_channel_offset;
  }

  return true;
}


//
// Peripheral setup
//

void setup_spi_dac() {
  // Assign the relevant pins to the SPI controller
  uint32_t spi0_pins =
      PIO_ABSR_P25 |  // SPI0_MISO
      PIO_ABSR_P26 |  // SPI0_MOSI
      PIO_ABSR_P27 |  // SPI0_SPCK
      PIO_ABSR_P28 |  // SPI0_NPCS0
      PIO_ABSR_P29 |  // SPI0_NPCS1
      PIO_ABSR_P30 |  // SPI0_NPCS2
      PIO_ABSR_P31;   // SPI0_NPCS3
  PIO_Configure(PIOA, PIO_PERIPH_A, spi0_pins, PIO_DEFAULT);

  SPI_Configure(SPI_DAC, ID_SPI_DAC, SPI_MR_MSTR | SPI_MR_PS);
  SPI_Enable(SPI_DAC);
  
  SPI_ConfigureNPCS(SPI_DAC, SPI_DAC_NPCS,
      SPI_CSR_CPOL | (SPI_CSR_NCPHA & 0) |  // SPI Mode 3
      SPI_DLYBCT(0, VARIANT_MCK) | SPI_DLYBS(60, VARIANT_MCK) |
      SPI_SCBR(8400000, VARIANT_MCK) | SPI_CSR_CSAAT | SPI_CSR_BITS_16_BIT);

  SPI_DAC->SPI_IDR = ~SPI_IDR_TDRE;
  SPI_DAC->SPI_IER = SPI_IER_TDRE;
  
  // We do not enable the IRQ line itself; we only want TDRE interrupts when
  // we are actully sending someting words.
}


void setup_tc_sampler() {
  // Turn on power for the TC
  pmc_enable_periph_clk(ID_TC_SAMPLER);

  // Set the TC to count up to the value of RC (TC register C), and generate RC
  // compare interrupts.

  TC_Configure(TC_SAMPLER, TC_SAMPLER_CHANNEL,
      TC_CMR_WAVE | TC_CMR_WAVSEL_UP_RC | TC_SAMPLER_TIMER_CLOCK);

  TC_SAMPLER->TC_CHANNEL[TC_SAMPLER_CHANNEL].TC_IDR = ~TC_IDR_CPCS;
  TC_SAMPLER->TC_CHANNEL[TC_SAMPLER_CHANNEL].TC_IER = TC_IER_CPCS;

  // Default to the lowest possible sample rate
  TC_SetRC(TC_SAMPLER, TC_SAMPLER_CHANNEL, 0xFFFF);
}


void start_tc_sampler() {
  g_tc_sampler_active = true;
  NVIC_ClearPendingIRQ(TC_SAMPLER_IRQn);
  TC_Start(TC_SAMPLER, TC_SAMPLER_CHANNEL);
  NVIC_EnableIRQ(TC_SAMPLER_IRQn);
}


void stop_tc_sampler() {
  NVIC_DisableIRQ(TC_SAMPLER_IRQn);
  TC_Stop(TC_SAMPLER, TC_SAMPLER_CHANNEL);
  g_tc_sampler_active = false;
}


void set_tc_sampler_freq(uint32_t freq) {
  uint32_t tick_count = TC_SAMPLER_TICK_FREQ / freq;

  bool tc_active = g_tc_sampler_active;
  if (tc_active) {
    stop_tc_sampler();
  }
  TC_SetRC(TC_SAMPLER, TC_SAMPLER_CHANNEL, tick_count);
  if (tc_active) {
    start_tc_sampler();
  }
}


// Send the sample for the next channel, and increment the next-channel index.
// May be called from ISRs
void send_channel_sample() {
  uint16_t sample;
  if (g_sending_adhoc_sample) {
    sample = g_adhoc_sample[g_next_channel];
  }
  else {
    uint16_t *waveform = g_current_sample_bank->channel_waveform[g_next_channel];
    size_t offset = g_current_sample_bank->channel_offset[g_next_channel];

    size_t index = offset + g_sample_index;
    if (index >= g_waveform_length) {
      index -= g_waveform_length;
    }

    sample = waveform[index];
  }

  g_last_sample[g_next_channel] = sample;

  uint32_t td = sample | SPI_PCS(SPI_DAC_NPCS);
  if (++g_next_channel == NUM_CHANNELS) {
    td |= SPI_TDR_LASTXFER;
  }

  SPI_DAC->SPI_TDR = td;
  NVIC_EnableIRQ(SPI_DAC_IRQn);
}


//
// Interrupt service routines for SPI transmission
//

// On the timer interrupt, we determine the current waveform bank and start
// sending the sample for channel 0.
void TC_Sampler_Handler() {
  // Access the TC to deassert the IRQ
  TC_GetStatus(TC_SAMPLER, TC_SAMPLER_CHANNEL);

  g_current_sample_bank = g_current_bank;
  g_next_channel = 0;

  send_channel_sample();
}


// Since we have enabled the SPI IRQ for TDRE (transmit data register empty),
// this ISR gets called every time we finish sending a word (sample).
// If we have not finished, we send the sample for the next channel. If we have
// finished with all channels, we reset the channel counter and increment the
// sample index.
void SPI_DAC_Handler() {
  NVIC_DisableIRQ(SPI_DAC_IRQn);
  
  if (g_next_channel == 0) {
    // We shouldn't be here for the 0th channel: this is spurious.
    // XXX Do we need this check? It should be a programming error.
    return;
  }
  
  if (g_next_channel >= NUM_CHANNELS) {
    g_next_channel = 0;

    if (g_sending_adhoc_sample) {
      g_sending_adhoc_sample = false;
    }
    else {
      g_sample_index++;
      if (g_sample_index >= g_waveform_length) {
        g_sample_index = 0;
      }
    }

    return;
  }

  send_channel_sample();
}


//
// Non-ISR routines for SPI transmission
//

// Start sending a single, ad hoc sample.
void send_adhoc_sample(uint16_t *channel_samples) {
  for (size_t i = 0; i < NUM_CHANNELS; i++) {
    g_adhoc_sample[i] = channel_samples[i];
  }
  g_sending_adhoc_sample = true;

  g_next_channel = 0;
  send_channel_sample();
}


// Cannot be called from ISRs (obviously)
void wait_for_spi_dac() {
  while (g_next_channel != 0) {
    // Wait until we finish sending the last channel's sample
  }
}


//
// Serial command handling
//

void respond(const char *str, uint32_t num) {
  Serial.print(str);
  Serial.print(num);
  Serial.write('\n');
}


void respond(const char *str) {
  Serial.print(str);
  Serial.write('\n');
}


inline void respond_error(uint32_t code) {
  respond("ER", code);
}


inline void respond_data_request(size_t bytes) {
  respond("EX", bytes);
}


inline void respond_ok(uint32_t response) {
  respond("OK", response);
}


inline void respond_ok() {
  respond("OK");
}


int parse_uint(const char *buffer, size_t *p, uint32_t *result) {
  *result = 0;
  if (!isdigit(buffer[*p])) {
    return ERR_EXPECTED_UINT;
  }

  do {
    if (*result >= UINT32_MAX / 10) {
      return ERR_OVERFLOW;
    }
    *result *= 10;
    *result += buffer[(*p)++] - '0';
  } while (isdigit(buffer[*p]));
  return ERR_OK;
}


int parse_uint_params(const char *buffer, uint32_t *params, size_t num_params) {
  size_t p = 0;
  for (int i = 0; i < num_params; i++) {
    if (int err = parse_uint(buffer, &p, &params[i])) {
      return err;
    }

    if (i == num_params - 1) {
      break;
    }

    if (buffer[p++] != ',') {
      return ERR_TOO_FEW_PARAMS;
    }
  }

  if (buffer[p] != '\0') {
    return ERR_BAD_PARAMS;
  }
  return ERR_OK;
}


// Command handlers in alphabetical order


void handle_AW(const char *param_str) {
  uint32_t params[3];
  if (int err = parse_uint_params(param_str, params, 3)) {
    respond_error(err);
    return;
  }

  if (params[0] >= g_n_tables) {
    respond_error(ERR_INVALID_TABLE);
    return;
  }

  if (params[1] >= g_n_banks) {
    respond_error(ERR_INVALID_BANK);
    return;
  }

  if (params[2] >= NUM_CHANNELS) {
    respond_error(ERR_INVALID_CHANNEL);
    return;
  }

  if (g_tc_sampler_active) {
    respond_error(ERR_BUSY);
    return;
  }

  g_banks[params[1]].channel_waveform[params[2]] = waveform_table(params[0]);
  respond_ok();
}


void handle_DM(const char *param_str) {
  uint32_t params[3];
  if (int err = parse_uint_params(param_str, params, 3)) {
    respond_error(err);
    return;
  }

  size_t waveform_length = params[0];
  if (params[1] > UINT8_MAX) {
    respond_error(ERR_OVERFLOW);
    return;
  }
  uint8_t n_banks = params[1];
  if (params[2] > UINT8_MAX) {
    respond_error(ERR_OVERFLOW);
    return;
  }
  uint8_t n_tables = params[2];

  if (g_tc_sampler_active) {
    respond_error(ERR_BUSY);
    return;
  }

  if (reset_waveform_data(waveform_length, n_banks, n_tables)) {
    respond_ok();
  }
  else {
    respond_error(ERR_BAD_ALLOC);
  }
}


void handle_FQ(const char *param_str) {
  uint32_t params[1];
  if (int err = parse_uint_params(param_str, params, 1)) {
    respond_error(err);
    return;
  }

  if (params[0] < MIN_SAMPLE_FREQ || params[0] > MAX_SAMPLE_FREQ) {
    respond_error(ERR_FREQ_OUT_OF_RANGE);
    return;
  }

  set_tc_sampler_freq(params[0]);
  respond_ok();
}


void handle_HL(const char *param_str) {
  if (param_str[0] != '\0') {
    respond_error(ERR_BAD_CMD);
    return;
  }

  stop_tc_sampler();
  wait_for_spi_dac();
  respond_ok();
}


void handle_ID(const char *param_str) {
  if (param_str[0] != '\0') {
    respond_error(ERR_BAD_CMD);
  }
  respond_ok(SERIAL_MAGIC_ID);
}


void handle_LW(const char *param_str) {
  uint32_t params[1];
  if (int err = parse_uint_params(param_str, params, 1)) {
    respond_error(err);
    return;
  }

  if (params[0] >= g_n_tables) {
    respond_error(ERR_INVALID_TABLE);
    return;
  }

  if (g_tc_sampler_active) {
    respond_error(ERR_BUSY);
    return;
  }

  uint16_t *table = waveform_table(params[0]);
  size_t length = sizeof(uint16_t) * g_waveform_length;

  respond_data_request(length);
  size_t bytes = Serial.readBytes((char *)table, length);
  if (bytes < length) {
    respond_error(ERR_TIMEOUT);
    return;
  }
  respond_ok();
}


void handle_MV(const char *param_str) {
  uint32_t params[2];
  if (int err = parse_uint_params(param_str, params, 2)) {
    respond_error(err);
    return;
  }

  if (params[0] >= NUM_CHANNELS) {
    respond_error(ERR_INVALID_CHANNEL);
    return;
  }

  if (params[1] > UINT16_MAX) {
    respond_error(ERR_INVALID_SAMPLE_VALUE);
    return;
  }

  if (g_tc_sampler_active) {
    respond_error(ERR_BUSY);
    return;
  }

  uint16_t sample[NUM_CHANNELS];
  for (size_t i = 0; i < NUM_CHANNELS; i++) {
    sample[i] = get_last_sample(i);
  }
  sample[params[0]] = params[1];
  send_adhoc_sample(sample);
  wait_for_spi_dac();
  respond_ok();
  return;
}


void handle_PH(const char *param_str) {
  uint32_t params[2];
  if (int err = parse_uint_params(param_str, params, 2)) {
    respond_error(err);
    return;
  }

  if (params[0] >= NUM_CHANNELS) {
    respond_error(ERR_INVALID_CHANNEL);
    return;
  }

  if (params[1] >= g_waveform_length) {
    respond_error(ERR_INVALID_PHASE_OFFSET);
    return;
  }

  if (g_tc_sampler_active) {
    respond_error(ERR_BUSY);
    return;
  }

  g_channel_offset[params[0]] = params[1];
  respond_ok();
}


void handle_RN(const char *param_str) {
  if (param_str[0] != '\0') {
    respond_error(ERR_BAD_CMD);
    return;
  }

  if (g_n_banks == 0 || g_n_tables == 0) {
    respond_error(ERR_NO_WAVEFORM);
    return;
  }

  start_tc_sampler();
  respond_ok();
}


void handle_SB(const char *param_str) {
  uint32_t params[1];
  if (int err = parse_uint_params(param_str, params, 1)) {
    respond_error(err);
    return;
  }

  if (params[0] >= g_n_banks) {
    respond_error(ERR_INVALID_BANK);
    return;
  }

  struct WaveformBank *bank = &g_banks[params[0]];
  set_bank(bank);
  respond_ok();
}


void handle_WH(const char *param_str) {
  uint32_t params[1];
  if (int err = parse_uint_params(param_str, params, 1)) {
    respond_error(err);
    return;
  }

  if (params[0] >= NUM_CHANNELS) {
    respond_error(ERR_INVALID_CHANNEL);
    return;
  }

  if (g_tc_sampler_active) {
    respond_error(ERR_BUSY);
    return;
  }

  respond_ok(get_last_sample(params[0]));
}


void dispatch_cmd(char first, char second, const char *param_str) {
  if (first == 'A') {
    if (second == 'W') { handle_AW(param_str); return; }
  }
  else if (first == 'D') {
    if (second == 'M') { handle_DM(param_str); return; }
  }
  else if (first == 'F') {
    if (second == 'Q') { handle_FQ(param_str); return; }
  }
  else if (first == 'H') {
    if (second == 'L') { handle_HL(param_str); return; }
  }
  else if (first == 'I') {
    if (second == 'D') { handle_ID(param_str); return; }
  }
  else if (first == 'L') {
    if (second == 'W') { handle_LW(param_str); return; }
  }
  else if (first == 'M') {
    if (second == 'V') { handle_MV(param_str); return; }
  }
  else if (first == 'P') {
    if (second == 'H') { handle_PH(param_str); return; }
  }
  else if (first == 'R') {
    if (second == 'N') { handle_RN(param_str); return; }
  }
  else if (first == 'S') {
    if (second == 'B') { handle_SB(param_str); return; }
  }
  else if (first == 'W') {
    if (second == 'H') { handle_WH(param_str); return; }
  }
  respond_error(ERR_BAD_CMD);
}


void handle_cmd() {
  char cmd_buffer[MAX_CMD_LEN];
  size_t cmd_len = Serial.readBytesUntil('\n', cmd_buffer, MAX_CMD_LEN);

  if (cmd_len == 0) {
    // Empty command - ignore
    return;
  }

  if (cmd_len == MAX_CMD_LEN) {
    while (Serial.readBytesUntil('\n', cmd_buffer, MAX_CMD_LEN) > 0) {
      // Discard the rest of the command
    }
    respond_error(ERR_CMD_TOO_LONG);
    return;
  }

  if (cmd_len < 2) {
    respond_error(ERR_BAD_CMD);
    return;
  }
  
  // Append a null terminator to the command string
  cmd_buffer[cmd_len] = '\0';

  dispatch_cmd(cmd_buffer[0], cmd_buffer[1], cmd_buffer + 2);
}


//
// Arduino main program
//

void setup() {
  Serial.begin(115200);
  Serial.setTimeout(3000);

  setup_spi_dac();
  setup_tc_sampler();

  // Set DAC values to default (midpoint) and bring g_last_sample into sync
  uint16_t sample[NUM_CHANNELS];
  for (size_t i = 0; i < NUM_CHANNELS; i++) {
    sample[i] = UINT16_MAX / 2;
  }
  send_adhoc_sample(sample);
  wait_for_spi_dac();
}

void loop() {
  // Kind of silly to have to poll, but otherwise we'd need to forgo the
  // Arduino serial library and things will get much more complicated.
  if (Serial.available()) {
    handle_cmd();
  }
}
