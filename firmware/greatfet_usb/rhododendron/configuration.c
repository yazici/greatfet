/*
 * This file is part of GreatFET
 *
 * Code for ULPI register interfacing via SGPIO for Rhododendron boards.
 */


#include <debug.h>
#include <errno.h>
#include <string.h>

#include <drivers/gpio.h>
#include <drivers/sgpio.h>
#include <drivers/platform_clock.h>
#include <drivers/scu.h>
#include <drivers/timer.h>
#include <toolchain.h>

// XXX: temporary
#include <libopencm3/lpc43xx/adc.h>

#include "../pin_manager.h"
#include "../rhododendron.h"

//#include <drivers/sct.h>

static gpio_pin_t gpio_leds[] = {
	[LED_VBUS]      = { .port = 2, .pin = 14 },
	[LED_TRIGGERED] = { .port = 2, .pin = 13 },
	[LED_STATUS]    = { .port = 2, .pin = 9  }
};



// Small buffers used for ULPI register exchanges.
static uint8_t  register_access_buffer[8];
static uint32_t register_direction_buffer;
static uint32_t register_stop_buffer;
static uint32_t register_nxt_buffer;


/**
 * Import our ULPI pins from the core capture code.
 */
static sgpio_pin_configuration_t ulpi_data_pins[] = {
	{ .sgpio_pin = 0,  .scu_group = 0, .scu_pin =  0, .pull_resistors = SCU_PULLDOWN},
	{ .sgpio_pin = 1,  .scu_group = 0, .scu_pin =  1, .pull_resistors = SCU_PULLDOWN},
	{ .sgpio_pin = 2,  .scu_group = 1, .scu_pin = 15, .pull_resistors = SCU_PULLDOWN},
	{ .sgpio_pin = 3,  .scu_group = 1, .scu_pin = 16, .pull_resistors = SCU_PULLDOWN},
	{ .sgpio_pin = 4,  .scu_group = 6, .scu_pin =  3, .pull_resistors = SCU_PULLDOWN},
	{ .sgpio_pin = 5,  .scu_group = 6, .scu_pin =  6, .pull_resistors = SCU_PULLDOWN},
	{ .sgpio_pin = 6,  .scu_group = 2, .scu_pin =  2, .pull_resistors = SCU_PULLDOWN},
	{ .sgpio_pin = 7,  .scu_group = 6, .scu_pin =  8, .pull_resistors = SCU_PULLDOWN},
};


static sgpio_pin_configuration_t ulpi_clk_pin =
	{ .sgpio_pin = 8,  .scu_group = 9, .scu_pin =  6,  .pull_resistors = SCU_NO_PULL};
static sgpio_pin_configuration_t ulpi_stp_pin =
	{ .sgpio_pin = 9,  .scu_group = 1, .scu_pin =  13, .pull_resistors = SCU_PULLDOWN};
static sgpio_pin_configuration_t ulpi_nxt_pin =
	{ .sgpio_pin = 10, .scu_group = 1, .scu_pin =  14, .pull_resistors = SCU_NO_PULL};



/*
// XXX: Temporary test of external reference-clock behavior.
static const sgpio_clock_source_t ulpi_clock_source = SGPIO_CLOCK_SOURCE_SGPIO11;
static sgpio_pin_configuration_t ulpi_clk_pin =
	{ .sgpio_pin = 11,  .scu_group = 1, .scu_pin = 4,  .pull_resistors = SCU_NO_PULL};
*/

// SGPIO pin definitions...

// ... and GPIO pin definitions.
static gpio_pin_t ulpi_phy_reset = { .port = 5, .pin = 0  };
static gpio_pin_t ulpi_dir_gpio  = { .port = 0, .pin = 12 };
static gpio_pin_t ulpi_stp_gpio  = { .port = 1, .pin = 6  };
static gpio_pin_t ulpi_clk_gpio  = { .port = 4, .pin = 11 };

/**
 * Functions used for ULPI register access.
 */
static sgpio_function_t ulpi_register_functions[] = {

	// Function 0: this function handles shifting out the actual register
	// write command on the ULPI data lines. It's bidirectional, as we need
	// to control the input and output timings with precision.
	{
		.enabled                      = true,

		// For now, we're only going to use register writes, not reads,
		// so we only need output mode. This will change in the future.
		// TODO: fix this, and switch us to bidirectional mode?
		.mode                         = SGPIO_MODE_STREAM_BIDIRECTIONAL,

		// Bind each of the lower eight pins to their proper places,
		// and by deafault sample the eight of them.
		.pin_configurations           = ulpi_data_pins,
		.bus_width                    = ARRAY_SIZE(ulpi_data_pins),


#ifdef RHODODENDRON_USE_USB1_CLK_AS_ULPI_CLOCK

		// We'll shift in time with rising edges of the PHY clock.
		.shift_clock_source          = SGPIO_CLOCK_SOURCE_COUNTER,
		.shift_clock_edge            = SGPIO_CLOCK_EDGE_RISING,
		.shift_clock_frequency       = 0, // Never divide; just use the SGPIO clock frequency.


#else

		// We'll shift in time with rising edges of the PHY clock.
		.shift_clock_source          = SGPIO_CLOCK_SOURCE_SGPIO08,
		.shift_clock_edge            = SGPIO_CLOCK_EDGE_FALLING,
		.shift_clock_input           = &ulpi_clk_pin,

#endif


		// Always shift. Ideally, this data would be qualified on the NXT
		// signal at appropriate times -- or by a signal generated by e.g. a
		// SCT FSM, but for now we take advantage of the fact that the PHY's
		// register-write timing is deterministic, and thus just reproduce
		// the bits at the right times.
		.shift_clock_qualifier        = SGPIO_ALWAYS_SHIFT_ON_SHIFT_CLOCK,


		// Use our eight-byte buffers for our shift-out registers.
		.buffer                       = register_access_buffer,
		.buffer_order                 = 3,
		.direction_buffer             = &register_direction_buffer,
		.direction_buffer_order       = 2,

		// For ULPI register writes, we want to stop after 8 shifts.
		.shift_count_limit            = 8
	},

	// Function 1: this function drives the STP line, which is used
	// to indicate that we've finished writing data at the end of a
	// register write. This implementation just shifts at the same time
	// as the data lines; we pre-program it to set STP at the right time.
	{
		.enabled                      = true,

		// The STP line is always driven by us (the 'link'), so we can
		// leave it as an output. We're scanning out short bursts, so
		// we'll use the "fixed" data mode.
		.mode                         = SGPIO_MODE_FIXED_DATA_OUT,

		// We only have the one STP pin to handle.
		.pin_configurations           = &ulpi_stp_pin,
		.bus_width                    = 1,


#ifdef RHODODENDRON_USE_USB1_CLK_AS_ULPI_CLOCK

		// We'll shift in time with rising edges of the PHY clock.
		.shift_clock_source          = SGPIO_CLOCK_SOURCE_COUNTER,
		.shift_clock_edge            = SGPIO_CLOCK_EDGE_RISING,
		.shift_clock_frequency       = 0, // Never divide; just use the SGPIO clock frequency.


#else

		// We'll shift in time with rising edges of the PHY clock.
		.shift_clock_source          = SGPIO_CLOCK_SOURCE_SGPIO08,
		.shift_clock_edge            = SGPIO_CLOCK_EDGE_RISING,
		.shift_clock_input           = &ulpi_clk_pin,

#endif

		// See the note on function 0.
		.shift_clock_qualifier        = SGPIO_ALWAYS_SHIFT_ON_SHIFT_CLOCK,

		.buffer                       = &register_stop_buffer,
		.buffer_order                 = 2,

		// For ULPI register writes, we want to stop after 8 shifts.
		.shift_count_limit            = 8,
	},
	// Function 2:
	// This captures the values of NXT during the shift; which we can use to
	// validate that our write happened correctly.
	{
		.enabled                      = true,

		// Scan in input from the NXT line.
		.mode                         = SGPIO_MODE_STREAM_DATA_IN,
		.pin_configurations           = &ulpi_nxt_pin,
		.bus_width                    = 1,

#ifdef RHODODENDRON_USE_USB1_CLK_AS_ULPI_CLOCK

		// We'll shift in time with rising edges of the PHY clock.
		.shift_clock_source          = SGPIO_CLOCK_SOURCE_COUNTER,
		.shift_clock_edge            = SGPIO_CLOCK_EDGE_RISING,
		.shift_clock_frequency       = 0, // Never divide; just use the SGPIO clock frequency.


#else

		// We'll shift in time with rising edges of the PHY clock.
		.shift_clock_source          = SGPIO_CLOCK_SOURCE_SGPIO08,
		.shift_clock_edge            = SGPIO_CLOCK_EDGE_FALLING,
		.shift_clock_input           = &ulpi_clk_pin,

#endif


		// We'll always shift, even if this isn't ideal.
		// See the note on function 0.
		.shift_clock_qualifier        = SGPIO_ALWAYS_SHIFT_ON_SHIFT_CLOCK,

		.buffer                       = &register_nxt_buffer,
		.buffer_order                 = 2,

		// For ULPI register writes, we want to stop after 8 shifts.
		.shift_count_limit            = 8,
	},
};

/**
 * Logic analyzer configuration using SGPIO.
 */
static sgpio_t ulpi_register_mode  = {
	.functions      = ulpi_register_functions,
	.function_count = ARRAY_SIZE(ulpi_register_functions),
};


#ifdef RHODODENDRON_SUPPORTS_VOLTAGE_SANITY_CHECKING

/**
 * FIXME: replace me with a libgreat ADC read
 */
static void set_up_onboard_adc(bool use_adc1, uint32_t pin_mask, uint8_t significant_bits)
{
	// FIXME: for now, since we're not using a libgreat built-in ADC, we'll hardcode the ADC
	// clock divider to meet datasheet requirements
	const uint32_t clkdiv = 45;	// divide 204MHz clock by 45 to get ~4MHz

	// Compute the number of clock cycles required to capture the given number of siginificant bits.
	// Clocks
	uint32_t clks = 10 - significant_bits;

	// Compute the control register value...
	uint32_t cr_value = ADC_CR_SEL(pin_mask) |
			ADC_CR_CLKDIV(clkdiv) |
			ADC_CR_CLKS(clks) |
			ADC_CR_PDN;


	// ... and apply it.
	if (use_adc1) {
		ADC1_CR = cr_value;
	} else {
		ADC0_CR = cr_value;
	}
}



/**
 * Diagnostic functionality for reading the voltage of VDD18 on Rhododendrons with it brought to ADC0_0.
 *

 */
static uint32_t read_vdd18_voltage(void)
{
	const uint32_t vcc_mv = 3300;

	uint16_t sample;

	// Read from ADC 0_0.
	set_up_onboard_adc(0, 1 << 0, 10);

	// Start a conversion, and then wait for it to complete.
	ADC0_CR |= ADC_CR_START(1);
	while(!(ADC0_DR0 & ADC_DR_DONE));

	// Read the ADC value, and send it
	sample = (ADC0_DR0 >> 6) & 0x3ff;

	return (sample * vcc_mv) / 1024;
}

#endif


static int sanity_check_environment(void)
{

#ifdef RHODODENDRON_SUPPORTS_CLOCK_SANITY_CHECKING

	// Synthetic maximum allowable frequency numbers;
	// which account mostly for our measurement inaccuracy.
	const uint32_t max_clock_frequency = 66000000UL;
	const uint32_t min_clock_frequency = 54000000UL;

#endif



#ifdef RHODODENDRON_SUPPORTS_VOLTAGE_SANITY_CHECKING
	uint32_t time_base = get_time();
	uint32_t timeout = 300 * 1000;

	// Datasheet maximum range.
	const uint32_t vdd18_voltage_min = 1600;
	const uint32_t vdd18_voltage_max = 2000;
	uint32_t vdd18_voltage_mv = 0;


	//
	// First, check that the PHY-generated supplies are within their datasheet  ranges.
	//

	// Give the regulator some time to stabalize before checking it.
	do {
		vdd18_voltage_mv = read_vdd18_voltage();

		if(get_time_since(time_base) > timeout) {
			break;
		}

	} while (vdd18_voltage_mv < vdd18_voltage_min);


	pr_info("rhododendron: read PHY VDD18 supply at %d.%03dV.\n",
			vdd18_voltage_mv / 1000, vdd18_voltage_mv % 1000);

	if (vdd18_voltage_mv < vdd18_voltage_min) {
		pr_warning("rhododendron: warning: PHY VDD18 voltage too low! (expected >= 1.6V)\n");
		return ENODEV;
	}
	if (vdd18_voltage_mv > vdd18_voltage_max) {
		pr_warning("rhododendron: warning: PHY VDD18 voltage too high! (expected <= 2.0V\n");
		return ENODEV;
	}

	pr_info("rhododendron: voltage supplies OK!\n");
#endif

#ifdef RHODODENDRON_SUPPORTS_CLOCK_SANITY_CHECKING

	//
	// Next, check the core clock.
	//

	// FIXME: abstract
	platform_scu_configure_pin_fast_io(4, 7, 1, SCU_NO_PULL);

	uint32_t freq = platform_detect_clock_source_frequency(CLOCK_SOURCE_GP_CLOCK_INPUT);
	pr_info("rhododendron: measured PHY clock frequency at %d Hz\n", freq);

	if (freq < min_clock_frequency) {
		pr_warning("rhododendron: warning: PHY clock frequency too low! (expected ~60MHz)\n");
		return ENODEV;
	}

	if (freq > max_clock_frequency) {
		pr_warning("rhododendron: warning: PHY clock frequency voltage too high! (expected ~60MHz)\n");
		return ENODEV;
	}

#endif

	return 0;
}



void rhododendron_turn_on_led(rhododendron_led_t led)
{
	// TODO: if the LED wasn't set up properly, skip it
	gpio_set_pin_value(gpio_leds[led], false);
}


void rhododendron_turn_off_led(rhododendron_led_t led)
{
	// TODO: if the LED wasn't set up properly, skip it
	gpio_set_pin_value(gpio_leds[led], true);
}


void rhododendron_toggle_led(rhododendron_led_t led)
{
	// TODO: if the LED wasn't set up properly, skip it
	gpio_toggle_pin(gpio_leds[led]);
}



static int set_up_leds(void)
{
	int rc;

	// Set up each of the LEDs in the array above.
	for (unsigned i = 0; i < ARRAY_SIZE(gpio_leds); ++i) {

		// FIXME: claim these in the pin manager!

		// Ensure that the relevant pin is in GPIO mode...
		rc = gpio_configure_pinmux(gpio_leds[i]);
		if (rc) {
			pr_error("error: could not set up one of the Rhododendron LEDs!");
			return rc;
		}

		// ... and start off with the relevant LED off.
		gpio_set_pin_value(gpio_leds[i], LED_OFF);
		gpio_set_pin_direction(gpio_leds[i], true);
	}

	return 0;
}


/**
 * Configures the PHY reset pin; also temporarily captures STP and DIR.
 * Note that the PHY will start in reset, and will remain their until bring_up_phy() is called.
 */
static int set_up_phy_reset(void)
{
	// FIXME: reserve reset pin in pin manager!

	// Set up the PHY pin.
	int rc = gpio_configure_pinmux(ulpi_phy_reset);
	if (rc) {
		pr_error("error: rhododenron: could not set up PHY reset pin!\n");
		return rc;
	}

	// Drive the pin, with an initial value of logic-0 / reset.
	gpio_set_pin_value(ulpi_phy_reset, false);
	gpio_set_pin_direction(ulpi_phy_reset, true);

	// Set up the DIR input.
	rc = gpio_configure_pinmux(ulpi_dir_gpio);
	if (rc) {
		pr_error("error: rhododenron: could not set up PHY direction pin!\n");
		return rc;
	}
	gpio_set_pin_direction(ulpi_dir_gpio, false);


	// Set up the STOP pin.
	rc = gpio_configure_pinmux(ulpi_stp_gpio);
	if (rc) {
		pr_error("error: rhododenron: could not set up PHY stop pin!\n");
		return rc;
	}

	// Assert STP, and leave it there, for now.
	gpio_set_pin_value(ulpi_stp_gpio, true);
	gpio_set_pin_direction(ulpi_stp_gpio, true);

	return 0;
}

int wait_with_timeout(gpio_pin_t pin, bool wait_for_high, uint32_t timeout)
{
	uint32_t base_time = get_time();
	bool loop_while_pin_is = !wait_for_high;

	while(gpio_get_pin_value(pin) == loop_while_pin_is) {
		if (get_time_since(base_time) > timeout) {
			return ETIMEDOUT;
		}
	}

	return 0;
}



/**
 * Initializes a connected Rhododendron board, preparing things for analysis.
 *
 * @return 0 on success, or an error code if the board could not be brought up
 */
int boot_up_phy(void)
{
	const uint32_t phy_startup_phase_timeout = 100 * 1000; // 100mS

	int rc;

	pr_info("rhododendron: booting up PHY!\n");


	// Clear the PHY's (active-low) reset, allowing it to start up.
	gpio_set_pin_value(ulpi_phy_reset, true);

	delay_us(10000);

	gpio_set_pin_value(ulpi_stp_gpio, false);

	/*

	// On startup, the PHY should assert DIR until the ULPI clock stabilizes.
	rc = wait_with_timeout(ulpi_dir_gpio, true, phy_startup_phase_timeout);
	if (rc) {
		pr_error("rhododendron: error: timed out waiting for PHY startup (waiting for DIR to assert)\n");
		return rc;
	}

	*/

	// Wait for the PHY to de-assert the DIR pin, which means it's completed startup.
	rc = wait_with_timeout(ulpi_dir_gpio, false, phy_startup_phase_timeout);
	if (rc) {
		pr_error("rhododendron: error: timed out waiting for PHY startup (waiting for PLL to stabilize)\n");
		return rc;
	}

	// If none of the above timed out, indicate success.
	return 0;
}


int set_up_clock_output(void)
{
	int rc;

	platform_scu_registers_t *scu = platform_get_scu_registers();
	platform_clock_generation_register_block_t *cgu = get_platform_clock_generation_registers();

	// Enable the generic CLKOUT output.
	platform_enable_base_clock(&cgu->out);

#ifdef RHODODENDRON_USE_USB1_CLK_AS_ULPI_CLOCK
	pr_info("Providing ULPI clock directly!\n");

	// If we're generating the ULPI clock directly, use DIVB as the source.
	platform_select_base_clock_source(&cgu->out, CLOCK_SOURCE_DIVIDER_B_OUT);
	platform_select_base_clock_source(&cgu->periph, CLOCK_SOURCE_DIVIDER_B_OUT);

	// Also, drive the ULPI_CLK line to VCC, configuring the PHY to accept its REFCLK directly.
	rc = gpio_configure_pinmux(ulpi_clk_gpio);
	if (rc) {
		pr_error("error: could not set the Rhododendron clock select!\n");
		return rc;
	}
	gpio_set_pin_value(ulpi_clk_gpio, 1);
	gpio_set_pin_direction(ulpi_clk_gpio, true);

	// Otherwise, we'll default to the audio PLL, which can generate a 26MHz reference clock.
#endif


	// Configure the CLK2 pin to be a high-speed output.
	platform_scu_pin_configuration_t clk2_config = {
		.function = 1,
		.pull_resistors = SCU_NO_PULL,
		.input_buffer_enabled = 0,
		.use_fast_slew = 1,
		.disable_glitch_filter = 1,
	};
	scu->clk[2] = clk2_config;


	return 0;
}

int rhododendron_early_init(void)
{
	int rc;

	rhododendron_turn_off_led(LED_STATUS);

	// Set up the PHY's reset pin.
	rc = set_up_phy_reset();
	if (rc) {
		return rc;
	}

	// Configure the rhododendron status LEDs.
	rc = set_up_leds();
	if (rc) {
		return rc;
	}


	return 0;
}


/**
 * Initializes a connected Rhododendron board, preparing things for analysis.
 *
 * @return 0 on success, or an error code if the board could not be brought up
 */
int initialize_rhododendron(void)
{
	int rc;

	// Start our clock output to the PHY.
	set_up_clock_output();

	// Boot up the PHY.
	rc = boot_up_phy();
	if (rc) {
		return rc;
	}

	// Configure our SGPIO, but don't run anything yet.
	// This ensures the pins have a known state before the PHY starts up (hiZ).
	rc = sgpio_set_up_functions(&ulpi_register_mode);
	if (rc) {
		return rc;
	}

	// Sanity check the environment.
	rc = sanity_check_environment();
	if (rc) {
		return rc;
	}

	return 0;
}


/**
 * Function that ends register access mode, freeing the SGPIO->ULPI bridge for other operations.
 */
void ulpi_register_access_stop(void)
{
	sgpio_halt(&ulpi_register_mode);
}


static uint32_t compute_direction_bits(uint16_t single_bit_direction)
{
	uint32_t two_bit_direction = 0;

	for (unsigned i = 0; i < 16; ++i)
	{
		uint16_t input_mask  = 0b1  << i;
		uint16_t output_mask = 0b11 << (2 * i);

		if(single_bit_direction & input_mask) {
			two_bit_direction |= output_mask;
		}
	}

	return two_bit_direction;
}



/**
 * Performs a write to a non-extended ('immediate') ULPI register.
 *
 * @returns 0 on success, or an error code on failure.
 */
int ulpi_register_write(uint8_t address, uint8_t value)
{
	const uint32_t expected_nxt_values = 0x00000030UL;

	uint8_t ulpi_command = ULPI_COMMAND_REGISTER_WRITE_MASK | address;
	int rc;

	// Configure our SGPIO functions, but don't run anything yet.
	// This ensures the pins have a known state before the PHY starts up (hiZ).
	rc = sgpio_set_up_functions(&ulpi_register_mode);
	if (rc) {
		return rc;
	}

	// Idle command; is held on the bus while the SGPIO shift-out starts up.
	register_access_buffer[0] = ULPI_COMMAND_IDLE;

	// Command + address word -- we hold these for three clock cycles, matching the
	// how long it is until the PHY asserts the NXT signal.
	register_access_buffer[1] = ulpi_command;
	register_access_buffer[2] = ulpi_command;
	register_access_buffer[3] = ulpi_command;

	// Register value for a single cycle, then idle the bus.
	// We'll eventually drop control over the bus and let the pull-downs hold things idle.
	register_access_buffer[4] = value;
	register_access_buffer[5] = ULPI_COMMAND_IDLE;
	register_access_buffer[6] = ULPI_COMMAND_IDLE;
	register_access_buffer[7] = ULPI_COMMAND_IDLE;

	// Set our direction to out for our data cycles, plus one idle cycle
	// to help discharge the ULPI bus.
	register_direction_buffer = compute_direction_bits(0b00111110);

	// Pulse stop immediately after we finish transmitting.
	register_stop_buffer      = 0b0100000;

	// Reset all of our positions.
	ulpi_register_functions[0].position_in_buffer = 0;
	ulpi_register_functions[0].position_in_direction_buffer = 0;
	ulpi_register_functions[1].position_in_buffer = 0;
	ulpi_register_functions[2].position_in_buffer = 0;

	// Run the SGPIO command; performing the register write.
	sgpio_run_blocking(&ulpi_register_mode);

	// Validate that the PHY responses with NXT values when we expect it to.
	if (register_nxt_buffer != expected_nxt_values) {
		pr_info("rhododendron: error: ulpi reg write failed (invalid NXT states)! (expected %08x, got %08x)\n",
			expected_nxt_values, register_nxt_buffer);
		return EIO;
	}

	return 0;
}


// Startup:
// Write:  reg[3a] := 0b10 (enable swap DP + DM)
// Write:  reg[04] := 0b01001000 (switch to HS, non-driving mdoe)
